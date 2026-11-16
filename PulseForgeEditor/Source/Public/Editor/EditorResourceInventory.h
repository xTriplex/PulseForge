#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace PulseForge
{
	class AssetDependencyCatalogSnapshot;
	class UUID;
}

namespace PulseForgeEditor
{
	enum class EditorResourceInventoryState : uint8_t
	{
		Available,
		Missing,
		Unavailable
	};

	struct EditorResourceInventoryEntry
	{
		std::string Identity;
		std::string Owner;
		std::string State;
		std::string StorageIdentity;
		std::vector<std::string> Dependencies;
		size_t OmittedDependencies = 0;
		std::optional<uint64_t> StorageBytes;
		EditorResourceInventoryState Availability = EditorResourceInventoryState::Available;
		std::string AvailabilityReason;
	};

	struct EditorResourceInventoryLimits
	{
		size_t MaximumSourceAssets = 4096;
		size_t MaximumDerivedArtifacts = 4096;
		size_t MaximumVisitedDerivedEntries = 16384;
		size_t MaximumDependencyEdgesVisited = 65536;
		size_t MaximumDependenciesPerEntry = 64;
	};

	struct EditorResourceInventorySnapshot
	{
		std::string ProjectIdentity;
		uint64_t Generation = 0;
		std::vector<EditorResourceInventoryEntry> Entries;
		size_t OmittedSourceAssets = 0;
		size_t OmittedDerivedArtifacts = 0;
		size_t OmittedDependencyEdges = 0;
		bool DerivedArtifactsTruncated = false;
		bool DependencyEdgesTruncated = false;
		bool Cancelled = false;
		uint64_t KnownUniquePathBytes = 0;
		size_t UnavailableStorageCount = 0;
	};

	[[nodiscard]] std::string GetEditorResourceInventoryProjectIdentity(
		const std::filesystem::path& ProjectRoot,
		const PulseForge::UUID& ProjectID);

	// Builds a read-only, bounded view from an already validated dependency snapshot. It never loads or mutates assets.
	[[nodiscard]] EditorResourceInventorySnapshot BuildEditorResourceInventory(
		const PulseForge::AssetDependencyCatalogSnapshot& Catalog,
		const std::filesystem::path& DerivedDataRoot,
		const EditorResourceInventoryLimits& Limits = {},
		std::stop_token CancellationToken = {});
} // namespace PulseForgeEditor
