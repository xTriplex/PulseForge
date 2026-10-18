#include "Renderer/Vulkan/VulkanSupport.h"

#include <algorithm>
#include <limits>

namespace PulseForge::VulkanSupport
{
	std::optional<PresentationPolicy> ParsePresentationPolicy(std::string_view Value) noexcept
	{
		if (Value.empty() || Value == "auto" || Value == "compatible") return PresentationPolicy::Compatible;
		if (Value == "require-fences") return PresentationPolicy::RequireFences;
		return std::nullopt;
	}

	PresentationInstanceExtensionPlan PlanPresentationInstanceExtensions(
		const PresentationInstanceExtensionAvailability& Available) noexcept
	{
		PresentationInstanceExtensionPlan Plan;
		if (!Available.Surface || !Available.SurfaceCapabilities2) return Plan;

		Plan.SurfaceCapabilities2 = true;
		Plan.KhrSurfaceMaintenance = Available.KhrSurfaceMaintenance;
		Plan.ExtSurfaceMaintenance = Available.ExtSurfaceMaintenance;
		return Plan;
	}

	bool HasCompatiblePresentationFencePath(const PresentationInstanceExtensionPlan& EnabledInstanceExtensions,
		const PresentationDeviceExtensionAvailability& AvailableDeviceExtensions) noexcept
	{
		return AvailableDeviceExtensions.Swapchain &&
			   ((EnabledInstanceExtensions.KhrSurfaceMaintenance && AvailableDeviceExtensions.KhrSwapchainMaintenance) ||
				   (EnabledInstanceExtensions.ExtSurfaceMaintenance && AvailableDeviceExtensions.ExtSwapchainMaintenance));
	}

	PresentationFenceCapability EvaluatePresentationFenceCapability(
		const PresentationInstanceExtensionAvailability& AvailableInstanceExtensions,
		const PresentationInstanceExtensionPlan& EnabledInstanceExtensions,
		const PresentationDeviceExtensionAvailability& AvailableDeviceExtensions, bool SwapchainMaintenanceFeatureSupported) noexcept
	{
		if (!AvailableInstanceExtensions.Surface)
			return { PresentationRetirementMechanism::LegacyBestEffort, PresentationFenceCapabilityReason::MissingSurfaceExtension };
		if (!AvailableInstanceExtensions.SurfaceCapabilities2 || !EnabledInstanceExtensions.SurfaceCapabilities2)
			return { PresentationRetirementMechanism::LegacyBestEffort, PresentationFenceCapabilityReason::MissingSurfaceCapabilities2 };

		const bool KhrPath = EnabledInstanceExtensions.KhrSurfaceMaintenance && AvailableDeviceExtensions.KhrSwapchainMaintenance;
		const bool ExtPath = EnabledInstanceExtensions.ExtSurfaceMaintenance && AvailableDeviceExtensions.ExtSwapchainMaintenance;
		if (!EnabledInstanceExtensions.KhrSurfaceMaintenance && !EnabledInstanceExtensions.ExtSurfaceMaintenance)
			return { PresentationRetirementMechanism::LegacyBestEffort,
				PresentationFenceCapabilityReason::MissingSurfaceMaintenanceExtension };
		if (!AvailableDeviceExtensions.Swapchain || (!KhrPath && !ExtPath))
			return { PresentationRetirementMechanism::LegacyBestEffort,
				PresentationFenceCapabilityReason::MissingMatchingSwapchainMaintenanceExtension };
		if (!SwapchainMaintenanceFeatureSupported)
			return { PresentationRetirementMechanism::LegacyBestEffort, PresentationFenceCapabilityReason::FeatureUnsupported };

		if (KhrPath) return { PresentationRetirementMechanism::KhrMaintenanceFence, PresentationFenceCapabilityReason::Available };
		return { PresentationRetirementMechanism::ExtMaintenanceFence, PresentationFenceCapabilityReason::Available };
	}

	PresentationPolicySelection ResolvePresentationPolicy(
		PresentationPolicy Policy, const PresentationFenceCapability& Capability, bool ForceLegacyForTesting) noexcept
	{
		if (ForceLegacyForTesting && Policy == PresentationPolicy::RequireFences)
			return { false, PresentationRetirementMechanism::LegacyBestEffort,
				PresentationFenceCapabilityReason::StrictPolicyConflictsWithForcedLegacy };
		if (ForceLegacyForTesting)
			return { true, PresentationRetirementMechanism::LegacyBestEffort, PresentationFenceCapabilityReason::ForcedLegacyForTesting };
		if (Capability.HasFences()) return { true, Capability.Mechanism, PresentationFenceCapabilityReason::Available };
		if (Policy == PresentationPolicy::RequireFences)
			return { false, PresentationRetirementMechanism::LegacyBestEffort, Capability.Reason };
		return { true, PresentationRetirementMechanism::LegacyBestEffort, Capability.Reason };
	}

	bool IsBetterPresentationCandidate(bool CandidateHasFences, int CandidateScore, bool CurrentHasFences, int CurrentScore) noexcept
	{
		if (CandidateHasFences != CurrentHasFences) return CandidateHasFences;
		return CandidateScore > CurrentScore;
	}

	PresentationRequestDisposition ResolvePresentationRequestDisposition(PresentationQueueResult Result) noexcept
	{
		return ResolvePresentationResult(Result).RequestDisposition;
	}

	PresentationResultHandling ResolvePresentationResult(PresentationQueueResult Result) noexcept
	{
		switch (Result)
		{
		case PresentationQueueResult::Success:
			return { PresentationRequestDisposition::Enqueued, PresentationRecoveryAction::Continue, true };
		case PresentationQueueResult::Suboptimal:
			return { PresentationRequestDisposition::Enqueued, PresentationRecoveryAction::RecreateSwapchain, true };
		case PresentationQueueResult::OutOfDate:
		case PresentationQueueResult::FullscreenExclusiveLost:
		case PresentationQueueResult::PresentTimingQueueFull:
			return { PresentationRequestDisposition::Enqueued, PresentationRecoveryAction::RecreateSwapchain, false };
		case PresentationQueueResult::SurfaceLost:
			return { PresentationRequestDisposition::Enqueued, PresentationRecoveryAction::BlockHost, false };
		case PresentationQueueResult::OutOfHostMemory:
		case PresentationQueueResult::OutOfDeviceMemory:
			return { PresentationRequestDisposition::NotEnqueued, PresentationRecoveryAction::BlockHost, false };
		case PresentationQueueResult::DeviceLost:
		case PresentationQueueResult::OtherFailure:
			return { PresentationRequestDisposition::Indeterminate, PresentationRecoveryAction::BlockHost, false };
		}
		return { PresentationRequestDisposition::Indeterminate, PresentationRecoveryAction::BlockHost, false };
	}

	const char* ToString(PresentationPolicy Policy) noexcept
	{
		switch (Policy)
		{
		case PresentationPolicy::Compatible:
			return "compatible/auto";
		case PresentationPolicy::RequireFences:
			return "require presentation fences";
		}
		return "unknown presentation policy";
	}

	const char* ToString(PresentationRetirementMechanism Mechanism) noexcept
	{
		switch (Mechanism)
		{
		case PresentationRetirementMechanism::LegacyBestEffort:
			return "legacy best-effort retirement";
		case PresentationRetirementMechanism::KhrMaintenanceFence:
			return "VK_KHR_swapchain_maintenance1 presentation fences";
		case PresentationRetirementMechanism::ExtMaintenanceFence:
			return "VK_EXT_swapchain_maintenance1 presentation fences";
		}
		return "unknown presentation retirement mechanism";
	}

	const char* ToString(PresentationFenceCapabilityReason Reason) noexcept
	{
		switch (Reason)
		{
		case PresentationFenceCapabilityReason::Available:
			return "maintenance presentation fence support is available";
		case PresentationFenceCapabilityReason::MissingSurfaceExtension:
			return "VK_KHR_surface is unavailable";
		case PresentationFenceCapabilityReason::MissingSurfaceCapabilities2:
			return "VK_KHR_get_surface_capabilities2 is unavailable or could not be enabled";
		case PresentationFenceCapabilityReason::MissingSurfaceMaintenanceExtension:
			return "neither VK_KHR_surface_maintenance1 nor VK_EXT_surface_maintenance1 is available";
		case PresentationFenceCapabilityReason::MissingMatchingSwapchainMaintenanceExtension:
			return "no matching swapchain-maintenance device extension is available";
		case PresentationFenceCapabilityReason::FeatureUnsupported:
			return "the swapchainMaintenance1 device feature is unsupported";
		case PresentationFenceCapabilityReason::ForcedLegacyForTesting:
			return "the Debug-only PULSEFORGE_VULKAN_FORCE_LEGACY_RETIREMENT test override is active";
		case PresentationFenceCapabilityReason::StrictPolicyConflictsWithForcedLegacy:
			return "require-fences conflicts with the Debug-only forced-legacy test override";
		}
		return "unknown presentation fence capability reason";
	}

	const char* ToString(PresentationRequestDisposition Disposition) noexcept
	{
		switch (Disposition)
		{
		case PresentationRequestDisposition::Enqueued:
			return "enqueued";
		case PresentationRequestDisposition::NotEnqueued:
			return "not enqueued";
		case PresentationRequestDisposition::Indeterminate:
			return "indeterminate";
		}
		return "unknown presentation request disposition";
	}

	SwapchainFrameStartAction ResolveSwapchainFrameStartAction(
		bool HasCurrentGeneration,
		bool RecreationRequested,
		bool CanCreateGeneration,
		bool CurrentGenerationUsable) noexcept
	{
		if (RecreationRequested && CanCreateGeneration)
			return SwapchainFrameStartAction::Recreate;
		return HasCurrentGeneration && CurrentGenerationUsable
			? SwapchainFrameStartAction::PresentCurrent
			: SwapchainFrameStartAction::Skip;
	}

	bool CanCreateSwapchainGeneration(bool PresentationFencesEnabled, bool HasCurrentGeneration, std::size_t DeferredGenerationCount,
		bool CurrentGenerationOutOfDate, bool RecoveryAttemptedAtLimit) noexcept
	{
		if (PresentationFencesEnabled)
			return true;
		if (DeferredGenerationCount < MaximumDeferredSwapchainGenerations)
			return true;
		if (DeferredGenerationCount > MaximumDeferredSwapchainGenerations)
			return false;
		const bool CurrentGenerationNeedsRecovery = !HasCurrentGeneration || CurrentGenerationOutOfDate;
		return CurrentGenerationNeedsRecovery && !RecoveryAttemptedAtLimit && MaximumOutOfDateRecoveryGenerations > 0;
	}

	bool ShouldRetireDeferredSwapchains(
		bool PresentationFencesEnabled,
		bool FirstPresentationPending,
		uint32_t FirstPresentationImageIndex,
		uint32_t AcquiredImageIndex,
		bool AcquireSemaphoreWaitSubmitted) noexcept
	{
		return !PresentationFencesEnabled && FirstPresentationPending &&
			FirstPresentationImageIndex == AcquiredImageIndex && AcquireSemaphoreWaitSubmitted;
	}

	bool SupportsRequiredDeviceFeatures(const RequiredDeviceFeatures& Features)
	{
		return Features.TimelineSemaphore && Features.Synchronization2 && Features.DynamicRendering &&
			Features.ShaderDemoteToHelperInvocation;
	}

	bool SupportsFormatUsage(
		const FormatFeatureSupport& Features,
		bool RequiresSampledImage,
		bool RequiresColorAttachment,
		bool RequiresLinearFiltering) noexcept
	{
		return (!RequiresSampledImage || Features.SampledImage) &&
			(!RequiresColorAttachment || Features.ColorAttachment) &&
			(!RequiresLinearFiltering || (Features.SampledImage && Features.LinearFiltering));
	}

	std::optional<QueueFamilySelection> ChooseQueueFamilies(std::span<const QueueFamily> Families)
	{
		std::optional<uint32_t> GraphicsIndex;
		std::optional<uint32_t> PresentIndex;

		for (const QueueFamily& Family : Families)
		{
			if (Family.QueueCount == 0)
				continue;

			if (Family.SupportsGraphics && Family.SupportsPresent)
				return QueueFamilySelection{ Family.Index, Family.Index };

			if (Family.SupportsGraphics && !GraphicsIndex)
				GraphicsIndex = Family.Index;

			if (Family.SupportsPresent && !PresentIndex)
				PresentIndex = Family.Index;
		}

		if (!GraphicsIndex || !PresentIndex)
			return std::nullopt;

		return QueueFamilySelection{ *GraphicsIndex, *PresentIndex };
	}

	std::optional<SurfaceFormat> ChooseSurfaceFormat(
		std::span<const SurfaceFormat> Formats,
		uint32_t PreferredFormat,
		uint32_t PreferredColorSpace,
		uint32_t UndefinedFormat)
	{
		if (Formats.empty())
			return std::nullopt;

		if (Formats.size() == 1 && Formats.front().Format == UndefinedFormat)
			return SurfaceFormat{ PreferredFormat, Formats.front().ColorSpace };

		for (const SurfaceFormat& Format : Formats)
		{
			if (Format.Format == PreferredFormat && Format.ColorSpace == PreferredColorSpace)
				return Format;
		}

		return Formats.front();
	}

	std::optional<PresentMode> ChoosePresentMode(std::span<const PresentMode> Modes, bool VSync)
	{
		if (Modes.empty())
			return std::nullopt;

		const auto HasMode = [Modes](PresentMode Mode)
		{
			return std::find(Modes.begin(), Modes.end(), Mode) != Modes.end();
		};

		if (VSync)
		{
			if (HasMode(PresentMode::Fifo))
				return PresentMode::Fifo;
		}
		else
		{
			if (HasMode(PresentMode::Mailbox))
				return PresentMode::Mailbox;
			if (HasMode(PresentMode::Immediate))
				return PresentMode::Immediate;
			if (HasMode(PresentMode::Fifo))
				return PresentMode::Fifo;
		}

		return Modes.front();
	}

	Extent2D ChooseExtent(
		Extent2D Requested,
		Extent2D Current,
		bool HasFixedExtent,
		Extent2D Minimum,
		Extent2D Maximum)
	{
		if (HasFixedExtent)
			return Current;

		if (Requested.Width == 0 || Requested.Height == 0)
			return {};

		return {
			std::clamp(Requested.Width, Minimum.Width, Maximum.Width),
			std::clamp(Requested.Height, Minimum.Height, Maximum.Height)
		};
	}

	uint32_t ChooseImageCount(uint32_t Minimum, uint32_t Maximum)
	{
		if (Minimum == std::numeric_limits<uint32_t>::max())
			return Minimum;

		const uint32_t Preferred = Minimum + 1;
		return Maximum > 0 ? std::min(Preferred, Maximum) : Preferred;
	}
}
