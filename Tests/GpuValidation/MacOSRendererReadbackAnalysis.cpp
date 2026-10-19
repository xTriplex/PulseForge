#include "GpuValidation/MacOSRendererReadbackAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace PulseForge::GpuValidation::MacOSRenderer
{
	ImageMetrics MeasureImage(const TextureReadback& Image)
	{
		ImageMetrics Metrics;
		const size_t PixelCount = static_cast<size_t>(Image.Width) * Image.Height;
		for (size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
		{
			const size_t Offset = Pixel * 4;
			const uint8_t Red = std::to_integer<uint8_t>(Image.Pixels[Offset]);
			const uint8_t Green = std::to_integer<uint8_t>(Image.Pixels[Offset + 1]);
			const uint8_t Blue = std::to_integer<uint8_t>(Image.Pixels[Offset + 2]);
			const uint8_t Luminance = static_cast<uint8_t>((54u * Red + 183u * Green + 19u * Blue) / 256u);
			if (Luminance > 18) ++Metrics.VisiblePixels;
			if (static_cast<unsigned int>((std::max)({ Red, Green, Blue })) -
				static_cast<unsigned int>((std::min)({ Red, Green, Blue })) > 20)
				++Metrics.ChromaticPixels;
			Metrics.MinimumLuminance = (std::min)(Metrics.MinimumLuminance, Luminance);
			Metrics.MaximumLuminance = (std::max)(Metrics.MaximumLuminance, Luminance);
		}
		return Metrics;
	}

	bool HasPbrImageSignal(const TextureReadback& Image)
	{
		if (Image.Format != TextureFormat::RGBA8_Srgb || Image.Width == 0 || Image.Height == 0 ||
			Image.Pixels.size() != static_cast<size_t>(Image.Width) * Image.Height * 4)
			return false;
		const ImageMetrics Metrics = MeasureImage(Image);
		const size_t PixelCount = static_cast<size_t>(Image.Width) * Image.Height;
		return Metrics.VisiblePixels > (std::max)(size_t{ 100 }, PixelCount / 100) &&
			   Metrics.ChromaticPixels > (std::max)(size_t{ 50 }, PixelCount / 500) &&
			   Metrics.MaximumLuminance > Metrics.MinimumLuminance + 24;
	}

	size_t CountWrittenDepthPixels(const TextureReadback& Image)
	{
		if (Image.Format != TextureFormat::Depth32Float || Image.Width == 0 || Image.Height == 0 ||
			Image.Pixels.size() != static_cast<size_t>(Image.Width) * Image.Height * sizeof(float))
			return 0;

		size_t WrittenPixels = 0;
		const size_t PixelCount = static_cast<size_t>(Image.Width) * Image.Height;
		for (size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
		{
			float Depth = 1.0f;
			std::memcpy(&Depth, Image.Pixels.data() + Pixel * sizeof(float), sizeof(Depth));
			if (std::isfinite(Depth) && Depth >= 0.0f && Depth < 0.99999f) ++WrittenPixels;
		}
		return WrittenPixels;
	}

	bool HasShadowDepthSignal(const TextureReadback& Image)
	{
		return CountWrittenDepthPixels(Image) >= 16;
	}

	size_t CountChangedPixels(const TextureReadback& Before, const TextureReadback& After)
	{
		if (Before.Format != After.Format || Before.Width != After.Width || Before.Height != After.Height ||
			Before.Pixels.size() != After.Pixels.size())
			return 0;

		size_t ChangedPixels = 0;
		const size_t PixelCount = static_cast<size_t>(Before.Width) * Before.Height;
		for (size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
		{
			const size_t Offset = Pixel * 4;
			unsigned int Difference = 0;
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				const int A = std::to_integer<uint8_t>(Before.Pixels[Offset + Channel]);
				const int B = std::to_integer<uint8_t>(After.Pixels[Offset + Channel]);
				Difference += static_cast<unsigned int>(std::abs(A - B));
			}
			if (Difference > 12) ++ChangedPixels;
		}
		return ChangedPixels;
	}

	static float DecodeHalfFloat(uint16_t Bits)
	{
		const uint16_t Exponent = static_cast<uint16_t>((Bits >> 10) & 0x1fu);
		const uint16_t Mantissa = static_cast<uint16_t>(Bits & 0x03ffu);
		const float Sign = (Bits & 0x8000u) != 0 ? -1.0f : 1.0f;
		if (Exponent == 0)
			return Sign * std::ldexp(static_cast<float>(Mantissa), -24);
		if (Exponent == 0x1fu)
			return Mantissa == 0 ? Sign * std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
		return Sign * std::ldexp(1.0f + static_cast<float>(Mantissa) / 1024.0f, static_cast<int>(Exponent) - 15);
	}

	std::expected<HdrRadianceMetrics, std::string> MeasureHdrRadiance(const TextureReadback& Image)
	{
		if (Image.Format != TextureFormat::RGBA16_Float || Image.Width == 0 || Image.Height == 0 ||
			Image.Pixels.size() != static_cast<size_t>(Image.Width) * Image.Height * 4 * sizeof(uint16_t))
			return std::unexpected("HDR readback format, extent, or byte count was invalid");

		HdrRadianceMetrics Metrics;
		const size_t PixelCount = static_cast<size_t>(Image.Width) * Image.Height;
		for (size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
		{
			bool NonClear = false;
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				uint16_t Bits = 0;
				std::memcpy(&Bits, Image.Pixels.data() + (Pixel * 4 + Channel) * sizeof(Bits), sizeof(Bits));
				if (((Bits >> 10) & 0x1fu) == 0x1fu)
					return std::unexpected("HDR readback contained a NaN or infinite RGB radiance value");
				const float Radiance = DecodeHalfFloat(Bits);
				if (!std::isfinite(Radiance))
					return std::unexpected("HDR readback contained a non-finite RGB radiance value");
				Metrics.MaximumRadiance = (std::max)(Metrics.MaximumRadiance, Radiance);
				NonClear |= std::abs(Radiance) > 0.001f;
			}
			if (NonClear) ++Metrics.NonClearPixels;
		}
		return Metrics;
	}
}
