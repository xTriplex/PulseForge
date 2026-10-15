#pragma once

#include "Assets/AssetRegistry.h"

#include <optional>
#include <vector>

namespace PulseForge
{
	class Project;
}

namespace PulseForgeEditor
{
	class EditorAssetSelector;
	class EditorMaterialDraftController;
	class EditorStyle;

	class EditorMaterialDetailsActionSink
	{
	public:
		virtual ~EditorMaterialDetailsActionSink() = default;
		virtual bool SaveMaterialFromDetails(const PulseForge::AssetID& Asset) = 0;
		virtual bool RevertMaterialFromDetails(const PulseForge::AssetID& Asset) = 0;
		virtual void MaterialDetailsDraftEdited(
			const PulseForge::UUID& Project,
			const PulseForge::AssetID& Asset) = 0;
	};

	class EditorMaterialDetailsPanel final
	{
	public:
		void DrawContents(
			const PulseForge::Project* Project,
			std::optional<PulseForge::AssetID>& SelectedAsset,
			const std::vector<PulseForge::AssetRecord>& Assets,
			EditorMaterialDraftController& Drafts,
			EditorAssetSelector& AssetSelector,
			EditorStyle& Style,
			EditorMaterialDetailsActionSink& Actions);

	private:
		[[nodiscard]] bool DrawMaterialProperties(
			const PulseForge::Project& Project,
			const PulseForge::AssetRecord& Asset,
			const std::vector<PulseForge::AssetRecord>& Assets,
			EditorMaterialDraftController& Drafts,
			EditorAssetSelector& AssetSelector,
			EditorStyle& Style,
			EditorMaterialDetailsActionSink& Actions);
		void DrawMaterialActions(
			const PulseForge::AssetRecord& Asset,
			const PulseForge::Project& Project,
			EditorMaterialDraftController& Drafts,
			EditorStyle& Style,
			EditorMaterialDetailsActionSink& Actions);
	};
}
