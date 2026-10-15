#pragma once

#include "Assets/MaterialAsset.h"
#include "Editor/EditorSnapshotHistory.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PulseForgeEditor
{
	class EditorMaterialDraftStore final
	{
	public:
		[[nodiscard]] bool ContainsDraft(std::string_view Key) const;
		[[nodiscard]] PulseForge::MaterialAssetDesc* FindDraft(std::string_view Key);
		[[nodiscard]] const PulseForge::MaterialAssetDesc* FindDraft(std::string_view Key) const;
		PulseForge::MaterialAssetDesc& PutDraft(std::string Key, PulseForge::MaterialAssetDesc Draft);
		void ReplaceDraft(std::string Key, PulseForge::MaterialAssetDesc Draft);

		[[nodiscard]] const std::string* FindError(std::string_view Key) const;
		void SetError(std::string Key, std::string Error);
		void ClearError(std::string_view Key);

		[[nodiscard]] bool IsDirty(std::string_view Key) const;
		[[nodiscard]] std::vector<std::string> GetDirtyKeys() const;
		[[nodiscard]] bool CanUndo(std::string_view Key) const;
		[[nodiscard]] bool CanRedo(std::string_view Key) const;
		[[nodiscard]] bool BeginEdit(std::string_view Key, const PulseForge::MaterialAssetDesc& InitialState);
		void FinalizePendingEdits(bool HasActiveImGuiItem);
		[[nodiscard]] std::optional<bool> ApplyHistoryStep(std::string_view Key, bool Redo);
		void MarkSaved(std::string_view Key);
		void Forget(std::string_view Key);
		void ClearProject(std::string_view ProjectKeyPrefix);

	private:
		struct Entry final
		{
			std::optional<PulseForge::MaterialAssetDesc> Draft;
			EditorSnapshotHistory<PulseForge::MaterialAssetDesc> History;
			std::optional<PulseForge::MaterialAssetDesc> EditStart;
			std::optional<std::string> Error;
			bool Dirty = false;
		};

		[[nodiscard]] Entry* FindEntry(std::string_view Key);
		[[nodiscard]] const Entry* FindEntry(std::string_view Key) const;
		[[nodiscard]] static bool KeyMatchesProject(std::string_view Key, std::string_view Prefix) noexcept;
		std::unordered_map<std::string, Entry> m_Entries;
	};
} // namespace PulseForgeEditor
