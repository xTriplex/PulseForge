#include "Renderer/Vulkan/VulkanRendererShutdown.h"

namespace PulseForge::VulkanDetail
{
	namespace
	{
		ShutdownResult Failure(ShutdownDisposition Disposition, ShutdownStage Stage, ShutdownWaitResult Wait)
		{
			return { Disposition, Stage, Wait.Outcome, Wait.VulkanResult };
		}

		ShutdownResult CleanupFailure(ShutdownStage Stage)
		{
			return { ShutdownDisposition::CleanupFailed, Stage, ShutdownWaitOutcome::Failed, 0 };
		}
	} // namespace

	ShutdownResult ShutdownRenderer(RendererShutdownOperations& Operations) noexcept
	{
		if (Operations.IsShutdownComplete()) return {};

		if (!Operations.HasDevice())
		{
			if (!Operations.ReleasePartialInitialization()) return CleanupFailure(ShutdownStage::InstanceRelease);
			return {};
		}

		bool DeviceLost = Operations.IsDeviceLost();
		ShutdownStage DeviceLossStage = DeviceLost ? ShutdownStage::DeviceAlreadyLost : ShutdownStage::None;
		ShutdownWaitResult DeviceLossWait{ ShutdownWaitOutcome::DeviceLost, Operations.DeviceLossResult() };

		if (!DeviceLost)
		{
			const ShutdownWaitResult NvrhiWait = Operations.WaitForNvrhiIdle();
			if (NvrhiWait.Outcome == ShutdownWaitOutcome::Failed)
				return Failure(ShutdownDisposition::WaitFailed, ShutdownStage::NvrhiIdle, NvrhiWait);
			if (NvrhiWait.Outcome == ShutdownWaitOutcome::DeviceLost || Operations.IsDeviceLost())
			{
				DeviceLost = true;
				DeviceLossStage = ShutdownStage::NvrhiIdle;
				DeviceLossWait = NvrhiWait.Outcome == ShutdownWaitOutcome::DeviceLost
									 ? NvrhiWait
									 : ShutdownWaitResult{ ShutdownWaitOutcome::DeviceLost, Operations.DeviceLossResult() };
			}
		}

		if (!DeviceLost)
		{
			const ShutdownWaitResult DeviceWait = Operations.WaitForDeviceIdle();
			if (DeviceWait.Outcome == ShutdownWaitOutcome::Failed)
				return Failure(ShutdownDisposition::WaitFailed, ShutdownStage::DeviceIdle, DeviceWait);
			if (DeviceWait.Outcome == ShutdownWaitOutcome::DeviceLost || Operations.IsDeviceLost())
			{
				DeviceLost = true;
				DeviceLossStage = ShutdownStage::DeviceIdle;
				DeviceLossWait = DeviceWait.Outcome == ShutdownWaitOutcome::DeviceLost
									 ? DeviceWait
									 : ShutdownWaitResult{ ShutdownWaitOutcome::DeviceLost, Operations.DeviceLossResult() };
			}
		}

		if (!DeviceLost)
		{
			const ShutdownWaitResult PresentationWait = Operations.WaitForPresentationRetirement();
			if (PresentationWait.Outcome == ShutdownWaitOutcome::Failed)
				return Failure(ShutdownDisposition::WaitFailed, ShutdownStage::PresentationRetirement, PresentationWait);
			if (PresentationWait.Outcome == ShutdownWaitOutcome::DeviceLost || Operations.IsDeviceLost())
			{
				DeviceLost = true;
				DeviceLossStage = ShutdownStage::PresentationRetirement;
				DeviceLossWait = PresentationWait.Outcome == ShutdownWaitOutcome::DeviceLost
									 ? PresentationWait
									 : ShutdownWaitResult{ ShutdownWaitOutcome::DeviceLost, Operations.DeviceLossResult() };
			}
		}
		if (!DeviceLost && Operations.IsDeviceLost())
		{
			DeviceLost = true;
			DeviceLossStage = ShutdownStage::DeviceAlreadyLost;
			DeviceLossWait = { ShutdownWaitOutcome::DeviceLost, Operations.DeviceLossResult() };
		}

		ShutdownPresentationReleaseMode ReleaseMode = ShutdownPresentationReleaseMode::HealthyDevice;
		if (DeviceLost)
		{
			if (Operations.UsesPresentationFences())
			{
				const ShutdownWaitResult FenceWait = Operations.WaitForPresentationFencesAfterDeviceLoss();
				if (FenceWait.Outcome != ShutdownWaitOutcome::Complete)
					return Failure(
						ShutdownDisposition::PresentationLifetimeUnknown, ShutdownStage::PresentationRetirementAfterDeviceLoss, FenceWait);
				ReleaseMode = ShutdownPresentationReleaseMode::DeviceLostWithFencesRetired;
			}
			else
			{
				if (DeviceLossStage != ShutdownStage::PresentationRetirement)
				{
					const ShutdownWaitResult LegacyWait = Operations.BestEffortWaitForLegacyPresentationAfterDeviceLoss();
					// This wait is explicitly best-effort on the legacy path. It cannot establish
					// presentation-engine handoff, and a failure must not be mistaken for a
					// healthy-device completion failure after device loss is already confirmed.
					if (LegacyWait.Outcome == ShutdownWaitOutcome::DeviceLost && DeviceLossWait.VulkanResult == 0)
						DeviceLossWait = LegacyWait;
				}
				ReleaseMode = ShutdownPresentationReleaseMode::LegacyBestEffortAfterDeviceLoss;
			}
		}

		if (!Operations.ReleaseDeviceOwnedChildren(ReleaseMode)) return CleanupFailure(ShutdownStage::ChildRelease);
		if (!Operations.ReleaseNvrhiDevice()) return CleanupFailure(ShutdownStage::NvrhiRelease);
		Operations.DestroyVulkanDevice();
		if (!Operations.ReleaseInstanceOwnedChildren()) return CleanupFailure(ShutdownStage::InstanceRelease);

		if (DeviceLost)
			return { ShutdownDisposition::CompleteAfterDeviceLoss, DeviceLossStage, DeviceLossWait.Outcome, DeviceLossWait.VulkanResult };
		return {};
	}
} // namespace PulseForge::VulkanDetail
