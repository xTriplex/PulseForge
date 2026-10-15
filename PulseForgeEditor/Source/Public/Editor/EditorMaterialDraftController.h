#pragma once

#include "Assets/MaterialAsset.h"
#include "Assets/Project.h"
#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorMaterialDraftStore.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	class EditorMaterialDraftController final
	{
	public:
		using DraftResult = std::expected<std::reference_wrapper<PulseForge::MaterialAssetDesc>, std::string>;

		[[nodiscard]] PulseForge::MaterialAssetDesc* FindDraft(const PulseForge::Project& Project, const PulseForge::AssetID& Asset);
		[[nodiscard]] bool ContainsDraft(const PulseForge::Project& Project, const PulseForge::AssetID& Asset) const;
		[[nodiscard]] DraftResult LoadDraft(const PulseForge::Project& Project, const PulseForge::AssetID& Asset);
		void ReplaceDraft(const PulseForge::Project& Project, const PulseForge::AssetID& Asset, PulseForge::MaterialAssetDesc Draft);

		[[nodiscard]] const std::string* FindError(const PulseForge::Project& Project, const PulseForge::AssetID& Asset) const;
		void ClearLoadError(const PulseForge::Project& Project, const PulseForge::AssetID& Asset);
		[[nodiscard]] bool IsDirty(const PulseForge::Project& Project, const PulseForge::AssetID& Asset) const;
		[[nodiscard]] bool CanUndo(const PulseForge::Project& Project, const PulseForge::AssetID& Asset) const;
		[[nodiscard]] bool CanRedo(const PulseForge::Project& Project, const PulseForge::AssetID& Asset) const;
		[[nodiscard]] bool BeginEdit(
			const PulseForge::Project& Project, const PulseForge::AssetID& Asset, const PulseForge::MaterialAssetDesc& InitialState);
		void FinalizePendingEdits(bool HasActiveImGuiItem);
		[[nodiscard]] std::optional<bool> ApplyHistoryStep(
			const PulseForge::Project& Project, const PulseForge::AssetID& Asset, bool Redo, EditorDocumentManager& Documents);

		[[nodiscard]] std::expected<std::filesystem::path, std::string> Save(
			PulseForge::Project& Project, const PulseForge::AssetID& Asset, EditorDocumentManager& Documents);
		[[nodiscard]] std::expected<void, std::string> Reload(
			PulseForge::Project& Project, const PulseForge::AssetID& Asset, EditorDocumentManager& Documents);
		[[nodiscard]] std::vector<PulseForge::AssetID> GetDirtyAssets(const PulseForge::Project& Project) const;
		void Forget(const PulseForge::Project& Project, const PulseForge::AssetID& Asset);
		void ClearProject(const PulseForge::Project& Project);

	private:
		[[nodiscard]] static std::string MakeKey(const PulseForge::Project& Project, const PulseForge::AssetID& Asset);
		EditorMaterialDraftStore m_Drafts;
	};
} // namespace PulseForgeEditor
