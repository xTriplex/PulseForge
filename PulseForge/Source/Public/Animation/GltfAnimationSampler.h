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

	class GltfAnimationClipValidationToken final
	{
	public:
		[[nodiscard]] bool IsFor(const ImportedGltfAnimationClip& Clip) const noexcept
		{
			return m_Clip == &Clip && m_ChannelCount == Clip.Channels.size();
		}

	private:
		const ImportedGltfAnimationClip* m_Clip = nullptr;
		size_t m_ChannelCount = 0;
		friend PULSEFORGE_API std::expected<GltfAnimationClipValidationToken, GltfAnimationSamplingError>
			ValidateGltfAnimationClip(const ImportedGltfAnimationClip&, std::stop_token);
	};

	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationSamplingError> ValidateGltfAnimationChannel(
		const ImportedGltfAnimationChannel& Channel,
		std::stop_token CancellationToken = {});
	[[nodiscard]] PULSEFORGE_API std::expected<GltfAnimationClipValidationToken, GltfAnimationSamplingError>
		ValidateGltfAnimationClip(const ImportedGltfAnimationClip& Clip, std::stop_token CancellationToken = {});
	[[nodiscard]] PULSEFORGE_API std::expected<std::array<float, 4>, GltfAnimationSamplingError>
		SampleGltfAnimationChannel(
			const ImportedGltfAnimationClip& Clip, const GltfAnimationClipValidationToken& Validation,
			size_t ChannelIndex, float Time, std::stop_token CancellationToken = {});

	// Translation and scale are linearly interpolated. Rotation values use shortest-path normalized quaternion slerp.
	// Times outside the key range clamp to the nearest endpoint; STEP holds the preceding sample.
	[[nodiscard]] PULSEFORGE_API std::expected<std::array<float, 4>, GltfAnimationSamplingError> SampleGltfAnimationChannel(
		const ImportedGltfAnimationChannel& Channel,
		float Time,
		std::stop_token CancellationToken = {});
}
