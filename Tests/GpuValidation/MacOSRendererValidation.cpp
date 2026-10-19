#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
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

#include <algorithm>
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

	enum class ValidationPhase
	{
		InitialFrame,
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

				auto OpenedProject = PulseForge::Project::Open(std::filesystem::path(PF_MACOS_RENDERER_PROJECT_FILE));
				if (!OpenedProject) throw std::runtime_error("Could not open the PBR validation project: " + OpenedProject.error().Message);
				m_Project.emplace(std::move(*OpenedProject));
				if (!m_Project->GetDescription().StartScene)
					throw std::runtime_error("The PBR validation project has no startup scene");
				const auto Loaded = PulseForge::SceneAssetService::Load(
					*m_Project->GetDescription().StartScene, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), m_Scene);
				if (!Loaded) throw std::runtime_error("Could not load the PBR startup scene: " + Loaded.error().Message);
				if (const char* TestMode = std::getenv("PULSEFORGE_MACOS_RENDERER_TEST_MODE"))
				{
					if (std::string_view(TestMode) == "suppress-geometry")
					{
						size_t RemovedMeshes = 0;
						for (const PulseForge::Entity& Entity : m_Scene.GetEntities())
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
					PF_INFO("MACOS_RENDERER_INITIAL_OUTPUT=PASS; extent={}x{}; geometry_draws={}; written_depth_pixels={}; shadow_depth_pixels={}",
						m_InitialExtent.first, m_InitialExtent.second, m_CapturedGeometryDraws, m_CapturedSceneDepthPixels,
						m_CapturedShadowDepthPixels);
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

			if (m_Phase == ValidationPhase::InitialFrame || m_Phase == ValidationPhase::ResizedFrame ||
				m_Phase == ValidationPhase::ExposureFrame)
			{
				const auto Extent = App.GetWindow().GetFramebufferSize();
				if (Extent.first == 0 || Extent.second == 0) return;
				const auto Prepared = m_SceneRenderer->PrepareScene(
					m_Scene, static_cast<float>(Extent.first) / static_cast<float>(Extent.second));
				if (!Prepared)
				{
					Fail("Could not prepare the PBR scene: " + Prepared.error().Message);
					return;
				}
				if (m_SceneRenderer->IsEnvironmentLightingPending()) return;
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
			m_Project.reset();
			m_CapturedHdrColor = nullptr;
			m_CapturedHdrDepth = nullptr;
			m_CapturedShadowDepth = nullptr;
			m_ShutdownComplete = !m_ImGuiRenderer && !m_ImGuiContext && !m_OutputTarget && !m_SceneRenderer && !m_Project;
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
		PulseForge::Scene m_Scene;
		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		PulseForge::RenderTargetHandle m_OutputTarget;
		const PulseForge::Texture* m_CapturedHdrColor = nullptr;
		const PulseForge::Texture* m_CapturedHdrDepth = nullptr;
		const PulseForge::Texture* m_CapturedShadowDepth = nullptr;
		PulseForge::TextureReadback m_CapturedColor;
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
