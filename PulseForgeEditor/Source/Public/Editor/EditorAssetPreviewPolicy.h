#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PulseForgeEditor
{
	enum class AssetPreviewKind : uint8_t
	{
		Image,
		MaterialSwatch
	};

	struct AssetPreviewKey
	{
		std::string ProjectID;
		std::string AssetID;
		std::array<uint64_t, 2> ContentRevision{};
		AssetPreviewKind Kind = AssetPreviewKind::Image;
		uint16_t Width = 0;
		uint16_t Height = 0;
		uint16_t AlgorithmVersion = 1;

		bool operator==(const AssetPreviewKey&) const = default;
	};

	struct AssetPreviewKeyHash
	{
		[[nodiscard]] size_t operator()(const AssetPreviewKey& Key) const noexcept
		{
			const auto Mix = [](size_t Seed, size_t Value)
			{
				return Seed ^ (Value + static_cast<size_t>(0x9e3779b9u) + (Seed << 6) + (Seed >> 2));
			};
			size_t Hash = std::hash<std::string>{}(Key.ProjectID);
			Hash = Mix(Hash, std::hash<std::string>{}(Key.AssetID));
			Hash = Mix(Hash, std::hash<uint64_t>{}(Key.ContentRevision[0]));
			Hash = Mix(Hash, std::hash<uint64_t>{}(Key.ContentRevision[1]));
			Hash = Mix(Hash, static_cast<size_t>(Key.Kind));
			Hash = Mix(Hash, Key.Width);
			Hash = Mix(Hash, Key.Height);
			return Mix(Hash, Key.AlgorithmVersion);
		}
	};

	[[nodiscard]] inline std::array<uint8_t, 4> MakeMaterialBaseColorSwatch(
		float LinearRed,
		float LinearGreen,
		float LinearBlue,
		float Alpha,
		bool Opaque) noexcept
	{
		const auto ToSrgbByte = [](float Linear)
		{
			const float Value = std::clamp(Linear, 0.0f, 1.0f);
			const float Encoded = Value <= 0.0031308f
				? Value * 12.92f : 1.055f * std::pow(Value, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(Encoded * 255.0f));
		};
		return { ToSrgbByte(LinearRed), ToSrgbByte(LinearGreen), ToSrgbByte(LinearBlue),
			static_cast<uint8_t>(std::lround((Opaque ? 1.0f : std::clamp(Alpha, 0.0f, 1.0f)) * 255.0f)) };
	}

	struct AssetPreviewDimensions
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	[[nodiscard]] inline AssetPreviewDimensions CalculateAssetPreviewDimensions(
		uint32_t SourceWidth,
		uint32_t SourceHeight,
		uint32_t MaximumEdge) noexcept
	{
		if (SourceWidth == 0 || SourceHeight == 0 || MaximumEdge == 0)
			return {};
		const double Scale = (std::min)(1.0, static_cast<double>(MaximumEdge) /
			static_cast<double>((std::max)(SourceWidth, SourceHeight)));
		return {
			(std::max)(1u, static_cast<uint32_t>(std::lround(static_cast<double>(SourceWidth) * Scale))),
			(std::max)(1u, static_cast<uint32_t>(std::lround(static_cast<double>(SourceHeight) * Scale))) };
	}

	struct AssetPreviewRect
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;
	};

	struct AssetPreviewEvictionCandidate
	{
		std::string Key;
		bool Visible = false;
		bool InFlight = false;
		uint64_t LastUse = 0;
		size_t GpuBytes = 0;
	};

	inline void BeginAssetPreviewFrame(bool& Visible) noexcept
	{
		Visible = false;
	}

	inline void TouchAssetPreviewEntry(bool& Visible, uint64_t& LastUse, uint64_t& UseCounter) noexcept
	{
		Visible = true;
		LastUse = ++UseCounter;
	}

	[[nodiscard]] inline std::optional<std::vector<std::string>> PlanAssetPreviewEvictions(
		std::span<const AssetPreviewEvictionCandidate> Candidates,
		size_t CurrentEntries,
		size_t MaximumEntries,
		size_t UsedGpuBytes,
		size_t MaximumGpuBytes,
		size_t RequiredGpuBytes,
		size_t RequiredEntrySlots,
		std::string_view ProtectedKey = {})
	{
		if (RequiredGpuBytes > MaximumGpuBytes || RequiredEntrySlots > MaximumEntries)
			return std::nullopt;

		size_t EntriesAfterRequired = CurrentEntries;
		if (RequiredEntrySlots > (std::numeric_limits<size_t>::max)() - EntriesAfterRequired)
			return std::nullopt;
		EntriesAfterRequired += RequiredEntrySlots;
		size_t EntriesToRemove = EntriesAfterRequired > MaximumEntries
			? EntriesAfterRequired - MaximumEntries
			: 0;
		size_t BytesToReclaim = UsedGpuBytes > MaximumGpuBytes - RequiredGpuBytes
			? UsedGpuBytes - (MaximumGpuBytes - RequiredGpuBytes)
			: 0;
		if (EntriesToRemove == 0 && BytesToReclaim == 0)
			return std::vector<std::string>{};

		std::vector<const AssetPreviewEvictionCandidate*> Ordered;
		Ordered.reserve(Candidates.size());
		for (const AssetPreviewEvictionCandidate& Candidate : Candidates)
			if (!Candidate.Visible && !Candidate.InFlight && Candidate.Key != ProtectedKey)
				Ordered.push_back(&Candidate);
		std::ranges::sort(Ordered, [](const auto* Left, const auto* Right)
		{
			if (Left->LastUse != Right->LastUse)
				return Left->LastUse < Right->LastUse;
			return Left->Key < Right->Key;
		});

		std::vector<std::string> Evictions;
		size_t ReclaimedBytes = 0;
		for (const AssetPreviewEvictionCandidate* Candidate : Ordered)
		{
			Evictions.push_back(Candidate->Key);
			if (EntriesToRemove > 0)
				--EntriesToRemove;
			const size_t RemainingBytes = BytesToReclaim - ReclaimedBytes;
			ReclaimedBytes += (std::min)(Candidate->GpuBytes, RemainingBytes);
			if (EntriesToRemove == 0 && ReclaimedBytes == BytesToReclaim)
				return Evictions;
		}
		return std::nullopt;
	}

	[[nodiscard]] inline bool IsCurrentAssetPreviewResult(
		uint64_t WorkGeneration,
		uint64_t EntryGeneration,
		uint64_t CurrentGeneration,
		bool Stopping) noexcept
	{
		return !Stopping && WorkGeneration == CurrentGeneration && EntryGeneration == WorkGeneration;
	}

	[[nodiscard]] inline AssetPreviewRect FitAssetPreview(
		float RegionX,
		float RegionY,
		float RegionWidth,
		float RegionHeight,
		uint32_t ImageWidth,
		uint32_t ImageHeight) noexcept
	{
		if (!std::isfinite(RegionX) || !std::isfinite(RegionY) || !std::isfinite(RegionWidth) ||
			!std::isfinite(RegionHeight) || RegionWidth <= 0.0f || RegionHeight <= 0.0f ||
			ImageWidth == 0 || ImageHeight == 0)
			return {};
		const float Scale = (std::min)(RegionWidth / static_cast<float>(ImageWidth),
			RegionHeight / static_cast<float>(ImageHeight));
		const float Width = static_cast<float>(ImageWidth) * Scale;
		const float Height = static_cast<float>(ImageHeight) * Scale;
		return { RegionX + (RegionWidth - Width) * 0.5f, RegionY + (RegionHeight - Height) * 0.5f, Width, Height };
	}

	[[nodiscard]] inline bool IsSupportedImagePreviewExtension(const std::filesystem::path& Path)
	{
		std::string Extension = Path.extension().string();
		std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character)
		{
			return static_cast<char>(Character >= 'A' && Character <= 'Z' ? Character + ('a' - 'A') : Character);
		});
		constexpr std::array<std::string_view, 12> Extensions = {
			".png", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".gif", ".pic", ".ppm", ".pnm", ".pgm", ".pbm" };
		return std::ranges::find(Extensions, Extension) != Extensions.end();
	}

	class AssetPreviewBudget final
	{
	public:
		explicit AssetPreviewBudget(size_t ByteLimit = 0) noexcept : m_ByteLimit(ByteLimit) {}

		void SetLimit(size_t ByteLimit) noexcept { m_ByteLimit = ByteLimit; }
		[[nodiscard]] size_t GetLimit() const noexcept { return m_ByteLimit; }
		[[nodiscard]] size_t GetUsed() const noexcept { return m_Used; }
		[[nodiscard]] bool CanFit(size_t Bytes) const noexcept
		{
			return Bytes <= m_ByteLimit && m_Used <= m_ByteLimit - Bytes;
		}
		[[nodiscard]] bool Add(size_t Bytes) noexcept
		{
			if (!CanFit(Bytes))
				return false;
			m_Used += Bytes;
			return true;
		}
		void Remove(size_t Bytes) noexcept { m_Used -= (std::min)(m_Used, Bytes); }
		void Clear() noexcept { m_Used = 0; }

	private:
		size_t m_ByteLimit = 0;
		size_t m_Used = 0;
	};
}
