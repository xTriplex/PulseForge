#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace PulseForge::GpuValidation::TransparencyImageAnalysis
{
	struct ImageView final
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::span<const std::byte> Pixels;
	};

	struct Region final
	{
		uint32_t MinX = 0;
		uint32_t MinY = 0;
		uint32_t MaxX = 0;
		uint32_t MaxY = 0;
	};

	struct Difference final
	{
		size_t ChangedPixels = 0;
		double MeanAbsoluteRgb = 0.0;
		uint32_t MinX = 0;
		uint32_t MinY = 0;
		uint32_t MaxX = 0;
		uint32_t MaxY = 0;
		size_t RegionChangedPixels = 0;
		double RegionMeanAbsoluteRgb = 0.0;
		std::vector<uint8_t> DifferenceRgba;
	};

	[[nodiscard]] std::expected<Difference, std::string> CompareRgba8(const ImageView& Before, const ImageView& After,
		std::optional<Region> FocusRegion = std::nullopt, bool IncludeDifferenceImage = false);
} // namespace PulseForge::GpuValidation::TransparencyImageAnalysis
