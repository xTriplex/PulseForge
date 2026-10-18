#include "Renderer/Vulkan/VulkanPresentationResources.h"

#include "Core/Log.h"

#include <limits>
#include <stdexcept>

namespace PulseForge::VulkanDetail
{
	void VulkanPresentationResources::Initialize(vk::Device Device, vk::Queue PresentQueue, nvrhi::IDevice* NvrhiDevice,
		VulkanSupport::PresentationRetirementMechanism RetirementMechanism)
	{
		m_Device = Device;
		m_PresentQueue = PresentQueue;
		m_NvrhiDevice = NvrhiDevice;
		m_RetirementMechanism = RetirementMechanism;
		m_LoggedLegacyPresentRetirement = false;
	}

	void VulkanPresentationResources::Reset() noexcept
	{
		m_Device = vk::Device();
		m_PresentQueue = vk::Queue();
		m_NvrhiDevice = nullptr;
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

	void VulkanPresentationResources::WaitForPresentation(VulkanSwapchainImage& Image)
	{
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
			throw std::runtime_error("Vulkan presentation-fence wait failed: " + vk::to_string(static_cast<vk::Result>(Result)));
		Image.PresentationState = PresentationRequestState::Complete;
	}

	void VulkanPresentationResources::WaitForRetirement(std::vector<VulkanSwapchainImage>& Images, std::string_view Label)
	{
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
		m_PresentQueue.waitIdle();
	}

	void VulkanPresentationResources::PrepareFenceForReuse(VulkanSwapchainImage& Image)
	{
		if (!UsesPresentationFences() || !Image.PresentFence) return;
		WaitForPresentation(Image);
		if (Image.PresentationState != PresentationRequestState::Complete) return;
		const VkFence Fence = static_cast<VkFence>(Image.PresentFence);
		const VkResult Result = VULKAN_HPP_DEFAULT_DISPATCHER.vkResetFences(static_cast<VkDevice>(m_Device), 1, &Fence);
		if (Result != VK_SUCCESS)
			throw std::runtime_error(
				"Could not reset a completed Vulkan presentation fence: " + vk::to_string(static_cast<vk::Result>(Result)));
		Image.PresentationState = PresentationRequestState::NotSubmitted;
	}

	void VulkanPresentationResources::ReleaseImages(std::vector<VulkanSwapchainImage>& Images)
	{
		ValidateImagesForRelease(Images);
		for (VulkanSwapchainImage& Image : Images)
		{
			if (Image.PresentFence) m_Device.destroyFence(Image.PresentFence);
			Image.PresentFence = vk::Fence();
			if (Image.PresentReady) m_Device.destroySemaphore(Image.PresentReady);
			Image.PresentReady = vk::Semaphore();
		}
		Images.clear();
		if (m_NvrhiDevice) m_NvrhiDevice->runGarbageCollection();
	}

	void VulkanPresentationResources::ReleaseGenerations(std::vector<VulkanRetiredSwapchainGeneration>& Generations)
	{
		for (const VulkanRetiredSwapchainGeneration& Generation : Generations)
			ValidateImagesForRelease(Generation.Images);
		for (VulkanRetiredSwapchainGeneration& Generation : Generations)
		{
			ReleaseImages(Generation.Images);
			if (Generation.Swapchain) m_Device.destroySwapchainKHR(Generation.Swapchain);
		}
		Generations.clear();
	}
} // namespace PulseForge::VulkanDetail
