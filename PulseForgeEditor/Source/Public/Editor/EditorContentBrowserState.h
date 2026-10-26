#pragma once

#include "Assets/AssetRegistry.h"
#include "Editor/EditorIcons.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorContentFolderNode
	{
		std::string Name;
		std::filesystem::path RelativePath;
		std::vector<EditorContentFolderNode> Children;
	};

	struct EditorContentBrowserAssetPresentation
	{
		std::string Identifier;
		std::string Name;
		std::string FullPath;
		std::string Extension;
		EditorIcon Icon = EditorIcon::File;
	};

	class EditorContentBrowserState final
	{
	public:
		[[nodiscard]] const std::vector<PulseForge::AssetRecord>& Assets() const noexcept { return m_Assets; }
		[[nodiscard]] const std::vector<EditorContentBrowserAssetPresentation>& AssetPresentations() const noexcept
		{
			return m_AssetPresentations;
		}
		[[nodiscard]] const std::vector<size_t>& FilteredAssetIndices() const noexcept { return m_FilteredAssetIndices; }
		[[nodiscard]] const EditorContentFolderNode& FolderRoot() const noexcept { return m_FolderRoot; }
		[[nodiscard]] const std::filesystem::path& CurrentFolder() const noexcept { return m_CurrentFolder; }
		[[nodiscard]] const std::optional<std::filesystem::path>& SelectedFolder() const noexcept { return m_SelectedFolder; }
		[[nodiscard]] std::optional<std::filesystem::path>& SelectedFolder() noexcept { return m_SelectedFolder; }
		[[nodiscard]] const std::string& SearchText() const noexcept { return m_SearchText; }
		[[nodiscard]] std::string& SearchText() noexcept { return m_SearchText; }
		[[nodiscard]] bool IsGridView() const noexcept { return m_IsGridView; }
		[[nodiscard]] bool& GridView() noexcept { return m_IsGridView; }
		[[nodiscard]] const std::optional<PulseForge::AssetID>& SelectedAsset() const noexcept { return m_SelectedAsset; }
		[[nodiscard]] const std::vector<PulseForge::AssetID>& SelectedAssets() const noexcept { return m_SelectedAssets; }
		[[nodiscard]] bool IsAssetSelected(const PulseForge::AssetID& Asset) const noexcept;
		void SelectAsset(const PulseForge::AssetID& Asset, bool Toggle = false);
		void SetPrimarySelectedAsset(const PulseForge::AssetID& Asset) noexcept;
		void SetSelectedAssets(std::vector<PulseForge::AssetID> Assets);
		void ClearAssetSelection() noexcept;

		void SetAssets(std::vector<PulseForge::AssetRecord> Assets);
		void ResetNavigation();
		void ClearProject();
		void SetCurrentFolder(std::filesystem::path Folder);
		void RebaseFoldersAfterMove(const std::filesystem::path& Source, const std::filesystem::path& Destination);
		void RebuildFilteredAssetIndices();
		[[nodiscard]] bool IsKnownFolder(const std::filesystem::path& Folder) const;
		[[nodiscard]] const EditorContentFolderNode* FindFolder(const std::filesystem::path& Folder) const;

	private:
		void RebuildFolderTree();
		void RebuildAssetPresentations();
		[[nodiscard]] static bool ContainsFolder(const EditorContentFolderNode& Node, const std::filesystem::path& Candidate);
		[[nodiscard]] static const EditorContentFolderNode* FindFolder(
			const EditorContentFolderNode& Node, const std::filesystem::path& Folder);

		std::vector<PulseForge::AssetRecord> m_Assets;
		std::vector<EditorContentBrowserAssetPresentation> m_AssetPresentations;
		std::vector<size_t> m_FilteredAssetIndices;
		EditorContentFolderNode m_FolderRoot{ "Assets", "Assets", {} };
		std::filesystem::path m_CurrentFolder{ "Assets" };
		std::optional<std::filesystem::path> m_SelectedFolder;
		std::string m_SearchText;
		std::optional<PulseForge::AssetID> m_SelectedAsset;
		std::vector<PulseForge::AssetID> m_SelectedAssets;
		bool m_IsGridView = true;
	};
} // namespace PulseForgeEditor
