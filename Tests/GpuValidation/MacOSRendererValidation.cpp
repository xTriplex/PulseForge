#include "Assets/Project.h"
#include "Assets/CookedProductCodec.h"
#include "Assets/MaterialAssetService.h"
#include "Assets/MaterialGraphCompiler.h"
#include "Assets/SceneAssetService.h"
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
#include "Assets/AssetCookService.h"
#include "Assets/CookedAssetCatalog.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorMaterialDetailsPanel.h"
#include "Editor/EditorMaterialDocumentEditor.h"
#include "Editor/EditorMaterialDraftController.h"
#include "Editor/EditorStyle.h"
#include "Runtime/CookedContentProvider.h"
#endif
#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Editor/EditorImGuiRenderer.h"
#include "GpuValidation/MacOSRendererReadbackAnalysis.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	struct HdrDifference final
	{
		size_t ChangedComponents = 0;
		float MaximumAbsolute = 0.0f;
		float MaximumRelative = 0.0f;
		bool AllFinite = true;
	};

	HdrDifference MeasureHdrDifference(
		const PulseForge::TextureReadback& Left,
		const PulseForge::TextureReadback& Right)
	{
		HdrDifference Difference;
		if (Left.Format != PulseForge::TextureFormat::RGBA16_Float || Right.Format != Left.Format ||
			Left.Width != Right.Width || Left.Height != Right.Height || Left.Pixels.size() != Right.Pixels.size() ||
			Left.Pixels.size() % sizeof(uint16_t) != 0)
		{
			Difference.ChangedComponents = std::numeric_limits<size_t>::max();
			Difference.MaximumAbsolute = std::numeric_limits<float>::infinity();
			Difference.MaximumRelative = std::numeric_limits<float>::infinity();
			Difference.AllFinite = false;
			return Difference;
		}

		for (size_t Offset = 0; Offset < Left.Pixels.size(); Offset += sizeof(uint16_t))
		{
			uint16_t LeftBits = 0;
			uint16_t RightBits = 0;
			std::memcpy(&LeftBits, Left.Pixels.data() + Offset, sizeof(LeftBits));
			std::memcpy(&RightBits, Right.Pixels.data() + Offset, sizeof(RightBits));
			const float LeftValue = glm::unpackHalf1x16(LeftBits);
			const float RightValue = glm::unpackHalf1x16(RightBits);
			if (!std::isfinite(LeftValue) || !std::isfinite(RightValue))
			{
				Difference.AllFinite = false;
				++Difference.ChangedComponents;
				continue;
			}
			const float Absolute = std::abs(LeftValue - RightValue);
			if (Absolute != 0.0f) ++Difference.ChangedComponents;
			Difference.MaximumAbsolute = std::max(Difference.MaximumAbsolute, Absolute);
			const float Relative = Absolute / std::max({ std::abs(LeftValue), std::abs(RightValue), 1.0e-6f });
			Difference.MaximumRelative = std::max(Difference.MaximumRelative, Relative);
		}
		return Difference;
	}

	enum class ValidationPhase
	{
		InitialFrame,
		SourceGraphMaterialFrame,
		GraphMaterialParameterFrame,
		GraphMaterialPresentationFrame,
		CookedGraphMaterialFrame,
		GraphMaterialFrame,
		SkinnedPoseFrame,
		WaitForResize,
		ResizedFrame,
		ExposureFrame,
		Complete,
		Failed
	};

	class MacOSRendererValidationLayer final : public PulseForge::Layer
	{
	public:
		MacOSRendererValidationLayer()
			: Layer("macOS PulseForge renderer output, ImGui, resize, and shutdown validation")
		{
		}

		void OnAttach() override
		{
			try
			{
				auto& App = PulseForge::Application::Get();
				m_NativeWindow = static_cast<GLFWwindow*>(App.GetWindow().GetNativeWindow());
				if (!m_NativeWindow) throw std::runtime_error("The application did not expose a GLFW native window");
				const auto LogicalExtent = std::pair{ App.GetWindow().GetWidth(), App.GetWindow().GetHeight() };
				const auto ContentScale = App.GetWindow().GetContentScale();
				m_InitialLogicalExtent = LogicalExtent;
				m_InitialExtent = App.GetWindow().GetFramebufferSize();
				if (LogicalExtent.first == 0 || LogicalExtent.second == 0 || !std::isfinite(ContentScale.first) ||
					!std::isfinite(ContentScale.second) || ContentScale.first <= 0.0f || ContentScale.second <= 0.0f)
					throw std::runtime_error("The macOS native window reported invalid logical dimensions or backing scale");
				if (m_InitialExtent.first == 0 || m_InitialExtent.second == 0)
					throw std::runtime_error("The Vulkan presentation surface has a zero initial framebuffer extent");
				PF_INFO("MACOS_RENDERER_WINDOW_SCALE=PASS; logical={}x{}; framebuffer={}x{}; content_scale={}x{}", LogicalExtent.first,
					LogicalExtent.second, m_InitialExtent.first, m_InitialExtent.second, ContentScale.first, ContentScale.second);

				std::filesystem::path ProjectPath(PF_MACOS_RENDERER_PROJECT_FILE);
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
				auto FixtureID = PulseForge::UUID::Generate();
				if (!FixtureID) throw std::runtime_error("Could not allocate a unique cooked graph renderer fixture identity");
				std::error_code TemporaryRootError;
				const std::filesystem::path SystemTemporaryRoot = std::filesystem::temp_directory_path(TemporaryRootError);
				if (TemporaryRootError)
					throw std::runtime_error("Could not resolve the macOS renderer fixture temp root: " +
						TemporaryRootError.message());
				const std::filesystem::path TemporaryRoot = std::filesystem::canonical(SystemTemporaryRoot, TemporaryRootError);
				if (TemporaryRootError)
					throw std::runtime_error("Could not resolve a canonical macOS renderer fixture temp root: " +
						TemporaryRootError.message());
				m_TestProjectRoot = TemporaryRoot / ("PulseForge-MacOS-CookedGraph-" + FixtureID->ToString());
				const std::filesystem::path SourceProjectRoot = ProjectPath.parent_path();
				std::filesystem::create_directories(m_TestProjectRoot, TemporaryRootError);
				if (TemporaryRootError)
					throw std::runtime_error("Could not create the isolated PBR renderer fixture root: " + TemporaryRootError.message());
				m_TestProjectRoot = std::filesystem::canonical(m_TestProjectRoot, TemporaryRootError);
				if (TemporaryRootError)
					throw std::runtime_error("Could not canonicalize the isolated PBR renderer fixture root: " +
						TemporaryRootError.message());
				const std::filesystem::path CanonicalCopiedProjectRoot = m_TestProjectRoot / SourceProjectRoot.filename();
				std::filesystem::copy(SourceProjectRoot, CanonicalCopiedProjectRoot, std::filesystem::copy_options::recursive,
					TemporaryRootError);
				if (TemporaryRootError)
					throw std::runtime_error("Could not isolate the PBR renderer fixture project: " + TemporaryRootError.message());
				ProjectPath = CanonicalCopiedProjectRoot / ProjectPath.filename();
#endif
				auto OpenedProject = PulseForge::Project::Open(ProjectPath);
				if (!OpenedProject) throw std::runtime_error("Could not open the PBR validation project: " + OpenedProject.error().Message);
				m_Project.emplace(std::move(*OpenedProject));
				if (!m_Project->GetDescription().StartScene)
					throw std::runtime_error("The PBR validation project has no startup scene");
				m_Scene = std::make_unique<PulseForge::Scene>();
				const auto Loaded = PulseForge::SceneAssetService::Load(
					*m_Project->GetDescription().StartScene, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), *m_Scene);
				if (!Loaded) throw std::runtime_error("Could not load the PBR startup scene: " + Loaded.error().Message);
				const char* TestMode = std::getenv("PULSEFORGE_MACOS_RENDERER_TEST_MODE");
				if (!TestMode || std::string_view(TestMode) == "normal") PrepareGraphMaterialEquivalence();
				if (TestMode)
				{
					if (std::string_view(TestMode) == "suppress-geometry")
					{
						size_t RemovedMeshes = 0;
						for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
						{
							auto MeshRenderer = Entity.GetMeshRenderer();
							if (!MeshRenderer)
								throw std::runtime_error("Could not inspect a mesh while preparing the geometry-negative fixture");
							if (!MeshRenderer->has_value()) continue;
							if (auto Removed = Entity.RemoveMeshRenderer(); !Removed)
								throw std::runtime_error("Could not suppress fixture geometry: " + Removed.error().Message);
							++RemovedMeshes;
						}
						if (RemovedMeshes == 0) throw std::runtime_error("Geometry-negative fixture found no mesh components to suppress");
						PF_INFO("MACOS_RENDERER_TEST_FAULT=suppress-geometry; removed_mesh_components={}", RemovedMeshes);
					}
					else if (std::string_view(TestMode) == "suppress-shadow-casters")
						PF_INFO("MACOS_RENDERER_TEST_FAULT=suppress-shadow-casters; render targets and clears remain active");
					else if (std::string_view(TestMode) != "normal")
						throw std::runtime_error("Unknown macOS renderer validation test mode");
				}

				auto CreatedRenderer =
					PulseForge::SceneRenderer::Create(App, *m_Project, std::filesystem::path(PF_MACOS_RENDERER_SCENE_SHADER_DIRECTORY));
				if (!CreatedRenderer) throw std::runtime_error("Could not create SceneRenderer: " + CreatedRenderer.error().Message);
				m_SceneRenderer = std::move(*CreatedRenderer);
				if (!TestMode || std::string_view(TestMode) == "normal") PrepareSkinnedRendererFixture();
				if (!m_SceneRenderer->SetToneMappingSettings({ 0.0f }))
					throw std::runtime_error("SceneRenderer rejected the baseline HDR exposure");
				CreateOutputTarget(m_InitialExtent.first, m_InitialExtent.second);

				m_ImGuiContext = ImGui::CreateContext();
				if (!m_ImGuiContext) throw std::runtime_error("Could not create the Dear ImGui context");
				ImGui::SetCurrentContext(m_ImGuiContext);
				ImGui::GetIO().BackendPlatformName = "imgui_impl_glfw";
				if (!ImGui_ImplGlfw_InitForVulkan(m_NativeWindow, true))
					throw std::runtime_error("Could not initialize Dear ImGui's GLFW Vulkan backend");
				m_GlfwBackendInitialized = true;
				m_ImGuiRenderer = std::make_unique<PulseForgeEditor::EditorImGuiRenderer>();
				if (auto Initialized = m_ImGuiRenderer->Initialize(
						App, std::filesystem::path(PF_MACOS_RENDERER_EDITOR_SHADER_DIRECTORY)); !Initialized)
					throw std::runtime_error("Could not initialize the production editor ImGui renderer: " + Initialized.error());
			}
			catch (const std::exception& Exception)
			{
				Fail(Exception.what());
			}
		}

		void OnUpdate(PulseForge::Timestep) override
		{
			if (m_Phase == ValidationPhase::Failed || m_Phase == ValidationPhase::Complete) return;
			if (Clock::now() - m_StartedAt > std::chrono::seconds(120))
			{
				Fail("Timed out waiting for rendered output, native resize, exposure comparison, or shutdown");
				return;
			}

			auto& App = PulseForge::Application::Get();
			if (m_FramePendingCapture)
			{
				m_FramePendingCapture = false;
				if (!CaptureRenderedOutput()) return;
				if (m_Phase == ValidationPhase::InitialFrame)
				{
					m_InitialHdrGeneration = m_CapturedHdrGeneration;
					m_InitialExtent = App.GetWindow().GetFramebufferSize();
					m_FixedMaterialBaseline = m_CapturedColor;
					m_ExpectedGeometryDrawCount = m_InitialSceneMeshCount + 1;
					if (m_CapturedGeometryDraws != m_ExpectedGeometryDrawCount)
					{
						Fail("The baseline renderer did not submit each authored mesh and the seeded skeletal mesh "
							"(expected draws=" + std::to_string(m_ExpectedGeometryDrawCount) + ", actual=" +
							std::to_string(m_CapturedGeometryDraws) + ")");
						return;
					}
					PF_INFO("MACOS_RENDERER_INITIAL_OUTPUT=PASS; extent={}x{}; geometry_draws={}; written_depth_pixels={}; shadow_depth_pixels={}",
						m_InitialExtent.first, m_InitialExtent.second, m_CapturedGeometryDraws, m_CapturedSceneDepthPixels,
						m_CapturedShadowDepthPixels);
					if (!m_GraphValidationMaterial)
					{
						Fail("The fixed-PBR baseline completed without a graph-material asset identity");
						return;
					}
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
					try
					{
						SaveGraphThroughMaterialDocument();
						m_SceneRenderer->RefreshMaterialAsset(*m_GraphValidationMaterial);
						const auto IdentityBaseline = m_SceneRenderer->GetResourceCacheStats();
						m_SourceGraphIdentityDescribeBaseline = IdentityBaseline.GraphMaterialIdentityDescribeCalls;
						m_SourceGraphIdentityCacheHitBaseline = IdentityBaseline.GraphMaterialIdentityCacheHits;
						m_Phase = ValidationPhase::SourceGraphMaterialFrame;
						return;
					}
					catch (const std::exception& Exception)
					{
						Fail(std::string("Could not save the discriminating graph renderer fixture: ") + Exception.what());
						return;
					}
#else
					if (!m_GraphValidationProduct)
					{
						Fail("The fixed-PBR baseline completed without a prepared graph-material equivalence fixture");
						return;
					}
					if (auto Override = m_SceneRenderer->SetTransientGraphMaterialOverride(*m_GraphValidationMaterial,
							m_GraphValidationDescription, *m_GraphValidationProduct, 1); !Override)
					{
						Fail("Could not install the graph-material readback equivalence fixture: " + Override.error().Message);
						return;
					}
#endif
					m_Phase = ValidationPhase::GraphMaterialFrame;
					return;
				}
				if (m_Phase == ValidationPhase::SourceGraphMaterialFrame)
				{
					if (!m_CookedGraphMaterialDrawObserved ||
						m_SceneRenderer->GetResourceCacheStats().GraphMaterialResources == 0)
					{
						Fail("The project-backed source renderer did not select and draw the saved graph material");
						return;
					}
					const auto GraphMaterialStats = m_SceneRenderer->GetResourceCacheStats();
					if (GraphMaterialStats.GraphMaterialIdentityDescribeCalls < m_SourceGraphIdentityDescribeBaseline ||
						GraphMaterialStats.GraphMaterialIdentityCacheHits < m_SourceGraphIdentityCacheHitBaseline)
					{
						Fail("The source graph identity counters were reset before the refreshed revision could be verified");
						return;
					}
					const size_t SourceRevisionDescribes = GraphMaterialStats.GraphMaterialIdentityDescribeCalls -
						m_SourceGraphIdentityDescribeBaseline;
					const size_t SourceRevisionCacheHits = GraphMaterialStats.GraphMaterialIdentityCacheHits -
						m_SourceGraphIdentityCacheHitBaseline;
					if (SourceRevisionDescribes != 1)
					{
						Fail("The refreshed saved source graph recipe was not described exactly once (describes=" +
							std::to_string(SourceRevisionDescribes) + ", lifetime=" +
							std::to_string(GraphMaterialStats.GraphMaterialIdentityDescribeCalls) + ")");
						return;
					}
					if (SourceRevisionCacheHits == 0) return;
					const size_t ChangedPixels = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_FixedMaterialBaseline, m_CapturedColor);
					if (ChangedPixels < 64)
					{
						Fail("The saved graph output did not visibly differ from fixed material fields in the source renderer "
							"(changed pixels=" + std::to_string(ChangedPixels) + ")");
						return;
					}
					m_SourceGraphReadback = m_CapturedColor;
					PF_INFO("MACOS_SOURCE_GRAPH_MATERIAL_RENDER=PASS; asset_id={}; graph_material_resources={}; changed_pixels={}; "
						"identity_describes={}; identity_cache_hits={}", m_GraphValidationMaterial->ToString(),
						GraphMaterialStats.GraphMaterialResources, ChangedPixels,
						GraphMaterialStats.GraphMaterialIdentityDescribeCalls, GraphMaterialStats.GraphMaterialIdentityCacheHits);
					try
					{
						m_GraphParameterEditCount = 0;
						UpdateSavedGraphParameterForReuse();
						m_Phase = ValidationPhase::GraphMaterialParameterFrame;
					}
					catch (const std::exception& Exception)
					{
						Fail(std::string("Could not prepare the saved graph parameter-reuse phase: ") + Exception.what());
					}
					return;
				}
				if (m_Phase == ValidationPhase::GraphMaterialParameterFrame)
				{
					const auto Stats = m_SceneRenderer->GetResourceCacheStats();
					const bool ResourcesReused = Stats.GraphMaterialDxcProcessLaunches == m_GraphParameterBaselineStats.GraphMaterialDxcProcessLaunches &&
						Stats.GraphMaterialShaderCreations == m_GraphParameterBaselineStats.GraphMaterialShaderCreations &&
						Stats.GraphMaterialBindingLayoutCreations == m_GraphParameterBaselineStats.GraphMaterialBindingLayoutCreations &&
						Stats.GraphMaterialParameterBufferCreations == m_GraphParameterBaselineStats.GraphMaterialParameterBufferCreations &&
						Stats.GraphMaterialBindingSetCreations == m_GraphParameterBaselineStats.GraphMaterialBindingSetCreations &&
						Stats.GraphMaterialPipelineCreations == m_GraphParameterBaselineStats.GraphMaterialPipelineCreations &&
						Stats.GraphMaterialParameterUploads == m_GraphParameterBaselineStats.GraphMaterialParameterUploads + 1;
					const size_t ChangedPixels = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_SourceGraphReadback, m_CapturedColor);
					if (!ResourcesReused || ChangedPixels < 16)
					{
						Fail("A saved scalar edit did not update the visible graph material through its existing GPU resources "
							"(dxc=" + std::to_string(Stats.GraphMaterialDxcProcessLaunches) + ", uploads=" +
							std::to_string(Stats.GraphMaterialParameterUploads) + ", changed_pixels=" + std::to_string(ChangedPixels) + ")");
						return;
					}
					++m_GraphParameterEditCount;
					PF_INFO("MACOS_GRAPH_MATERIAL_PARAMETER_EDIT=PASS; edit={}; dxc_launches={}; shader_creations={}; layout_creations={}; "
						"parameter_buffer_creations={}; binding_set_creations={}; pipeline_creations={}; parameter_uploads={}; "
						"retained_pipelines={}; changed_pixels={}", m_GraphParameterEditCount,
						Stats.GraphMaterialDxcProcessLaunches, Stats.GraphMaterialShaderCreations,
						Stats.GraphMaterialBindingLayoutCreations, Stats.GraphMaterialParameterBufferCreations,
						Stats.GraphMaterialBindingSetCreations, Stats.GraphMaterialPipelineCreations,
						Stats.GraphMaterialParameterUploads, Stats.GraphMaterialPipelines, ChangedPixels);
					m_SourceGraphReadback = m_CapturedColor;
					if (m_GraphParameterEditCount < 3)
					{
						try { UpdateSavedGraphParameterForReuse(); }
						catch (const std::exception& Exception)
						{
							Fail(std::string("Could not prepare the next saved parameter-only edit: ") + Exception.what());
						}
						return;
					}
					PF_INFO("MACOS_GRAPH_MATERIAL_PARAMETER_REUSE=PASS; edits={}; dxc_launches={}; shader_creations={}; "
						"layout_creations={}; parameter_buffer_creations={}; binding_set_creations={}; pipeline_creations={}; "
						"parameter_uploads={}; retained_pipelines={}", m_GraphParameterEditCount,
						Stats.GraphMaterialDxcProcessLaunches, Stats.GraphMaterialShaderCreations,
						Stats.GraphMaterialBindingLayoutCreations, Stats.GraphMaterialParameterBufferCreations,
						Stats.GraphMaterialBindingSetCreations, Stats.GraphMaterialPipelineCreations,
						Stats.GraphMaterialParameterUploads, Stats.GraphMaterialPipelines);
					try
					{
						ApplySavedGraphPresentationEdit();
						m_Phase = ValidationPhase::GraphMaterialPresentationFrame;
					}
					catch (const std::exception& Exception)
					{
						Fail(std::string("Could not prepare a presentation-only graph edit: ") + Exception.what());
					}
					return;
				}
				if (m_Phase == ValidationPhase::GraphMaterialPresentationFrame)
				{
					const auto Stats = m_SceneRenderer->GetResourceCacheStats();
					const size_t ChangedPixels = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_SourceGraphReadback, m_CapturedColor);
					const bool ResourcesReused = Stats.GraphMaterialDxcProcessLaunches == m_GraphParameterBaselineStats.GraphMaterialDxcProcessLaunches &&
						Stats.GraphMaterialShaderCreations == m_GraphParameterBaselineStats.GraphMaterialShaderCreations &&
						Stats.GraphMaterialBindingLayoutCreations == m_GraphParameterBaselineStats.GraphMaterialBindingLayoutCreations &&
						Stats.GraphMaterialParameterBufferCreations == m_GraphParameterBaselineStats.GraphMaterialParameterBufferCreations &&
						Stats.GraphMaterialBindingSetCreations == m_GraphParameterBaselineStats.GraphMaterialBindingSetCreations &&
						Stats.GraphMaterialPipelineCreations == m_GraphParameterBaselineStats.GraphMaterialPipelineCreations &&
						Stats.GraphMaterialParameterUploads == m_GraphParameterBaselineStats.GraphMaterialParameterUploads;
					if (!ResourcesReused || ChangedPixels > 4)
					{
						Fail("A presentation-only Material layout edit changed rendered pixels or recreated graph GPU resources "
							"(dxc=" + std::to_string(Stats.GraphMaterialDxcProcessLaunches) + ", uploads=" +
							std::to_string(Stats.GraphMaterialParameterUploads) + ", changed_pixels=" + std::to_string(ChangedPixels) + ")");
						return;
					}
					PF_INFO("MACOS_GRAPH_MATERIAL_PRESENTATION_REUSE=PASS; dxc_launches={}; shader_creations={}; layouts={}; "
						"pipelines={}; parameter_uploads={}; changed_pixels={}", Stats.GraphMaterialDxcProcessLaunches,
						Stats.GraphMaterialShaderCreations, Stats.GraphMaterialBindingLayoutCreations,
						Stats.GraphMaterialPipelineCreations, Stats.GraphMaterialParameterUploads, ChangedPixels);
					try
					{
						CookAndActivateSavedGraphMaterial();
						m_CookedGraphMaterialDrawObserved = false;
						m_Phase = ValidationPhase::CookedGraphMaterialFrame;
					}
					catch (const std::exception& Exception)
					{
						Fail(std::string("Could not create the saved-source cooked graph renderer fixture: ") + Exception.what());
					}
					return;
				}
				if (m_Phase == ValidationPhase::CookedGraphMaterialFrame)
				{
					if (!m_CookedGraphMaterialDrawObserved ||
						m_SceneRenderer->GetResourceCacheStats().GraphMaterialResources == 0)
					{
						Fail("The cooked provider did not select and draw the saved graph material");
						return;
					}
					const size_t ChangedPixels = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_FixedMaterialBaseline, m_CapturedColor);
					const size_t SourceCookedDifference = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_SourceGraphReadback, m_CapturedColor);
					if (ChangedPixels < 64 || SourceCookedDifference > 4)
					{
						Fail("The cooked graph render did not match the discriminating source render "
							"(changed from fixed=" + std::to_string(ChangedPixels) + ", source/cooked=" +
							std::to_string(SourceCookedDifference) + ")");
						return;
					}
					PF_INFO("MACOS_COOKED_GRAPH_MATERIAL_RENDER=PASS; asset_id={}; changed_pixels={}; source_cooked_difference={}",
						m_GraphValidationMaterial->ToString(), ChangedPixels, SourceCookedDifference);
					BeginSkinnedPoseFrame();
					return;
				}
				if (m_Phase == ValidationPhase::GraphMaterialFrame)
				{
					if (!m_CookedGraphMaterialDrawObserved)
					{
						Fail("Cooked graph material renderer branch did not submit before readback acceptance");
						return;
					}
					const size_t ChangedPixels = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_FixedMaterialBaseline, m_CapturedColor);
					if (ChangedPixels != 0)
					{
						Fail("The base-color texture and UV graph changed the fixed PBR color readback");
						return;
					}
					PF_INFO("MACOS_GRAPH_MATERIAL_FIXED_PBR_EQUIVALENCE=PASS; changed_pixels={}; extent={}x{}",
						ChangedPixels, m_CapturedColor.Width, m_CapturedColor.Height);
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
					PF_INFO("MACOS_COOKED_GRAPH_MATERIAL_RENDER=PASS; asset_id={}; graph_material_resources={}; indexed_draws={}; readback_pixels={}",
						m_GraphValidationMaterial->ToString(), m_SceneRenderer->GetResourceCacheStats().GraphMaterialResources,
						m_CapturedGeometryDraws, ChangedPixels);
#endif
					BeginSkinnedPoseFrame();
					return;
				}
				if (m_Phase == ValidationPhase::SkinnedPoseFrame)
				{
					const size_t ColorChanges = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_FixedMaterialBaseline, m_CapturedColor);
					const bool SceneDepthChanged = HasChangedReadback(m_FixedMaterialDepthBaseline, m_CapturedSceneDepth);
					const bool ShadowDepthChanged = HasChangedReadback(m_FixedMaterialShadowBaseline, m_CapturedShadowReadback);
					const bool AoDepthChanged = HasChangedReadback(m_FixedMaterialAoDepthBaseline, m_CapturedAoDepth);
					const bool AoOutputChanged = HasChangedReadback(m_FixedMaterialAoOutputBaseline, m_CapturedAoOutput);
					if (ColorChanges < 8 || m_CapturedGeometryDraws != m_ExpectedGeometryDrawCount || !SceneDepthChanged ||
						!ShadowDepthChanged || !AoDepthChanged || !AoOutputChanged)
					{
						Fail("The moved skeletal pose did not reach color, scene depth, AO input/output, and shadow depth "
							"(color pixels=" + std::to_string(ColorChanges) + ", scene depth=" +
							(SceneDepthChanged ? "changed" : "unchanged") + ", AO depth=" +
							(AoDepthChanged ? "changed" : "unchanged") + ", shadow depth=" +
							(ShadowDepthChanged ? "changed" : "unchanged") + ", AO output=" +
							(AoOutputChanged ? "changed" : "unchanged") + ", geometry draws=" +
							std::to_string(m_CapturedGeometryDraws) + ")");
						return;
					}
					PF_INFO("G26_SKINNED_RENDER_READBACK=PASS; color_pixels={}; geometry_draws={}; scene_depth=changed; ao_input=changed; "
							"ao_output=changed; shadow_depth=changed; static_scene_meshes=present; graph_material_override=active",
						ColorChanges, m_CapturedGeometryDraws);
					glfwSetWindowSize(m_NativeWindow, 960, 640);
					m_Phase = ValidationPhase::WaitForResize;
					return;
				}
				if (m_Phase == ValidationPhase::ResizedFrame)
				{
					if (m_CapturedHdrGeneration == m_InitialHdrGeneration)
					{
						Fail("SceneRenderer did not recreate its HDR target after the native framebuffer resize");
						return;
					}
					m_ResizedColorReadback = m_CapturedColor;
					m_ResizedHdrGeneration = m_CapturedHdrGeneration;
					m_ResizedExtent = App.GetWindow().GetFramebufferSize();
					if (m_ResizedColorReadback.Width != m_ResizedExtent.first || m_ResizedColorReadback.Height != m_ResizedExtent.second)
					{
						Fail("The resized offscreen color readback did not match the native framebuffer extent");
						return;
					}
					if (!m_SceneRenderer->SetToneMappingSettings({ 2.0f }))
					{
						Fail("SceneRenderer rejected the controlled exposure change");
						return;
					}
					m_Phase = ValidationPhase::ExposureFrame;
				}
				else if (m_Phase == ValidationPhase::ExposureFrame)
				{
					if (m_CapturedHdrGeneration != m_ResizedHdrGeneration)
					{
						Fail("Changing exposure unexpectedly recreated the same-size HDR target");
						return;
					}
					const size_t ChangedPixels = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
						m_ResizedColorReadback, m_CapturedColor);
					if (ChangedPixels < 100)
					{
						Fail("Changing HDR exposure did not measurably change the tone-mapped PBR readback");
						return;
					}
					PF_INFO("MACOS_RENDERER_EXPOSURE_DIFF=PASS; changed_pixels={}; HDR_generation={}", ChangedPixels, m_CapturedHdrGeneration);
					m_Phase = ValidationPhase::Complete;
					m_ValidationSucceeded = true;
					App.RequestClose();
					return;
				}
			}

			if (m_Phase == ValidationPhase::WaitForResize)
			{
				const auto Extent = App.GetWindow().GetFramebufferSize();
				if (Extent.first == 0 || Extent.second == 0) return;
				const auto LogicalExtent = std::pair{ App.GetWindow().GetWidth(), App.GetWindow().GetHeight() };
				if (LogicalExtent.first == 0 || LogicalExtent.second == 0) return;
				if (Extent == m_InitialExtent || LogicalExtent == m_InitialLogicalExtent) return;
				const auto ContentScale = App.GetWindow().GetContentScale();
				if (!std::isfinite(ContentScale.first) || !std::isfinite(ContentScale.second) || ContentScale.first <= 0.0f ||
					ContentScale.second <= 0.0f)
				{
					Fail("The macOS native window reported an invalid backing scale after resize");
					return;
				}
				m_ResizedExtent = Extent;
				CreateOutputTarget(Extent.first, Extent.second);
				PF_INFO("MACOS_RENDERER_NATIVE_RESIZE=PASS; logical={}x{}; framebuffer={}x{}; content_scale={}x{}; requesting recreated "
						"scene targets",
					LogicalExtent.first, LogicalExtent.second, Extent.first, Extent.second, ContentScale.first, ContentScale.second);
				m_Phase = ValidationPhase::ResizedFrame;
			}

			if (m_Phase == ValidationPhase::InitialFrame || m_Phase == ValidationPhase::SourceGraphMaterialFrame ||
				m_Phase == ValidationPhase::GraphMaterialParameterFrame ||
				m_Phase == ValidationPhase::GraphMaterialPresentationFrame ||
				m_Phase == ValidationPhase::CookedGraphMaterialFrame || m_Phase == ValidationPhase::GraphMaterialFrame ||
				m_Phase == ValidationPhase::SkinnedPoseFrame ||
				m_Phase == ValidationPhase::ResizedFrame ||
				m_Phase == ValidationPhase::ExposureFrame)
			{
				const auto Extent = App.GetWindow().GetFramebufferSize();
				if (Extent.first == 0 || Extent.second == 0) return;
				const auto Prepared = m_SceneRenderer->PrepareScene(
					*m_Scene, static_cast<float>(Extent.first) / static_cast<float>(Extent.second));
				if (!Prepared)
				{
					Fail("Could not prepare the PBR scene: " + Prepared.error().Message);
					return;
				}
				if (m_SceneRenderer->IsAssetPreparationPending()) return;
				if (!m_SceneRenderer->HasPreparedEnvironmentLighting())
				{
					Fail("The PBR scene did not finish preparing its HDR environment lighting");
					return;
				}
				m_ScenePrepared = true;
			}
		}

		void OnRender() override
		{
			if (m_Phase == ValidationPhase::Failed || m_Phase == ValidationPhase::Complete) return;
			auto& App = PulseForge::Application::Get();
			if (!App.IsMainFrameActive())
			{
				Fail("The renderer did not provide an active Vulkan presentation frame");
				return;
			}

			if (m_ScenePrepared)
			{
				const uint32_t Width = m_OutputTarget->GetDescription().Width;
				const uint32_t Height = m_OutputTarget->GetDescription().Height;
				auto Hdr = m_SceneRenderer->RenderPreparedSceneToHdr(Width, Height);
				if (!Hdr)
				{
					Fail("Could not render the PBR scene to HDR: " + Hdr.error().Message);
					return;
				}
				if (!Hdr->ColorTexture || !Hdr->DepthTexture || Hdr->GeometryDrawCount == 0 || !Hdr->EnvironmentBackgroundDrawn ||
					Hdr->ColorTexture->GetDescription().Format != PulseForge::TextureFormat::RGBA16_Float ||
					Hdr->DepthTexture->GetDescription().Format != PulseForge::TextureFormat::Depth32Float)
				{
					Fail("PBR HDR render did not produce required RGBA16F color, D32 depth, geometry, and environment output");
					return;
				}
				m_CapturedHdrDepth = Hdr->DepthTexture;
				m_CapturedHdrColor = Hdr->ColorTexture;
				m_CapturedHdrGeneration = Hdr->TargetGeneration;
				m_CapturedHdrExtent = { Hdr->Width, Hdr->Height };

				auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_OutputTarget);
				if (!Rendered)
				{
					Fail("Could not tone-map the PBR scene to its readback target: " + Rendered.error().Message);
					return;
				}
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
				if (m_Phase == ValidationPhase::SourceGraphMaterialFrame ||
					m_Phase == ValidationPhase::GraphMaterialParameterFrame ||
					m_Phase == ValidationPhase::GraphMaterialPresentationFrame ||
					m_Phase == ValidationPhase::CookedGraphMaterialFrame || m_Phase == ValidationPhase::GraphMaterialFrame)
				{
					const PulseForge::SceneRendererResourceCacheStats ResourceStats = m_SceneRenderer->GetResourceCacheStats();
					if (ResourceStats.GraphMaterialResources == 0 || Rendered->GeometryDrawCount == 0)
					{
						Fail("The graph-material render submitted no graph resource and indexed geometry draw");
						return;
					}
					m_CookedGraphMaterialDrawObserved = true;
				}
#endif
				if (Rendered->GeometryDrawCount == 0 ||
					(Rendered->SpotlightShadowPassCount == 0 && Rendered->PointShadowPassCount == 0 &&
						!m_SceneRenderer->GetDirectionalShadowTextureForValidation(0)))
				{
					Fail("PBR output did not submit geometry or expose a scene shadow resource");
					return;
				}
				m_CapturedGeometryDraws = Rendered->GeometryDrawCount;
				m_CapturedShadowDepth = Rendered->SpotlightShadowPassCount > 0
					? m_SceneRenderer->GetSpotlightShadowTextureForValidation(0)
					: Rendered->PointShadowPassCount > 0
						? m_SceneRenderer->GetPointShadowFaceTextureForValidation(0, 0)
						: m_SceneRenderer->GetDirectionalShadowTextureForValidation(0);
				if (!m_CapturedShadowDepth)
				{
					Fail("The PBR scene reported shadow rendering but exposed no corresponding depth texture");
					return;
				}
				m_CapturedAoDepthTexture = m_SceneRenderer->GetAmbientOcclusionDepthTextureForValidation();
				m_CapturedAoOutputTexture = m_SceneRenderer->GetAmbientOcclusionTextureForValidation();
				if (!m_CapturedAoDepthTexture || !m_CapturedAoOutputTexture)
				{
					Fail("The PBR scene did not expose its enabled ambient-occlusion prepass and final texture");
					return;
				}
				m_ScenePrepared = false;
				m_FramePendingCapture = true;
			}

			ImGui::SetCurrentContext(m_ImGuiContext);
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(380.0f, 132.0f), ImGuiCond_Always);
			ImGui::Begin("PulseForge Apple renderer proof", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
			ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.56f, 1.0f), "Production Dear ImGui renderer is active");
			ImGui::Text("PBR, HDR, depth, shadow readback, native resize");
			ImGui::End();
			ImGui::Render();
			const ImDrawData* DrawData = ImGui::GetDrawData();
			if (!DrawData || DrawData->TotalVtxCount == 0 || DrawData->TotalIdxCount == 0)
			{
				Fail("Dear ImGui produced no visible draw data for the native presentation frame");
				return;
			}
			m_ImGuiVerticesSubmitted = static_cast<size_t>(DrawData->TotalVtxCount);
			if (auto Rendered = m_ImGuiRenderer->RenderDrawData(DrawData, 0); !Rendered)
			{
				Fail("The production editor ImGui renderer failed to submit draw data: " + Rendered.error());
				return;
			}
			m_ImGuiRendererSubmitted = true;
			if (!m_ImGuiSubmissionReported)
			{
				PF_INFO("MACOS_RENDERER_IMGUI_SUBMISSION=PASS; vertices={}; production editor renderer returned success",
					m_ImGuiVerticesSubmitted);
				m_ImGuiSubmissionReported = true;
			}
		}

		void OnDetach() override { ShutdownFixture(); }

		void ShutdownFixture() noexcept
		{
			if (m_ImGuiRenderer)
			{
				m_ImGuiRenderer->Shutdown();
				m_ImGuiRenderer.reset();
			}
			if (m_GlfwBackendInitialized)
			{
				ImGui_ImplGlfw_Shutdown();
				m_GlfwBackendInitialized = false;
			}
			if (m_ImGuiContext)
			{
				ImGui::SetCurrentContext(m_ImGuiContext);
				ImGui::DestroyContext(m_ImGuiContext);
				m_ImGuiContext = nullptr;
			}
			m_OutputTarget.reset();
			m_SceneRenderer.reset();
			m_Scene.reset();
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
			m_GraphContent.reset();
#endif
			m_Project.reset();
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
			if (!m_TestProjectRoot.empty())
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(m_TestProjectRoot, CleanupError);
				if (CleanupError)
					PF_ERROR("Could not clean isolated cooked graph renderer fixture {}: {}",
						m_TestProjectRoot.string(), CleanupError.message());
				else
					m_TestProjectRoot.clear();
			}
#endif
			m_CapturedHdrColor = nullptr;
			m_CapturedHdrDepth = nullptr;
			m_CapturedShadowDepth = nullptr;
			m_ShutdownComplete = !m_ImGuiRenderer && !m_ImGuiContext && !m_OutputTarget && !m_SceneRenderer && !m_Scene && !m_Project;
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
			m_ShutdownComplete = m_ShutdownComplete && !m_GraphContent && m_TestProjectRoot.empty();
#endif
			if (m_ValidationSucceeded && !m_ShutdownComplete)
				Fail("Renderer fixture resources remained alive after explicit orderly shutdown");
		}

		[[nodiscard]] bool Succeeded() const noexcept { return m_ValidationSucceeded && m_ShutdownComplete; }

	private:
		void CreateOutputTarget(uint32_t Width, uint32_t Height)
		{
			PulseForge::RenderTargetDesc Description;
			Description.Width = Width;
			Description.Height = Height;
			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			Description.DebugName = "macOS PBR renderer proof output";
			auto Created = PulseForge::Application::Get().CreateRenderTarget(Description);
			if (!Created) throw std::runtime_error("Could not create the PBR readback target: " + Created.error().Message);
			m_OutputTarget = std::move(*Created);
		}

		void PrepareGraphMaterialEquivalence()
		{
			for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
			{
				auto MeshRenderer = Entity.GetMeshRenderer();
				if (!MeshRenderer)
					throw std::runtime_error("Could not inspect a mesh while preparing graph-material readback equivalence");
				if (!MeshRenderer->has_value() || !(**MeshRenderer).MaterialAsset) continue;
				const PulseForge::AssetID MaterialID = *(**MeshRenderer).MaterialAsset;
				auto Description = PulseForge::MaterialAssetService::Load(
					MaterialID, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
				if (!Description)
					throw std::runtime_error("Could not load the fixed PBR material for graph equivalence: " + Description.error().Message);
				if (Description->BaseColorTexture.IsNil()) continue;
				m_GraphValidationMaterial = MaterialID;
				m_GraphValidationSourceMesh = (**MeshRenderer).MeshAsset;
				m_GraphValidationDescription = std::move(*Description);
				break;
			}
			if (!m_GraphValidationMaterial)
				throw std::runtime_error("The PBR scene has no textured mesh material for graph equivalence");

#if !defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
			PulseForge::MaterialGraphSource Source;
			Source.GraphID = PulseForge::UUID(0x475055, 1);
			Source.Nodes = {
				{ PulseForge::UUID(0x475055, 2), PulseForge::MaterialGraphNodeKind::UV0, 0.0f, {} },
				{ PulseForge::UUID(0x475055, 3), PulseForge::MaterialGraphNodeKind::BaseColorTextureSample, 0.0f, {} },
				{ PulseForge::UUID(0x475055, 4), PulseForge::MaterialGraphNodeKind::PbrOutput, 0.0f, {} },
				{ PulseForge::UUID(0x475055, 7), PulseForge::MaterialGraphNodeKind::FloatConstant,
					m_GraphValidationDescription.MetallicFactor },
				{ PulseForge::UUID(0x475055, 8), PulseForge::MaterialGraphNodeKind::FloatConstant,
					m_GraphValidationDescription.RoughnessFactor }
			};
			Source.Connections = {
				{ PulseForge::UUID(0x475055, 5), PulseForge::UUID(0x475055, 2), PulseForge::UUID(0x475055, 3), 0 },
				{ PulseForge::UUID(0x475055, 6), PulseForge::UUID(0x475055, 3), PulseForge::UUID(0x475055, 4), 0 },
				{ PulseForge::UUID(0x475055, 9), PulseForge::UUID(0x475055, 7), PulseForge::UUID(0x475055, 4), 1 },
				{ PulseForge::UUID(0x475055, 10), PulseForge::UUID(0x475055, 8), PulseForge::UUID(0x475055, 4), 2 }
			};
			PulseForge::MaterialGraphCompileRequest Request;
			Request.Source = std::move(Source);
			Request.ToolchainFingerprint = PulseForge::GetMaterialGraphCompilerToolchainFingerprint();
			auto Compiled = PulseForge::MaterialGraphCompiler::Compile(Request);
			if (!Compiled)
				throw std::runtime_error("Could not compile the fixed-PBR graph equivalence fixture: " + Compiled.error().Message);
			PulseForge::CookedGraphMaterialProduct Product;
			Product.RecipeFingerprint = Compiled->RecipeFingerprint;
			Product.Parameters.reserve(Compiled->Parameters.size());
			for (const auto& Parameter : Compiled->Parameters)
				Product.Parameters.push_back({ Parameter.ID, Parameter.Name, Parameter.DefaultValue });
			Product.SpirV = std::move(Compiled->SpirV);
			m_GraphValidationProduct.emplace(std::move(Product));
#endif
		}

#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
		void SaveGraphThroughMaterialDocument()
		{
			using namespace PulseForge;
			using namespace PulseForgeEditor;
			const auto MaterialRecord = m_Project->GetAssetRegistry().Find(*m_GraphValidationMaterial);
			if (!MaterialRecord) throw std::runtime_error("The selected PBR material disappeared from the isolated project copy");
			EditorDocumentManager Documents;
			const EditorDocumentIdentity Identity{ m_Project->GetDescription().ID, *m_GraphValidationMaterial,
				EditorDocumentKind::Material };
			auto Opened = Documents.Open(Identity, MaterialRecord->ProjectRelativePath,
				MaterialRecord->ProjectRelativePath.filename().string(), EditorDocumentOpenPreference::MainDocumentStrip);
			if (!Opened) throw std::runtime_error("Could not open the PBR material document: " + Opened.error());
			EditorMaterialDraftController Drafts;
			auto Loaded = Drafts.LoadDraft(*m_Project, *m_GraphValidationMaterial);
			if (!Loaded) throw std::runtime_error("Could not load the PBR Material document draft: " + Loaded.error());
			struct MaterialActions final : EditorMaterialDetailsActionSink
			{
				Project& SourceProject;
				EditorMaterialDraftController& Drafts;
				EditorDocumentManager& Documents;
				MaterialActions(Project& ProjectValue, EditorMaterialDraftController& DraftsValue,
					EditorDocumentManager& DocumentsValue)
					: SourceProject(ProjectValue), Drafts(DraftsValue), Documents(DocumentsValue) {}
				bool SaveMaterialFromDetails(const AssetID& Asset) override
					{ return Drafts.Save(SourceProject, Asset, Documents).has_value(); }
				bool RevertMaterialFromDetails(const AssetID& Asset) override
					{ return Drafts.Reload(SourceProject, Asset, Documents).has_value(); }
				void MaterialDetailsDraftEdited(const UUID& ProjectID, const AssetID& Asset) override
				{
					const EditorDocumentIdentity Document{ ProjectID, Asset, EditorDocumentKind::Material };
					if (Documents.Find(Document)) (void)Documents.MarkEdited(Document);
				}
			} Actions{ *m_Project, Drafts, Documents };
			EditorDocumentEditorRegistry Registry;
			EditorMaterialDetailsPanel MaterialPanel;
			EditorAssetSelector AssetSelector;
			EditorStyle Style;
			EditorMaterialDocumentEditor MaterialEditor(Documents, Registry, Drafts, MaterialPanel, AssetSelector, Style, Actions,
				[this] { return m_Project ? &*m_Project : nullptr; },
				[this] { return m_Project ? m_Project->GetAssetRegistry().GetAssets() : std::vector<AssetRecord>{}; });
			if (auto Registered = MaterialEditor.Register(); !Registered)
				throw std::runtime_error("Could not register the production Material document editor: " + Registered.error());
			auto* Session = Documents.Find(Identity);
			const EditorDocumentEditorContext Context{ Identity, m_Project->GetRootPath(), MaterialRecord->ProjectRelativePath,
				MaterialRecord->ProjectRelativePath.filename().string(), &m_Project->GetAssetRegistry() };
			if (!Session || !MaterialEditor.Open(*Session, Context))
				throw std::runtime_error("Could not initialize the registered Material document editor");
			auto* Draft = Drafts.FindDraft(*m_Project, *m_GraphValidationMaterial);
			if (!Draft) throw std::runtime_error("The registered Material document editor did not create its draft");
			MaterialGraphSource Graph;
			Graph.GraphID = UUID(0x475055, 1);
			Graph.Nodes = {
				{ UUID(0x475055, 3), MaterialGraphNodeKind::ColorConstant, 0.0f, {}, 220.0, 0.0,
					{ 0.92f, 0.035f, 0.025f } },
				{ UUID(0x475055, 4), MaterialGraphNodeKind::PbrOutput, 0.0f, {} },
				{ UUID(0x475055, 7), MaterialGraphNodeKind::FloatConstant, m_GraphValidationDescription.MetallicFactor },
				{ UUID(0x475055, 8), MaterialGraphNodeKind::FloatParameter, m_GraphValidationDescription.RoughnessFactor,
					"Roughness" }
			};
			Graph.Connections = {
				{ UUID(0x475055, 6), UUID(0x475055, 3), UUID(0x475055, 4), 0 },
				{ UUID(0x475055, 9), UUID(0x475055, 7), UUID(0x475055, 4), 1 },
				{ UUID(0x475055, 10), UUID(0x475055, 8), UUID(0x475055, 4), 2 }
			};
			if (!ValidateMaterialGraphSource(Graph).IsValid())
				throw std::runtime_error("The saved graph fixture failed Material schema validation");
			const MaterialAssetDesc Before = *Draft;
			if (!Drafts.BeginEdit(*m_Project, *m_GraphValidationMaterial, Before))
				throw std::runtime_error("The Material draft owner did not begin the graph edit transaction");
			Draft->Graph = std::move(Graph);
			Actions.MaterialDetailsDraftEdited(m_Project->GetDescription().ID, *m_GraphValidationMaterial);
			Drafts.FinalizePendingEdits(false);
			if (!Registry.Save(*Session)) throw std::runtime_error("The registered Material document editor failed to save the graph draft");
			const auto SavedSource = MaterialAssetService::LoadSourceSnapshot(
				*m_GraphValidationMaterial, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
			if (!SavedSource || !SavedSource->Description.Graph ||
				SavedSource->Description.Graph->GraphID != UUID(0x475055, 1) ||
				SavedSource->Description.BaseColorTexture != m_GraphValidationDescription.BaseColorTexture)
				throw std::runtime_error("The saved Material source no longer matches the edited graph or stable asset reference");
			m_GraphValidationDescription = SavedSource->Description;
		}

		void UpdateSavedGraphParameterForReuse()
		{
			using namespace PulseForge;
			auto Description = MaterialAssetService::Load(
				*m_GraphValidationMaterial, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
			if (!Description || !Description->Graph)
				throw std::runtime_error("Could not reload the saved graph before its parameter-only edit");
			auto Parameter = std::ranges::find(Description->Graph->Nodes, UUID(0x475055, 8), &MaterialGraphNode::ID);
			if (Parameter == Description->Graph->Nodes.end() || Parameter->Kind != MaterialGraphNodeKind::FloatParameter)
				throw std::runtime_error("The saved graph has no stable Roughness parameter node");
			Parameter->Value = Parameter->Value < 0.5f ? 0.95f : 0.05f;
			if (auto Saved = MaterialAssetService::Save(*m_GraphValidationMaterial, m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(), *Description); !Saved)
				throw std::runtime_error("Could not save the scalar-only graph edit: " + Saved.error().Message);
			m_GraphParameterBaselineStats = m_SceneRenderer->GetResourceCacheStats();
			m_SourceGraphReadback = m_CapturedColor;
			m_SceneRenderer->RefreshMaterialAsset(*m_GraphValidationMaterial);
		}

		void ApplySavedGraphPresentationEdit()
		{
			using namespace PulseForge;
			auto Description = MaterialAssetService::Load(
				*m_GraphValidationMaterial, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
			if (!Description || !Description->Graph)
				throw std::runtime_error("Could not reload the saved graph before its layout-only edit");
			auto Node = std::ranges::find(Description->Graph->Nodes, UUID(0x475055, 3), &MaterialGraphNode::ID);
			if (Node == Description->Graph->Nodes.end())
				throw std::runtime_error("The saved graph lost its stable layout probe node");
			Node->X += 37.0;
			Node->Y += 19.0;
			if (auto Saved = MaterialAssetService::Save(*m_GraphValidationMaterial, m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(), *Description); !Saved)
				throw std::runtime_error("Could not save the presentation-only graph edit: " + Saved.error().Message);
			m_GraphParameterBaselineStats = m_SceneRenderer->GetResourceCacheStats();
			m_SourceGraphReadback = m_CapturedColor;
			m_SceneRenderer->RefreshMaterialAsset(*m_GraphValidationMaterial);
		}

		void CookAndActivateSavedGraphMaterial()
		{
			using namespace PulseForge;
			if (!m_Project->GetDescription().StartScene)
				throw std::runtime_error("The isolated PBR project has no startup scene for the graph cook");
			const auto SavedSource = MaterialAssetService::LoadSourceSnapshot(
				*m_GraphValidationMaterial, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
			if (!SavedSource || !SavedSource->Description.Graph)
				throw std::runtime_error("The saved Material graph source could not be reloaded for cooking");
			MaterialGraphCompileRequest CompileRequest;
			CompileRequest.Source = *SavedSource->Description.Graph;
			CompileRequest.ToolchainFingerprint = GetMaterialGraphCompilerToolchainFingerprint();
			auto ExpectedProduct = MaterialGraphCompiler::Compile(CompileRequest);
			if (!ExpectedProduct) throw std::runtime_error("The saved graph failed independent host validation: " +
				ExpectedProduct.error().Message);
			auto SourceSnapshot = AssetDependencyCatalogBuilder::Build(
				m_Project->GetRootPath(), m_Project->GetDescription().ID, 1);
			if (!SourceSnapshot) throw std::runtime_error("Could not snapshot the edited PBR project for cooking: " +
				SourceSnapshot.error().Message);
#if defined(_WIN32)
			const CookedAssetCompatibility Compatibility{ "Windows", "Vulkan", "pulseforge-runtime-v1" };
#elif defined(__APPLE__)
			const CookedAssetCompatibility Compatibility{ "macOS", "Vulkan", "pulseforge-runtime-v1" };
#else
			const CookedAssetCompatibility Compatibility{ "Linux", "Vulkan", "pulseforge-runtime-v1" };
#endif
			const std::filesystem::path CookRoot = m_TestProjectRoot / "CookedContent";
			AssetCookRequest Request{ *SourceSnapshot, m_Project->GetFilePath(), *m_Project->GetDescription().StartScene,
				{ *m_Project->GetDescription().StartScene, *m_GraphValidationMaterial }, Compatibility, CookRoot };
			auto Plan = AssetCookService::Plan(Request);
			if (!Plan) throw std::runtime_error("Could not plan the edited PBR graph cook: " + Plan.error().Message);
			auto Cooked = AssetCookService::Cook(*Plan);
			if (!Cooked) throw std::runtime_error("Could not cook the edited PBR graph: " + Cooked.error().Message);
			auto OpenedContent = CookedContentProvider::Open(CookRoot, Compatibility);
			if (!OpenedContent) throw std::runtime_error("Could not open the graph cook from the cooked-only provider: " +
				OpenedContent.error().Message);
			if (!(*OpenedContent)->FindGraphMaterial(*m_GraphValidationMaterial))
				throw std::runtime_error("The cooked catalog did not retain the authored Material asset ID as a graph product");
			auto Product = (*OpenedContent)->LoadGraphMaterialProduct(*m_GraphValidationMaterial);
			uint32_t SpirVMagic = 0;
			if (Product && Product->SpirV.size() >= sizeof(SpirVMagic))
				std::memcpy(&SpirVMagic, Product->SpirV.data(), sizeof(SpirVMagic));
			if (!Product || Product->RecipeFingerprint != ExpectedProduct->RecipeFingerprint || SpirVMagic != 0x07230203u)
				throw std::runtime_error("The cooked provider graph product differs from the validated saved source recipe");
			PF_INFO("MACOS_GRAPH_MATERIAL_COOKED_SOURCE=PASS; asset_id={}; recipe={}; spirv_bytes={}",
				m_GraphValidationMaterial->ToString(), Product->RecipeFingerprint, Product->SpirV.size());
			m_GraphValidationProduct = std::move(*Product);
			auto CookedScene = (*OpenedContent)->LoadScene(*m_Project->GetDescription().StartScene);
			if (!CookedScene) throw std::runtime_error("The cooked PBR startup scene could not be loaded: " + CookedScene.error().Message);
			m_Scene = std::move(*CookedScene);
			m_GraphContent = *OpenedContent;
			auto Renderer = SceneRenderer::Create(Application::Get(), m_GraphContent,
				std::filesystem::path(PF_MACOS_RENDERER_SCENE_SHADER_DIRECTORY));
			if (!Renderer) throw std::runtime_error("Could not create the cooked-provider SceneRenderer: " + Renderer.error().Message);
			m_SceneRenderer = std::move(*Renderer);
			if (!m_SceneRenderer->SetToneMappingSettings({ 0.0f }))
				throw std::runtime_error("Cooked-provider renderer rejected the baseline HDR exposure");
			PrepareSkinnedRendererFixture();
		}
#endif

		void BeginSkinnedPoseFrame()
		{
			if (!m_SkinnedValidationEntity)
			{
				Fail("The renderer fixture did not create its skeletal validation entity");
				return;
			}
			auto Pose = MakeBindPose();
			Pose[1][13] += 0.65f;
			if (auto Updated = m_Scene->SetRuntimeSkeletalPose(m_SkinnedValidationEntity->GetUUID(), std::move(Pose)); !Updated)
			{
				Fail("Could not update the runtime skeletal validation pose: " + Updated.error().Message);
				return;
			}
			m_Phase = ValidationPhase::SkinnedPoseFrame;
		}

		static std::array<float, 16> IdentityTransform()
		{
			return { 1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f };
		}

		static std::vector<std::array<float, 16>> MakeBindPose()
		{
			return { IdentityTransform(), IdentityTransform() };
		}

		static bool HasChangedReadback(
			const PulseForge::TextureReadback& Baseline,
			const PulseForge::TextureReadback& Current)
		{
			return Baseline.Format == Current.Format && Baseline.Width == Current.Width && Baseline.Height == Current.Height &&
				Baseline.Pixels.size() == Current.Pixels.size() && Baseline.Pixels != Current.Pixels;
		}

		void PrepareSkinnedRendererFixture()
		{
			if (!m_GraphValidationMaterial || !m_GraphValidationSourceMesh)
				throw std::runtime_error("The graph material fixture did not retain its source mesh identity");
			m_InitialSceneMeshCount = 0;

			PulseForge::ImportedGltfSkeletalMesh Product;
			Product.Geometry.Name = "G26 dynamic skinned renderer validation";
			Product.Geometry.Vertices = {
				{ { -0.45f, -0.40f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f } },
				{ { 0.45f, -0.40f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f } },
				{ { 0.0f, 0.45f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.5f, 1.0f } }
			};
			Product.Geometry.Indices = { 0, 1, 2 };
			const auto Identity = IdentityTransform();
			Product.SkeletonNodes = {
				{ "node/0", "mesh", -1, false, Identity },
				{ "node/1", "joint", 0, true, Identity }
			};
			Product.MeshNodeIndex = 0;
			Product.JointNodeIndices = { 1 };
			Product.InverseBindMatrices = { Identity };
			Product.VertexInfluences.resize(Product.Geometry.Vertices.size());
			for (auto& Influence : Product.VertexInfluences)
			{
				Influence.JointIndices[0] = 0;
				Influence.Weights[0] = 1.0f;
			}
			constexpr std::string_view ProductKey = "skeletal/26262626262626262626262626262626";
			if (auto Seeded = m_SceneRenderer->SeedSkeletalProductForValidation(
					*m_GraphValidationSourceMesh, ProductKey, std::make_shared<const PulseForge::ImportedGltfSkeletalMesh>(std::move(Product))); !Seeded)
				throw std::runtime_error("Could not seed the skeletal renderer fixture: " + Seeded.error().Message);

			for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
			{
				auto MeshRenderer = Entity.GetMeshRenderer();
				if (!MeshRenderer) throw std::runtime_error("Could not count authored meshes for the skeletal fixture");
				if (MeshRenderer->has_value()) ++m_InitialSceneMeshCount;
			}
			glm::mat4 CameraWorld(1.0f);
			bool FoundPrimaryCamera = false;
			for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
			{
				auto Camera = Entity.GetCamera();
				if (!Camera || !Camera->has_value() || !(**Camera).IsPrimary) continue;
				auto World = Entity.GetWorldMatrix();
				if (!World) throw std::runtime_error("Could not resolve the primary camera transform for the skeletal fixture");
				CameraWorld = *World;
				FoundPrimaryCamera = true;
				break;
			}
			if (!FoundPrimaryCamera) throw std::runtime_error("The PBR fixture has no primary camera for skeletal placement");
			const glm::quat CameraRotation = glm::normalize(glm::quat_cast(glm::mat3(CameraWorld)));
			PulseForge::TransformComponent Transform;
			Transform.Rotation = CameraRotation;
			Transform.Translation = glm::vec3(CameraWorld[3]) + CameraRotation * glm::vec3(0.0f, 0.0f, -3.0f);
			Transform.Scale = { -1.0f, 1.0f, 1.0f };
			auto Created = m_Scene->CreateEntity("G26 skeletal renderer validation");
			if (!Created) throw std::runtime_error("Could not create the skeletal renderer fixture entity: " + Created.error().Message);
			m_SkinnedValidationEntity = *Created;
			if (auto Set = m_SkinnedValidationEntity->SetTransform(Transform); !Set)
				throw std::runtime_error("Could not place the skeletal renderer fixture: " + Set.error().Message);
			if (auto Set = m_SkinnedValidationEntity->SetMeshRenderer({
					*m_GraphValidationSourceMesh, m_GraphValidationMaterial, std::string(ProductKey), std::nullopt }); !Set)
				throw std::runtime_error("Could not assign the skeletal renderer fixture: " + Set.error().Message);
			if (auto Set = m_Scene->SetRuntimeSkeletalPose(m_SkinnedValidationEntity->GetUUID(), MakeBindPose()); !Set)
				throw std::runtime_error("Could not publish the skeletal renderer bind pose: " + Set.error().Message);
		}

		bool CaptureRenderedOutput()
		{
			auto& App = PulseForge::Application::Get();
			if (!m_ImGuiRendererSubmitted)
			{
				Fail("The production editor ImGui renderer did not submit draw data on the captured presentation frame");
				return false;
			}
			const PulseForge::Texture* Color = m_OutputTarget ? m_OutputTarget->GetColorTexture() : nullptr;
			if (!Color || !m_CapturedHdrColor || !m_CapturedHdrDepth || !m_CapturedShadowDepth)
			{
				Fail("Rendered frame did not retain its color, HDR depth, and shadow depth outputs");
				return false;
			}
			if (Color->GetDescription().Format != PulseForge::TextureFormat::RGBA8_Srgb ||
				Color->GetDescription().Width != m_CapturedHdrExtent.first ||
				Color->GetDescription().Height != m_CapturedHdrExtent.second)
			{
				Fail("Tone-mapped PBR output format or extent did not match the HDR render");
				return false;
			}
			const auto& HdrDescription = m_CapturedHdrColor->GetDescription();
			if (HdrDescription.Format != PulseForge::TextureFormat::RGBA16_Float ||
				HdrDescription.Width != m_CapturedHdrExtent.first || HdrDescription.Height != m_CapturedHdrExtent.second)
			{
				Fail("Rendered HDR source format or extent did not match the PBR frame");
				return false;
			}
			auto HdrReadback = App.ReadTextureForValidation(*m_CapturedHdrColor);
			if (!HdrReadback)
			{
				Fail("Could not read back the rendered RGBA16F HDR source: " + HdrReadback.error().Message);
				return false;
			}
			const auto HdrMetrics = PulseForge::GpuValidation::MacOSRenderer::MeasureHdrRadiance(*HdrReadback);
			if (!HdrMetrics || HdrMetrics->NonClearPixels < 64 || HdrMetrics->MaximumRadiance <= 1.0f)
			{
				Fail(HdrMetrics
						? "Rendered RGBA16F HDR source lacked finite non-clear radiance above SDR white"
						: "Rendered RGBA16F HDR source was invalid: " + HdrMetrics.error());
				return false;
			}
			auto ColorReadback = App.ReadTextureForValidation(*Color);
			if (!ColorReadback)
			{
				Fail("Could not read back the rendered PBR output: " + ColorReadback.error().Message);
				return false;
			}
			if (!PulseForge::GpuValidation::MacOSRenderer::HasPbrImageSignal(*ColorReadback))
			{
				Fail("Rendered PBR output was empty, grayscale, or lacked meaningful luminance variation");
				return false;
			}
			auto SceneDepth = App.ReadTextureForValidation(*m_CapturedHdrDepth);
			if (!SceneDepth)
			{
				Fail("Could not read back the rendered D32 scene depth: " + SceneDepth.error().Message);
				return false;
			}
			const size_t SceneDepthPixels = PulseForge::GpuValidation::MacOSRenderer::CountWrittenDepthPixels(*SceneDepth);
			if (SceneDepthPixels < 64)
			{
				Fail("Rendered D32 scene depth contained too few written geometry pixels");
				return false;
			}
			auto ShadowDepth = App.ReadTextureForValidation(*m_CapturedShadowDepth);
			if (!ShadowDepth)
			{
				Fail("Could not read back the rendered shadow depth: " + ShadowDepth.error().Message);
				return false;
			}
			const size_t ShadowDepthPixels = PulseForge::GpuValidation::MacOSRenderer::CountWrittenDepthPixels(*ShadowDepth);
			if (!PulseForge::GpuValidation::MacOSRenderer::HasShadowDepthSignal(*ShadowDepth))
			{
				Fail("Rendered shadow depth contained too few written caster pixels");
				return false;
			}
			if (m_Phase == ValidationPhase::InitialFrame)
			{
				m_FixedMaterialBaseline = *ColorReadback;
				m_FixedMaterialHdrBaseline = *HdrReadback;
				m_FixedMaterialDepthBaseline = *SceneDepth;
				m_FixedMaterialShadowBaseline = *ShadowDepth;
				auto AoDepth = App.ReadTextureForValidation(*m_CapturedAoDepthTexture);
				if (!AoDepth)
				{
					Fail("Could not read back the rendered ambient-occlusion prepass depth: " + AoDepth.error().Message);
					return false;
				}
				m_FixedMaterialAoDepthBaseline = *AoDepth;
				auto AoOutput = App.ReadTextureForValidation(*m_CapturedAoOutputTexture);
				if (!AoOutput)
				{
					Fail("Could not read back the final ambient-occlusion texture: " + AoOutput.error().Message);
					return false;
				}
				m_FixedMaterialAoOutputBaseline = *AoOutput;
			}
			else if (m_Phase == ValidationPhase::SkinnedPoseFrame)
			{
				m_CapturedSceneDepth = *SceneDepth;
				m_CapturedShadowReadback = *ShadowDepth;
				auto AoDepth = App.ReadTextureForValidation(*m_CapturedAoDepthTexture);
				if (!AoDepth)
				{
					Fail("Could not read back the moved skeletal ambient-occlusion depth: " + AoDepth.error().Message);
					return false;
				}
				m_CapturedAoDepth = *AoDepth;
				auto AoOutput = App.ReadTextureForValidation(*m_CapturedAoOutputTexture);
				if (!AoOutput)
				{
					Fail("Could not read back the moved skeletal final ambient-occlusion texture: " + AoOutput.error().Message);
					return false;
				}
				m_CapturedAoOutput = *AoOutput;
			}
			else if (m_Phase == ValidationPhase::GraphMaterialFrame)
			{
				const size_t ChangedPixels = PulseForge::GpuValidation::MacOSRenderer::CountChangedPixels(
					m_FixedMaterialBaseline, *ColorReadback);
				const bool HdrMatches = m_FixedMaterialHdrBaseline.Pixels == HdrReadback->Pixels;
				const HdrDifference HdrDelta = MeasureHdrDifference(m_FixedMaterialHdrBaseline, *HdrReadback);
				const bool HdrWithinTolerance = HdrMatches || (HdrDelta.AllFinite &&
					(HdrDelta.MaximumAbsolute <= 5.0e-4f && HdrDelta.MaximumRelative <= 1.0e-3f));
				const bool DepthMatches = m_FixedMaterialDepthBaseline.Pixels == SceneDepth->Pixels;
				const bool ShadowMatches = m_FixedMaterialShadowBaseline.Pixels == ShadowDepth->Pixels;
				if (ChangedPixels != 0 || !HdrWithinTolerance || !DepthMatches || !ShadowMatches)
				{
					Fail("The graph texture/UV and metallic/roughness path changed equivalent fixed-PBR readback "
						"(changed color pixels=" + std::to_string(ChangedPixels) + ", HDR exact=" +
						(HdrMatches ? "true" : "false") + ", scene depth exact=" +
						(DepthMatches ? "true" : "false") + ", shadow depth exact=" +
						(ShadowMatches ? "true" : "false") + ", HDR within tolerance=" +
						(HdrWithinTolerance ? "true" : "false") + ", HDR changed components=" +
						std::to_string(HdrDelta.ChangedComponents) + ", HDR max abs=" +
						std::to_string(HdrDelta.MaximumAbsolute) + ", HDR max relative=" +
						std::to_string(HdrDelta.MaximumRelative) + ")");
					return false;
				}
			}

			// Prove the positive-image predicate rejects a known all-clear readback.
			PulseForge::TextureReadback ClearedColor = *ColorReadback;
			std::fill(ClearedColor.Pixels.begin(), ClearedColor.Pixels.end(), std::byte{ 0 });
			if (PulseForge::GpuValidation::MacOSRenderer::HasPbrImageSignal(ClearedColor))
			{
				Fail("The rendered-output assertion accepted a controlled all-clear negative image");
				return false;
			}
			PulseForge::TextureReadback ClearedDepth = *ShadowDepth;
			for (size_t Pixel = 0; Pixel < static_cast<size_t>(ClearedDepth.Width) * ClearedDepth.Height; ++Pixel)
			{
				constexpr float ClearValue = 1.0f;
				std::memcpy(ClearedDepth.Pixels.data() + Pixel * sizeof(float), &ClearValue, sizeof(ClearValue));
			}
			if (PulseForge::GpuValidation::MacOSRenderer::HasShadowDepthSignal(ClearedDepth))
			{
				Fail("The shadow-depth assertion failed its controlled all-clear sensitivity check");
				return false;
			}
			PF_INFO("MACOS_RENDERER_READBACK=PASS; extent={}x{}; visible_pixels={}; chromatic_pixels={}; hdr_non_clear_pixels={}; "
					"hdr_max_radiance={:.4f}; scene_depth_pixels={}; shadow_depth_pixels={}; imgui_vertices={}",
				ColorReadback->Width, ColorReadback->Height,
				PulseForge::GpuValidation::MacOSRenderer::MeasureImage(*ColorReadback).VisiblePixels,
				PulseForge::GpuValidation::MacOSRenderer::MeasureImage(*ColorReadback).ChromaticPixels, HdrMetrics->NonClearPixels,
				HdrMetrics->MaximumRadiance, SceneDepthPixels,
				ShadowDepthPixels, m_ImGuiVerticesSubmitted);
			m_CapturedColor = std::move(*ColorReadback);
			m_CapturedSceneDepthPixels = SceneDepthPixels;
			m_CapturedShadowDepthPixels = ShadowDepthPixels;
			return true;
		}

		void Fail(std::string Message)
		{
			if (m_Phase == ValidationPhase::Failed) return;
			m_Failure = std::move(Message);
			m_Phase = ValidationPhase::Failed;
			PF_CRITICAL("MACOS_RENDERER_VALIDATION=FAIL; {}", m_Failure);
			PulseForge::Application::Get().RequestClose();
		}

		GLFWwindow* m_NativeWindow = nullptr;
		ImGuiContext* m_ImGuiContext = nullptr;
		std::unique_ptr<PulseForgeEditor::EditorImGuiRenderer> m_ImGuiRenderer;
		std::optional<PulseForge::Project> m_Project;
#if defined(PF_G22_MATERIAL_DOCUMENT_COOKED_RENDER_VALIDATION)
		std::filesystem::path m_TestProjectRoot;
		std::shared_ptr<const PulseForge::CookedContentProvider> m_GraphContent;
		bool m_CookedGraphMaterialDrawObserved = false;
		PulseForge::SceneRendererResourceCacheStats m_GraphParameterBaselineStats;
		size_t m_GraphParameterEditCount = 0;
#endif
		std::unique_ptr<PulseForge::Scene> m_Scene;
		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		std::optional<PulseForge::AssetID> m_GraphValidationMaterial;
		std::optional<PulseForge::AssetID> m_GraphValidationSourceMesh;
		std::optional<PulseForge::Entity> m_SkinnedValidationEntity;
		PulseForge::MaterialAssetDesc m_GraphValidationDescription;
		std::optional<PulseForge::CookedGraphMaterialProduct> m_GraphValidationProduct;
		PulseForge::RenderTargetHandle m_OutputTarget;
		const PulseForge::Texture* m_CapturedHdrColor = nullptr;
		const PulseForge::Texture* m_CapturedHdrDepth = nullptr;
		const PulseForge::Texture* m_CapturedShadowDepth = nullptr;
		const PulseForge::Texture* m_CapturedAoDepthTexture = nullptr;
		const PulseForge::Texture* m_CapturedAoOutputTexture = nullptr;
		PulseForge::TextureReadback m_CapturedColor;
		PulseForge::TextureReadback m_FixedMaterialBaseline;
		PulseForge::TextureReadback m_SourceGraphReadback;
		size_t m_SourceGraphIdentityDescribeBaseline = 0;
		size_t m_SourceGraphIdentityCacheHitBaseline = 0;
		PulseForge::TextureReadback m_FixedMaterialHdrBaseline;
		PulseForge::TextureReadback m_FixedMaterialDepthBaseline;
		PulseForge::TextureReadback m_FixedMaterialShadowBaseline;
		PulseForge::TextureReadback m_FixedMaterialAoDepthBaseline;
		PulseForge::TextureReadback m_FixedMaterialAoOutputBaseline;
		PulseForge::TextureReadback m_CapturedSceneDepth;
		PulseForge::TextureReadback m_CapturedShadowReadback;
		PulseForge::TextureReadback m_CapturedAoDepth;
		PulseForge::TextureReadback m_CapturedAoOutput;
		PulseForge::TextureReadback m_ResizedColorReadback;
		std::pair<unsigned int, unsigned int> m_InitialExtent{};
		std::pair<unsigned int, unsigned int> m_InitialLogicalExtent{};
		std::pair<unsigned int, unsigned int> m_ResizedExtent{};
		std::pair<uint32_t, uint32_t> m_CapturedHdrExtent{};
		Clock::time_point m_StartedAt = Clock::now();
		uint64_t m_CapturedHdrGeneration = 0;
		uint64_t m_InitialHdrGeneration = 0;
		uint64_t m_ResizedHdrGeneration = 0;
		size_t m_CapturedGeometryDraws = 0;
		size_t m_InitialSceneMeshCount = 0;
		size_t m_ExpectedGeometryDrawCount = 0;
		size_t m_CapturedSceneDepthPixels = 0;
		size_t m_CapturedShadowDepthPixels = 0;
		size_t m_ImGuiVerticesSubmitted = 0;
		ValidationPhase m_Phase = ValidationPhase::InitialFrame;
		bool m_GlfwBackendInitialized = false;
		bool m_ScenePrepared = false;
		bool m_FramePendingCapture = false;
		bool m_ImGuiRendererSubmitted = false;
		bool m_ImGuiSubmissionReported = false;
		bool m_ValidationSucceeded = false;
		bool m_ShutdownComplete = false;
		std::string m_Failure;
	};

	class MacOSRendererValidationApplication final : public PulseForge::Application
	{
	public:
		MacOSRendererValidationApplication()
			: Application(PulseForge::RendererAPI::Vulkan)
		{
			auto Layer = std::make_unique<MacOSRendererValidationLayer>();
			m_Layer = Layer.get();
			PushLayer(std::move(Layer));
		}

		int RunValidation()
		{
			Run();
			if (!m_Layer) return EXIT_FAILURE;
			m_Layer->ShutdownFixture();
			return m_Layer->Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
		}

	private:
		MacOSRendererValidationLayer* m_Layer = nullptr;
	};

	bool HasValidationOrShutdownError()
	{
		const auto Entries = PulseForge::Log::GetRecentEntries();
		return std::any_of(Entries.begin(), Entries.end(), [](const PulseForge::LogEntry& Entry)
		{
			const bool HasVulkanId = Entry.Message.find("VUID-") != std::string::npos;
			const bool IsLoaderDiagnostic = Entry.Message.find("id=Loader Message") != std::string::npos;
			const bool IsLoggedError = Entry.Level >= PulseForge::LogLevel::Error &&
				(!IsLoaderDiagnostic &&
					(Entry.Message.find("Vulkan error (") != std::string::npos || Entry.Message.starts_with("NVRHI:") ||
						Entry.Message.find("Vulkan shutdown") != std::string::npos));
			return HasVulkanId || IsLoggedError;
		});
	}
} // namespace

int main()
{
	PulseForge::Log::Init();
	try
	{
		int Result = EXIT_FAILURE;
		{
			MacOSRendererValidationApplication App;
			Result = App.RunValidation();
		}
		if (HasValidationOrShutdownError())
		{
			PF_CRITICAL("MACOS_RENDERER_VALIDATION_DIAGNOSTICS=FAIL; Vulkan/NVRHI validation or shutdown error was logged");
			return EXIT_FAILURE;
		}
		if (Result == EXIT_SUCCESS)
			PF_INFO("MACOS_RENDERER_ORDERLY_SHUTDOWN=PASS; ImGui, scene, targets, Vulkan renderer, and native window destroyed cleanly");
		PF_INFO("MACOS_RENDERER_PROCESS_SHUTDOWN=PASS; application and fixture scope exited");
		return Result;
	}
	catch (const std::exception& Exception)
	{
		constexpr std::string_view StrictCapabilityRejection =
			"No otherwise suitable Vulkan GPU satisfies PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences";
		const char* Policy = std::getenv("PULSEFORGE_VULKAN_PRESENTATION_POLICY");
		if (Policy && std::string_view(Policy) == "require-fences" &&
			std::string_view(Exception.what()).starts_with(StrictCapabilityRejection) && !HasValidationOrShutdownError())
		{
			PF_WARN("MACOS_RENDERER_STRICT_POLICY_REJECTED=CAPABILITY; {}", Exception.what());
			return 77;
		}
		PF_CRITICAL("MACOS_RENDERER_VALIDATION=FAIL; startup or renderer unavailable: {}", Exception.what());
		return EXIT_FAILURE;
	}
}
