#include "TestSupport.h"

#include "Renderer/Vulkan/VulkanSupport.h"

#include <iterator>
#include <vector>

namespace
{
	using namespace PulseForge::VulkanSupport;

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
}
