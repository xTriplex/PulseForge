#pragma once

#include "Core/Core.h"
#include "Renderer/Graphics.h"
#include "Renderer/Binding.h"
#include "Renderer/EnvironmentLightingCache.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/DepthCubemap.h"
#include "Renderer/DirectionalShadowMath.h"
#include "Renderer/SpotlightShadowMath.h"
#include "Renderer/PointLightShadowMath.h"
#include "Renderer/ScreenSpaceAmbientOcclusion.h"
#include "Renderer/ToneMapping.h"
#include "Renderer/LocalLighting.h"
#include "Scene/SceneRenderSnapshot.h"

#include <expected>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace PulseForge
{
	class Application;
	class MaterialAssetCache;
	class MeshAssetCache;
	class Project;
	class Scene;
	class TextureAssetCache;

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

	// Runtime renderer for opaque direct-lit metallic/roughness materials. The Application and unmoved Project must outlive it.
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
		[[nodiscard]] const ToneMappingSettings& GetToneMappingSettings() const noexcept { return m_ToneMappingSettings; }
		[[nodiscard]] bool SetAmbientOcclusionSettings(const AmbientOcclusionSettings& Settings) noexcept;
		[[nodiscard]] const AmbientOcclusionSettings& GetAmbientOcclusionSettings() const noexcept { return m_AmbientOcclusionSettings; }
		[[nodiscard]] bool IsEnvironmentLightingPending() const noexcept { return m_EnvironmentLightingPending; }
		[[nodiscard]] bool HasPreparedEnvironmentLighting() const noexcept { return m_PreparedEnvironmentReady; }

	private:
		SceneRenderer(Application& Runtime, const Project& SourceProject);
		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(
			const std::filesystem::path& CompiledShaderDirectory);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureMaterialBindings(
			const AssetID& MaterialAsset,
			const std::optional<AssetID>& EnvironmentAsset,
			const EnvironmentLightingTextures& EnvironmentTextures);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePipeline(ColorTargetFormat ColorFormat);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureBackgroundPipeline(ColorTargetFormat ColorFormat);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureHdrSceneTarget(uint32_t Width, uint32_t Height);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureToneMappingResources();
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureToneMappingPipeline(
			ColorTargetFormat ColorFormat,
			bool DepthAttachmentEnabled);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureToneMappingBindings(const Texture& HdrTexture, uint64_t Generation);
		[[nodiscard]] std::expected<size_t, SceneRendererError> ToneMapToOutput(
			const Texture& HdrTexture,
			uint64_t Generation,
			ColorTargetFormat DestinationFormat,
			bool DepthAttachmentEnabled);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureShadowPipeline(float DepthBias);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureSpotShadowResources();
		[[nodiscard]] std::expected<void, SceneRendererError> EnsurePointShadowResources();
		[[nodiscard]] std::expected<void, SceneRendererError> PrepareLocalLighting();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderSpotlightShadows();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderPointLightShadows();
		[[nodiscard]] std::expected<void, SceneRendererError> RenderShadowCascades();
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureAmbientOcclusionResources(uint32_t Width, uint32_t Height);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureAmbientOcclusionPipelines();
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

		struct alignas(16) MaterialConstants
		{
			float BaseColor[4];
			float Surface[4];
		};
		static_assert(sizeof(MaterialConstants) == 32);

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

		struct alignas(16) AmbientOcclusionConstants
		{
			glm::mat4 InverseProjection{ 1.0f };
			glm::mat4 Projection{ 1.0f };
			glm::vec4 OutputSize{};
			glm::vec4 HalfSize{};
			glm::vec4 Parameters{};
			AmbientOcclusionKernel Kernel;
		};
		static_assert(sizeof(AmbientOcclusionConstants) == 688);

		struct alignas(16) AmbientOcclusionBlurConstants
		{
			glm::mat4 InverseProjection{ 1.0f };
			glm::vec4 OutputSize{};
			glm::vec4 HalfSize{};
			glm::vec4 Direction{};
		};
		static_assert(sizeof(AmbientOcclusionBlurConstants) == 112);

		struct alignas(16) AmbientOcclusionObjectConstants
		{
			glm::mat4 ViewProjectionModel{ 1.0f };
			glm::mat4 ViewNormalTransform{ 1.0f };
		};
		static_assert(sizeof(AmbientOcclusionObjectConstants) == 128);

		struct alignas(16) ToneMappingConstants
		{
			float ExposureEV = 0.0f;
			uint32_t EncodeSrgbForUnorm = 0;
			std::array<uint32_t, 2> Padding{};
		};
		static_assert(sizeof(ToneMappingConstants) == 16);

		Application& m_Runtime;
		const Project& m_Project;
		std::unique_ptr<MeshAssetCache> m_MeshAssetCache;
		std::unique_ptr<TextureAssetCache> m_TextureAssetCache;
		std::unique_ptr<MaterialAssetCache> m_MaterialAssetCache;
		std::unique_ptr<EnvironmentLightingCache> m_EnvironmentLightingCache;
		ShaderHandle m_VertexShader;
		ShaderHandle m_FragmentShader;
		ShaderHandle m_BackgroundVertexShader;
		ShaderHandle m_BackgroundFragmentShader;
		ShaderHandle m_ShadowVertexShader;
		ShaderHandle m_PointShadowVertexShader;
		ShaderHandle m_PointShadowFragmentShader;
		ShaderHandle m_AmbientOcclusionPrepassVertexShader;
		ShaderHandle m_AmbientOcclusionPrepassFragmentShader;
		ShaderHandle m_AmbientOcclusionVertexShader;
		ShaderHandle m_AmbientOcclusionFragmentShader;
		ShaderHandle m_AmbientOcclusionBlurVertexShader;
		ShaderHandle m_AmbientOcclusionBlurFragmentShader;
		ShaderHandle m_ToneMappingVertexShader;
		ShaderHandle m_ToneMappingFragmentShader;
		SamplerHandle m_Sampler;
		SamplerHandle m_EnvironmentSampler;
		SamplerHandle m_ShadowSampler;
		SamplerHandle m_AmbientOcclusionPointSampler;
		SamplerHandle m_AmbientOcclusionLinearSampler;
		SamplerHandle m_ToneMappingSampler;
		BindingLayoutHandle m_BindingLayout;
		BindingLayoutHandle m_ShadowBindingLayout;
		BindingLayoutHandle m_SpotShadowBindingLayout;
		BindingLayoutHandle m_PointShadowBindingLayout;
		BindingLayoutHandle m_ShadowObjectBindingLayout;
		BindingLayoutHandle m_PointShadowObjectBindingLayout;
		BindingLayoutHandle m_AmbientOcclusionFinalBindingLayout;
		BindingLayoutHandle m_AmbientOcclusionEvaluationBindingLayout;
		BindingLayoutHandle m_AmbientOcclusionBlurBindingLayout;
		BindingLayoutHandle m_AmbientOcclusionPrepassBindingLayout;
		BindingLayoutHandle m_ToneMappingBindingLayout;
		BindingSetHandle m_ShadowBindingSet;
		BindingSetHandle m_SpotShadowBindingSet;
		BindingSetHandle m_PointShadowBindingSet;
		BindingSetHandle m_ShadowObjectBindingSet;
		BindingSetHandle m_PointShadowObjectBindingSet;
		BindingSetHandle m_AmbientOcclusionFinalBindingSet;
		BindingSetHandle m_AmbientOcclusionFallbackBindingSet;
		BindingSetHandle m_AmbientOcclusionEvaluationBindingSet;
		std::array<BindingSetHandle, 2> m_AmbientOcclusionBlurBindingSets;
		BindingSetHandle m_AmbientOcclusionPrepassBindingSet;
		BindingSetHandle m_ToneMappingBindingSet;
		std::unordered_map<int32_t, GraphicsPipelineHandle> m_ShadowPipelines;
		GraphicsPipelineHandle m_PointShadowPipeline;
		BufferHandle m_ObjectConstantsBuffer;
		BufferHandle m_ShadowObjectConstantsBuffer;
		BufferHandle m_PointShadowObjectConstantsBuffer;
		BufferHandle m_FrameConstantsBuffer;
		BufferHandle m_LocalLightingConstantsBuffer;
		BufferHandle m_BackgroundTriangleBuffer;
		BufferHandle m_AmbientOcclusionParametersBuffer;
		BufferHandle m_AmbientOcclusionBlurParametersBuffer;
		BufferHandle m_AmbientOcclusionObjectConstantsBuffer;
		BufferHandle m_ToneMappingConstantsBuffer;
		BufferHandle m_FallbackMaterialConstantsBuffer;
		TextureHandle m_FallbackBaseColorTexture;
		TextureHandle m_FallbackAmbientOcclusionTexture;
		TextureHandle m_FallbackPointShadowTexture;
		std::array<RenderTargetHandle, 4> m_ShadowTargets;
		std::array<RenderTargetHandle, MaxSpotlightShadowMapCount> m_SpotShadowTargets;
		std::array<DepthCubemapHandle, MaxPointLightShadowCount> m_PointShadowCubemaps;
		RenderTargetHandle m_HdrSceneTarget;
		RenderTargetHandle m_AmbientOcclusionPrepassTarget;
		RenderTargetHandle m_AmbientOcclusionRawTarget;
		std::array<RenderTargetHandle, 2> m_AmbientOcclusionBlurTargets;
		EnvironmentLightingTextures m_FallbackEnvironmentTextures;
		std::unordered_map<std::string, MaterialBindingResources> m_MaterialBindings;
		std::unordered_map<std::string, BindingSetHandle> m_EnvironmentBindingSets;
		std::unordered_map<ColorTargetFormat, GraphicsPipelineHandle> m_Pipelines;
		std::unordered_map<ColorTargetFormat, GraphicsPipelineHandle> m_BackgroundPipelines;
		std::map<std::pair<ColorTargetFormat, bool>, GraphicsPipelineHandle> m_ToneMappingPipelines;
		GraphicsPipelineHandle m_AmbientOcclusionPrepassPipeline;
		GraphicsPipelineHandle m_AmbientOcclusionPipeline;
		GraphicsPipelineHandle m_AmbientOcclusionBlurPipeline;
		std::optional<VertexLayoutDesc> m_PipelineVertexLayout;
		std::optional<SceneRenderSnapshot> m_PreparedSnapshot;
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
		AmbientOcclusionSettings m_AmbientOcclusionSettings;
		ToneMappingSettings m_ToneMappingSettings;
		ToneMappingConstants m_UploadedToneMappingConstants{};
		uint64_t m_HdrSceneTargetGeneration = 0;
		uint64_t m_ToneMappingBindingGeneration = 0;
		bool m_HasUploadedToneMappingConstants = false;
		AmbientOcclusionKernel m_AmbientOcclusionKernel;
		uint32_t m_AmbientOcclusionWidth = 0;
		uint32_t m_AmbientOcclusionHeight = 0;
		bool m_AmbientOcclusionFrameAvailable = false;
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
