#pragma once

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetDependencyTypes.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	struct AssetDependencyExtraction
	{
		std::vector<AssetDependencyEdge> Edges;
		std::vector<AssetDependencyIssue> Issues;
		std::vector<AssetSubresourceRecord> Subresources;
		std::vector<AssetImportObservation> ImportObservations;
		std::vector<AssetDependencyInputReceipt> ReadSet;
	};

	struct AssetDependencyCatalogBuildOptions
	{
		// Deterministic test seam invoked after extraction and before the final receipt validation.
		std::function<void()> BeforeFinalInputValidation;
	};

	class PULSEFORGE_API AssetDependencyExtractor final
	{
	public:
		// Fatal errors indicate the candidate could not be analyzed coherently. Valid but unresolved or unsupported
		// references are returned as typed edges and issues so a catalog can publish useful diagnostics.
		[[nodiscard]] static std::expected<AssetDependencyExtraction, AssetDependencyCatalogError> Extract(
			const std::filesystem::path& ProjectRoot, const UUID& ProjectID, const AssetRegistry& Registry);

		[[nodiscard]] static std::expected<void, AssetDependencyCatalogError> ValidateInputReceipts(
			const std::filesystem::path& ProjectRoot,
			std::span<const AssetDependencyInputReceipt> ReadSet);
	};
} // namespace PulseForge
