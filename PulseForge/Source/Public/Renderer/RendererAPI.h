#pragma once

namespace PulseForge
{
	// Describes whether the active presentation attachment converts linear shader output to sRGB.
	enum class OutputColorEncoding
	{
		SrgbAttachment,
		UnormAttachment
	};

	enum class RendererAPI
	{
		OpenGL,
		Vulkan
	};
}
