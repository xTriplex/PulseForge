#pragma once

#include "Assets/MaterialAssetCache.h"
#include "Renderer/AssetCache/TextureAssetCache.h"
#include "Renderer/SceneRenderer.h"

#include <expected>
#include <optional>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class SceneRendererMaterialBindingCache final
	{
	public:
		struct Resources final
		{
			MaterialAssetCache& Materials;
			TextureAssetCache& Textures;
			const BindingLayoutHandle& Layout;
			const Texture& OcclusionFallback;
			const BufferHandle& ObjectConstants;
			const BufferHandle& FrameConstants;
			const BufferHandle& LocalLightingConstants;
		};

		explicit SceneRendererMaterialBindingCache(RenderDevice& Runtime) noexcept;

		[[nodiscard]] std::expected<void, SceneRendererError> Initialize(const Resources& Resources);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureMaterialBindings(
			const AssetID& MaterialAsset,
			const std::optional<AssetID>& EnvironmentAsset,
			const EnvironmentLightingTextures& EnvironmentTextures);
		[[nodiscard]] std::expected<void, SceneRendererError> EnsureEnvironmentBindings(
			const AssetID& EnvironmentAsset,
			const EnvironmentLightingTextures& EnvironmentTextures,
			bool IsReady);
		[[nodiscard]] std::expected<const MaterialAssetDesc*, SceneRendererError> ResolveMaterialDescription(
			const SceneMeshInstance& Instance);
		[[nodiscard]] std::expected<const BindingSet*, SceneRendererError> ResolveMaterialBinding(
			const SceneMeshInstance& Instance,
			const std::optional<AssetID>& EnvironmentAsset) const;
		[[nodiscard]] const BindingSet* FindEnvironmentBindings(const AssetID& EnvironmentAsset, bool IsReady) const;
		void RefreshMaterialAssets() noexcept;

	private:
		struct MaterialConstants final
		{
			float BaseColor[4];
			float Surface[4];
			float Emissive[4];
			float Alpha[4];
		};
		static_assert(sizeof(MaterialConstants) == 64);

		struct MaterialBinding final
		{
			BufferHandle Constants;
			BindingSetHandle Set;
		};

		[[nodiscard]] std::expected<void, SceneRendererError> RequireInitialized() const;
		[[nodiscard]] static SceneRendererError MakeResourceError(std::string Message);
		[[nodiscard]] static std::string MakeMaterialBindingKey(
			const AssetID& MaterialAsset,
			const std::optional<AssetID>& EnvironmentAsset);
		[[nodiscard]] static std::string MakeEnvironmentBindingKey(const AssetID& EnvironmentAsset, bool IsReady);

		RenderDevice& m_Runtime;
		MaterialAssetCache* m_Materials = nullptr;
		TextureAssetCache* m_Textures = nullptr;
		const BindingLayoutHandle* m_Layout = nullptr;
		const Texture* m_OcclusionFallback = nullptr;
		const BufferHandle* m_ObjectConstants = nullptr;
		const BufferHandle* m_FrameConstants = nullptr;
		const BufferHandle* m_LocalLightingConstants = nullptr;
		SamplerHandle m_MaterialSampler;
		SamplerHandle m_EnvironmentSampler;
		TextureHandle m_FallbackBaseColor;
		TextureHandle m_FallbackMetallicRoughness;
		TextureHandle m_FallbackNormal;
		TextureHandle m_FallbackEmissive;
		BufferHandle m_FallbackMaterialConstants;
		std::unordered_map<std::string, MaterialBinding> m_MaterialBindings;
		std::unordered_map<std::string, BindingSetHandle> m_EnvironmentBindings;
	};
} // namespace PulseForge
