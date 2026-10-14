#include "Editor/EditorAssetWorkflowState.h"

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
	}

	void EditorAssetWorkflowState::BeginAssetDestination(const PulseForge::AssetRecord& Asset, bool Duplicate)
	{
		m_DestinationAsset = Asset.ID;
		m_IsDuplicate = Duplicate;
		m_CreateKind = ProjectAssetDestinationCreateKind::None;
		m_PrefabRoot.reset();
		m_DestinationFolder = PathToUtf8(Asset.ProjectRelativePath.parent_path());
		std::string Filename = PathToUtf8(Asset.ProjectRelativePath.filename());
		if (Duplicate)
			Filename = PathToUtf8(Asset.ProjectRelativePath.stem()) + " Copy" +
				PathToUtf8(Asset.ProjectRelativePath.extension());
		InitializeDestination(m_DestinationFolder, Filename);
	}

	void EditorAssetWorkflowState::BeginSceneDestination(
		bool SaveAs,
		const std::optional<PulseForge::AssetRecord>& Source)
	{
		m_DestinationAsset.reset();
		m_IsDuplicate = false;
		m_CreateKind = SaveAs
			? ProjectAssetDestinationCreateKind::SaveSceneAs
			: ProjectAssetDestinationCreateKind::NewScene;
		m_PrefabRoot.reset();
		m_DestinationFolder = "Assets";
		std::string Filename = SaveAs ? "Scene Copy.scene" : "NewScene.scene";
		if (SaveAs && Source)
		{
			m_DestinationFolder = PathToUtf8(Source->ProjectRelativePath.parent_path());
			Filename = PathToUtf8(Source->ProjectRelativePath.stem()) + " Copy.scene";
		}
		InitializeDestination(m_DestinationFolder, Filename);
	}

	void EditorAssetWorkflowState::BeginPrefabDestination(const PulseForge::UUID& Root)
	{
		m_DestinationAsset.reset();
		m_IsDuplicate = false;
		m_CreateKind = ProjectAssetDestinationCreateKind::Prefab;
		m_PrefabRoot = Root;
		m_DestinationFolder = "Assets";
		InitializeDestination(m_DestinationFolder, "NewPrefab.prefab");
	}

	void EditorAssetWorkflowState::DismissDestination() noexcept
	{
		m_DestinationAsset.reset();
		m_IsDuplicate = false;
		m_CreateKind = ProjectAssetDestinationCreateKind::None;
		m_PrefabRoot.reset();
		m_ValidationError.clear();
		m_DestinationDialogRequested = false;
	}

	bool EditorAssetWorkflowState::TakeDestinationDialogRequest() noexcept
	{
		return std::exchange(m_DestinationDialogRequested, false);
	}

	void EditorAssetWorkflowState::BeginDelete(const PulseForge::AssetID& Asset)
	{
		m_PendingDeleteAsset = Asset;
		m_DeleteDialogRequested = true;
	}

	void EditorAssetWorkflowState::DismissDelete() noexcept
	{
		m_PendingDeleteAsset.reset();
		m_DeleteDialogRequested = false;
	}

	bool EditorAssetWorkflowState::TakeDeleteDialogRequest() noexcept
	{
		return std::exchange(m_DeleteDialogRequested, false);
	}

	void EditorAssetWorkflowState::InitializeDestination(std::string_view Folder, std::string_view Filename)
	{
		m_DestinationFolder = Folder;
		CopyText(m_Filename, Filename);
		m_Search.fill('\0');
		m_ValidationError.clear();
		m_DestinationDialogRequested = true;
	}

	void EditorAssetWorkflowState::CopyText(std::span<char> Destination, std::string_view Source) noexcept
	{
		std::fill(Destination.begin(), Destination.end(), '\0');
		if (Destination.empty())
			return;
		std::copy_n(Source.data(), (std::min)(Source.size(), Destination.size() - 1), Destination.data());
	}
}
