#pragma once

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

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
		CaseOnlyRenameUnsupported,
		InspectionFailed
	};

	[[nodiscard]] constexpr AssetDestinationConflict ResolveAssetDestinationConflict(
		bool IsDuplicate,
		bool DestinationExists,
		bool SidecarExists,
		bool DestinationIsSourceAsset,
		bool SamePathSpelling,
		bool InspectionFailed) noexcept
	{
		if (InspectionFailed)
			return AssetDestinationConflict::InspectionFailed;
		if (!IsDuplicate && DestinationExists && DestinationIsSourceAsset && !SamePathSpelling)
			return AssetDestinationConflict::CaseOnlyRenameUnsupported;
		if (!IsDuplicate && SamePathSpelling)
			return AssetDestinationConflict::SamePath;
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

		const auto Lower = [](std::string Value)
		{
			std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Character)
			{
				return static_cast<char>(std::tolower(Character));
			});
			return Value;
		};
		const std::string Extension = Lower(FinalFilename.extension().string());
		if (Extension == ".meta")
			return std::unexpected(AssetDestinationNameError::ReservedSidecarExtension);
		if (Extension != Lower(SourcePath.extension().string()))
			return std::unexpected(AssetDestinationNameError::ExtensionMismatch);

		return FinalFilename;
	}
}
