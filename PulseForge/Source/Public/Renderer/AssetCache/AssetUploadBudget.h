#pragma once

#include <cstddef>

namespace PulseForge
{
	// Per-render-update upload admission. The renderer currently exposes atomic resource creation calls rather than
	// staged copies, so one bounded oversized product may use an otherwise empty frame budget.
	class AssetUploadBudget final
	{
	public:
		static constexpr size_t DefaultMaximumBytes = 64ull * 1024ull * 1024ull;
		static constexpr size_t DefaultMaximumOperations = 4;
		static constexpr size_t MaximumIndivisibleProductBytes = 256ull * 1024ull * 1024ull;

		void BeginFrame() noexcept
		{
			m_UsedBytes = 0;
			m_UsedOperations = 0;
		}

		[[nodiscard]] bool TryReserve(size_t Bytes) noexcept
		{
			if (Bytes == 0 || m_UsedOperations >= DefaultMaximumOperations)
				return false;

			if (m_UsedBytes <= DefaultMaximumBytes && Bytes <= DefaultMaximumBytes - m_UsedBytes)
			{
				m_UsedBytes += Bytes;
				++m_UsedOperations;
				return true;
			}

			if (m_UsedOperations == 0 && Bytes <= MaximumIndivisibleProductBytes)
			{
				m_UsedBytes = Bytes;
				m_UsedOperations = 1;
				return true;
			}
			return false;
		}

		[[nodiscard]] size_t GetUsedBytes() const noexcept { return m_UsedBytes; }
		[[nodiscard]] size_t GetUsedOperations() const noexcept { return m_UsedOperations; }

	private:
		size_t m_UsedBytes = 0;
		size_t m_UsedOperations = 0;
	};
} // namespace PulseForge
