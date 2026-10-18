#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	template<typename Snapshot>
	class EditorSnapshotHistory final
	{
	public:
		void Reset(const Snapshot& SavedState)
		{
			m_States.assign(1, SavedState);
			m_Cursor = 0;
			m_SavedCursor = 0;
		}

		void Record(const Snapshot& State, size_t MaximumStates = 129)
		{
			static_assert(std::is_nothrow_move_constructible_v<Snapshot> && std::is_nothrow_move_assignable_v<Snapshot>,
				"Snapshot history requires non-throwing moves after capacity is prepared");
			if (m_States.empty())
			{
				Reset(State);
				return;
			}

			Snapshot Candidate(State);
			m_States.reserve(m_Cursor + 2);
			if (m_Cursor + 1 < m_States.size())
			{
				m_States.resize(m_Cursor + 1);
			}
			m_States.push_back(std::move(Candidate));
			if (m_SavedCursor && *m_SavedCursor > m_Cursor) m_SavedCursor.reset();
			m_Cursor = m_States.size() - 1;
			const size_t StateLimit = (std::max)(MaximumStates, size_t{ 1 });
			while (m_States.size() > StateLimit)
			{
				m_States.erase(m_States.begin());
				--m_Cursor;
				if (m_SavedCursor)
				{
					if (*m_SavedCursor == 0) m_SavedCursor.reset();
					else --*m_SavedCursor;
				}
			}
		}

		[[nodiscard]] bool Undo() noexcept
		{
			if (!CanUndo())
				return false;
			--m_Cursor;
			return true;
		}

		[[nodiscard]] bool Redo() noexcept
		{
			if (!CanRedo())
				return false;
			++m_Cursor;
			return true;
		}

		void MarkSaved() noexcept { m_SavedCursor = m_Cursor; }

		[[nodiscard]] bool IsEmpty() const noexcept { return m_States.empty(); }
		[[nodiscard]] bool CanUndo() const noexcept { return !m_States.empty() && m_Cursor > 0; }
		[[nodiscard]] bool CanRedo() const noexcept { return m_Cursor + 1 < m_States.size(); }
		[[nodiscard]] bool IsDirty() const noexcept
		{
			return !m_SavedCursor || m_Cursor != *m_SavedCursor;
		}
		[[nodiscard]] size_t GetCursor() const noexcept { return m_Cursor; }
		[[nodiscard]] size_t GetStateCount() const noexcept { return m_States.size(); }
		[[nodiscard]] const Snapshot& GetCurrent() const { return m_States.at(m_Cursor); }

	private:
		std::vector<Snapshot> m_States;
		size_t m_Cursor = 0;
		std::optional<size_t> m_SavedCursor = 0;
	};
}
