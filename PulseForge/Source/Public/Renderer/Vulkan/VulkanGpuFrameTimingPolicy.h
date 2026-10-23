#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace PulseForge::VulkanSupport
{
	struct GpuFrameTimingState
	{
		uint64_t SessionId = 0;
		uint64_t FrameSequence = 0;
		std::optional<uint64_t> SubmissionValue;
		bool Pending = false;
		bool Recording = false;
		bool Failed = false;
	};

	enum class GpuFrameTimingRetirementDecision : uint8_t
	{
		NotPending,
		Recording,
		Failed,
		SubmissionUnassigned,
		CompletionUnavailable,
		SubmissionIncomplete,
		SubmissionComplete
	};

	[[nodiscard]] inline bool BeginGpuFrameTiming(
		GpuFrameTimingState& Timing,
		uint64_t SessionId,
		uint64_t FrameSequence) noexcept
	{
		if (Timing.Pending || Timing.Recording) return false;
		Timing = { SessionId, FrameSequence, std::nullopt, false, true, false };
		return true;
	}

	[[nodiscard]] inline bool EndGpuFrameTimingRecording(GpuFrameTimingState& Timing) noexcept
	{
		if (!Timing.Recording || Timing.Pending) return false;
		Timing.Recording = false;
		Timing.Pending = true;
		return true;
	}

	[[nodiscard]] inline bool AssociateGpuFrameTimingSubmission(
		GpuFrameTimingState& Timing,
		uint64_t SubmissionValue) noexcept
	{
		if (!Timing.Pending || Timing.Recording || Timing.Failed || SubmissionValue == 0 || Timing.SubmissionValue)
			return false;
		Timing.SubmissionValue = SubmissionValue;
		return true;
	}

	inline void FailGpuFrameTiming(GpuFrameTimingState& Timing) noexcept
	{
		if (Timing.Pending || Timing.Recording) Timing.Failed = true;
	}

	inline void ResetGpuFrameTiming(GpuFrameTimingState& Timing) noexcept
	{
		Timing = {};
	}

	template <typename RetirementAction>
	[[nodiscard]] GpuFrameTimingRetirementDecision TryRetireGpuFrameTiming(
		const GpuFrameTimingState& Timing,
		std::optional<uint64_t> CompletedSubmissionValue,
		RetirementAction&& RetireQuery)
	{
		if (Timing.Recording) return GpuFrameTimingRetirementDecision::Recording;
		if (Timing.Failed) return GpuFrameTimingRetirementDecision::Failed;
		if (!Timing.Pending) return GpuFrameTimingRetirementDecision::NotPending;
		if (!Timing.SubmissionValue || *Timing.SubmissionValue == 0)
			return GpuFrameTimingRetirementDecision::SubmissionUnassigned;
		if (!CompletedSubmissionValue) return GpuFrameTimingRetirementDecision::CompletionUnavailable;
		if (*CompletedSubmissionValue < *Timing.SubmissionValue)
			return GpuFrameTimingRetirementDecision::SubmissionIncomplete;

		std::invoke(std::forward<RetirementAction>(RetireQuery));
		return GpuFrameTimingRetirementDecision::SubmissionComplete;
	}
} // namespace PulseForge::VulkanSupport
