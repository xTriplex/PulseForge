#include "Editor/EditorContentBrowserState.h"
#include "Editor/EditorWorkspaceView.h"

#include <algorithm>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string UTF8Path = Path.generic_u8string();
			return { reinterpret_cast<const char*>(UTF8Path.data()), UTF8Path.size() };
		}

		std::string NormalizedExtension(const std::filesystem::path& Path)
		{
			std::string Extension = PathToUtf8(Path.extension());
			std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](unsigned char Character)
			{
				return Character >= 'A' && Character <= 'Z'
					? static_cast<char>(Character - 'A' + 'a')
					: static_cast<char>(Character);
			});
			return Extension;
		}
	}

	void EditorContentBrowserState::SetAssets(std::vector<PulseForge::AssetRecord> Assets)
	{
		m_Assets = std::move(Assets);
		RebuildAssetPresentations();
		RebuildFolderTree();
	}

	void EditorContentBrowserState::RebuildAssetPresentations()
	{
		m_AssetPresentations.clear();
		m_AssetPresentations.reserve(m_Assets.size());
		for (const PulseForge::AssetRecord& Asset : m_Assets)
			m_AssetPresentations.push_back({
				Asset.ID.ToString(),
				PathToUtf8(Asset.ProjectRelativePath.filename()),
				PathToUtf8(Asset.ProjectRelativePath),
				NormalizedExtension(Asset.ProjectRelativePath),
				GetAssetBrowserIcon(Asset.ProjectRelativePath) });
	}

	void EditorContentBrowserState::RebuildFolderTree()
	{
		m_FolderRoot = { "Assets", "Assets", {} };
		for (const PulseForge::AssetRecord& Asset : m_Assets)
		{
			const std::filesystem::path Normalized = Asset.ProjectRelativePath.lexically_normal();
			const std::filesystem::path ParentPath = Normalized.parent_path();
			auto Component = ParentPath.begin();
			if (Component == ParentPath.end() || *Component != "Assets")
				continue;
			++Component;

			std::filesystem::path CurrentPath = "Assets";
			EditorContentFolderNode* CurrentNode = &m_FolderRoot;
			for (; Component != ParentPath.end(); ++Component)
			{
				CurrentPath /= *Component;
				const std::string Name = PathToUtf8(*Component);
				auto Child = std::find_if(CurrentNode->Children.begin(), CurrentNode->Children.end(),
					[&Name](const EditorContentFolderNode& Candidate)
					{
						return Candidate.Name == Name;
					});
				if (Child == CurrentNode->Children.end())
				{
					CurrentNode->Children.push_back({ Name, CurrentPath, {} });
					Child = std::prev(CurrentNode->Children.end());
				}
				CurrentNode = &*Child;
			}
		}

		const auto SortChildren = [](auto&& Self, EditorContentFolderNode& Node) -> void
		{
			std::sort(Node.Children.begin(), Node.Children.end(), [](const EditorContentFolderNode& Left,
				const EditorContentFolderNode& Right)
			{
				return Left.Name < Right.Name;
			});
			for (EditorContentFolderNode& Child : Node.Children)
				Self(Self, Child);
		};
		SortChildren(SortChildren, m_FolderRoot);
		if (!IsKnownFolder(m_CurrentFolder))
			m_CurrentFolder = "Assets";
		if (m_SelectedFolder && !IsKnownFolder(*m_SelectedFolder))
			m_SelectedFolder.reset();
		RebuildFilteredAssetIndices();
	}

	void EditorContentBrowserState::ResetNavigation()
	{
		m_CurrentFolder = "Assets";
		m_SearchText.clear();
		m_IsGridView = true;
		m_SelectedFolder.reset();
		RebuildFilteredAssetIndices();
	}

	void EditorContentBrowserState::ClearProject()
	{
		m_Assets.clear();
		m_AssetPresentations.clear();
		m_SelectedAsset.reset();
		m_FolderRoot = { "Assets", "Assets", {} };
		ResetNavigation();
	}

	void EditorContentBrowserState::SetCurrentFolder(std::filesystem::path Folder)
	{
		Folder = Folder.lexically_normal();
		if (IsKnownFolder(Folder))
		{
			m_CurrentFolder = std::move(Folder);
			RebuildFilteredAssetIndices();
		}
	}

	void EditorContentBrowserState::RebaseFoldersAfterMove(
		const std::filesystem::path& Source,
		const std::filesystem::path& Destination)
	{
		const auto Rebase = [&Source, &Destination](const std::filesystem::path& Folder)
			-> std::optional<std::filesystem::path>
		{
			if (Folder == Source)
				return Destination;
			const std::filesystem::path Relative = Folder.lexically_relative(Source);
			if (Relative.empty() || Relative.is_absolute() ||
				(Relative.begin() != Relative.end() && *Relative.begin() == ".."))
				return std::nullopt;
			return Destination / Relative;
		};

		if (const auto Rebased = Rebase(m_CurrentFolder))
			m_CurrentFolder = *Rebased;
		if (m_SelectedFolder)
			if (const auto Rebased = Rebase(*m_SelectedFolder))
				m_SelectedFolder = *Rebased;
		RebuildFilteredAssetIndices();
	}

	void EditorContentBrowserState::RebuildFilteredAssetIndices()
	{
		m_FilteredAssetIndices.clear();
		m_FilteredAssetIndices.reserve(m_Assets.size());
		for (size_t Index = 0; Index < m_Assets.size(); ++Index)
			if (MatchesManagedAssetFolderSearch(
				m_Assets[Index].ProjectRelativePath, m_CurrentFolder, m_SearchText))
				m_FilteredAssetIndices.push_back(Index);
	}

	bool EditorContentBrowserState::IsKnownFolder(const std::filesystem::path& Folder) const
	{
		return ContainsFolder(m_FolderRoot, Folder.lexically_normal());
	}

	const EditorContentFolderNode* EditorContentBrowserState::FindFolder(
		const std::filesystem::path& Folder) const
	{
		return FindFolder(m_FolderRoot, Folder);
	}

	bool EditorContentBrowserState::ContainsFolder(
		const EditorContentFolderNode& Node,
		const std::filesystem::path& Candidate)
	{
		if (Node.RelativePath.lexically_normal() == Candidate)
			return true;
		return std::any_of(Node.Children.begin(), Node.Children.end(), [&](const EditorContentFolderNode& Child)
		{
			return ContainsFolder(Child, Candidate);
		});
	}

	const EditorContentFolderNode* EditorContentBrowserState::FindFolder(
		const EditorContentFolderNode& Node,
		const std::filesystem::path& Folder)
	{
		if (Node.RelativePath.lexically_normal() == Folder.lexically_normal())
			return &Node;
		for (const EditorContentFolderNode& Child : Node.Children)
			if (const EditorContentFolderNode* Found = FindFolder(Child, Folder))
				return Found;
		return nullptr;
	}
}
