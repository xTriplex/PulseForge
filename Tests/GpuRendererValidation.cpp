#include "Core/Application.h"
#include "GpuValidation/PointShadowImageAnalysis.h"
#include "GpuValidation/PointShadowImageValidation.h"
#include "GpuValidation/TransparencyImageAnalysis.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "GpuValidation/ActiveFrameUploadValidation.h"
#include "GpuValidation/EditorUiUnormValidation.h"
#include "GpuValidation/ValidationShader.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/PointLightShadowMath.h"
#include "Scene/Scene.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"
#include "Scene/Components/DirectionalLightComponent.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../PulseForge/Vendor/stb/stb_image_write.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

namespace
{
	struct ValidationVertex
	{
		float Position[2];
		float TexCoord[2];
	};

	namespace PointShadowAnalysis = PulseForge::GpuValidation::PointShadowImageAnalysis;

	float PixelLuminance(const PulseForge::TextureReadback& Image, size_t PixelIndex)
	{
		const PointShadowAnalysis::ImageView ImageView{ Image.Width, Image.Height, std::span<const std::byte>(Image.Pixels) };
		return PointShadowAnalysis::PixelLuminance(ImageView, PixelIndex);
	}

	class GpuValidationLayer final : public PulseForge::Layer
	{
	public:
		GpuValidationLayer(bool ValidateSceneHdr, bool ValidateToneMapping, bool ValidateEditorUiUnorm, bool ValidateLocalLights,
			bool ValidateSpotlightShadows, bool ValidatePointShadows, bool ValidatePbrMaterials, bool ValidateTransparency,
			bool ValidatePointShadowImages, std::filesystem::path PointShadowImageDirectory)
			: Layer(ValidateTransparency		? "Transparency rendered-image validation"
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
			  m_ValidatePointShadowImages(ValidatePointShadowImages), m_PointShadowImageDirectory(std::move(PointShadowImageDirectory))
		{
		}

		void OnAttach() override
		{
			m_StartedAt = std::chrono::steady_clock::now();
			try
			{
				InitializeResources();
				if (m_ValidateEditorUiUnorm) m_EditorUiValidation.Initialize(PulseForge::Application::Get(), m_BindingLayout, *m_Sampler);
				if (m_ValidateSceneHdr || m_ValidateToneMapping || m_ValidateLocalLights || m_ValidateSpotlightShadows ||
					m_ValidatePointShadows || m_ValidatePbrMaterials || m_ValidateTransparency || m_ValidatePointShadowImages)
					InitializeSceneResources();
				if (m_ValidateLocalLights)
					InitializeLocalLightScenario();
				if (m_ValidateSpotlightShadows)
					InitializeSpotlightShadowScenario();
				if (m_ValidatePointShadows)
					InitializePointShadowScenario();
				if (m_ValidatePbrMaterials)
					InitializePbrMaterialScenario();
				if (m_ValidateTransparency)
					InitializeTransparencyScenario();
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
			if (m_ValidateEditorUiUnorm)
			{
				if (m_EditorUiValidation.CompletedFrames() >= 3)
				{
					if (m_EditorUiValidation.TargetCreations() != 2)
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
			if (m_ValidateSpotlightShadows)
			{
				if (m_SpotShadowFrames >= 9)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; spotlight slot ownership was deterministically reassigned through repeated oversubscribed Cast Shadows toggles, with directional-light absence and target resize coverage");
					PulseForge::Application::Get().RequestClose();
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(45))
				{
					Fail("Timed out preparing or rendering spotlight-shadow scene");
					return;
				}
				const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
				if (Width == 0 || Height == 0)
					return;
				if (m_ScenePrepared)
					return;
				UpdateSpotlightShadowScenario();
				const auto Prepared = m_SceneRenderer->PrepareScene(
					m_SpotlightShadowScene, static_cast<float>(Width) / static_cast<float>(Height));
				if (!Prepared)
				{
					Fail("Could not prepare spotlight-shadow scene: " + Prepared.error().Message);
					return;
				}
				if (m_SceneRenderer->IsEnvironmentLightingPending())
					return;
				m_ScenePrepared = true;
				return;
			}
			if (m_ValidatePointShadows)
			{
				if (m_PointShadowFrames >= 9)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; point-shadow cube faces rendered, persistent slots reassigned through live Cast Shadows changes, and point-only shadows rendered without directional CSM");
					PulseForge::Application::Get().RequestClose();
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
				{
					Fail("Timed out preparing or rendering point-shadow scene");
					return;
				}
				const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
				if (Width == 0 || Height == 0 || m_ScenePrepared)
					return;
				if (!m_PointShadowScenarioUpdated)
				{
					UpdatePointShadowScenario();
					m_PointShadowScenarioUpdated = true;
				}
				const auto Prepared = m_SceneRenderer->PrepareScene(
					m_PointShadowScene, static_cast<float>(Width) / static_cast<float>(Height));
				if (!Prepared)
				{
					Fail("Could not prepare point-shadow scene: " + Prepared.error().Message);
					return;
				}
				if (m_SceneRenderer->IsEnvironmentLightingPending())
					return;
				m_ScenePrepared = true;
				return;
			}
			if (m_ValidatePbrMaterials)
			{
				if (m_PbrMaterialCapturePending)
				{
					CapturePbrMaterialImage();
					m_PbrMaterialCapturePending = false;
					if (!m_Succeeded)
						return;
				}
				if (m_PbrMaterialCaptureComplete)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; PBR project loaded, all seven indexed mesh instances rendered, and actual sRGB output was captured; image: {0}",
						m_PbrMaterialImagePath.string());
					PulseForge::Application::Get().RequestClose();
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
				{
					Fail("Timed out preparing or capturing the PBR material demonstration scene");
					return;
				}
				const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
				if (Width == 0 || Height == 0 || m_ScenePrepared)
					return;
				const auto Prepared = m_SceneRenderer->PrepareScene(
					m_Scene,
					static_cast<float>(m_PbrMaterialOutputTarget->GetDescription().Width) /
						static_cast<float>(m_PbrMaterialOutputTarget->GetDescription().Height));
				if (!Prepared)
				{
					Fail("Could not prepare the PBR project startup scene: " + Prepared.error().Message);
					return;
				}
				if (m_SceneRenderer->IsEnvironmentLightingPending())
					return;
				m_ScenePrepared = true;
				return;
			}
			if (m_ValidateTransparency)
			{
				if (m_TransparencyCapturePending)
				{
					CaptureTransparencyImage();
					m_TransparencyCapturePending = false;
					if (!m_Succeeded)
						return;
					if (m_TransparencyCaptureStage < 2)
					{
						const auto Spot = m_TransparencySpotLight.GetSpotLight();
						if (!Spot || !Spot->has_value())
						{
							Fail("Transparency image comparison lost its spotlight component");
							return;
						}
						PulseForge::SpotLightComponent Updated = Spot->value();
						if (m_TransparencyCaptureStage == 0)
						{
							Updated.Intensity = m_TransparencySpotIntensity;
							Updated.CastShadows = false;
						}
						else
							Updated.CastShadows = true;
						if (!m_TransparencySpotLight.SetSpotLight(Updated))
						{
							Fail("Could not update the spotlight for alpha-mask image comparison");
							return;
						}
						PF_INFO("TRANSPARENCY_SPOTLIGHT_UPDATE next_stage={0}; intensity={1:.1f}; cast_shadows={2}",
							m_TransparencyCaptureStage + 1, Updated.Intensity, Updated.CastShadows);
						++m_TransparencyCaptureStage;
						m_ScenePrepared = false;
						return;
					}
					if (m_TransparencyCaptureStage == 2)
					{
						if (!ValidateTransparencyLightDifference() || !ValidateTransparencyShadowDifference() ||
							!SetTransparencySpotShadows(false) || !CreateTransparencyPointLight())
							return;
						m_TransparencyCaptureStage = 3;
					}
					else if (m_TransparencyCaptureStage == 3)
					{
						if (!SetTransparencyPointShadows(true))
							return;
						m_TransparencyCaptureStage = 4;
					}
					else if (m_TransparencyCaptureStage == 4)
					{
						if (!ValidateTransparencyPointShadowDifference() || !PrepareTransparencyDirectionalShadowTest(false))
							return;
						m_TransparencyCaptureStage = 5;
					}
					else if (m_TransparencyCaptureStage == 5)
					{
						if (!PrepareTransparencyDirectionalShadowTest(true))
							return;
						m_TransparencyCaptureStage = 6;
					}
					else if (m_TransparencyCaptureStage == 6)
					{
						if (!ValidateTransparencyDirectionalShadowDifference() || !SetTransparencyBluePanelOpaqueControl())
							return;
						m_TransparencyCaptureStage = 7;
					}
					else
					{
						if (!ValidateTransparencyBlendDifference())
							return;
						m_TransparencyCaptureComplete = true;
					}
					m_ScenePrepared = false;
					return;
				}
				if (m_TransparencyCaptureComplete)
				{
					PF_INFO("GPU_VALIDATION_RESULT=PASS; Transparency project rendered actual sRGB comparisons for masked spotlight, point-light, and directional shadows, plus HDR blending; startup image: {0}",
						m_TransparencyImagePath.string());
					PulseForge::Application::Get().RequestClose();
					return;
				}
				if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
				{
					Fail("Timed out preparing or capturing the transparency demonstration scene");
					return;
				}
				const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
				if (Width == 0 || Height == 0 || m_ScenePrepared)
					return;
				const auto Prepared = m_SceneRenderer->PrepareScene(
					m_Scene,
					static_cast<float>(m_TransparencyOutputTarget->GetDescription().Width) /
						static_cast<float>(m_TransparencyOutputTarget->GetDescription().Height));
				if (!Prepared)
				{
					Fail("Could not prepare the transparency project startup scene: " + Prepared.error().Message);
					return;
				}
				if (m_SceneRenderer->IsEnvironmentLightingPending())
					return;
				const auto Snapshot = PulseForge::SceneRenderSnapshotBuilder::Build(
					m_Scene, static_cast<float>(m_TransparencyOutputTarget->GetDescription().Width) /
						static_cast<float>(m_TransparencyOutputTarget->GetDescription().Height));
				if (!Snapshot)
				{
					Fail("Could not inspect prepared transparency light values: " + Snapshot.error().Message);
					return;
				}
				const auto Spot = std::find_if(Snapshot->SpotLights.begin(), Snapshot->SpotLights.end(),
					[this](const PulseForge::SceneSpotLight& Light) { return Light.Entity == m_TransparencySpotLight.GetUUID(); });
				if (Spot == Snapshot->SpotLights.end())
				{
					Fail("Transparency spotlight disappeared from the prepared scene snapshot");
					return;
				}
				PF_INFO("TRANSPARENCY_PREPARED_LIGHT stage={0}; intensity={1:.1f}; cast_shadows={2}; direction=({3:.3f},{4:.3f},{5:.3f})",
					m_TransparencyCaptureStage, Spot->Intensity, Spot->CastShadows,
					Spot->WorldDirection.x, Spot->WorldDirection.y, Spot->WorldDirection.z);
				m_ScenePrepared = true;
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
				if (const auto Result = m_EditorUiValidation.RenderFrame(PulseForge::Application::Get()); !Result) Fail(Result.error());
				return;
			}
			if (m_ValidateLocalLights)
			{
				RenderLocalLightFrame();
				return;
			}
			if (m_ValidateSpotlightShadows)
			{
				RenderSpotlightShadowFrame();
				return;
			}
			if (m_ValidatePointShadows)
			{
				RenderPointShadowFrame();
				return;
			}
			if (m_ValidatePbrMaterials)
			{
				RenderPbrMaterialFrame();
				return;
			}
			if (m_ValidateTransparency)
			{
				RenderTransparencyFrame();
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

		void OnDetach() override
		{
			const auto ReleasedPointShadowImages = m_PointShadowImageValidation.Release();
			if (!ReleasedPointShadowImages) Fail(ReleasedPointShadowImages.error());
			m_PointShadowOutputTarget.reset();
			m_PbrMaterialOutputTarget.reset();
			m_TransparencyOutputTarget.reset();
			m_SpotlightShadowOutputTarget.reset();
			m_EditorUiValidation.Reset();
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
			return m_Succeeded && (m_ValidateEditorUiUnorm			? m_EditorUiValidation.IsComplete()
									  : m_ValidateTransparency		? m_TransparencyCaptureComplete
									  : m_ValidatePbrMaterials		? m_PbrMaterialCaptureComplete
									  : m_ValidatePointShadowImages ? m_PointShadowImageValidation.IsComplete()
									  : m_ValidatePointShadows		? m_PointShadowFrames == 9
									  : m_ValidateSpotlightShadows	? m_SpotShadowFrames == 9
									  : m_ValidateLocalLights		? m_LocalLightFrames == 6
									  : m_ValidateToneMapping		? m_ToneMappingFrame == 10
									  : m_ValidateSceneHdr			? m_SceneCompletedFrames == 4 && m_ScenarioIndex == 4
																	: m_CompletedFrames == 3);
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
			const std::filesystem::path ProjectFile = m_ValidateTransparency
				? std::filesystem::path(PF_TRANSPARENCY_DEMO_PROJECT_FILE)
				: m_ValidatePbrMaterials
					? std::filesystem::path(PF_PBR_DEMO_PROJECT_FILE)
				: m_ValidatePointShadows || m_ValidatePointShadowImages
					? std::filesystem::path(PF_POINT_SHADOW_DEMO_PROJECT_FILE)
					: std::filesystem::path(PF_GPU_VALIDATION_PROJECT_FILE);
			auto OpenedProject = PulseForge::Project::Open(ProjectFile);
			if (!OpenedProject)
				throw std::runtime_error("Could not open GPU-validation project '" + ProjectFile.string() + "': " +
					OpenedProject.error().Message);
			m_Project.emplace(std::move(*OpenedProject));
			if (!m_Project->GetDescription().StartScene)
				throw std::runtime_error("GPU-validation project has no start scene");
			const auto SceneLoad = PulseForge::SceneAssetService::Load(
				*m_Project->GetDescription().StartScene,
				m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(),
				m_Scene);
			if (!SceneLoad)
				throw std::runtime_error("Could not load GPU-validation project startup scene: " + SceneLoad.error().Message);

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

		void InitializeSpotlightShadowScenario()
		{
			BuildScenario(m_SpotlightShadowScene, true, true, true);
			for (size_t Index = 0; Index < 5; ++Index)
			{
				PulseForge::SpotLightComponent Spot;
				Spot.Color = Index % 2 == 0 ? glm::vec3(1.0f, 0.65f, 0.35f) : glm::vec3(0.35f, 0.65f, 1.0f);
				static constexpr std::array<float, 5> Intensities = { 500.0f, 100.0f, 80.0f, 60.0f, 1.0f };
				Spot.Intensity = Intensities[Index];
				Spot.Range = 30.0f;
				Spot.InnerConeAngleDegrees = 20.0f;
				Spot.OuterConeAngleDegrees = 35.0f;
				auto Light = m_SpotlightShadowScene.CreateEntity("GPU validation shadow spotlight");
				if (!Light || !Light->SetSpotLight(Spot))
					throw std::runtime_error("Could not create spotlight-shadow validation light");
				auto Transform = Light->GetTransform();
				if (!Transform)
					throw std::runtime_error("Could not read spotlight-shadow validation transform");
				Transform->Translation = { static_cast<float>(Index) * 2.0f - 4.0f, 5.0f, 3.0f };
				Transform->Rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
				if (!Light->SetTransform(*Transform))
					throw std::runtime_error("Could not position spotlight-shadow validation light");
				m_SpotlightShadowEntities.push_back(*Light);
			}

			PulseForge::RenderTargetDesc OutputDescription;
			OutputDescription.Width = 128;
			OutputDescription.Height = 72;
			OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			OutputDescription.DebugName = "Spotlight-shadow GPU validation SDR output";
			auto OutputTarget = PulseForge::Application::Get().CreateRenderTarget(OutputDescription);
			if (!OutputTarget)
				throw std::runtime_error("Could not create spotlight-shadow validation output: " + OutputTarget.error().Message);
			m_SpotlightShadowOutputTarget = std::move(*OutputTarget);
			PF_INFO("Prepared real SceneRenderer spotlight-shadow reassignment scenario: A-D outrank E; E must be evicted when A-D are all eligible; includes bundled geometry, IBL, directional CSM, and SSAO");
		}

		void InitializePointShadowScenario()
		{
			BuildScenario(m_PointShadowScene, true, true, true);
			static constexpr std::array<float, 3> Intensities = { 300.0f, 200.0f, 100.0f };
			for (size_t Index = 0; Index < Intensities.size(); ++Index)
			{
				PulseForge::PointLightComponent Point;
				Point.Color = Index == 0 ? glm::vec3(1.0f, 0.72f, 0.45f) :
					Index == 1 ? glm::vec3(0.45f, 0.72f, 1.0f) : glm::vec3(0.65f, 1.0f, 0.5f);
				Point.Intensity = Intensities[Index];
				Point.Range = 30.0f;
				Point.CastShadows = true;
				auto Light = m_PointShadowScene.CreateEntity("GPU validation shadow point light");
				if (!Light || !Light->SetPointLight(Point))
					throw std::runtime_error("Could not create point-shadow validation light");
				auto Transform = Light->GetTransform();
				if (!Transform)
					throw std::runtime_error("Could not read point-shadow validation transform");
				Transform->Translation = { static_cast<float>(Index) * 2.0f - 2.0f, 2.0f, -1.0f };
				if (!Light->SetTransform(*Transform))
					throw std::runtime_error("Could not position point-shadow validation light");
				if (Index == 0)
				{
					PulseForge::SpotLightComponent Spot;
					Spot.Color = { 0.8f, 0.7f, 1.0f };
					Spot.Intensity = 1000.0f;
					Spot.Range = 30.0f;
					if (!Light->SetSpotLight(Spot))
						throw std::runtime_error("Could not add the co-located spotlight to the point-shadow validation light");
				}
				m_PointShadowEntities.push_back(*Light);
			}

			PulseForge::RenderTargetDesc OutputDescription;
			OutputDescription.Width = 128;
			OutputDescription.Height = 72;
			OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			OutputDescription.DebugName = "Point-shadow GPU validation SDR output";
			auto OutputTarget = PulseForge::Application::Get().CreateRenderTarget(OutputDescription);
			if (!OutputTarget)
				throw std::runtime_error("Could not create point-shadow validation output: " + OutputTarget.error().Message);
			m_PointShadowOutputTarget = std::move(*OutputTarget);
			PF_INFO("Prepared point-shadow GPU scene with three differently weighted shadow-requesting points, mixed with directional CSM, IBL, SSAO, and bundled indexed geometry");
		}

		void InitializePbrMaterialScenario()
		{
			PulseForge::RenderTargetDesc Description;
			Description.Width = 960;
			Description.Height = 540;
			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			Description.DebugName = "PBR material project validation image";
			auto Target = PulseForge::Application::Get().CreateRenderTarget(Description);
			if (!Target)
				throw std::runtime_error("Could not create PBR material validation output: " + Target.error().Message);
			m_PbrMaterialOutputTarget = std::move(*Target);
			m_PbrMaterialImagePath = m_PointShadowImageDirectory / "PBRMaterials" / "overview.png";
			std::filesystem::create_directories(m_PbrMaterialImagePath.parent_path());
			PF_INFO("PBR validation will capture the actual project-owned 960x540 sRGB scene output");
		}

		void InitializeTransparencyScenario()
		{
			PulseForge::RenderTargetDesc Description;
			Description.Width = 960;
			Description.Height = 540;
			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			Description.DebugName = "Transparency project validation image";
			auto Target = PulseForge::Application::Get().CreateRenderTarget(Description);
			if (!Target)
				throw std::runtime_error("Could not create transparency validation output: " + Target.error().Message);
			m_TransparencyOutputTarget = std::move(*Target);
			m_TransparencyImagePath = m_PointShadowImageDirectory / "Transparency" / "overview.png";
			std::filesystem::create_directories(m_TransparencyImagePath.parent_path());
			for (const PulseForge::Entity Entity : m_Scene.GetEntities())
			{
				const auto Tag = Entity.GetTag();
				if (Tag && Tag->Name == "Directional fill - no shadow test interference")
				{
					m_TransparencyDirectionalLight = Entity;
					const auto Directional = Entity.GetDirectionalLight();
					if (!Directional || !Directional->has_value() || Directional->value().CastShadows)
						throw std::runtime_error("Transparency demo's directional fill must not cast test shadows");
				}
				if (Tag && Tag->Name == "Key spot - masked shadow on backdrop")
				{
					m_TransparencySpotLight = Entity;
					const auto Spot = Entity.GetSpotLight();
					if (!Spot || !Spot->has_value())
						throw std::runtime_error("Transparency scene spotlight component could not be read");
					PulseForge::SpotLightComponent Disabled = Spot->value();
					Disabled.CastShadows = false;
					// Keep the authored key strong enough for an sRGB shadow/image comparison.
					m_TransparencySpotIntensity = Spot->value().Intensity;
					Disabled.Intensity = 0.0f;
					if (!Entity.SetSpotLight(Disabled))
						throw std::runtime_error("Could not disable the spotlight for the no-light baseline image");
				}
				if (Tag && Tag->Name == "Blue glass - transparent depth test")
					m_TransparencyBlueGlassPanel = Entity;
			}
			if (!m_TransparencyDirectionalLight || !m_TransparencySpotLight)
				throw std::runtime_error("Transparency scene is missing its shadow-isolated key and fill lights");
			if (!m_TransparencyBlueGlassPanel)
				throw std::runtime_error("Transparency scene is missing its blue glass blend comparison mesh");
			for (const PulseForge::AssetRecord& Record : m_Project->GetAssetRegistry().GetAssets())
				if (Record.ProjectRelativePath.generic_string() == "Assets/Materials/BlueOpaqueControl.material")
					m_TransparencyBlueOpaqueControlMaterial = Record.ID;
			if (m_TransparencyBlueOpaqueControlMaterial.IsNil())
				throw std::runtime_error("Transparency project is missing its matching opaque material control");
			PF_INFO("Transparency validation will capture the actual project-owned 960x540 sRGB scene output");
		}

		bool SetTransparencyBluePanelOpaqueControl()
		{
			const auto Mesh = m_TransparencyBlueGlassPanel.GetMeshRenderer();
			if (!Mesh || !*Mesh)
			{
				Fail("Could not read the blue panel mesh renderer for the blend material comparison");
				return false;
			}
			PulseForge::MeshRendererComponent OpaqueControl = **Mesh;
			OpaqueControl.MaterialAsset = m_TransparencyBlueOpaqueControlMaterial;
			if (!m_TransparencyBlueGlassPanel.SetMeshRenderer(OpaqueControl))
			{
				Fail("Could not assign the matching opaque material control for blend comparison");
				return false;
			}
			PF_INFO("TRANSPARENCY_BLEND_CONTROL=matching OPAQUE blue material assigned for pixel comparison");
			return true;
		}

		bool SetTransparencySpotShadows(bool Enabled)
		{
			const auto Spot = m_TransparencySpotLight.GetSpotLight();
			if (!Spot || !Spot->has_value())
			{
				Fail("Transparency spotlight component is unavailable while switching shadow tests");
				return false;
			}
			PulseForge::SpotLightComponent Updated = Spot->value();
			Updated.CastShadows = Enabled;
			if (!m_TransparencySpotLight.SetSpotLight(Updated))
			{
				Fail("Could not update transparency spotlight shadow state");
				return false;
			}
			return true;
		}

		bool CreateTransparencyPointLight()
		{
			if (m_TransparencyPointLight)
				return true;
			const auto SpotTransform = m_TransparencySpotLight.GetTransform();
			if (!SpotTransform)
			{
				Fail("Could not read the transparency light transform for point-shadow testing");
				return false;
			}
			auto PointEntity = m_Scene.CreateEntity("GPU alpha-mask point shadow light");
			if (!PointEntity)
			{
				Fail("Could not create a runtime point light for alpha-mask shadow validation");
				return false;
			}
			PulseForge::PointLightComponent Point;
			Point.Color = { 1.0f, 0.82f, 0.58f };
			Point.Intensity = m_TransparencySpotIntensity;
			Point.Range = 8.0f;
			Point.CastShadows = false;
			if (!PointEntity->SetPointLight(Point) || !PointEntity->SetTransform(*SpotTransform))
			{
				Fail("Could not configure the runtime point light for alpha-mask shadow validation");
				return false;
			}
			m_TransparencyPointLight = *PointEntity;
			return true;
		}

		bool SetTransparencyPointShadows(bool Enabled)
		{
			const auto Point = m_TransparencyPointLight.GetPointLight();
			if (!Point || !Point->has_value())
			{
				Fail("Transparency point-light component is unavailable while switching shadow tests");
				return false;
			}
			PulseForge::PointLightComponent Updated = Point->value();
			Updated.CastShadows = Enabled;
			if (!m_TransparencyPointLight.SetPointLight(Updated))
			{
				Fail("Could not update transparency point-light shadow state");
				return false;
			}
			return true;
		}

		bool PrepareTransparencyDirectionalShadowTest(bool Enabled)
		{
			if (m_TransparencyPointLight)
			{
				const auto Point = m_TransparencyPointLight.GetPointLight();
				if (!Point || !Point->has_value())
				{
					Fail("Transparency point-light component is unavailable for directional shadow isolation");
					return false;
				}
				PulseForge::PointLightComponent Disabled = Point->value();
				Disabled.CastShadows = false;
				Disabled.Intensity = 0.0f;
				if (!m_TransparencyPointLight.SetPointLight(Disabled))
				{
					Fail("Could not disable the point light for directional shadow validation");
					return false;
				}
			}
			const auto Directional = m_TransparencyDirectionalLight.GetDirectionalLight();
			if (!Directional || !Directional->has_value())
			{
				Fail("Transparency directional-light component is unavailable for masked shadow validation");
				return false;
			}
			PulseForge::DirectionalLightComponent Updated = Directional->value();
			Updated.Intensity = 4.0f;
			Updated.CastShadows = Enabled;
			Updated.ShadowDistance = 32.0f;
			Updated.ShadowBias = 0.0f;
			Updated.ShadowNormalBias = 0.01f;
			Updated.ShadowSoftness = 1.0f;
			if (!m_TransparencyDirectionalLight.SetDirectionalLight(Updated))
			{
				Fail("Could not configure the directional light for alpha-mask shadow validation");
				return false;
			}
			PF_INFO("TRANSPARENCY_DIRECTIONAL_SHADOW_UPDATE cast_shadows={0}; intensity=4.0", Enabled);
			return true;
		}

		void RenderPbrMaterialFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_PbrMaterialCaptureComplete)
				return;
			const auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_PbrMaterialOutputTarget);
			if (!Rendered || Rendered->GeometryDrawCount != 7)
			{
				Fail(Rendered ? "PBR material startup scene did not submit its seven indexed mesh instances" :
					"PBR material scene render failed: " + Rendered.error().Message);
				return;
			}
			m_PbrMaterialGeometryDrawCount = Rendered->GeometryDrawCount;
			const PulseForge::Texture* Color = m_PbrMaterialOutputTarget->GetColorTexture();
			if (!Color)
			{
				Fail("PBR material output target has no readable color texture");
				return;
			}
			m_PbrMaterialCapturePending = true;
		}

		void CapturePbrMaterialImage()
		{
			const PulseForge::Texture* Color = m_PbrMaterialOutputTarget->GetColorTexture();
			if (!Color)
			{
				Fail("PBR material output target has no readable color texture");
				return;
			}
			const auto Readback = PulseForge::Application::Get().ReadTextureForValidation(*Color);
			if (!Readback || Readback->Format != PulseForge::TextureFormat::RGBA8_Srgb)
			{
				Fail(Readback ? "PBR material output readback was not RGBA8 sRGB" :
					"Could not read actual PBR material output pixels: " + Readback.error().Message);
				return;
			}
			if (!stbi_write_png(m_PbrMaterialImagePath.string().c_str(), static_cast<int>(Readback->Width),
				static_cast<int>(Readback->Height), 4,
				reinterpret_cast<const uint8_t*>(Readback->Pixels.data()), static_cast<int>(Readback->Width * 4)))
			{
				Fail("Could not save actual PBR material scene pixels: " + m_PbrMaterialImagePath.string());
				return;
			}
			double LuminanceSum = 0.0;
			float MinimumLuminance = 1.0f;
			float MaximumLuminance = 0.0f;
			size_t LitPixelCount = 0;
			for (size_t Pixel = 0; Pixel < static_cast<size_t>(Readback->Width) * Readback->Height; ++Pixel)
			{
				const float Luminance = PixelLuminance(*Readback, Pixel);
				LuminanceSum += Luminance;
				MinimumLuminance = (std::min)(MinimumLuminance, Luminance);
				MaximumLuminance = (std::max)(MaximumLuminance, Luminance);
				if (Luminance > 0.1f)
					++LitPixelCount;
			}
			const double MeanLuminance = LuminanceSum /
				static_cast<double>(static_cast<size_t>(Readback->Width) * Readback->Height);
			PF_INFO("PBR_MATERIAL_IMAGE={0}; extent={1}x{2}; draws={3}; mean_luma={4:.4f}; min={5:.3f}; max={6:.3f}; lit_pixels={7}",
				m_PbrMaterialImagePath.string(), Readback->Width, Readback->Height, m_PbrMaterialGeometryDrawCount,
				MeanLuminance, MinimumLuminance, MaximumLuminance, LitPixelCount);
			if (MaximumLuminance < 0.55f || MaximumLuminance - MinimumLuminance < 0.2f || LitPixelCount < 1000)
			{
				Fail("Captured PBR material image lacks the expected visible HDR/material contrast");
				return;
			}
			m_PbrMaterialCaptureComplete = true;
		}

		void RenderTransparencyFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_TransparencyCaptureComplete || m_TransparencyCapturePending)
				return;
			const auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_TransparencyOutputTarget);
			if (!Rendered || Rendered->GeometryDrawCount != 10)
			{
				Fail(Rendered ? "Transparency startup scene did not submit its ten indexed mesh instances" :
					"Transparency scene render failed: " + Rendered.error().Message);
				return;
			}
			const size_t ExpectedSpotShadowPasses = m_TransparencyCaptureStage == 2 ? 1u : 0u;
			if (Rendered->SpotlightShadowPassCount != ExpectedSpotShadowPasses ||
				(m_TransparencyCaptureStage < 2 && Rendered->SpotlightShadowCasterDrawCount != 0) ||
				(m_TransparencyCaptureStage == 2 && Rendered->SpotlightShadowCasterDrawCount == 0))
			{
				Fail("Transparency capture did not submit the expected alpha-tested spotlight shadow pass and casters");
				return;
			}
			const size_t ExpectedPointShadowPasses = m_TransparencyCaptureStage == 4 ? 6u : 0u;
			if (Rendered->PointShadowPassCount != ExpectedPointShadowPasses ||
				(m_TransparencyCaptureStage == 4 && Rendered->PointShadowCasterDrawCount == 0))
			{
				Fail("Transparency capture did not submit all six alpha-tested point-shadow faces and their casters");
				return;
			}
			PF_INFO("TRANSPARENCY_RENDER_STAGE={0}; spot_shadow_passes={1}; spot_shadow_caster_draws={2}; point_shadow_passes={3}; point_shadow_caster_draws={4}; selected_shadow_owners={5}",
				m_TransparencyCaptureStage, Rendered->SpotlightShadowPassCount,
				Rendered->SpotlightShadowCasterDrawCount,
				Rendered->PointShadowPassCount, Rendered->PointShadowCasterDrawCount,
				static_cast<size_t>(std::count_if(Rendered->SpotlightShadowSlotOwners.begin(),
					Rendered->SpotlightShadowSlotOwners.end(), [](const auto& Owner) { return Owner.has_value(); })));
			m_TransparencyGeometryDrawCount = Rendered->GeometryDrawCount;
			m_TransparencyCapturePending = true;
		}

		void CaptureTransparencyImage()
		{
			const PulseForge::Texture* Color = m_TransparencyOutputTarget->GetColorTexture();
			if (!Color)
			{
				Fail("Transparency output target has no readable color texture");
				return;
			}
			const auto Readback = PulseForge::Application::Get().ReadTextureForValidation(*Color);
			if (!Readback || Readback->Format != PulseForge::TextureFormat::RGBA8_Srgb)
			{
				Fail(Readback ? "Transparency output readback was not RGBA8 sRGB" :
					"Could not read actual transparency output pixels: " + Readback.error().Message);
				return;
			}
			static constexpr std::array<const char*, 8> ComparisonNames = {
				"spot_disabled.png", "spot_shadows_off.png", "spot_shadows_on.png",
				"point_shadows_off.png", "point_shadows_on.png",
				"directional_shadows_off.png", "directional_shadows_on.png", "opaque_control.png"
			};
			if (m_TransparencyCaptureStage >= ComparisonNames.size())
			{
				Fail("Transparency capture reached an invalid shadow test stage");
				return;
			}
			const std::filesystem::path ComparisonPath = m_PointShadowImageDirectory / "Transparency" /
				ComparisonNames[m_TransparencyCaptureStage];
			const auto SaveImage = [&](const std::filesystem::path& Path)
			{
				return stbi_write_png(Path.string().c_str(), static_cast<int>(Readback->Width),
					static_cast<int>(Readback->Height), 4,
					reinterpret_cast<const uint8_t*>(Readback->Pixels.data()), static_cast<int>(Readback->Width * 4)) != 0;
			};
			if (!SaveImage(ComparisonPath) || (m_TransparencyCaptureStage == 2 && !SaveImage(m_TransparencyImagePath)))
			{
				Fail("Could not save actual transparency scene pixels: " + ComparisonPath.string());
				return;
			}
			switch (m_TransparencyCaptureStage)
			{
				case 0: m_TransparencyNoSpotPixels = Readback->Pixels; break;
				case 1: m_TransparencyShadowOffPixels = Readback->Pixels; break;
				case 2: m_TransparencyShadowOnPixels = Readback->Pixels; break;
				case 3: m_TransparencyPointShadowOffPixels = Readback->Pixels; break;
				case 4: m_TransparencyPointShadowOnPixels = Readback->Pixels; break;
				case 5: m_TransparencyDirectionalShadowOffPixels = Readback->Pixels; break;
				case 6: m_TransparencyDirectionalShadowOnPixels = Readback->Pixels; break;
				case 7: m_TransparencyOpaqueControlPixels = Readback->Pixels; break;
			}
			double LuminanceSum = 0.0;
			float MinimumLuminance = 1.0f;
			float MaximumLuminance = 0.0f;
			size_t LitPixelCount = 0;
			size_t DistinctColorPixels = 0;
			for (size_t Pixel = 0; Pixel < static_cast<size_t>(Readback->Width) * Readback->Height; ++Pixel)
			{
				const float Luminance = PixelLuminance(*Readback, Pixel);
				LuminanceSum += Luminance;
				MinimumLuminance = (std::min)(MinimumLuminance, Luminance);
				MaximumLuminance = (std::max)(MaximumLuminance, Luminance);
				if (Luminance > 0.1f)
					++LitPixelCount;
				const size_t Channel = Pixel * 4;
				const uint8_t Red = static_cast<uint8_t>(Readback->Pixels[Channel]);
				const uint8_t Green = static_cast<uint8_t>(Readback->Pixels[Channel + 1]);
				const uint8_t Blue = static_cast<uint8_t>(Readback->Pixels[Channel + 2]);
				if (static_cast<uint8_t>((std::max)({ Red, Green, Blue }) - (std::min)({ Red, Green, Blue })) > 28)
					++DistinctColorPixels;
			}
			const double MeanLuminance = LuminanceSum /
				static_cast<double>(static_cast<size_t>(Readback->Width) * Readback->Height);
			PF_INFO("TRANSPARENCY_IMAGE={0}; extent={1}x{2}; draws={3}; mean_luma={4:.4f}; range={5:.3f}-{6:.3f}; lit_pixels={7}; chromatic_pixels={8}",
				ComparisonPath.string(), Readback->Width, Readback->Height, m_TransparencyGeometryDrawCount,
				MeanLuminance, MinimumLuminance, MaximumLuminance, LitPixelCount, DistinctColorPixels);
			if (MaximumLuminance < 0.35f || MaximumLuminance - MinimumLuminance < 0.15f ||
				LitPixelCount < 1000 || DistinctColorPixels < 1000)
			{
				Fail("Captured transparency image lacks the expected visible geometry and color variation");
				return;
			}
		}

		bool ValidateTransparencyShadowDifference()
		{
			const auto Difference = PulseForge::GpuValidation::TransparencyImageAnalysis::CompareRgba8(
				{ 960, 540, m_TransparencyShadowOffPixels }, { 960, 540, m_TransparencyShadowOnPixels },
				PulseForge::GpuValidation::TransparencyImageAnalysis::Region{ 185, 95, 350, 282 }, true);
			if (!Difference)
			{
				Fail("Transparency shadow comparison failed: " + Difference.error());
				return false;
			}
			const std::filesystem::path DifferencePath = m_PointShadowImageDirectory / "Transparency" / "shadow_difference.png";
			if (!stbi_write_png(DifferencePath.string().c_str(), 960, 540, 4, Difference->DifferenceRgba.data(), 960 * 4))
			{
				Fail("Could not save actual transparency shadow-difference image");
				return false;
			}
			PF_INFO("TRANSPARENCY_SHADOW_IMAGE_DIFF=changed_pixels:{0}; mean_abs_rgb:{1:.3f}", Difference->ChangedPixels,
				Difference->MeanAbsoluteRgb);
			PF_INFO("TRANSPARENCY_MASKED_BACKDROP_DIFF=changed_pixels:{0}; roi=(185,95)-(350,282)", Difference->RegionChangedPixels);
			if (Difference->ChangedPixels < 150 || Difference->MeanAbsoluteRgb < 0.15)
			{
				Fail("Enabling alpha-tested spotlight shadows did not produce a measurable image change");
				return false;
			}
			if (Difference->RegionChangedPixels < 12)
			{
				Fail("Alpha-tested fence shadow did not change pixels in its projected backdrop region");
				return false;
			}
			return true;
		}

		bool ValidateMaskedShadowImageDifference(const std::vector<std::byte>& ShadowsOff, const std::vector<std::byte>& ShadowsOn,
			const char* ShadowType, uint32_t MinX, uint32_t MinY, uint32_t MaxX, uint32_t MaxY)
		{
			const auto Difference = PulseForge::GpuValidation::TransparencyImageAnalysis::CompareRgba8({ 960, 540, ShadowsOff },
				{ 960, 540, ShadowsOn }, PulseForge::GpuValidation::TransparencyImageAnalysis::Region{ MinX, MinY, MaxX, MaxY });
			if (!Difference)
			{
				Fail(std::string("Transparency ") + ShadowType + " comparison failed: " + Difference.error());
				return false;
			}
			PF_INFO("TRANSPARENCY_{0}_SHADOW_IMAGE_DIFF=changed_pixels:{1}; mean_abs_rgb:{2:.3f}; receiver_roi_pixels:{3}; "
					"roi=({4},{5})-({6},{7})",
				ShadowType, Difference->ChangedPixels, Difference->MeanAbsoluteRgb, Difference->RegionChangedPixels, MinX, MinY, MaxX,
				MaxY);
			if (Difference->ChangedPixels < 100 || Difference->MeanAbsoluteRgb < 0.1 || Difference->RegionChangedPixels < 12)
			{
				Fail(std::string("The alpha-masked backdrop did not show a measurable ") + ShadowType + " shadow difference");
				return false;
			}
			return true;
		}

		bool ValidateTransparencyPointShadowDifference()
		{
			return ValidateMaskedShadowImageDifference(
				m_TransparencyPointShadowOffPixels, m_TransparencyPointShadowOnPixels, "point-light", 180, 280, 370, 360);
		}

		bool ValidateTransparencyDirectionalShadowDifference()
		{
			return ValidateMaskedShadowImageDifference(
				m_TransparencyDirectionalShadowOffPixels, m_TransparencyDirectionalShadowOnPixels, "directional", 185, 260, 350, 360);
		}

		bool ValidateTransparencyBlendDifference()
		{
			const auto Difference = PulseForge::GpuValidation::TransparencyImageAnalysis::CompareRgba8(
				{ 960, 540, m_TransparencyDirectionalShadowOnPixels }, { 960, 540, m_TransparencyOpaqueControlPixels },
				PulseForge::GpuValidation::TransparencyImageAnalysis::Region{ 410, 190, 550, 325 });
			if (!Difference)
			{
				Fail("Transparency blend comparison failed: " + Difference.error());
				return false;
			}
			PF_INFO("TRANSPARENCY_BLEND_IMAGE_DIFF=changed_pixels:{0}; mean_abs_rgb:{1:.3f}; blue_panel_roi=(410,190)-(550,325)",
				Difference->RegionChangedPixels, Difference->RegionMeanAbsoluteRgb);
			if (Difference->RegionChangedPixels < 1000 || Difference->RegionMeanAbsoluteRgb < 1.0)
			{
				Fail("The BLEND blue panel did not differ from its matching OPAQUE control in the expected image region");
				return false;
			}
			return true;
		}

		bool ValidateTransparencyLightDifference()
		{
			const auto Difference = PulseForge::GpuValidation::TransparencyImageAnalysis::CompareRgba8(
				{ 960, 540, m_TransparencyNoSpotPixels }, { 960, 540, m_TransparencyShadowOffPixels });
			if (!Difference)
			{
				Fail("Transparency light comparison failed: " + Difference.error());
				return false;
			}
			PF_INFO("TRANSPARENCY_DIRECT_LIGHT_DIFF=changed_pixels:{0}; mean_abs_rgb:{1:.3f}; bounds=({2},{3})-({4},{5})",
				Difference->ChangedPixels, Difference->MeanAbsoluteRgb, Difference->MinX, Difference->MinY, Difference->MaxX,
				Difference->MaxY);
			if (Difference->ChangedPixels < 150 || Difference->MeanAbsoluteRgb < 0.15)
			{
				Fail("The configured transparency spotlight did not measurably illuminate the scene");
				return false;
			}
			return true;
		}
		void UpdatePointShadowScenario()
		{
			const size_t Frame = m_PointShadowFrames;
			auto DualSpot = m_PointShadowEntities.front().GetSpotLight();
			if (!DualSpot || !DualSpot->has_value())
				throw std::runtime_error("Could not read the co-located spotlight shadow state");
			DualSpot->value().CastShadows = Frame == 1 || Frame == 2 || Frame == 3 ||
				Frame == 5 || Frame == 6 || Frame == 7;
			DualSpot->value().Intensity = Frame == 2 || Frame == 3 || Frame == 6 ? 25.0f : 1000.0f;
			if (!m_PointShadowEntities.front().SetSpotLight(DualSpot->value()))
				throw std::runtime_error("Could not update the co-located spotlight shadow state");

			if (Frame == 0 || Frame == 8)
			{
				const std::array<size_t, 3> Lights = { 0, 1, 2 };
				for (size_t Index : Lights)
				{
					auto Point = m_PointShadowEntities[Index].GetPointLight();
					if (!Point || !Point->has_value())
						throw std::runtime_error("Could not read point-shadow toggle state");
					Point->value().CastShadows = false;
					if (!m_PointShadowEntities[Index].SetPointLight(Point->value()))
						throw std::runtime_error("Could not apply point-shadow toggle state");
				}
			}
			else if (Frame == 1)
			{
				for (size_t Index = 0; Index < m_PointShadowEntities.size(); ++Index)
				{
					auto Point = m_PointShadowEntities[Index].GetPointLight();
					if (!Point || !Point->has_value())
						throw std::runtime_error("Could not read point light before enabling the shadow budget");
					Point->value().CastShadows = true;
					if (!m_PointShadowEntities[Index].SetPointLight(Point->value()))
						throw std::runtime_error("Could not enable the point-shadow budget");
				}
			}
			else
			{
				const size_t Toggled = Frame == 4 || Frame == 5 ? 1 : 0;
				auto Point = m_PointShadowEntities[Toggled].GetPointLight();
				if (!Point || !Point->has_value())
					throw std::runtime_error("Could not read point light before shadow-slot reassignment");
				Point->value().CastShadows = Frame == 3 || Frame == 5 || Frame == 7;
				if (!m_PointShadowEntities[Toggled].SetPointLight(Point->value()))
					throw std::runtime_error("Could not apply point-shadow reassignment");
			}
			if (Frame == 3)
			{
				auto Transform = m_PointShadowEntities[0].GetTransform();
				if (!Transform)
					throw std::runtime_error("Could not read moving point-light transform");
				Transform->Translation.x += 0.25f;
				if (!m_PointShadowEntities[0].SetTransform(*Transform))
					throw std::runtime_error("Could not move point-shadow validation light");
			}
			if (Frame == 7)
			{
				for (const PulseForge::Entity Entity : m_PointShadowScene.GetEntities())
				{
					auto Directional = Entity.GetDirectionalLight();
					if (!Directional)
						throw std::runtime_error("Could not inspect directional light in point-shadow validation scene");
					if (*Directional && !Entity.RemoveDirectionalLight())
						throw std::runtime_error("Could not remove directional light for point-only shadow validation");
				}
				PulseForge::RenderTargetDesc Description;
				Description.Width = 96;
				Description.Height = 64;
				Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
				Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
				Description.DebugName = "Resized point-shadow GPU validation output";
				auto Target = PulseForge::Application::Get().CreateRenderTarget(Description);
				if (!Target)
					throw std::runtime_error("Could not resize point-shadow validation output: " + Target.error().Message);
				m_PointShadowOutputTarget = std::move(*Target);
			}
		}

		void RenderPointShadowFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_PointShadowFrames >= 9)
				return;
			auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_PointShadowOutputTarget);
			if (!Rendered || Rendered->GeometryDrawCount == 0)
			{
				Fail(Rendered ? "Point-shadow validation rendered no indexed PBR geometry" :
					"Point-shadow scene render failed: " + Rendered.error().Message);
				return;
			}
			static constexpr std::array<size_t, 9> ExpectedPasses = { 0, 12, 12, 12, 12, 12, 12, 12, 0 };
			const size_t Expected = ExpectedPasses[m_PointShadowFrames];
			const bool ExpectSpotShadow = m_PointShadowFrames == 1 || m_PointShadowFrames == 2 ||
				m_PointShadowFrames == 3 || m_PointShadowFrames == 5 || m_PointShadowFrames == 6 ||
				m_PointShadowFrames == 7;
			if (Rendered->PointShadowPassCount != Expected ||
				Rendered->PointShadowCasterDrawCount != Expected * Rendered->GeometryDrawCount ||
				Rendered->SpotlightShadowPassCount != static_cast<size_t>(ExpectSpotShadow) ||
				Rendered->SpotlightShadowCasterDrawCount !=
					static_cast<size_t>(ExpectSpotShadow) * Rendered->GeometryDrawCount)
			{
				Fail("Mixed point/spot shadow pass counts did not match the dual-component allocation");
				return;
			}
			if ((ExpectSpotShadow && Rendered->SpotlightShadowSlotOwners[0] !=
					std::optional<PulseForge::UUID>{ m_PointShadowEntities.front().GetUUID() }) ||
				(!ExpectSpotShadow && Rendered->SpotlightShadowSlotOwners[0]))
			{
				Fail("Spotlight shadow ownership did not resolve to the spotlight on the dual-component entity");
				return;
			}
			const uint64_t ExpectedGeneration = m_PointShadowFrames == 0 ? 0u : 1u;
			if (Rendered->PointShadowResourceGeneration != ExpectedGeneration)
			{
				Fail("Point-shadow cubemaps were allocated eagerly or recreated after ownership changes");
				return;
			}
			const auto OwnerIs = [&](size_t Slot, std::optional<size_t> LightIndex)
			{
				if (!LightIndex)
					return !Rendered->PointShadowSlotOwners[Slot];
				return Rendered->PointShadowSlotOwners[Slot] &&
					*Rendered->PointShadowSlotOwners[Slot] == m_PointShadowEntities[*LightIndex].GetUUID();
			};
			static constexpr std::array<std::array<int32_t, 2>, 9> ExpectedOwners = {{
				{{ -1, -1 }}, {{ 0, 1 }}, {{ 2, 1 }}, {{ 0, 1 }}, {{ 0, 2 }},
				{{ 0, 1 }}, {{ 2, 1 }}, {{ 0, 1 }}, {{ -1, -1 }}
			}};
			for (size_t Slot = 0; Slot < 2; ++Slot)
			{
				const int32_t Owner = ExpectedOwners[m_PointShadowFrames][Slot];
				if (!OwnerIs(Slot, Owner < 0 ? std::nullopt : std::optional<size_t>{ static_cast<size_t>(Owner) }))
				{
					Fail("Point-shadow cube slots did not match deterministic relevance selection after toggling");
					return;
				}
			}
			PF_INFO("POINT_SHADOW_FRAME={0}; face_passes={1}; caster_draws={2}; pbr_draws={3}; resource_generation={4}; owners=[{5},{6}]; hdr_generation={7}",
				m_PointShadowFrames, Rendered->PointShadowPassCount, Rendered->PointShadowCasterDrawCount,
				Rendered->GeometryDrawCount, Rendered->PointShadowResourceGeneration,
				Rendered->PointShadowSlotOwners[0] ? Rendered->PointShadowSlotOwners[0]->ToString() : "none",
				Rendered->PointShadowSlotOwners[1] ? Rendered->PointShadowSlotOwners[1]->ToString() : "none",
				Rendered->HdrTargetGeneration);
			++m_PointShadowFrames;
			m_ScenePrepared = false;
			m_PointShadowScenarioUpdated = false;
		}

		void UpdateSpotlightShadowScenario()
		{
			const size_t Frame = m_SpotShadowFrames;
			if (Frame == 0)
			{
				for (size_t Index = 0; Index < m_SpotlightShadowEntities.size(); ++Index)
				{
					auto Spot = m_SpotlightShadowEntities[Index].GetSpotLight();
					if (!Spot || !Spot->has_value())
						throw std::runtime_error("Could not initialize spotlight-shadow validation settings");
					Spot->value().CastShadows = false;
					if (!m_SpotlightShadowEntities[Index].SetSpotLight(Spot->value()))
						throw std::runtime_error("Could not initialize spotlight-shadow validation settings");
				}
			}
			else if (Frame >= 2 && Frame <= 7)
			{
				const size_t ToggledIndex = Frame == 2 || Frame == 3 ? 0 : Frame == 4 || Frame == 5 ? 1 : 2;
				auto Spot = m_SpotlightShadowEntities[ToggledIndex].GetSpotLight();
				if (!Spot || !Spot->has_value())
					throw std::runtime_error("Could not read spotlight before repeated shadow-slot reassignment");
				Spot->value().CastShadows = Frame % 2 != 0;
				if (!m_SpotlightShadowEntities[ToggledIndex].SetSpotLight(Spot->value()))
					throw std::runtime_error("Could not apply repeated Cast Shadows change");
				if (Frame == 2)
				{
					auto Transform = m_SpotlightShadowEntities[0].GetTransform();
					if (!Transform)
						throw std::runtime_error("Could not read moving spotlight-shadow transform");
					Transform->Translation.x += 0.5f;
					if (!m_SpotlightShadowEntities[0].SetTransform(*Transform))
						throw std::runtime_error("Could not move spotlight-shadow validation light");
				}
			}
			else if (Frame == 1)
			{
				for (size_t Index = 0; Index < m_SpotlightShadowEntities.size(); ++Index)
				{
					auto Spot = m_SpotlightShadowEntities[Index].GetSpotLight();
					if (!Spot || !Spot->has_value())
						throw std::runtime_error("Could not enable spotlight budget validation");
					Spot->value().CastShadows = true;
					if (!m_SpotlightShadowEntities[Index].SetSpotLight(Spot->value()))
						throw std::runtime_error("Could not enable spotlight budget validation");
				}
			}
			else if (Frame == 8)
			{
				for (const PulseForge::Entity Entity : m_SpotlightShadowScene.GetEntities())
				{
					auto Directional = Entity.GetDirectionalLight();
					if (!Directional)
						throw std::runtime_error("Could not inspect directional light in spotlight-shadow scenario");
					if (*Directional && !Entity.RemoveDirectionalLight())
						throw std::runtime_error("Could not remove directional light for spotlight-only shadow validation");
				}
				PulseForge::RenderTargetDesc OutputDescription;
				OutputDescription.Width = 96;
				OutputDescription.Height = 64;
				OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
				OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
				OutputDescription.DebugName = "Resized spotlight-shadow GPU validation output";
				auto OutputTarget = PulseForge::Application::Get().CreateRenderTarget(OutputDescription);
				if (!OutputTarget)
					throw std::runtime_error("Could not resize spotlight-shadow validation output: " + OutputTarget.error().Message);
				m_SpotlightShadowOutputTarget = std::move(*OutputTarget);
			}
		}

		void RenderSpotlightShadowFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_SpotShadowFrames >= 9)
				return;
			auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_SpotlightShadowOutputTarget);
			if (!Rendered || Rendered->GeometryDrawCount == 0)
			{
				Fail(Rendered ? "Spotlight-shadow validation rendered no PBR geometry" :
					"Spotlight-shadow scene render failed: " + Rendered.error().Message);
				return;
			}

			static constexpr std::array<size_t, 9> ExpectedShadowPasses = { 0, 4, 4, 4, 4, 4, 4, 4, 4 };
			const size_t ExpectedPasses = ExpectedShadowPasses[m_SpotShadowFrames];
			if (Rendered->SpotlightShadowPassCount != ExpectedPasses ||
				Rendered->SpotlightShadowCasterDrawCount != ExpectedPasses * Rendered->GeometryDrawCount)
			{
				Fail("Spotlight shadow-pass or depth-only caster draw count did not match the scenario");
				return;
			}
			if ((m_SpotShadowFrames == 0 && Rendered->SpotlightShadowResourceGeneration != 0) ||
				(m_SpotShadowFrames > 0 && Rendered->SpotlightShadowResourceGeneration != 1))
			{
				Fail("Spotlight shadow targets were allocated eagerly or recreated during the scenario");
				return;
			}
			const auto HasOwner = [&](size_t Index)
			{
				return std::ranges::find(Rendered->SpotlightShadowSlotOwners, m_SpotlightShadowEntities[Index].GetUUID()) !=
					Rendered->SpotlightShadowSlotOwners.end();
			};
			const auto HasExpectedOwners = [&](std::initializer_list<size_t> Expected)
			{
				for (size_t Index = 0; Index < m_SpotlightShadowEntities.size(); ++Index)
					if (HasOwner(Index) != (std::ranges::find(Expected, Index) != Expected.end()))
						return false;
				return true;
			};
			const auto HasExpectedSlotOrder = [&](const std::array<int32_t, 4>& Expected)
			{
				for (size_t Slot = 0; Slot < Expected.size(); ++Slot)
				{
					if (Expected[Slot] < 0)
					{
						if (Rendered->SpotlightShadowSlotOwners[Slot])
							return false;
					}
					else if (!Rendered->SpotlightShadowSlotOwners[Slot] ||
						*Rendered->SpotlightShadowSlotOwners[Slot] != m_SpotlightShadowEntities[static_cast<size_t>(Expected[Slot])].GetUUID())
						return false;
				}
				return true;
			};
			if ((m_SpotShadowFrames == 0 && !HasExpectedOwners({})) ||
				(m_SpotShadowFrames == 1 && !HasExpectedOwners({ 0, 1, 2, 3 })) ||
				(m_SpotShadowFrames == 2 && !HasExpectedOwners({ 1, 2, 3, 4 })) ||
				(m_SpotShadowFrames == 3 && !HasExpectedOwners({ 0, 1, 2, 3 })) ||
				(m_SpotShadowFrames == 4 && !HasExpectedOwners({ 0, 2, 3, 4 })) ||
				(m_SpotShadowFrames == 5 && !HasExpectedOwners({ 0, 1, 2, 3 })) ||
				(m_SpotShadowFrames == 6 && !HasExpectedOwners({ 0, 1, 3, 4 })) ||
				(m_SpotShadowFrames == 7 && !HasExpectedOwners({ 0, 1, 2, 3 })) ||
				(m_SpotShadowFrames == 8 && !HasExpectedOwners({ 0, 1, 2, 3 })))
			{
				Fail("Spotlight shadow slot owners did not match deterministic top-four selection after Cast Shadows changes");
				return;
			}
			static constexpr std::array<std::array<int32_t, 4>, 9> ExpectedSlotOwners = {{
				{{ -1, -1, -1, -1 }},
				{{ 0, 1, 2, 3 }},
				{{ 4, 1, 2, 3 }},
				{{ 0, 1, 2, 3 }},
				{{ 0, 4, 2, 3 }},
				{{ 0, 1, 2, 3 }},
				{{ 0, 1, 4, 3 }},
				{{ 0, 1, 2, 3 }},
				{{ 0, 1, 2, 3 }}
			}};
			if (!HasExpectedSlotOrder(ExpectedSlotOwners[m_SpotShadowFrames]))
			{
				Fail("Spotlight shadow maps were not rebound to their exact deterministic slot owners");
				return;
			}
			if (m_SpotShadowFrames == 0)
				m_FirstSpotShadowHdrGeneration = Rendered->HdrTargetGeneration;
			else if (m_SpotShadowFrames < 4 && Rendered->HdrTargetGeneration != m_FirstSpotShadowHdrGeneration)
			{
				Fail("Unchanged output extents did not reuse the HDR target during spotlight-shadow rendering");
				return;
			}
			else if (m_SpotShadowFrames == 8 && Rendered->HdrTargetGeneration == m_FirstSpotShadowHdrGeneration)
			{
				Fail("Resizing the spotlight-shadow output did not replace the HDR target");
				return;
			}
			PF_INFO("SPOT_SHADOW_FRAME={0}; shadow_passes={1}; caster_draws={2}; pbr_draws={3}; hdr_generation={4}; shadow_resource_generation={5}; slot_owners=[{6},{7},{8},{9}]",
				m_SpotShadowFrames, Rendered->SpotlightShadowPassCount, Rendered->SpotlightShadowCasterDrawCount,
				Rendered->GeometryDrawCount, Rendered->HdrTargetGeneration, Rendered->SpotlightShadowResourceGeneration,
				Rendered->SpotlightShadowSlotOwners[0] ? Rendered->SpotlightShadowSlotOwners[0]->ToString() : "none",
				Rendered->SpotlightShadowSlotOwners[1] ? Rendered->SpotlightShadowSlotOwners[1]->ToString() : "none",
				Rendered->SpotlightShadowSlotOwners[2] ? Rendered->SpotlightShadowSlotOwners[2]->ToString() : "none",
				Rendered->SpotlightShadowSlotOwners[3] ? Rendered->SpotlightShadowSlotOwners[3]->ToString() : "none");
			++m_SpotShadowFrames;
			m_ScenePrepared = false;
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
		PulseForge::GpuValidation::EditorUiUnormValidation m_EditorUiValidation;
		std::optional<PulseForge::Project> m_Project;
		PulseForge::Scene m_Scene;
		PulseForge::Scene m_EmptyScene;
		PulseForge::Scene m_EnvironmentOnlyScene;
		PulseForge::Scene m_GeometryOnlyScene;
		PulseForge::Scene m_DirectionalOnlyScene;
		PulseForge::Scene m_LocalLightsScene;
		PulseForge::Scene m_SpotlightShadowScene;
		PulseForge::Scene m_PointShadowScene;
		std::vector<PulseForge::Entity> m_LocalLightEntities;
		std::vector<PulseForge::Entity> m_SpotlightShadowEntities;
		std::vector<PulseForge::Entity> m_PointShadowEntities;
		PulseForge::RenderTargetHandle m_LocalLightOutputTarget;
		PulseForge::RenderTargetHandle m_SpotlightShadowOutputTarget;
		PulseForge::RenderTargetHandle m_PointShadowOutputTarget;
		PulseForge::RenderTargetHandle m_PbrMaterialOutputTarget;
		std::filesystem::path m_PointShadowImageDirectory;
		std::filesystem::path m_PbrMaterialImagePath;
		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		PulseForge::GpuValidation::PointShadowImageValidation m_PointShadowImageValidation;
		const PulseForge::Texture* m_FirstHdrTexture = nullptr;
		const PulseForge::Texture* m_ResizedHdrTexture = nullptr;
		const PulseForge::Texture* m_BoundHdrTexture = nullptr;
		const PulseForge::Texture* m_BoundOutputTexture = nullptr;
		std::chrono::steady_clock::time_point m_StartedAt{};
		uint32_t m_CompletedFrames = 0;
		uint32_t m_SceneCompletedFrames = 0;
		uint32_t m_LocalLightFrames = 0;
		uint32_t m_SpotShadowFrames = 0;
		uint32_t m_PointShadowFrames = 0;
		uint32_t m_ToneMappingFrame = 0;
		size_t m_ScenarioIndex = 0;
		uint64_t m_SwapchainHdrGeneration = 0;
		uint64_t m_SrgbHdrGeneration = 0;
		uint64_t m_ResizedHdrGeneration = 0;
		uint64_t m_FirstSpotShadowHdrGeneration = 0;
		const std::array<const char*, 4> ScenarioNames = { "empty", "environment-only", "geometry-only", "directional-only" };
		bool m_Succeeded = true;
		bool m_ValidateSceneHdr = false;
		bool m_ValidateToneMapping = false;
		bool m_ValidateEditorUiUnorm = false;
		bool m_ValidateLocalLights = false;
		bool m_ValidateSpotlightShadows = false;
		bool m_ValidatePointShadows = false;
		bool m_ValidatePbrMaterials = false;
		bool m_ValidateTransparency = false;
		bool m_ValidatePointShadowImages = false;
		bool m_ScenePrepared = false;
		bool m_PointShadowScenarioUpdated = false;
		bool m_PbrMaterialCapturePending = false;
		bool m_PbrMaterialCaptureComplete = false;
		size_t m_PbrMaterialGeometryDrawCount = 0;
		std::unique_ptr<PulseForge::RenderTarget> m_TransparencyOutputTarget;
		std::filesystem::path m_TransparencyImagePath;
		PulseForge::Entity m_TransparencyDirectionalLight;
		PulseForge::Entity m_TransparencySpotLight;
		PulseForge::Entity m_TransparencyPointLight;
		PulseForge::Entity m_TransparencyBlueGlassPanel;
		PulseForge::UUID m_TransparencyBlueOpaqueControlMaterial;
		std::vector<std::byte> m_TransparencyShadowOffPixels;
		std::vector<std::byte> m_TransparencyShadowOnPixels;
		std::vector<std::byte> m_TransparencyNoSpotPixels;
		std::vector<std::byte> m_TransparencyPointShadowOffPixels;
		std::vector<std::byte> m_TransparencyPointShadowOnPixels;
		std::vector<std::byte> m_TransparencyDirectionalShadowOffPixels;
		std::vector<std::byte> m_TransparencyDirectionalShadowOnPixels;
		std::vector<std::byte> m_TransparencyOpaqueControlPixels;
		float m_TransparencySpotIntensity = 0.0f;
		bool m_TransparencyCapturePending = false;
		bool m_TransparencyCaptureComplete = false;
		size_t m_TransparencyCaptureStage = 0;
		size_t m_TransparencyGeometryDrawCount = 0;
	};

	class GpuValidationApplication final : public PulseForge::Application
	{
	public:
		GpuValidationApplication(bool ValidateSceneHdr, bool ValidateToneMapping, bool ValidateEditorUiUnorm, bool ValidateLocalLights,
			bool ValidateSpotlightShadows, bool ValidatePointShadows, bool ValidatePbrMaterials, bool ValidateTransparency,
			bool ValidatePointShadowImages, std::filesystem::path PointShadowImageDirectory)
			: Application(PulseForge::RendererAPI::Vulkan)
		{
			auto Layer = std::make_unique<GpuValidationLayer>(ValidateSceneHdr, ValidateToneMapping, ValidateEditorUiUnorm,
				ValidateLocalLights, ValidateSpotlightShadows, ValidatePointShadows, ValidatePbrMaterials, ValidateTransparency,
				ValidatePointShadowImages, std::move(PointShadowImageDirectory));
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
		const std::filesystem::path PointShadowImageDirectory = ArgumentCount > 2
			? std::filesystem::path(Arguments[2])
			: std::filesystem::current_path() / "Diagnostics" / "PointShadowDirection";
		GpuValidationApplication App(ValidateSceneHdr, ValidateToneMapping, ValidateEditorUiUnorm, ValidateLocalLights,
			ValidateSpotlightShadows, ValidatePointShadows, ValidatePbrMaterials, ValidateTransparency, ValidatePointShadowImages,
			PointShadowImageDirectory);
		return App.RunValidation();
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("GPU validation startup/runtime failure: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
