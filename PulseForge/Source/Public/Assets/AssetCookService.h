#pragma once

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/CookedAssetCatalog.h"
#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge
{
	struct AssetCookRequest final
	{
		std::shared_ptr<const AssetDependencyCatalogSnapshot> SourceSnapshot;
		std::filesystem::path SourceProjectFile;
		AssetID StartupScene;
		std::vector<AssetID> DynamicRoots;
		CookedAssetCompatibility Compatibility;
		std::filesystem::path OutputRoot;
	};

	enum class AssetCookErrorCode : uint8_t
	{
		InvalidRequest,
		UnsupportedTarget,
		InvalidRoot,
		InvalidDependency,
		UnsupportedProduct,
		SourceReadFailed,
		SourceChanged,
		ProductProcessingFailed,
		SerializationFailed,
		OutputConflict,
		StagingFailed,
		PublicationFailed
	};

	struct AssetCookError final
	{
		AssetCookErrorCode Code = AssetCookErrorCode::InvalidRequest;
		AssetID Asset;
		std::filesystem::path Path;
		std::string Message;
	};

	struct AssetCookPlannedProduct final
	{
		AssetID Asset;
		CookedAssetProductKind Product = CookedAssetProductKind::Scene;
		std::string Subresource;
		std::filesystem::path SourcePath;
		std::vector<CookedAssetDependency> RuntimeDependencies;
	};

	// An owned deterministic closure over one retained G10 snapshot. Cook revalidates the snapshot and project-file
	// receipts before publication; callers should treat the plan as immutable after creation.
	struct AssetCookPlan final
	{
		AssetCookRequest Request;
		UUID ProjectID;
		std::vector<AssetID> SortedRoots;
		std::vector<AssetCookPlannedProduct> Products;
		AssetContentFingerprint ProjectFileFingerprint;
		uint64_t ProjectFileBytes = 0;
		std::optional<AssetContentFingerprint> GameplayInputFingerprint;
		uint64_t GameplayInputBytes = 0;
		bool GameplayInputPresent = false;
	};

	struct AssetCookReport final
	{
		std::filesystem::path ManifestPath;
		std::string GenerationIdentity;
		uint64_t PayloadBytes = 0;
		size_t ProductCount = 0;
		bool StagingCleanupPending = false;
		std::filesystem::path RetainedStagingPath;
	std::vector<std::filesystem::path> RetainedStalePayloads;
	};

	// CPU cook owner. It does not create renderer resources and never mutates authored source assets.
	class PULSEFORGE_API AssetCookService final
	{
	public:
		[[nodiscard]] static std::expected<AssetCookPlan, AssetCookError> Plan(const AssetCookRequest& Request);
		[[nodiscard]] static std::expected<AssetCookReport, AssetCookError> Cook(const AssetCookPlan& Plan);
		[[nodiscard]] static std::expected<AssetCookReport, AssetCookError> Cook(const AssetCookRequest& Request);
	};
} // namespace PulseForge
