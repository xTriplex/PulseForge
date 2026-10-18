#pragma once

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "Renderer/RendererBackend.h"
#include "Renderer/Vulkan/VulkanRendererShutdown.h"
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

	enum class PresentationResourceReleaseMode : uint8_t
	{
		Normal,
		ShutdownAfterIdle,
		UnpresentedCandidate,
		DeviceLostWithFencesRetired,
		LegacyBestEffortAfterDeviceLoss
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
			VulkanSupport::PresentationRetirementMechanism RetirementMechanism, VulkanDeviceHealth* DeviceHealth);
		void Reset() noexcept;

		void WaitForPresentation(VulkanSwapchainImage& Image);
		[[nodiscard]] bool WaitForPresentationAfterDeviceLoss(VulkanSwapchainImage& Image, int32_t& VulkanResult) noexcept;
		void WaitForRetirement(std::vector<VulkanSwapchainImage>& Images, std::string_view Label);
		void PrepareFenceForReuse(VulkanSwapchainImage& Image);
		void ReleaseImages(
			std::vector<VulkanSwapchainImage>& Images, PresentationResourceReleaseMode Mode = PresentationResourceReleaseMode::Normal);
		void ReleaseGenerations(std::vector<VulkanRetiredSwapchainGeneration>& Generations,
			PresentationResourceReleaseMode Mode = PresentationResourceReleaseMode::Normal);

	private:
		[[nodiscard]] bool UsesPresentationFences() const noexcept;
		void ValidateImagesForRelease(const std::vector<VulkanSwapchainImage>& Images) const;
		// Valid only for swapchain candidates not yet published to frame acquisition.
		void ValidateImagesWereNeverPresented(const std::vector<VulkanSwapchainImage>& Images) const;

		vk::Device m_Device;
		vk::Queue m_PresentQueue;
		nvrhi::IDevice* m_NvrhiDevice = nullptr;
		VulkanDeviceHealth* m_DeviceHealth = nullptr;
		VulkanSupport::PresentationRetirementMechanism m_RetirementMechanism =
			VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort;
		bool m_LoggedLegacyPresentRetirement = false;
	};
} // namespace PulseForge::VulkanDetail
