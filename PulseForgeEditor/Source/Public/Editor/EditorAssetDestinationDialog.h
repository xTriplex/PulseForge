#pragma once

#include "Assets/AssetRegistry.h"

#include <filesystem>
#include <optional>

namespace PulseForge
{
	class Project;
}

namespace PulseForgeEditor
{
	class EditorAssetWorkflowState;
	class EditorContentBrowserState;
	class EditorStyle;

	enum class EditorAssetDestinationOperationKind
	{
		MoveAsset,
		DuplicateAsset,
		CreateScene,
		SaveSceneAs,
		CreatePrefab
	};

	struct EditorAssetDestinationOperation
	{
		EditorAssetDestinationOperationKind Kind;
		std::optional<PulseForge::AssetID> Asset;
		std::filesystem::path ProjectRelativeDestination;
		std::optional<PulseForge::UUID> PrefabRoot;
	};

	class EditorAssetDestinationDialog final
	{
	public:
		[[nodiscard]] static std::optional<EditorAssetDestinationOperation> Draw(EditorStyle& Style, EditorAssetWorkflowState& Workflow,
			const PulseForge::Project* Project, const EditorContentBrowserState& ContentBrowser);
	};
} // namespace PulseForgeEditor
