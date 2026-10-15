#pragma once

#include "Core/Core.h"
#include "Renderer/Graphics.h"
#include "Renderer/Binding.h"
#include "Renderer/EnvironmentLightingTextures.h"
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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace PulseForge
{
	class Application;
	struct MaterialAssetDesc;
	class Project;
	class Scene;

	enum class SceneRendererErrorCode : uint8_t
	{
		ShaderLoadFailed,
		ResourceCreationFailed,
		SnapshotBuildFailed,
		AssetLoadFailed,
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
		PointShadowSlotOwners PointShadowSlotOwners{};
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
		SpotlightShadowSlotOwners SpotlightShadowSlotOwners{};
		size_t PointShadowPassCount = 0;
		size_t PointShadowCasterDrawCount = 0;
		uint64_t PointShadowResourceGeneration = 0;
		PointShadowSlotOwners PointShadowSlotOwners{};
	};

	struct AmbientOcclusionSettings
	{
		bool Enabled = true;
		float Radius = 0.65f;
		float Bias = 0.025f;
		float Strength = 1.15f;
	};

	// Runtime renderer for opaque, masked, and sorted alpha-blended PBR materials. The Application and unmoved Project must outlive it.
	// Call PrepareScene before BeginFrame and RenderPreparedScene only during an active renderer frame.
	class PULSEFORGE_API SceneRenderer final
	{
	public:
		static constexpr size_t MaxLocalLights = MaxLocalLightCount;
		[[nodiscard]] static std::expected<std::unique_ptr<SceneRenderer>, SceneRendererError> Create(
			Application& Runtime,
			const Project& SourceProject,
			const std::filesystem::path& CompiledShaderDirectory);

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
		// Begins, clears, renders to, and ends Target during the active Application frame.
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
		[[nodiscard]] bool IsEnvironmentLightingPending() const noexcept { return m_EnvironmentLightingPending; }
		[[nodiscard]] bool HasPreparedEnvironmentLighting() const noexcept { return m_PreparedEnvironmentReady; }
		// Narrow diagnostic access used by the opt-in GPU validation harness to read actual cubemap face depth.
		[[nodiscard]] const Texture* GetPointShadowFaceTextureForValidation(size_t Slot, uint32_t Face) const noexcept;

	private:
		struct ProjectAssetCaches;
		class ToneMappingStage;
		class AmbientOcclusionPass;

		SceneRenderer(Application& Runtime, const Project& SourceProject);
		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(
			const std::filesystem::path& CompiledShaderDirectory);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureMaterialBindings(
			const AssetID& MaterialAsset,
			const std::optional<AssetID>& EnvironmentAsset,
			const EnvironmentLightingTextures& EnvironmentTextures);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePipeline(
			ColorTargetFormat ColorFormat,
			bool AlphaBlend = false,
			bool DoubleSided = false);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureBackgroundPipeline(ColorTargetFormat ColorFormat);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureHdrSceneTarget(uint32_t Width, uint32_t Height);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureShadowPipeline(float DepthBias, bool DoubleSided = false);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureSpotShadowResources();
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePointShadowResources();
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareLocalLighting();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderSpotlightShadows();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderPointLightShadows();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderShadowCascades();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderAmbientOcclusion(uint32_t Width, uint32_t Height);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureEnvironmentBindings(
			const AssetID& EnvironmentAsset,
			const EnvironmentLightingTextures& EnvironmentTextures,
			bool IsReady);
		[[nodiscard]] std::expected<size_t, SceneRendererError> RenderPreparedSceneForFormat(
			ColorTargetFormat ColorFormat,
			uint32_t Width,
			uint32_t Height);
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareSnapshot(
			std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> Snapshot);

		struct MaterialBindingResources
		{
			BufferHandle MaterialConstantsBuffer;
			BindingSetHandle BindingSet;
		};
		[[nodiscard]] std::expected<const MaterialAssetDesc*, SceneRendererError> ResolveMaterialDescription(
			const SceneMeshInstance& Instance);
		[[nodiscard]] std::expected<const MaterialBindingResources*, SceneRendererError> ResolveMaterialBinding(
			const SceneMeshInstance& Instance);

		struct alignas(16) MaterialConstants
		{
			float BaseColor[4];
			float Surface[4];
			float Emissive[4];
			float Alpha[4];
		};
		static_assert(sizeof(MaterialConstants) == 64);

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

		struct SelectedLocalLight
		{
			LocalLightRelevance Relevance;
			LocalLightGpuData Data;
			UUID Entity;
			const SceneSpotLight* Spot = nullptr;
			const ScenePointLight* Point = nullptr;
		};

		Application& m_Runtime;
		const Project& m_Project;
		std::unique_ptr<ProjectAssetCaches> m_ProjectAssetCaches;
		std::unique_ptr<ToneMappingStage> m_ToneMappingStage;
		ShaderHandle m_VertexShader;
		ShaderHandle m_FragmentShader;
		ShaderHandle m_BackgroundVertexShader;
		ShaderHandle m_BackgroundFragmentShader;
		ShaderHandle m_ShadowVertexShader;
		ShaderHandle m_ShadowFragmentShader;
		ShaderHandle m_PointShadowVertexShader;
		ShaderHandle m_PointShadowFragmentShader;
		SamplerHandle m_Sampler;
		SamplerHandle m_EnvironmentSampler;
		SamplerHandle m_ShadowSampler;
		BindingLayoutHandle m_BindingLayout;
		BindingLayoutHandle m_ShadowBindingLayout;
		BindingLayoutHandle m_SpotShadowBindingLayout;
		BindingLayoutHandle m_PointShadowBindingLayout;
		BindingLayoutHandle m_ShadowObjectBindingLayout;
		BindingLayoutHandle m_PointShadowObjectBindingLayout;
		BindingSetHandle m_ShadowBindingSet;
		BindingSetHandle m_SpotShadowBindingSet;
		BindingSetHandle m_PointShadowBindingSet;
		BindingSetHandle m_ShadowObjectBindingSet;
		BindingSetHandle m_PointShadowObjectBindingSet;
		std::map<std::pair<int32_t, bool>, GraphicsPipelineHandle> m_ShadowPipelines;
		std::array<GraphicsPipelineHandle, 2> m_PointShadowPipelines;
		BufferHandle m_ObjectConstantsBuffer;
		BufferHandle m_ShadowObjectConstantsBuffer;
		BufferHandle m_PointShadowObjectConstantsBuffer;
		BufferHandle m_FrameConstantsBuffer;
		BufferHandle m_LocalLightingConstantsBuffer;
		BufferHandle m_BackgroundTriangleBuffer;
		BufferHandle m_FallbackMaterialConstantsBuffer;
		TextureHandle m_FallbackBaseColorTexture;
		TextureHandle m_FallbackMetallicRoughnessTexture;
		TextureHandle m_FallbackNormalTexture;
		TextureHandle m_FallbackEmissiveTexture;
		TextureHandle m_FallbackPointShadowTexture;
		std::array<RenderTargetHandle, 4> m_ShadowTargets;
		std::array<RenderTargetHandle, MaxSpotlightShadowMapCount> m_SpotShadowTargets;
		std::array<DepthCubemapHandle, MaxPointLightShadowCount> m_PointShadowCubemaps;
		RenderTargetHandle m_HdrSceneTarget;
		EnvironmentLightingTextures m_FallbackEnvironmentTextures;
		std::unordered_map<std::string, MaterialBindingResources> m_MaterialBindings;
		std::unordered_map<std::string, BindingSetHandle> m_EnvironmentBindingSets;
		std::map<std::tuple<ColorTargetFormat, bool, bool>, GraphicsPipelineHandle> m_Pipelines;
		std::unordered_map<ColorTargetFormat, GraphicsPipelineHandle> m_BackgroundPipelines;
		std::optional<VertexLayoutDesc> m_PipelineVertexLayout;
		std::optional<SceneRenderSnapshot> m_PreparedSnapshot;
		std::vector<const SceneMeshInstance*> m_TransparentDrawOrder;
		std::optional<DirectionalShadowCascadeSet> m_PreparedCascades;
		std::array<SelectedLocalLight, MaxLocalLightCount> m_SelectedLocalLights{};
		std::array<std::optional<UUID>, MaxSpotlightShadowMapCount> m_SpotShadowSlotOwners{};
		PointShadowSlotOwners m_PointShadowSlotOwners{};
		std::array<glm::mat4, MaxSpotlightShadowMapCount> m_PreparedSpotShadowMatrices{};
		std::array<glm::vec4, MaxSpotlightShadowMapCount> m_PreparedSpotShadowParameters{};
		std::array<std::array<glm::mat4, 6>, MaxPointLightShadowCount> m_PreparedPointShadowMatrices{};
		std::array<glm::vec4, MaxPointLightShadowCount> m_PreparedPointShadowParameters{};
		LocalLightingConstants m_PreparedLocalLighting{};
		size_t m_SelectedLocalLightCount = 0;
		const EnvironmentLightingTextures* m_PreparedEnvironmentTextures = nullptr;
		std::unique_ptr<AmbientOcclusionPass> m_AmbientOcclusionPass;
		uint64_t m_HdrSceneTargetGeneration = 0;
		bool m_EnvironmentLightingPending = false;
		bool m_PreparedEnvironmentReady = false;
		bool m_LoggedFirstSnapshotPreparation = false;
		bool m_LoggedLocalLightOverflow = false;
		bool m_LoggedSpotShadowOverflow = false;
		bool m_LoggedUnsupportedSpotShadowCone = false;
		bool m_LoggedPointShadowOverflow = false;
		size_t m_LastSpotShadowPassCount = 0;
		size_t m_LastSpotShadowCasterDrawCount = 0;
		size_t m_LastPointShadowPassCount = 0;
		size_t m_LastPointShadowCasterDrawCount = 0;
		uint64_t m_SpotShadowResourceGeneration = 0;
		uint64_t m_PointShadowResourceGeneration = 0;
	};
}
