#include "Renderer/Vulkan/VulkanPresentationResources.h"

#include "Core/Log.h"

#include <limits>
#include <stdexcept>

namespace PulseForge::VulkanDetail
{
	void VulkanPresentationResources::Initialize(vk::Device Device, vk::Queue PresentQueue, nvrhi::IDevice* NvrhiDevice,
		VulkanSupport::PresentationRetirementMechanism RetirementMechanism, VulkanDeviceHealth* DeviceHealth)
	{
		m_Device = Device;
		m_PresentQueue = PresentQueue;
		m_NvrhiDevice = NvrhiDevice;
		m_DeviceHealth = DeviceHealth;
		m_RetirementMechanism = RetirementMechanism;
		m_LoggedLegacyPresentRetirement = false;
	}

	void VulkanPresentationResources::Reset() noexcept
	{
		m_Device = vk::Device();
		m_PresentQueue = vk::Queue();
		m_NvrhiDevice = nullptr;
		m_DeviceHealth = nullptr;
		m_RetirementMechanism = VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort;
		m_LoggedLegacyPresentRetirement = false;
	}

	bool VulkanPresentationResources::UsesPresentationFences() const noexcept
	{
		return m_RetirementMechanism != VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort;
	}

	void VulkanPresentationResources::ValidateImagesForRelease(const std::vector<VulkanSwapchainImage>& Images) const
	{
		for (const VulkanSwapchainImage& Image : Images)
		{
			if (Image.PresentationState == PresentationRequestState::Indeterminate ||
				Image.PresentationState == PresentationRequestState::DeviceLostIndeterminate)
				throw std::runtime_error("Cannot release a Vulkan swapchain image after an indeterminate presentation result");
			if (UsesPresentationFences() && Image.PresentationState == PresentationRequestState::Pending)
				throw std::runtime_error("Cannot release a Vulkan swapchain image before its presentation fence signals");
		}
	}

	void VulkanPresentationResources::ValidateImagesWereNeverPresented(const std::vector<VulkanSwapchainImage>& Images) const
	{
		for (const VulkanSwapchainImage& Image : Images)
			if (Image.PresentationState != PresentationRequestState::NotSubmitted)
				throw std::runtime_error("Unpublished Vulkan host contains an image with presentation work");
	}

	void VulkanPresentationResources::WaitForPresentation(VulkanSwapchainImage& Image)
	{
		if (m_DeviceHealth && m_DeviceHealth->IsLost())
			throw std::runtime_error("Cannot wait for normal Vulkan presentation while the shared device is lost");
		if (Image.PresentationState == PresentationRequestState::Indeterminate)
			throw std::runtime_error("Vulkan presentation completion is indeterminate; resources cannot be retired safely");
		if (Image.PresentationState == PresentationRequestState::DeviceLostIndeterminate && !UsesPresentationFences())
			throw std::runtime_error("Vulkan device loss left legacy presentation completion indeterminate");

		if (!UsesPresentationFences())
		{
			// Reacquiring this image proves its previous present completed. The acquire
			// semaphore is consumed by graphics before its present semaphore is signaled again.
			if (Image.PresentationState == PresentationRequestState::Pending) Image.PresentationState = PresentationRequestState::Complete;
			return;
		}
		if ((Image.PresentationState != PresentationRequestState::Pending &&
				Image.PresentationState != PresentationRequestState::DeviceLostIndeterminate) ||
			!Image.PresentFence)
			return;
		const VkFence Fence = static_cast<VkFence>(Image.PresentFence);
		const VkResult Result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(
			static_cast<VkDevice>(m_Device), 1, &Fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		if (Result != VK_SUCCESS)
		{
			if (Result == VK_ERROR_DEVICE_LOST && m_DeviceHealth) m_DeviceHealth->MarkLost(Result);
			throw std::runtime_error("Vulkan presentation-fence wait failed: " + vk::to_string(static_cast<vk::Result>(Result)));
		}
		Image.PresentationState = PresentationRequestState::Complete;
	}

	bool VulkanPresentationResources::WaitForPresentationAfterDeviceLoss(VulkanSwapchainImage& Image, int32_t& VulkanResult) noexcept
	{
		VulkanResult = VK_SUCCESS;
		if (Image.PresentationState == PresentationRequestState::NotSubmitted ||
			Image.PresentationState == PresentationRequestState::Complete)
			return true;
		if (!UsesPresentationFences() || !Image.PresentFence)
		{
			VulkanResult = VK_ERROR_UNKNOWN;
			return false;
		}

		const VkFence Fence = static_cast<VkFence>(Image.PresentFence);
		const VkResult Result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(
			static_cast<VkDevice>(m_Device), 1, &Fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		if (Result == VK_SUCCESS)
		{
			Image.PresentationState = PresentationRequestState::Complete;
			return true;
		}
		if (Result == VK_ERROR_DEVICE_LOST && m_DeviceHealth) m_DeviceHealth->MarkLost(Result);
		VulkanResult = Result;
		return false;
	}

	void VulkanPresentationResources::WaitForRetirement(std::vector<VulkanSwapchainImage>& Images, std::string_view Label)
	{
		if (m_DeviceHealth && m_DeviceHealth->IsLost())
			throw std::runtime_error("Cannot perform normal Vulkan presentation retirement after shared device loss");
		if (Images.empty()) return;
		if (UsesPresentationFences())
		{
			for (VulkanSwapchainImage& Image : Images)
				WaitForPresentation(Image);
			return;
		}
		ValidateImagesForRelease(Images);

		if (!m_LoggedLegacyPresentRetirement)
		{
			PF_CORE_WARN("Retiring Vulkan {} without swapchain-maintenance fences. The present queue-idle fallback does not provide an "
						 "explicit presentation-engine completion guarantee",
				Label);
			m_LoggedLegacyPresentRetirement = true;
		}
		const VkResult Result = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle(static_cast<VkQueue>(m_PresentQueue));
		if (Result == VK_ERROR_DEVICE_LOST && m_DeviceHealth) m_DeviceHealth->MarkLost(Result);
		if (Result != VK_SUCCESS)
			throw std::runtime_error("Vulkan present-queue idle wait failed: " + vk::to_string(static_cast<vk::Result>(Result)));
	}

	void VulkanPresentationResources::PrepareFenceForReuse(VulkanSwapchainImage& Image)
	{
		if (m_DeviceHealth && m_DeviceHealth->IsLost())
			throw std::runtime_error("Cannot reuse a Vulkan presentation fence after shared device loss");
		if (!UsesPresentationFences() || !Image.PresentFence) return;
		WaitForPresentation(Image);
		if (Image.PresentationState != PresentationRequestState::Complete) return;
		const VkFence Fence = static_cast<VkFence>(Image.PresentFence);
		const VkResult Result = VULKAN_HPP_DEFAULT_DISPATCHER.vkResetFences(static_cast<VkDevice>(m_Device), 1, &Fence);
		if (Result != VK_SUCCESS)
		{
			if (Result == VK_ERROR_DEVICE_LOST && m_DeviceHealth) m_DeviceHealth->MarkLost(Result);
			throw std::runtime_error(
				"Could not reset a completed Vulkan presentation fence: " + vk::to_string(static_cast<vk::Result>(Result)));
		}
		Image.PresentationState = PresentationRequestState::NotSubmitted;
	}

	void VulkanPresentationResources::ReleaseImages(std::vector<VulkanSwapchainImage>& Images, PresentationResourceReleaseMode Mode)
	{
		if (Mode == PresentationResourceReleaseMode::UnpresentedCandidate) ValidateImagesWereNeverPresented(Images);
		else if (Mode == PresentationResourceReleaseMode::LegacyBestEffortAfterDeviceLoss)
		{
			if (UsesPresentationFences() || !m_DeviceHealth || !m_DeviceHealth->IsLost())
				throw std::runtime_error(
					"Legacy best-effort presentation release requires a confirmed lost device without presentation fences");
		}
		else if (Mode == PresentationResourceReleaseMode::DeviceLostWithFencesRetired)
		{
			if (!UsesPresentationFences() || !m_DeviceHealth || !m_DeviceHealth->IsLost())
				throw std::runtime_error("Fence-backed lost-device release requires a confirmed lost device and maintenance fences");
			ValidateImagesForRelease(Images);
		}
		else
		{
			if (m_DeviceHealth && m_DeviceHealth->IsLost())
				throw std::runtime_error("Normal Vulkan presentation release is unavailable after shared device loss");
			ValidateImagesForRelease(Images);
		}

		for (VulkanSwapchainImage& Image : Images)
		{
			if (Image.PresentFence) m_Device.destroyFence(Image.PresentFence);
			Image.PresentFence = vk::Fence();
			if (Image.PresentReady) m_Device.destroySemaphore(Image.PresentReady);
			Image.PresentReady = vk::Semaphore();
		}
		Images.clear();
		if (Mode == PresentationResourceReleaseMode::Normal && m_NvrhiDevice && (!m_DeviceHealth || !m_DeviceHealth->IsLost()))
		{
			try
			{
				m_NvrhiDevice->runGarbageCollection();
			}
			catch (const vk::DeviceLostError&)
			{
				if (m_DeviceHealth) m_DeviceHealth->MarkLost(VK_ERROR_DEVICE_LOST);
				throw;
			}
		}
	}

	void VulkanPresentationResources::ReleaseGenerations(
		std::vector<VulkanRetiredSwapchainGeneration>& Generations, PresentationResourceReleaseMode Mode)
	{
		if (Mode == PresentationResourceReleaseMode::UnpresentedCandidate)
		{
			for (const VulkanRetiredSwapchainGeneration& Generation : Generations)
				ValidateImagesWereNeverPresented(Generation.Images);
		}
		else if (Mode == PresentationResourceReleaseMode::LegacyBestEffortAfterDeviceLoss)
		{
			if (UsesPresentationFences() || !m_DeviceHealth || !m_DeviceHealth->IsLost())
				throw std::runtime_error(
					"Legacy best-effort generation release requires a confirmed lost device without presentation fences");
		}
		else if (Mode == PresentationResourceReleaseMode::DeviceLostWithFencesRetired)
		{
			if (!UsesPresentationFences() || !m_DeviceHealth || !m_DeviceHealth->IsLost())
				throw std::runtime_error("Fence-backed generation release requires a confirmed lost device and maintenance fences");
			for (const VulkanRetiredSwapchainGeneration& Generation : Generations)
				ValidateImagesForRelease(Generation.Images);
		}
		else
		{
			if (m_DeviceHealth && m_DeviceHealth->IsLost())
				throw std::runtime_error("Normal Vulkan generation release is unavailable after shared device loss");
			for (const VulkanRetiredSwapchainGeneration& Generation : Generations)
				ValidateImagesForRelease(Generation.Images);
		}
		for (VulkanRetiredSwapchainGeneration& Generation : Generations)
		{
			ReleaseImages(Generation.Images, Mode);
			if (Generation.Swapchain) m_Device.destroySwapchainKHR(Generation.Swapchain);
		}
		Generations.clear();
	}
} // namespace PulseForge::VulkanDetail
