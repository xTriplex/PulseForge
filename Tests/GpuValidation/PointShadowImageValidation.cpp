#include "GpuValidation/PointShadowImageValidation.h"

#include "Assets/Project.h"
#include "Core/Application.h"
#include "Core/Log.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Scene.h"
#include "GpuValidation/PointShadowImageAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

#include "../../PulseForge/Vendor/stb/stb_image_write.h"

namespace PulseForge::GpuValidation
{
	namespace
	{
		struct PointShadowImageCase
		{
			const char* Name;
			glm::vec3 LightPosition;
			glm::vec3 ExpectedReceiver;
			glm::vec3 ControlReceiver;
			bool CastShadows;
			bool UseCeiling;
			bool PointLightRotated;
			glm::vec3 CasterOffset{ 0.0f };
		};

		struct PointShadowOverviewImageCase
		{
			const char* Name;
			glm::vec3 LightPosition;
			bool CastShadows;
		};

		constexpr std::array<PointShadowOverviewImageCase, 4> PointShadowOverviewImageCases = {
			{ { "overview_committed_position_shadows_off", { 0.0f, 3.2f, 1.3f }, false },
				{ "overview_committed_position_shadows_on", { 0.0f, 3.2f, 1.3f }, true },
				{ "overview_user_position_shadows_off", { 0.0f, 3.109f, -1.233f }, false },
				{ "overview_user_position_shadows_on", { 0.0f, 3.109f, -1.233f }, true } }
		};

		constexpr std::array<PointShadowImageCase, 23> PointShadowImageCases = {
			{ { "positive_x_shadows_off", { 2.0f, 3.0f, 0.0f }, { -1.5f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }, false, false, false },
				{ "positive_x_shadows_on", { 2.0f, 3.0f, 0.0f }, { -1.5f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }, true, false, false },
				{ "negative_x_shadows_off", { -2.0f, 3.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }, { -1.5f, 0.0f, 0.0f }, false, false, false },
				{ "negative_x_shadows_on", { -2.0f, 3.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }, { -1.5f, 0.0f, 0.0f }, true, false, false },
				{ "positive_z_shadows_off", { 0.0f, 3.0f, 2.0f }, { 0.0f, 0.0f, -2.0f }, { 0.0f, 0.0f, 2.0f }, false, false, false },
				{ "positive_z_shadows_on", { 0.0f, 3.0f, 2.0f }, { 0.0f, 0.0f, -2.0f }, { 0.0f, 0.0f, 2.0f }, true, false, false },
				{ "negative_z_shadows_off", { 0.0f, 3.0f, -2.0f }, { 0.0f, 0.0f, 2.0f }, { 0.0f, 0.0f, -2.0f }, false, false, false },
				{ "negative_z_shadows_on", { 0.0f, 3.0f, -2.0f }, { 0.0f, 0.0f, 2.0f }, { 0.0f, 0.0f, -2.0f }, true, false, false },
				{ "overhead_shadows_off", { 0.0f, 3.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }, false, false, false },
				{ "overhead_shadows_on", { 0.0f, 3.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }, true, false, false },
				{ "negative_y_shadows_off", { 0.0f, -2.0f, 0.0f }, { 0.0f, 3.0f, 0.0f }, { 2.0f, 3.0f, 0.0f }, false, true, false },
				{ "negative_y_shadows_on", { 0.0f, -2.0f, 0.0f }, { 0.0f, 3.0f, 0.0f }, { 2.0f, 3.0f, 0.0f }, true, true, false },
				{ "positive_z_off_axis_caster_shadows_off", { 0.0f, 3.0f, 2.0f }, { 1.25f, 0.0f, -1.0f }, { -1.5f, 0.0f, 1.5f }, false,
					false, false, { 0.5f, 0.0f, 0.0f } },
				{ "positive_z_off_axis_caster_shadows_on", { 0.0f, 3.0f, 2.0f }, { 1.25f, 0.0f, -1.0f }, { -1.5f, 0.0f, 1.5f }, true, false,
					false, { 0.5f, 0.0f, 0.0f } },
				{ "positive_x_off_axis_caster_shadows_off", { 2.0f, 3.0f, 0.0f }, { -1.0f, 0.0f, 0.375f }, { 1.5f, 0.0f, 1.5f }, false,
					false, false, { 0.0f, 0.0f, 0.25f } },
				{ "positive_x_off_axis_caster_shadows_on", { 2.0f, 3.0f, 0.0f }, { -1.0f, 0.0f, 0.375f }, { 1.5f, 0.0f, 1.5f }, true, false,
					false, { 0.0f, 0.0f, 0.25f } },
				{ "overhead_off_axis_caster_shadows_off", { 0.0f, 3.0f, 0.0f }, { 0.0f, 0.0f, 0.75f }, { 2.0f, 0.0f, 2.0f }, false, false,
					false, { 0.0f, 0.0f, 0.5f } },
				{ "overhead_off_axis_caster_shadows_on", { 0.0f, 3.0f, 0.0f }, { 0.0f, 0.0f, 0.75f }, { 2.0f, 0.0f, 2.0f }, true, false,
					false, { 0.0f, 0.0f, 0.5f } },
				{ "negative_x_negative_y_face_edge_shadows_off", { 1.0f, 2.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 1.5f, 0.0f, 1.5f }, false,
					false, false },
				{ "negative_x_negative_y_face_edge_shadows_on", { 1.0f, 2.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 1.5f, 0.0f, 1.5f }, true,
					false, false },
				{ "negative_x_negative_z_cube_corner_shadows_off", { 2.0f, 3.0f, 2.0f }, { -1.0f, 0.0f, -1.0f }, { 2.0f, 0.0f, 0.0f },
					false, false, false },
				{ "negative_x_negative_z_cube_corner_shadows_on", { 2.0f, 3.0f, 2.0f }, { -1.0f, 0.0f, -1.0f }, { 2.0f, 0.0f, 0.0f }, true,
					false, false },
				{ "positive_x_rotated_point_light", { 2.0f, 3.0f, 0.0f }, { -1.5f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }, true, false, true } }
		};

		namespace PointShadowAnalysis = PulseForge::GpuValidation::PointShadowImageAnalysis;

		using PointShadowImageMetrics = PointShadowAnalysis::Metrics;
		using PointShadowDepthProbe = PointShadowAnalysis::DepthProbe;

		PointShadowAnalysis::ImageView MakeImageView(const PulseForge::TextureReadback& Image)
		{
			return { Image.Width, Image.Height, std::span<const std::byte>(Image.Pixels) };
		}

		bool SegmentIntersectsShadowCaster(
			const glm::vec3& Start, const glm::vec3& End, const glm::vec3& CasterOffset = glm::vec3(0.0f)) noexcept
		{
			return PointShadowAnalysis::SegmentIntersectsShadowCaster(Start, End, CasterOffset);
		}

		PointShadowImageMetrics MeasurePointShadowImage(const PulseForge::TextureReadback& Off, const PulseForge::TextureReadback& On,
			const glm::mat4& ViewProjection, const glm::vec3& CameraPosition, const glm::vec3& LightPosition, float ReceiverPlaneY,
			const glm::vec3& CasterOffset)
		{
			return PointShadowAnalysis::MeasurePointShadowImage(
				MakeImageView(Off), MakeImageView(On), ViewProjection, CameraPosition, LightPosition, ReceiverPlaneY, CasterOffset);
		}

		PointShadowDepthProbe ProbePointShadowDepth(const std::array<std::optional<PulseForge::TextureReadback>, 6>& Faces,
			const glm::vec3& LightPosition, const glm::vec3& Receiver, float Range)
		{
			std::array<std::optional<PointShadowAnalysis::ImageView>, 6> FaceViews{};
			for (size_t Face = 0; Face < Faces.size(); ++Face)
				if (Faces[Face]) FaceViews[Face] = MakeImageView(*Faces[Face]);
			return PointShadowAnalysis::ProbePointShadowDepth(FaceViews, LightPosition, Receiver, Range);
		}

	} // namespace

	struct PointShadowImageValidation::Impl
	{
		Impl(Scene& StartupScene, SceneRenderer& Renderer, std::filesystem::path OutputDirectory)
			: m_Scene(StartupScene), m_SceneRenderer(&Renderer), m_PointShadowImageDirectory(std::move(OutputDirectory))
		{
		}

		void Initialize()
		{
			InitializePointShadowImageScenario();
			InitializePointShadowOverviewImageScenario();
		}

		std::expected<bool, std::string> Update(std::chrono::steady_clock::time_point StartedAt)
		{
			if (!m_Error.empty()) return std::unexpected(m_Error);
			if (m_PointShadowImageCapturePending)
			{
				CapturePointShadowImage();
				if (!m_Error.empty()) return std::unexpected(m_Error);
				m_PointShadowImageCapturePending = false;
				++m_PointShadowImageCase;
				m_ScenePrepared = false;
			}
			if (m_PointShadowImageCase >= PointShadowImageCases.size())
			{
				if (!m_PointShadowImageValidationComplete)
				{
					if (!ValidatePointShadowImageResults() || !m_Error.empty())
						return std::unexpected(m_Error.empty() ? "Rendered point-shadow image validation failed." : m_Error);
					m_PointShadowImageValidationComplete = true;
				}
				if (!m_PointShadowOverviewSettingsRestored)
				{
					const auto RestoredSettings = RestoreRendererSettings();
					if (!RestoredSettings)
					{
						Fail(RestoredSettings.error());
						return std::unexpected(m_Error);
					}
					m_PointShadowOverviewSettingsRestored = true;
				}
				if (m_PointShadowOverviewCapturePending)
				{
					CapturePointShadowOverviewImage();
					if (!m_Error.empty()) return std::unexpected(m_Error);
					m_PointShadowOverviewCapturePending = false;
					++m_PointShadowOverviewImageCase;
					m_ScenePrepared = false;
				}
				if (m_PointShadowOverviewImageCase >= PointShadowOverviewImageCases.size())
				{
					m_Complete = true;
					return true;
				}
				if (m_ScenePrepared) return false;
				PreparePointShadowOverviewImageCase();
				if (!m_Error.empty()) return std::unexpected(m_Error);
				return false;
			}
			if (std::chrono::steady_clock::now() - StartedAt > std::chrono::seconds(90))
				return std::unexpected("Timed out capturing point-shadow direction images");
			if (m_ScenePrepared) return false;
			PreparePointShadowImageCase();
			if (!m_Error.empty()) return std::unexpected(m_Error);
			return false;
		}

		std::expected<void, std::string> Render()
		{
			if (!m_Error.empty()) return std::unexpected(m_Error);
			if (!m_Succeeded || !m_ScenePrepared) return {};
			if (m_CapturingPointShadowOverview) RenderPointShadowOverviewImageFrame();
			else RenderPointShadowImageFrame();
			if (!m_Error.empty()) return std::unexpected(m_Error);
			return {};
		}

		std::expected<void, std::string> Release()
		{
			if (m_AmbientOcclusionChanged || m_ToneMappingChanged)
			{
				const auto RestoredSettings = RestoreRendererSettings();
				if (!RestoredSettings)
				{
					if (m_Error.empty()) m_Error = RestoredSettings.error();
					else m_Error += "; " + RestoredSettings.error();
				}
			}
			m_PointShadowImageOutputTarget.reset();
			m_PointShadowOverviewOutputTarget.reset();
			if (!m_Error.empty()) return std::unexpected(m_Error);
			return {};
		}

		bool IsComplete() const noexcept { return m_Complete && m_Error.empty(); }

		std::expected<void, std::string> RestoreRendererSettings()
		{
			if (m_AmbientOcclusionChanged)
			{
				if (!m_SavedAmbientOcclusionSettings || !m_SceneRenderer->SetAmbientOcclusionSettings(*m_SavedAmbientOcclusionSettings))
					return std::unexpected(
						"Could not restore the renderer's ambient occlusion settings after point-shadow image validation");
				m_AmbientOcclusionChanged = false;
			}
			if (m_ToneMappingChanged)
			{
				if (!m_SavedToneMappingSettings || !m_SceneRenderer->SetToneMappingSettings(*m_SavedToneMappingSettings))
					return std::unexpected("Could not restore the renderer's tone-mapping settings after point-shadow image validation");
				m_ToneMappingChanged = false;
			}
			return {};
		}

		void Fail(const std::string& Message)
		{
			m_Succeeded = false;
			if (m_Error.empty()) m_Error = Message;
		}

		void InitializePointShadowImageScenario()
		{
			std::filesystem::create_directories(m_PointShadowImageDirectory);
			PulseForge::Entity CameraSource;
			PulseForge::Entity FloorSource;
			PulseForge::Entity CubeSource;
			for (const PulseForge::Entity Source : m_Scene.GetEntities())
			{
				const auto Tag = Source.GetTag();
				const auto Transform = Source.GetTransform();
				const auto Camera = Source.GetCamera();
				const auto Mesh = Source.GetMeshRenderer();
				if (!Tag || !Transform || !Camera || !Mesh) throw std::runtime_error("Could not inspect point-shadow image source scene");
				if (*Camera) CameraSource = Source;
				if (*Mesh && Tag->Name == "Floor - broad shadow receiver") FloorSource = Source;
				if (*Mesh && Tag->Name == "Warm matte block") CubeSource = Source;
			}
			if (!CameraSource || !FloorSource || !CubeSource)
				throw std::runtime_error("Point-shadow image source scene lacks its camera, floor, or cube mesh");

			const auto CopyMeshEntity = [&](const PulseForge::Entity& Source, const char* Name)
			{
				const auto Transform = Source.GetTransform();
				const auto Mesh = Source.GetMeshRenderer();
				if (!Transform || !Mesh || !*Mesh) throw std::runtime_error("Could not copy controlled point-shadow image geometry");
				auto Copy = m_PointShadowImageScene.CreateEntity(Name);
				if (!Copy || !Copy->SetTransform(*Transform) || !Copy->SetMeshRenderer(**Mesh))
					throw std::runtime_error("Could not create controlled point-shadow image geometry");
				return *Copy;
			};

			const auto CameraTransform = CameraSource.GetTransform();
			const auto Camera = CameraSource.GetCamera();
			if (!CameraTransform || !Camera || !*Camera) throw std::runtime_error("Could not copy point-shadow image camera");
			auto ImageCamera = m_PointShadowImageScene.CreateEntity("Fixed camera for point-shadow direction captures");
			if (!ImageCamera || !ImageCamera->SetTransform(*CameraTransform))
				throw std::runtime_error("Could not create point-shadow image camera");
			PulseForge::CameraComponent CameraComponent = **Camera;
			CameraComponent.IsPrimary = true;
			if (!ImageCamera->SetCamera(CameraComponent)) throw std::runtime_error("Could not configure point-shadow image camera");
			m_PointShadowImageCamera = *ImageCamera;
			m_PointShadowImageCameraTransform = *CameraTransform;

			m_PointShadowImageFloor = CopyMeshEntity(FloorSource, "Only receiving floor");
			const auto FloorMesh = m_PointShadowImageFloor.GetMeshRenderer();
			if (!FloorMesh || !*FloorMesh) throw std::runtime_error("Could not preserve the point-shadow image floor material reference");
			m_PointShadowImageFloorMesh = **FloorMesh;
			m_PointShadowImageCeiling = CopyMeshEntity(FloorSource, "Ceiling receiver for negative-Y face");
			m_PointShadowImageCeilingTransform = *m_PointShadowImageCeiling.GetTransform();
			m_PointShadowImageCeilingTransform.Translation = { 0.0f, 3.0f, 0.0f };
			m_PointShadowImageCeilingTransform.Rotation = glm::angleAxis(std::numbers::pi_v<float>, glm::vec3(1.0f, 0.0f, 0.0f));
			m_PointShadowImageCeilingTransform.Scale = { 8.0f, 0.12f, 8.0f };
			if (!m_PointShadowImageCeiling.SetTransform(m_PointShadowImageCeilingTransform))
				throw std::runtime_error("Could not orient the negative-Y shadow receiver");

			m_PointShadowImageCube = CopyMeshEntity(CubeSource, "Single centered shadow-casting cube");
			PulseForge::TransformComponent CubeTransform = *m_PointShadowImageCube.GetTransform();
			CubeTransform.Translation = { 0.0f, 1.0f, 0.0f };
			CubeTransform.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			CubeTransform.Scale = { 1.0f, 1.0f, 1.0f };
			if (!m_PointShadowImageCube.SetTransform(CubeTransform))
				throw std::runtime_error("Could not center the point-shadow image cube");

			PulseForge::PointLightComponent Point;
			Point.Color = glm::vec3(1.0f);
			Point.Intensity = 70.0f;
			Point.Range = 10.0f;
			Point.CastShadows = false;
			auto Light = m_PointShadowImageScene.CreateEntity("Single omnidirectional point light");
			if (!Light || !Light->SetPointLight(Point)) throw std::runtime_error("Could not create controlled point-shadow image light");
			m_PointShadowImageLight = *Light;

			m_SavedAmbientOcclusionSettings = m_SceneRenderer->GetAmbientOcclusionSettings();
			m_SavedToneMappingSettings = m_SceneRenderer->GetToneMappingSettings();
			if (!m_SavedAmbientOcclusionSettings || !m_SavedToneMappingSettings)
				throw std::runtime_error("Could not read renderer settings for controlled point-shadow image captures");
			PulseForge::AmbientOcclusionSettings Ao = *m_SavedAmbientOcclusionSettings;
			Ao.Enabled = false;
			if (!m_SceneRenderer->SetAmbientOcclusionSettings(Ao))
				throw std::runtime_error("Could not disable SSAO for controlled point-shadow image captures");
			m_AmbientOcclusionChanged = true;
			PulseForge::ToneMappingSettings Tone = *m_SavedToneMappingSettings;
			Tone.ExposureEV = 0.0f;
			if (!m_SceneRenderer->SetToneMappingSettings(Tone))
				throw std::runtime_error("Could not fix exposure for controlled point-shadow image captures");
			m_ToneMappingChanged = true;

			PulseForge::RenderTargetDesc OutputDescription;
			OutputDescription.Width = 512;
			OutputDescription.Height = 512;
			OutputDescription.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			OutputDescription.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			OutputDescription.DebugName = "Point-shadow direction image readback output";
			auto Output = PulseForge::Application::Get().CreateRenderTarget(OutputDescription);
			if (!Output) throw std::runtime_error("Could not create point-shadow image readback target: " + Output.error().Message);
			m_PointShadowImageOutputTarget = std::move(*Output);
			PF_INFO("Prepared isolated image test: one 70-intensity point light, a unit cube, one broad receiver, no IBL/CSM/SSAO, fixed "
					"EV 0; readback path: {0}",
				m_PointShadowImageDirectory.string());
		}

		void InitializePointShadowOverviewImageScenario()
		{
			for (const PulseForge::Entity Entity : m_Scene.GetEntities())
			{
				const auto Tag = Entity.GetTag();
				const auto Point = Entity.GetPointLight();
				if (!Tag || !Point) throw std::runtime_error("Could not inspect the startup scene's point lights for image capture");
				if (*Point && Tag->Name == "Point Light A - Primary Shadow") m_PointShadowOverviewPrimary = Entity;
				if (*Point && Tag->Name == "Point Light B - Secondary Shadow") m_PointShadowOverviewSecondary = Entity;
			}
			if (!m_PointShadowOverviewPrimary || !m_PointShadowOverviewSecondary)
				throw std::runtime_error("Startup scene lacks the expected primary or secondary point light");

			PulseForge::RenderTargetDesc Description;
			Description.Width = 960;
			Description.Height = 540;
			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			Description.DepthMode = PulseForge::DepthAttachmentMode::Attachment;
			Description.DebugName = "Point-shadow startup Overview image readback output";
			auto Target = PulseForge::Application::Get().CreateRenderTarget(Description);
			if (!Target) throw std::runtime_error("Could not create startup Overview image target: " + Target.error().Message);
			m_PointShadowOverviewOutputTarget = std::move(*Target);
		}

		bool PreparePointShadowOverviewImageCase()
		{
			const PointShadowOverviewImageCase& TestCase = PointShadowOverviewImageCases[m_PointShadowOverviewImageCase];
			auto Point = m_PointShadowOverviewPrimary.GetPointLight();
			auto Transform = m_PointShadowOverviewPrimary.GetTransform();
			if (!Point || !*Point || !Transform)
			{
				Fail("Could not update the startup-scene primary point light for image capture");
				return false;
			}
			Point->value().CastShadows = TestCase.CastShadows;
			if (!m_PointShadowOverviewPrimary.SetPointLight(Point->value()))
			{
				Fail("Startup-scene point-light shadow toggle failed validation");
				return false;
			}
			Transform->Translation = TestCase.LightPosition;
			Transform->Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			if (!m_PointShadowOverviewPrimary.SetTransform(*Transform))
			{
				Fail("Could not set the startup-scene point-light transform for image capture");
				return false;
			}
			const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
			if (Width == 0 || Height == 0) return false;
			const auto Prepared =
				m_SceneRenderer->PrepareScene(m_Scene, static_cast<float>(m_PointShadowOverviewOutputTarget->GetDescription().Width) /
														   static_cast<float>(m_PointShadowOverviewOutputTarget->GetDescription().Height));
			if (!Prepared)
			{
				Fail("Could not prepare the committed startup scene for image capture: " + Prepared.error().Message);
				return false;
			}
			m_CapturingPointShadowOverview = true;
			m_ScenePrepared = true;
			PF_INFO(
				"POINT_SHADOW_OVERVIEW_CASE={0}; primary_position=({1},{2},{3}); cast_shadows={4}; secondary_shadow_owner_retained=true",
				TestCase.Name, TestCase.LightPosition.x, TestCase.LightPosition.y, TestCase.LightPosition.z, TestCase.CastShadows);
			return true;
		}

		bool PreparePointShadowImageCase()
		{
			const PointShadowImageCase& TestCase = PointShadowImageCases[m_PointShadowImageCase];
			const auto Point = m_PointShadowImageLight.GetPointLight();
			const auto Transform = m_PointShadowImageLight.GetTransform();
			if (!Point || !*Point || !Transform)
			{
				Fail("Could not update the point-shadow image test light");
				return false;
			}
			PulseForge::PointLightComponent UpdatedPoint = **Point;
			UpdatedPoint.CastShadows = TestCase.CastShadows;
			if (!m_PointShadowImageLight.SetPointLight(UpdatedPoint))
			{
				Fail("Point-shadow image test light failed component validation");
				return false;
			}
			PulseForge::TransformComponent UpdatedTransform = *Transform;
			UpdatedTransform.Translation = TestCase.LightPosition;
			UpdatedTransform.Rotation = TestCase.PointLightRotated
											? glm::angleAxis(glm::radians(137.0f), glm::normalize(glm::vec3(1.0f, 2.0f, -0.75f)))
											: glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			if (!m_PointShadowImageLight.SetTransform(UpdatedTransform))
			{
				Fail("Point-shadow image test light transform was rejected");
				return false;
			}
			PulseForge::TransformComponent CubeTransform = *m_PointShadowImageCube.GetTransform();
			CubeTransform.Translation = glm::vec3(0.0f, 1.0f, 0.0f) + TestCase.CasterOffset;
			if (!m_PointShadowImageCube.SetTransform(CubeTransform))
			{
				Fail("Point-shadow image test caster transform was rejected");
				return false;
			}

			const bool UseCeiling = TestCase.UseCeiling;
			const auto FloorMesh = m_PointShadowImageFloor.GetMeshRenderer();
			const auto CeilingMesh = m_PointShadowImageCeiling.GetMeshRenderer();
			if (!FloorMesh || !CeilingMesh)
			{
				Fail("Could not resolve point-shadow image receiver mesh state");
				return false;
			}
			if (!UseCeiling)
			{
				if (!*FloorMesh && !m_PointShadowImageFloor.SetMeshRenderer(m_PointShadowImageFloorMesh))
				{
					Fail("Could not restore the floor receiver");
					return false;
				}
				if (*CeilingMesh && !m_PointShadowImageCeiling.RemoveMeshRenderer())
				{
					Fail("Could not disable the ceiling receiver");
					return false;
				}
			}
			if (UseCeiling)
			{
				if (*FloorMesh && !m_PointShadowImageFloor.RemoveMeshRenderer())
				{
					Fail("Could not disable the floor for the negative-Y point-shadow image case");
					return false;
				}
				if (!*CeilingMesh)
				{
					if (!m_PointShadowImageCeiling.SetMeshRenderer(m_PointShadowImageFloorMesh))
					{
						Fail("Could not enable the ceiling receiver for the negative-Y image case");
						return false;
					}
				}
				PulseForge::TransformComponent CameraTransform = m_PointShadowImageCameraTransform;
				CameraTransform.Translation = { 12.0f, -5.0f, 0.0f };
				CameraTransform.Rotation = glm::angleAxis(std::numbers::pi_v<float> * 0.5f, glm::vec3(0.0f, 1.0f, 0.0f)) *
										   glm::angleAxis(0.588f, glm::vec3(1.0f, 0.0f, 0.0f));
				if (!m_PointShadowImageCamera.SetTransform(CameraTransform))
				{
					Fail("Could not aim the camera at the negative-Y face receiver");
					return false;
				}
			}
			else if (std::abs(TestCase.LightPosition.z) > 0.0f)
			{
				PulseForge::TransformComponent CameraTransform = m_PointShadowImageCameraTransform;
				CameraTransform.Translation = { 12.5f, 4.4f, 0.0f };
				CameraTransform.Rotation = glm::angleAxis(std::numbers::pi_v<float> * 0.5f, glm::vec3(0.0f, 1.0f, 0.0f)) *
										   glm::angleAxis(-0.276f, glm::vec3(1.0f, 0.0f, 0.0f));
				if (!m_PointShadowImageCamera.SetTransform(CameraTransform))
				{
					Fail("Could not orient the camera sideways for a Z-face shadow image");
					return false;
				}
			}
			else
			{
				if (!m_PointShadowImageCamera.SetTransform(m_PointShadowImageCameraTransform))
				{
					Fail("Could not restore the fixed camera for a horizontal/upward point-shadow case");
					return false;
				}
			}

			auto Snapshot =
				PulseForge::SceneRenderSnapshotBuilder::Build(m_PointShadowImageScene, m_PointShadowImageCamera.GetUUID(), 1.0f);
			if (!Snapshot)
			{
				Fail("Could not build point-shadow image camera reference: " + Snapshot.error().Message);
				return false;
			}
			m_PointShadowImageViewProjections[m_PointShadowImageCase] = Snapshot->ViewProjection;
			m_PointShadowImageCameraPositions[m_PointShadowImageCase] = Snapshot->CameraWorldPosition;
			const auto Prepared = m_SceneRenderer->PrepareScene(m_PointShadowImageScene, 1.0f);
			if (!Prepared)
			{
				Fail("Could not prepare point-shadow image case: " + Prepared.error().Message);
				return false;
			}
			m_ScenePrepared = true;
			PF_INFO("POINT_SHADOW_IMAGE_CASE={0}; light=({1},{2},{3}); cast_shadows={4}; rotation={5}", TestCase.Name,
				TestCase.LightPosition.x, TestCase.LightPosition.y, TestCase.LightPosition.z, TestCase.CastShadows,
				TestCase.PointLightRotated ? "rotated" : "identity");
			return true;
		}

		void CapturePointShadowImage()
		{
			if (!m_PointShadowImageOutputTarget->GetColorTexture())
			{
				Fail("Point-shadow output target has no readable color texture");
				return;
			}
			auto Readback = PulseForge::Application::Get().ReadTextureForValidation(*m_PointShadowImageOutputTarget->GetColorTexture());
			if (!Readback)
			{
				Fail("Could not read actual point-shadow output pixels: " + Readback.error().Message);
				return;
			}
			const PointShadowImageCase& TestCase = PointShadowImageCases[m_PointShadowImageCase];
			const std::filesystem::path FilePath = m_PointShadowImageDirectory / (std::string(TestCase.Name) + ".png");
			const int Written =
				stbi_write_png(FilePath.string().c_str(), static_cast<int>(Readback->Width), static_cast<int>(Readback->Height), 4,
					reinterpret_cast<const uint8_t*>(Readback->Pixels.data()), static_cast<int>(Readback->Width * 4));
			if (!Written)
			{
				Fail("Could not write rendered point-shadow PNG: " + FilePath.string());
				return;
			}
			m_PointShadowImageResults[m_PointShadowImageCase] = std::move(*Readback);
			PF_INFO("POINT_SHADOW_IMAGE_CAPTURE={0}; dimensions={1}x{2}; format=sRGB8; bytes={3}", FilePath.string(),
				m_PointShadowImageResults[m_PointShadowImageCase]->Width, m_PointShadowImageResults[m_PointShadowImageCase]->Height,
				m_PointShadowImageResults[m_PointShadowImageCase]->Pixels.size());
			if (m_PointShadowImageCase == 5 || m_PointShadowImageCase == 7)
			{
				for (uint32_t Face = 0; Face < 6; ++Face)
				{
					const PulseForge::Texture* FaceDepth = m_SceneRenderer->GetPointShadowFaceTextureForValidation(0, Face);
					if (!FaceDepth)
					{
						Fail("Point-shadow cubemap did not expose the selected slot face for diagnostics");
						return;
					}
					auto Depth = PulseForge::Application::Get().ReadTextureForValidation(*FaceDepth, Face);
					if (!Depth || Depth->Format != PulseForge::TextureFormat::Depth32Float)
					{
						Fail("Could not read actual D32 point-shadow face pixels: " +
							 (Depth ? std::string("readback returned a non-depth format") : Depth.error().Message));
						return;
					}
					std::vector<uint8_t> Grayscale(static_cast<size_t>(Depth->Width) * Depth->Height);
					size_t NonClearPixels = 0;
					float MinimumDepth = 1.0f;
					for (size_t Pixel = 0; Pixel < Grayscale.size(); ++Pixel)
					{
						float Value = 1.0f;
						std::memcpy(&Value, Depth->Pixels.data() + Pixel * sizeof(float), sizeof(float));
						Value = std::isfinite(Value) ? std::clamp(Value, 0.0f, 1.0f) : 1.0f;
						MinimumDepth = (std::min)(MinimumDepth, Value);
						if (Value < 0.99999f) ++NonClearPixels;
						Grayscale[Pixel] = static_cast<uint8_t>(std::lround(Value * 255.0f));
					}
					const std::string FaceLabel = m_PointShadowImageCase == 5 ? "positive_z_light" : "negative_z_light";
					const std::filesystem::path FacePath =
						m_PointShadowImageDirectory / (FaceLabel + "_cubeface_" + std::to_string(Face) + ".png");
					if (!stbi_write_png(FacePath.string().c_str(), static_cast<int>(Depth->Width), static_cast<int>(Depth->Height), 1,
							Grayscale.data(), static_cast<int>(Depth->Width)))
					{
						Fail("Could not write point-shadow depth-face PNG: " + FacePath.string());
						return;
					}
					m_PointShadowCubemapDepths[m_PointShadowImageCase == 5 ? 0 : 1][Face] = std::move(*Depth);
					PF_INFO("POINT_SHADOW_FACE_CAPTURE={0}; face={1}; non_clear_pixels={2}; minimum_radial_depth={3:.5f}",
						FacePath.string(), Face, NonClearPixels, MinimumDepth);
				}
			}
		}

		void CapturePointShadowOverviewImage()
		{
			const PulseForge::Texture* Color = m_PointShadowOverviewOutputTarget->GetColorTexture();
			if (!Color)
			{
				Fail("Startup-scene point-shadow output target has no readable color texture");
				return;
			}
			auto Readback = PulseForge::Application::Get().ReadTextureForValidation(*Color);
			if (!Readback)
			{
				Fail("Could not read the startup-scene point-shadow output: " + Readback.error().Message);
				return;
			}
			const PointShadowOverviewImageCase& TestCase = PointShadowOverviewImageCases[m_PointShadowOverviewImageCase];
			const std::filesystem::path FilePath = m_PointShadowImageDirectory / (std::string(TestCase.Name) + ".png");
			if (!stbi_write_png(FilePath.string().c_str(), static_cast<int>(Readback->Width), static_cast<int>(Readback->Height), 4,
					reinterpret_cast<const uint8_t*>(Readback->Pixels.data()), static_cast<int>(Readback->Width * 4)))
			{
				Fail("Could not save the startup-scene point-shadow image: " + FilePath.string());
				return;
			}
			PF_INFO("POINT_SHADOW_OVERVIEW_CAPTURE={0}; dimensions={1}x{2}; format=sRGB8", FilePath.string(), Readback->Width,
				Readback->Height);
		}

		bool ValidatePointShadowImageResults()
		{
			bool AllDirectionsPass = true;
			for (size_t Pair = 0; Pair < 11; ++Pair)
			{
				const size_t OffIndex = Pair * 2;
				const size_t OnIndex = OffIndex + 1;
				if (!m_PointShadowImageResults[OffIndex] || !m_PointShadowImageResults[OnIndex])
				{
					AllDirectionsPass = false;
					continue;
				}
				const PointShadowImageCase& TestCase = PointShadowImageCases[OnIndex];
				if (!SegmentIntersectsShadowCaster(TestCase.LightPosition, TestCase.ExpectedReceiver, TestCase.CasterOffset) ||
					SegmentIntersectsShadowCaster(TestCase.LightPosition, TestCase.ControlReceiver, TestCase.CasterOffset))
				{
					PF_ERROR("POINT_SHADOW_ORACLE_SETUP_ERROR={0}", TestCase.Name);
					AllDirectionsPass = false;
					continue;
				}
				const PointShadowImageMetrics Metrics = MeasurePointShadowImage(*m_PointShadowImageResults[OffIndex],
					*m_PointShadowImageResults[OnIndex], m_PointShadowImageViewProjections[OnIndex],
					m_PointShadowImageCameraPositions[OnIndex], TestCase.LightPosition, TestCase.ExpectedReceiver.y, TestCase.CasterOffset);
				const double ShadowDelta = Metrics.ExpectedOff - Metrics.ExpectedOn;
				const double ControlDelta = Metrics.ControlOff - Metrics.ControlOn;
				PF_INFO("POINT_SHADOW_IMAGE_ORACLE={0}; expected_sample=({1},{2},{3}); visible_floor_pixels_expected/control=({4},{5}); "
						"mean_luma_expected_off_on=({6:.4f},{7:.4f}); mean_luma_control_off_on=({8:.4f},{9:.4f}); expected_delta={10:.4f}; "
						"control_delta={11:.4f}; changed_pixels={12}; changed_expected/other=({13},{14}); "
						"changed_centroid_world_xz=({15:.3f},{16:.3f}); mean_changed_luma={17:.4f}",
					TestCase.Name, TestCase.ExpectedReceiver.x, TestCase.ExpectedReceiver.y, TestCase.ExpectedReceiver.z,
					Metrics.ExpectedPixelCount, Metrics.ControlPixelCount, Metrics.ExpectedOff, Metrics.ExpectedOn, Metrics.ControlOff,
					Metrics.ControlOn, ShadowDelta, ControlDelta, Metrics.ChangedPixelCount, Metrics.ChangedExpectedPixelCount,
					Metrics.ChangedOtherPixelCount, Metrics.ChangedWorldX, Metrics.ChangedWorldZ, Metrics.ChangedMean);
				if (Metrics.ExpectedPixelCount < 20 || Metrics.ControlPixelCount < 20 || Metrics.ExpectedOff < 0.025 ||
					Metrics.ControlOff < 0.025 || ShadowDelta < 0.025 || ShadowDelta <= ControlDelta + 0.01)
				{
					AllDirectionsPass = false;
				}
				if (Pair == 2 || Pair == 3)
				{
					const size_t DepthSet = Pair == 2 ? 0 : 1;
					const PointShadowDepthProbe Probe = ProbePointShadowDepth(
						m_PointShadowCubemapDepths[DepthSet], TestCase.LightPosition, TestCase.ExpectedReceiver, 10.0f);
					PF_INFO("POINT_SHADOW_DEPTH_PROBE={0}; rendered_depth_face={1}; texel=({2},{3}); receiver_radial_depth={4:.5f}; "
							"direct_uv_depth={5:.5f}; vertically_flipped_uv_depth={6:.5f}",
						TestCase.Name, Probe.RenderedFace, Probe.X, Probe.Y, Probe.ReceiverDepth, Probe.DirectDepth,
						Probe.VerticallyFlippedDepth);
				}
			}

			if (!m_PointShadowImageResults[1] || !m_PointShadowImageResults[22])
			{
				Fail("Point-light rotation-invariance images are missing");
				return false;
			}
			double TotalDifference = 0.0;
			uint8_t MaximumDifference = 0;
			for (size_t Index = 0; Index < m_PointShadowImageResults[1]->Pixels.size(); ++Index)
			{
				const uint8_t A = std::to_integer<uint8_t>(m_PointShadowImageResults[1]->Pixels[Index]);
				const uint8_t B = std::to_integer<uint8_t>(m_PointShadowImageResults[22]->Pixels[Index]);
				const uint8_t Difference = static_cast<uint8_t>(std::abs(static_cast<int>(A) - static_cast<int>(B)));
				TotalDifference += Difference;
				MaximumDifference = (std::max)(MaximumDifference, Difference);
			}
			const double MeanDifference = TotalDifference / static_cast<double>(m_PointShadowImageResults[1]->Pixels.size());
			PF_INFO("POINT_LIGHT_ROTATION_PIXEL_CHECK=identity_vs_137deg; mean_channel_delta={0:.6f}; max_channel_delta={1}",
				MeanDifference, MaximumDifference);
			if (MaximumDifference != 0)
			{
				AllDirectionsPass = false;
			}
			if (!AllDirectionsPass) Fail("Rendered point-shadow image/analytic checks did not agree for every cube-face direction");
			return AllDirectionsPass;
		}

		void RenderPointShadowImageFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_PointShadowImageCapturePending) return;
			auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_PointShadowImageOutputTarget);
			const PointShadowImageCase& TestCase = PointShadowImageCases[m_PointShadowImageCase];
			if (!Rendered || Rendered->GeometryDrawCount != 2 || Rendered->PointShadowPassCount != (TestCase.CastShadows ? 6u : 0u) ||
				Rendered->PointShadowCasterDrawCount != (TestCase.CastShadows ? 12u : 0u))
			{
				Fail(Rendered ? "Point-shadow image case did not render the expected six depth faces and two casters"
							  : "Point-shadow image scene render failed: " + Rendered.error().Message);
				return;
			}
			m_PointShadowImageCapturePending = true;
		}

		void RenderPointShadowOverviewImageFrame()
		{
			if (!m_Succeeded || !m_ScenePrepared || m_PointShadowOverviewCapturePending) return;
			auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_PointShadowOverviewOutputTarget);
			const PointShadowOverviewImageCase& TestCase = PointShadowOverviewImageCases[m_PointShadowOverviewImageCase];
			const size_t ExpectedPasses = TestCase.CastShadows ? 12u : 6u;
			if (!Rendered || Rendered->GeometryDrawCount == 0 || Rendered->PointShadowPassCount != ExpectedPasses ||
				Rendered->PointShadowCasterDrawCount != ExpectedPasses * Rendered->GeometryDrawCount)
			{
				Fail(Rendered ? "Startup-scene capture did not render the expected independent point-shadow passes"
							  : "Startup-scene point-shadow render failed: " + Rendered.error().Message);
				return;
			}
			PF_INFO("POINT_SHADOW_OVERVIEW_DRAWS={0}; point_shadow_face_passes={1}; caster_draws={2}; spotlight_passes={3}", TestCase.Name,
				Rendered->PointShadowPassCount, Rendered->PointShadowCasterDrawCount, Rendered->SpotlightShadowPassCount);
			m_PointShadowOverviewCapturePending = true;
		}
		Scene& m_Scene;
		SceneRenderer* m_SceneRenderer = nullptr;
		Scene m_PointShadowImageScene;
		std::optional<AmbientOcclusionSettings> m_SavedAmbientOcclusionSettings;
		std::optional<ToneMappingSettings> m_SavedToneMappingSettings;
		RenderTargetHandle m_PointShadowImageOutputTarget;
		RenderTargetHandle m_PointShadowOverviewOutputTarget;
		Entity m_PointShadowOverviewPrimary;
		Entity m_PointShadowOverviewSecondary;
		Entity m_PointShadowImageCamera;
		Entity m_PointShadowImageFloor;
		Entity m_PointShadowImageCeiling;
		Entity m_PointShadowImageCube;
		Entity m_PointShadowImageLight;
		TransformComponent m_PointShadowImageCameraTransform;
		TransformComponent m_PointShadowImageCeilingTransform;
		MeshRendererComponent m_PointShadowImageFloorMesh;
		std::array<glm::mat4, PointShadowImageCases.size()> m_PointShadowImageViewProjections{};
		std::array<glm::vec3, PointShadowImageCases.size()> m_PointShadowImageCameraPositions{};
		std::array<std::optional<TextureReadback>, PointShadowImageCases.size()> m_PointShadowImageResults{};
		std::array<std::array<std::optional<TextureReadback>, 6>, 2> m_PointShadowCubemapDepths{};
		std::filesystem::path m_PointShadowImageDirectory;
		size_t m_PointShadowImageCase = 0;
		size_t m_PointShadowOverviewImageCase = 0;
		std::string m_Error;
		bool m_Succeeded = true;
		bool m_ScenePrepared = false;
		bool m_PointShadowImageCapturePending = false;
		bool m_PointShadowImageValidationComplete = false;
		bool m_PointShadowOverviewCapturePending = false;
		bool m_PointShadowOverviewSettingsRestored = false;
		bool m_CapturingPointShadowOverview = false;
		bool m_AmbientOcclusionChanged = false;
		bool m_ToneMappingChanged = false;
		bool m_Complete = false;
	};

	PointShadowImageValidation::PointShadowImageValidation() = default;
	PointShadowImageValidation::~PointShadowImageValidation() = default;
	PointShadowImageValidation::PointShadowImageValidation(PointShadowImageValidation&&) noexcept = default;
	PointShadowImageValidation& PointShadowImageValidation::operator=(PointShadowImageValidation&&) noexcept = default;

	void PointShadowImageValidation::Initialize(Scene& StartupScene, SceneRenderer& Renderer, std::filesystem::path OutputDirectory)
	{
		m_Impl = std::make_unique<Impl>(StartupScene, Renderer, std::move(OutputDirectory));
		m_Impl->Initialize();
	}

	std::expected<bool, std::string> PointShadowImageValidation::Update(std::chrono::steady_clock::time_point StartedAt)
	{
		if (!m_Impl) return std::unexpected("Point-shadow image validation was not initialized");
		return m_Impl->Update(StartedAt);
	}

	std::expected<void, std::string> PointShadowImageValidation::Render()
	{
		if (!m_Impl) return std::unexpected("Point-shadow image validation was not initialized");
		return m_Impl->Render();
	}

	std::expected<void, std::string> PointShadowImageValidation::Release()
	{
		if (!m_Impl) return {};
		return m_Impl->Release();
	}

	bool PointShadowImageValidation::IsComplete() const noexcept
	{
		return m_Impl && m_Impl->IsComplete();
	}
} // namespace PulseForge::GpuValidation
