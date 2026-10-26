#include "GpuValidation/GpuLightingValidation.h"

#include "Core/Application.h"
#include "Core/Log.h"
#include "GpuValidation/GpuSceneScenario.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"
#include "Scene/Components/DirectionalLightComponent.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace PulseForge::GpuValidation
{
	GpuLightingValidation::GpuLightingValidation(Application& App, const Scene& StartupScene, SceneRenderer& Renderer,
		LightingValidationMode Mode, std::chrono::steady_clock::time_point StartedAt) noexcept
		: m_Application(App), m_StartupScene(StartupScene), m_SceneRenderer(Renderer), m_Mode(Mode), m_StartedAt(StartedAt)
	{
	}

	void GpuLightingValidation::Initialize()
	{
		m_Scene = std::make_unique<Scene>();
		BuildGpuValidationSceneScenario(m_StartupScene, *m_Scene, true, true, true);
		switch (m_Mode)
		{
		case LightingValidationMode::LocalLights:
			InitializeLocalLightScenario();
			break;
		case LightingValidationMode::SpotlightShadows:
			InitializeSpotlightShadowScenario();
			break;
		case LightingValidationMode::PointShadows:
			InitializePointShadowScenario();
			break;
		}
	}

	std::expected<bool, std::string> GpuLightingValidation::Update()
	{
		if (!m_Succeeded) return std::unexpected(m_Failure.value_or("GPU lighting validation has failed"));
		if (IsComplete()) return true;

		const auto Timeout = m_Mode == LightingValidationMode::PointShadows ? std::chrono::seconds(60) : std::chrono::seconds(45);
		if (std::chrono::steady_clock::now() - m_StartedAt > Timeout)
			return std::unexpected(m_Mode == LightingValidationMode::LocalLights ? "Timed out preparing or rendering local-light scene"
								   : m_Mode == LightingValidationMode::SpotlightShadows
									   ? "Timed out preparing or rendering spotlight-shadow scene"
									   : "Timed out preparing or rendering point-shadow scene");

		const auto [Width, Height] = m_Application.GetWindow().GetFramebufferSize();
		if (Width == 0 || Height == 0 || (m_Mode != LightingValidationMode::LocalLights && m_ScenePrepared)) return false;

		try
		{
			if (m_Mode == LightingValidationMode::LocalLights && m_LocalLightFrames > 0) UpdateLocalLightTransforms();
			if (m_Mode == LightingValidationMode::SpotlightShadows) UpdateSpotlightShadowScenario();
			if (m_Mode == LightingValidationMode::PointShadows && !m_PointShadowScenarioUpdated)
			{
				UpdatePointShadowScenario();
				m_PointShadowScenarioUpdated = true;
			}

			const auto Prepared = m_SceneRenderer.PrepareScene(*m_Scene, static_cast<float>(Width) / static_cast<float>(Height));
			if (!Prepared)
				return std::unexpected(m_Mode == LightingValidationMode::LocalLights
										   ? "Could not prepare mixed local-light scene: " + Prepared.error().Message
									   : m_Mode == LightingValidationMode::SpotlightShadows
										   ? "Could not prepare spotlight-shadow scene: " + Prepared.error().Message
										   : "Could not prepare point-shadow scene: " + Prepared.error().Message);
			if (m_SceneRenderer.IsAssetPreparationPending()) return false;
			m_ScenePrepared = true;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(Exception.what());
		}
		return false;
	}

	std::expected<void, std::string> GpuLightingValidation::Render()
	{
		if (!m_Succeeded) return std::unexpected(m_Failure.value_or("GPU lighting validation has failed"));
		try
		{
			switch (m_Mode)
			{
			case LightingValidationMode::LocalLights:
				RenderLocalLightFrame();
				break;
			case LightingValidationMode::SpotlightShadows:
				RenderSpotlightShadowFrame();
				break;
			case LightingValidationMode::PointShadows:
				RenderPointShadowFrame();
				break;
			}
		}
		catch (const std::exception& Exception)
		{
			Fail(Exception.what());
		}
		if (m_Failure) return std::unexpected(*m_Failure);
		return {};
	}

	void GpuLightingValidation::Reset() noexcept
	{
		m_OutputTarget.reset();
		m_LocalLightEntities.clear();
		m_SpotlightShadowEntities.clear();
		m_PointShadowEntities.clear();
	}

	bool GpuLightingValidation::IsComplete() const noexcept
	{
		switch (m_Mode)
		{
		case LightingValidationMode::LocalLights:
			return m_LocalLightFrames >= 6;
		case LightingValidationMode::SpotlightShadows:
			return m_SpotShadowFrames >= 9;
		case LightingValidationMode::PointShadows:
			return m_PointShadowFrames >= 9;
		}
		return false;
	}

	std::string_view GpuLightingValidation::CompletionSummary() const noexcept
	{
		switch (m_Mode)
		{
		case LightingValidationMode::LocalLights:
			return "SceneRenderer submitted real PBR geometry with mixed point/spot lights through HDR and SDR presentation across light "
				   "updates";
		case LightingValidationMode::SpotlightShadows:
			return "spotlight slot ownership was deterministically reassigned through repeated oversubscribed Cast Shadows toggles, with "
				   "directional-light absence and target resize coverage";
		case LightingValidationMode::PointShadows:
			return "point-shadow cube faces rendered, persistent slots reassigned through live Cast Shadows changes, and point-only "
				   "shadows rendered without directional CSM";
		}
		return "Unknown GPU lighting validation mode";
	}

	void GpuLightingValidation::Fail(std::string Message)
	{
		m_Succeeded = false;
		m_Failure = std::move(Message);
	}
	void GpuLightingValidation::InitializeLocalLightScenario()
	{
		const std::array<PulseForge::PointLightComponent, 2> Points = { PulseForge::PointLightComponent{
																			{ 1.0f, 0.35f, 0.15f }, 18.0f, 12.0f },
			PulseForge::PointLightComponent{ { 0.15f, 0.35f, 1.0f }, 12.0f, 10.0f } };
		for (size_t Index = 0; Index < Points.size(); ++Index)
		{
			auto Light = m_Scene->CreateEntity("GPU validation point light");
			if (!Light || !Light->SetPointLight(Points[Index])) throw std::runtime_error("Could not create GPU validation point light");
			auto Transform = Light->GetTransform();
			if (!Transform) throw std::runtime_error("Could not read GPU validation point-light transform");
			Transform->Translation = { static_cast<float>(Index) * 2.0f - 1.0f, 2.0f, 1.0f };
			if (!Light->SetTransform(*Transform)) throw std::runtime_error("Could not position GPU validation point light");
			m_LocalLightEntities.push_back(*Light);
		}
		const std::array<PulseForge::SpotLightComponent, 2> Spots = { PulseForge::SpotLightComponent{
																		  { 0.3f, 1.0f, 0.4f }, 24.0f, 18.0f, 10.0f, 28.0f },
			PulseForge::SpotLightComponent{ { 1.0f, 0.75f, 0.25f }, 20.0f, 16.0f, 12.0f, 32.0f } };
		for (size_t Index = 0; Index < Spots.size(); ++Index)
		{
			auto Light = m_Scene->CreateEntity("GPU validation spot light");
			if (!Light || !Light->SetSpotLight(Spots[Index])) throw std::runtime_error("Could not create GPU validation spot light");
			auto Transform = Light->GetTransform();
			if (!Transform) throw std::runtime_error("Could not read GPU validation spot-light transform");
			Transform->Translation = { static_cast<float>(Index) * 3.0f - 1.5f, 3.0f, 1.5f };
			if (!Light->SetTransform(*Transform)) throw std::runtime_error("Could not position GPU validation spot light");
			m_LocalLightEntities.push_back(*Light);
		}
		for (size_t Index = 0; Index < 30; ++Index)
		{
			auto Light = m_Scene->CreateEntity("GPU validation capacity point light");
			if (!Light || !Light->SetPointLight(PulseForge::PointLightComponent{ { 0.4f, 0.5f, 0.6f }, 0.5f, 5.0f }))
				throw std::runtime_error("Could not create GPU validation capacity light");
			auto Transform = Light->GetTransform();
			if (!Transform) throw std::runtime_error("Could not read GPU validation capacity-light transform");
			Transform->Translation = { 100.0f + static_cast<float>(Index), 0.0f, 0.0f };
			if (!Light->SetTransform(*Transform)) throw std::runtime_error("Could not position GPU validation capacity light");
			m_LocalLightEntities.push_back(*Light);
		}
		PulseForge::RenderTargetDesc OutputDescription;
		OutputDescription.Width = 128;
		OutputDescription.Height = 72;
		OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
		OutputDescription.DebugName = "Local-light GPU validation SDR output";
		auto OutputTarget = m_Application.CreateRenderTarget(OutputDescription);
		if (!OutputTarget) throw std::runtime_error("Could not create local-light SDR validation target: " + OutputTarget.error().Message);
		m_OutputTarget = std::move(*OutputTarget);
		PF_INFO("Prepared GPU local-light scene with 32 point and 2 spot lights (over the combined 32-light budget) alongside bundled "
				"geometry, environment, and directional light");
	}

	void GpuLightingValidation::InitializeSpotlightShadowScenario()
	{
		for (size_t Index = 0; Index < 5; ++Index)
		{
			PulseForge::SpotLightComponent Spot;
			Spot.Color = Index % 2 == 0 ? glm::vec3(1.0f, 0.65f, 0.35f) : glm::vec3(0.35f, 0.65f, 1.0f);
			static constexpr std::array<float, 5> Intensities = { 500.0f, 100.0f, 80.0f, 60.0f, 1.0f };
			Spot.Intensity = Intensities[Index];
			Spot.Range = 30.0f;
			Spot.InnerConeAngleDegrees = 20.0f;
			Spot.OuterConeAngleDegrees = 35.0f;
			auto Light = m_Scene->CreateEntity("GPU validation shadow spotlight");
			if (!Light || !Light->SetSpotLight(Spot)) throw std::runtime_error("Could not create spotlight-shadow validation light");
			auto Transform = Light->GetTransform();
			if (!Transform) throw std::runtime_error("Could not read spotlight-shadow validation transform");
			Transform->Translation = { static_cast<float>(Index) * 2.0f - 4.0f, 5.0f, 3.0f };
			Transform->Rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			if (!Light->SetTransform(*Transform)) throw std::runtime_error("Could not position spotlight-shadow validation light");
			m_SpotlightShadowEntities.push_back(*Light);
		}

		PulseForge::RenderTargetDesc OutputDescription;
		OutputDescription.Width = 128;
		OutputDescription.Height = 72;
		OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
		OutputDescription.DebugName = "Spotlight-shadow GPU validation SDR output";
		auto OutputTarget = m_Application.CreateRenderTarget(OutputDescription);
		if (!OutputTarget) throw std::runtime_error("Could not create spotlight-shadow validation output: " + OutputTarget.error().Message);
		m_OutputTarget = std::move(*OutputTarget);
		PF_INFO("Prepared real SceneRenderer spotlight-shadow reassignment scenario: A-D outrank E; E must be evicted when A-D are all "
				"eligible; includes bundled geometry, IBL, directional CSM, and SSAO");
	}

	void GpuLightingValidation::InitializePointShadowScenario()
	{
		static constexpr std::array<float, 3> Intensities = { 300.0f, 200.0f, 100.0f };
		for (size_t Index = 0; Index < Intensities.size(); ++Index)
		{
			PulseForge::PointLightComponent Point;
			Point.Color = Index == 0   ? glm::vec3(1.0f, 0.72f, 0.45f)
						  : Index == 1 ? glm::vec3(0.45f, 0.72f, 1.0f)
									   : glm::vec3(0.65f, 1.0f, 0.5f);
			Point.Intensity = Intensities[Index];
			Point.Range = 30.0f;
			Point.CastShadows = true;
			auto Light = m_Scene->CreateEntity("GPU validation shadow point light");
			if (!Light || !Light->SetPointLight(Point)) throw std::runtime_error("Could not create point-shadow validation light");
			auto Transform = Light->GetTransform();
			if (!Transform) throw std::runtime_error("Could not read point-shadow validation transform");
			Transform->Translation = { static_cast<float>(Index) * 2.0f - 2.0f, 2.0f, -1.0f };
			if (!Light->SetTransform(*Transform)) throw std::runtime_error("Could not position point-shadow validation light");
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
		auto OutputTarget = m_Application.CreateRenderTarget(OutputDescription);
		if (!OutputTarget) throw std::runtime_error("Could not create point-shadow validation output: " + OutputTarget.error().Message);
		m_OutputTarget = std::move(*OutputTarget);
		PF_INFO("Prepared point-shadow GPU scene with three differently weighted shadow-requesting points, mixed with directional CSM, "
				"IBL, SSAO, and bundled indexed geometry");
	}

	void GpuLightingValidation::UpdatePointShadowScenario()
	{
		const size_t Frame = m_PointShadowFrames;
		auto DualSpot = m_PointShadowEntities.front().GetSpotLight();
		if (!DualSpot || !DualSpot->has_value()) throw std::runtime_error("Could not read the co-located spotlight shadow state");
		DualSpot->value().CastShadows = Frame == 1 || Frame == 2 || Frame == 3 || Frame == 5 || Frame == 6 || Frame == 7;
		DualSpot->value().Intensity = Frame == 2 || Frame == 3 || Frame == 6 ? 25.0f : 1000.0f;
		if (!m_PointShadowEntities.front().SetSpotLight(DualSpot->value()))
			throw std::runtime_error("Could not update the co-located spotlight shadow state");

		if (Frame == 0 || Frame == 8)
		{
			const std::array<size_t, 3> Lights = { 0, 1, 2 };
			for (size_t Index : Lights)
			{
				auto Point = m_PointShadowEntities[Index].GetPointLight();
				if (!Point || !Point->has_value()) throw std::runtime_error("Could not read point-shadow toggle state");
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
				if (!Point || !Point->has_value()) throw std::runtime_error("Could not read point light before enabling the shadow budget");
				Point->value().CastShadows = true;
				if (!m_PointShadowEntities[Index].SetPointLight(Point->value()))
					throw std::runtime_error("Could not enable the point-shadow budget");
			}
		}
		else
		{
			const size_t Toggled = Frame == 4 || Frame == 5 ? 1 : 0;
			auto Point = m_PointShadowEntities[Toggled].GetPointLight();
			if (!Point || !Point->has_value()) throw std::runtime_error("Could not read point light before shadow-slot reassignment");
			Point->value().CastShadows = Frame == 3 || Frame == 5 || Frame == 7;
			if (!m_PointShadowEntities[Toggled].SetPointLight(Point->value()))
				throw std::runtime_error("Could not apply point-shadow reassignment");
		}
		if (Frame == 3)
		{
			auto Transform = m_PointShadowEntities[0].GetTransform();
			if (!Transform) throw std::runtime_error("Could not read moving point-light transform");
			Transform->Translation.x += 0.25f;
			if (!m_PointShadowEntities[0].SetTransform(*Transform))
				throw std::runtime_error("Could not move point-shadow validation light");
		}
		if (Frame == 7)
		{
			for (const PulseForge::Entity Entity : m_Scene->GetEntities())
			{
				auto Directional = Entity.GetDirectionalLight();
				if (!Directional) throw std::runtime_error("Could not inspect directional light in point-shadow validation scene");
				if (*Directional && !Entity.RemoveDirectionalLight())
					throw std::runtime_error("Could not remove directional light for point-only shadow validation");
			}
			PulseForge::RenderTargetDesc Description;
			Description.Width = 96;
			Description.Height = 64;
			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			Description.DebugName = "Resized point-shadow GPU validation output";
			auto Target = m_Application.CreateRenderTarget(Description);
			if (!Target) throw std::runtime_error("Could not resize point-shadow validation output: " + Target.error().Message);
			m_OutputTarget = std::move(*Target);
		}
	}

	void GpuLightingValidation::RenderPointShadowFrame()
	{
		if (!m_Succeeded || !m_ScenePrepared || m_PointShadowFrames >= 9) return;
		auto Rendered = m_SceneRenderer.RenderPreparedSceneToOutput(*m_OutputTarget);
		if (!Rendered || Rendered->GeometryDrawCount == 0)
		{
			Fail(Rendered ? "Point-shadow validation rendered no indexed PBR geometry"
						  : "Point-shadow scene render failed: " + Rendered.error().Message);
			return;
		}
		static constexpr std::array<size_t, 9> ExpectedPasses = { 0, 12, 12, 12, 12, 12, 12, 12, 0 };
		const size_t Expected = ExpectedPasses[m_PointShadowFrames];
		const bool ExpectSpotShadow = m_PointShadowFrames == 1 || m_PointShadowFrames == 2 || m_PointShadowFrames == 3 ||
									  m_PointShadowFrames == 5 || m_PointShadowFrames == 6 || m_PointShadowFrames == 7;
		if (Rendered->PointShadowPassCount != Expected || Rendered->PointShadowCasterDrawCount != Expected * Rendered->GeometryDrawCount ||
			Rendered->SpotlightShadowPassCount != static_cast<size_t>(ExpectSpotShadow) ||
			Rendered->SpotlightShadowCasterDrawCount != static_cast<size_t>(ExpectSpotShadow) * Rendered->GeometryDrawCount)
		{
			Fail("Mixed point/spot shadow pass counts did not match the dual-component allocation");
			return;
		}
		if ((ExpectSpotShadow &&
				Rendered->SpotlightShadowSlotOwners[0] != std::optional<PulseForge::UUID>{ m_PointShadowEntities.front().GetUUID() }) ||
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
			if (!LightIndex) return !Rendered->PointShadowSlotOwners[Slot];
			return Rendered->PointShadowSlotOwners[Slot] &&
				   *Rendered->PointShadowSlotOwners[Slot] == m_PointShadowEntities[*LightIndex].GetUUID();
		};
		static constexpr std::array<std::array<int32_t, 2>, 9> ExpectedOwners = { { { { -1, -1 } }, { { 0, 1 } }, { { 2, 1 } },
			{ { 0, 1 } }, { { 0, 2 } }, { { 0, 1 } }, { { 2, 1 } }, { { 0, 1 } }, { { -1, -1 } } } };
		for (size_t Slot = 0; Slot < 2; ++Slot)
		{
			const int32_t Owner = ExpectedOwners[m_PointShadowFrames][Slot];
			if (!OwnerIs(Slot, Owner < 0 ? std::nullopt : std::optional<size_t>{ static_cast<size_t>(Owner) }))
			{
				Fail("Point-shadow cube slots did not match deterministic relevance selection after toggling");
				return;
			}
		}
		PF_INFO("POINT_SHADOW_FRAME={0}; face_passes={1}; caster_draws={2}; pbr_draws={3}; resource_generation={4}; owners=[{5},{6}]; "
				"hdr_generation={7}",
			m_PointShadowFrames, Rendered->PointShadowPassCount, Rendered->PointShadowCasterDrawCount, Rendered->GeometryDrawCount,
			Rendered->PointShadowResourceGeneration,
			Rendered->PointShadowSlotOwners[0] ? Rendered->PointShadowSlotOwners[0]->ToString() : "none",
			Rendered->PointShadowSlotOwners[1] ? Rendered->PointShadowSlotOwners[1]->ToString() : "none", Rendered->HdrTargetGeneration);
		++m_PointShadowFrames;
		m_ScenePrepared = false;
		m_PointShadowScenarioUpdated = false;
	}

	void GpuLightingValidation::UpdateSpotlightShadowScenario()
	{
		const size_t Frame = m_SpotShadowFrames;
		if (Frame == 0)
		{
			for (size_t Index = 0; Index < m_SpotlightShadowEntities.size(); ++Index)
			{
				auto Spot = m_SpotlightShadowEntities[Index].GetSpotLight();
				if (!Spot || !Spot->has_value()) throw std::runtime_error("Could not initialize spotlight-shadow validation settings");
				Spot->value().CastShadows = false;
				if (!m_SpotlightShadowEntities[Index].SetSpotLight(Spot->value()))
					throw std::runtime_error("Could not initialize spotlight-shadow validation settings");
			}
		}
		else if (Frame >= 2 && Frame <= 7)
		{
			const size_t ToggledIndex = Frame == 2 || Frame == 3 ? 0 : Frame == 4 || Frame == 5 ? 1 : 2;
			auto Spot = m_SpotlightShadowEntities[ToggledIndex].GetSpotLight();
			if (!Spot || !Spot->has_value()) throw std::runtime_error("Could not read spotlight before repeated shadow-slot reassignment");
			Spot->value().CastShadows = Frame % 2 != 0;
			if (!m_SpotlightShadowEntities[ToggledIndex].SetSpotLight(Spot->value()))
				throw std::runtime_error("Could not apply repeated Cast Shadows change");
			if (Frame == 2)
			{
				auto Transform = m_SpotlightShadowEntities[0].GetTransform();
				if (!Transform) throw std::runtime_error("Could not read moving spotlight-shadow transform");
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
				if (!Spot || !Spot->has_value()) throw std::runtime_error("Could not enable spotlight budget validation");
				Spot->value().CastShadows = true;
				if (!m_SpotlightShadowEntities[Index].SetSpotLight(Spot->value()))
					throw std::runtime_error("Could not enable spotlight budget validation");
			}
		}
		else if (Frame == 8)
		{
			for (const PulseForge::Entity Entity : m_Scene->GetEntities())
			{
				auto Directional = Entity.GetDirectionalLight();
				if (!Directional) throw std::runtime_error("Could not inspect directional light in spotlight-shadow scenario");
				if (*Directional && !Entity.RemoveDirectionalLight())
					throw std::runtime_error("Could not remove directional light for spotlight-only shadow validation");
			}
			PulseForge::RenderTargetDesc OutputDescription;
			OutputDescription.Width = 96;
			OutputDescription.Height = 64;
			OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			OutputDescription.DebugName = "Resized spotlight-shadow GPU validation output";
			auto OutputTarget = m_Application.CreateRenderTarget(OutputDescription);
			if (!OutputTarget)
				throw std::runtime_error("Could not resize spotlight-shadow validation output: " + OutputTarget.error().Message);
			m_OutputTarget = std::move(*OutputTarget);
		}
	}

	void GpuLightingValidation::RenderSpotlightShadowFrame()
	{
		if (!m_Succeeded || !m_ScenePrepared || m_SpotShadowFrames >= 9) return;
		auto Rendered = m_SceneRenderer.RenderPreparedSceneToOutput(*m_OutputTarget);
		if (!Rendered || Rendered->GeometryDrawCount == 0)
		{
			Fail(Rendered ? "Spotlight-shadow validation rendered no PBR geometry"
						  : "Spotlight-shadow scene render failed: " + Rendered.error().Message);
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
				if (HasOwner(Index) != (std::ranges::find(Expected, Index) != Expected.end())) return false;
			return true;
		};
		const auto HasExpectedSlotOrder = [&](const std::array<int32_t, 4>& Expected)
		{
			for (size_t Slot = 0; Slot < Expected.size(); ++Slot)
			{
				if (Expected[Slot] < 0)
				{
					if (Rendered->SpotlightShadowSlotOwners[Slot]) return false;
				}
				else if (!Rendered->SpotlightShadowSlotOwners[Slot] ||
						 *Rendered->SpotlightShadowSlotOwners[Slot] !=
							 m_SpotlightShadowEntities[static_cast<size_t>(Expected[Slot])].GetUUID())
					return false;
			}
			return true;
		};
		if ((m_SpotShadowFrames == 0 && !HasExpectedOwners({})) || (m_SpotShadowFrames == 1 && !HasExpectedOwners({ 0, 1, 2, 3 })) ||
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
		static constexpr std::array<std::array<int32_t, 4>, 9> ExpectedSlotOwners = { { { { -1, -1, -1, -1 } }, { { 0, 1, 2, 3 } },
			{ { 4, 1, 2, 3 } }, { { 0, 1, 2, 3 } }, { { 0, 4, 2, 3 } }, { { 0, 1, 2, 3 } }, { { 0, 1, 4, 3 } }, { { 0, 1, 2, 3 } },
			{ { 0, 1, 2, 3 } } } };
		if (!HasExpectedSlotOrder(ExpectedSlotOwners[m_SpotShadowFrames]))
		{
			Fail("Spotlight shadow maps were not rebound to their exact deterministic slot owners");
			return;
		}
		if (m_SpotShadowFrames == 0) m_FirstSpotShadowHdrGeneration = Rendered->HdrTargetGeneration;
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
		PF_INFO("SPOT_SHADOW_FRAME={0}; shadow_passes={1}; caster_draws={2}; pbr_draws={3}; hdr_generation={4}; "
				"shadow_resource_generation={5}; slot_owners=[{6},{7},{8},{9}]",
			m_SpotShadowFrames, Rendered->SpotlightShadowPassCount, Rendered->SpotlightShadowCasterDrawCount, Rendered->GeometryDrawCount,
			Rendered->HdrTargetGeneration, Rendered->SpotlightShadowResourceGeneration,
			Rendered->SpotlightShadowSlotOwners[0] ? Rendered->SpotlightShadowSlotOwners[0]->ToString() : "none",
			Rendered->SpotlightShadowSlotOwners[1] ? Rendered->SpotlightShadowSlotOwners[1]->ToString() : "none",
			Rendered->SpotlightShadowSlotOwners[2] ? Rendered->SpotlightShadowSlotOwners[2]->ToString() : "none",
			Rendered->SpotlightShadowSlotOwners[3] ? Rendered->SpotlightShadowSlotOwners[3]->ToString() : "none");
		++m_SpotShadowFrames;
		m_ScenePrepared = false;
	}

	void GpuLightingValidation::UpdateLocalLightTransforms()
	{
		for (size_t Index = 0; Index < m_LocalLightEntities.size(); ++Index)
		{
			const auto Current = m_LocalLightEntities[Index].GetTransform();
			if (!Current) throw std::runtime_error("Could not read moving GPU validation light transform");
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
			if (!Point || !Point->has_value()) throw std::runtime_error("Could not update GPU validation point-light count");
			Point->value().Intensity = m_LocalLightFrames == 1 ? 0.0f : 18.0f;
			if (!m_LocalLightEntities[0].SetPointLight(Point->value()))
				throw std::runtime_error("Could not change GPU validation point-light count");
		}
		if (m_LocalLightFrames == 2 || m_LocalLightFrames == 4)
		{
			if (m_LocalLightFrames == 2)
			{
				if (!m_LocalLightEntities[2].RemoveSpotLight()) throw std::runtime_error("Could not reduce GPU validation spotlight count");
			}
			else if (!m_LocalLightEntities[2].SetSpotLight(
						 PulseForge::SpotLightComponent{ { 0.3f, 1.0f, 0.4f }, 24.0f, 18.0f, 10.0f, 28.0f }))
				throw std::runtime_error("Could not restore GPU validation spotlight count");
		}
	}

	void GpuLightingValidation::RenderLocalLightFrame()
	{
		if (!m_Succeeded || !m_ScenePrepared || m_LocalLightFrames >= 6) return;
		auto Rendered = m_SceneRenderer.RenderPreparedSceneToOutput(*m_OutputTarget);
		if (!Rendered || Rendered->GeometryDrawCount == 0)
		{
			Fail(Rendered ? "Local-light validation rendered no PBR geometry"
						  : "Local-light scene render failed: " + Rendered.error().Message);
			return;
		}
		PF_INFO("Local-light GPU validation frame {0}: {1} geometry draw(s), HDR generation {2}", m_LocalLightFrames,
			Rendered->GeometryDrawCount, Rendered->HdrTargetGeneration);
		++m_LocalLightFrames;
		m_ScenePrepared = false;
	}

} // namespace PulseForge::GpuValidation
