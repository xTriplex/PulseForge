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
		[[nodiscard]] bool HasPublishedFrame() const noexcept { return m_HasPublishedFrame; }
		[[nodiscard]] uint64_t GetPublishedFingerprint() const noexcept { return m_PublishedFingerprint; }
		[[nodiscard]] uint64_t GetPublishedAssetRevision() const noexcept { return m_PublishedAssetRevision; }
		[[nodiscard]] bool IsCurrent(uint64_t Fingerprint, uint64_t AssetRevision) const noexcept
		{
			return m_HasPublishedFrame && m_IsCurrent && m_PublishedFingerprint == Fingerprint &&
				m_PublishedAssetRevision == AssetRevision && m_CurrentAssetRevision == AssetRevision;
		}
		[[nodiscard]] bool PublishCandidate(uint64_t PreparedFingerprint,
			uint64_t PreparedAssetRevision, uint64_t CurrentAssetRevision,
			uint64_t PreparedDraftContentRevision, uint64_t CurrentDraftContentRevision,
			bool PreparationComplete, bool RenderSucceeded, size_t GeometryDrawCount) noexcept
		{
			if (PreparedFingerprint == 0 || PreparedAssetRevision != CurrentAssetRevision ||
				PreparedDraftContentRevision != CurrentDraftContentRevision || !PreparationComplete ||
				!RenderSucceeded || GeometryDrawCount != 1) return false;
			m_DisplayedSlot = GetCandidateSlot();
			m_PublishedFingerprint = PreparedFingerprint;
			m_PublishedAssetRevision = PreparedAssetRevision;
			m_CurrentAssetRevision = CurrentAssetRevision;
			m_HasPublishedFrame = true;
			m_IsCurrent = true;
			return true;
		}
		void Invalidate(uint64_t AssetRevision) noexcept
		{
			m_CurrentAssetRevision = AssetRevision;
			m_IsCurrent = false;
		}

	private:
		size_t m_DisplayedSlot = 0;
		uint64_t m_PublishedFingerprint = 0;
		uint64_t m_PublishedAssetRevision = 0;
		uint64_t m_CurrentAssetRevision = 0;
		bool m_HasPublishedFrame = false;
		bool m_IsCurrent = false;
	};
} // namespace PulseForgeEditor
