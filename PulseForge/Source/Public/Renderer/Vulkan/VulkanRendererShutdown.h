#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace PulseForge::VulkanDetail
{
	class VulkanDeviceHealth final
	{
	public:
		void MarkLost(int32_t VulkanResult = 0) noexcept
		{
			if (VulkanResult != 0)
			{
				int32_t NoResult = 0;
				m_VulkanResult.compare_exchange_strong(NoResult, VulkanResult, std::memory_order_relaxed);
			}
			m_Lost.store(true, std::memory_order_release);
		}

		[[nodiscard]] bool IsLost() const noexcept { return m_Lost.load(std::memory_order_acquire); }

		[[nodiscard]] int32_t VulkanResult() const noexcept { return m_VulkanResult.load(std::memory_order_relaxed); }

		void EnsureOperational() const
		{
			if (!IsLost()) return;
			const int32_t Result = VulkanResult();
			if (Result != 0)
				throw std::runtime_error("The Vulkan device has been reported lost (VkResult " + std::to_string(Result) +
										 "); no more rendering or submission is allowed");
			throw std::runtime_error("The Vulkan device has been reported lost by NVRHI; no more rendering or submission is allowed");
		}

	private:
		std::atomic_bool m_Lost = false;
		std::atomic<int32_t> m_VulkanResult = 0;
	};

	enum class ShutdownWaitOutcome : uint8_t
	{
		Complete,
		DeviceLost,
		Failed,
	};

	struct ShutdownWaitResult
	{
		ShutdownWaitOutcome Outcome = ShutdownWaitOutcome::Complete;
		// Zero when the source API does not provide a Vulkan result, such as NVRHI's
		// waitForIdle() boolean. Otherwise this preserves the original VkResult value.
		int32_t VulkanResult = 0;
	};

	enum class ShutdownStage : uint8_t
	{
		None,
		DeviceAlreadyLost,
		NvrhiIdle,
		DeviceIdle,
		PresentationRetirement,
		PresentationRetirementAfterDeviceLoss,
		ChildRelease,
		NvrhiRelease,
		DeviceRelease,
		InstanceRelease
	};

	enum class ShutdownDisposition : uint8_t
	{
		Complete,
		CompleteAfterDeviceLoss,
		WaitFailed,
		PresentationLifetimeUnknown,
		CleanupFailed
	};

	enum class ShutdownPresentationReleaseMode : uint8_t
	{
		HealthyDevice,
		DeviceLostWithFencesRetired,
		LegacyBestEffortAfterDeviceLoss
	};

	struct ShutdownResult
	{
		ShutdownDisposition Disposition = ShutdownDisposition::Complete;
		ShutdownStage Stage = ShutdownStage::None;
		ShutdownWaitOutcome WaitOutcome = ShutdownWaitOutcome::Complete;
		int32_t VulkanResult = 0;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return Disposition == ShutdownDisposition::Complete || Disposition == ShutdownDisposition::CompleteAfterDeviceLoss;
		}
	};

	// A narrow call boundary for the renderer's one-shot teardown sequence. Implementations
	// preserve Vulkan result classification; tests provide deterministic wait and release
	// outcomes without creating a device or a presentation engine.
	class RendererShutdownOperations
	{
	public:
		virtual ~RendererShutdownOperations() = default;

		[[nodiscard]] virtual bool IsShutdownComplete() const noexcept = 0;
		[[nodiscard]] virtual bool HasDevice() const noexcept = 0;
		[[nodiscard]] virtual bool IsDeviceLost() const noexcept = 0;
		[[nodiscard]] virtual int32_t DeviceLossResult() const noexcept = 0;
		[[nodiscard]] virtual bool UsesPresentationFences() const noexcept = 0;

		[[nodiscard]] virtual ShutdownWaitResult WaitForNvrhiIdle() noexcept = 0;
		[[nodiscard]] virtual ShutdownWaitResult WaitForDeviceIdle() noexcept = 0;
		[[nodiscard]] virtual ShutdownWaitResult WaitForPresentationRetirement() noexcept = 0;
		[[nodiscard]] virtual ShutdownWaitResult WaitForPresentationFencesAfterDeviceLoss() noexcept = 0;
		[[nodiscard]] virtual ShutdownWaitResult BestEffortWaitForLegacyPresentationAfterDeviceLoss() noexcept = 0;

		[[nodiscard]] virtual bool ReleasePartialInitialization() noexcept = 0;
		[[nodiscard]] virtual bool ReleaseDeviceOwnedChildren(ShutdownPresentationReleaseMode Mode) noexcept = 0;
		[[nodiscard]] virtual bool ReleaseNvrhiDevice() noexcept = 0;
		virtual void DestroyVulkanDevice() noexcept = 0;
		[[nodiscard]] virtual bool ReleaseInstanceOwnedChildren() noexcept = 0;
	};

	[[nodiscard]] ShutdownResult ShutdownRenderer(RendererShutdownOperations& Operations) noexcept;
} // namespace PulseForge::VulkanDetail
