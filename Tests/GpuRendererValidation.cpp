#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	struct ValidationVertex
	{
		float Position[2];
		float TexCoord[2];
	};

	struct EditorValidationVertex
	{
		float Position[2];
		float TexCoord[2];
		float Color[4];
	};

	std::vector<std::byte> LoadShader(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary | std::ios::ate);
		if (!Input)
			throw std::runtime_error("Could not open GPU-validation shader: " + Path.string());

		const std::streamoff FileSize = Input.tellg();
		if (FileSize < 20 || FileSize > 16 * 1024 * 1024 || FileSize % sizeof(uint32_t) != 0)
			throw std::runtime_error("GPU-validation shader has an invalid byte size: " + Path.string());

		std::vector<std::byte> Bytecode(static_cast<size_t>(FileSize));
		Input.seekg(0, std::ios::beg);
		Input.read(reinterpret_cast<char*>(Bytecode.data()), static_cast<std::streamsize>(FileSize));
		if (!Input)
			throw std::runtime_error("Could not read GPU-validation shader: " + Path.string());
		return Bytecode;
	}

	class GpuValidationLayer final : public PulseForge::Layer
	{
	public:
		GpuValidationLayer(bool ValidateSceneHdr, bool ValidateToneMapping, bool ValidateEditorUiUnorm, bool ValidateLocalLights)
			: Layer(ValidateEditorUiUnorm ? "Editor UNORM UI composition GPU validation" :
				ValidateLocalLights ? "SceneRenderer local-light GPU validation" :
				ValidateToneMapping ? "HDR tonemapping GPU validation" :
				ValidateSceneHdr ? "SceneRenderer HDR GPU validation" : "RGBA16F GPU validation"),
			  m_ValidateSceneHdr(ValidateSceneHdr),
			  m_ValidateToneMapping(ValidateToneMapping),
			  m_ValidateEditorUiUnorm(ValidateEditorUiUnorm),
			  m_ValidateLocalLights(ValidateLocalLights)
		{
		}

		void OnAttach() override
		{
			m_StartedAt = std::chrono::steady_clock::now();
			try
			{
				InitializeResources();
				if (m_ValidateEditorUiUnorm)
					InitializeEditorUiUnormResources();
				if (m_ValidateSceneHdr || m_ValidateToneMapping || m_ValidateLocalLights)
					InitializeSceneResources();
				if (m_ValidateLocalLights)
					InitializeLocalLightScenario();
				PF_INFO("RGBA16F GPU validation resources initialized");
			}
			catch (const std::exception& Exception)
			{
				Fail(Exception.what());
			}
		}

		void OnUpdate(PulseForge::Timestep) override
		{
			if (m_ValidateEditorUiUnorm)
			{
				if (m_EditorUiFrames >= 3)
				{
					if (m_EditorUiTargetCreations != 2)
						Fail("Editor UI intermediate was not reused at a stable extent and replaced after resize");
					else
					{
						PF_INFO("GPU_VALIDATION_RESULT=PASS; editor UI rendered alpha-blended primitives to a persistent sRGB target, sampled it, and encoded to UNORM across reuse and resize");
						PulseForge::Application::Get().RequestClose();
					}
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(15))
					Fail("Timed out during editor UI UNORM composition validation");
				return;
			}
			if (m_ValidateLocalLights)
			{
				if (m_LocalLightFrames >= 6)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; SceneRenderer submitted real PBR geometry with mixed point/spot lights through HDR and SDR presentation across light updates");
					PulseForge::Application::Get().RequestClose();
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(45))
				{
					Fail("Timed out preparing or rendering local-light scene");
					return;
				}
				const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
				if (Width == 0 || Height == 0)
					return;
				if (m_LocalLightFrames > 0)
					UpdateLocalLightTransforms();
				const auto Prepared = m_SceneRenderer->PrepareScene(
					m_LocalLightsScene, static_cast<float>(Width) / static_cast<float>(Height));
				if (!Prepared)
				{
					Fail("Could not prepare mixed local-light scene: " + Prepared.error().Message);
					return;
				}
				if (m_SceneRenderer->IsEnvironmentLightingPending())
					return;
				m_ScenePrepared = true;
				return;
			}
			if (m_ValidateToneMapping)
			{
				if (m_ToneMappingFrame >= 10)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; HDR scene tone mapping covered swapchain, sRGB and UNORM targets, exposure, resize, reuse, and scene variants");
					PulseForge::Application::Get().RequestClose();
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
				{
					Fail("Timed out preparing or rendering tone-mapped scene outputs");
					return;
				}
				m_ScenePrepared = false;
				const bool Offscreen = m_ToneMappingFrame >= 2;
				uint32_t Width = 0;
				uint32_t Height = 0;
				if (Offscreen)
				{
					Width = m_ToneMappingFrame >= 4 ? 96u : 64u;
					Height = 64;
				}
				else
				{
					const auto Extent = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
					Width = Extent.first;
					Height = Extent.second;
				}
				if (Width == 0 || Height == 0)
					return;
				PulseForge::Scene* SceneToPrepare = &m_Scene;
				if (m_ToneMappingFrame == 6) SceneToPrepare = &m_EnvironmentOnlyScene;
				if (m_ToneMappingFrame == 7) SceneToPrepare = &m_GeometryOnlyScene;
				if (m_ToneMappingFrame == 8) SceneToPrepare = &m_EmptyScene;
				if (m_ToneMappingFrame == 9) SceneToPrepare = &m_DirectionalOnlyScene;
				const float AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
				const auto Prepared = m_SceneRenderer->PrepareScene(*SceneToPrepare, AspectRatio);
				if (!Prepared)
				{
					Fail("Could not prepare the scene for tone mapping: " + Prepared.error().Message);
					return;
				}
				const bool HasEnvironment = m_ToneMappingFrame < 7;
				if (HasEnvironment && m_SceneRenderer->IsEnvironmentLightingPending())
					return;
				if (HasEnvironment && !m_SceneRenderer->HasPreparedEnvironmentLighting())
				{
					Fail("Tone-mapping scenario did not prepare its environment lighting");
					return;
				}
				m_ScenePrepared = true;
				return;
			}

			if (m_ValidateSceneHdr)
			{
				if (m_SceneCompletedFrames >= 4 && m_ScenarioIndex >= 4)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; SceneRenderer rendered and sampled persistent RGBA16F targets across reuse, resize, empty, environment-only, geometry-only, and directional-only cases");
					PulseForge::Application::Get().RequestClose();
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(40))
				{
					Fail("Timed out preparing or rendering the sample HDR scene");
					return;
				}
				m_ScenePrepared = false;

				const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
				if (Width == 0 || Height == 0)
					return;
				PulseForge::Scene* SceneToPrepare = m_SceneCompletedFrames < 4
					? &m_Scene
					: &GetScenarioScene(m_ScenarioIndex);
				PulseForge::AmbientOcclusionSettings AoSettings = m_SceneRenderer->GetAmbientOcclusionSettings();
				AoSettings.Enabled = !(m_SceneCompletedFrames >= 4 && m_ScenarioIndex == 2);
				if (!m_SceneRenderer->SetAmbientOcclusionSettings(AoSettings))
				{
					Fail("Could not select the validation scene's ambient-occlusion mode");
					return;
				}
				const auto Prepared = m_SceneRenderer->PrepareScene(
					*SceneToPrepare, static_cast<float>(Width) / static_cast<float>(Height));
				if (!Prepared)
				{
					Fail("Could not prepare the bundled scene for HDR rendering: " + Prepared.error().Message);
					return;
				}
				const bool SceneHasEnvironment = m_SceneCompletedFrames < 4 || m_ScenarioIndex == 1;
				if (SceneHasEnvironment && m_SceneRenderer->IsEnvironmentLightingPending())
					return;
				if (SceneHasEnvironment && !m_SceneRenderer->HasPreparedEnvironmentLighting())
				{
					Fail("Bundled validation scene did not produce prepared environment lighting");
					return;
				}
				m_ScenePrepared = true;
				return;
			}

			if (m_CompletedFrames >= 3)
			{
				PF_INFO("GPU_VALIDATION_RESULT=PASS; completed three RGBA16F render/sample frames");
				PulseForge::Application::Get().RequestClose();
			}
			else if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(10))
				Fail("Timed out before completing three RGBA16F render/sample frames");
		}

		void OnRender() override
		{
			if (m_ValidateEditorUiUnorm)
			{
				RenderEditorUiUnormFrame();
				return;
			}
			if (m_ValidateLocalLights)
			{
				RenderLocalLightFrame();
				return;
			}
			if (m_ValidateToneMapping)
			{
				RenderToneMappingFrame();
				return;
			}
			if (m_ValidateSceneHdr)
			{
				RenderSceneHdrFrame();
				return;
			}
			if (!m_Succeeded || m_CompletedFrames >= 3)
				return;

			PulseForge::Application& App = PulseForge::Application::Get();
			const PulseForge::RenderTargetClearValue ClearValue{
				.Color = { 2.0f, 0.25f, 0.125f, 1.0f },
				.Depth = 1.0f
			};
			const PulseForge::GraphicsResult BeginResult = App.BeginRenderTarget(*m_Target, ClearValue);
			if (!BeginResult)
			{
				Fail("Could not begin the RGBA16F render target: " + BeginResult.error().Message);
				return;
			}

			const PulseForge::DrawArguments DrawArguments{ .VertexCount = 3 };
			const PulseForge::GraphicsResult ColorDrawResult = App.Draw(*m_ColorPipeline, *m_VertexBuffer, DrawArguments);
			const PulseForge::GraphicsResult EndResult = App.EndRenderTarget();
			if (!ColorDrawResult)
			{
				Fail("RGBA16F draw recording failed: " + ColorDrawResult.error().Message);
				return;
			}
			if (!EndResult)
			{
				Fail("Could not end the RGBA16F render target: " + EndResult.error().Message);
				return;
			}

			const std::array<const PulseForge::BindingSet*, 1> BindingSets = { m_SamplingBindingSet.get() };
			const PulseForge::GraphicsResult SamplingDrawResult = App.Draw(
				*m_SamplingPipeline,
				*m_VertexBuffer,
				DrawArguments,
				BindingSets);
			if (!SamplingDrawResult)
			{
				Fail("RGBA16F shader-resource sampling draw failed: " + SamplingDrawResult.error().Message);
				return;
			}

			++m_CompletedFrames;
			if (m_CompletedFrames == 1)
				PF_INFO("RGBA16F attachment draw, shader-resource transition, and sampling draw recorded");
		}

		void OnDetach() override
		{
			m_EditorUiBindingSet.reset();
			m_EditorUiDrawBindingSet.reset();
			m_EditorUiUnormTarget.reset();
			m_EditorUiSrgbTarget.reset();
			m_EditorUiPresentationPipeline.reset();
			m_EditorUiPipeline.reset();
			m_EditorUiFullscreenBuffer.reset();
			m_EditorUiVertexBuffer.reset();
			m_EditorUiFontTexture.reset();
			m_EditorUiPresentationFragmentShader.reset();
			m_EditorUiPresentationVertexShader.reset();
			m_EditorUiFragmentShader.reset();
			m_EditorUiVertexShader.reset();
			m_SamplingBindingSet.reset();
			m_SdrTarget.reset();
			m_SceneRenderer.reset();
			m_Project.reset();
			m_SamplingPipeline.reset();
			m_ColorPipeline.reset();
			m_Sampler.reset();
			m_BindingLayout.reset();
			m_VertexBuffer.reset();
			m_Target.reset();
		}

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return m_Succeeded && (m_ValidateEditorUiUnorm ? m_EditorUiFrames == 3 && m_EditorUiTargetCreations == 2 :
				m_ValidateLocalLights ? m_LocalLightFrames == 6 :
				m_ValidateToneMapping ? m_ToneMappingFrame == 10 :
				m_ValidateSceneHdr ? m_SceneCompletedFrames == 4 && m_ScenarioIndex == 4 : m_CompletedFrames == 3);
		}

	private:
		void Fail(const std::string& Message)
		{
			m_Succeeded = false;
			PF_ERROR("GPU_VALIDATION_RESULT=FAIL: {0}", Message);
			PulseForge::Application::Get().RequestClose();
		}

		void InitializeResources()
		{
			PulseForge::Application& App = PulseForge::Application::Get();
			PulseForge::RenderTargetDesc TargetDescription;
			TargetDescription.Width = 32;
			TargetDescription.Height = 32;
			TargetDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA16_Float;
			TargetDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			TargetDescription.DebugName = "RGBA16F GPU validation target";
			auto TargetResult = App.CreateRenderTarget(TargetDescription);
			if (!TargetResult)
				throw std::runtime_error("RGBA16F render-target creation failed: " + TargetResult.error().Message);
			m_Target = std::move(TargetResult.value());

			const PulseForge::Texture* ColorTexture = m_Target->GetColorTexture();
			const PulseForge::Texture* DepthTexture = m_Target->GetDepthTexture();
			if (!ColorTexture || !DepthTexture)
				throw std::runtime_error("RGBA16F target did not provide its requested color and depth attachments");
			const PulseForge::TextureDesc& ColorDescription = ColorTexture->GetDescription();
			if (ColorDescription.Width != 32 || ColorDescription.Height != 32 ||
				ColorDescription.Format != PulseForge::TextureFormat::RGBA16_Float ||
				!PulseForge::HasTextureUsage(ColorDescription.Usage, PulseForge::TextureUsage::ColorAttachment) ||
				!PulseForge::HasTextureUsage(ColorDescription.Usage, PulseForge::TextureUsage::ShaderResource))
				throw std::runtime_error("RGBA16F target color texture has unexpected dimensions, format, or usage");
			if (DepthTexture->GetDescription().Format != PulseForge::TextureFormat::Depth32Float)
				throw std::runtime_error("RGBA16F target has an unexpected depth attachment format");
			PF_INFO("Created 32x32 RGBA16_Float color+shader-resource target with D32 attachment");

			const std::filesystem::path ShaderDirectory(PF_GPU_VALIDATION_SHADER_DIRECTORY);
			const std::vector<std::byte> VertexBytecode = LoadShader(ShaderDirectory / "GpuRendererValidation.vs.spv");
			const std::vector<std::byte> ColorBytecode = LoadShader(ShaderDirectory / "GpuRendererValidation.color.ps.spv");
			const std::vector<std::byte> SamplingBytecode = LoadShader(ShaderDirectory / "GpuRendererValidation.sample.ps.spv");

			PulseForge::ShaderDesc VertexShaderDescription;
			VertexShaderDescription.Stage = PulseForge::ShaderStage::Vertex;
			VertexShaderDescription.EntryPoint = "VSMain";
			VertexShaderDescription.DebugName = "RGBA16F validation vertex shader";
			auto VertexShaderResult = App.CreateShader(VertexShaderDescription, VertexBytecode);
			if (!VertexShaderResult)
				throw std::runtime_error("GPU-validation vertex shader creation failed: " + VertexShaderResult.error().Message);

			PulseForge::ShaderDesc ColorShaderDescription;
			ColorShaderDescription.Stage = PulseForge::ShaderStage::Fragment;
			ColorShaderDescription.EntryPoint = "PSColor";
			ColorShaderDescription.DebugName = "RGBA16F validation color shader";
			auto ColorShaderResult = App.CreateShader(ColorShaderDescription, ColorBytecode);
			if (!ColorShaderResult)
				throw std::runtime_error("GPU-validation color shader creation failed: " + ColorShaderResult.error().Message);

			PulseForge::ShaderDesc SamplingShaderDescription = ColorShaderDescription;
			SamplingShaderDescription.EntryPoint = "PSSample";
			SamplingShaderDescription.DebugName = "RGBA16F validation sampling shader";
			auto SamplingShaderResult = App.CreateShader(SamplingShaderDescription, SamplingBytecode);
			if (!SamplingShaderResult)
				throw std::runtime_error("GPU-validation sampling shader creation failed: " + SamplingShaderResult.error().Message);

			PulseForge::BindingLayoutDesc LayoutDescription;
			LayoutDescription.Visibility = PulseForge::ShaderVisibility::Fragment;
			LayoutDescription.Items = {
				{ PulseForge::BindingResourceType::Texture2D, 0 },
				{ PulseForge::BindingResourceType::Sampler, 0 }
			};
			LayoutDescription.DebugName = "RGBA16F validation sampling layout";
			auto LayoutResult = App.CreateBindingLayout(LayoutDescription);
			if (!LayoutResult)
				throw std::runtime_error("GPU-validation binding-layout creation failed: " + LayoutResult.error().Message);
			m_BindingLayout = std::move(LayoutResult.value());

			PulseForge::SamplerDesc SamplerDescription;
			SamplerDescription.Minification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.Magnification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.AddressU = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.AddressV = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.DebugName = "RGBA16F validation linear sampler";
			auto SamplerResult = App.CreateSampler(SamplerDescription);
			if (!SamplerResult)
				throw std::runtime_error("GPU-validation sampler creation failed: " + SamplerResult.error().Message);
			m_Sampler = std::move(SamplerResult.value());

			PulseForge::BindingSetDesc BindingDescription;
			BindingDescription.Layout = m_BindingLayout;
			BindingDescription.Textures.push_back({ 0, *ColorTexture, PulseForge::BindingResourceType::Texture2D });
			BindingDescription.Samplers.push_back({ 0, *m_Sampler });
			auto BindingResult = App.CreateBindingSet(BindingDescription);
			if (!BindingResult)
				throw std::runtime_error("GPU-validation sampling binding creation failed: " + BindingResult.error().Message);
			m_SamplingBindingSet = std::move(BindingResult.value());

			const std::array<ValidationVertex, 3> Vertices = {
				ValidationVertex{ { -1.0f, -1.0f }, { 0.0f, 1.0f } },
				ValidationVertex{ { 3.0f, -1.0f }, { 2.0f, 1.0f } },
				ValidationVertex{ { -1.0f, 3.0f }, { 0.0f, -1.0f } }
			};
			PulseForge::BufferDesc VertexBufferDescription;
			VertexBufferDescription.ByteSize = sizeof(Vertices);
			VertexBufferDescription.Usage = PulseForge::BufferUsage::Vertex;
			VertexBufferDescription.DebugName = "RGBA16F validation fullscreen triangle";
			auto VertexBufferResult = App.CreateBuffer(VertexBufferDescription, std::as_bytes(std::span(Vertices)));
			if (!VertexBufferResult)
				throw std::runtime_error("GPU-validation vertex-buffer creation failed: " + VertexBufferResult.error().Message);
			m_VertexBuffer = std::move(VertexBufferResult.value());

			PulseForge::GraphicsPipelineDesc ColorPipelineDescription;
			ColorPipelineDescription.VertexShader = std::move(VertexShaderResult.value());
			ColorPipelineDescription.FragmentShader = std::move(ColorShaderResult.value());
			ColorPipelineDescription.VertexLayout.Stride = sizeof(ValidationVertex);
			ColorPipelineDescription.VertexLayout.Attributes = {
				{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2, offsetof(ValidationVertex, Position) },
				{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(ValidationVertex, TexCoord) }
			};
			ColorPipelineDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA16_Float;
			ColorPipelineDescription.DepthAttachmentEnabled = true;
			ColorPipelineDescription.Depth.TestEnabled = true;
			ColorPipelineDescription.Depth.WriteEnabled = true;
			ColorPipelineDescription.DebugName = "RGBA16F validation target pipeline";
			auto ColorPipelineResult = App.CreateGraphicsPipeline(ColorPipelineDescription);
			if (!ColorPipelineResult)
				throw std::runtime_error("RGBA16F graphics-pipeline creation failed: " + ColorPipelineResult.error().Message);
			m_ColorPipeline = std::move(ColorPipelineResult.value());

			PulseForge::GraphicsPipelineDesc SamplingPipelineDescription = ColorPipelineDescription;
			SamplingPipelineDescription.FragmentShader = std::move(SamplingShaderResult.value());
			SamplingPipelineDescription.BindingLayouts = { m_BindingLayout };
			SamplingPipelineDescription.ColorFormat = PulseForge::ColorTargetFormat::Swapchain;
			SamplingPipelineDescription.Depth.TestEnabled = false;
			SamplingPipelineDescription.Depth.WriteEnabled = false;
			SamplingPipelineDescription.DebugName = "RGBA16F validation sampling pipeline";
			auto SamplingPipelineResult = App.CreateGraphicsPipeline(SamplingPipelineDescription);
			if (!SamplingPipelineResult)
				throw std::runtime_error("RGBA16F sampling-pipeline creation failed: " + SamplingPipelineResult.error().Message);
			m_SamplingPipeline = std::move(SamplingPipelineResult.value());
		}

		void InitializeSceneResources()
		{
			auto OpenedProject = PulseForge::Project::Open(std::filesystem::path(PF_GPU_VALIDATION_PROJECT_FILE));
			if (!OpenedProject)
				throw std::runtime_error("Could not open bundled GPU-validation project: " + OpenedProject.error().Message);
			m_Project.emplace(std::move(*OpenedProject));
			if (!m_Project->GetDescription().StartScene)
				throw std::runtime_error("Bundled GPU-validation project has no start scene");
			const auto SceneLoad = PulseForge::SceneAssetService::Load(
				*m_Project->GetDescription().StartScene,
				m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(),
				m_Scene);
			if (!SceneLoad)
				throw std::runtime_error("Could not load bundled GPU-validation scene: " + SceneLoad.error().Message);

			auto CreatedRenderer = PulseForge::SceneRenderer::Create(
				PulseForge::Application::Get(),
				*m_Project,
				std::filesystem::path(PF_GPU_VALIDATION_SCENE_SHADER_DIRECTORY));
			if (!CreatedRenderer)
				throw std::runtime_error("Could not create SceneRenderer for HDR validation: " + CreatedRenderer.error().Message);
			m_SceneRenderer = std::move(*CreatedRenderer);
			BuildScenario(m_EmptyScene, false, false, false);
			BuildScenario(m_EnvironmentOnlyScene, false, true, false);
			BuildScenario(m_GeometryOnlyScene, true, false, false);
			BuildScenario(m_DirectionalOnlyScene, true, false, true);
			ValidatePreparationFailureHandling();
			const auto MissingSnapshot = m_SceneRenderer->RenderPreparedSceneToHdr(64, 64);
			if (MissingSnapshot || MissingSnapshot.error().Code != PulseForge::SceneRendererErrorCode::SnapshotBuildFailed)
				throw std::runtime_error("HDR rendering did not reject a missing prepared snapshot");
			PF_INFO("SceneRenderer rejected HDR rendering before scene preparation as expected");
		}

		void InitializeLocalLightScenario()
		{
			BuildScenario(m_LocalLightsScene, true, true, true);
			const std::array<PulseForge::PointLightComponent, 2> Points = {
				PulseForge::PointLightComponent{ { 1.0f, 0.35f, 0.15f }, 18.0f, 12.0f },
				PulseForge::PointLightComponent{ { 0.15f, 0.35f, 1.0f }, 12.0f, 10.0f }
			};
			for (size_t Index = 0; Index < Points.size(); ++Index)
			{
				auto Light = m_LocalLightsScene.CreateEntity("GPU validation point light");
				if (!Light || !Light->SetPointLight(Points[Index]))
					throw std::runtime_error("Could not create GPU validation point light");
				auto Transform = Light->GetTransform();
				if (!Transform)
					throw std::runtime_error("Could not read GPU validation point-light transform");
				Transform->Translation = { static_cast<float>(Index) * 2.0f - 1.0f, 2.0f, 1.0f };
				if (!Light->SetTransform(*Transform))
					throw std::runtime_error("Could not position GPU validation point light");
				m_LocalLightEntities.push_back(*Light);
			}
			const std::array<PulseForge::SpotLightComponent, 2> Spots = {
				PulseForge::SpotLightComponent{ { 0.3f, 1.0f, 0.4f }, 24.0f, 18.0f, 10.0f, 28.0f },
				PulseForge::SpotLightComponent{ { 1.0f, 0.75f, 0.25f }, 20.0f, 16.0f, 12.0f, 32.0f }
			};
			for (size_t Index = 0; Index < Spots.size(); ++Index)
			{
				auto Light = m_LocalLightsScene.CreateEntity("GPU validation spot light");
				if (!Light || !Light->SetSpotLight(Spots[Index]))
					throw std::runtime_error("Could not create GPU validation spot light");
				auto Transform = Light->GetTransform();
				if (!Transform)
					throw std::runtime_error("Could not read GPU validation spot-light transform");
				Transform->Translation = { static_cast<float>(Index) * 3.0f - 1.5f, 3.0f, 1.5f };
				if (!Light->SetTransform(*Transform))
					throw std::runtime_error("Could not position GPU validation spot light");
				m_LocalLightEntities.push_back(*Light);
			}
			for (size_t Index = 0; Index < 30; ++Index)
			{
				auto Light = m_LocalLightsScene.CreateEntity("GPU validation capacity point light");
				if (!Light || !Light->SetPointLight(PulseForge::PointLightComponent{ { 0.4f, 0.5f, 0.6f }, 0.5f, 5.0f }))
					throw std::runtime_error("Could not create GPU validation capacity light");
				auto Transform = Light->GetTransform();
				if (!Transform)
					throw std::runtime_error("Could not read GPU validation capacity-light transform");
				Transform->Translation = { 100.0f + static_cast<float>(Index), 0.0f, 0.0f };
				if (!Light->SetTransform(*Transform))
					throw std::runtime_error("Could not position GPU validation capacity light");
				m_LocalLightEntities.push_back(*Light);
			}
			PulseForge::RenderTargetDesc OutputDescription;
			OutputDescription.Width = 128;
			OutputDescription.Height = 72;
			OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			OutputDescription.DebugName = "Local-light GPU validation SDR output";
			auto OutputTarget = PulseForge::Application::Get().CreateRenderTarget(OutputDescription);
			if (!OutputTarget)
				throw std::runtime_error("Could not create local-light SDR validation target: " + OutputTarget.error().Message);
			m_LocalLightOutputTarget = std::move(*OutputTarget);
			PF_INFO("Prepared GPU local-light scene with 32 point and 2 spot lights (over the combined 32-light budget) alongside bundled geometry, environment, and directional light");
		}

		void UpdateLocalLightTransforms()
		{
			for (size_t Index = 0; Index < m_LocalLightEntities.size(); ++Index)
			{
				const auto Current = m_LocalLightEntities[Index].GetTransform();
				if (!Current)
					throw std::runtime_error("Could not read moving GPU validation light transform");
				PulseForge::TransformComponent Updated = *Current;
				Updated.Translation.x += 0.1f;
				if (Index >= 2)
				{
					const float HalfAngle = 0.025f * static_cast<float>(m_LocalLightFrames);
					Updated.Rotation = glm::quat(std::cos(HalfAngle), 0.0f, std::sin(HalfAngle), 0.0f);
				}
				if (!m_LocalLightEntities[Index].SetTransform(Updated))
					throw std::runtime_error("Could not update moving GPU validation light transform");
			}
			if (m_LocalLightFrames == 1 || m_LocalLightFrames == 4)
			{
				auto Point = m_LocalLightEntities[0].GetPointLight();
				if (!Point || !Point->has_value())
					throw std::runtime_error("Could not update GPU validation point-light count");
				Point->value().Intensity = m_LocalLightFrames == 1 ? 0.0f : 18.0f;
				if (!m_LocalLightEntities[0].SetPointLight(Point->value()))
					throw std::runtime_error("Could not change GPU validation point-light count");
			}
			if (m_LocalLightFrames == 2 || m_LocalLightFrames == 4)
			{
				if (m_LocalLightFrames == 2)
				{
					if (!m_LocalLightEntities[2].RemoveSpotLight())
						throw std::runtime_error("Could not reduce GPU validation spotlight count");
				}
				else if (!m_LocalLightEntities[2].SetSpotLight(
					PulseForge::SpotLightComponent{ { 0.3f, 1.0f, 0.4f }, 24.0f, 18.0f, 10.0f, 28.0f }))
					throw std::runtime_error("Could not restore GPU validation spotlight count");
			}
		}

		void RenderLocalLightFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_LocalLightFrames >= 6)
				return;
			auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_LocalLightOutputTarget);
			if (!Rendered || Rendered->GeometryDrawCount == 0)
			{
				Fail(Rendered ? "Local-light validation rendered no PBR geometry" :
					"Local-light scene render failed: " + Rendered.error().Message);
				return;
			}
			PF_INFO("Local-light GPU validation frame {0}: {1} geometry draw(s), HDR generation {2}",
				m_LocalLightFrames, Rendered->GeometryDrawCount, Rendered->HdrTargetGeneration);
			++m_LocalLightFrames;
			m_ScenePrepared = false;
		}

		void InitializeEditorUiUnormResources()
		{
			PulseForge::Application& App = PulseForge::Application::Get();
			const std::filesystem::path ShaderDirectory(PF_GPU_VALIDATION_SHADER_DIRECTORY);
			const auto VertexBytecode = LoadShader(ShaderDirectory / "EditorImGui.vs.spv");
			const auto FragmentBytecode = LoadShader(ShaderDirectory / "EditorImGui.ps.spv");
			const auto PresentVertexBytecode = LoadShader(ShaderDirectory / "EditorImGuiPresent.vs.spv");
			const auto PresentFragmentBytecode = LoadShader(ShaderDirectory / "EditorImGuiPresent.ps.spv");

			PulseForge::ShaderDesc Description;
			Description.Stage = PulseForge::ShaderStage::Vertex;
			Description.EntryPoint = "VSMain";
			Description.DebugName = "Production editor ImGui vertex shader (GPU validation)";
			auto Vertex = App.CreateShader(Description, VertexBytecode);
			if (!Vertex)
				throw std::runtime_error("Could not create production editor ImGui vertex shader: " + Vertex.error().Message);
			m_EditorUiVertexShader = std::move(*Vertex);

			Description.Stage = PulseForge::ShaderStage::Fragment;
			Description.EntryPoint = "PSMain";
			Description.DebugName = "Production editor ImGui fragment shader (GPU validation)";
			auto Fragment = App.CreateShader(Description, FragmentBytecode);
			if (!Fragment)
				throw std::runtime_error("Could not create production editor ImGui fragment shader: " + Fragment.error().Message);
			m_EditorUiFragmentShader = std::move(*Fragment);

			Description.Stage = PulseForge::ShaderStage::Vertex;
			Description.EntryPoint = "VSMain";
			Description.DebugName = "Editor UI UNORM presentation vertex shader";
			auto PresentVertex = App.CreateShader(Description, PresentVertexBytecode);
			if (!PresentVertex)
				throw std::runtime_error("Could not create UI presentation vertex shader: " + PresentVertex.error().Message);
			m_EditorUiPresentationVertexShader = std::move(*PresentVertex);

			Description.Stage = PulseForge::ShaderStage::Fragment;
			Description.EntryPoint = "PSMain";
			Description.DebugName = "Editor UI UNORM presentation fragment shader";
			auto PresentFragment = App.CreateShader(Description, PresentFragmentBytecode);
			if (!PresentFragment)
				throw std::runtime_error("Could not create UI presentation fragment shader: " + PresentFragment.error().Message);
			m_EditorUiPresentationFragmentShader = std::move(*PresentFragment);

			constexpr std::array<std::byte, 4> WhitePixel = {
				std::byte{ 0xff }, std::byte{ 0xff }, std::byte{ 0xff }, std::byte{ 0xff }
			};
			PulseForge::TextureDesc FontTextureDescription;
			FontTextureDescription.Width = 1;
			FontTextureDescription.Height = 1;
			FontTextureDescription.Format = PulseForge::TextureFormat::RGBA8_UNorm;
			FontTextureDescription.Usage = PulseForge::TextureUsage::ShaderResource;
			FontTextureDescription.DebugName = "Editor UI validation white font texel";
			auto FontTexture = App.CreateTexture(FontTextureDescription, WhitePixel);
			if (!FontTexture)
				throw std::runtime_error("Could not create the editor UI validation texel: " + FontTexture.error().Message);
			m_EditorUiFontTexture = std::move(*FontTexture);

			PulseForge::BindingSetDesc UiBindingDescription;
			UiBindingDescription.Layout = m_BindingLayout;
			UiBindingDescription.Textures.push_back({ 0, *m_EditorUiFontTexture, PulseForge::BindingResourceType::Texture2D });
			UiBindingDescription.Samplers.push_back({ 0, *m_Sampler });
			auto UiBinding = App.CreateBindingSet(UiBindingDescription);
			if (!UiBinding)
				throw std::runtime_error("Could not bind the editor UI validation texel: " + UiBinding.error().Message);
			m_EditorUiDrawBindingSet = std::move(*UiBinding);

			PulseForge::GraphicsPipelineDesc UiPipeline;
			UiPipeline.VertexShader = m_EditorUiVertexShader;
			UiPipeline.FragmentShader = m_EditorUiFragmentShader;
			UiPipeline.BindingLayouts = { m_BindingLayout };
			UiPipeline.VertexLayout.Stride = sizeof(EditorValidationVertex);
			UiPipeline.VertexLayout.Attributes = {
				{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2, offsetof(EditorValidationVertex, Position) },
				{ PulseForge::VertexSemantic::Color, PulseForge::VertexFormat::Float4, offsetof(EditorValidationVertex, Color) },
				{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(EditorValidationVertex, TexCoord) }
			};
			UiPipeline.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			UiPipeline.DepthAttachmentEnabled = false;
			UiPipeline.Blend.Enabled = true;
			UiPipeline.DebugName = "Production editor ImGui linear-light sRGB composition validation";
			auto UiPipelineResult = App.CreateGraphicsPipeline(UiPipeline);
			if (!UiPipelineResult)
				throw std::runtime_error("Could not create production editor ImGui pipeline: " + UiPipelineResult.error().Message);
			m_EditorUiPipeline = std::move(*UiPipelineResult);
			constexpr std::array<EditorValidationVertex, 3> UiVertices = {{
				{{ -1.0f, -1.0f }, { 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f, 0.5f }},
				{{ 3.0f, -1.0f }, { 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f, 0.5f }},
				{{ -1.0f, 3.0f }, { 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f, 0.5f }}
			}};
			PulseForge::BufferDesc UiVertexDescription;
			UiVertexDescription.ByteSize = sizeof(UiVertices);
			UiVertexDescription.Usage = PulseForge::BufferUsage::Vertex;
			UiVertexDescription.DebugName = "Production editor ImGui alpha-blend validation triangle";
			auto UiVertexBuffer = App.CreateBuffer(UiVertexDescription, std::as_bytes(std::span(UiVertices)));
			if (!UiVertexBuffer)
				throw std::runtime_error("Could not create editor UI validation draw data: " + UiVertexBuffer.error().Message);
			m_EditorUiVertexBuffer = std::move(*UiVertexBuffer);

			PulseForge::GraphicsPipelineDesc PresentationPipeline;
			PresentationPipeline.VertexShader = m_EditorUiPresentationVertexShader;
			PresentationPipeline.FragmentShader = m_EditorUiPresentationFragmentShader;
			PresentationPipeline.BindingLayouts = { m_BindingLayout };
			PresentationPipeline.VertexLayout.Stride = sizeof(ValidationVertex);
			PresentationPipeline.VertexLayout.Attributes = {
				{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2, offsetof(ValidationVertex, Position) }
			};
			PresentationPipeline.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_UNorm;
			PresentationPipeline.DepthAttachmentEnabled = false;
			PresentationPipeline.DebugName = "Editor UI UNORM output transfer validation";
			auto PresentationPipelineResult = App.CreateGraphicsPipeline(PresentationPipeline);
			if (!PresentationPipelineResult)
				throw std::runtime_error("Could not create UI presentation pipeline: " + PresentationPipelineResult.error().Message);
			m_EditorUiPresentationPipeline = std::move(*PresentationPipelineResult);

			constexpr std::array<ValidationVertex, 3> Fullscreen = {
				ValidationVertex{ { -1.0f, -1.0f }, {} },
				ValidationVertex{ { 3.0f, -1.0f }, {} },
				ValidationVertex{ { -1.0f, 3.0f }, {} }
			};
			PulseForge::BufferDesc BufferDescription;
			BufferDescription.ByteSize = sizeof(Fullscreen);
			BufferDescription.Usage = PulseForge::BufferUsage::Vertex;
			BufferDescription.DebugName = "Editor UI presentation fullscreen triangle validation";
			auto Buffer = App.CreateBuffer(BufferDescription, std::as_bytes(std::span(Fullscreen)));
			if (!Buffer)
				throw std::runtime_error("Could not create UI presentation vertex buffer: " + Buffer.error().Message);
			m_EditorUiFullscreenBuffer = std::move(*Buffer);
		}

		void EnsureEditorUiTargets(uint32_t Width, uint32_t Height)
		{
			if (m_EditorUiSrgbTarget && m_EditorUiUnormTarget && m_EditorUiBindingSet &&
				m_EditorUiWidth == Width && m_EditorUiHeight == Height)
				return;

			PulseForge::Application& App = PulseForge::Application::Get();
			PulseForge::RenderTargetDesc Description;
			Description.Width = Width;
			Description.Height = Height;
			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			Description.DepthMode = PulseForge::DepthAttachmentMode::None;
			Description.DebugName = "Editor UI linear-light validation intermediate";
			auto SrgbTarget = App.CreateRenderTarget(Description);
			if (!SrgbTarget)
				throw std::runtime_error("Could not create UI sRGB intermediate: " + SrgbTarget.error().Message);
			const PulseForge::Texture* SrgbTexture = (*SrgbTarget)->GetColorTexture();
			if (!SrgbTexture)
				throw std::runtime_error("UI sRGB intermediate has no sampleable color texture");

			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_UNorm;
			Description.DebugName = "Editor UI UNORM validation output";
			auto UnormTarget = App.CreateRenderTarget(Description);
			if (!UnormTarget)
				throw std::runtime_error("Could not create UI UNORM output target: " + UnormTarget.error().Message);

			PulseForge::BindingSetDesc BindingDescription;
			BindingDescription.Layout = m_BindingLayout;
			BindingDescription.Textures.push_back({ 0, *SrgbTexture, PulseForge::BindingResourceType::Texture2D });
			BindingDescription.Samplers.push_back({ 0, *m_Sampler });
			auto Binding = App.CreateBindingSet(BindingDescription);
			if (!Binding)
				throw std::runtime_error("Could not bind UI sRGB intermediate: " + Binding.error().Message);

			m_EditorUiBindingSet = std::move(*Binding);
			m_EditorUiSrgbTarget = std::move(*SrgbTarget);
			m_EditorUiUnormTarget = std::move(*UnormTarget);
			m_EditorUiWidth = Width;
			m_EditorUiHeight = Height;
			++m_EditorUiTargetCreations;
			PF_INFO("Created editor UI validation targets: {}x{} sRGB intermediate + UNORM output", Width, Height);
		}

		void RenderEditorUiUnormFrame()
		{
			if (!m_Succeeded || m_EditorUiFrames >= 3)
				return;
			const uint32_t Width = m_EditorUiFrames < 2 ? 32u : 48u;
			const uint32_t Height = 32u;
			try
			{
				EnsureEditorUiTargets(Width, Height);
				const PulseForge::Texture* Source = m_EditorUiSrgbTarget->GetColorTexture();
				const PulseForge::Texture* Destination = m_EditorUiUnormTarget->GetColorTexture();
				if (!Source || !Destination || Source->GetDescription().Format != PulseForge::TextureFormat::RGBA8_Srgb ||
					Destination->GetDescription().Format != PulseForge::TextureFormat::RGBA8_UNorm ||
					Source->GetDescription().Width != Width || Source->GetDescription().Height != Height ||
					Destination->GetDescription().Width != Width || Destination->GetDescription().Height != Height ||
					m_EditorUiSrgbTarget->GetDescription().DepthMode != PulseForge::DepthAttachmentMode::None ||
					m_EditorUiUnormTarget->GetDescription().DepthMode != PulseForge::DepthAttachmentMode::None)
					throw std::runtime_error("Editor UI validation target format or extent was incorrect");
				if (m_EditorUiFrames == 0)
					m_EditorUiFirstTexture = Source;
				else if (m_EditorUiFrames == 1 && Source != m_EditorUiFirstTexture)
					throw std::runtime_error("Same-sized editor UI target was not reused");
				if (m_EditorUiTargetCreations != (m_EditorUiFrames < 2 ? 1u : 2u))
					throw std::runtime_error("Editor UI target reuse or resize recreation count was incorrect");

				PulseForge::RenderTargetClearValue Clear;
				Clear.Color = { 0.0f, 0.0f, 0.0f, 1.0f };
				if (const auto Begin = PulseForge::Application::Get().BeginRenderTarget(*m_EditorUiSrgbTarget, Clear); !Begin)
					throw std::runtime_error("Could not begin editor UI sRGB target: " + Begin.error().Message);
				const std::array<const PulseForge::BindingSet*, 1> UiBindings = { m_EditorUiDrawBindingSet.get() };
				const auto BlendDraw = PulseForge::Application::Get().Draw(
					*m_EditorUiPipeline,
					*m_EditorUiVertexBuffer,
					{ 3, 1, 0, 0 },
					UiBindings);
				const auto BlendEnd = PulseForge::Application::Get().EndRenderTarget();
				if (!BlendDraw)
					throw std::runtime_error("Could not record alpha blend into sRGB UI target: " + BlendDraw.error().Message);
				if (!BlendEnd)
					throw std::runtime_error("Could not end editor UI sRGB target: " + BlendEnd.error().Message);

				if (const auto Begin = PulseForge::Application::Get().BeginRenderTarget(*m_EditorUiUnormTarget, Clear); !Begin)
					throw std::runtime_error("Could not begin editor UI UNORM output: " + Begin.error().Message);
				const std::array<const PulseForge::BindingSet*, 1> Bindings = { m_EditorUiBindingSet.get() };
				const auto PresentDraw = PulseForge::Application::Get().Draw(
					*m_EditorUiPresentationPipeline, *m_EditorUiFullscreenBuffer, { 3, 1, 0, 0 }, Bindings);
				const auto PresentEnd = PulseForge::Application::Get().EndRenderTarget();
				if (!PresentDraw)
					throw std::runtime_error("Could not record editor UI UNORM output-transfer draw: " + PresentDraw.error().Message);
				if (!PresentEnd)
					throw std::runtime_error("Could not end editor UI UNORM output: " + PresentEnd.error().Message);
				++m_EditorUiFrames;
			}
			catch (const std::exception& Exception)
			{
				Fail(Exception.what());
			}
		}

		void RenderSceneHdrFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || (m_SceneCompletedFrames >= 4 && m_ScenarioIndex >= 4))
				return;

			PulseForge::Application& App = PulseForge::Application::Get();
			if (m_SceneCompletedFrames == 0)
			{
				const auto InvalidExtent = m_SceneRenderer->RenderPreparedSceneToHdr(0, 64);
				if (InvalidExtent || InvalidExtent.error().Code != PulseForge::SceneRendererErrorCode::ResourceCreationFailed)
				{
					Fail("HDR target did not reject a zero-width extent");
					return;
				}
			}
			if (m_SceneCompletedFrames == 2)
				m_SamplingBindingSet.reset();

			const bool SampleValidationScene = m_SceneCompletedFrames < 4;
			const uint32_t Width = SampleValidationScene && m_SceneCompletedFrames < 2 ? 64u : 96u;
			const uint32_t Height = 64u;
			auto Rendered = m_SceneRenderer->RenderPreparedSceneToHdr(Width, Height);
			if (!Rendered)
			{
				Fail("SceneRenderer HDR rendering failed: " + Rendered.error().Message);
				return;
			}
			const PulseForge::Texture* Color = Rendered->ColorTexture;
			const bool ExpectedGeometry = SampleValidationScene || m_ScenarioIndex >= 2;
			const bool ExpectedEnvironment = SampleValidationScene || m_ScenarioIndex == 1;
			if (!Color || !Rendered->DepthTexture || Rendered->Width != Width || Rendered->Height != Height ||
				(Rendered->GeometryDrawCount > 0) != ExpectedGeometry ||
				Rendered->EnvironmentBackgroundDrawn != ExpectedEnvironment)
			{
				Fail("SceneRenderer HDR result did not match expected geometry/environment participation");
				return;
			}
			const PulseForge::TextureDesc& ColorDescription = Color->GetDescription();
			if (ColorDescription.Width != Width || ColorDescription.Height != Height ||
				ColorDescription.Format != PulseForge::TextureFormat::RGBA16_Float ||
				!PulseForge::HasTextureUsage(ColorDescription.Usage, PulseForge::TextureUsage::ShaderResource) ||
				!PulseForge::HasTextureUsage(ColorDescription.Usage, PulseForge::TextureUsage::ColorAttachment))
			{
				Fail("SceneRenderer HDR color texture has incompatible metadata or usage");
				return;
			}
			const PulseForge::TextureDesc& DepthDescription = Rendered->DepthTexture->GetDescription();
			if (DepthDescription.Width != Width || DepthDescription.Height != Height ||
				DepthDescription.Format != PulseForge::TextureFormat::Depth32Float ||
				!PulseForge::HasTextureUsage(DepthDescription.Usage, PulseForge::TextureUsage::DepthStencilAttachment))
			{
				Fail("SceneRenderer HDR target has incompatible D32 depth attachment metadata");
				return;
			}

			if (SampleValidationScene && m_SceneCompletedFrames == 0)
				m_FirstHdrTexture = Color;
			else if (SampleValidationScene && m_SceneCompletedFrames == 1 && Color != m_FirstHdrTexture)
			{
				Fail("SceneRenderer recreated its HDR target at an unchanged extent");
				return;
			}
			else if (SampleValidationScene && m_SceneCompletedFrames == 2 && Color == m_FirstHdrTexture)
			{
				Fail("SceneRenderer did not replace its HDR target after resize");
				return;
			}
			else if (SampleValidationScene && m_SceneCompletedFrames == 3 && Color != m_ResizedHdrTexture)
			{
				Fail("SceneRenderer recreated its resized HDR target at an unchanged extent");
				return;
			}
			if (SampleValidationScene && m_SceneCompletedFrames == 2)
				m_ResizedHdrTexture = Color;

			if (m_BoundHdrTexture != Color)
			{
				m_SamplingBindingSet.reset();
				PulseForge::BindingSetDesc SamplingDescription;
				SamplingDescription.Layout = m_BindingLayout;
				SamplingDescription.Textures.push_back({ 0, *Color, PulseForge::BindingResourceType::Texture2D });
				SamplingDescription.Samplers.push_back({ 0, *m_Sampler });
				auto SamplingSet = App.CreateBindingSet(SamplingDescription);
				if (!SamplingSet)
				{
					Fail("Could not bind SceneRenderer HDR output for sampling: " + SamplingSet.error().Message);
					return;
				}
				m_SamplingBindingSet = std::move(*SamplingSet);
				m_BoundHdrTexture = Color;
			}
			const std::array<const PulseForge::BindingSet*, 1> BindingSets = { m_SamplingBindingSet.get() };
			const PulseForge::GraphicsResult SamplingDraw = App.Draw(
				*m_SamplingPipeline,
				*m_VertexBuffer,
				{ 3, 1, 0, 0 },
				BindingSets);
			if (!SamplingDraw)
			{
				Fail("Could not sample SceneRenderer HDR output: " + SamplingDraw.error().Message);
				return;
			}
			if (SampleValidationScene)
				++m_SceneCompletedFrames;
			else
				++m_ScenarioIndex;
			PF_INFO("Scene HDR validation: scenario {0}, extent {1}x{2}, {3} indexed geometry draw(s), environment background {4}",
				SampleValidationScene ? "bundled" : ScenarioNames[m_ScenarioIndex - 1], Width, Height, Rendered->GeometryDrawCount,
				Rendered->EnvironmentBackgroundDrawn ? "drawn" : "not present");
		}

		void EnsureSdrValidationTarget(PulseForge::ColorTargetFormat Format, uint32_t Width, uint32_t Height)
		{
			if (m_SdrTarget && m_SdrTarget->GetDescription().Width == Width &&
				m_SdrTarget->GetDescription().Height == Height && m_SdrTarget->GetDescription().ColorFormat == Format)
				return;
			m_SamplingBindingSet.reset();
			m_BoundOutputTexture = nullptr;
			PulseForge::RenderTargetDesc Description;
			Description.Width = Width;
			Description.Height = Height;
			Description.ColorFormat = Format;
			Description.DepthMode = Format == PulseForge::ColorTargetFormat::RGBA8_UNorm
				? PulseForge::DepthAttachmentMode::None
				: PulseForge::DepthAttachmentMode::Attachment;
			Description.DebugName = Format == PulseForge::ColorTargetFormat::RGBA8_Srgb
				? "Tone-mapping validation sRGB output"
				: "Tone-mapping validation UNORM output";
			auto Target = PulseForge::Application::Get().CreateRenderTarget(Description);
			if (!Target)
				throw std::runtime_error("Could not create SDR validation target: " + Target.error().Message);
			m_SdrTarget = std::move(*Target);
		}

		void RenderToneMappingFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_ToneMappingFrame >= 10)
				return;

			PulseForge::Application& App = PulseForge::Application::Get();
			const bool DirectToSwapchain = m_ToneMappingFrame < 2;
			const bool UseUnormTarget = m_ToneMappingFrame == 5;
			const uint32_t Width = DirectToSwapchain ? App.GetWindow().GetFramebufferSize().first :
				(m_ToneMappingFrame >= 4 ? 96u : 64u);
			const uint32_t Height = DirectToSwapchain ? App.GetWindow().GetFramebufferSize().second : 64u;
			if (Width == 0 || Height == 0)
				return;
			const std::array<float, 10> Exposures = { 0.0f, 1.0f, -2.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
			if (!m_SceneRenderer->SetToneMappingSettings({ Exposures[m_ToneMappingFrame] }))
			{
				Fail("SceneRenderer rejected a valid GPU-validation exposure setting");
				return;
			}

			PulseForge::SceneRenderOutputResult Output;
			const PulseForge::Texture* OutputTexture = nullptr;
			if (DirectToSwapchain)
			{
				auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput();
				if (!Rendered)
				{
					Fail("SceneRenderer swapchain tonemapping failed: " + Rendered.error().Message);
					return;
				}
				Output = *Rendered;
				if (Output.DestinationFormat != PulseForge::ColorTargetFormat::Swapchain ||
					Output.ShaderSrgbEncoded != (App.GetOutputColorEncoding() == PulseForge::OutputColorEncoding::UnormAttachment))
				{
					Fail("Swapchain tone-mapping output encoding did not match the active attachment");
					return;
				}
			}
			else
			{
				const PulseForge::ColorTargetFormat Format = UseUnormTarget
					? PulseForge::ColorTargetFormat::RGBA8_UNorm : PulseForge::ColorTargetFormat::RGBA8_Srgb;
				EnsureSdrValidationTarget(Format, Width, Height);
				auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_SdrTarget);
				if (!Rendered)
				{
					Fail("SceneRenderer offscreen tonemapping failed: " + Rendered.error().Message);
					return;
				}
				Output = *Rendered;
				OutputTexture = m_SdrTarget->GetColorTexture();
				const PulseForge::TextureFormat ExpectedFormat = UseUnormTarget
					? PulseForge::TextureFormat::RGBA8_UNorm : PulseForge::TextureFormat::RGBA8_Srgb;
				if (!OutputTexture || Output.DestinationFormat != Format ||
					Output.ShaderSrgbEncoded != UseUnormTarget ||
					m_SdrTarget->GetDescription().DepthMode != (UseUnormTarget
						? PulseForge::DepthAttachmentMode::None
						: PulseForge::DepthAttachmentMode::Attachment) ||
					OutputTexture->GetDescription().Format != ExpectedFormat ||
					OutputTexture->GetDescription().Width != Width || OutputTexture->GetDescription().Height != Height)
				{
					Fail("Tone-mapped SDR target format, extent, or transfer-function decision was incorrect");
					return;
				}
				if (Output.GeometryDrawCount != ((m_ToneMappingFrame == 6 || m_ToneMappingFrame == 8) ? 0u : 3u))
				{
					Fail("Tone-mapping scene variant submitted an unexpected indexed geometry count");
					return;
				}
				if (m_BoundOutputTexture != OutputTexture)
				{
					m_SamplingBindingSet.reset();
					PulseForge::BindingSetDesc Description;
					Description.Layout = m_BindingLayout;
					Description.Textures.push_back({ 0, *OutputTexture, PulseForge::BindingResourceType::Texture2D });
					Description.Samplers.push_back({ 0, *m_Sampler });
					auto Set = App.CreateBindingSet(Description);
					if (!Set)
					{
						Fail("Could not bind tone-mapped SDR output for validation sampling: " + Set.error().Message);
						return;
					}
					m_SamplingBindingSet = std::move(*Set);
					m_BoundOutputTexture = OutputTexture;
				}
				const std::array<const PulseForge::BindingSet*, 1> Sets = { m_SamplingBindingSet.get() };
				if (const auto Draw = App.Draw(*m_SamplingPipeline, *m_VertexBuffer, { 3, 1, 0, 0 }, Sets); !Draw)
				{
					Fail("Could not sample tone-mapped SDR output: " + Draw.error().Message);
					return;
				}
			}

			if (m_ToneMappingFrame == 0)
				m_SwapchainHdrGeneration = Output.HdrTargetGeneration;
			else if (m_ToneMappingFrame == 1 && Output.HdrTargetGeneration != m_SwapchainHdrGeneration)
			{
				Fail("Unchanged swapchain extent recreated the HDR scene target");
				return;
			}
			if (m_ToneMappingFrame == 2)
				m_SrgbHdrGeneration = Output.HdrTargetGeneration;
			else if (m_ToneMappingFrame == 3 && Output.HdrTargetGeneration != m_SrgbHdrGeneration)
			{
				Fail("Exposure changes recreated the unchanged HDR target");
				return;
			}
			if (m_ToneMappingFrame == 4 && Output.HdrTargetGeneration == m_SrgbHdrGeneration)
			{
				Fail("HDR target generation did not change after the 64x64 to 96x64 resize");
				return;
			}
			if (m_ToneMappingFrame == 4)
				m_ResizedHdrGeneration = Output.HdrTargetGeneration;
			else if (m_ToneMappingFrame >= 5 && Output.HdrTargetGeneration != m_ResizedHdrGeneration)
			{
				Fail("Changing output format or scene scenario recreated the same-size HDR target");
				return;
			}
			PF_INFO("Tonemapping validation frame {0}: {1}, EV {2}, {3}x{4}, HDR generation {5}, shader sRGB encode {6}",
				m_ToneMappingFrame,
				DirectToSwapchain ? "swapchain" : UseUnormTarget ? "RGBA8_UNorm" : "RGBA8_Srgb",
				Exposures[m_ToneMappingFrame], Width, Height, Output.HdrTargetGeneration, Output.ShaderSrgbEncoded);
			++m_ToneMappingFrame;
		}

		PulseForge::Scene& GetScenarioScene(size_t Index)
		{
			switch (Index)
			{
				case 0: return m_EmptyScene;
				case 1: return m_EnvironmentOnlyScene;
				case 2: return m_GeometryOnlyScene;
				default: return m_DirectionalOnlyScene;
			}
		}

		void BuildScenario(PulseForge::Scene& Destination, bool IncludeGeometry, bool IncludeEnvironment, bool IncludeDirectional)
		{
			bool HasCamera = false;
			for (const PulseForge::Entity Source : m_Scene.GetEntities())
			{
				const auto Transform = Source.GetTransform();
				const auto Camera = Source.GetCamera();
				if (!Transform || !Camera)
					throw std::runtime_error("Could not inspect bundled scene entities for GPU validation scenarios");
				if (!HasCamera && *Camera)
				{
					auto Entity = Destination.CreateEntity("GPU validation camera");
					if (!Entity)
						throw std::runtime_error("Could not create scenario camera: " + Entity.error().Message);
					PulseForge::CameraComponent Component = **Camera;
					Component.IsPrimary = true;
					if (auto Result = Entity->SetTransform(*Transform); !Result)
						throw std::runtime_error("Could not set scenario camera transform: " + Result.error().Message);
					if (auto Result = Entity->SetCamera(Component); !Result)
						throw std::runtime_error("Could not set scenario camera: " + Result.error().Message);
					HasCamera = true;
				}

				const auto Environment = Source.GetEnvironmentLight();
				const auto Directional = Source.GetDirectionalLight();
				const auto Mesh = Source.GetMeshRenderer();
				if (!Environment || !Directional || !Mesh)
					throw std::runtime_error("Could not inspect bundled scene components for GPU validation scenarios");
				if ((IncludeEnvironment && *Environment) || (IncludeDirectional && *Directional) || (IncludeGeometry && *Mesh))
				{
					auto Entity = Destination.CreateEntity("GPU validation scenario object");
					if (!Entity)
						throw std::runtime_error("Could not create scenario object: " + Entity.error().Message);
					if (auto Result = Entity->SetTransform(*Transform); !Result)
						throw std::runtime_error("Could not set scenario object transform: " + Result.error().Message);
					if (IncludeEnvironment && *Environment)
						if (auto Result = Entity->SetEnvironmentLight(**Environment); !Result)
							throw std::runtime_error("Could not set scenario environment: " + Result.error().Message);
					if (IncludeDirectional && *Directional)
						if (auto Result = Entity->SetDirectionalLight(**Directional); !Result)
							throw std::runtime_error("Could not set scenario directional light: " + Result.error().Message);
					if (IncludeGeometry && *Mesh)
						if (auto Result = Entity->SetMeshRenderer(**Mesh); !Result)
							throw std::runtime_error("Could not set scenario mesh: " + Result.error().Message);
				}
			}
			if (!HasCamera)
				throw std::runtime_error("Bundled validation scene has no camera for GPU scenario setup");
		}

		void ValidatePreparationFailureHandling()
		{
			PulseForge::Scene InvalidScene;
			auto CameraEntity = InvalidScene.CreateEntity("GPU validation invalid-scene camera");
			if (!CameraEntity)
				throw std::runtime_error("Could not create invalid-scene camera: " + CameraEntity.error().Message);
			PulseForge::CameraComponent Camera;
			Camera.IsPrimary = true;
			if (auto Result = CameraEntity->SetCamera(Camera); !Result)
				throw std::runtime_error("Could not set invalid-scene camera: " + Result.error().Message);
			auto MeshEntity = InvalidScene.CreateEntity("GPU validation missing mesh");
			if (!MeshEntity)
				throw std::runtime_error("Could not create invalid-scene mesh entity: " + MeshEntity.error().Message);
			const PulseForge::MeshRendererComponent MissingMesh{
				PulseForge::AssetID{ 0x13579bdf2468ace0ull, 0x1029384756abcdefull }, {}};
			if (auto Result = MeshEntity->SetMeshRenderer(MissingMesh); !Result)
				throw std::runtime_error("Could not set missing-mesh component: " + Result.error().Message);
			const auto Preparation = m_SceneRenderer->PrepareScene(InvalidScene, 1.0f);
			if (Preparation || Preparation.error().Code != PulseForge::SceneRendererErrorCode::AssetLoadFailed)
				throw std::runtime_error("SceneRenderer did not propagate a missing mesh preparation error");
			PF_INFO("SceneRenderer propagated missing mesh preparation failure as expected");
		}

		PulseForge::RenderTargetHandle m_Target;
		PulseForge::BufferHandle m_VertexBuffer;
		PulseForge::BindingLayoutHandle m_BindingLayout;
		PulseForge::SamplerHandle m_Sampler;
		PulseForge::BindingSetHandle m_SamplingBindingSet;
		PulseForge::GraphicsPipelineHandle m_ColorPipeline;
		PulseForge::GraphicsPipelineHandle m_SamplingPipeline;
		PulseForge::RenderTargetHandle m_SdrTarget;
		PulseForge::ShaderHandle m_EditorUiVertexShader;
		PulseForge::ShaderHandle m_EditorUiFragmentShader;
		PulseForge::ShaderHandle m_EditorUiPresentationVertexShader;
		PulseForge::ShaderHandle m_EditorUiPresentationFragmentShader;
		PulseForge::TextureHandle m_EditorUiFontTexture;
		PulseForge::BindingSetHandle m_EditorUiDrawBindingSet;
		PulseForge::BindingSetHandle m_EditorUiBindingSet;
		PulseForge::GraphicsPipelineHandle m_EditorUiPipeline;
		PulseForge::GraphicsPipelineHandle m_EditorUiPresentationPipeline;
		PulseForge::RenderTargetHandle m_EditorUiSrgbTarget;
		PulseForge::RenderTargetHandle m_EditorUiUnormTarget;
		PulseForge::BufferHandle m_EditorUiVertexBuffer;
		PulseForge::BufferHandle m_EditorUiFullscreenBuffer;
		const PulseForge::Texture* m_EditorUiFirstTexture = nullptr;
		std::optional<PulseForge::Project> m_Project;
		PulseForge::Scene m_Scene;
		PulseForge::Scene m_EmptyScene;
		PulseForge::Scene m_EnvironmentOnlyScene;
		PulseForge::Scene m_GeometryOnlyScene;
		PulseForge::Scene m_DirectionalOnlyScene;
		PulseForge::Scene m_LocalLightsScene;
		std::vector<PulseForge::Entity> m_LocalLightEntities;
		PulseForge::RenderTargetHandle m_LocalLightOutputTarget;
		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		const PulseForge::Texture* m_FirstHdrTexture = nullptr;
		const PulseForge::Texture* m_ResizedHdrTexture = nullptr;
		const PulseForge::Texture* m_BoundHdrTexture = nullptr;
		const PulseForge::Texture* m_BoundOutputTexture = nullptr;
		std::chrono::steady_clock::time_point m_StartedAt{};
		uint32_t m_CompletedFrames = 0;
		uint32_t m_SceneCompletedFrames = 0;
		uint32_t m_LocalLightFrames = 0;
		uint32_t m_ToneMappingFrame = 0;
		uint32_t m_EditorUiFrames = 0;
		uint32_t m_EditorUiTargetCreations = 0;
		uint32_t m_EditorUiWidth = 0;
		uint32_t m_EditorUiHeight = 0;
		size_t m_ScenarioIndex = 0;
		uint64_t m_SwapchainHdrGeneration = 0;
		uint64_t m_SrgbHdrGeneration = 0;
		uint64_t m_ResizedHdrGeneration = 0;
		const std::array<const char*, 4> ScenarioNames = { "empty", "environment-only", "geometry-only", "directional-only" };
		bool m_Succeeded = true;
		bool m_ValidateSceneHdr = false;
		bool m_ValidateToneMapping = false;
		bool m_ValidateEditorUiUnorm = false;
		bool m_ValidateLocalLights = false;
		bool m_ScenePrepared = false;
	};

	class GpuValidationApplication final : public PulseForge::Application
	{
	public:
		GpuValidationApplication(bool ValidateSceneHdr, bool ValidateToneMapping, bool ValidateEditorUiUnorm, bool ValidateLocalLights)
			: Application(PulseForge::RendererAPI::Vulkan)
		{
			auto Layer = std::make_unique<GpuValidationLayer>(ValidateSceneHdr, ValidateToneMapping, ValidateEditorUiUnorm, ValidateLocalLights);
			m_Layer = Layer.get();
			PushLayer(std::move(Layer));
		}

		int RunValidation()
		{
			Run();
			return m_Layer && m_Layer->Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
		}

	private:
		GpuValidationLayer* m_Layer = nullptr;
	};
}

int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	try
	{
		const std::string_view Mode = ArgumentCount > 1 ? std::string_view(Arguments[1]) : std::string_view{};
		const bool ValidateSceneHdr = Mode == "--scene-hdr";
		const bool ValidateToneMapping = Mode == "--scene-tonemap";
		const bool ValidateEditorUiUnorm = Mode == "--editor-ui-unorm";
		const bool ValidateLocalLights = Mode == "--scene-local-lights";
		GpuValidationApplication App(ValidateSceneHdr, ValidateToneMapping, ValidateEditorUiUnorm, ValidateLocalLights);
		return App.RunValidation();
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("GPU validation startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
