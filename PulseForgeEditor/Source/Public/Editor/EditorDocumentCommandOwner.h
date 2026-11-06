#pragma once

#include "Editor/EditorDocumentManager.h"

#include <optional>
#include <span>

namespace PulseForgeEditor
{
	struct EditorDocumentCommandOwner final
	{
		std::optional<EditorDocumentIdentity> Document;
		std::optional<PulseForge::AssetID> MaterialDetailsAsset;
	};

	[[nodiscard]] inline EditorDocumentCommandOwner ResolveEditorDocumentCommandOwner(
		bool MainWindowFocused,
		bool MaterialDetailsFocused,
		std::optional<PulseForge::AssetID> MaterialDetailsAsset,
		std::optional<EditorDocumentIdentity> MainActiveDocument,
		std::span<const EditorDocumentIdentity> FocusedNativeDocuments)
	{
		if (FocusedNativeDocuments.size() == 1) return { FocusedNativeDocuments.front(), std::nullopt };
		if (!FocusedNativeDocuments.empty() || !MainWindowFocused) return {};
		if (MaterialDetailsFocused && MaterialDetailsAsset) return { std::nullopt, MaterialDetailsAsset };
		return { MainActiveDocument, std::nullopt };
	}
} // namespace PulseForgeEditor
