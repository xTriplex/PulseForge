#pragma once

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
		const std::string Extension = AssetPath.extension().string();
		const auto IsExtension = [&Extension](std::string_view Expected)
		{
			if (Extension.size() != Expected.size())
				return false;
			for (size_t Index = 0; Index < Extension.size(); ++Index)
				if (FoldAscii(static_cast<unsigned char>(Extension[Index])) !=
					FoldAscii(static_cast<unsigned char>(Expected[Index])))
					return false;
			return true;
		};

		if (IsExtension(".scene"))
			return EditorIcon::Scene;
		if (IsExtension(".prefab"))
			return EditorIcon::Prefab;
		if (IsExtension(".gltf") || IsExtension(".glb"))
			return EditorIcon::Cube;
		if (IsExtension(".material"))
			return EditorIcon::Material;
		if (IsExtension(".lua") || IsExtension(".hlsl") || IsExtension(".glsl"))
			return EditorIcon::Script;
		if (IsExtension(".png") || IsExtension(".jpg") || IsExtension(".jpeg") || IsExtension(".tga") ||
			IsExtension(".ppm"))
			return EditorIcon::Image;
		if (IsExtension(".wav") || IsExtension(".mp3") || IsExtension(".flac") || IsExtension(".ogg"))
			return EditorIcon::AudioSource;
		return EditorIcon::File;
	}
}
