#pragma once

#include "Renderer/Graphics.h"
#include "Renderer/RendererAPI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PulseForge
{
	struct ToneMappingSettings
	{
		// Manual exposure in photographic EV stops; +1 doubles scene radiance.
		float ExposureEV = 0.0f;
	};

	inline constexpr float MinimumToneMappingExposureEV = -16.0f;
	inline constexpr float MaximumToneMappingExposureEV = 16.0f;

	[[nodiscard]] inline bool IsValidToneMappingSettings(const ToneMappingSettings& Settings) noexcept
	{
		return std::isfinite(Settings.ExposureEV) &&
			Settings.ExposureEV >= MinimumToneMappingExposureEV &&
			Settings.ExposureEV <= MaximumToneMappingExposureEV;
	}

	[[nodiscard]] inline bool TryUpdateToneMappingSettings(
		ToneMappingSettings& Current,
		const ToneMappingSettings& Candidate) noexcept
	{
		if (!IsValidToneMappingSettings(Candidate))
			return false;
		Current = Candidate;
		return true;
	}

	[[nodiscard]] inline bool ShouldShaderEncodeSrgb(
		ColorTargetFormat DestinationFormat,
		OutputColorEncoding SwapchainEncoding) noexcept
	{
		if (DestinationFormat == ColorTargetFormat::RGBA8_UNorm)
			return true;
		if (DestinationFormat == ColorTargetFormat::RGBA8_Srgb)
			return false;
		return SwapchainEncoding == OutputColorEncoding::UnormAttachment;
	}

	[[nodiscard]] inline bool IsToneMappingBindingCurrent(
		uint64_t BoundGeneration,
		uint64_t TargetGeneration) noexcept
	{
		return TargetGeneration != 0 && BoundGeneration == TargetGeneration;
	}

	[[nodiscard]] inline float LinearToSrgb(float Value) noexcept
	{
		const float Clamped = std::clamp(Value, 0.0f, 1.0f);
		if (Clamped <= 0.0f || Clamped >= 1.0f)
			return Clamped;
		return Clamped <= 0.0031308f
			? Clamped * 12.92f
			: 1.055f * std::pow(Clamped, 1.0f / 2.4f) - 0.055f;
	}

	[[nodiscard]] inline std::array<float, 3> ApplyAcesFittedToneMapping(
		std::array<float, 3> SceneLinearColor,
		float ExposureEV) noexcept
	{
		const float Exposure = std::isfinite(ExposureEV) ? std::exp2(ExposureEV) : 1.0f;
		for (float& Channel : SceneLinearColor)
		{
			if (std::isnan(Channel) || Channel <= 0.0f)
			{
				Channel = 0.0f;
				continue;
			}
			if (std::isinf(Channel))
			{
				Channel = 1.0f;
				continue;
			}
			const double X = static_cast<double>(Channel) * static_cast<double>(Exposure);
			const double Numerator = X * (2.51 * X + 0.03);
			const double Denominator = X * (2.43 * X + 0.59) + 0.14;
			Channel = static_cast<float>(std::clamp(Numerator / Denominator, 0.0, 1.0));
		}
		return SceneLinearColor;
	}
}
