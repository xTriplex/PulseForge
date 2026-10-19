#pragma once

#include "Core/Application.h"

#include <expected>
#include <string>

namespace PulseForge::GpuValidation::MacOSRenderer
{
	struct ImageMetrics
	{
		size_t VisiblePixels = 0;
		size_t ChromaticPixels = 0;
		uint8_t MinimumLuminance = 255;
		uint8_t MaximumLuminance = 0;
	};

	struct HdrRadianceMetrics
	{
		size_t NonClearPixels = 0;
		float MaximumRadiance = 0.0f;
	};

	[[nodiscard]] ImageMetrics MeasureImage(const TextureReadback& Image);
	[[nodiscard]] bool HasPbrImageSignal(const TextureReadback& Image);
	[[nodiscard]] size_t CountWrittenDepthPixels(const TextureReadback& Image);
	[[nodiscard]] bool HasShadowDepthSignal(const TextureReadback& Image);
	[[nodiscard]] size_t CountChangedPixels(const TextureReadback& Before, const TextureReadback& After);
	[[nodiscard]] std::expected<HdrRadianceMetrics, std::string> MeasureHdrRadiance(const TextureReadback& Image);
}
