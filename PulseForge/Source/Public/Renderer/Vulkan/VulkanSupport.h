#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace PulseForge::VulkanSupport
{
	struct RequiredDeviceFeatures
	{
		bool TimelineSemaphore = false;
		bool Synchronization2 = false;
		bool DynamicRendering = false;
	};

	bool SupportsRequiredDeviceFeatures(const RequiredDeviceFeatures& Features);

	struct QueueFamily
	{
		uint32_t Index = 0;
		uint32_t QueueCount = 0;
		bool SupportsGraphics = false;
		bool SupportsPresent = false;
	};

	struct QueueFamilySelection
	{
		uint32_t GraphicsIndex = 0;
		uint32_t PresentIndex = 0;
	};

	std::optional<QueueFamilySelection> ChooseQueueFamilies(std::span<const QueueFamily> Families);

	struct SurfaceFormat
	{
		uint32_t Format = 0;
		uint32_t ColorSpace = 0;
	};

	std::optional<SurfaceFormat> ChooseSurfaceFormat(
		std::span<const SurfaceFormat> Formats,
		uint32_t PreferredFormat,
		uint32_t PreferredColorSpace,
		uint32_t UndefinedFormat);

	enum class PresentMode
	{
		Immediate,
		Mailbox,
		Fifo,
		Other
	};

	std::optional<PresentMode> ChoosePresentMode(std::span<const PresentMode> Modes, bool VSync);

	struct Extent2D
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	Extent2D ChooseExtent(
		Extent2D Requested,
		Extent2D Current,
		bool HasFixedExtent,
		Extent2D Minimum,
		Extent2D Maximum);

	uint32_t ChooseImageCount(uint32_t Minimum, uint32_t Maximum);
}
