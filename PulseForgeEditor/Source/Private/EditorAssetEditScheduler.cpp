#include "Editor/EditorAssetEditScheduler.h"

#include <algorithm>
#include <utility>

namespace PulseForgeEditor
{
	std::optional<EditorAssetEditDocumentConflict> FindDirtyAssetEditDocumentConflict(
		const PulseForge::AssetEditMovePlan& Plan, std::span<const EditorDocumentIdentity> DirtyDocuments)
	{
		for (const PulseForge::AssetEditRewrittenSource& Rewrite : Plan.GetRewrittenSources())
			if (std::any_of(DirtyDocuments.begin(), DirtyDocuments.end(), [&](const EditorDocumentIdentity& Identity)
					{ return Identity.Project == Plan.GetProjectID() && Identity.Asset == Rewrite.Asset; }))
				return EditorAssetEditDocumentConflict{ Rewrite.Asset, Rewrite.ProjectRelativePath };
		return std::nullopt;
	}

	void EditorAssetEditScheduler::Enqueue(EditorDeferredOperationQueue& Operations, PulseForge::AssetEditTransaction::PlanPtr Plan,
		EditorAssetEditSchedulerCallbacks Callbacks)
	{
		if (!Plan || !Callbacks.IsCurrent || !Callbacks.FinishActiveEdit || !Callbacks.Apply) return;

		const EditorAssetEditExecutionContext Context{ Plan->GetProjectID(), Plan->GetProjectRoot().lexically_normal(),
			Plan->GetCatalogGeneration() };
		Operations.Enqueue(
			[Plan = std::move(Plan), Context, Callbacks = std::move(Callbacks)]() mutable
			{
				if (!Callbacks.IsCurrent(Context))
				{
					if (Callbacks.RejectStale) Callbacks.RejectStale();
					return;
				}
				if (!Callbacks.FinishActiveEdit()) return;
				Callbacks.Apply(std::move(Plan));
			});
	}
} // namespace PulseForgeEditor
