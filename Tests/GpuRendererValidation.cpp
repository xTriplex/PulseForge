#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

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
		explicit GpuValidationLayer(bool ValidateSceneHdr)
			: Layer(ValidateSceneHdr ? "SceneRenderer HDR GPU validation" : "RGBA16F GPU validation"),
			  m_ValidateSceneHdr(ValidateSceneHdr)
		{
		}

		void OnAttach() override
		{
			m_StartedAt = std::chrono::steady_clock::now();
			try
			{
				InitializeResources();
				if (m_ValidateSceneHdr)
					InitializeSceneResources();
				PF_INFO("RGBA16F GPU validation resources initialized");
			}
			catch (const std::exception& Exception)
			{
				Fail(Exception.what());
			}
		}

		void OnUpdate(PulseForge::Timestep) override
		{
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
			m_SamplingBindingSet.reset();
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
			return m_Succeeded && (m_ValidateSceneHdr ? m_SceneCompletedFrames == 4 : m_CompletedFrames == 3);
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
		std::optional<PulseForge::Project> m_Project;
		PulseForge::Scene m_Scene;
		PulseForge::Scene m_EmptyScene;
		PulseForge::Scene m_EnvironmentOnlyScene;
		PulseForge::Scene m_GeometryOnlyScene;
		PulseForge::Scene m_DirectionalOnlyScene;
		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		const PulseForge::Texture* m_FirstHdrTexture = nullptr;
		const PulseForge::Texture* m_ResizedHdrTexture = nullptr;
		const PulseForge::Texture* m_BoundHdrTexture = nullptr;
		std::chrono::steady_clock::time_point m_StartedAt{};
		uint32_t m_CompletedFrames = 0;
		uint32_t m_SceneCompletedFrames = 0;
		size_t m_ScenarioIndex = 0;
		const std::array<const char*, 4> ScenarioNames = { "empty", "environment-only", "geometry-only", "directional-only" };
		bool m_Succeeded = true;
		bool m_ValidateSceneHdr = false;
		bool m_ScenePrepared = false;
	};

	class GpuValidationApplication final : public PulseForge::Application
	{
	public:
		explicit GpuValidationApplication(bool ValidateSceneHdr)
			: Application(PulseForge::RendererAPI::Vulkan)
		{
			auto Layer = std::make_unique<GpuValidationLayer>(ValidateSceneHdr);
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
		const bool ValidateSceneHdr = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--scene-hdr";
		GpuValidationApplication App(ValidateSceneHdr);
		return App.RunValidation();
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("GPU validation startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
