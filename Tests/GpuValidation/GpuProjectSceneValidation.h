#pragma once

#include "Assets/MaterialAsset.h"
#include "Renderer/RenderTarget.h"
#include "Scene/Components/EnvironmentLightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Entity.h"
#include "Scene/UUID.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	class Project;
	class Scene;
	class SceneRenderer;
} // namespace PulseForge

namespace PulseForge::GpuValidation
{
	enum class ProjectSceneValidationMode : uint8_t
	{
		PbrMaterials,
		Transparency
	};

	// Owns project-scene image captures and their multi-frame comparisons. The
	// project, startup scene, and SceneRenderer are borrowed from the harness and
	// must outlive this owner.
	class GpuProjectSceneValidation final
	{
	public:
		GpuProjectSceneValidation(Project& SourceProject, Scene& SourceScene, SceneRenderer& Renderer, ProjectSceneValidationMode Mode,
			std::filesystem::path OutputRoot, std::chrono::steady_clock::time_point StartedAt) noexcept;

		void Initialize();
		void Update();
		void Render();
		void Reset() noexcept;

		[[nodiscard]] bool IsComplete() const noexcept;
		[[nodiscard]] bool Succeeded() const noexcept;
		[[nodiscard]] std::optional<std::string> TakeFailure();
		[[nodiscard]] std::string_view CompletionSummary() const noexcept;
		[[nodiscard]] const std::filesystem::path& CapturePath() const noexcept;

	private:
		enum class PbrMaterialCaptureStage : uint8_t
		{
			AssignedBaseline,
			DefaultFloorRemoved,
			DefaultFloorRestored,
			TargetMaterialCleared,
			TargetMaterialRestored,
			TargetMaterialClearedAgain,
			TargetMaterialRestoredAgain,
			EnvironmentRemoved,
			EnvironmentFallbackRefreshed,
			EnvironmentRestored,
			AssetMaterialRefreshed,
			AssetMaterialRestored,
			AssetsUnchangedRefreshed,
			TextureSourceChanged,
			TextureSourceRestored,
			MeshSourceChanged,
			MeshSourceRestored,
			EnvironmentSourceChanged,
			EnvironmentSourceRestored,
			EnvironmentSourceMissing,
			EnvironmentSourceRepaired,
			Complete
		};

		void InitializePbrMaterial();
		void InitializeTransparency();
		void UpdatePbrMaterial();
		void UpdateTransparency();
		void RenderPbrMaterial();
		void CapturePbrMaterialImage();
		bool AdvancePbrMaterialCapture();
		bool SavePbrTargetMaterialAndRefresh(const PulseForge::MaterialAssetDesc& Description, std::string_view Phase);
		bool RefreshPbrProjectAssets(std::string_view Phase);
		bool WriteChangedPbrTexture();
		bool RestorePbrTexture();
		bool ReplacePbrMeshWithAlternate();
		bool RestorePbrMesh();
		bool WriteChangedPbrEnvironment();
		bool RestorePbrEnvironment();
		void RenderTransparency();
		void CaptureTransparencyImage();
		void Fail(std::string Message);
		bool SetTransparencyBluePanelOpaqueControl();
		bool SetTransparencySpotShadows(bool Enabled);
		bool CreateTransparencyPointLight();
		bool SetTransparencyPointShadows(bool Enabled);
		bool PrepareTransparencyDirectionalShadowTest(bool Enabled);
		bool ValidateTransparencyShadowDifference();
		bool ValidateMaskedShadowImageDifference(const std::vector<std::byte>& ShadowsOff, const std::vector<std::byte>& ShadowsOn,
			const char* ShadowType, uint32_t MinX, uint32_t MinY, uint32_t MaxX, uint32_t MaxY);
		bool ValidateTransparencyPointShadowDifference();
		bool ValidateTransparencyDirectionalShadowDifference();
		bool ValidateTransparencyBlendDifference();
		bool ValidateTransparencyLightDifference();

		Project& m_Project;
		Scene& m_Scene;
		SceneRenderer& m_SceneRenderer;
		ProjectSceneValidationMode m_Mode;
		std::filesystem::path m_OutputRoot;
		std::chrono::steady_clock::time_point m_StartedAt;
		std::unique_ptr<RenderTarget> m_PbrMaterialOutputTarget;
		std::filesystem::path m_PbrMaterialImagePath;
		Entity m_PbrFloorEntity;
		Entity m_PbrTargetEntity;
		Entity m_PbrUnaffectedEntity;
		Entity m_PbrEnvironmentEntity;
		MeshRendererComponent m_PbrFloorComponent;
		MeshRendererComponent m_PbrTargetComponent;
		PulseForge::MaterialAssetDesc m_PbrOriginalMaterial;
		std::filesystem::path m_PbrTargetMaterialPath;
		std::filesystem::path m_PbrTexturePath;
		std::filesystem::path m_PbrOriginalTextureBackupPath;
		std::filesystem::path m_PbrMeshPath;
		std::filesystem::path m_PbrOriginalMeshBackupPath;
		std::filesystem::path m_PbrAlternateMeshPath;
		std::filesystem::path m_PbrEnvironmentPath;
		std::filesystem::path m_PbrOriginalEnvironmentBackupPath;
		EnvironmentLightComponent m_PbrEnvironmentComponent;
		std::vector<std::byte> m_PbrBaselinePixels;
		std::vector<std::byte> m_PbrEnvironmentFallbackPixels;
		PbrMaterialCaptureStage m_PbrMaterialStage = PbrMaterialCaptureStage::AssignedBaseline;
		std::filesystem::path m_TransparencyImagePath;
		std::unique_ptr<RenderTarget> m_TransparencyOutputTarget;
		Entity m_TransparencyDirectionalLight;
		Entity m_TransparencySpotLight;
		Entity m_TransparencyPointLight;
		Entity m_TransparencyBlueGlassPanel;
		UUID m_TransparencyBlueOpaqueControlMaterial;
		std::vector<std::byte> m_TransparencyShadowOffPixels;
		std::vector<std::byte> m_TransparencyShadowOnPixels;
		std::vector<std::byte> m_TransparencyNoSpotPixels;
		std::vector<std::byte> m_TransparencyPointShadowOffPixels;
		std::vector<std::byte> m_TransparencyPointShadowOnPixels;
		std::vector<std::byte> m_TransparencyDirectionalShadowOffPixels;
		std::vector<std::byte> m_TransparencyDirectionalShadowOnPixels;
		std::vector<std::byte> m_TransparencyOpaqueControlPixels;
		float m_TransparencySpotIntensity = 0.0f;
		size_t m_PbrMaterialGeometryDrawCount = 0;
		size_t m_TransparencyGeometryDrawCount = 0;
		size_t m_TransparencyCaptureStage = 0;
		bool m_ScenePrepared = false;
		bool m_PbrMaterialCapturePending = false;
		bool m_TransparencyCapturePending = false;
		bool m_TransparencyCaptureComplete = false;
		bool m_Succeeded = true;
		std::optional<std::string> m_Failure;
	};
} // namespace PulseForge::GpuValidation
