#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace PulseForgeEditor
{
	[[nodiscard]] inline uint64_t ComputeEditorMaterialPreviewFingerprint(std::string_view Bytes) noexcept
	{
		uint64_t Hash = 14695981039346656037ull;
		for (const unsigned char Byte : Bytes)
		{
			Hash ^= Byte;
			Hash *= 1099511628211ull;
		}
		return Hash == 0 ? 1 : Hash;
	}

	class EditorMaterialPreviewPublication final
	{
	public:
		[[nodiscard]] size_t GetDisplayedSlot() const noexcept { return m_DisplayedSlot; }
		[[nodiscard]] size_t GetCandidateSlot() const noexcept { return 1 - m_DisplayedSlot; }
		[[nodiscard]] bool HasPublishedFrame() const noexcept { return m_PublishedFingerprint != 0; }
		[[nodiscard]] uint64_t GetPublishedFingerprint() const noexcept { return m_PublishedFingerprint; }
		[[nodiscard]] bool IsCurrent(uint64_t Fingerprint) const noexcept
		{
			return m_PublishedFingerprint != 0 && m_PublishedFingerprint == Fingerprint;
		}
		void Publish(uint64_t Fingerprint) noexcept
		{
			if (Fingerprint == 0) return;
			m_DisplayedSlot = GetCandidateSlot();
			m_PublishedFingerprint = Fingerprint;
		}
		void Invalidate() noexcept { m_PublishedFingerprint = 0; }

	private:
		size_t m_DisplayedSlot = 0;
		uint64_t m_PublishedFingerprint = 0;
	};
} // namespace PulseForgeEditor
