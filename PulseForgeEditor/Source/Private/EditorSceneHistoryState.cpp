#include "Editor/EditorSceneHistoryState.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <charconv>
#include <unordered_set>
#include <vector>

namespace PulseForgeEditor
{
	std::expected<std::string, std::string> EditorSceneHistoryState::CaptureSnapshot(
		const PulseForge::Scene& SceneValue, const EditorSceneWorkspaceState& Workspace)
	{
		auto Serialized = PulseForge::SceneSerializer::Serialize(SceneValue);
		if (!Serialized) return std::unexpected(Serialized.error().Message);

		std::string State = std::to_string(Serialized->size());
		State.push_back('\n');
		State.append(*Serialized);
		State.push_back('\n');
		if (Workspace.Selection().GetPrimary()) State.append(Workspace.Selection().GetPrimary()->ToString());
		State.push_back('\n');
		State.append(std::to_string(Workspace.Selection().GetEntities().size()));
		State.push_back('\n');
		for (const PulseForge::UUID& Identifier : Workspace.Selection().GetEntities())
		{
			State.append(Identifier.ToString());
			State.push_back('\n');
		}
		const auto AppendVisibilitySet = [&State](const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>& Identifiers)
		{
			std::vector<std::string> SortedIdentifiers;
			SortedIdentifiers.reserve(Identifiers.size());
			for (const PulseForge::UUID& Identifier : Identifiers)
				SortedIdentifiers.push_back(Identifier.ToString());
			std::sort(SortedIdentifiers.begin(), SortedIdentifiers.end());
			State.append(std::to_string(SortedIdentifiers.size()));
			State.push_back('\n');
			for (const std::string& Identifier : SortedIdentifiers)
			{
				State.append(Identifier);
				State.push_back('\n');
			}
		};
		AppendVisibilitySet(Workspace.Visibility().GetHiddenEntities());
		const auto IsolationBaseline = Workspace.Visibility().GetIsolationBaseline();
		State.append(IsolationBaseline ? "1\n" : "0\n");
		if (IsolationBaseline) AppendVisibilitySet(*IsolationBaseline);
		return State;
	}

	std::expected<void, std::string> EditorSceneHistoryState::RestoreSnapshot(
		std::string_view State, PulseForge::Scene& SceneValue, EditorSceneWorkspaceState& Workspace)
	{
		const size_t LengthTerminator = State.find('\n');
		if (LengthTerminator == std::string_view::npos) return std::unexpected("Scene history snapshot is malformed");
		size_t SceneLength = 0;
		const auto ParsedLength = std::from_chars(State.data(), State.data() + LengthTerminator, SceneLength);
		if (ParsedLength.ec != std::errc{} || ParsedLength.ptr != State.data() + LengthTerminator || State.size() < LengthTerminator + 2 ||
			SceneLength > State.size() - LengthTerminator - 2)
			return std::unexpected("Scene history snapshot has an invalid payload length");
		const size_t SceneStart = LengthTerminator + 1;
		const std::string_view SceneData = State.substr(SceneStart, SceneLength);
		const size_t SelectionStart = SceneStart + SceneLength + 1;
		if (State[SelectionStart - 1] != '\n') return std::unexpected("Scene history snapshot has an invalid editor-state separator");
		const std::string_view EditorState = State.substr(SelectionStart);
		const size_t PrimaryTerminator = EditorState.find('\n');
		if (PrimaryTerminator == std::string_view::npos)
			return std::unexpected("Scene history snapshot has no selection primary separator");
		const std::string_view PrimaryData = EditorState.substr(0, PrimaryTerminator);
		const size_t CountStart = PrimaryTerminator + 1;
		const size_t CountTerminator = EditorState.find('\n', CountStart);
		if (CountTerminator == std::string_view::npos) return std::unexpected("Scene history snapshot has no selection count separator");
		size_t SelectionCount = 0;
		const auto ParsedCount = std::from_chars(EditorState.data() + CountStart, EditorState.data() + CountTerminator, SelectionCount);
		if (ParsedCount.ec != std::errc{} || ParsedCount.ptr != EditorState.data() + CountTerminator ||
			SelectionCount > (EditorState.size() - CountTerminator - 1) / 37)
			return std::unexpected("Scene history snapshot has an invalid selection count");
		std::vector<PulseForge::UUID> Selection;
		Selection.reserve(SelectionCount);
		size_t SelectionCursor = CountTerminator + 1;
		for (size_t Index = 0; Index < SelectionCount; ++Index)
		{
			const size_t IdentifierEnd = EditorState.find('\n', SelectionCursor);
			if (IdentifierEnd == std::string_view::npos)
				return std::unexpected("Scene history snapshot contains a truncated selection identifier");
			const auto Identifier = PulseForge::UUID::Parse(EditorState.substr(SelectionCursor, IdentifierEnd - SelectionCursor));
			if (!Identifier || std::find(Selection.begin(), Selection.end(), *Identifier) != Selection.end())
				return std::unexpected("Scene history snapshot contains an invalid or duplicate selection identifier");
			Selection.push_back(*Identifier);
			SelectionCursor = IdentifierEnd + 1;
		}
		const auto ParseVisibilitySet =
			[&](size_t& Cursor,
				std::string_view FieldName) -> std::expected<std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>, std::string>
		{
			const size_t CountEnd = EditorState.find('\n', Cursor);
			if (CountEnd == std::string_view::npos)
				return std::unexpected("Scene history snapshot has no " + std::string(FieldName) + " count");
			size_t Count = 0;
			const auto Parsed = std::from_chars(EditorState.data() + Cursor, EditorState.data() + CountEnd, Count);
			if (Parsed.ec != std::errc{} || Parsed.ptr != EditorState.data() + CountEnd || Count > (EditorState.size() - CountEnd - 1) / 37)
				return std::unexpected("Scene history snapshot has an invalid " + std::string(FieldName) + " count");
			Cursor = CountEnd + 1;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Identifiers;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				const size_t IdentifierEnd = EditorState.find('\n', Cursor);
				if (IdentifierEnd == std::string_view::npos)
					return std::unexpected("Scene history snapshot has a truncated " + std::string(FieldName) + " identifier");
				const auto Identifier = PulseForge::UUID::Parse(EditorState.substr(Cursor, IdentifierEnd - Cursor));
				if (!Identifier || !Identifiers.insert(*Identifier).second)
					return std::unexpected("Scene history snapshot has an invalid or duplicate " + std::string(FieldName) + " identifier");
				Cursor = IdentifierEnd + 1;
			}
			return Identifiers;
		};
		auto HiddenEntities = ParseVisibilitySet(SelectionCursor, "hidden-entity");
		if (!HiddenEntities) return std::unexpected(HiddenEntities.error());
		const size_t IsolationFlagEnd = EditorState.find('\n', SelectionCursor);
		if (IsolationFlagEnd == std::string_view::npos) return std::unexpected("Scene history snapshot has no isolation-state flag");
		const std::string_view IsolationFlag = EditorState.substr(SelectionCursor, IsolationFlagEnd - SelectionCursor);
		if (IsolationFlag != "0" && IsolationFlag != "1")
			return std::unexpected("Scene history snapshot has an invalid isolation-state flag");
		SelectionCursor = IsolationFlagEnd + 1;
		std::optional<std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>> IsolationBaseline;
		if (IsolationFlag == "1")
		{
			auto ParsedBaseline = ParseVisibilitySet(SelectionCursor, "isolation-baseline");
			if (!ParsedBaseline) return std::unexpected(ParsedBaseline.error());
			IsolationBaseline = std::move(*ParsedBaseline);
		}
		if (SelectionCursor != EditorState.size()) return std::unexpected("Scene history snapshot contains trailing editor-state data");
		std::optional<PulseForge::UUID> Primary;
		if (!PrimaryData.empty())
		{
			const auto ParsedPrimary = PulseForge::UUID::Parse(PrimaryData);
			if (!ParsedPrimary) return std::unexpected("Scene history snapshot contains an invalid selection primary");
			Primary = *ParsedPrimary;
			if (!Primary || std::find(Selection.begin(), Selection.end(), *Primary) == Selection.end())
				return std::unexpected("Scene history snapshot contains an invalid selection primary");
		}
		if ((Selection.empty() && Primary) || (!Selection.empty() && !Primary))
			return std::unexpected("Scene history snapshot selection primary does not match its selection");
		if (auto Result = PulseForge::SceneSerializer::Deserialize(SceneData, SceneValue); !Result)
			return std::unexpected(Result.error().Message);
		std::erase_if(
			Selection, [&SceneValue](const PulseForge::UUID& Identifier) { return !SceneValue.FindEntity(Identifier).has_value(); });
		if (Primary && std::find(Selection.begin(), Selection.end(), *Primary) == Selection.end()) Primary.reset();
		const auto RemoveMissingEntities = [&SceneValue](std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>& Identifiers)
		{
			std::erase_if(
				Identifiers, [&SceneValue](const PulseForge::UUID& Identifier) { return !SceneValue.FindEntity(Identifier).has_value(); });
		};
		RemoveMissingEntities(*HiddenEntities);
		if (IsolationBaseline) RemoveMissingEntities(*IsolationBaseline);
		Workspace.Selection().Set(std::move(Selection), Primary);
		Workspace.Visibility().RestoreState(std::move(*HiddenEntities), std::move(IsolationBaseline));
		return {};
	}

	void EditorSceneHistoryState::Reset(const PulseForge::Scene* SceneValue)
	{
		m_Commands.Reset();
		m_InspectorHistoryItem = 0;
		m_SavedSceneSnapshot.reset();
		if (SceneValue)
			if (auto Serialized = PulseForge::SceneSerializer::Serialize(*SceneValue)) m_SavedSceneSnapshot = std::move(*Serialized);
		UpdateDirty(SceneValue);
	}

	void EditorSceneHistoryState::UpdateDirty(const PulseForge::Scene* SceneValue)
	{
		if (!SceneValue)
		{
			m_Dirty = false;
			return;
		}
		auto Serialized = PulseForge::SceneSerializer::Serialize(*SceneValue);
		m_Dirty = !Serialized || !m_SavedSceneSnapshot || *Serialized != *m_SavedSceneSnapshot;
	}

	void EditorSceneHistoryState::MarkSaved(const PulseForge::Scene* SceneValue)
	{
		m_Commands.MarkSaved();
		if (SceneValue)
			if (auto Serialized = PulseForge::SceneSerializer::Serialize(*SceneValue)) m_SavedSceneSnapshot = std::move(*Serialized);
		UpdateDirty(SceneValue);
	}

	std::expected<void, std::string> EditorSceneHistoryState::Begin(std::string Description, std::string StateBefore)
	{
		return m_Commands.Begin(std::move(Description), std::move(StateBefore));
	}

	std::expected<bool, std::string> EditorSceneHistoryState::Commit(std::string StateAfter)
	{
		return m_Commands.Commit(std::move(StateAfter));
	}
} // namespace PulseForgeEditor
