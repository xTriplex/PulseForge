#pragma once

#include "Assets/AssetTypes.h"

#include "Editor/EditorIcons.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace PulseForgeEditor
{
	[[nodiscard]] constexpr unsigned char FoldAscii(unsigned char Character) noexcept
	{
		return Character >= 'A' && Character <= 'Z'
			? static_cast<unsigned char>(Character - 'A' + 'a')
			: Character;
	}

	[[nodiscard]] constexpr bool ContainsCaseInsensitive(
		std::string_view Text,
		std::string_view Query) noexcept
	{
		if (Query.empty())
			return true;
		if (Query.size() > Text.size())
			return false;

		for (size_t Start = 0; Start <= Text.size() - Query.size(); ++Start)
		{
			size_t Offset = 0;
			while (Offset < Query.size() && FoldAscii(static_cast<unsigned char>(Text[Start + Offset])) ==
				FoldAscii(static_cast<unsigned char>(Query[Offset])))
				++Offset;
			if (Offset == Query.size())
				return true;
		}
		return false;
	}

	[[nodiscard]] inline bool IsManagedAssetInFolder(
		const std::filesystem::path& AssetPath,
		const std::filesystem::path& FolderPath)
	{
		const std::filesystem::path NormalizedAsset = AssetPath.lexically_normal();
		const std::filesystem::path NormalizedFolder = FolderPath.lexically_normal();
		auto Asset = NormalizedAsset.begin();
		auto Folder = NormalizedFolder.begin();
		if (Asset == NormalizedAsset.end() || *Asset != "Assets")
			return false;
		if (Folder == NormalizedFolder.end() || *Folder != "Assets")
			return false;
		return NormalizedAsset.parent_path() == NormalizedFolder;
	}

	[[nodiscard]] inline bool IsManagedAssetInFolderSubtree(
		const std::filesystem::path& AssetPath,
		const std::filesystem::path& FolderPath)
	{
		const std::filesystem::path NormalizedAsset = AssetPath.lexically_normal();
		const std::filesystem::path NormalizedFolder = FolderPath.lexically_normal();
		auto Asset = NormalizedAsset.begin();
		auto Folder = NormalizedFolder.begin();
		if (Asset == NormalizedAsset.end() || *Asset != "Assets" ||
			Folder == NormalizedFolder.end() || *Folder != "Assets")
			return false;

		for (; Folder != NormalizedFolder.end(); ++Asset, ++Folder)
			if (Asset == NormalizedAsset.end() || *Asset != *Folder)
				return false;

		return Asset != NormalizedAsset.end() && NormalizedAsset.has_filename();
	}

	[[nodiscard]] inline bool IsImmediateChildFolder(
		const std::filesystem::path& CandidateFolder,
		const std::filesystem::path& ParentFolder)
	{
		const std::filesystem::path NormalizedCandidate = CandidateFolder.lexically_normal();
		const std::filesystem::path NormalizedParent = ParentFolder.lexically_normal();
		return NormalizedCandidate != NormalizedParent &&
			IsManagedAssetInFolderSubtree(NormalizedCandidate / "__folder_marker__", NormalizedParent) &&
			NormalizedCandidate.parent_path() == NormalizedParent;
	}

	[[nodiscard]] inline bool MatchesManagedAssetFolderSearch(
		const std::filesystem::path& AssetPath,
		const std::filesystem::path& FolderPath,
		std::string_view Query)
	{
		if (Query.empty())
			return IsManagedAssetInFolder(AssetPath, FolderPath);
		if (!IsManagedAssetInFolderSubtree(AssetPath, FolderPath))
			return false;

		const std::string Path = AssetPath.lexically_normal().generic_string();
		const std::string Filename = AssetPath.filename().string();
		return ContainsCaseInsensitive(Filename, Query) || ContainsCaseInsensitive(Path, Query);
	}

	[[nodiscard]] constexpr bool IsFolderAncestorOrSelf(
		std::string_view Candidate,
		std::string_view Folder) noexcept
	{
		if (Candidate == Folder)
			return true;
		return Candidate.size() > Folder.size() && Candidate.starts_with(Folder) &&
			(Folder.empty() || Folder.back() == '/' || Candidate[Folder.size()] == '/');
	}

	[[nodiscard]] inline EditorIcon GetAssetBrowserIcon(const std::filesystem::path& AssetPath)
	{
		if (PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::Scene)) return EditorIcon::Scene;
		if (PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::Prefab)) return EditorIcon::Prefab;
		if (PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::Mesh)) return EditorIcon::Cube;
		if (PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::Material)) return EditorIcon::Material;
		if (PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::Script) ||
			PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::ShaderSource))
			return EditorIcon::Script;
		if (PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::Image)) return EditorIcon::Image;
		if (PulseForge::HasAssetTypeExtension(AssetPath, PulseForge::AssetFileType::Audio)) return EditorIcon::AudioSource;
		return EditorIcon::File;
	}
}
