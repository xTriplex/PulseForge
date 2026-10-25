#pragma once

#include "Assets/AssetPreparationService.h"
#include "Core/Core.h"
#include "Renderer/Graphics.h"
#include "Renderer/Binding.h"
#include "Renderer/EnvironmentLightingTextures.h"
#include "Renderer/RenderDevice.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/DepthCubemap.h"
#include "Renderer/DirectionalShadowMath.h"
#include "Renderer/SpotlightShadowMath.h"
#include "Renderer/PointLightShadowMath.h"
#include "Renderer/ToneMapping.h"
#include "Renderer/LocalLighting.h"
#include "Scene/SceneRenderSnapshot.h"

#include <expected>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace PulseForge
{
	struct MaterialAssetDesc;
	class Project;
	class Scene;
	class SceneRendererPipelineCache;
	class SceneRendererMaterialBindingCache;

	enum class SceneRendererErrorCode : uint8_t
	{
		ShaderLoadFailed,
		ResourceCreationFailed,
		SnapshotBuildFailed,
		AssetLoadFailed,
		AssetLoadPending,
		MissingMaterial,
		UnsupportedVertexLayout,
		DrawFailed
	};

	struct SceneRendererError
	{
		SceneRendererErrorCode Code;
		std::optional<UUID> Entity;
		std::optional<AssetID> Asset;
		std::string Message;
	};

	struct HdrSceneRenderResult
	{
		// Borrowed from SceneRenderer; valid until the next HDR render at a different extent or renderer destruction.
		const Texture* ColorTexture = nullptr;
		const Texture* DepthTexture = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
		size_t GeometryDrawCount = 0;
		size_t SpotlightShadowPassCount = 0;
		size_t SpotlightShadowCasterDrawCount = 0;
		bool EnvironmentBackgroundDrawn = false;
		uint64_t TargetGeneration = 0;
		uint64_t SpotlightShadowResourceGeneration = 0;
		size_t PointShadowPassCount = 0;
		size_t PointShadowCasterDrawCount = 0;
		uint64_t PointShadowResourceGeneration = 0;
		::PulseForge::PointShadowSlotOwners PointShadowSlotOwners{};
	};

	struct SceneRenderOutputResult
	{
		size_t GeometryDrawCount = 0;
		uint64_t HdrTargetGeneration = 0;
		ColorTargetFormat DestinationFormat = ColorTargetFormat::Swapchain;
		bool ShaderSrgbEncoded = false;
		size_t SpotlightShadowPassCount = 0;
		size_t SpotlightShadowCasterDrawCount = 0;
		uint64_t SpotlightShadowResourceGeneration = 0;
		// Diagnostic ownership snapshot for the completed frame, ordered by physical shadow-map slot.
		::PulseForge::SpotlightShadowSlotOwners SpotlightShadowSlotOwners{};
		size_t PointShadowPassCount = 0;
		size_t PointShadowCasterDrawCount = 0;
		uint64_t PointShadowResourceGeneration = 0;
		::PulseForge::PointShadowSlotOwners PointShadowSlotOwners{};
	};

	struct AmbientOcclusionSettings
	{
		bool Enabled = true;
		float Radius = 0.65f;
		float Bias = 0.025f;
		float Strength = 1.15f;
	};

	// Runtime renderer for opaque, masked, and sorted alpha-blended PBR materials. The device and unmoved Project must outlive it.
	// Call PrepareScene before the active frame and RenderPreparedScene only during a renderer frame.
	class PULSEFORGE_API SceneRenderer final
	{
	public:
		static constexpr size_t MaxLocalLights = MaxLocalLightCount;
		[[nodiscard]] static std::expected<std::unique_ptr<SceneRenderer>, SceneRendererError> Create(
			RenderDevice& Runtime, const Project& SourceProject, const std::filesystem::path& CompiledShaderDirectory);

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;
		~SceneRenderer();

		[[nodiscard]] std::expected<void, SceneRendererError> PrepareScene(
			const Scene& Source,
			UUID CameraEntity,
			float AspectRatio);
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareScene(
			const Scene& Source,
			float AspectRatio);
		// Prepares scene geometry with a transient view and explicit world-space camera position.
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareScene(
			const Scene& Source,
			const glm::mat4& ViewProjection,
			const glm::vec3& CameraWorldPosition);
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareScene(
			const Scene& Source,
			const glm::mat4& View,
			const glm::mat4& Projection,
			const glm::vec3& CameraWorldPosition,
			float NearClipPlane,
			float FarClipPlane);
		// Excludes editor-hidden entities and their render contributions without changing authored/runtime scene state.
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareSceneFiltered(
			const Scene& Source,
			const glm::mat4& View,
			const glm::mat4& Projection,
			const glm::vec3& CameraWorldPosition,
			float NearClipPlane,
			float FarClipPlane,
			std::span<const UUID> ExcludedEntities);
		[[nodiscard]] std::expected<size_t, SceneRendererError> RenderPreparedScene();
		// Begins, clears, renders to, and ends Target during the active renderer frame.
		[[nodiscard]] std::expected<size_t, SceneRendererError> RenderPreparedScene(
			const RenderTarget& Target,
			const RenderTargetClearValue& ClearValue = {});
		// Renders the prepared scene into a persistent RGBA16F+D32 target during an active frame.
		// A scene snapshot must have been prepared beforehand. The returned color texture is borrowed and
		// returned color/depth textures remain valid until this renderer next renders at a different extent or is destroyed.
		[[nodiscard]] std::expected<HdrSceneRenderResult, SceneRendererError> RenderPreparedSceneToHdr(
			uint32_t Width,
			uint32_t Height);
		// Renders once to the persistent HDR target, then tone maps to the active swapchain.
		[[nodiscard]] std::expected<SceneRenderOutputResult, SceneRendererError> RenderPreparedSceneToOutput();
		// Renders once to HDR, then tone maps into the supplied SDR offscreen target.
		[[nodiscard]] std::expected<SceneRenderOutputResult, SceneRendererError> RenderPreparedSceneToOutput(const RenderTarget& Target);
		[[nodiscard]] bool SetToneMappingSettings(const ToneMappingSettings& Settings) noexcept;
		[[nodiscard]] const ToneMappingSettings& GetToneMappingSettings() const noexcept;
		[[nodiscard]] bool SetAmbientOcclusionSettings(const AmbientOcclusionSettings& Settings) noexcept;
		[[nodiscard]] const AmbientOcclusionSettings& GetAmbientOcclusionSettings() const noexcept;
		// Clears cached material descriptions and binding sets after an explicit material-asset save.
		void RefreshMaterialAssets() noexcept;
		// Invalidates project asset data and dependent bindings at an update boundary before command recording.
		void RefreshProjectAssets() noexcept;
		[[nodiscard]] bool IsEnvironmentLightingPending() const noexcept { return m_EnvironmentLightingPending; }
		[[nodiscard]] bool IsAssetPreparationPending() const noexcept { return m_AssetPreparationPending; }
		[[nodiscard]] AssetPreparationServiceStats GetAssetPreparationStats() const noexcept;
		[[nodiscard]] bool HasPreparedEnvironmentLighting() const noexcept { return m_PreparedEnvironmentReady; }
		// Narrow diagnostic access used by the opt-in GPU validation harness to read actual cubemap face depth.
		[[nodiscard]] const Texture* GetPointShadowFaceTextureForValidation(size_t Slot, uint32_t Face) const noexcept;
		[[nodiscard]] const Texture* GetDirectionalShadowTextureForValidation(size_t Cascade) const noexcept;
		[[nodiscard]] const Texture* GetSpotlightShadowTextureForValidation(size_t Slot) const noexcept;
		[[nodiscard]] const Texture* GetAmbientOcclusionDepthTextureForValidation() const noexcept;

	private:
		struct ProjectAssetCaches;
		class ToneMappingStage;
		class AmbientOcclusionPass;
		class HdrTargetCache;
		class ShadowTargetCache;
		class LocalLightingState;
		class ShadowPass;

		SceneRenderer(RenderDevice& Runtime, const Project& SourceProject);
		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(
			const std::filesystem::path& CompiledShaderDirectory);
		[[nodiscard]] std::expected<void, SceneRendererError> RenderShadowCascades();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderAmbientOcclusion(uint32_t Width, uint32_t Height);
		[[nodiscard]] std::expected<size_t, SceneRendererError> RenderPreparedSceneForFormat(
			ColorTargetFormat ColorFormat,
			uint32_t Width,
			uint32_t Height);
		[[nodiscard]] std::expected<const MaterialAssetDesc*, SceneRendererError> ResolveMaterialDescription(
			const SceneMeshInstance& Instance);
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareSnapshot(
			std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> Snapshot);

		[[nodiscard]] std::expected<const BindingSet*, SceneRendererError> ResolveMaterialBinding(const SceneMeshInstance& Instance);

		struct alignas(16) ObjectConstants
		{
			glm::mat4 Model{ 1.0f };
			glm::mat4 ModelViewProjection{ 1.0f };
			glm::mat4 NormalTransform{ 1.0f };
		};
		static_assert(sizeof(ObjectConstants) == 192);

		struct alignas(16) FrameConstants
		{
			float CameraWorldPosition[4]{};
			float LightRayDirection[4]{ 0.0f, 0.0f, -1.0f, 0.0f };
			float LightColorIntensity[4]{};
			float EnvironmentInverseRotation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
			float EnvironmentParameters[4]{};
			glm::mat4 InverseViewProjection{ 1.0f };
			glm::mat4 View{ 1.0f };
			glm::mat4 Projection{ 1.0f };
			glm::vec4 OutputSize{ 1.0f };
			float CascadeSplitDepths[4]{};
			float ShadowParameters[4]{}; // enabled, receiver normal bias, PCF radius texels, max distance
			std::array<glm::mat4, 4> CascadeViewProjection{ glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
			std::array<glm::mat4, MaxSpotlightShadowMapCount> SpotShadowViewProjection{};
			std::array<glm::vec4, MaxSpotlightShadowMapCount> SpotShadowParameters{}; // depth bias, normal bias, PCF radius, enabled
			std::array<glm::vec4, MaxPointLightShadowCount> PointShadowParameters{}; // radial bias, normal bias, PCF radius, enabled
		};
		static_assert(sizeof(FrameConstants) == 928);

		struct alignas(16) PointShadowObjectConstants
		{
			glm::mat4 ViewProjection{ 1.0f };
			glm::vec4 LightPositionRange{ 0.0f, 0.0f, 0.0f, 1.0f };
			glm::mat4 Model{ 1.0f };
		};
		static_assert(sizeof(PointShadowObjectConstants) == 144);

		RenderDevice& m_Runtime;
		const Project& m_Project;
		std::unique_ptr<ProjectAssetCaches> m_ProjectAssetCaches;
		std::unique_ptr<SceneRendererPipelineCache> m_PipelineCache;
		std::unique_ptr<ToneMappingStage> m_ToneMappingStage;
		std::unique_ptr<HdrTargetCache> m_HdrTargetCache;
		std::unique_ptr<ShadowTargetCache> m_ShadowTargetCache;
		std::unique_ptr<LocalLightingState> m_LocalLightingState;
		SamplerHandle m_ShadowSampler;
		BindingLayoutHandle m_BindingLayout;
		BindingLayoutHandle m_ShadowBindingLayout;
		BindingLayoutHandle m_SpotShadowBindingLayout;
		BindingLayoutHandle m_PointShadowBindingLayout;
		BindingLayoutHandle m_ShadowObjectBindingLayout;
		BindingLayoutHandle m_PointShadowObjectBindingLayout;
		BindingSetHandle m_ShadowObjectBindingSet;
		BindingSetHandle m_PointShadowObjectBindingSet;
		BufferHandle m_ObjectConstantsBuffer;
		BufferHandle m_ShadowObjectConstantsBuffer;
		BufferHandle m_PointShadowObjectConstantsBuffer;
		BufferHandle m_FrameConstantsBuffer;
		BufferHandle m_LocalLightingConstantsBuffer;
		BufferHandle m_BackgroundTriangleBuffer;
		TextureHandle m_FallbackPointShadowTexture;
		EnvironmentLightingTextures m_FallbackEnvironmentTextures;
		std::optional<SceneRenderSnapshot> m_PreparedSnapshot;
		std::vector<const SceneMeshInstance*> m_TransparentDrawOrder;
		std::optional<DirectionalShadowCascadeSet> m_PreparedCascades;
		const EnvironmentLightingTextures* m_PreparedEnvironmentTextures = nullptr;
		std::unique_ptr<AmbientOcclusionPass> m_AmbientOcclusionPass;
		bool m_EnvironmentLightingPending = false;
		bool m_AssetPreparationPending = false;
		bool m_PreparedEnvironmentReady = false;
		bool m_LoggedFirstSnapshotPreparation = false;
		std::unique_ptr<SceneRendererMaterialBindingCache> m_MaterialBindingCache;
		std::unique_ptr<ShadowPass> m_ShadowPass;
	};
}
