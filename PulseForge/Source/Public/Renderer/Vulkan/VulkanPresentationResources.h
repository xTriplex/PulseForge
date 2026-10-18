#pragma once

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "Renderer/RendererBackend.h"
#include "Renderer/Vulkan/VulkanSupport.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace PulseForge::VulkanDetail
{
	enum class PresentationRequestState : uint8_t
	{
		NotSubmitted,
		Pending,
		Complete,
		DeviceLostIndeterminate,
		Indeterminate
	};

	struct VulkanSwapchainImage
	{
		vk::Image NativeImage;
		nvrhi::TextureHandle Texture;
		TextureHandle DepthTarget;
		nvrhi::FramebufferHandle Framebuffer;
		vk::Semaphore PresentReady;
		vk::Fence PresentFence;
		PresentationRequestState PresentationState = PresentationRequestState::NotSubmitted;
	};

	struct VulkanRetiredSwapchainGeneration
	{
		vk::SwapchainKHR Swapchain;
		std::vector<VulkanSwapchainImage> Images;
	};

	// The renderer owns the device and controls when graphics and presentation work
	// has completed. This owner releases image-associated resources only at those
	// explicit completion points, before the borrowed device is destroyed.
	class VulkanPresentationResources final
	{
	public:
		void Initialize(vk::Device Device, vk::Queue PresentQueue, nvrhi::IDevice* NvrhiDevice,
			VulkanSupport::PresentationRetirementMechanism RetirementMechanism);
		void Reset() noexcept;

		void WaitForPresentation(VulkanSwapchainImage& Image);
		void WaitForRetirement(std::vector<VulkanSwapchainImage>& Images, std::string_view Label);
		void PrepareFenceForReuse(VulkanSwapchainImage& Image);
		void ReleaseImages(std::vector<VulkanSwapchainImage>& Images);
		void ReleaseGenerations(std::vector<VulkanRetiredSwapchainGeneration>& Generations);

	private:
		[[nodiscard]] bool UsesPresentationFences() const noexcept;
		void ValidateImagesForRelease(const std::vector<VulkanSwapchainImage>& Images) const;

		vk::Device m_Device;
		vk::Queue m_PresentQueue;
		nvrhi::IDevice* m_NvrhiDevice = nullptr;
		VulkanSupport::PresentationRetirementMechanism m_RetirementMechanism =
			VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort;
		bool m_LoggedLegacyPresentRetirement = false;
	};
} // namespace PulseForge::VulkanDetail
