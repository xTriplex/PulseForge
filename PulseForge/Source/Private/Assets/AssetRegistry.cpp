#include "Core/PulseForgePCH.h"
#include "Assets/AssetRegistry.h"
#include "Core/ScopedProfileTimer.h"

#include <algorithm>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		AssetRegistryIssue MakeIssue(
			AssetRegistryIssueCode Code,
			std::filesystem::path Path,
			std::string Message,
			std::filesystem::path RelatedPath = {})
		{
			return { Code, std::move(Path), std::move(RelatedPath), std::move(Message) };
		}

		std::string PathForMessage(const std::filesystem::path& Path)
		{
			return Path.generic_string();
		}
	}

	std::expected<void, AssetRegistryError> AssetRegistry::Rebuild(const std::filesystem::path& ProjectRoot)
	{
		Detail::ScopedProfileTimer Timer("AssetRegistry::Rebuild");
		AssetRegistryError Error;
		if (ProjectRoot.empty())
		{
			Error.Issues.push_back(MakeIssue(
				AssetRegistryIssueCode::InvalidProjectRoot,
				{},
				"Project root path must not be empty"));
			return std::unexpected(std::move(Error));
		}

		std::error_code FileError;
		const std::filesystem::path AbsoluteProjectRoot =
			std::filesystem::absolute(ProjectRoot, FileError).lexically_normal();
		if (FileError || !std::filesystem::is_directory(AbsoluteProjectRoot, FileError) || FileError)
		{
			Error.Issues.push_back(MakeIssue(
				AssetRegistryIssueCode::InvalidProjectRoot,
				ProjectRoot,
				"Project root is not an accessible directory: " + PathForMessage(ProjectRoot)));
			return std::unexpected(std::move(Error));
		}

		const std::filesystem::path AssetRoot = AbsoluteProjectRoot / "Assets";
		FileError.clear();
		const std::filesystem::file_status AssetRootStatus = std::filesystem::symlink_status(AssetRoot, FileError);
		if (FileError || AssetRootStatus.type() == std::filesystem::file_type::not_found)
		{
			Error.Issues.push_back(MakeIssue(
				AssetRegistryIssueCode::MissingAssetDirectory,
				"Assets",
				"Project must contain an accessible Assets directory: " + PathForMessage(AssetRoot)));
			return std::unexpected(std::move(Error));
		}
		if (std::filesystem::is_symlink(AssetRootStatus))
		{
			Error.Issues.push_back(MakeIssue(
				AssetRegistryIssueCode::UnsupportedEntry,
				"Assets",
				"Project Assets directory must not be a symbolic link: " + PathForMessage(AssetRoot)));
			return std::unexpected(std::move(Error));
		}
		if (!std::filesystem::is_directory(AssetRootStatus))
		{
			Error.Issues.push_back(MakeIssue(
				AssetRegistryIssueCode::MissingAssetDirectory,
				"Assets",
				"Project must contain an accessible Assets directory: " + PathForMessage(AssetRoot)));
			return std::unexpected(std::move(Error));
		}

		std::vector<std::filesystem::path> SourceFiles;
		std::vector<std::filesystem::path> SidecarFiles;
		std::filesystem::recursive_directory_iterator Iterator(AssetRoot, FileError);
		const std::filesystem::recursive_directory_iterator End;
		if (FileError)
		{
			Error.Issues.push_back(MakeIssue(
				AssetRegistryIssueCode::AssetScanFailed,
				"Assets",
				"Could not enumerate project assets: " + FileError.message()));
			return std::unexpected(std::move(Error));
		}

		while (Iterator != End)
		{
			const std::filesystem::directory_entry Entry = *Iterator;
			const std::filesystem::path RelativePath = Entry.path().lexically_relative(AbsoluteProjectRoot);
			const std::filesystem::file_status Status = Entry.symlink_status(FileError);
			if (FileError)
			{
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::AssetScanFailed,
					RelativePath,
					"Could not inspect project asset entry: " + FileError.message()));
				FileError.clear();
			}
			else if (std::filesystem::is_symlink(Status))
			{
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::UnsupportedEntry,
					RelativePath,
					"Symbolic links are not registered as managed assets: " + PathForMessage(RelativePath)));
			}
			else if (std::filesystem::is_regular_file(Status))
			{
				if (Entry.path().extension() == ".meta")
					SidecarFiles.push_back(Entry.path());
				else
					SourceFiles.push_back(Entry.path());
			}
			else if (!std::filesystem::is_directory(Status))
			{
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::UnsupportedEntry,
					RelativePath,
					"Unsupported filesystem entry in project Assets directory: " + PathForMessage(RelativePath)));
			}

			Iterator.increment(FileError);
			if (FileError)
			{
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::AssetScanFailed,
					"Assets",
					"Could not continue enumerating project assets: " + FileError.message()));
				break;
			}
		}

		const auto SortByPath = [](const std::filesystem::path& First, const std::filesystem::path& Second)
		{
			return First.generic_string() < Second.generic_string();
		};
		std::sort(SourceFiles.begin(), SourceFiles.end(), SortByPath);
		std::sort(SidecarFiles.begin(), SidecarFiles.end(), SortByPath);

		for (const std::filesystem::path& SourcePath : SourceFiles)
		{
			const std::filesystem::path SidecarPath = AssetMetadataSerializer::GetSidecarPath(SourcePath);
			FileError.clear();
			const std::filesystem::file_status SidecarStatus = std::filesystem::symlink_status(SidecarPath, FileError);
			if (FileError == std::errc::no_such_file_or_directory ||
				(!FileError && SidecarStatus.type() == std::filesystem::file_type::not_found))
			{
				const std::filesystem::path RelativePath = SourcePath.lexically_relative(AbsoluteProjectRoot);
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::MissingMetadata,
					RelativePath,
					"Managed source asset has no adjacent .meta file: " + PathForMessage(RelativePath)));
			}
			else if (FileError)
			{
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::AssetScanFailed,
					SourcePath.lexically_relative(AbsoluteProjectRoot),
					"Could not check adjacent asset metadata: " + FileError.message()));
			}
			else if (std::filesystem::is_symlink(SidecarStatus) || !std::filesystem::is_regular_file(SidecarStatus))
			{
				const std::filesystem::path RelativePath = SourcePath.lexically_relative(AbsoluteProjectRoot);
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::InvalidMetadata,
					RelativePath,
					"Adjacent asset metadata must be a regular, non-symlink file: " + PathForMessage(RelativePath)));
			}
		}

		std::unordered_map<AssetID, AssetRecord, UUIDHash> RebuiltAssets;
		RebuiltAssets.reserve(SidecarFiles.size());
		for (const std::filesystem::path& SidecarPath : SidecarFiles)
		{
			std::filesystem::path SourcePath = SidecarPath;
			SourcePath.replace_extension();
			if (SourcePath.extension() == ".meta")
			{
				const std::filesystem::path RelativePath = SidecarPath.lexically_relative(AbsoluteProjectRoot);
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::UnsupportedEntry,
					RelativePath,
					"The .meta suffix is reserved for sidecar files and cannot identify a source asset: " +
						PathForMessage(RelativePath)));
				continue;
			}

			FileError.clear();
			if (!std::filesystem::is_regular_file(SourcePath, FileError) || FileError)
			{
				const std::filesystem::path RelativePath = SourcePath.lexically_relative(AbsoluteProjectRoot);
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::MissingSourceAsset,
					RelativePath,
					"Asset metadata has no corresponding source file: " + PathForMessage(RelativePath)));
				continue;
			}

			const auto Metadata = AssetMetadataSerializer::LoadFromFile(SidecarPath);
			if (!Metadata)
			{
				const std::filesystem::path RelativePath = SourcePath.lexically_relative(AbsoluteProjectRoot);
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::InvalidMetadata,
					RelativePath,
					Metadata.error().Message));
				continue;
			}

			const std::filesystem::path RelativePath = SourcePath.lexically_relative(AbsoluteProjectRoot);
			const auto [Existing, Inserted] = RebuiltAssets.try_emplace(
				Metadata->ID,
				AssetRecord{ Metadata->ID, RelativePath });
			if (!Inserted)
			{
				Error.Issues.push_back(MakeIssue(
					AssetRegistryIssueCode::DuplicateAssetID,
					RelativePath,
					"Asset UUID " + Metadata->ID.ToString() + " is also assigned to " +
						PathForMessage(Existing->second.ProjectRelativePath),
					Existing->second.ProjectRelativePath));
			}
		}

		if (!Error.Issues.empty())
			return std::unexpected(std::move(Error));

		m_Assets.swap(RebuiltAssets);
		return {};
	}

	std::optional<AssetRecord> AssetRegistry::Find(const AssetID& ID) const
	{
		const auto Asset = m_Assets.find(ID);
		if (Asset == m_Assets.end())
			return std::nullopt;
		return Asset->second;
	}

	std::vector<AssetRecord> AssetRegistry::GetAssets() const
	{
		std::vector<AssetRecord> Assets;
		Assets.reserve(m_Assets.size());
		for (const auto& Entry : m_Assets)
		{
			Assets.push_back(Entry.second);
		}
		std::sort(Assets.begin(), Assets.end(), [](const AssetRecord& First, const AssetRecord& Second)
		{
			return First.ProjectRelativePath < Second.ProjectRelativePath;
		});
		return Assets;
	}
}
