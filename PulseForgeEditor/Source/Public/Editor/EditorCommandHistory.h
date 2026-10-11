#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	// Bounded state history for editor transactions. Callers capture state only at
	// transaction boundaries, so pointer-move events never allocate snapshots.
	class EditorCommandHistory final
	{
	public:
		static constexpr size_t DefaultMaximumCommands = 128;
		static constexpr size_t DefaultMaximumBytes = 64u * 1024u * 1024u;
		static constexpr size_t MaximumTransactionBytes = 32u * 1024u * 1024u;

		struct CommandView
		{
			std::string_view Description;
		};

		explicit EditorCommandHistory(
			size_t MaximumCommands = DefaultMaximumCommands,
			size_t MaximumBytes = DefaultMaximumBytes) noexcept
			: m_MaximumCommands(MaximumCommands), m_MaximumBytes(MaximumBytes)
		{
		}

		void Reset() noexcept
		{
			m_Commands.clear();
			m_ActiveDescription.clear();
			m_ActiveBefore.clear();
			m_Active = false;
			m_Cursor = 0;
			m_Bytes = 0;
			m_CurrentState = NewStateIdentifier();
			m_SavedState = m_CurrentState;
		}

		[[nodiscard]] std::expected<void, std::string> Begin(std::string Description, std::string StateBefore)
		{
			if (m_Active)
				return std::unexpected("An editor transaction is already active");
			if (Description.empty())
				return std::unexpected("An editor transaction requires a description");
			if (StateBefore.size() > MaximumTransactionBytes)
				return std::unexpected("The editor transaction exceeds the configured snapshot limit");

			m_ActiveDescription = std::move(Description);
			m_ActiveBefore = std::move(StateBefore);
			m_Active = true;
			return {};
		}

		[[nodiscard]] std::expected<bool, std::string> Commit(std::string StateAfter)
		{
			if (!m_Active)
				return std::unexpected("No editor transaction is active");
			if (StateAfter.size() > MaximumTransactionBytes)
				return std::unexpected("The editor transaction exceeds the configured snapshot limit");
			if (StateAfter == m_ActiveBefore)
			{
				ClearActive();
				return false;
			}

			const size_t EntryBytes = m_ActiveBefore.size() + StateAfter.size();
			if (EntryBytes > m_MaximumBytes || m_MaximumCommands == 0)
				return std::unexpected("The editor transaction does not fit in the bounded history");

			size_t RetainedBytes = m_Bytes;
			for (size_t Index = m_Cursor; Index < m_Commands.size(); ++Index)
				RetainedBytes -= m_Commands[Index].Before.size() + m_Commands[Index].After.size();
			if (EntryBytes > m_MaximumBytes - RetainedBytes)
			{
				for (size_t Index = 0; Index < m_Cursor && EntryBytes > m_MaximumBytes - RetainedBytes; ++Index)
					RetainedBytes -= m_Commands[Index].Before.size() + m_Commands[Index].After.size();
			}
			if (EntryBytes > m_MaximumBytes - RetainedBytes)
				return std::unexpected("The editor transaction does not fit in the bounded history");

			while (m_Commands.size() > m_Cursor)
			{
				m_Bytes -= m_Commands.back().Before.size() + m_Commands.back().After.size();
				m_Commands.pop_back();
			}

			const uint64_t BeforeState = m_CurrentState;
			const uint64_t AfterState = NewStateIdentifier();
			m_Commands.push_back({
				std::move(m_ActiveDescription),
				std::move(m_ActiveBefore),
				std::move(StateAfter),
				BeforeState,
				AfterState });
			m_Bytes += EntryBytes;
			m_CurrentState = AfterState;
			++m_Cursor;
			ClearActive();

			while (m_Commands.size() > m_MaximumCommands || m_Bytes > m_MaximumBytes)
			{
				m_Bytes -= m_Commands.front().Before.size() + m_Commands.front().After.size();
				m_Commands.erase(m_Commands.begin());
				--m_Cursor;
			}
			return true;
		}

		template<typename Restore>
		[[nodiscard]] std::expected<void, std::string> Cancel(Restore&& RestoreState)
		{
			if (!m_Active)
				return {};
			if (auto Result = RestoreState(std::string_view(m_ActiveBefore)); !Result)
				return std::unexpected(Result.error());
			ClearActive();
			return {};
		}

		template<typename Restore>
		[[nodiscard]] std::expected<bool, std::string> Undo(Restore&& RestoreState)
		{
			if (m_Active)
				return std::unexpected("Finish or cancel the active editor transaction before undoing");
			if (!CanUndo())
				return false;
			const Command& Entry = m_Commands[m_Cursor - 1];
			if (auto Result = RestoreState(std::string_view(Entry.Before)); !Result)
				return std::unexpected(Result.error());
			m_CurrentState = Entry.BeforeState;
			--m_Cursor;
			return true;
		}

		template<typename Restore>
		[[nodiscard]] std::expected<bool, std::string> Redo(Restore&& RestoreState)
		{
			if (m_Active)
				return std::unexpected("Finish or cancel the active editor transaction before redoing");
			if (!CanRedo())
				return false;
			const Command& Entry = m_Commands[m_Cursor];
			if (auto Result = RestoreState(std::string_view(Entry.After)); !Result)
				return std::unexpected(Result.error());
			m_CurrentState = Entry.AfterState;
			++m_Cursor;
			return true;
		}

		void MarkSaved() noexcept { m_SavedState = m_CurrentState; }
		[[nodiscard]] bool IsDirty() const noexcept { return m_CurrentState != m_SavedState; }
		[[nodiscard]] bool CanUndo() const noexcept { return !m_Active && m_Cursor > 0; }
		[[nodiscard]] bool CanRedo() const noexcept { return !m_Active && m_Cursor < m_Commands.size(); }
		[[nodiscard]] bool IsActive() const noexcept { return m_Active; }
		[[nodiscard]] size_t GetCommandCount() const noexcept { return m_Commands.size(); }
		[[nodiscard]] size_t GetCursor() const noexcept { return m_Cursor; }
		[[nodiscard]] size_t GetStoredBytes() const noexcept { return m_Bytes; }
		[[nodiscard]] size_t GetMaximumCommands() const noexcept { return m_MaximumCommands; }
		[[nodiscard]] size_t GetMaximumBytes() const noexcept { return m_MaximumBytes; }
		[[nodiscard]] std::optional<CommandView> GetUndoCommand() const noexcept
		{
			return CanUndo() ? std::optional<CommandView>{ CommandView{ m_Commands[m_Cursor - 1].Description } } : std::nullopt;
		}
		[[nodiscard]] std::optional<CommandView> GetRedoCommand() const noexcept
		{
			return CanRedo() ? std::optional<CommandView>{ CommandView{ m_Commands[m_Cursor].Description } } : std::nullopt;
		}

	private:
		struct Command
		{
			std::string Description;
			std::string Before;
			std::string After;
			uint64_t BeforeState = 0;
			uint64_t AfterState = 0;
		};

		[[nodiscard]] uint64_t NewStateIdentifier() noexcept
		{
			++m_NextState;
			if (m_NextState == 0)
				++m_NextState;
			return m_NextState;
		}

		void ClearActive() noexcept
		{
			m_ActiveDescription.clear();
			m_ActiveBefore.clear();
			m_Active = false;
		}

		std::vector<Command> m_Commands;
		std::string m_ActiveDescription;
		std::string m_ActiveBefore;
		size_t m_MaximumCommands = DefaultMaximumCommands;
		size_t m_MaximumBytes = DefaultMaximumBytes;
		size_t m_Bytes = 0;
		size_t m_Cursor = 0;
		uint64_t m_NextState = 0;
		uint64_t m_CurrentState = 0;
		uint64_t m_SavedState = 0;
		bool m_Active = false;
	};
}
