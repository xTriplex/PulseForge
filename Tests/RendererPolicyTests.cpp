#include "TestSupport.h"

#include "Scene/SceneRenderSnapshot.h"
#include "Renderer/Vulkan/VulkanPresentationResources.h"
#include "Renderer/Vulkan/VulkanRendererShutdown.h"
#include "Renderer/Vulkan/VulkanSupport.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace
{
	using namespace PulseForge::VulkanSupport;
	using namespace PulseForge::VulkanDetail;

	constexpr int32_t TestVkErrorDeviceLost = -4;

	struct FakeRendererShutdown final : RendererShutdownOperations
	{
		bool ShutdownComplete = false;
		bool HasLogicalDevice = true;
		bool DeviceLost = false;
		int32_t LostResult = 0;
		bool PresentationFences = true;
		int NativeHostCount = 0;
		bool GpuTimerQueryPending = false;
		bool GpuTimerQueryReleasedAfterRetirement = false;
		ShutdownWaitResult NvrhiWait;
		ShutdownWaitResult DeviceWait;
		ShutdownWaitResult PresentationWait;
		ShutdownWaitResult FenceWaitAfterLoss;
		ShutdownWaitResult LegacyWaitAfterLoss;
		std::vector<std::string> Calls;

		bool IsShutdownComplete() const noexcept override { return ShutdownComplete; }
		bool HasDevice() const noexcept override { return HasLogicalDevice; }
		bool IsDeviceLost() const noexcept override { return DeviceLost; }
		int32_t DeviceLossResult() const noexcept override { return LostResult; }
		bool UsesPresentationFences() const noexcept override { return PresentationFences; }

		ShutdownWaitResult WaitForNvrhiIdle() noexcept override
		{
			Calls.emplace_back("nvrhi-idle");
			if (NvrhiWait.Outcome == ShutdownWaitOutcome::Complete) GpuTimerQueryPending = false;
			if (NvrhiWait.Outcome == ShutdownWaitOutcome::DeviceLost)
			{
				DeviceLost = true;
				LostResult = NvrhiWait.VulkanResult;
			}
			return NvrhiWait;
		}
		ShutdownWaitResult WaitForDeviceIdle() noexcept override
		{
			Calls.emplace_back("device-idle");
			if (DeviceWait.Outcome == ShutdownWaitOutcome::DeviceLost)
			{
				DeviceLost = true;
				LostResult = DeviceWait.VulkanResult;
			}
			return DeviceWait;
		}
		ShutdownWaitResult WaitForPresentationRetirement() noexcept override
		{
			Calls.emplace_back("presentation-main");
			for (int Index = 0; Index < NativeHostCount; ++Index)
				Calls.emplace_back("presentation-native");
			if (PresentationWait.Outcome == ShutdownWaitOutcome::DeviceLost)
			{
				DeviceLost = true;
				LostResult = PresentationWait.VulkanResult;
			}
			return PresentationWait;
		}
		ShutdownWaitResult WaitForPresentationFencesAfterDeviceLoss() noexcept override
		{
			Calls.emplace_back("lost-fence-main");
			for (int Index = 0; Index < NativeHostCount; ++Index)
				Calls.emplace_back("lost-fence-native");
			return FenceWaitAfterLoss;
		}
		ShutdownWaitResult BestEffortWaitForLegacyPresentationAfterDeviceLoss() noexcept override
		{
			Calls.emplace_back("legacy-best-effort");
			return LegacyWaitAfterLoss;
		}
		bool ReleasePartialInitialization() noexcept override
		{
			Calls.emplace_back("release-partial");
			ShutdownComplete = true;
			return true;
		}
		bool ReleaseDeviceOwnedChildren(ShutdownPresentationReleaseMode Mode) noexcept override
		{
			switch (Mode)
			{
			case ShutdownPresentationReleaseMode::HealthyDevice:
				Calls.emplace_back("release-children-healthy");
				break;
			case ShutdownPresentationReleaseMode::DeviceLostWithFencesRetired:
				Calls.emplace_back("release-children-fenced-loss");
				break;
			case ShutdownPresentationReleaseMode::LegacyBestEffortAfterDeviceLoss:
				Calls.emplace_back("release-children-legacy-loss");
				break;
			}
			GpuTimerQueryReleasedAfterRetirement = !GpuTimerQueryPending;
			return true;
		}
		bool ReleaseNvrhiDevice() noexcept override
		{
			Calls.emplace_back("release-nvrhi");
			return true;
		}
		void DestroyVulkanDevice() noexcept override { Calls.emplace_back("destroy-device"); }
		bool ReleaseInstanceOwnedChildren() noexcept override
		{
			Calls.emplace_back("release-instance");
			ShutdownComplete = true;
			return true;
		}
	};

	void TestQueueFamilySelection(TestRunner& Tests)
	{
		const std::vector<QueueFamily> SameFamily = { { 0, 0, true, true }, { 1, 1, true, true } };
		auto Same = ChooseQueueFamilies(SameFamily);
		PF_CHECK(Tests, Same.has_value());
		PF_CHECK(Tests, Same->GraphicsIndex == 1 && Same->PresentIndex == 1);

		const std::vector<QueueFamily> SeparateFamilies = { { 3, 1, true, false }, { 5, 1, false, true } };
		auto Separate = ChooseQueueFamilies(SeparateFamilies);
		PF_CHECK(Tests, Separate.has_value());
		PF_CHECK(Tests, Separate->GraphicsIndex == 3 && Separate->PresentIndex == 5);

		const std::vector<QueueFamily> MissingPresent = { { 0, 1, true, false } };
		PF_CHECK(Tests, !ChooseQueueFamilies(MissingPresent).has_value());
	}

	void TestRequiredDeviceFeatures(TestRunner& Tests)
	{
		PF_CHECK(Tests, SupportsRequiredDeviceFeatures({ true, true, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ false, true, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, false, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, true, false, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, true, true, false }));
	}

	void TestSwapchainRetirementPolicy(TestRunner& Tests)
	{
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(false, true, false) == SwapchainFrameStartAction::Skip);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(false, true, true) == SwapchainFrameStartAction::Recreate);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, false) == SwapchainFrameStartAction::PresentCurrent);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, true) == SwapchainFrameStartAction::Recreate);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, false, false) == SwapchainFrameStartAction::PresentCurrent);
		PF_CHECK(Tests, CanCreateSwapchainGeneration(true, true, MaximumDeferredSwapchainGenerations));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, true, 0));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations - 1));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, false, MaximumDeferredSwapchainGenerations - 1, true, true));
		PF_CHECK(Tests, !CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, false, MaximumDeferredSwapchainGenerations));
		PF_CHECK(Tests, CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations, true));
		PF_CHECK(Tests, !CanCreateSwapchainGeneration(false, false, MaximumDeferredSwapchainGenerations, false, true));
		PF_CHECK(Tests, !CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations, true, true));
		PF_CHECK(Tests,
			!CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations + MaximumOutOfDateRecoveryGenerations, true));
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, false, false) == SwapchainFrameStartAction::Skip);
		PF_CHECK(Tests, ResolveSwapchainFrameStartAction(true, true, false, true) == SwapchainFrameStartAction::PresentCurrent);
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(false, false, 2, 2, true));
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(false, true, 2, 1, true));
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(false, true, 2, 2, false));
		PF_CHECK(Tests, !ShouldRetireDeferredSwapchains(true, true, 2, 2, true));
		PF_CHECK(Tests, ShouldRetireDeferredSwapchains(false, true, 2, 2, true));
	}

	void TestPresentationCapabilitySelection(TestRunner& Tests)
	{
		const PresentationInstanceExtensionAvailability KhrInstance{ true, true, true, false };
		const PresentationInstanceExtensionAvailability ExtInstance{ true, true, false, true };
		const PresentationInstanceExtensionAvailability BothInstances{ true, true, true, true };
		const PresentationInstanceExtensionAvailability MissingCapabilities2{ true, false, true, true };
		const PresentationInstanceExtensionAvailability MissingSurface{ false, true, true, true };
		const auto KhrEnabled = PlanPresentationInstanceExtensions(KhrInstance);
		const auto ExtEnabled = PlanPresentationInstanceExtensions(ExtInstance);
		const auto BothEnabled = PlanPresentationInstanceExtensions(BothInstances);
		const auto MissingDependencyEnabled = PlanPresentationInstanceExtensions(MissingCapabilities2);

		PF_CHECK(Tests, KhrEnabled.SurfaceCapabilities2 && KhrEnabled.KhrSurfaceMaintenance && !KhrEnabled.ExtSurfaceMaintenance);
		PF_CHECK(Tests, ExtEnabled.SurfaceCapabilities2 && !ExtEnabled.KhrSurfaceMaintenance && ExtEnabled.ExtSurfaceMaintenance);
		PF_CHECK(Tests, BothEnabled.KhrSurfaceMaintenance && BothEnabled.ExtSurfaceMaintenance);
		PF_CHECK(Tests, !MissingDependencyEnabled.SurfaceCapabilities2 && !MissingDependencyEnabled.KhrSurfaceMaintenance &&
							!MissingDependencyEnabled.ExtSurfaceMaintenance);

		const auto KhrOnly = EvaluatePresentationFenceCapability(KhrInstance, KhrEnabled, { true, true, false }, true);
		PF_CHECK(Tests, KhrOnly.Mechanism == PresentationRetirementMechanism::KhrMaintenanceFence);

		const auto ExtOnly = EvaluatePresentationFenceCapability(ExtInstance, ExtEnabled, { true, false, true }, true);
		PF_CHECK(Tests, ExtOnly.Mechanism == PresentationRetirementMechanism::ExtMaintenanceFence);

		const auto PreferKhr = EvaluatePresentationFenceCapability(BothInstances, BothEnabled, { true, true, true }, true);
		PF_CHECK(Tests, PreferKhr.Mechanism == PresentationRetirementMechanism::KhrMaintenanceFence);

		// Both instance variants are enabled, so an EXT-only physical device remains eligible.
		const auto ExtDeviceWithBothInstances =
			EvaluatePresentationFenceCapability(BothInstances, BothEnabled, { true, false, true }, true);
		PF_CHECK(Tests, ExtDeviceWithBothInstances.Mechanism == PresentationRetirementMechanism::ExtMaintenanceFence);

		const auto KHRDeviceWithBothInstances =
			EvaluatePresentationFenceCapability(BothInstances, BothEnabled, { true, true, false }, true);
		PF_CHECK(Tests, KHRDeviceWithBothInstances.Mechanism == PresentationRetirementMechanism::KhrMaintenanceFence);

		const auto MismatchedExtensions = EvaluatePresentationFenceCapability(KhrInstance, KhrEnabled, { true, false, true }, true);
		PF_CHECK(Tests, !MismatchedExtensions.HasFences());
		PF_CHECK(Tests, MismatchedExtensions.Reason == PresentationFenceCapabilityReason::MissingMatchingSwapchainMaintenanceExtension);

		const auto MissingFeature = EvaluatePresentationFenceCapability(KhrInstance, KhrEnabled, { true, true, false }, false);
		PF_CHECK(Tests, !MissingFeature.HasFences());
		PF_CHECK(Tests, MissingFeature.Reason == PresentationFenceCapabilityReason::FeatureUnsupported);

		const auto MissingCaps2 =
			EvaluatePresentationFenceCapability(MissingCapabilities2, MissingDependencyEnabled, { true, true, true }, true);
		PF_CHECK(Tests, MissingCaps2.Reason == PresentationFenceCapabilityReason::MissingSurfaceCapabilities2);

		const auto MissingSurfacePath = EvaluatePresentationFenceCapability(
			MissingSurface, PlanPresentationInstanceExtensions(MissingSurface), { true, true, true }, true);
		PF_CHECK(Tests, MissingSurfacePath.Reason == PresentationFenceCapabilityReason::MissingSurfaceExtension);

		const auto MissingBothVariants = EvaluatePresentationFenceCapability(
			{ true, true, false, false }, PlanPresentationInstanceExtensions({ true, true, false, false }), { true, false, false }, false);
		PF_CHECK(Tests, MissingBothVariants.Reason == PresentationFenceCapabilityReason::MissingSurfaceMaintenanceExtension);

		const auto NoDeviceExtensions = EvaluatePresentationFenceCapability(BothInstances, BothEnabled, { true, false, false }, false);
		PF_CHECK(Tests, NoDeviceExtensions.Reason == PresentationFenceCapabilityReason::MissingMatchingSwapchainMaintenanceExtension);
		PF_CHECK(Tests, !HasCompatiblePresentationFencePath(BothEnabled, { true, false, false }));
	}

	void TestPresentationPoliciesAndCandidatePreference(TestRunner& Tests)
	{
		PF_CHECK(Tests, ParsePresentationPolicy("") == PresentationPolicy::Compatible);
		PF_CHECK(Tests, ParsePresentationPolicy("auto") == PresentationPolicy::Compatible);
		PF_CHECK(Tests, ParsePresentationPolicy("compatible") == PresentationPolicy::Compatible);
		PF_CHECK(Tests, ParsePresentationPolicy("require-fences") == PresentationPolicy::RequireFences);
		PF_CHECK(Tests, !ParsePresentationPolicy("required").has_value());

		const PresentationFenceCapability Supported{ PresentationRetirementMechanism::ExtMaintenanceFence,
			PresentationFenceCapabilityReason::Available };
		const PresentationFenceCapability Unsupported{ PresentationRetirementMechanism::LegacyBestEffort,
			PresentationFenceCapabilityReason::FeatureUnsupported };
		const auto AutoFences = ResolvePresentationPolicy(PresentationPolicy::Compatible, Supported, false);
		PF_CHECK(Tests, AutoFences.Eligible && AutoFences.Mechanism == PresentationRetirementMechanism::ExtMaintenanceFence);
		const auto StrictFences = ResolvePresentationPolicy(PresentationPolicy::RequireFences, Supported, false);
		PF_CHECK(Tests, StrictFences.Eligible && StrictFences.Mechanism == PresentationRetirementMechanism::ExtMaintenanceFence);

		const auto CompatibleFallback = ResolvePresentationPolicy(PresentationPolicy::Compatible, Unsupported, false);
		PF_CHECK(Tests, CompatibleFallback.Eligible);
		PF_CHECK(Tests, CompatibleFallback.Mechanism == PresentationRetirementMechanism::LegacyBestEffort);
		PF_CHECK(Tests, CompatibleFallback.Reason == PresentationFenceCapabilityReason::FeatureUnsupported);
		const auto StrictReject = ResolvePresentationPolicy(PresentationPolicy::RequireFences, Unsupported, false);
		PF_CHECK(Tests, !StrictReject.Eligible);
		PF_CHECK(Tests, StrictReject.Reason == PresentationFenceCapabilityReason::FeatureUnsupported);

		const auto ForcedLegacy = ResolvePresentationPolicy(PresentationPolicy::Compatible, Supported, true);
		PF_CHECK(Tests, ForcedLegacy.Eligible && ForcedLegacy.Mechanism == PresentationRetirementMechanism::LegacyBestEffort);
		PF_CHECK(Tests, ForcedLegacy.Reason == PresentationFenceCapabilityReason::ForcedLegacyForTesting);
		const auto StrictForcedConflict = ResolvePresentationPolicy(PresentationPolicy::RequireFences, Supported, true);
		PF_CHECK(Tests, !StrictForcedConflict.Eligible);
		PF_CHECK(Tests, StrictForcedConflict.Reason == PresentationFenceCapabilityReason::StrictPolicyConflictsWithForcedLegacy);

		// Fence support outranks device-type score; scoring decides among equivalent paths.
		PF_CHECK(Tests, IsBetterPresentationCandidate(true, 200, false, 300));
		PF_CHECK(Tests, !IsBetterPresentationCandidate(false, 300, true, 200));
		PF_CHECK(Tests, IsBetterPresentationCandidate(true, 300, true, 200));
		PF_CHECK(Tests, !IsBetterPresentationCandidate(false, 200, false, 300));

		struct Candidate
		{
			bool Eligible;
			bool HasFences;
			int Score;
		};
		const Candidate Devices[] = { { true, false, 300 }, { true, true, 200 }, { false, false, 500 } };
		std::optional<size_t> CompatibleBest;
		std::optional<size_t> StrictBest;
		for (size_t Index = 0; Index < std::size(Devices); ++Index)
		{
			const Candidate& Device = Devices[Index];
			if (Device.Eligible && (!CompatibleBest || IsBetterPresentationCandidate(Device.HasFences, Device.Score,
														   Devices[*CompatibleBest].HasFences, Devices[*CompatibleBest].Score)))
				CompatibleBest = Index;
			if (Device.Eligible && Device.HasFences &&
				(!StrictBest || IsBetterPresentationCandidate(
									Device.HasFences, Device.Score, Devices[*StrictBest].HasFences, Devices[*StrictBest].Score)))
				StrictBest = Index;
		}
		PF_CHECK(Tests, CompatibleBest == 1);
		PF_CHECK(Tests, StrictBest == 1);
	}

	void TestPresentationRequestDisposition(TestRunner& Tests)
	{
		using Disposition = PresentationRequestDisposition;
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::Success) == Disposition::Enqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::Suboptimal) == Disposition::Enqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::OutOfDate) == Disposition::Enqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::SurfaceLost) == Disposition::Enqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::FullscreenExclusiveLost) == Disposition::Enqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::PresentTimingQueueFull) == Disposition::Enqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::OutOfHostMemory) == Disposition::NotEnqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::OutOfDeviceMemory) == Disposition::NotEnqueued);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::DeviceLost) == Disposition::Indeterminate);
		PF_CHECK(Tests, ResolvePresentationRequestDisposition(PresentationQueueResult::OtherFailure) == Disposition::Indeterminate);

		using Action = PresentationRecoveryAction;
		const auto Success = ResolvePresentationResult(PresentationQueueResult::Success);
		PF_CHECK(Tests, Success.RequestDisposition == Disposition::Enqueued);
		PF_CHECK(Tests, Success.RecoveryAction == Action::Continue && Success.CurrentSwapchainUsable);
		const auto Suboptimal = ResolvePresentationResult(PresentationQueueResult::Suboptimal);
		PF_CHECK(Tests, Suboptimal.RequestDisposition == Disposition::Enqueued);
		PF_CHECK(Tests, Suboptimal.RecoveryAction == Action::RecreateSwapchain && Suboptimal.CurrentSwapchainUsable);
		const auto OutOfDate = ResolvePresentationResult(PresentationQueueResult::OutOfDate);
		PF_CHECK(Tests, OutOfDate.RequestDisposition == Disposition::Enqueued);
		PF_CHECK(Tests, OutOfDate.RecoveryAction == Action::RecreateSwapchain && !OutOfDate.CurrentSwapchainUsable);
		const auto FailedEnqueue = ResolvePresentationResult(PresentationQueueResult::OutOfHostMemory);
		PF_CHECK(Tests, FailedEnqueue.RequestDisposition == Disposition::NotEnqueued);
		PF_CHECK(Tests, FailedEnqueue.RecoveryAction == Action::BlockHost && !FailedEnqueue.CurrentSwapchainUsable);
		const auto LostSurface = ResolvePresentationResult(PresentationQueueResult::SurfaceLost);
		PF_CHECK(Tests, LostSurface.RequestDisposition == Disposition::Enqueued);
		PF_CHECK(Tests, LostSurface.RecoveryAction == Action::BlockHost && !LostSurface.CurrentSwapchainUsable);
		PF_CHECK(Tests, !IsSharedDeviceLoss(PresentationQueueResult::SurfaceLost));
		PF_CHECK(Tests, IsSharedDeviceLoss(PresentationQueueResult::DeviceLost));
		const auto Indeterminate = ResolvePresentationResult(PresentationQueueResult::OtherFailure);
		PF_CHECK(Tests, Indeterminate.RequestDisposition == Disposition::Indeterminate);
		PF_CHECK(Tests, Indeterminate.RecoveryAction == Action::BlockHost && !Indeterminate.CurrentSwapchainUsable);
	}

	void TestFormatUsageCapabilities(TestRunner& Tests)
	{
		const FormatFeatureSupport AllFeatures{ true, true, true };
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, true, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, false, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, false, true, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, true, true));
		PF_CHECK(Tests, SupportsFormatUsage({}, false, false, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ false, true, true }, true, true, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, false, true }, true, true, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, true, false }, true, true, true));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, true, false }, false, false, true));
		PF_CHECK(Tests, !SupportsFormatUsage({ false, false, true }, false, false, true));
	}

	void TestSurfaceAndPresentationSelection(TestRunner& Tests)
	{
		const std::vector<SurfaceFormat> Formats = { { 10, 1 }, { 20, 3 } };
		auto Preferred = ChooseSurfaceFormat(Formats, 20, 3, 0);
		PF_CHECK(Tests, Preferred.has_value());
		PF_CHECK(Tests, Preferred->Format == 20 && Preferred->ColorSpace == 3);

		const std::vector<SurfaceFormat> UndefinedFormat = { { 0, 5 } };
		auto ChosenForUndefined = ChooseSurfaceFormat(UndefinedFormat, 20, 3, 0);
		PF_CHECK(Tests, ChosenForUndefined.has_value());
		PF_CHECK(Tests, ChosenForUndefined->Format == 20 && ChosenForUndefined->ColorSpace == 5);

		const std::vector<PresentMode> Modes = { PresentMode::Immediate, PresentMode::Mailbox, PresentMode::Fifo };
		PF_CHECK(Tests, ChoosePresentMode(Modes, true) == PresentMode::Fifo);
		PF_CHECK(Tests, ChoosePresentMode(Modes, false) == PresentMode::Mailbox);
		const std::vector<PresentMode> NoMailbox = { PresentMode::Immediate, PresentMode::Fifo };
		PF_CHECK(Tests, ChoosePresentMode(NoMailbox, false) == PresentMode::Immediate);
	}

	void TestExtentAndImageCountSelection(TestRunner& Tests)
	{
		const auto Clamped = ChooseExtent({ 2400, 200 }, {}, false, { 640, 480 }, { 1920, 1080 });
		PF_CHECK(Tests, Clamped.Width == 1920 && Clamped.Height == 480);

		const auto Fixed = ChooseExtent({ 800, 600 }, { 1600, 900 }, true, {}, {});
		PF_CHECK(Tests, Fixed.Width == 1600 && Fixed.Height == 900);

		const auto Minimized = ChooseExtent({ 0, 0 }, {}, false, { 640, 480 }, { 1920, 1080 });
		PF_CHECK(Tests, Minimized.Width == 0 && Minimized.Height == 0);
		PF_CHECK(Tests, ChooseImageCount(2, 0) == 3);
		PF_CHECK(Tests, ChooseImageCount(2, 2) == 2);
		PF_CHECK(Tests, ChooseImageCount(3, 5) == 4);
	}

	void TestHealthyRendererShutdownOrder(TestRunner& Tests)
	{
		FakeRendererShutdown Operations;
		Operations.NativeHostCount = 1;
		const ShutdownResult Result = ShutdownRenderer(Operations);
		const std::vector<std::string> Expected = { "nvrhi-idle", "device-idle", "presentation-main", "presentation-native",
			"release-children-healthy", "release-nvrhi", "destroy-device", "release-instance" };
		PF_CHECK(Tests, Result.Disposition == ShutdownDisposition::Complete);
		PF_CHECK(Tests, Operations.Calls == Expected);

		FakeRendererShutdown PendingGpuQuery;
		PendingGpuQuery.GpuTimerQueryPending = true;
		const ShutdownResult PendingQueryResult = ShutdownRenderer(PendingGpuQuery);
		const std::vector<std::string> ExpectedPendingQueryCalls = { "nvrhi-idle", "device-idle", "presentation-main",
			"release-children-healthy", "release-nvrhi", "destroy-device", "release-instance" };
		PF_CHECK(Tests, PendingQueryResult.Disposition == ShutdownDisposition::Complete);
		PF_CHECK(Tests, !PendingGpuQuery.GpuTimerQueryPending);
		PF_CHECK(Tests, PendingGpuQuery.GpuTimerQueryReleasedAfterRetirement);
		PF_CHECK(Tests, PendingGpuQuery.Calls == ExpectedPendingQueryCalls);

		const size_t CallsAfterFirstShutdown = Operations.Calls.size();
		const ShutdownResult SecondResult = ShutdownRenderer(Operations);
		PF_CHECK(Tests, SecondResult.Disposition == ShutdownDisposition::Complete);
		PF_CHECK(Tests, Operations.Calls.size() == CallsAfterFirstShutdown);
	}

	void TestDeviceLossShutdown(TestRunner& Tests)
	{
		FakeRendererShutdown LostDuringPresent;
		LostDuringPresent.NativeHostCount = 1;
		LostDuringPresent.PresentationWait = { ShutdownWaitOutcome::DeviceLost, TestVkErrorDeviceLost };
		LostDuringPresent.FenceWaitAfterLoss = {};
		const ShutdownResult PresentResult = ShutdownRenderer(LostDuringPresent);
		const std::vector<std::string> ExpectedPresentCalls = { "nvrhi-idle", "device-idle", "presentation-main", "presentation-native",
			"lost-fence-main", "lost-fence-native", "release-children-fenced-loss", "release-nvrhi", "destroy-device", "release-instance" };
		PF_CHECK(Tests, PresentResult.Disposition == ShutdownDisposition::CompleteAfterDeviceLoss);
		PF_CHECK(Tests, PresentResult.Stage == ShutdownStage::PresentationRetirement);
		PF_CHECK(Tests, PresentResult.VulkanResult == TestVkErrorDeviceLost);
		PF_CHECK(Tests, LostDuringPresent.Calls == ExpectedPresentCalls);

		FakeRendererShutdown LostDuringNvrhiWait;
		LostDuringNvrhiWait.NvrhiWait = { ShutdownWaitOutcome::DeviceLost, 0 };
		const ShutdownResult NvrhiResult = ShutdownRenderer(LostDuringNvrhiWait);
		PF_CHECK(Tests, NvrhiResult.Disposition == ShutdownDisposition::CompleteAfterDeviceLoss);
		PF_CHECK(Tests, NvrhiResult.Stage == ShutdownStage::NvrhiIdle);
		PF_CHECK(Tests, std::find(LostDuringNvrhiWait.Calls.begin(), LostDuringNvrhiWait.Calls.end(), "device-idle") ==
							LostDuringNvrhiWait.Calls.end());

		FakeRendererShutdown LostDuringDeviceWait;
		LostDuringDeviceWait.DeviceWait = { ShutdownWaitOutcome::DeviceLost, TestVkErrorDeviceLost };
		const ShutdownResult DeviceResult = ShutdownRenderer(LostDuringDeviceWait);
		PF_CHECK(Tests, DeviceResult.Disposition == ShutdownDisposition::CompleteAfterDeviceLoss);
		PF_CHECK(Tests, DeviceResult.Stage == ShutdownStage::DeviceIdle);
		PF_CHECK(Tests, DeviceResult.VulkanResult == TestVkErrorDeviceLost);
		PF_CHECK(Tests, std::find(LostDuringDeviceWait.Calls.begin(), LostDuringDeviceWait.Calls.end(), "presentation-main") ==
							LostDuringDeviceWait.Calls.end());

		FakeRendererShutdown AlreadyLostWithNativeHost;
		AlreadyLostWithNativeHost.DeviceLost = true;
		AlreadyLostWithNativeHost.LostResult = TestVkErrorDeviceLost;
		AlreadyLostWithNativeHost.NativeHostCount = 1;
		const ShutdownResult ExistingLossResult = ShutdownRenderer(AlreadyLostWithNativeHost);
		const auto NativeFenceWait =
			std::find(AlreadyLostWithNativeHost.Calls.begin(), AlreadyLostWithNativeHost.Calls.end(), "lost-fence-native");
		const auto ChildRelease = std::find_if(AlreadyLostWithNativeHost.Calls.begin(), AlreadyLostWithNativeHost.Calls.end(),
			[](const std::string& Call) { return Call.starts_with("release-children-"); });
		PF_CHECK(Tests, ExistingLossResult.Disposition == ShutdownDisposition::CompleteAfterDeviceLoss);
		PF_CHECK(Tests, ExistingLossResult.Stage == ShutdownStage::DeviceAlreadyLost);
		PF_CHECK(Tests, NativeFenceWait < ChildRelease);
	}

	void TestRendererShutdownFailureContracts(TestRunner& Tests)
	{
		FakeRendererShutdown FailedWait;
		FailedWait.DeviceWait = { ShutdownWaitOutcome::Failed, -7 };
		const ShutdownResult FailedResult = ShutdownRenderer(FailedWait);
		PF_CHECK(Tests, FailedResult.Disposition == ShutdownDisposition::WaitFailed);
		PF_CHECK(Tests, FailedResult.Stage == ShutdownStage::DeviceIdle);
		PF_CHECK(Tests, FailedResult.VulkanResult == -7);
		PF_CHECK(Tests, std::find(FailedWait.Calls.begin(), FailedWait.Calls.end(), "release-nvrhi") == FailedWait.Calls.end());

		FakeRendererShutdown FailedNvrhiWait;
		FailedNvrhiWait.NvrhiWait = { ShutdownWaitOutcome::Failed, 0 };
		const ShutdownResult FailedNvrhiResult = ShutdownRenderer(FailedNvrhiWait);
		PF_CHECK(Tests, FailedNvrhiResult.Disposition == ShutdownDisposition::WaitFailed);
		PF_CHECK(Tests, FailedNvrhiResult.Stage == ShutdownStage::NvrhiIdle);
		PF_CHECK(Tests, !FailedNvrhiWait.DeviceLost);
		PF_CHECK(
			Tests, std::find(FailedNvrhiWait.Calls.begin(), FailedNvrhiWait.Calls.end(), "release-nvrhi") == FailedNvrhiWait.Calls.end());

		FakeRendererShutdown UnknownFenceRetirement;
		UnknownFenceRetirement.DeviceLost = true;
		UnknownFenceRetirement.LostResult = TestVkErrorDeviceLost;
		UnknownFenceRetirement.FenceWaitAfterLoss = { ShutdownWaitOutcome::DeviceLost, TestVkErrorDeviceLost };
		const ShutdownResult UnknownResult = ShutdownRenderer(UnknownFenceRetirement);
		PF_CHECK(Tests, UnknownResult.Disposition == ShutdownDisposition::PresentationLifetimeUnknown);
		PF_CHECK(Tests, UnknownResult.Stage == ShutdownStage::PresentationRetirementAfterDeviceLoss);
		PF_CHECK(Tests, UnknownResult.VulkanResult == TestVkErrorDeviceLost);
		PF_CHECK(Tests, std::find(UnknownFenceRetirement.Calls.begin(), UnknownFenceRetirement.Calls.end(), "release-nvrhi") ==
							UnknownFenceRetirement.Calls.end());

		FakeRendererShutdown LegacyLoss;
		LegacyLoss.DeviceLost = true;
		LegacyLoss.LostResult = TestVkErrorDeviceLost;
		LegacyLoss.PresentationFences = false;
		LegacyLoss.LegacyWaitAfterLoss = { ShutdownWaitOutcome::DeviceLost, TestVkErrorDeviceLost };
		const ShutdownResult LegacyResult = ShutdownRenderer(LegacyLoss);
		PF_CHECK(Tests, LegacyResult.Disposition == ShutdownDisposition::CompleteAfterDeviceLoss);
		PF_CHECK(Tests, std::find(LegacyLoss.Calls.begin(), LegacyLoss.Calls.end(), "legacy-best-effort") != LegacyLoss.Calls.end());
		PF_CHECK(
			Tests, std::find(LegacyLoss.Calls.begin(), LegacyLoss.Calls.end(), "release-children-legacy-loss") != LegacyLoss.Calls.end());

		FakeRendererShutdown LegacyWaitFailureAfterLoss;
		LegacyWaitFailureAfterLoss.DeviceLost = true;
		LegacyWaitFailureAfterLoss.LostResult = TestVkErrorDeviceLost;
		LegacyWaitFailureAfterLoss.PresentationFences = false;
		LegacyWaitFailureAfterLoss.LegacyWaitAfterLoss = { ShutdownWaitOutcome::Failed, -7 };
		const ShutdownResult LegacyWaitFailureResult = ShutdownRenderer(LegacyWaitFailureAfterLoss);
		PF_CHECK(Tests, LegacyWaitFailureResult.Disposition == ShutdownDisposition::CompleteAfterDeviceLoss);
		PF_CHECK(Tests, LegacyWaitFailureResult.VulkanResult == TestVkErrorDeviceLost);
		PF_CHECK(Tests, std::find(LegacyWaitFailureAfterLoss.Calls.begin(), LegacyWaitFailureAfterLoss.Calls.end(),
							"release-children-legacy-loss") != LegacyWaitFailureAfterLoss.Calls.end());

		FakeRendererShutdown PartialInitialization;
		PartialInitialization.HasLogicalDevice = false;
		const ShutdownResult PartialResult = ShutdownRenderer(PartialInitialization);
		PF_CHECK(Tests, PartialResult.Disposition == ShutdownDisposition::Complete);
		const std::vector<std::string> ExpectedPartialCalls = { "release-partial" };
		PF_CHECK(Tests, PartialInitialization.Calls == ExpectedPartialCalls);
	}

	void TestDeviceLossStopsRendererWork(TestRunner& Tests)
	{
		VulkanDeviceHealth Health;
		PF_CHECK(Tests, !Health.IsLost());
		Health.MarkLost(TestVkErrorDeviceLost);
		PF_CHECK(Tests, Health.IsLost());
		PF_CHECK(Tests, Health.VulkanResult() == TestVkErrorDeviceLost);

		bool Rejected = false;
		try
		{
			Health.EnsureOperational();
		}
		catch (const std::runtime_error& Exception)
		{
			Rejected = std::string_view(Exception.what()).find("VkResult -4") != std::string_view::npos;
		}
		PF_CHECK(Tests, Rejected);
	}

	void TestUnpresentedCandidateReleaseGuard(TestRunner& Tests)
	{
		VulkanPresentationResources Resources;
		VulkanDeviceHealth LostDevice;
		LostDevice.MarkLost(TestVkErrorDeviceLost);
		Resources.Initialize(vk::Device(), vk::Queue(), nullptr, PresentationRetirementMechanism::KhrMaintenanceFence, &LostDevice);
		std::vector<VulkanSwapchainImage> UnpresentedImages(1);
		Resources.ReleaseImages(UnpresentedImages, PresentationResourceReleaseMode::UnpresentedCandidate);
		PF_CHECK(Tests, UnpresentedImages.empty());

		std::vector<VulkanSwapchainImage> PendingImages(1);
		PendingImages[0].PresentationState = PresentationRequestState::Pending;
		bool RejectedPending = false;
		try
		{
			Resources.ReleaseImages(PendingImages, PresentationResourceReleaseMode::UnpresentedCandidate);
		}
		catch (const std::runtime_error&)
		{
			RejectedPending = true;
		}
		PF_CHECK(Tests, RejectedPending);
		PF_CHECK(Tests, PendingImages.size() == 1);
		PF_CHECK(Tests, PendingImages.front().PresentationState == PresentationRequestState::Pending);

		std::vector<VulkanRetiredSwapchainGeneration> PendingGenerations(1);
		PendingGenerations[0].Images.resize(1);
		PendingGenerations[0].Images[0].PresentationState = PresentationRequestState::DeviceLostIndeterminate;
		bool RejectedIndeterminate = false;
		try
		{
			Resources.ReleaseGenerations(PendingGenerations, PresentationResourceReleaseMode::UnpresentedCandidate);
		}
		catch (const std::runtime_error&)
		{
			RejectedIndeterminate = true;
		}
		PF_CHECK(Tests, RejectedIndeterminate);
		PF_CHECK(Tests, PendingGenerations.size() == 1);
		PF_CHECK(Tests, PendingGenerations.front().Images.size() == 1);
	}

	void TestWorldTransformOrientation(TestRunner& Tests)
	{
		using PulseForge::ClassifyWorldTransform;
		using PulseForge::WorldTransformOrientation;

		const glm::mat4 Identity(1.0f);
		const glm::mat4 PositiveScale = glm::scale(Identity, glm::vec3(2.0f, 0.5f, 3.0f));
		const glm::mat4 OneReflection = glm::scale(Identity, glm::vec3(-1.0f, 1.0f, 1.0f));
		const glm::mat4 TwoReflections = glm::scale(Identity, glm::vec3(-1.0f, -1.0f, 1.0f));
		PF_CHECK(Tests, ClassifyWorldTransform(Identity) == WorldTransformOrientation::Normal);
		PF_CHECK(Tests, ClassifyWorldTransform(PositiveScale) == WorldTransformOrientation::Normal);
		PF_CHECK(Tests, ClassifyWorldTransform(OneReflection) == WorldTransformOrientation::Mirrored);
		PF_CHECK(Tests, ClassifyWorldTransform(TwoReflections) == WorldTransformOrientation::Normal);
		PF_CHECK(Tests, PulseForge::UsesCounterClockwiseFrontFace(WorldTransformOrientation::Normal));
		PF_CHECK(Tests, !PulseForge::UsesCounterClockwiseFrontFace(WorldTransformOrientation::Mirrored));

		// Parent/child composition uses the same determinant parity as the resulting world matrix.
		const glm::mat4 ParentReflection = glm::scale(Identity, glm::vec3(1.0f, -1.0f, 1.0f));
		const glm::mat4 ChildRotation = glm::rotate(Identity, glm::radians(35.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		PF_CHECK(Tests, ClassifyWorldTransform(ParentReflection * ChildRotation) == WorldTransformOrientation::Mirrored);
		PF_CHECK(Tests, ClassifyWorldTransform(ParentReflection * OneReflection) == WorldTransformOrientation::Normal);

		const glm::mat4 Singular = glm::scale(Identity, glm::vec3(1.0f, 0.0f, 1.0f));
		PF_CHECK(Tests, ClassifyWorldTransform(Singular) == WorldTransformOrientation::Singular);
		PF_CHECK(Tests, !PulseForge::BuildNormalTransform(Singular).has_value());
	}
} // namespace

void RunRendererPolicyTests(TestRunner& Tests)
{
	TestQueueFamilySelection(Tests);
	TestRequiredDeviceFeatures(Tests);
	TestSwapchainRetirementPolicy(Tests);
	TestPresentationCapabilitySelection(Tests);
	TestPresentationPoliciesAndCandidatePreference(Tests);
	TestPresentationRequestDisposition(Tests);
	TestFormatUsageCapabilities(Tests);
	TestSurfaceAndPresentationSelection(Tests);
	TestExtentAndImageCountSelection(Tests);
	TestHealthyRendererShutdownOrder(Tests);
	TestDeviceLossShutdown(Tests);
	TestRendererShutdownFailureContracts(Tests);
	TestDeviceLossStopsRendererWork(Tests);
	TestUnpresentedCandidateReleaseGuard(Tests);
	TestWorldTransformOrientation(Tests);
}
