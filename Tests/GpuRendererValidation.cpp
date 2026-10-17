#include "Core/Application.h"
#include "GpuValidation/GpuProjectSceneValidation.h"
#include "GpuValidation/PointShadowImageValidation.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "GpuValidation/ActiveFrameUploadValidation.h"
#include "GpuValidation/EditorUiUnormValidation.h"
#include "GpuValidation/GpuLightingValidation.h"
#include "GpuValidation/GpuSceneOutputValidation.h"
#include "GpuValidation/ValidationShader.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	enum class LegacyRetirementScenario
	{
		None,
		MainRecoveryFailure,
		NativeRecoveryFailure
	};

	struct ValidationVertex
	{
		float Position[2];
		float TexCoord[2];
	};

	class GpuValidationLayer final : public PulseForge::Layer
	{
	public:
		GpuValidationLayer(bool ValidateSceneHdr, bool ValidateToneMapping, bool ValidateEditorUiUnorm, bool ValidateLocalLights,
			bool ValidateSpotlightShadows, bool ValidatePointShadows, bool ValidatePbrMaterials, bool ValidateTransparency,
			bool ValidatePointShadowImages, std::filesystem::path PointShadowImageDirectory, LegacyRetirementScenario RetirementScenario)
			: Layer(RetirementScenario == LegacyRetirementScenario::MainRecoveryFailure
						? "Main swapchain legacy-retirement recovery validation"
					: RetirementScenario == LegacyRetirementScenario::NativeRecoveryFailure
						? "Native swapchain legacy-retirement recovery validation"
						: ValidateTransparency		? "Transparency rendered-image validation"
					: ValidatePbrMaterials		? "PBR material rendered-image validation"
					: ValidatePointShadowImages ? "Point-shadow rendered-image investigation"
					: ValidatePointShadows		? "SceneRenderer point-shadow GPU validation"
					: ValidateSpotlightShadows	? "SceneRenderer spotlight-shadow GPU validation"
					: ValidateEditorUiUnorm		? "Editor UNORM UI composition GPU validation"
					: ValidateLocalLights		? "SceneRenderer local-light GPU validation"
					: ValidateToneMapping		? "HDR tonemapping GPU validation"
					: ValidateSceneHdr			? "SceneRenderer HDR GPU validation"
												: "RGBA16F GPU validation"),
			  m_ValidateSceneHdr(ValidateSceneHdr), m_ValidateToneMapping(ValidateToneMapping),
			  m_ValidateEditorUiUnorm(ValidateEditorUiUnorm), m_ValidateLocalLights(ValidateLocalLights),
			  m_ValidateSpotlightShadows(ValidateSpotlightShadows), m_ValidatePointShadows(ValidatePointShadows),
			  m_ValidatePbrMaterials(ValidatePbrMaterials), m_ValidateTransparency(ValidateTransparency),
			  m_ValidatePointShadowImages(ValidatePointShadowImages), m_PointShadowImageDirectory(std::move(PointShadowImageDirectory)),
			  m_LegacyRetirementScenario(RetirementScenario)
		{
		}

		void OnAttach() override
		{
			m_StartedAt = std::chrono::steady_clock::now();
			try
			{
				if (m_LegacyRetirementScenario != LegacyRetirementScenario::None)
				{
					auto& App = PulseForge::Application::Get();
					m_RetirementWindow = PulseForge::Window::Create(PulseForge::WindowProps(
						"PulseForge legacy retirement fixture", 640, 480, PulseForge::EWindowStyle::Default,
						PulseForge::EWindowClientAPI::None));
					if (!m_RetirementWindow)
						throw std::runtime_error("Could not create the secondary window for legacy retirement validation");
					auto PresentationTarget = App.CreatePlatformPresentationTarget(m_RetirementWindow->GetNativeWindow());
					if (!PresentationTarget)
						throw std::runtime_error("Could not create the secondary presentation target: " + PresentationTarget.error());
					m_RetirementTargetID = *PresentationTarget;
					return;
				}
				InitializeResources();
				if (m_ValidateEditorUiUnorm) m_EditorUiValidation.Initialize(PulseForge::Application::Get(), m_BindingLayout, *m_Sampler);
				if (m_ValidateSceneHdr || m_ValidateToneMapping || m_ValidateLocalLights || m_ValidateSpotlightShadows ||
					m_ValidatePointShadows || m_ValidatePbrMaterials || m_ValidateTransparency || m_ValidatePointShadowImages)
					InitializeSceneResources();
				if (m_ValidateSceneHdr || m_ValidateToneMapping)
				{
					const auto Mode = m_ValidateSceneHdr ? PulseForge::GpuValidation::SceneOutputValidationMode::Hdr
														 : PulseForge::GpuValidation::SceneOutputValidationMode::ToneMapping;
					m_SceneOutputValidation =
						std::make_unique<PulseForge::GpuValidation::GpuSceneOutputValidation>(PulseForge::Application::Get(), m_Scene,
							*m_SceneRenderer, m_BindingLayout, m_Sampler, m_SamplingPipeline, m_VertexBuffer, Mode, m_StartedAt);
					m_SceneOutputValidation->Initialize();
				}
				if (m_ValidateLocalLights || m_ValidateSpotlightShadows || m_ValidatePointShadows)
				{
					PulseForge::GpuValidation::LightingValidationMode Mode =
						m_ValidateLocalLights		 ? PulseForge::GpuValidation::LightingValidationMode::LocalLights
						: m_ValidateSpotlightShadows ? PulseForge::GpuValidation::LightingValidationMode::SpotlightShadows
													 : PulseForge::GpuValidation::LightingValidationMode::PointShadows;
					m_LightingValidation = std::make_unique<PulseForge::GpuValidation::GpuLightingValidation>(
						PulseForge::Application::Get(), m_Scene, *m_SceneRenderer, Mode, m_StartedAt);
					m_LightingValidation->Initialize();
				}
				if (m_ValidatePbrMaterials || m_ValidateTransparency)
				{
					const auto Mode = m_ValidatePbrMaterials ? PulseForge::GpuValidation::ProjectSceneValidationMode::PbrMaterials
															 : PulseForge::GpuValidation::ProjectSceneValidationMode::Transparency;
					m_ProjectSceneValidation = std::make_unique<PulseForge::GpuValidation::GpuProjectSceneValidation>(
						*m_Project, m_Scene, *m_SceneRenderer, Mode, m_PointShadowImageDirectory, m_StartedAt);
					m_ProjectSceneValidation->Initialize();
				}
				if (m_ValidatePointShadowImages)
					m_PointShadowImageValidation.Initialize(m_Scene, *m_SceneRenderer, m_PointShadowImageDirectory);
				PF_INFO("RGBA16F GPU validation resources initialized");
			}
			catch (const std::exception& Exception)
			{
				Fail(Exception.what());
			}
		}

		void OnUpdate(PulseForge::Timestep) override
		{
			if (m_LegacyRetirementScenario != LegacyRetirementScenario::None)
			{
				UpdateLegacyRetirementValidation();
				return;
			}
			if (m_ValidateEditorUiUnorm)
			{
				if (m_EditorUiValidation.CompletedFrames() >= 3)
				{
					if (m_EditorUiValidation.TargetCreations() != 2)
						Fail("Editor UI intermediate was not reused at a stable extent and replaced after resize");
					else
					{
						PF_INFO("GPU_VALIDATION_RESULT=PASS; editor UI rendered alpha-blended primitives to a persistent sRGB target, "
								"sampled it, and encoded to UNORM across reuse and resize");
						PulseForge::Application::Get().RequestClose();
					}
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(15))
					Fail("Timed out during editor UI UNORM composition validation");
				return;
			}
			if (m_ValidateLocalLights || m_ValidateSpotlightShadows || m_ValidatePointShadows)
			{
				const auto Updated = m_LightingValidation->Update();
				if (!Updated)
				{
					Fail(Updated.error());
					return;
				}
				if (*Updated)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; {0}", m_LightingValidation->CompletionSummary());
					PulseForge::Application::Get().RequestClose();
				}
				return;
			}
			if (m_ValidatePbrMaterials || m_ValidateTransparency)
			{
				m_ProjectSceneValidation->Update();
				if (auto Failure = m_ProjectSceneValidation->TakeFailure())
				{
					Fail(*Failure);
					return;
				}
				if (m_ProjectSceneValidation->IsComplete())
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; {0}; image: {1}", m_ProjectSceneValidation->CompletionSummary(),
						m_ProjectSceneValidation->CapturePath().string());
					PulseForge::Application::Get().RequestClose();
				}
				return;
			}
			if (m_ValidatePointShadowImages)
			{
				const auto Updated = m_PointShadowImageValidation.Update(m_StartedAt);
				if (!Updated)
				{
					Fail(Updated.error());
					return;
				}
				if (*Updated)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; captured real isolated and startup-scene point-shadow images, evaluated "
							"independent ray-box samples, and verified rotation invariance; images: {0}",
						m_PointShadowImageDirectory.string());
					PulseForge::Application::Get().RequestClose();
				}
				return;
			}
			if (m_ValidateToneMapping)
			{
				const auto Updated = m_SceneOutputValidation->Update();
				if (!Updated)
				{
					Fail(Updated.error());
					return;
				}
				if (*Updated)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; HDR scene tone mapping covered swapchain, sRGB and UNORM targets, exposure, "
							"resize, reuse, and scene variants");
					PulseForge::Application::Get().RequestClose();
					return;
				}
				return;
			}

			if (m_ValidateSceneHdr)
			{
				const auto Updated = m_SceneOutputValidation->Update();
				if (!Updated)
				{
					Fail(Updated.error());
					return;
				}
				if (*Updated)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; SceneRenderer rendered and sampled persistent RGBA16F targets across reuse, "
							"resize, empty, environment-only, geometry-only, and directional-only cases");
					PulseForge::Application::Get().RequestClose();
					return;
				}
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
			if (m_LegacyRetirementScenario != LegacyRetirementScenario::None)
			{
				if (PulseForge::Application::Get().IsMainFrameActive())
				{
					++m_RetirementMainFrames;
					if (m_LegacyRetirementScenario == LegacyRetirementScenario::MainRecoveryFailure)
					{
						if (m_RetirementValidationFrames < 5)
							m_SawMainFrameBeforeRecoveryFailure = true;
						else if (m_RetirementValidationFrames >= 8)
							m_MainFrameResumedAfterRecoveryFailure = true;
					}
				}
				return;
			}
			if (m_ValidateEditorUiUnorm)
			{
				if (const auto Result = m_EditorUiValidation.RenderFrame(PulseForge::Application::Get()); !Result) Fail(Result.error());
				return;
			}
			if (m_ValidateLocalLights || m_ValidateSpotlightShadows || m_ValidatePointShadows)
			{
				if (auto Rendered = m_LightingValidation->Render(); !Rendered) Fail(Rendered.error());
				return;
			}
			if (m_ValidatePbrMaterials || m_ValidateTransparency)
			{
				m_ProjectSceneValidation->Render();
				if (auto Failure = m_ProjectSceneValidation->TakeFailure()) Fail(*Failure);
				return;
			}
			if (m_ValidatePointShadowImages)
			{
				const auto Rendered = m_PointShadowImageValidation.Render();
				if (!Rendered) Fail(Rendered.error());
				return;
			}
			if (m_ValidateToneMapping)
			{
				if (const auto Result = m_SceneOutputValidation->Render(); !Result) Fail(Result.error());
				return;
			}
			if (m_ValidateSceneHdr)
			{
				if (const auto Result = m_SceneOutputValidation->Render(); !Result) Fail(Result.error());
				return;
			}
			if (!m_Succeeded || m_CompletedFrames >= 3) return;

			PulseForge::Application& App = PulseForge::Application::Get();
			const PulseForge::RenderTargetClearValue ClearValue{ .Color = { 2.0f, 0.25f, 0.125f, 1.0f }, .Depth = 1.0f };
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
			const PulseForge::GraphicsResult SamplingDrawResult =
				App.Draw(*m_SamplingPipeline, *m_VertexBuffer, DrawArguments, BindingSets);
			if (!SamplingDrawResult)
			{
				Fail("RGBA16F shader-resource sampling draw failed: " + SamplingDrawResult.error().Message);
				return;
			}

			++m_CompletedFrames;
			if (m_CompletedFrames == 1) PF_INFO("RGBA16F attachment draw, shader-resource transition, and sampling draw recorded");
		}

		void OnRenderPlatformWindows() override
		{
			if (m_RetirementTargetID == 0)
				return;
			auto& App = PulseForge::Application::Get();
			if (!App.BeginPlatformPresentationTarget(m_RetirementTargetID))
				return;
			App.EndPlatformPresentationTarget(m_RetirementTargetID);
			++m_RetirementNativeSubmissions;
		}

		void OnDetach() override
		{
			ReleaseRetirementFixture();
			const auto ReleasedPointShadowImages = m_PointShadowImageValidation.Release();
			if (!ReleasedPointShadowImages) Fail(ReleasedPointShadowImages.error());
			if (m_LightingValidation)
			{
				m_LightingValidation->Reset();
				m_LightingValidation.reset();
			}
			if (m_ProjectSceneValidation)
			{
				m_ProjectSceneValidation->Reset();
				m_ProjectSceneValidation.reset();
			}
			m_EditorUiValidation.Reset();
			m_SceneOutputValidation.reset();
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
			return m_Succeeded && (m_LegacyRetirementScenario != LegacyRetirementScenario::None
							? m_LegacyRetirementValidationComplete
							: m_ValidateEditorUiUnorm			? m_EditorUiValidation.IsComplete()
									  : m_ValidateTransparency		? (m_ProjectSceneValidation && m_ProjectSceneValidation->Succeeded())
									  : m_ValidatePbrMaterials		? (m_ProjectSceneValidation && m_ProjectSceneValidation->Succeeded())
									  : m_ValidatePointShadowImages ? m_PointShadowImageValidation.IsComplete()
									  : m_ValidatePointShadows || m_ValidateSpotlightShadows || m_ValidateLocalLights
										  ? (m_LightingValidation && m_LightingValidation->IsComplete())
									  : m_ValidateToneMapping ? (m_SceneOutputValidation && m_SceneOutputValidation->IsComplete())
									  : m_ValidateSceneHdr	  ? (m_SceneOutputValidation && m_SceneOutputValidation->IsComplete())
															  : m_CompletedFrames == 3);
		}

	private:
		void Fail(const std::string& Message)
		{
			m_Succeeded = false;
			PF_ERROR("GPU_VALIDATION_RESULT=FAIL: {0}", Message);
			PulseForge::Application::Get().RequestClose();
		}

		void UpdateLegacyRetirementValidation()
		{
			if (!m_Succeeded)
				return;
			auto& App = PulseForge::Application::Get();
			++m_RetirementValidationFrames;

			if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(10))
			{
				Fail("Timed out while exercising bounded legacy swapchain recovery");
				return;
			}
			if (m_RetirementValidationFrames < 16)
				return;

			if (m_LegacyRetirementScenario == LegacyRetirementScenario::MainRecoveryFailure)
			{
				if (!m_SawMainFrameBeforeRecoveryFailure || m_MainFrameResumedAfterRecoveryFailure ||
					m_RetirementNativeSubmissions < 12)
				{
					Fail("The failed main recovery was not bounded or the independent native host stopped submitting frames");
					return;
				}
				PF_INFO("GPU_VALIDATION_RESULT=PASS; failed main cap recovery remained blocked while the native host submitted {} frames",
					m_RetirementNativeSubmissions);
			}
			else if (m_RetirementMainFrames < 12 || m_RetirementNativeSubmissions != 0)
			{
				Fail("The failed native recovery retried instead of blocking or prevented main frame rendering");
				return;
			}
			else
				PF_INFO("GPU_VALIDATION_RESULT=PASS; failed native cap recovery remained blocked while the main host ran {} frame callbacks",
					m_RetirementMainFrames);

			if (!ReleaseRetirementFixture())
			{
				Fail("Could not release native presentation resources after the cap-recovery scenario");
				return;
			}
			m_LegacyRetirementValidationComplete = true;
			App.RequestClose();
		}

		bool ReleaseRetirementFixture()
		{
			if (m_RetirementTargetID != 0)
			{
				auto& App = PulseForge::Application::Get();
				if (!App.DestroyPlatformPresentationTarget(m_RetirementTargetID))
					return false;
				m_RetirementTargetID = 0;
			}
			m_RetirementWindow.reset();
			return !PulseForge::Application::Get().HasPlatformPresentationTargets();
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
			if (!TargetResult) throw std::runtime_error("RGBA16F render-target creation failed: " + TargetResult.error().Message);
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
			const std::vector<std::byte> VertexBytecode =
				PulseForge::GpuValidation::LoadValidationShader(ShaderDirectory / "GpuRendererValidation.vs.spv");
			const std::vector<std::byte> ColorBytecode =
				PulseForge::GpuValidation::LoadValidationShader(ShaderDirectory / "GpuRendererValidation.color.ps.spv");
			const std::vector<std::byte> SamplingBytecode =
				PulseForge::GpuValidation::LoadValidationShader(ShaderDirectory / "GpuRendererValidation.sample.ps.spv");

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
			LayoutDescription.Items = { { PulseForge::BindingResourceType::Texture2D, 0 },
				{ PulseForge::BindingResourceType::Sampler, 0 } };
			LayoutDescription.DebugName = "RGBA16F validation sampling layout";
			auto LayoutResult = App.CreateBindingLayout(LayoutDescription);
			if (!LayoutResult) throw std::runtime_error("GPU-validation binding-layout creation failed: " + LayoutResult.error().Message);
			m_BindingLayout = std::move(LayoutResult.value());

			PulseForge::SamplerDesc SamplerDescription;
			SamplerDescription.Minification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.Magnification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.AddressU = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.AddressV = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.DebugName = "RGBA16F validation linear sampler";
			auto SamplerResult = App.CreateSampler(SamplerDescription);
			if (!SamplerResult) throw std::runtime_error("GPU-validation sampler creation failed: " + SamplerResult.error().Message);
			m_Sampler = std::move(SamplerResult.value());

			PulseForge::BindingSetDesc BindingDescription;
			BindingDescription.Layout = m_BindingLayout;
			BindingDescription.Textures.push_back({ 0, *ColorTexture, PulseForge::BindingResourceType::Texture2D });
			BindingDescription.Samplers.push_back({ 0, *m_Sampler });
			auto BindingResult = App.CreateBindingSet(BindingDescription);
			if (!BindingResult)
				throw std::runtime_error("GPU-validation sampling binding creation failed: " + BindingResult.error().Message);
			m_SamplingBindingSet = std::move(BindingResult.value());

			const std::array<ValidationVertex, 3> Vertices = { ValidationVertex{ { -1.0f, -1.0f }, { 0.0f, 1.0f } },
				ValidationVertex{ { 3.0f, -1.0f }, { 2.0f, 1.0f } }, ValidationVertex{ { -1.0f, 3.0f }, { 0.0f, -1.0f } } };
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
			ColorPipelineDescription.VertexLayout.Attributes = { { PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2,
																	 offsetof(ValidationVertex, Position) },
				{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(ValidationVertex, TexCoord) } };
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
			const std::filesystem::path ProjectFile = m_ValidateTransparency   ? std::filesystem::path(PF_TRANSPARENCY_DEMO_PROJECT_FILE)
													  : m_ValidatePbrMaterials ? std::filesystem::path(PF_PBR_DEMO_PROJECT_FILE)
													  : m_ValidatePointShadows || m_ValidatePointShadowImages
														  ? std::filesystem::path(PF_POINT_SHADOW_DEMO_PROJECT_FILE)
														  : std::filesystem::path(PF_GPU_VALIDATION_PROJECT_FILE);
			auto OpenedProject = PulseForge::Project::Open(ProjectFile);
			if (!OpenedProject)
				throw std::runtime_error(
					"Could not open GPU-validation project '" + ProjectFile.string() + "': " + OpenedProject.error().Message);
			m_Project.emplace(std::move(*OpenedProject));
			if (!m_Project->GetDescription().StartScene) throw std::runtime_error("GPU-validation project has no start scene");
			const auto SceneLoad = PulseForge::SceneAssetService::Load(
				*m_Project->GetDescription().StartScene, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), m_Scene);
			if (!SceneLoad) throw std::runtime_error("Could not load GPU-validation project startup scene: " + SceneLoad.error().Message);

			auto CreatedRenderer = PulseForge::SceneRenderer::Create(
				PulseForge::Application::Get(), *m_Project, std::filesystem::path(PF_GPU_VALIDATION_SCENE_SHADER_DIRECTORY));
			if (!CreatedRenderer)
				throw std::runtime_error("Could not create SceneRenderer for HDR validation: " + CreatedRenderer.error().Message);
			m_SceneRenderer = std::move(*CreatedRenderer);
			ValidatePreparationFailureHandling();
			const auto MissingSnapshot = m_SceneRenderer->RenderPreparedSceneToHdr(64, 64);
			if (MissingSnapshot || MissingSnapshot.error().Code != PulseForge::SceneRendererErrorCode::SnapshotBuildFailed)
				throw std::runtime_error("HDR rendering did not reject a missing prepared snapshot");
			PF_INFO("SceneRenderer rejected HDR rendering before scene preparation as expected");
		}

		void ValidatePreparationFailureHandling()
		{
			PulseForge::Scene InvalidScene;
			auto CameraEntity = InvalidScene.CreateEntity("GPU validation invalid-scene camera");
			if (!CameraEntity) throw std::runtime_error("Could not create invalid-scene camera: " + CameraEntity.error().Message);
			PulseForge::CameraComponent Camera;
			Camera.IsPrimary = true;
			if (auto Result = CameraEntity->SetCamera(Camera); !Result)
				throw std::runtime_error("Could not set invalid-scene camera: " + Result.error().Message);
			auto MeshEntity = InvalidScene.CreateEntity("GPU validation missing mesh");
			if (!MeshEntity) throw std::runtime_error("Could not create invalid-scene mesh entity: " + MeshEntity.error().Message);
			const PulseForge::MeshRendererComponent MissingMesh{ PulseForge::AssetID{ 0x13579bdf2468ace0ull, 0x1029384756abcdefull }, {} };
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
		PulseForge::GpuValidation::EditorUiUnormValidation m_EditorUiValidation;
		std::optional<PulseForge::Project> m_Project;
		PulseForge::Scene m_Scene;
		std::filesystem::path m_PointShadowImageDirectory;
		std::unique_ptr<PulseForge::Window> m_RetirementWindow;
		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		std::unique_ptr<PulseForge::GpuValidation::GpuSceneOutputValidation> m_SceneOutputValidation;
		PulseForge::GpuValidation::PointShadowImageValidation m_PointShadowImageValidation;
		std::unique_ptr<PulseForge::GpuValidation::GpuLightingValidation> m_LightingValidation;
		std::unique_ptr<PulseForge::GpuValidation::GpuProjectSceneValidation> m_ProjectSceneValidation;
		std::chrono::steady_clock::time_point m_StartedAt{};
		uint32_t m_CompletedFrames = 0;
		uint32_t m_RetirementValidationFrames = 0;
		uint32_t m_RetirementNativeSubmissions = 0;
		uint32_t m_RetirementMainFrames = 0;
		uint64_t m_RetirementTargetID = 0;
		bool m_Succeeded = true;
		bool m_SawMainFrameBeforeRecoveryFailure = false;
		bool m_MainFrameResumedAfterRecoveryFailure = false;
		bool m_LegacyRetirementValidationComplete = false;
		bool m_ValidateSceneHdr = false;
		bool m_ValidateToneMapping = false;
		bool m_ValidateEditorUiUnorm = false;
		bool m_ValidateLocalLights = false;
		bool m_ValidateSpotlightShadows = false;
		bool m_ValidatePointShadows = false;
		bool m_ValidatePbrMaterials = false;
		bool m_ValidateTransparency = false;
		bool m_ValidatePointShadowImages = false;
		LegacyRetirementScenario m_LegacyRetirementScenario = LegacyRetirementScenario::None;
	};

	class GpuValidationApplication final : public PulseForge::Application
	{
	public:
		GpuValidationApplication(bool ValidateSceneHdr, bool ValidateToneMapping, bool ValidateEditorUiUnorm, bool ValidateLocalLights,
			bool ValidateSpotlightShadows, bool ValidatePointShadows, bool ValidatePbrMaterials, bool ValidateTransparency,
			bool ValidatePointShadowImages, std::filesystem::path PointShadowImageDirectory, LegacyRetirementScenario RetirementScenario)
			: Application(PulseForge::RendererAPI::Vulkan)
		{
			auto Layer = std::make_unique<GpuValidationLayer>(ValidateSceneHdr, ValidateToneMapping, ValidateEditorUiUnorm,
				ValidateLocalLights, ValidateSpotlightShadows, ValidatePointShadows, ValidatePbrMaterials, ValidateTransparency,
				ValidatePointShadowImages, std::move(PointShadowImageDirectory), RetirementScenario);
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
} // namespace

int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	try
	{
		const std::string_view Mode = ArgumentCount > 1 ? std::string_view(Arguments[1]) : std::string_view{};
		if (Mode == "--active-frame-resource-upload") return PulseForge::GpuValidation::RunActiveFrameUploadValidation();
		const bool ValidateSceneHdr = Mode == "--scene-hdr";
		const bool ValidateToneMapping = Mode == "--scene-tonemap";
		const bool ValidateEditorUiUnorm = Mode == "--editor-ui-unorm";
		const bool ValidateLocalLights = Mode == "--scene-local-lights";
		const bool ValidateSpotlightShadows = Mode == "--scene-spot-shadows";
		const bool ValidatePointShadows = Mode == "--scene-point-shadows";
		const bool ValidatePbrMaterials = Mode == "--scene-pbr-materials";
		const bool ValidateTransparency = Mode == "--scene-transparency";
		const bool ValidatePointShadowImages = Mode == "--scene-point-shadow-images";
		const LegacyRetirementScenario RetirementScenario =
			Mode == "--legacy-main-retirement-cap" ? LegacyRetirementScenario::MainRecoveryFailure
			: Mode == "--legacy-native-retirement-cap" ? LegacyRetirementScenario::NativeRecoveryFailure
																	  : LegacyRetirementScenario::None;
		const std::filesystem::path PointShadowImageDirectory =
			ArgumentCount > 2 ? std::filesystem::path(Arguments[2])
							  : std::filesystem::current_path() / "Diagnostics" / "PointShadowDirection";
		GpuValidationApplication App(ValidateSceneHdr, ValidateToneMapping, ValidateEditorUiUnorm, ValidateLocalLights,
			ValidateSpotlightShadows, ValidatePointShadows, ValidatePbrMaterials, ValidateTransparency, ValidatePointShadowImages,
			PointShadowImageDirectory, RetirementScenario);
		return App.RunValidation();
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("GPU validation startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
