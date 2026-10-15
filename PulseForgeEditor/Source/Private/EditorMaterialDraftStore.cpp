#include "Editor/EditorMaterialDraftStore.h"

#include <utility>

namespace PulseForgeEditor
{
	bool EditorMaterialDraftStore::ContainsDraft(std::string_view Key) const
	{
		const Entry* const Found = FindEntry(Key);
		return Found && Found->Draft.has_value();
	}

	PulseForge::MaterialAssetDesc* EditorMaterialDraftStore::FindDraft(std::string_view Key)
	{
		Entry* const Found = FindEntry(Key);
		return Found && Found->Draft ? &*Found->Draft : nullptr;
	}

	const PulseForge::MaterialAssetDesc* EditorMaterialDraftStore::FindDraft(std::string_view Key) const
	{
		const Entry* const Found = FindEntry(Key);
		return Found && Found->Draft ? &*Found->Draft : nullptr;
	}

	PulseForge::MaterialAssetDesc& EditorMaterialDraftStore::PutDraft(
		std::string Key,
		PulseForge::MaterialAssetDesc Draft)
	{
		Entry& State = m_Entries[std::move(Key)];
		State.Draft = std::move(Draft);
		if (State.History.IsEmpty())
			State.History.Reset(*State.Draft);
		return *State.Draft;
	}

	void EditorMaterialDraftStore::ReplaceDraft(std::string Key, PulseForge::MaterialAssetDesc Draft)
	{
		Entry& State = m_Entries[std::move(Key)];
		State.Draft = std::move(Draft);
		State.History.Reset(*State.Draft);
		State.EditStart.reset();
		State.Error.reset();
		State.Dirty = false;
	}

	const std::string* EditorMaterialDraftStore::FindError(std::string_view Key) const
	{
		const Entry* const Found = FindEntry(Key);
		return Found && Found->Error ? &*Found->Error : nullptr;
	}

	void EditorMaterialDraftStore::SetError(std::string Key, std::string Error)
	{
		m_Entries[std::move(Key)].Error = std::move(Error);
	}

	void EditorMaterialDraftStore::ClearError(std::string_view Key)
	{
		if (Entry* const Found = FindEntry(Key))
			Found->Error.reset();
	}

	bool EditorMaterialDraftStore::IsDirty(std::string_view Key) const
	{
		const Entry* const Found = FindEntry(Key);
		return Found && Found->Dirty;
	}

	std::vector<std::string> EditorMaterialDraftStore::GetDirtyKeys() const
	{
		std::vector<std::string> Keys;
		for (const auto& [Key, State] : m_Entries)
			if (State.Dirty)
				Keys.push_back(Key);
		return Keys;
	}

	bool EditorMaterialDraftStore::CanUndo(std::string_view Key) const
	{
		const Entry* const Found = FindEntry(Key);
		return Found && Found->Draft && Found->History.CanUndo();
	}

	bool EditorMaterialDraftStore::CanRedo(std::string_view Key) const
	{
		const Entry* const Found = FindEntry(Key);
		return Found && Found->Draft && Found->History.CanRedo();
	}

	bool EditorMaterialDraftStore::BeginEdit(
		std::string_view Key,
		const PulseForge::MaterialAssetDesc& InitialState)
	{
		Entry* const Found = FindEntry(Key);
		if (!Found || !Found->Draft)
			return false;
		const bool NewEdit = !Found->EditStart.has_value();
		if (NewEdit)
			Found->EditStart = InitialState;
		Found->Dirty = true;
		return NewEdit;
	}

	void EditorMaterialDraftStore::FinalizePendingEdits(bool HasActiveImGuiItem)
	{
		if (HasActiveImGuiItem)
			return;
		for (auto& [Key, State] : m_Entries)
		{
			(void)Key;
			if (!State.EditStart)
				continue;
			if (State.Draft)
			{
				if (State.History.IsEmpty())
					State.History.Reset(*State.EditStart);
				State.History.Record(*State.Draft);
				State.Dirty = State.History.IsDirty();
			}
			State.EditStart.reset();
		}
	}

	std::optional<bool> EditorMaterialDraftStore::ApplyHistoryStep(std::string_view Key, bool Redo)
	{
		Entry* const Found = FindEntry(Key);
		if (!Found || !Found->Draft || Found->History.IsEmpty())
			return std::nullopt;
		const bool Changed = Redo ? Found->History.Redo() : Found->History.Undo();
		if (Changed)
		{
			*Found->Draft = Found->History.GetCurrent();
			Found->Dirty = Found->History.IsDirty();
		}
		return Changed;
	}

	void EditorMaterialDraftStore::MarkSaved(std::string_view Key)
	{
		if (Entry* const Found = FindEntry(Key))
		{
			Found->Dirty = false;
			Found->Error.reset();
			if (!Found->History.IsEmpty())
				Found->History.MarkSaved();
		}
	}

	void EditorMaterialDraftStore::Forget(std::string_view Key)
	{
		m_Entries.erase(std::string(Key));
	}

	void EditorMaterialDraftStore::ClearProject(std::string_view ProjectKeyPrefix)
	{
		for (auto Entry = m_Entries.begin(); Entry != m_Entries.end();)
		{
			if (KeyMatchesProject(Entry->first, ProjectKeyPrefix))
				Entry = m_Entries.erase(Entry);
			else
				++Entry;
		}
	}

	EditorMaterialDraftStore::Entry* EditorMaterialDraftStore::FindEntry(std::string_view Key)
	{
		const auto Found = m_Entries.find(std::string(Key));
		return Found == m_Entries.end() ? nullptr : &Found->second;
	}

	const EditorMaterialDraftStore::Entry* EditorMaterialDraftStore::FindEntry(std::string_view Key) const
	{
		const auto Found = m_Entries.find(std::string(Key));
		return Found == m_Entries.end() ? nullptr : &Found->second;
	}

	bool EditorMaterialDraftStore::KeyMatchesProject(std::string_view Key, std::string_view Prefix) noexcept
	{
		return Key.starts_with(Prefix);
	}
}
