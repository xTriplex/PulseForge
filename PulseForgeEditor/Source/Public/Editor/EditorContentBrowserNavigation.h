#pragma once

#include "Editor/EditorContentBrowserState.h"

#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	class EditorStyle;

	class EditorContentBrowserNavigationActions
	{
	public:
		virtual ~EditorContentBrowserNavigationActions() = default;
		virtual void QueueAssetMoveAfterSave(
			const PulseForge::AssetID& Asset,
			const std::filesystem::path& Destination) = 0;
		virtual void QueueFolderMoveAfterSave(
			const std::filesystem::path& Source,
			const std::filesystem::path& Destination) = 0;
	};

	class EditorContentBrowserNavigation final
	{
	public:
		void DrawFolderTree(
			EditorContentBrowserState& State,
			EditorStyle& Style,
			EditorContentBrowserNavigationActions& Actions,
			std::function<void()>& DeferredAction);
		void DrawBreadcrumbs(EditorContentBrowserState& State, EditorStyle& Style);
		void DrawCompactFolderPicker(EditorContentBrowserState& State);
		void DrawFolderItems(
			EditorContentBrowserState& State,
			const std::vector<EditorContentFolderNode>& Folders,
			EditorStyle& Style,
			EditorContentBrowserNavigationActions& Actions,
			std::function<void()>& DeferredAction);

	private:
		void DrawFolderNode(
			EditorContentBrowserState& State,
			const EditorContentFolderNode& Node,
			EditorStyle& Style,
			EditorContentBrowserNavigationActions& Actions,
			std::function<void()>& DeferredAction,
			bool IsRoot = false);
		void BeginFolderDragSource(const std::filesystem::path& Folder, std::string_view Name);
		void HandleFolderDropTarget(
			const EditorContentBrowserState& State,
			const std::filesystem::path& Destination,
			EditorContentBrowserNavigationActions& Actions,
			std::function<void()>& DeferredAction);
		void DrawFolderList(
			EditorContentBrowserState& State,
			const std::vector<EditorContentFolderNode>& Folders,
			EditorStyle& Style,
			EditorContentBrowserNavigationActions& Actions,
			std::function<void()>& DeferredAction);
		void DrawFolderGrid(
			EditorContentBrowserState& State,
			const std::vector<EditorContentFolderNode>& Folders,
			EditorStyle& Style,
			EditorContentBrowserNavigationActions& Actions,
			std::function<void()>& DeferredAction);
	};
}
