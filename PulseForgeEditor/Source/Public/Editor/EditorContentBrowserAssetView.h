#pragma once

#include "Editor/EditorContentBrowserState.h"

#include <functional>

namespace PulseForge
{
	class Project;
}

namespace PulseForgeEditor
{
	class EditorAssetPreviewService;
	class EditorStyle;

	enum class EditorContentBrowserAssetAction
	{
		OpenScene,
		ShowMaterialDetails,
		Move,
		Duplicate,
		InstantiatePrefab,
		SetStartupScene,
		Delete
	};

	using EditorContentBrowserAssetActionHandler = std::function<void(EditorContentBrowserAssetAction, const PulseForge::AssetID&)>;

	class EditorContentBrowserAssetView final
	{
	public:
		void DrawList(EditorContentBrowserState& State, const PulseForge::Project* Project, bool SceneAvailable, EditorStyle& Style,
			EditorContentBrowserAssetActionHandler ActionHandler, std::function<void()>& DeferredAction);
		void DrawGrid(EditorContentBrowserState& State, const PulseForge::Project* Project, bool SceneAvailable,
			EditorAssetPreviewService& Previews, EditorStyle& Style, EditorContentBrowserAssetActionHandler ActionHandler,
			std::function<void()>& DeferredAction);

	private:
		void HandleAssetItem(EditorContentBrowserState& State, const PulseForge::Project* Project, const PulseForge::AssetRecord& Asset,
			const EditorContentBrowserAssetPresentation& Presentation, bool Activated, bool SceneAvailable, EditorStyle& Style,
			const EditorContentBrowserAssetActionHandler& ActionHandler, std::function<void()>& DeferredAction);
	};
} // namespace PulseForgeEditor
