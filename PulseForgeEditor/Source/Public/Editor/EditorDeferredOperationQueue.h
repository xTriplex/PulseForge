#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <utility>

namespace PulseForgeEditor
{
	class EditorDeferredOperationQueue
	{
	public:
		void Enqueue(std::function<void()> Operation)
		{
			if (Operation)
				m_Operations.push_back(std::move(Operation));
		}

		[[nodiscard]] bool ExecuteNext()
		{
			if (m_Operations.empty())
				return false;

			std::function<void()> Operation = std::move(m_Operations.front());
			m_Operations.pop_front();
			Operation();
			return true;
		}

		void Clear() noexcept
		{
			m_Operations.clear();
		}

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return m_Operations.size();
		}

	private:
		std::deque<std::function<void()>> m_Operations;
	};
}
