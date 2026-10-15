#pragma once

#include "Assets/AssetRegistry.h"
#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace PulseForgeEditor
{
	class EditorStyle;

	enum class EditorMainMenuActionKind : uint8_t
	{
		NewProject,
		OpenProject,
		CloseProject,
		Exit,
		Undo,
		Redo,
		Cut,
		Copy,
		Paste,
		Duplicate,
		Delete,
		SelectAll,
		Rename,
		FocusSelection,
		SnapSelection,
		HideSelection,
		ShowAll,
		IsolateSelection,
		RestoreIsolation,
		NewScene,
		SaveScene,
		SaveSceneAs,
		OpenScene,
		SetPanelVisible,
		SetOpeningPreference,
		ResetLayout
	};

	struct EditorMainMenuAction final
	{
		EditorMainMenuActionKind Kind = EditorMainMenuActionKind::Exit;
		std::optional<PulseForge::AssetID> Asset;
		EditorPanel Panel = EditorPanel::SceneViewport;
		EditorDocumentOpenPreference OpeningPreference = EditorDocumentOpenPreference::NewNativeWindow;
		bool PanelVisible = false;
	};

	struct EditorMainMenuState final
	{
		std::span<const PulseForge::AssetRecord> Assets;
		std::optional<PulseForge::AssetID> ActiveSceneAsset;
		std::array<bool, static_cast<size_t>(EditorPanel::Count)> PanelVisibility{};
		EditorDocumentOpenPreference OpeningPreference = EditorDocumentOpenPreference::NewNativeWindow;
		std::string UndoLabel = "Undo";
		std::string RedoLabel = "Redo";
		bool ProjectOpen = false;
		bool EditMenuEnabled = false;
		bool CanEditScene = false;
		bool CanUndo = false;
		bool CanRedo = false;
		bool HasSelection = false;
		bool CanPaste = false;
		bool HasSceneEntities = false;
		bool CanRename = false;
		bool CanFocusSelection = false;
		bool CanSnapSelection = false;
		bool CanHideSelection = false;
		bool CanShowAll = false;
		bool CanIsolateSelection = false;
		bool CanRestoreIsolation = false;
		bool CanSaveScene = false;
		bool CanSaveSceneAs = false;
	};

	struct EditorMainMenuDrawResult final
	{
		std::optional<EditorMainMenuAction> Action;
		bool FocusMainDocumentHost = false;
	};

	class EditorMainMenuPanel final
	{
	public:
		[[nodiscard]] EditorMainMenuDrawResult Draw(const EditorMainMenuState& State, const EditorStyle& Style) const;
	};
} // namespace PulseForgeEditor
