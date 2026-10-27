#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	enum class CookedBuildConfiguration : uint8_t
	{
		Debug,
		Release,
		Dist
	};

	[[nodiscard]] PULSEFORGE_API std::string_view GetCookedBuildConfigurationName(CookedBuildConfiguration Configuration) noexcept;
	[[nodiscard]] PULSEFORGE_API std::expected<CookedBuildConfiguration, std::string> ParseCookedBuildConfiguration(
		std::string_view Configuration);
	inline constexpr std::string_view CookedRuntimeABIIdentifier = "pulseforge-runtime-v1";

	struct CookedAssetDigest final
	{
		std::string Algorithm;
		std::string Value;
		friend bool operator==(const CookedAssetDigest&, const CookedAssetDigest&) = default;
	};

	enum class CookedAssetProductKind : uint8_t
	{
		Scene,
		Prefab,
		Material,
		Script,
		Audio,
		MeshPrimitive,
		Texture,
		Environment,
		GameplayInput
	};

	struct CookedAssetDependency final
	{
		AssetID Asset;
		CookedAssetProductKind Product = CookedAssetProductKind::Scene;
		std::string Subresource;
		friend bool operator==(const CookedAssetDependency&, const CookedAssetDependency&) = default;
	};

	struct CookedAssetEntry final
	{
		AssetID Asset;
		CookedAssetProductKind Product = CookedAssetProductKind::Scene;
		std::string Subresource;
		std::string RelativePayloadPath;
		uint32_t PayloadVersion = 1;
		uint64_t PayloadBytes = 0;
		std::string DigestAlgorithm;
		std::string Digest;
		std::vector<CookedAssetDependency> RuntimeDependencies;
		friend bool operator==(const CookedAssetEntry&, const CookedAssetEntry&) = default;
	};

	struct CookedAssetCompatibility final
	{
		// These three fields determine whether the runtime can consume the products.
		std::string TargetPlatform;
		std::string RendererBackend;
		std::string RuntimeABI;
		// Manifest provenance. Not a compatibility discriminator while all adapters are configuration-independent.
		CookedBuildConfiguration Configuration = CookedBuildConfiguration::Debug;
	};

	enum class CookedAssetCatalogErrorCode : uint8_t
	{
		InvalidRoot,
		ManifestReadFailed,
		ManifestTooLarge,
		InvalidManifest,
		UnsupportedVersion,
		IncompatibleTarget,
		DuplicateIdentity,
		UnsafePayloadPath,
		MissingPayload,
		PayloadTooLarge,
		PayloadReadFailed,
		PayloadDigestMismatch,
		UnsupportedConfiguration,
		AssetNotFound,
		InvalidPayload
	};

	struct CookedAssetCatalogError final
	{
		CookedAssetCatalogErrorCode Code = CookedAssetCatalogErrorCode::InvalidManifest;
		std::filesystem::path Path;
		std::optional<AssetID> Asset;
		std::string Message;
	};

	// Immutable manifest-backed runtime lookup. It owns its content root and never falls back to source files.
	class PULSEFORGE_API CookedAssetCatalog final
	{
	public:
		// Version 2 adds required build-configuration provenance; version 1 is intentionally rejected.
		static constexpr uint32_t ManifestVersion = 2;
		static constexpr std::string_view ManifestFileName = "manifest.pfmanifest";
		static constexpr size_t MaximumManifestBytes = 16u * 1024u * 1024u;
		static constexpr size_t MaximumEntries = 250'000;
		static constexpr uint64_t MaximumPayloadBytes = 512ull * 1024ull * 1024ull;

		[[nodiscard]] static std::expected<std::shared_ptr<const CookedAssetCatalog>, CookedAssetCatalogError> Open(
			const std::filesystem::path& ContentRoot, const CookedAssetCompatibility& ExpectedCompatibility);
		[[nodiscard]] static std::expected<std::string, CookedAssetCatalogError> SerializeManifest(
			const UUID& ProjectID,
			const AssetID& StartupScene,
			const CookedAssetCompatibility& Compatibility,
			std::vector<AssetID> DynamicRoots,
			std::vector<CookedAssetEntry> Entries);
		[[nodiscard]] static CookedAssetDigest ComputePayloadDigest(std::span<const std::byte> Bytes);

		[[nodiscard]] const UUID& GetProjectID() const noexcept { return m_ProjectID; }
		[[nodiscard]] const AssetID& GetStartupScene() const noexcept { return m_StartupScene; }
		[[nodiscard]] const CookedAssetCompatibility& GetCompatibility() const noexcept { return m_Compatibility; }
		[[nodiscard]] const std::vector<AssetID>& GetDynamicRoots() const noexcept { return m_DynamicRoots; }
		[[nodiscard]] const std::vector<CookedAssetEntry>& GetEntries() const noexcept { return m_Entries; }
		[[nodiscard]] const CookedAssetEntry* Find(
			const AssetID& Asset, CookedAssetProductKind Product, std::string_view Subresource = {}) const noexcept;
		[[nodiscard]] std::expected<std::vector<std::byte>, CookedAssetCatalogError> ReadPayload(
			const CookedAssetEntry& Entry) const;
		[[nodiscard]] const std::filesystem::path& GetContentRoot() const noexcept { return m_ContentRoot; }

	private:
		CookedAssetCatalog(std::filesystem::path ContentRoot, UUID ProjectID, AssetID StartupScene,
			CookedAssetCompatibility Compatibility, std::vector<AssetID> DynamicRoots, std::vector<CookedAssetEntry> Entries);

		std::filesystem::path m_ContentRoot;
		UUID m_ProjectID;
		AssetID m_StartupScene;
		CookedAssetCompatibility m_Compatibility;
		std::vector<AssetID> m_DynamicRoots;
		std::vector<CookedAssetEntry> m_Entries;
	};
} // namespace PulseForge
