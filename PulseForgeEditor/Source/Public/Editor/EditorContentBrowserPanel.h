#pragma once

#include "Editor/EditorContentBrowserAssetView.h"
#include "Editor/EditorContentBrowserNavigation.h"

namespace PulseForge
{
	class Project;
}

namespace PulseForgeEditor
{
	class EditorAssetPreviewService;
	class EditorStyle;

	enum class EditorContentBrowserToolbarAction
	{
		Import,
		Refresh
	};

	class EditorContentBrowserPanelActions : public EditorContentBrowserNavigationActions
	{
	public:
		virtual void HandleContentBrowserToolbarAction(EditorContentBrowserToolbarAction Action) = 0;
		virtual void HandleContentBrowserAssetAction(
			EditorContentBrowserAssetAction Action,
			const PulseForge::AssetID& Identifier) = 0;
		[[nodiscard]] virtual bool IsContentBrowserMaterialDraftDirty(
			const PulseForge::AssetID& Identifier) const = 0;
		[[nodiscard]] virtual bool IsProtectedSceneAsset(const PulseForge::AssetID& Identifier) const = 0;
	};

	class EditorContentBrowserPanel final
	{
	public:
		void DrawContents(
			EditorContentBrowserState& State,
			const PulseForge::Project& Project,
			bool SceneAvailable,
			EditorAssetPreviewService& Previews,
			EditorStyle& Style,
			EditorContentBrowserPanelActions& Actions);
		void FocusSearch() noexcept { m_FocusSearchNextFrame = true; }

	private:
		EditorContentBrowserNavigation m_Navigation;
		EditorContentBrowserAssetView m_AssetView;
		bool m_FocusSearchNextFrame = false;
		bool m_DetailsExpandedCompact = false;
		void DrawSelectedAssetDetails(
			EditorContentBrowserState& State,
			const PulseForge::Project& Project,
			bool SceneAvailable,
			EditorStyle& Style,
			EditorContentBrowserPanelActions& Actions);
	};
}
