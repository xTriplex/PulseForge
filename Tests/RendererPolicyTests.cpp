#include "TestSupport.h"

#include "Renderer/Vulkan/VulkanSupport.h"

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
		PF_CHECK(Tests,
			!CanCreateSwapchainGeneration(false, false, MaximumDeferredSwapchainGenerations, false, true));
		PF_CHECK(Tests,
			!CanCreateSwapchainGeneration(false, true, MaximumDeferredSwapchainGenerations, true, true));
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
	TestFormatUsageCapabilities(Tests);
	TestSurfaceAndPresentationSelection(Tests);
	TestExtentAndImageCountSelection(Tests);
}
