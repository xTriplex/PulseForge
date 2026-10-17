#pragma once

#include "Assets/AssetTypes.h"

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	enum class AssetDestinationNameError
	{
		InvalidFilename,
		ExtensionMismatch,
		ReservedSidecarExtension
	};

	enum class AssetDestinationConflict
	{
		None,
		SamePath,
		ExistingDestination,
		CaseOnlyRename,
		InspectionFailed
	};

	[[nodiscard]] inline std::vector<std::filesystem::path> BuildAssetDestinationBreadcrumbs(
		const std::filesystem::path& RelativeFolder)
	{
		std::vector<std::filesystem::path> Breadcrumbs;
		std::filesystem::path Prefix;
		for (const auto& Component : RelativeFolder.lexically_normal())
		{
			if (Component.empty() || Component == ".")
				continue;
			Prefix /= Component;
			Breadcrumbs.push_back(Prefix);
		}
		return Breadcrumbs;
	}

	[[nodiscard]] constexpr AssetDestinationConflict ResolveAssetDestinationConflict(
		bool IsDuplicate,
		bool DestinationExists,
		bool SidecarExists,
		bool DestinationIsSourceAsset,
		bool SamePathSpelling,
		bool InspectionFailed,
		bool SidecarIsSourceSidecar = false) noexcept
	{
		if (InspectionFailed)
			return AssetDestinationConflict::InspectionFailed;
		if (!IsDuplicate && SamePathSpelling)
			return AssetDestinationConflict::SamePath;
		if (!IsDuplicate && DestinationExists && SidecarExists && DestinationIsSourceAsset && SidecarIsSourceSidecar &&
			!SamePathSpelling)
		{
			return AssetDestinationConflict::CaseOnlyRename;
		}
		if (DestinationExists || SidecarExists)
			return AssetDestinationConflict::ExistingDestination;
		return AssetDestinationConflict::None;
	}

	[[nodiscard]] inline std::expected<std::filesystem::path, AssetDestinationNameError>
	ResolveAssetDestinationFilename(std::string_view FilenameText, const std::filesystem::path& SourcePath)
	{
		if (FilenameText.empty() || FilenameText == "." || FilenameText == "..")
			return std::unexpected(AssetDestinationNameError::InvalidFilename);

		const std::filesystem::path Filename = std::filesystem::u8path(FilenameText);
		if (Filename.has_parent_path() || FilenameText.find('/') != std::string_view::npos ||
			FilenameText.find_first_of("\\<>:\"|?*") != std::string_view::npos ||
			FilenameText.back() == '.' || FilenameText.back() == ' ')
			return std::unexpected(AssetDestinationNameError::InvalidFilename);

		std::filesystem::path FinalFilename = Filename;
		if (FinalFilename.extension().empty())
			FinalFilename += SourcePath.extension();

		if (PulseForge::IsMetadataSidecarPath(FinalFilename)) return std::unexpected(AssetDestinationNameError::ReservedSidecarExtension);
		if (!PulseForge::AssetExtensionsEqual(FinalFilename.extension().string(), SourcePath.extension().string()))
			return std::unexpected(AssetDestinationNameError::ExtensionMismatch);

		return FinalFilename;
	}
}
