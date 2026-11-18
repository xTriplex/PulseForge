#pragma once

#include "Animation/GltfAnimationSampler.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PulseForge
{
	inline constexpr size_t MaximumGltfAnimationPoseNodes = 4096;
	inline constexpr size_t MaximumGltfAnimationPoseChannels = 4096;

	enum class GltfAnimationPlaybackErrorCode : uint8_t
	{
		InvalidClock,
		InvalidPose,
		InvalidClip,
		Cancelled,
		ChannelSamplingFailed,
		ResourceLimitExceeded
	};

	struct GltfAnimationPlaybackError final
	{
		GltfAnimationPlaybackErrorCode Code = GltfAnimationPlaybackErrorCode::InvalidClock;
		std::string Message;
	};

	// One instance owns its own clock. Speed may be negative for reverse playback.
	struct GltfAnimationPlaybackClock final
	{
		float TimeSeconds = 0.0f;
		float Speed = 1.0f;
		bool Looping = true;
		bool Paused = false;
	};

	// Crossfade progress is instance-local and advances only when its owner calls Advance.
	struct GltfAnimationCrossfade final
	{
		float ElapsedSeconds = 0.0f;
		float DurationSeconds = 0.0f;
		bool Active = false;
	};

	struct GltfAnimationNodePose final
	{
		std::string NodeKey;
		std::array<float, 4> Translation{ 0.0f, 0.0f, 0.0f, 0.0f };
		std::array<float, 4> Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
		std::array<float, 4> Scale{ 1.0f, 1.0f, 1.0f, 0.0f };
	};

	using GltfAnimationClipValidationMap = std::unordered_map<std::string, GltfAnimationClipValidationToken>;

	[[nodiscard]] PULSEFORGE_API std::expected<GltfAnimationClipValidationMap, GltfAnimationSamplingError>
		ValidateGltfAnimationProduct(const ImportedGltfSkeletalMesh& Product, std::stop_token CancellationToken = {});

	// Value-owned state for one animated instance. Mesh and clip data remain shared immutable asset data.
	struct GltfAnimationInstancePlayback final
	{
		std::string CurrentClipKey;
		GltfAnimationPlaybackClock CurrentClock;
		std::string TargetClipKey;
		GltfAnimationPlaybackClock TargetClock;
		GltfAnimationCrossfade Crossfade;
		std::vector<GltfAnimationNodePose> TransitionSourcePose;
		std::vector<GltfAnimationNodePose> LastPose;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationPlaybackError> SetGltfAnimationPlaybackSpeed(
		GltfAnimationPlaybackClock& Clock,
		float Speed,
		std::stop_token CancellationToken = {});

	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationPlaybackError> SeekGltfAnimationPlayback(
		GltfAnimationPlaybackClock& Clock,
		float ClipDuration,
		float RequestedTime,
		std::stop_token CancellationToken = {});

	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationPlaybackError> AdvanceGltfAnimationPlayback(
		GltfAnimationPlaybackClock& Clock,
		float ClipDuration,
		float DeltaSeconds,
		std::stop_token CancellationToken = {});

	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationPlaybackError> BeginGltfAnimationCrossfade(
		GltfAnimationCrossfade& Crossfade,
		float DurationSeconds,
		std::stop_token CancellationToken = {});

	[[nodiscard]] PULSEFORGE_API std::expected<float, GltfAnimationPlaybackError> AdvanceGltfAnimationCrossfade(
		GltfAnimationCrossfade& Crossfade,
		float DeltaSeconds,
		std::stop_token CancellationToken = {});

	// Samples onto caller-provided rest pose values and returns a new pose atomically on success.
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError>
		SampleGltfAnimationPose(
			const ImportedGltfAnimationClip& Clip,
			std::span<const GltfAnimationNodePose> RestPose,
			const GltfAnimationPlaybackClock& Clock,
			std::stop_token CancellationToken = {});
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError>
		SampleGltfAnimationPose(
			const ImportedGltfAnimationClip& Clip,
			const GltfAnimationClipValidationToken& Validation,
			std::span<const GltfAnimationNodePose> RestPose,
			const GltfAnimationPlaybackClock& Clock,
			std::stop_token CancellationToken = {});

	// Blends matching node-keyed poses; rotations use shortest-path normalized quaternion interpolation.
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError>
		BlendGltfAnimationPoses(
			std::span<const GltfAnimationNodePose> FromPose,
			std::span<const GltfAnimationNodePose> ToPose,
			float Weight,
			std::stop_token CancellationToken = {});

	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationPlaybackError> PlayGltfAnimationInstance(
		GltfAnimationInstancePlayback& Instance,
		const ImportedGltfSkeletalMesh& Mesh,
		std::string_view ClipKey,
		bool Looping = true,
		std::stop_token CancellationToken = {});

	// An interrupted transition starts from LastPose, avoiding a snap back to an earlier source clip.
	[[nodiscard]] PULSEFORGE_API std::expected<void, GltfAnimationPlaybackError> TransitionGltfAnimationInstance(
		GltfAnimationInstancePlayback& Instance,
		const ImportedGltfSkeletalMesh& Mesh,
		std::span<const GltfAnimationNodePose> RestPose,
		std::string_view TargetClipKey,
		float DurationSeconds,
		bool Looping = true,
		std::stop_token CancellationToken = {});

	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError>
		AdvanceGltfAnimationInstance(
			GltfAnimationInstancePlayback& Instance,
			const ImportedGltfSkeletalMesh& Mesh,
			std::span<const GltfAnimationNodePose> RestPose,
			float DeltaSeconds,
		std::stop_token CancellationToken = {},
		const GltfAnimationClipValidationMap* ValidatedClips = nullptr);
}
