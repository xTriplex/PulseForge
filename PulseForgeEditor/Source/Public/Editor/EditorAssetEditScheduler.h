#pragma once

#include "Assets/AssetEditTransaction.h"
#include "Editor/EditorDeferredOperationQueue.h"
#include "Editor/EditorDocumentManager.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

namespace PulseForgeEditor
{
	struct EditorAssetEditExecutionContext
	{
		PulseForge::UUID ProjectID;
		std::filesystem::path ProjectRoot;
		uint64_t Generation = 0;
	};

	struct EditorAssetEditSchedulerCallbacks
	{
		std::function<bool(const EditorAssetEditExecutionContext&)> IsCurrent;
		std::function<bool()> FinishActiveEdit;
		std::function<void(PulseForge::AssetEditTransaction::PlanPtr)> Apply;
		std::function<void()> RejectStale;
	};

	struct EditorAssetEditDocumentConflict
	{
		PulseForge::AssetID Asset;
		std::filesystem::path RewrittenSource;
	};

	[[nodiscard]] std::optional<EditorAssetEditDocumentConflict> FindDirtyAssetEditDocumentConflict(
		const PulseForge::AssetEditMovePlan& Plan, std::span<const EditorDocumentIdentity> DirtyDocuments);

	// Retains an accepted immutable plan until the editor's established safe update phase executes it.
	class EditorAssetEditScheduler final
	{
	public:
		static void Enqueue(EditorDeferredOperationQueue& Operations, PulseForge::AssetEditTransaction::PlanPtr Plan,
			EditorAssetEditSchedulerCallbacks Callbacks);
	};
} // namespace PulseForgeEditor
