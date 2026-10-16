#pragma once

#include <array>

#include <cstdint>

#include <vector>

namespace PulseForge

{

	struct ImportedImage

	{

		uint32_t Width = 0;

		uint32_t Height = 0;

		// Two independent 64-bit content fingerprints of the managed source bytes, for derived-data invalidation.

		std::array<uint64_t, 2> SourceFingerprint{};

		// Always four channels in top-to-bottom row order. The renderer chooses sRGB or linear interpretation.

		std::vector<uint8_t> RGBA8Pixels;
	};

	struct ImportedFloatImage

	{

		uint32_t Width = 0;

		uint32_t Height = 0;

		// Two independent 64-bit fingerprints of the managed source bytes for derived-data invalidation.

		std::array<uint64_t, 2> SourceFingerprint{};

		// Four linear-light floating-point channels in top-to-bottom row order.

		// HDR values are not clamped; alpha is supplied by the decoder.

		std::vector<float> RGBA32FPixels;
	};

} // namespace PulseForge
