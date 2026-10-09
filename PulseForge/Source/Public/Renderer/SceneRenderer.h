#pragma once

#include "Core/Core.h"
#include "Renderer/Graphics.h"
#include "Renderer/Binding.h"
#include "Renderer/EnvironmentLightingCache.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/DirectionalShadowMath.h"
#include "Scene/SceneRenderSnapshot.h"

#include <expected>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

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

	// Runtime renderer for opaque direct-lit metallic/roughness materials. The Application and unmoved Project must outlive it.
	// Call PrepareScene before BeginFrame and RenderPreparedScene only during an active renderer frame.
	class PULSEFORGE_API SceneRenderer final
	{
	public:
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
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureShadowPipeline(float DepthBias);
		[[nodiscard]] std::expected<void, SceneRendererError> RenderShadowCascades();
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureEnvironmentBindings(
			const AssetID& EnvironmentAsset,
			const EnvironmentLightingTextures& EnvironmentTextures);
		[[nodiscard]] std::expected<size_t, SceneRendererError> RenderPreparedSceneForFormat(ColorTargetFormat ColorFormat);
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
			float CascadeSplitDepths[4]{};
			float ShadowParameters[4]{}; // enabled, receiver normal bias, PCF radius texels, max distance
			std::array<glm::mat4, 4> CascadeViewProjection{ glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
		};
		static_assert(sizeof(FrameConstants) == 496);

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
		SamplerHandle m_Sampler;
		SamplerHandle m_EnvironmentSampler;
		SamplerHandle m_ShadowSampler;
		BindingLayoutHandle m_BindingLayout;
		BindingLayoutHandle m_ShadowBindingLayout;
		BindingLayoutHandle m_ShadowObjectBindingLayout;
		BindingSetHandle m_ShadowBindingSet;
		BindingSetHandle m_ShadowObjectBindingSet;
		std::unordered_map<int32_t, GraphicsPipelineHandle> m_ShadowPipelines;
		BufferHandle m_ObjectConstantsBuffer;
		BufferHandle m_ShadowObjectConstantsBuffer;
		BufferHandle m_FrameConstantsBuffer;
		BufferHandle m_BackgroundTriangleBuffer;
		BufferHandle m_FallbackMaterialConstantsBuffer;
		TextureHandle m_FallbackBaseColorTexture;
		std::array<RenderTargetHandle, 4> m_ShadowTargets;
		EnvironmentLightingTextures m_FallbackEnvironmentTextures;
		std::unordered_map<std::string, MaterialBindingResources> m_MaterialBindings;
		std::unordered_map<std::string, BindingSetHandle> m_EnvironmentBindingSets;
		std::unordered_map<ColorTargetFormat, GraphicsPipelineHandle> m_Pipelines;
		std::unordered_map<ColorTargetFormat, GraphicsPipelineHandle> m_BackgroundPipelines;
		std::optional<VertexLayoutDesc> m_PipelineVertexLayout;
		std::optional<SceneRenderSnapshot> m_PreparedSnapshot;
		std::optional<DirectionalShadowCascadeSet> m_PreparedCascades;
		const EnvironmentLightingTextures* m_PreparedEnvironmentTextures = nullptr;
	};
}
