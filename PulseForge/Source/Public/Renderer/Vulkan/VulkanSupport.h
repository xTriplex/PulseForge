#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace PulseForge::VulkanSupport
{
	inline constexpr std::size_t MaximumDeferredSwapchainGenerations = 4;
	inline constexpr std::size_t MaximumOutOfDateRecoveryGenerations = 1;

	enum class PresentationPolicy : uint8_t
	{
		Compatible,
		RequireFences
	};

	enum class PresentationRetirementMechanism : uint8_t
	{
		LegacyBestEffort,
		KhrMaintenanceFence,
		ExtMaintenanceFence
	};

	enum class PresentationFenceCapabilityReason : uint8_t
	{
		Available,
		MissingSurfaceExtension,
		MissingSurfaceCapabilities2,
		MissingSurfaceMaintenanceExtension,
		MissingMatchingSwapchainMaintenanceExtension,
		FeatureUnsupported,
		ForcedLegacyForTesting,
		StrictPolicyConflictsWithForcedLegacy
	};

	struct PresentationInstanceExtensionAvailability
	{
		bool Surface = false;
		bool SurfaceCapabilities2 = false;
		bool KhrSurfaceMaintenance = false;
		bool ExtSurfaceMaintenance = false;
	};

	struct PresentationInstanceExtensionPlan
	{
		bool SurfaceCapabilities2 = false;
		bool KhrSurfaceMaintenance = false;
		bool ExtSurfaceMaintenance = false;
	};

	struct PresentationDeviceExtensionAvailability
	{
		bool Swapchain = false;
		bool KhrSwapchainMaintenance = false;
		bool ExtSwapchainMaintenance = false;
	};

	struct PresentationFenceCapability
	{
		PresentationRetirementMechanism Mechanism = PresentationRetirementMechanism::LegacyBestEffort;
		PresentationFenceCapabilityReason Reason = PresentationFenceCapabilityReason::MissingSurfaceExtension;

		[[nodiscard]] bool HasFences() const noexcept { return Mechanism != PresentationRetirementMechanism::LegacyBestEffort; }
	};

	struct PresentationPolicySelection
	{
		bool Eligible = false;
		PresentationRetirementMechanism Mechanism = PresentationRetirementMechanism::LegacyBestEffort;
		PresentationFenceCapabilityReason Reason = PresentationFenceCapabilityReason::MissingSurfaceExtension;
	};

	[[nodiscard]] std::optional<PresentationPolicy> ParsePresentationPolicy(std::string_view Value) noexcept;
	[[nodiscard]] PresentationInstanceExtensionPlan PlanPresentationInstanceExtensions(
		const PresentationInstanceExtensionAvailability& Available) noexcept;
	[[nodiscard]] bool HasCompatiblePresentationFencePath(const PresentationInstanceExtensionPlan& EnabledInstanceExtensions,
		const PresentationDeviceExtensionAvailability& AvailableDeviceExtensions) noexcept;
	[[nodiscard]] PresentationFenceCapability EvaluatePresentationFenceCapability(
		const PresentationInstanceExtensionAvailability& AvailableInstanceExtensions,
		const PresentationInstanceExtensionPlan& EnabledInstanceExtensions,
		const PresentationDeviceExtensionAvailability& AvailableDeviceExtensions, bool SwapchainMaintenanceFeatureSupported) noexcept;
	[[nodiscard]] PresentationPolicySelection ResolvePresentationPolicy(
		PresentationPolicy Policy, const PresentationFenceCapability& Capability, bool ForceLegacyForTesting) noexcept;
	[[nodiscard]] bool IsBetterPresentationCandidate(
		bool CandidateHasFences, int CandidateScore, bool CurrentHasFences, int CurrentScore) noexcept;

	enum class PresentationQueueResult : uint8_t
	{
		Success,
		Suboptimal,
		OutOfDate,
		SurfaceLost,
		FullscreenExclusiveLost,
		PresentTimingQueueFull,
		OutOfHostMemory,
		OutOfDeviceMemory,
		DeviceLost,
		OtherFailure
	};

	[[nodiscard]] constexpr bool IsSharedDeviceLoss(PresentationQueueResult Result) noexcept
	{
		return Result == PresentationQueueResult::DeviceLost;
	}

	enum class PresentationRequestDisposition : uint8_t
	{
		Enqueued,
		NotEnqueued,
		Indeterminate
	};

	enum class PresentationRecoveryAction : uint8_t
	{
		Continue,
		RecreateSwapchain,
		BlockHost
	};

	struct PresentationResultHandling
	{
		PresentationRequestDisposition RequestDisposition = PresentationRequestDisposition::Indeterminate;
		PresentationRecoveryAction RecoveryAction = PresentationRecoveryAction::BlockHost;
		bool CurrentSwapchainUsable = false;
	};

	[[nodiscard]] PresentationRequestDisposition ResolvePresentationRequestDisposition(PresentationQueueResult Result) noexcept;
	[[nodiscard]] PresentationResultHandling ResolvePresentationResult(PresentationQueueResult Result) noexcept;
	[[nodiscard]] const char* ToString(PresentationPolicy Policy) noexcept;
	[[nodiscard]] const char* ToString(PresentationRetirementMechanism Mechanism) noexcept;
	[[nodiscard]] const char* ToString(PresentationFenceCapabilityReason Reason) noexcept;
	[[nodiscard]] const char* ToString(PresentationRequestDisposition Disposition) noexcept;

	enum class SwapchainFrameStartAction : uint8_t
	{
		Skip,
		Recreate,
		PresentCurrent
	};

	[[nodiscard]] SwapchainFrameStartAction ResolveSwapchainFrameStartAction(
		bool HasCurrentGeneration,
		bool RecreationRequested,
		bool CanCreateGeneration,
		bool CurrentGenerationUsable = true) noexcept;

	[[nodiscard]] bool CanCreateSwapchainGeneration(bool PresentationFencesEnabled, bool HasCurrentGeneration,
		std::size_t DeferredGenerationCount, bool CurrentGenerationOutOfDate = false, bool RecoveryAttemptedAtLimit = false) noexcept;

	[[nodiscard]] bool ShouldRetireDeferredSwapchains(
		bool PresentationFencesEnabled,
		bool FirstPresentationPending,
		uint32_t FirstPresentationImageIndex,
		uint32_t AcquiredImageIndex,
		bool AcquireSemaphoreWaitSubmitted) noexcept;

	struct RequiredDeviceFeatures
	{
		bool TimelineSemaphore = false;
		bool Synchronization2 = false;
		bool DynamicRendering = false;
		bool ShaderDemoteToHelperInvocation = false;
	};

	bool SupportsRequiredDeviceFeatures(const RequiredDeviceFeatures& Features);

	struct DescriptorSetLimitCheck
	{
		uint64_t RequiredCount = 0;
		uint32_t MaximumCount = 0;

		[[nodiscard]] bool IsSupported() const noexcept { return RequiredCount <= MaximumCount; }
	};

	[[nodiscard]] DescriptorSetLimitCheck CheckDescriptorSetLimit(
		std::span<const uint32_t> RegisterSpaces, uint32_t MaximumCount) noexcept;

	struct FormatFeatureSupport
	{
		bool SampledImage = false;
		bool ColorAttachment = false;
		bool LinearFiltering = false;
	};

	[[nodiscard]] bool SupportsFormatUsage(
		const FormatFeatureSupport& Features,
		bool RequiresSampledImage,
		bool RequiresColorAttachment,
		bool RequiresLinearFiltering) noexcept;

	struct QueueFamily
	{
		uint32_t Index = 0;
		uint32_t QueueCount = 0;
		bool SupportsGraphics = false;
		bool SupportsPresent = false;
	};

	struct QueueFamilySelection
	{
		uint32_t GraphicsIndex = 0;
		uint32_t PresentIndex = 0;
	};

	std::optional<QueueFamilySelection> ChooseQueueFamilies(std::span<const QueueFamily> Families);

	enum class GpuTimestampCapabilityReason : uint8_t
	{
		Supported,
		QueueDoesNotSupportTimestamps,
		InvalidTimestampPeriod
	};

	[[nodiscard]] GpuTimestampCapabilityReason EvaluateGpuTimestampCapability(
		uint32_t TimestampValidBits, float TimestampPeriodNanoseconds) noexcept;
	[[nodiscard]] const char* ToString(GpuTimestampCapabilityReason Reason) noexcept;

	struct SurfaceFormat
	{
		uint32_t Format = 0;
		uint32_t ColorSpace = 0;
	};

	std::optional<SurfaceFormat> ChooseSurfaceFormat(
		std::span<const SurfaceFormat> Formats,
		uint32_t PreferredFormat,
		uint32_t PreferredColorSpace,
		uint32_t UndefinedFormat);

	enum class PresentMode
	{
		Immediate,
		Mailbox,
		Fifo,
		Other
	};

	std::optional<PresentMode> ChoosePresentMode(std::span<const PresentMode> Modes, bool VSync);

	struct Extent2D
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	Extent2D ChooseExtent(
		Extent2D Requested,
		Extent2D Current,
		bool HasFixedExtent,
		Extent2D Minimum,
		Extent2D Maximum);

	uint32_t ChooseImageCount(uint32_t Minimum, uint32_t Maximum);
} // namespace PulseForge::VulkanSupport
