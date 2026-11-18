#include "Animation/GltfAnimationSampler.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace PulseForge
{
	namespace
	{
		constexpr float QuaternionLinearBlendThreshold = 0.9995f;

		[[nodiscard]] GltfAnimationSamplingError MakeError(GltfAnimationSamplingErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		[[nodiscard]] bool IsSupportedPath(GltfAnimationPath Path)
		{
			switch (Path)
			{
			case GltfAnimationPath::Translation:
			case GltfAnimationPath::Rotation:
			case GltfAnimationPath::Scale:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] bool IsSupportedInterpolation(GltfAnimationInterpolation Interpolation)
		{
			switch (Interpolation)
			{
			case GltfAnimationInterpolation::Step:
			case GltfAnimationInterpolation::Linear:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] float Dot(const std::array<float, 4>& A, const std::array<float, 4>& B)
		{
			return A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3];
		}

		[[nodiscard]] double LengthSquared(const std::array<float, 4>& Value)
		{
			double Result = 0.0;
			for (const float Component : Value)
				Result += static_cast<double>(Component) * Component;
			return Result;
		}

		[[nodiscard]] std::array<float, 4> Normalize(const std::array<float, 4>& Value)
		{
			const double LengthSquaredValue = LengthSquared(Value);
			if (!std::isfinite(LengthSquaredValue) || LengthSquaredValue <= 0.0)
				return { 0.0f, 0.0f, 0.0f, 1.0f };
			const float InverseLength = static_cast<float>(1.0 / std::sqrt(LengthSquaredValue));
			return { Value[0] * InverseLength, Value[1] * InverseLength,
				Value[2] * InverseLength, Value[3] * InverseLength };
		}

		[[nodiscard]] std::array<float, 4> SampleRotation(
			const std::array<float, 4>& A,
			const std::array<float, 4>& B,
			float Alpha)
		{
			const std::array<float, 4> NormalizedA = Normalize(A);
			std::array<float, 4> NormalizedB = Normalize(B);
			float Cosine = Dot(NormalizedA, NormalizedB);
			if (Cosine < 0.0f)
			{
				Cosine = -Cosine;
				for (float& Component : NormalizedB)
					Component = -Component;
			}
			Cosine = std::clamp(Cosine, -1.0f, 1.0f);

			if (Cosine > QuaternionLinearBlendThreshold)
			{
				std::array<float, 4> Result{};
				for (size_t Component = 0; Component < Result.size(); ++Component)
					Result[Component] = NormalizedA[Component] + Alpha * (NormalizedB[Component] - NormalizedA[Component]);
				return Normalize(Result);
			}

			const float Angle = std::acos(Cosine);
			const float Sine = std::sin(Angle);
			const float WeightA = std::sin((1.0f - Alpha) * Angle) / Sine;
			const float WeightB = std::sin(Alpha * Angle) / Sine;
			std::array<float, 4> Result{};
			for (size_t Component = 0; Component < Result.size(); ++Component)
				Result[Component] = WeightA * NormalizedA[Component] + WeightB * NormalizedB[Component];
			return Normalize(Result);
		}

		[[nodiscard]] std::expected<std::array<float, 4>, GltfAnimationSamplingError> SampleValidatedChannel(
			const ImportedGltfAnimationChannel& Channel, float Time, std::stop_token CancellationToken)
		{
			if (!std::isfinite(Time))
				return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidTime,
					"Requested animation sample time must be finite."));
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::Cancelled,
					"Animation sampling was cancelled."));
			if (Time <= Channel.Times.front())
				return Channel.Path == GltfAnimationPath::Rotation ? Normalize(Channel.Values.front()) : Channel.Values.front();
			if (Time >= Channel.Times.back())
				return Channel.Path == GltfAnimationPath::Rotation ? Normalize(Channel.Values.back()) : Channel.Values.back();

			const auto Upper = std::upper_bound(Channel.Times.begin(), Channel.Times.end(), Time);
			const size_t NextIndex = static_cast<size_t>(Upper - Channel.Times.begin());
			const size_t PreviousIndex = NextIndex - 1;
			if (Channel.Interpolation == GltfAnimationInterpolation::Step)
				return Channel.Path == GltfAnimationPath::Rotation ? Normalize(Channel.Values[PreviousIndex]) : Channel.Values[PreviousIndex];

			const double AlphaDouble = (static_cast<double>(Time) - Channel.Times[PreviousIndex]) /
				(static_cast<double>(Channel.Times[NextIndex]) - Channel.Times[PreviousIndex]);
			const float Alpha = static_cast<float>(AlphaDouble);
			if (Channel.Path == GltfAnimationPath::Rotation)
				return SampleRotation(Channel.Values[PreviousIndex], Channel.Values[NextIndex], Alpha);

			std::array<float, 4> Result{};
			for (size_t Component = 0; Component < Result.size(); ++Component)
				Result[Component] = static_cast<float>(
					(1.0 - AlphaDouble) * Channel.Values[PreviousIndex][Component] +
					AlphaDouble * Channel.Values[NextIndex][Component]);
			return Result;
		}
	}

	std::expected<void, GltfAnimationSamplingError> ValidateGltfAnimationChannel(
		const ImportedGltfAnimationChannel& Channel,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::Cancelled,
				"Animation channel validation was cancelled."));
		if (Channel.TargetNodeKey.empty())
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
				"Animation channel has no target node key."));
		if (!IsSupportedPath(Channel.Path))
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
				"Animation channel uses an unsupported target path."));
		if (!IsSupportedInterpolation(Channel.Interpolation))
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
				"Animation channel uses an unsupported interpolation mode."));
		if (Channel.Times.empty() || Channel.Values.empty())
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
				"Animation channel must contain at least one key sample."));
		if (Channel.Times.size() != Channel.Values.size())
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
				"Animation channel time and value counts must match."));
		if (Channel.Times.size() > MaximumGltfAnimationSamplesPerChannel)
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::ResourceLimitExceeded,
				"Animation channel exceeds the supported sample limit."));

		for (size_t SampleIndex = 0; SampleIndex < Channel.Times.size(); ++SampleIndex)
		{
			if ((SampleIndex & 0xffu) == 0 && CancellationToken.stop_requested())
				return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::Cancelled,
					"Animation channel validation was cancelled."));
			const float SampleTime = Channel.Times[SampleIndex];
			if (!std::isfinite(SampleTime))
				return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
					"Animation channel contains a non-finite key time."));
			if (SampleIndex > 0 && SampleTime <= Channel.Times[SampleIndex - 1])
				return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
					"Animation key times must be strictly increasing."));

			const auto& Value = Channel.Values[SampleIndex];
			for (const float Component : Value)
				if (!std::isfinite(Component))
					return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
						"Animation channel contains a non-finite sample value."));
			if (Channel.Path == GltfAnimationPath::Rotation)
			{
				const double QuaternionLengthSquared = LengthSquared(Value);
				if (!std::isfinite(QuaternionLengthSquared) || QuaternionLengthSquared <= 0.0)
					return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
						"Rotation sample quaternion must have a finite, nonzero length."));
			}
		}
		return {};
	}

	std::expected<GltfAnimationClipValidationToken, GltfAnimationSamplingError> ValidateGltfAnimationClip(
		const ImportedGltfAnimationClip& Clip, std::stop_token CancellationToken)
	{
		if (Clip.StableKey.empty() || Clip.Channels.empty() || Clip.Channels.size() > 4096 ||
			!std::isfinite(Clip.Duration) || Clip.Duration < 0.0f)
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
				"Animation clip identity, duration, or channel count is invalid."));
		for (size_t Index = 0; Index < Clip.Channels.size(); ++Index)
		{
			if ((Index & 0x3fu) == 0 && CancellationToken.stop_requested())
				return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::Cancelled,
					"Animation clip validation was cancelled."));
			if (auto Valid = ValidateGltfAnimationChannel(Clip.Channels[Index], CancellationToken); !Valid)
				return std::unexpected(Valid.error());
		}
		GltfAnimationClipValidationToken Token;
		Token.m_Clip = &Clip;
		Token.m_ChannelCount = Clip.Channels.size();
		return Token;
	}

	std::expected<std::array<float, 4>, GltfAnimationSamplingError> SampleGltfAnimationChannel(
		const ImportedGltfAnimationChannel& Channel,
		float Time,
		std::stop_token CancellationToken)
	{
		if (auto Validation = ValidateGltfAnimationChannel(Channel, CancellationToken); !Validation)
			return std::unexpected(Validation.error());
		return SampleValidatedChannel(Channel, Time, CancellationToken);
	}

	std::expected<std::array<float, 4>, GltfAnimationSamplingError> SampleGltfAnimationChannel(
		const ImportedGltfAnimationClip& Clip, const GltfAnimationClipValidationToken& Validation,
		size_t ChannelIndex, float Time, std::stop_token CancellationToken)
	{
		if (!Validation.IsFor(Clip) || ChannelIndex >= Clip.Channels.size())
			return std::unexpected(MakeError(GltfAnimationSamplingErrorCode::InvalidChannel,
				"Animation validation token does not match the immutable clip."));
		return SampleValidatedChannel(Clip.Channels[ChannelIndex], Time, CancellationToken);
	}
}
#include "Animation/GltfAnimationPlayback.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace PulseForge
{
	namespace
	{
		[[nodiscard]] GltfAnimationPlaybackError MakePlaybackError(
			GltfAnimationPlaybackErrorCode Code,
			std::string Message)
		{
			return { Code, std::move(Message) };
		}

		[[nodiscard]] std::expected<void, GltfAnimationPlaybackError> ValidateDurationAndClock(
			const GltfAnimationPlaybackClock& Clock,
			float ClipDuration,
			std::stop_token CancellationToken)
		{
			if (CancellationToken.stop_requested())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
					"Animation playback operation was cancelled."));
			if (!std::isfinite(ClipDuration) || ClipDuration < 0.0f || !std::isfinite(Clock.TimeSeconds) ||
				!std::isfinite(Clock.Speed))
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
					"Animation duration, playback time, and speed must be finite; duration cannot be negative."));
			return {};
		}

		[[nodiscard]] bool IsFinitePoseValue(const std::array<float, 4>& Value)
		{
			return std::all_of(Value.begin(), Value.end(), [](float Component) { return std::isfinite(Component); });
		}

		[[nodiscard]] const ImportedGltfAnimationClip* FindAnimationClip(
			const ImportedGltfSkeletalMesh& Mesh,
			std::string_view ClipKey)
		{
			const auto Clip = std::find_if(Mesh.Animations.begin(), Mesh.Animations.end(),
				[&](const ImportedGltfAnimationClip& Candidate) { return Candidate.StableKey == ClipKey; });
			return Clip == Mesh.Animations.end() ? nullptr : &*Clip;
		}
	}

	std::expected<void, GltfAnimationPlaybackError> SetGltfAnimationPlaybackSpeed(
		GltfAnimationPlaybackClock& Clock,
		float Speed,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation speed change was cancelled."));
		if (!std::isfinite(Speed))
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Animation playback speed must be finite."));
		Clock.Speed = Speed;
		return {};
	}

	std::expected<void, GltfAnimationPlaybackError> SeekGltfAnimationPlayback(
		GltfAnimationPlaybackClock& Clock,
		float ClipDuration,
		float RequestedTime,
		std::stop_token CancellationToken)
	{
		if (auto Validation = ValidateDurationAndClock(Clock, ClipDuration, CancellationToken); !Validation)
			return std::unexpected(Validation.error());
		if (!std::isfinite(RequestedTime))
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Requested animation seek time must be finite."));
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation seek was cancelled."));
		Clock.TimeSeconds = std::clamp(RequestedTime, 0.0f, ClipDuration);
		return {};
	}

	std::expected<void, GltfAnimationPlaybackError> AdvanceGltfAnimationPlayback(
		GltfAnimationPlaybackClock& Clock,
		float ClipDuration,
		float DeltaSeconds,
		std::stop_token CancellationToken)
	{
		if (auto Validation = ValidateDurationAndClock(Clock, ClipDuration, CancellationToken); !Validation)
			return std::unexpected(Validation.error());
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Playback delta must be finite and nonnegative."));
		if (Clock.Paused || ClipDuration == 0.0f)
		{
			if (ClipDuration == 0.0f)
				Clock.TimeSeconds = 0.0f;
			return {};
		}

		const double Duration = ClipDuration;
		const double ProposedTime = static_cast<double>(Clock.TimeSeconds) +
			static_cast<double>(DeltaSeconds) * Clock.Speed;
		if (!std::isfinite(ProposedTime))
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Advanced animation time is outside the supported numeric range."));

		double NextTime = ProposedTime;
		if (Clock.Looping)
		{
			NextTime = std::fmod(ProposedTime, Duration);
			if (NextTime < 0.0)
				NextTime += Duration;
		}
		else
			NextTime = std::clamp(ProposedTime, 0.0, Duration);

		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation clock advance was cancelled."));
		Clock.TimeSeconds = static_cast<float>(NextTime);
		return {};
	}

	std::expected<void, GltfAnimationPlaybackError> BeginGltfAnimationCrossfade(
		GltfAnimationCrossfade& Crossfade,
		float DurationSeconds,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation crossfade start was cancelled."));
		if (!std::isfinite(DurationSeconds) || DurationSeconds < 0.0f)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Animation crossfade duration must be finite and nonnegative."));

		Crossfade.ElapsedSeconds = 0.0f;
		Crossfade.DurationSeconds = DurationSeconds;
		Crossfade.Active = DurationSeconds > 0.0f;
		return {};
	}

	std::expected<float, GltfAnimationPlaybackError> AdvanceGltfAnimationCrossfade(
		GltfAnimationCrossfade& Crossfade,
		float DeltaSeconds,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation crossfade advance was cancelled."));
		if (!std::isfinite(Crossfade.DurationSeconds) || Crossfade.DurationSeconds < 0.0f ||
			!std::isfinite(Crossfade.ElapsedSeconds) || Crossfade.ElapsedSeconds < 0.0f ||
			Crossfade.ElapsedSeconds > Crossfade.DurationSeconds || !std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Animation crossfade state and delta must be finite and within their duration."));

		const float Weight = Crossfade.DurationSeconds == 0.0f ? 1.0f :
			std::clamp(static_cast<float>((static_cast<double>(Crossfade.ElapsedSeconds) + DeltaSeconds) /
				Crossfade.DurationSeconds), 0.0f, 1.0f);
		const float Elapsed = Crossfade.DurationSeconds == 0.0f ? 0.0f :
			std::min(Crossfade.DurationSeconds,
				static_cast<float>(static_cast<double>(Crossfade.ElapsedSeconds) + DeltaSeconds));
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation crossfade advance was cancelled."));

		Crossfade.ElapsedSeconds = Elapsed;
		Crossfade.Active = Crossfade.DurationSeconds > 0.0f && Elapsed < Crossfade.DurationSeconds;
		return Weight;
	}

	std::expected<GltfAnimationClipValidationMap, GltfAnimationSamplingError> ValidateGltfAnimationProduct(
		const ImportedGltfSkeletalMesh& Product, std::stop_token CancellationToken)
	{
		if (Product.Animations.size() > MaximumGltfAnimationPoseChannels)
			return std::unexpected(GltfAnimationSamplingError{
				GltfAnimationSamplingErrorCode::ResourceLimitExceeded, "Skeletal product exceeds its animation clip limit." });
		GltfAnimationClipValidationMap Validated;
		Validated.reserve(Product.Animations.size());
		for (size_t Index = 0; Index < Product.Animations.size(); ++Index)
		{
			if ((Index & 0x3fu) == 0 && CancellationToken.stop_requested())
				return std::unexpected(GltfAnimationSamplingError{
					GltfAnimationSamplingErrorCode::Cancelled, "Skeletal animation validation was cancelled." });
			const ImportedGltfAnimationClip& Clip = Product.Animations[Index];
			auto Token = ValidateGltfAnimationClip(Clip, CancellationToken);
			if (!Token) return std::unexpected(Token.error());
			if (!Validated.emplace(Clip.StableKey, std::move(*Token)).second)
				return std::unexpected(GltfAnimationSamplingError{
					GltfAnimationSamplingErrorCode::InvalidChannel, "Skeletal product contains duplicate animation clip keys." });
		}
		return Validated;
	}

	std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError> SampleGltfAnimationPose(
		const ImportedGltfAnimationClip& Clip,
		std::span<const GltfAnimationNodePose> RestPose,
		const GltfAnimationPlaybackClock& Clock,
		std::stop_token CancellationToken)
	{
		auto Validation = ValidateGltfAnimationClip(Clip, CancellationToken);
		if (!Validation)
			return std::unexpected(MakePlaybackError(
				Validation.error().Code == GltfAnimationSamplingErrorCode::Cancelled
					? GltfAnimationPlaybackErrorCode::Cancelled
					: GltfAnimationPlaybackErrorCode::InvalidClip,
				Validation.error().Message));
		return SampleGltfAnimationPose(Clip, *Validation, RestPose, Clock, CancellationToken);
	}

	std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError> SampleGltfAnimationPose(
		const ImportedGltfAnimationClip& Clip,
		const GltfAnimationClipValidationToken& Validation,
		std::span<const GltfAnimationNodePose> RestPose,
		const GltfAnimationPlaybackClock& Clock,
		std::stop_token CancellationToken)
	{
		if (!Validation.IsFor(Clip))
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
				"Animation clip validation token does not match this immutable clip."));
		if (auto Validation = ValidateDurationAndClock(Clock, Clip.Duration, CancellationToken); !Validation)
			return std::unexpected(Validation.error());
		if (Clip.StableKey.empty() || Clip.Channels.empty() || Clip.Channels.size() > MaximumGltfAnimationPoseChannels)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
				"Animation clip identity or channel count is invalid."));
		if (RestPose.empty() || RestPose.size() > MaximumGltfAnimationPoseNodes)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidPose,
				"Rest pose node count is empty or exceeds the supported limit."));

		std::unordered_map<std::string, size_t> NodeIndices;
		NodeIndices.reserve(RestPose.size());
		for (size_t Index = 0; Index < RestPose.size(); ++Index)
		{
			if ((Index & 0x3fu) == 0 && CancellationToken.stop_requested())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
					"Animation rest-pose validation was cancelled."));
			const GltfAnimationNodePose& Pose = RestPose[Index];
			if (Pose.NodeKey.empty() || !IsFinitePoseValue(Pose.Translation) || !IsFinitePoseValue(Pose.Rotation) ||
				!IsFinitePoseValue(Pose.Scale) || !NodeIndices.emplace(Pose.NodeKey, Index).second)
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidPose,
					"Rest pose contains an empty/duplicate node key or a non-finite transform value."));
		}

		std::vector<GltfAnimationNodePose> Result(RestPose.begin(), RestPose.end());
		for (size_t ChannelIndex = 0; ChannelIndex < Clip.Channels.size(); ++ChannelIndex)
		{
			if ((ChannelIndex & 0x3fu) == 0 && CancellationToken.stop_requested())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
					"Animation pose sampling was cancelled."));
			const ImportedGltfAnimationChannel& Channel = Clip.Channels[ChannelIndex];
			const auto Node = NodeIndices.find(Channel.TargetNodeKey);
			if (Node == NodeIndices.end())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
					"Animation channel targets a node that is missing from the rest pose: " + Channel.TargetNodeKey));

			const auto Sample = SampleGltfAnimationChannel(Clip, Validation, ChannelIndex,
				Clock.TimeSeconds, CancellationToken);
			if (!Sample)
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::ChannelSamplingFailed,
					"Channel for node " + Channel.TargetNodeKey + " failed: " + Sample.error().Message));
			GltfAnimationNodePose& Pose = Result[Node->second];
			switch (Channel.Path)
			{
			case GltfAnimationPath::Translation:
				Pose.Translation = *Sample;
				break;
			case GltfAnimationPath::Rotation:
				Pose.Rotation = *Sample;
				break;
			case GltfAnimationPath::Scale:
				Pose.Scale = *Sample;
				break;
			default:
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
					"Animation channel has an unsupported path."));
			}
		}
		return Result;
	}

	std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError> BlendGltfAnimationPoses(
		std::span<const GltfAnimationNodePose> FromPose,
		std::span<const GltfAnimationNodePose> ToPose,
		float Weight,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation pose blend was cancelled."));
		if (!std::isfinite(Weight) || Weight < 0.0f || Weight > 1.0f)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Animation pose blend weight must be finite and between zero and one."));
		if (FromPose.empty() || FromPose.size() > MaximumGltfAnimationPoseNodes || FromPose.size() != ToPose.size())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidPose,
				"Animation poses must have the same nonempty node count within the supported limit."));

		try
		{
			std::vector<GltfAnimationNodePose> Result;
			Result.reserve(FromPose.size());
			for (size_t Index = 0; Index < FromPose.size(); ++Index)
			{
				if ((Index & 0x3fu) == 0 && CancellationToken.stop_requested())
					return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
						"Animation pose blend was cancelled."));
				const GltfAnimationNodePose& From = FromPose[Index];
				const GltfAnimationNodePose& To = ToPose[Index];
				if (From.NodeKey.empty() || From.NodeKey != To.NodeKey || !IsFinitePoseValue(From.Translation) ||
					!IsFinitePoseValue(From.Rotation) || !IsFinitePoseValue(From.Scale) ||
					!IsFinitePoseValue(To.Translation) || !IsFinitePoseValue(To.Rotation) ||
					!IsFinitePoseValue(To.Scale) || LengthSquared(From.Rotation) <= 0.0 || LengthSquared(To.Rotation) <= 0.0)
					return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidPose,
						"Animation poses contain mismatched node keys or invalid transform values."));

				GltfAnimationNodePose Blended = From;
				for (size_t Component = 0; Component < 4; ++Component)
				{
					Blended.Translation[Component] = static_cast<float>(
						(1.0 - Weight) * From.Translation[Component] + Weight * To.Translation[Component]);
					Blended.Scale[Component] = static_cast<float>(
						(1.0 - Weight) * From.Scale[Component] + Weight * To.Scale[Component]);
				}
			Blended.Rotation = SampleRotation(From.Rotation, To.Rotation, Weight);
			Result.push_back(std::move(Blended));
			}
			return Result;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::ResourceLimitExceeded,
				"Insufficient memory while blending animation poses."));
		}
	}

	std::expected<void, GltfAnimationPlaybackError> PlayGltfAnimationInstance(
		GltfAnimationInstancePlayback& Instance,
		const ImportedGltfSkeletalMesh& Mesh,
		std::string_view ClipKey,
		bool Looping,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation instance play request was cancelled."));
		if (ClipKey.empty() || FindAnimationClip(Mesh, ClipKey) == nullptr)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
				"Animation instance clip key is not present in the immutable mesh asset."));

		try
		{
			GltfAnimationInstancePlayback Next;
			Next.CurrentClipKey = ClipKey;
			Next.CurrentClock.Looping = Looping;
			if (CancellationToken.stop_requested())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
					"Animation instance play request was cancelled."));
			Instance = std::move(Next);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::ResourceLimitExceeded,
				"Insufficient memory while setting the animation instance clip."));
		}
	}

	std::expected<void, GltfAnimationPlaybackError> TransitionGltfAnimationInstance(
		GltfAnimationInstancePlayback& Instance,
		const ImportedGltfSkeletalMesh& Mesh,
		std::span<const GltfAnimationNodePose> RestPose,
		std::string_view TargetClipKey,
		float DurationSeconds,
		bool Looping,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation instance transition was cancelled."));
		if (!std::isfinite(DurationSeconds) || DurationSeconds < 0.0f)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Animation transition duration must be finite and nonnegative."));
		const ImportedGltfAnimationClip* const TargetClip = FindAnimationClip(Mesh, TargetClipKey);
		if (TargetClip == nullptr)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
				"Animation transition target clip key is not present in the immutable mesh asset."));
		if (Instance.CurrentClipKey.empty())
			return PlayGltfAnimationInstance(Instance, Mesh, TargetClipKey, Looping, CancellationToken);
		if (Instance.CurrentClipKey == TargetClipKey && Instance.TargetClipKey.empty())
			return BeginGltfAnimationCrossfade(Instance.Crossfade, 0.0f, CancellationToken);

		const ImportedGltfAnimationClip* const CurrentClip = FindAnimationClip(Mesh, Instance.CurrentClipKey);
		if (CurrentClip == nullptr)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
				"Animation instance current clip key is no longer present in its asset."));

		try
		{
			std::vector<GltfAnimationNodePose> SourcePose = Instance.LastPose;
			if (SourcePose.empty())
			{
				auto Sampled = SampleGltfAnimationPose(*CurrentClip, RestPose, Instance.CurrentClock, CancellationToken);
				if (!Sampled) return std::unexpected(Sampled.error());
				SourcePose = std::move(*Sampled);
			}
			if (SourcePose.size() != RestPose.size() || SourcePose.size() > MaximumGltfAnimationPoseNodes)
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidPose,
					"Animation transition source pose does not match the supplied rest pose."));
			for (size_t Index = 0; Index < SourcePose.size(); ++Index)
			{
				if ((Index & 0x3fu) == 0 && CancellationToken.stop_requested())
					return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
						"Animation instance transition was cancelled."));
				if (SourcePose[Index].NodeKey != RestPose[Index].NodeKey ||
					!IsFinitePoseValue(SourcePose[Index].Translation) || !IsFinitePoseValue(SourcePose[Index].Rotation) ||
					!IsFinitePoseValue(SourcePose[Index].Scale))
					return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidPose,
						"Animation transition source pose does not match the rest-pose node identities."));
			}

			GltfAnimationInstancePlayback Next = Instance;
			Next.TargetClipKey = TargetClipKey;
			Next.TargetClock = {};
			Next.TargetClock.Looping = Looping;
			Next.TargetClock.Speed = Next.CurrentClock.Speed;
			Next.TargetClock.Paused = Next.CurrentClock.Paused;
			if (auto Fade = BeginGltfAnimationCrossfade(Next.Crossfade, DurationSeconds, CancellationToken); !Fade)
				return std::unexpected(Fade.error());
			Next.TransitionSourcePose = std::move(SourcePose);
			Next.LastPose = Next.TransitionSourcePose;
			if (CancellationToken.stop_requested())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
					"Animation instance transition was cancelled."));
			Instance = std::move(Next);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::ResourceLimitExceeded,
				"Insufficient memory while starting an animation instance transition."));
		}
	}

	std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError> AdvanceGltfAnimationInstance(
		GltfAnimationInstancePlayback& Instance,
		const ImportedGltfSkeletalMesh& Mesh,
		std::span<const GltfAnimationNodePose> RestPose,
		float DeltaSeconds,
		std::stop_token CancellationToken,
		const GltfAnimationClipValidationMap* ValidatedClips)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
				"Animation instance advance was cancelled."));
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f)
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClock,
				"Animation instance delta must be finite and nonnegative."));

		try
		{
			const auto SampleClip = [&](const ImportedGltfAnimationClip& Clip,
				const GltfAnimationPlaybackClock& Clock)
				-> std::expected<std::vector<GltfAnimationNodePose>, GltfAnimationPlaybackError>
			{
				if (!ValidatedClips) return SampleGltfAnimationPose(Clip, RestPose, Clock, CancellationToken);
				const auto Validation = ValidatedClips->find(Clip.StableKey);
				if (Validation == ValidatedClips->end() || !Validation->second.IsFor(Clip))
					return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
						"Validated animation clip is unavailable for the immutable skeletal product."));
				return SampleGltfAnimationPose(Clip, Validation->second, RestPose, Clock, CancellationToken);
			};
			GltfAnimationInstancePlayback Next = Instance;
			const ImportedGltfAnimationClip* const CurrentClip = FindAnimationClip(Mesh, Next.CurrentClipKey);
			if (CurrentClip == nullptr)
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
					"Animation instance current clip key is not present in the immutable mesh asset."));

			if (Next.TargetClipKey.empty())
			{
				if (auto Advanced = AdvanceGltfAnimationPlayback(Next.CurrentClock, CurrentClip->Duration,
					DeltaSeconds, CancellationToken); !Advanced)
					return std::unexpected(Advanced.error());
				auto Pose = SampleClip(*CurrentClip, Next.CurrentClock);
				if (!Pose) return std::unexpected(Pose.error());
				Next.LastPose = *Pose;
				if (CancellationToken.stop_requested())
					return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
						"Animation instance advance was cancelled."));
				Instance = std::move(Next);
				return Pose;
			}

			const ImportedGltfAnimationClip* const TargetClip = FindAnimationClip(Mesh, Next.TargetClipKey);
			if (TargetClip == nullptr)
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidClip,
					"Animation instance target clip key is not present in the immutable mesh asset."));
			if (Next.TransitionSourcePose.size() != RestPose.size())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::InvalidPose,
					"Animation transition source pose does not match the supplied rest pose."));
			if (auto Advanced = AdvanceGltfAnimationPlayback(Next.TargetClock, TargetClip->Duration,
				DeltaSeconds, CancellationToken); !Advanced)
				return std::unexpected(Advanced.error());
			auto Weight = AdvanceGltfAnimationCrossfade(
				Next.Crossfade, Next.CurrentClock.Paused ? 0.0f : DeltaSeconds, CancellationToken);
			if (!Weight) return std::unexpected(Weight.error());
			auto TargetPose = SampleClip(*TargetClip, Next.TargetClock);
			if (!TargetPose) return std::unexpected(TargetPose.error());
			auto BlendedPose = BlendGltfAnimationPoses(
				Next.TransitionSourcePose, *TargetPose, *Weight, CancellationToken);
			if (!BlendedPose) return std::unexpected(BlendedPose.error());
			Next.LastPose = *BlendedPose;
			if (!Next.Crossfade.Active)
			{
				Next.CurrentClipKey = Next.TargetClipKey;
				Next.CurrentClock = Next.TargetClock;
				Next.TargetClipKey.clear();
				Next.TargetClock = {};
				Next.TransitionSourcePose.clear();
			}
			if (CancellationToken.stop_requested())
				return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::Cancelled,
					"Animation instance advance was cancelled."));
			Instance = std::move(Next);
			return BlendedPose;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakePlaybackError(GltfAnimationPlaybackErrorCode::ResourceLimitExceeded,
				"Insufficient memory while advancing an animation instance."));
		}
	}
}
