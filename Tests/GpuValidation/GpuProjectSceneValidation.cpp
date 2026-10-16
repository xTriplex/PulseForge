#include "GpuValidation/GpuProjectSceneValidation.h"

#include "Assets/Project.h"
#include "Core/Application.h"
#include "Core/Log.h"
#include "Scene/SceneRenderSnapshot.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"
#include "Scene/Scene.h"
#include "GpuValidation/PointShadowImageAnalysis.h"
#include "GpuValidation/TransparencyImageAnalysis.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../PulseForge/Vendor/stb/stb_image_write.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace PulseForge::GpuValidation
{
	namespace PointShadowAnalysis = PulseForge::GpuValidation::PointShadowImageAnalysis;

	namespace
	{
		float PixelLuminance(const TextureReadback& Image, size_t PixelIndex)
		{
			const PointShadowAnalysis::ImageView ImageView{ Image.Width, Image.Height, std::span<const std::byte>(Image.Pixels) };
			return PointShadowAnalysis::PixelLuminance(ImageView, PixelIndex);
		}
	} // namespace

	GpuProjectSceneValidation::GpuProjectSceneValidation(Project& SourceProject, Scene& SourceScene, SceneRenderer& Renderer,
		ProjectSceneValidationMode Mode, std::filesystem::path OutputRoot, std::chrono::steady_clock::time_point StartedAt) noexcept
		: m_Project(SourceProject), m_Scene(SourceScene), m_SceneRenderer(Renderer), m_Mode(Mode), m_OutputRoot(std::move(OutputRoot)),
		  m_StartedAt(StartedAt)
	{
	}

	void GpuProjectSceneValidation::Initialize()
	{
		if (m_Mode == ProjectSceneValidationMode::PbrMaterials) InitializePbrMaterial();
		else InitializeTransparency();
	}

	void GpuProjectSceneValidation::Update()
	{
		if (m_Mode == ProjectSceneValidationMode::PbrMaterials) UpdatePbrMaterial();
		else UpdateTransparency();
	}

	void GpuProjectSceneValidation::Render()
	{
		if (m_Mode == ProjectSceneValidationMode::PbrMaterials) RenderPbrMaterial();
		else RenderTransparency();
	}

	void GpuProjectSceneValidation::UpdatePbrMaterial()
	{
		if (!m_Succeeded || m_PbrMaterialCaptureComplete) return;
		if (m_PbrMaterialCapturePending)
		{
			CapturePbrMaterialImage();
			m_PbrMaterialCapturePending = false;
			if (!m_Succeeded) return;
		}
		if (m_PbrMaterialCaptureComplete) return;
		if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
		{
			Fail("Timed out preparing or capturing the PBR material demonstration scene");
			return;
		}
		const auto [Width, Height] = Application::Get().GetWindow().GetFramebufferSize();
		if (Width == 0 || Height == 0 || m_ScenePrepared) return;
		const auto Prepared =
			m_SceneRenderer.PrepareScene(m_Scene, static_cast<float>(m_PbrMaterialOutputTarget->GetDescription().Width) /
													  static_cast<float>(m_PbrMaterialOutputTarget->GetDescription().Height));
		if (!Prepared)
		{
			Fail("Could not prepare the PBR project startup scene: " + Prepared.error().Message);
			return;
		}
		if (m_SceneRenderer.IsEnvironmentLightingPending()) return;
		m_ScenePrepared = true;
	}

	void GpuProjectSceneValidation::UpdateTransparency()
	{
		if (!m_Succeeded || m_TransparencyCaptureComplete) return;
		if (m_TransparencyCapturePending)
		{
			CaptureTransparencyImage();
			m_TransparencyCapturePending = false;
			if (!m_Succeeded) return;
			if (m_TransparencyCaptureStage < 2)
			{
				const auto Spot = m_TransparencySpotLight.GetSpotLight();
				if (!Spot || !Spot->has_value())
				{
					Fail("Transparency image comparison lost its spotlight component");
					return;
				}
				SpotLightComponent Updated = Spot->value();
				if (m_TransparencyCaptureStage == 0)
				{
					Updated.Intensity = m_TransparencySpotIntensity;
					Updated.CastShadows = false;
				}
				else Updated.CastShadows = true;
				if (!m_TransparencySpotLight.SetSpotLight(Updated))
				{
					Fail("Could not update the spotlight for alpha-mask image comparison");
					return;
				}
				PF_INFO("TRANSPARENCY_SPOTLIGHT_UPDATE next_stage={0}; intensity={1:.1f}; cast_shadows={2}", m_TransparencyCaptureStage + 1,
					Updated.Intensity, Updated.CastShadows);
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
				if (!SetTransparencyPointShadows(true)) return;
				m_TransparencyCaptureStage = 4;
			}
			else if (m_TransparencyCaptureStage == 4)
			{
				if (!ValidateTransparencyPointShadowDifference() || !PrepareTransparencyDirectionalShadowTest(false)) return;
				m_TransparencyCaptureStage = 5;
			}
			else if (m_TransparencyCaptureStage == 5)
			{
				if (!PrepareTransparencyDirectionalShadowTest(true)) return;
				m_TransparencyCaptureStage = 6;
			}
			else if (m_TransparencyCaptureStage == 6)
			{
				if (!ValidateTransparencyDirectionalShadowDifference() || !SetTransparencyBluePanelOpaqueControl()) return;
				m_TransparencyCaptureStage = 7;
			}
			else
			{
				if (!ValidateTransparencyBlendDifference()) return;
				m_TransparencyCaptureComplete = true;
			}
			m_ScenePrepared = false;
			return;
		}
		if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
		{
			Fail("Timed out preparing or capturing the transparency demonstration scene");
			return;
		}
		const auto [Width, Height] = Application::Get().GetWindow().GetFramebufferSize();
		if (Width == 0 || Height == 0 || m_ScenePrepared) return;
		const float Aspect = static_cast<float>(m_TransparencyOutputTarget->GetDescription().Width) /
							 static_cast<float>(m_TransparencyOutputTarget->GetDescription().Height);
		const auto Prepared = m_SceneRenderer.PrepareScene(m_Scene, Aspect);
		if (!Prepared)
		{
			Fail("Could not prepare the transparency project startup scene: " + Prepared.error().Message);
			return;
		}
		if (m_SceneRenderer.IsEnvironmentLightingPending()) return;
		const auto Snapshot = SceneRenderSnapshotBuilder::Build(m_Scene, Aspect);
		if (!Snapshot)
		{
			Fail("Could not inspect prepared transparency light values: " + Snapshot.error().Message);
			return;
		}
		const auto Spot = std::find_if(Snapshot->SpotLights.begin(), Snapshot->SpotLights.end(),
			[this](const SceneSpotLight& Light) { return Light.Entity == m_TransparencySpotLight.GetUUID(); });
		if (Spot == Snapshot->SpotLights.end())
		{
			Fail("Transparency spotlight disappeared from the prepared scene snapshot");
			return;
		}
		PF_INFO("TRANSPARENCY_PREPARED_LIGHT stage={0}; intensity={1:.1f}; cast_shadows={2}; direction=({3:.3f},{4:.3f},{5:.3f})",
			m_TransparencyCaptureStage, Spot->Intensity, Spot->CastShadows, Spot->WorldDirection.x, Spot->WorldDirection.y,
			Spot->WorldDirection.z);
		m_ScenePrepared = true;
	}

	void GpuProjectSceneValidation::Reset() noexcept
	{
		m_PbrMaterialOutputTarget.reset();
		m_TransparencyOutputTarget.reset();
		m_TransparencyDirectionalLight = {};
		m_TransparencySpotLight = {};
		m_TransparencyPointLight = {};
		m_TransparencyBlueGlassPanel = {};
		m_TransparencyBlueOpaqueControlMaterial = {};
		m_TransparencyShadowOffPixels.clear();
		m_TransparencyShadowOnPixels.clear();
		m_TransparencyNoSpotPixels.clear();
		m_TransparencyPointShadowOffPixels.clear();
		m_TransparencyPointShadowOnPixels.clear();
		m_TransparencyDirectionalShadowOffPixels.clear();
		m_TransparencyDirectionalShadowOnPixels.clear();
		m_TransparencyOpaqueControlPixels.clear();
	}

	bool GpuProjectSceneValidation::IsComplete() const noexcept
	{
		return m_Mode == ProjectSceneValidationMode::PbrMaterials ? m_PbrMaterialCaptureComplete : m_TransparencyCaptureComplete;
	}

	bool GpuProjectSceneValidation::Succeeded() const noexcept
	{
		return m_Succeeded && IsComplete();
	}

	std::optional<std::string> GpuProjectSceneValidation::TakeFailure()
	{
		return std::exchange(m_Failure, std::nullopt);
	}

	std::string_view GpuProjectSceneValidation::CompletionSummary() const noexcept
	{
		return m_Mode == ProjectSceneValidationMode::PbrMaterials
				   ? "PBR project loaded, all seven indexed mesh instances rendered, and actual sRGB output was captured"
				   : "Transparency project rendered actual sRGB comparisons for masked spotlight, point-light, and directional shadows, "
					 "plus HDR blending";
	}

	const std::filesystem::path& GpuProjectSceneValidation::CapturePath() const noexcept
	{
		return m_Mode == ProjectSceneValidationMode::PbrMaterials ? m_PbrMaterialImagePath : m_TransparencyImagePath;
	}

	void GpuProjectSceneValidation::Fail(std::string Message)
	{
		if (m_Succeeded) m_Failure = std::move(Message);
		m_Succeeded = false;
	}
	void GpuProjectSceneValidation::InitializePbrMaterial()
	{
		PulseForge::RenderTargetDesc Description;
		Description.Width = 960;
		Description.Height = 540;
		Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
		Description.DebugName = "PBR material project validation image";
		auto Target = PulseForge::Application::Get().CreateRenderTarget(Description);
		if (!Target) throw std::runtime_error("Could not create PBR material validation output: " + Target.error().Message);
		m_PbrMaterialOutputTarget = std::move(*Target);
		m_PbrMaterialImagePath = m_OutputRoot / "PBRMaterials" / "overview.png";
		std::filesystem::create_directories(m_PbrMaterialImagePath.parent_path());
		PF_INFO("PBR validation will capture the actual project-owned 960x540 sRGB scene output");
	}

	void GpuProjectSceneValidation::InitializeTransparency()
	{
		PulseForge::RenderTargetDesc Description;
		Description.Width = 960;
		Description.Height = 540;
		Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
		Description.DebugName = "Transparency project validation image";
		auto Target = PulseForge::Application::Get().CreateRenderTarget(Description);
		if (!Target) throw std::runtime_error("Could not create transparency validation output: " + Target.error().Message);
		m_TransparencyOutputTarget = std::move(*Target);
		m_TransparencyImagePath = m_OutputRoot / "Transparency" / "overview.png";
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
				if (!Spot || !Spot->has_value()) throw std::runtime_error("Transparency scene spotlight component could not be read");
				PulseForge::SpotLightComponent Disabled = Spot->value();
				Disabled.CastShadows = false;
				// Keep the authored key strong enough for an sRGB shadow/image comparison.
				m_TransparencySpotIntensity = Spot->value().Intensity;
				Disabled.Intensity = 0.0f;
				if (!Entity.SetSpotLight(Disabled))
					throw std::runtime_error("Could not disable the spotlight for the no-light baseline image");
			}
			if (Tag && Tag->Name == "Blue glass - transparent depth test") m_TransparencyBlueGlassPanel = Entity;
		}
		if (!m_TransparencyDirectionalLight || !m_TransparencySpotLight)
			throw std::runtime_error("Transparency scene is missing its shadow-isolated key and fill lights");
		if (!m_TransparencyBlueGlassPanel) throw std::runtime_error("Transparency scene is missing its blue glass blend comparison mesh");
		for (const PulseForge::AssetRecord& Record : m_Project.GetAssetRegistry().GetAssets())
			if (Record.ProjectRelativePath.generic_string() == "Assets/Materials/BlueOpaqueControl.material")
				m_TransparencyBlueOpaqueControlMaterial = Record.ID;
		if (m_TransparencyBlueOpaqueControlMaterial.IsNil())
			throw std::runtime_error("Transparency project is missing its matching opaque material control");
		PF_INFO("Transparency validation will capture the actual project-owned 960x540 sRGB scene output");
	}

	bool GpuProjectSceneValidation::SetTransparencyBluePanelOpaqueControl()
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

	bool GpuProjectSceneValidation::SetTransparencySpotShadows(bool Enabled)
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

	bool GpuProjectSceneValidation::CreateTransparencyPointLight()
	{
		if (m_TransparencyPointLight) return true;
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

	bool GpuProjectSceneValidation::SetTransparencyPointShadows(bool Enabled)
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

	bool GpuProjectSceneValidation::PrepareTransparencyDirectionalShadowTest(bool Enabled)
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

	void GpuProjectSceneValidation::RenderPbrMaterial()
	{
		if (!m_Succeeded || !m_ScenePrepared || m_PbrMaterialCaptureComplete) return;
		const auto Rendered = m_SceneRenderer.RenderPreparedSceneToOutput(*m_PbrMaterialOutputTarget);
		if (!Rendered || Rendered->GeometryDrawCount != 7)
		{
			Fail(Rendered ? "PBR material startup scene did not submit its seven indexed mesh instances"
						  : "PBR material scene render failed: " + Rendered.error().Message);
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

	void GpuProjectSceneValidation::CapturePbrMaterialImage()
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
			Fail(Readback ? "PBR material output readback was not RGBA8 sRGB"
						  : "Could not read actual PBR material output pixels: " + Readback.error().Message);
			return;
		}
		if (!stbi_write_png(m_PbrMaterialImagePath.string().c_str(), static_cast<int>(Readback->Width), static_cast<int>(Readback->Height),
				4, reinterpret_cast<const uint8_t*>(Readback->Pixels.data()), static_cast<int>(Readback->Width * 4)))
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
			if (Luminance > 0.1f) ++LitPixelCount;
		}
		const double MeanLuminance = LuminanceSum / static_cast<double>(static_cast<size_t>(Readback->Width) * Readback->Height);
		PF_INFO("PBR_MATERIAL_IMAGE={0}; extent={1}x{2}; draws={3}; mean_luma={4:.4f}; min={5:.3f}; max={6:.3f}; lit_pixels={7}",
			m_PbrMaterialImagePath.string(), Readback->Width, Readback->Height, m_PbrMaterialGeometryDrawCount, MeanLuminance,
			MinimumLuminance, MaximumLuminance, LitPixelCount);
		if (MaximumLuminance < 0.55f || MaximumLuminance - MinimumLuminance < 0.2f || LitPixelCount < 1000)
		{
			Fail("Captured PBR material image lacks the expected visible HDR/material contrast");
			return;
		}
		m_PbrMaterialCaptureComplete = true;
	}

	void GpuProjectSceneValidation::RenderTransparency()
	{
		if (!m_Succeeded || !m_ScenePrepared || m_TransparencyCaptureComplete || m_TransparencyCapturePending) return;
		const auto Rendered = m_SceneRenderer.RenderPreparedSceneToOutput(*m_TransparencyOutputTarget);
		if (!Rendered || Rendered->GeometryDrawCount != 10)
		{
			Fail(Rendered ? "Transparency startup scene did not submit its ten indexed mesh instances"
						  : "Transparency scene render failed: " + Rendered.error().Message);
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
		PF_INFO("TRANSPARENCY_RENDER_STAGE={0}; spot_shadow_passes={1}; spot_shadow_caster_draws={2}; point_shadow_passes={3}; "
				"point_shadow_caster_draws={4}; selected_shadow_owners={5}",
			m_TransparencyCaptureStage, Rendered->SpotlightShadowPassCount, Rendered->SpotlightShadowCasterDrawCount,
			Rendered->PointShadowPassCount, Rendered->PointShadowCasterDrawCount,
			static_cast<size_t>(std::count_if(Rendered->SpotlightShadowSlotOwners.begin(), Rendered->SpotlightShadowSlotOwners.end(),
				[](const auto& Owner) { return Owner.has_value(); })));
		m_TransparencyGeometryDrawCount = Rendered->GeometryDrawCount;
		m_TransparencyCapturePending = true;
	}

	void GpuProjectSceneValidation::CaptureTransparencyImage()
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
			Fail(Readback ? "Transparency output readback was not RGBA8 sRGB"
						  : "Could not read actual transparency output pixels: " + Readback.error().Message);
			return;
		}
		static constexpr std::array<const char*, 8> ComparisonNames = { "spot_disabled.png", "spot_shadows_off.png", "spot_shadows_on.png",
			"point_shadows_off.png", "point_shadows_on.png", "directional_shadows_off.png", "directional_shadows_on.png",
			"opaque_control.png" };
		if (m_TransparencyCaptureStage >= ComparisonNames.size())
		{
			Fail("Transparency capture reached an invalid shadow test stage");
			return;
		}
		const std::filesystem::path ComparisonPath = m_OutputRoot / "Transparency" / ComparisonNames[m_TransparencyCaptureStage];
		const auto SaveImage = [&](const std::filesystem::path& Path)
		{
			return stbi_write_png(Path.string().c_str(), static_cast<int>(Readback->Width), static_cast<int>(Readback->Height), 4,
					   reinterpret_cast<const uint8_t*>(Readback->Pixels.data()), static_cast<int>(Readback->Width * 4)) != 0;
		};
		if (!SaveImage(ComparisonPath) || (m_TransparencyCaptureStage == 2 && !SaveImage(m_TransparencyImagePath)))
		{
			Fail("Could not save actual transparency scene pixels: " + ComparisonPath.string());
			return;
		}
		switch (m_TransparencyCaptureStage)
		{
		case 0:
			m_TransparencyNoSpotPixels = Readback->Pixels;
			break;
		case 1:
			m_TransparencyShadowOffPixels = Readback->Pixels;
			break;
		case 2:
			m_TransparencyShadowOnPixels = Readback->Pixels;
			break;
		case 3:
			m_TransparencyPointShadowOffPixels = Readback->Pixels;
			break;
		case 4:
			m_TransparencyPointShadowOnPixels = Readback->Pixels;
			break;
		case 5:
			m_TransparencyDirectionalShadowOffPixels = Readback->Pixels;
			break;
		case 6:
			m_TransparencyDirectionalShadowOnPixels = Readback->Pixels;
			break;
		case 7:
			m_TransparencyOpaqueControlPixels = Readback->Pixels;
			break;
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
			if (Luminance > 0.1f) ++LitPixelCount;
			const size_t Channel = Pixel * 4;
			const uint8_t Red = static_cast<uint8_t>(Readback->Pixels[Channel]);
			const uint8_t Green = static_cast<uint8_t>(Readback->Pixels[Channel + 1]);
			const uint8_t Blue = static_cast<uint8_t>(Readback->Pixels[Channel + 2]);
			if (static_cast<uint8_t>((std::max)({ Red, Green, Blue }) - (std::min)({ Red, Green, Blue })) > 28) ++DistinctColorPixels;
		}
		const double MeanLuminance = LuminanceSum / static_cast<double>(static_cast<size_t>(Readback->Width) * Readback->Height);
		PF_INFO("TRANSPARENCY_IMAGE={0}; extent={1}x{2}; draws={3}; mean_luma={4:.4f}; range={5:.3f}-{6:.3f}; lit_pixels={7}; "
				"chromatic_pixels={8}",
			ComparisonPath.string(), Readback->Width, Readback->Height, m_TransparencyGeometryDrawCount, MeanLuminance, MinimumLuminance,
			MaximumLuminance, LitPixelCount, DistinctColorPixels);
		if (MaximumLuminance < 0.35f || MaximumLuminance - MinimumLuminance < 0.15f || LitPixelCount < 1000 || DistinctColorPixels < 1000)
		{
			Fail("Captured transparency image lacks the expected visible geometry and color variation");
			return;
		}
	}

	bool GpuProjectSceneValidation::ValidateTransparencyShadowDifference()
	{
		const auto Difference = PulseForge::GpuValidation::TransparencyImageAnalysis::CompareRgba8(
			{ 960, 540, m_TransparencyShadowOffPixels }, { 960, 540, m_TransparencyShadowOnPixels },
			PulseForge::GpuValidation::TransparencyImageAnalysis::Region{ 185, 95, 350, 282 }, true);
		if (!Difference)
		{
			Fail("Transparency shadow comparison failed: " + Difference.error());
			return false;
		}
		const std::filesystem::path DifferencePath = m_OutputRoot / "Transparency" / "shadow_difference.png";
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

	bool GpuProjectSceneValidation::ValidateMaskedShadowImageDifference(const std::vector<std::byte>& ShadowsOff,
		const std::vector<std::byte>& ShadowsOn, const char* ShadowType, uint32_t MinX, uint32_t MinY, uint32_t MaxX, uint32_t MaxY)
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
			ShadowType, Difference->ChangedPixels, Difference->MeanAbsoluteRgb, Difference->RegionChangedPixels, MinX, MinY, MaxX, MaxY);
		if (Difference->ChangedPixels < 100 || Difference->MeanAbsoluteRgb < 0.1 || Difference->RegionChangedPixels < 12)
		{
			Fail(std::string("The alpha-masked backdrop did not show a measurable ") + ShadowType + " shadow difference");
			return false;
		}
		return true;
	}

	bool GpuProjectSceneValidation::ValidateTransparencyPointShadowDifference()
	{
		return ValidateMaskedShadowImageDifference(
			m_TransparencyPointShadowOffPixels, m_TransparencyPointShadowOnPixels, "point-light", 180, 280, 370, 360);
	}

	bool GpuProjectSceneValidation::ValidateTransparencyDirectionalShadowDifference()
	{
		return ValidateMaskedShadowImageDifference(
			m_TransparencyDirectionalShadowOffPixels, m_TransparencyDirectionalShadowOnPixels, "directional", 185, 260, 350, 360);
	}

	bool GpuProjectSceneValidation::ValidateTransparencyBlendDifference()
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

	bool GpuProjectSceneValidation::ValidateTransparencyLightDifference()
	{
		const auto Difference = PulseForge::GpuValidation::TransparencyImageAnalysis::CompareRgba8(
			{ 960, 540, m_TransparencyNoSpotPixels }, { 960, 540, m_TransparencyShadowOffPixels });
		if (!Difference)
		{
			Fail("Transparency light comparison failed: " + Difference.error());
			return false;
		}
		PF_INFO("TRANSPARENCY_DIRECT_LIGHT_DIFF=changed_pixels:{0}; mean_abs_rgb:{1:.3f}; bounds=({2},{3})-({4},{5})",
			Difference->ChangedPixels, Difference->MeanAbsoluteRgb, Difference->MinX, Difference->MinY, Difference->MaxX, Difference->MaxY);
		if (Difference->ChangedPixels < 150 || Difference->MeanAbsoluteRgb < 0.15)
		{
			Fail("The configured transparency spotlight did not measurably illuminate the scene");
			return false;
		}
		return true;
	}

} // namespace PulseForge::GpuValidation
