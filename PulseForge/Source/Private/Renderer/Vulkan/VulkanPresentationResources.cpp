#include "Renderer/Vulkan/VulkanPresentationResources.h"

#include "Core/Log.h"

#include <limits>
#include <stdexcept>

namespace PulseForge::VulkanDetail
{
	void VulkanPresentationResources::Initialize(
		vk::Device Device, vk::Queue PresentQueue, nvrhi::IDevice* NvrhiDevice, bool PresentFenceMaintenanceEnabled)
	{
		m_Device = Device;
		m_PresentQueue = PresentQueue;
		m_NvrhiDevice = NvrhiDevice;
		m_PresentFenceMaintenanceEnabled = PresentFenceMaintenanceEnabled;
		m_LoggedLegacyPresentRetirement = false;
	}

	void VulkanPresentationResources::Reset() noexcept
	{
		m_Device = vk::Device();
		m_PresentQueue = vk::Queue();
		m_NvrhiDevice = nullptr;
		m_PresentFenceMaintenanceEnabled = false;
		m_LoggedLegacyPresentRetirement = false;
	}

	void VulkanPresentationResources::WaitForPresentation(VulkanSwapchainImage& Image)
	{
		if (!m_PresentFenceMaintenanceEnabled)
		{
			// Reacquiring this image proves its previous present completed. The acquire
			// semaphore is consumed by graphics before its present semaphore is signaled again.
			Image.PresentPending = false;
			return;
		}
		if (!Image.PresentPending || !Image.PresentFence) return;
		const VkFence Fence = static_cast<VkFence>(Image.PresentFence);
		const VkResult Result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(
			static_cast<VkDevice>(m_Device), 1, &Fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		if (Result != VK_SUCCESS)
			throw std::runtime_error("Vulkan presentation-fence wait failed: " + vk::to_string(static_cast<vk::Result>(Result)));
		Image.PresentPending = false;
	}

	void VulkanPresentationResources::WaitForRetirement(std::vector<VulkanSwapchainImage>& Images, std::string_view Label)
	{
		if (Images.empty()) return;
		if (m_PresentFenceMaintenanceEnabled)
		{
			for (VulkanSwapchainImage& Image : Images)
				WaitForPresentation(Image);
			return;
		}

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
		if (!m_PresentFenceMaintenanceEnabled || !Image.PresentFence) return;
		WaitForPresentation(Image);
		if (!Image.PresentFenceUsed) return;
		const VkFence Fence = static_cast<VkFence>(Image.PresentFence);
		const VkResult Result = VULKAN_HPP_DEFAULT_DISPATCHER.vkResetFences(static_cast<VkDevice>(m_Device), 1, &Fence);
		if (Result != VK_SUCCESS)
			throw std::runtime_error(
				"Could not reset a completed Vulkan presentation fence: " + vk::to_string(static_cast<vk::Result>(Result)));
	}

	void VulkanPresentationResources::ReleaseImages(std::vector<VulkanSwapchainImage>& Images)
	{
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
		for (VulkanRetiredSwapchainGeneration& Generation : Generations)
		{
			ReleaseImages(Generation.Images);
			if (Generation.Swapchain) m_Device.destroySwapchainKHR(Generation.Swapchain);
		}
		Generations.clear();
	}
} // namespace PulseForge::VulkanDetail
