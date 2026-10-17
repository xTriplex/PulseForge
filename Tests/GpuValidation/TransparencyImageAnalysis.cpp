#include "GpuValidation/TransparencyImageAnalysis.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace PulseForge::GpuValidation::TransparencyImageAnalysis
{
	std::expected<Difference, std::string> CompareRgba8(
		const ImageView& Before, const ImageView& After, std::optional<Region> FocusRegion, bool IncludeDifferenceImage)
	{
		if (Before.Width == 0 || Before.Height == 0 || Before.Width != After.Width || Before.Height != After.Height)
			return std::unexpected("Image comparison requires matching, non-empty extents");

		const size_t PixelCount = static_cast<size_t>(Before.Width) * Before.Height;
		if (PixelCount > std::numeric_limits<size_t>::max() / 4 || Before.Pixels.size() != PixelCount * 4 ||
			After.Pixels.size() != PixelCount * 4)
			return std::unexpected("Image comparison requires complete RGBA8 pixel buffers");

		if (FocusRegion && (FocusRegion->MinX >= FocusRegion->MaxX || FocusRegion->MinY >= FocusRegion->MaxY ||
							   FocusRegion->MaxX > Before.Width || FocusRegion->MaxY > Before.Height))
			return std::unexpected("Image comparison region is empty or outside the image extent");

		Difference Result;
		Result.MinX = Before.Width;
		Result.MinY = Before.Height;
		if (IncludeDifferenceImage) Result.DifferenceRgba.resize(PixelCount * 4);
		double AbsoluteRgb = 0.0;
		double RegionAbsoluteRgb = 0.0;
		const auto ChannelDifference = [](std::byte First, std::byte Second)
		{
			const int A = std::to_integer<uint8_t>(First);
			const int B = std::to_integer<uint8_t>(Second);
			return std::abs(A - B);
		};

		for (uint32_t Y = 0; Y < Before.Height; ++Y)
		{
			for (uint32_t X = 0; X < Before.Width; ++X)
			{
				const size_t Channel = (static_cast<size_t>(Y) * Before.Width + X) * 4;
				const int Red = ChannelDifference(Before.Pixels[Channel], After.Pixels[Channel]);
				const int Green = ChannelDifference(Before.Pixels[Channel + 1], After.Pixels[Channel + 1]);
				const int Blue = ChannelDifference(Before.Pixels[Channel + 2], After.Pixels[Channel + 2]);
				const int Maximum = (std::max)({ Red, Green, Blue });
				AbsoluteRgb += Red + Green + Blue;
				if (Maximum > 8)
				{
					++Result.ChangedPixels;
					Result.MinX = (std::min)(Result.MinX, X);
					Result.MinY = (std::min)(Result.MinY, Y);
					Result.MaxX = (std::max)(Result.MaxX, X);
					Result.MaxY = (std::max)(Result.MaxY, Y);
				}

				if (FocusRegion && X >= FocusRegion->MinX && X < FocusRegion->MaxX && Y >= FocusRegion->MinY && Y < FocusRegion->MaxY)
				{
					RegionAbsoluteRgb += Red + Green + Blue;
					if (Maximum > 8) ++Result.RegionChangedPixels;
				}

				if (IncludeDifferenceImage)
				{
					Result.DifferenceRgba[Channel] = static_cast<uint8_t>((std::min)(Red * 5, 255));
					Result.DifferenceRgba[Channel + 1] = static_cast<uint8_t>((std::min)(Green * 5, 255));
					Result.DifferenceRgba[Channel + 2] = static_cast<uint8_t>((std::min)(Blue * 5, 255));
					Result.DifferenceRgba[Channel + 3] = 255;
				}
			}
		}

		Result.MeanAbsoluteRgb = AbsoluteRgb / static_cast<double>(PixelCount * 3);
		if (FocusRegion)
		{
			const size_t RegionPixels =
				static_cast<size_t>(FocusRegion->MaxX - FocusRegion->MinX) * (FocusRegion->MaxY - FocusRegion->MinY);
			Result.RegionMeanAbsoluteRgb = RegionAbsoluteRgb / static_cast<double>(RegionPixels * 3);
		}
		return Result;
	}
} // namespace PulseForge::GpuValidation::TransparencyImageAnalysis
