#pragma once

#include "Assets/CookedProductCodec.h"
#include "Core/Core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>

namespace PulseForge
{
	inline constexpr size_t MaximumGltfAnimationSamplesPerChannel = 1'000'000;

	enum class GltfAnimationSamplingErrorCode : uint8_t
	{
		InvalidChannel,
		InvalidTime,
		ResourceLimitExceeded,
		Cancelled
	};

	struct GltfAnimationSamplingError final
	{
		GltfAnimationSamplingErrorCode Code = GltfAnimationSamplingErrorCode::InvalidChannel;
		std::string Message;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationSamplingError> ValidateGltfAnimationChannel(
		const ImportedGltfAnimationChannel& Channel,
		std::stop_token CancellationToken = {});

	// Translation and scale are linearly interpolated. Rotation values use shortest-path normalized quaternion slerp.
	// Times outside the key range clamp to the nearest endpoint; STEP holds the preceding sample.
	[[nodiscard]] PULSEFORGE_API std::expected<std::array<float, 4>, GltfAnimationSamplingError> SampleGltfAnimationChannel(
		const ImportedGltfAnimationChannel& Channel,
		float Time,
		std::stop_token CancellationToken = {});
}
