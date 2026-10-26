#pragma once

#include "Assets/CookedAssetCatalog.h"
#include "Assets/CookedProductCodec.h"
#include "Core/Core.h"
#include "Input/GameplayInput.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace PulseForge
{
	class Scene;
	class Entity;
	// Typed, immutable access to cooked runtime products. This adapter owns its catalog and never resolves authored paths.
	class PULSEFORGE_API CookedContentProvider final
	{
	public:
		[[nodiscard]] static std::expected<std::shared_ptr<const CookedContentProvider>, CookedAssetCatalogError> Open(
			const std::filesystem::path& ContentRoot,
			const CookedAssetCompatibility& ExpectedCompatibility);

		[[nodiscard]] const UUID& GetProjectID() const noexcept;
		[[nodiscard]] const AssetID& GetStartupScene() const noexcept;
		[[nodiscard]] const std::vector<AssetID>& GetDynamicRoots() const noexcept;
		[[nodiscard]] const CookedAssetCompatibility& GetCompatibility() const noexcept;

		[[nodiscard]] const CookedAssetEntry* FindScene(const AssetID& Asset) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadScene(const AssetID& Asset) const;
		[[nodiscard]] std::expected<std::unique_ptr<Scene>, CookedAssetCatalogError> LoadScene(const AssetID& Asset) const;
		[[nodiscard]] std::expected<Entity, CookedAssetCatalogError> InstantiatePrefab(
			const AssetID& Asset, Scene& Destination) const;

		[[nodiscard]] const CookedAssetEntry* FindPrefab(const AssetID& Asset) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadPrefab(const AssetID& Asset) const;

		[[nodiscard]] const CookedAssetEntry* FindMaterial(const AssetID& Asset) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadMaterial(const AssetID& Asset) const;
		[[nodiscard]] std::expected<CookedMaterialProduct, CookedAssetCatalogError> LoadMaterialProduct(
			const AssetID& Asset) const;

		[[nodiscard]] const CookedAssetEntry* FindScript(const AssetID& Asset) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadScript(const AssetID& Asset) const;

		[[nodiscard]] const CookedAssetEntry* FindAudio(const AssetID& Asset) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadAudio(const AssetID& Asset) const;

		[[nodiscard]] const CookedAssetEntry* FindMeshPrimitive(
			const AssetID& Asset, std::string_view Subresource) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadMeshPrimitive(
			const AssetID& Asset, std::string_view Subresource) const;
		[[nodiscard]] std::expected<ImportedGltfMesh, CookedAssetCatalogError> LoadMeshPrimitive(
			const AssetID& Asset, std::string_view Subresource) const;

		[[nodiscard]] const CookedAssetEntry* FindTexture(
			const AssetID& Asset, std::string_view Subresource = "srgb") const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadTexture(
			const AssetID& Asset, std::string_view Subresource = "srgb") const;
		[[nodiscard]] std::expected<Detail::PreparedTextureCpuProduct, CookedAssetCatalogError> LoadTextureProduct(
			const AssetID& Asset, std::string_view Subresource = "srgb") const;

		[[nodiscard]] const CookedAssetEntry* FindEnvironment(const AssetID& Asset) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadEnvironment(
			const AssetID& Asset) const;
		[[nodiscard]] std::expected<Detail::DecodedEnvironmentCpuProduct, CookedAssetCatalogError> LoadEnvironmentProduct(
			const AssetID& Asset) const;

		[[nodiscard]] const CookedAssetEntry* FindGameplayInput(const AssetID& Asset) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadGameplayInput(
			const AssetID& Asset) const;
		[[nodiscard]] std::expected<GameplayActionMap, CookedAssetCatalogError> LoadGameplayInput(
			const AssetID& Asset) const;

	private:
		explicit CookedContentProvider(std::shared_ptr<const CookedAssetCatalog> Catalog);

		[[nodiscard]] const CookedAssetEntry* Find(
			const AssetID& Asset, CookedAssetProductKind Product, std::string_view Subresource = {}) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> Read(
			const AssetID& Asset, CookedAssetProductKind Product, std::string_view Subresource = {}) const;

		std::shared_ptr<const CookedAssetCatalog> m_Catalog;
	};
} // namespace PulseForge
