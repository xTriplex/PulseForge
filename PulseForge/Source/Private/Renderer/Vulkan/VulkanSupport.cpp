#include "Core/PulseForgePCH.h"
#include "Renderer/Vulkan/VulkanSupport.h"

#include <limits>

namespace PulseForge::VulkanSupport
{
	bool SupportsRequiredDeviceFeatures(const RequiredDeviceFeatures& Features)
	{
		return Features.TimelineSemaphore && Features.Synchronization2 && Features.DynamicRendering;
	}

	bool SupportsFormatUsage(
		const FormatFeatureSupport& Features,
		bool RequiresSampledImage,
		bool RequiresColorAttachment,
		bool RequiresLinearFiltering) noexcept
	{
		return (!RequiresSampledImage || Features.SampledImage) &&
			(!RequiresColorAttachment || Features.ColorAttachment) &&
			(!RequiresLinearFiltering || (Features.SampledImage && Features.LinearFiltering));
	}

	std::optional<QueueFamilySelection> ChooseQueueFamilies(std::span<const QueueFamily> Families)
	{
		std::optional<uint32_t> GraphicsIndex;
		std::optional<uint32_t> PresentIndex;

		for (const QueueFamily& Family : Families)
		{
			if (Family.QueueCount == 0)
				continue;

			if (Family.SupportsGraphics && Family.SupportsPresent)
				return QueueFamilySelection{ Family.Index, Family.Index };

			if (Family.SupportsGraphics && !GraphicsIndex)
				GraphicsIndex = Family.Index;

			if (Family.SupportsPresent && !PresentIndex)
				PresentIndex = Family.Index;
		}

		if (!GraphicsIndex || !PresentIndex)
			return std::nullopt;

		return QueueFamilySelection{ *GraphicsIndex, *PresentIndex };
	}

	std::optional<SurfaceFormat> ChooseSurfaceFormat(
		std::span<const SurfaceFormat> Formats,
		uint32_t PreferredFormat,
		uint32_t PreferredColorSpace,
		uint32_t UndefinedFormat)
	{
		if (Formats.empty())
			return std::nullopt;

		if (Formats.size() == 1 && Formats.front().Format == UndefinedFormat)
			return SurfaceFormat{ PreferredFormat, Formats.front().ColorSpace };

		for (const SurfaceFormat& Format : Formats)
		{
			if (Format.Format == PreferredFormat && Format.ColorSpace == PreferredColorSpace)
				return Format;
		}

		return Formats.front();
	}

	std::optional<PresentMode> ChoosePresentMode(std::span<const PresentMode> Modes, bool VSync)
	{
		if (Modes.empty())
			return std::nullopt;

		const auto HasMode = [Modes](PresentMode Mode)
		{
			return std::find(Modes.begin(), Modes.end(), Mode) != Modes.end();
		};

		if (VSync)
		{
			if (HasMode(PresentMode::Fifo))
				return PresentMode::Fifo;
		}
		else
		{
			if (HasMode(PresentMode::Mailbox))
				return PresentMode::Mailbox;
			if (HasMode(PresentMode::Immediate))
				return PresentMode::Immediate;
			if (HasMode(PresentMode::Fifo))
				return PresentMode::Fifo;
		}

		return Modes.front();
	}

	Extent2D ChooseExtent(
		Extent2D Requested,
		Extent2D Current,
		bool HasFixedExtent,
		Extent2D Minimum,
		Extent2D Maximum)
	{
		if (HasFixedExtent)
			return Current;

		if (Requested.Width == 0 || Requested.Height == 0)
			return {};

		return {
			std::clamp(Requested.Width, Minimum.Width, Maximum.Width),
			std::clamp(Requested.Height, Minimum.Height, Maximum.Height)
		};
	}

	uint32_t ChooseImageCount(uint32_t Minimum, uint32_t Maximum)
	{
		if (Minimum == std::numeric_limits<uint32_t>::max())
			return Minimum;

		const uint32_t Preferred = Minimum + 1;
		return Maximum > 0 ? std::min(Preferred, Maximum) : Preferred;
	}
}
