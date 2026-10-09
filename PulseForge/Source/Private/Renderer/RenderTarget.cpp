#include "Core/PulseForgePCH.h"
#include "Renderer/RenderTarget.h"

#include <cmath>

namespace PulseForge
{
	namespace
	{
		std::unexpected<RenderTargetError> MakeError(RenderTargetErrorCode Code, const char* Message)
		{
			return std::unexpected(RenderTargetError{ Code, Message });
		}
	}

	std::expected<void, RenderTargetError> ValidateRenderTargetDescription(const RenderTargetDesc& Description)
	{
		if (Description.Width == 0 || Description.Height == 0)
			return MakeError(RenderTargetErrorCode::InvalidDescription, "Render-target dimensions must be non-zero");
		if (Description.ColorFormat == ColorTargetFormat::None && Description.DepthMode == DepthAttachmentMode::None)
			return MakeError(RenderTargetErrorCode::InvalidDescription, "Render targets require at least one attachment");

		if (Description.ColorFormat != ColorTargetFormat::None && Description.ColorFormat != ColorTargetFormat::RGBA8_UNorm &&
			Description.ColorFormat != ColorTargetFormat::RGBA8_Srgb &&
			Description.ColorFormat != ColorTargetFormat::RGBA16_Float)
		{
			return MakeError(
				RenderTargetErrorCode::InvalidDescription,
				"Offscreen render targets require None, RGBA8_UNorm, RGBA8_Srgb, or RGBA16_Float color format");
		}
		if (Description.DepthMode != DepthAttachmentMode::None && Description.DepthMode != DepthAttachmentMode::Attachment &&
			Description.DepthMode != DepthAttachmentMode::ShaderReadableAttachment)
			return MakeError(RenderTargetErrorCode::InvalidDescription, "Render target has an unsupported depth attachment mode");

		return {};
	}

	bool MatchesRenderTargetConfiguration(
		const RenderTargetDesc& Description,
		uint32_t Width,
		uint32_t Height,
		ColorTargetFormat ColorFormat,
		DepthAttachmentMode DepthMode) noexcept
	{
		return Width != 0 && Height != 0 &&
			Description.Width == Width && Description.Height == Height &&
			Description.ColorFormat == ColorFormat && Description.DepthMode == DepthMode;
	}

	std::expected<void, RenderTargetError> ValidateRenderTargetClearValue(const RenderTargetClearValue& ClearValue)
	{
		for (const float Component : ClearValue.Color)
		{
			if (!std::isfinite(Component))
				return MakeError(RenderTargetErrorCode::InvalidDescription, "Render-target clear color must contain only finite values");
		}

		if (!std::isfinite(ClearValue.Depth) || ClearValue.Depth < 0.0f || ClearValue.Depth > 1.0f)
			return MakeError(RenderTargetErrorCode::InvalidDescription, "Render-target depth clear must be between 0 and 1");

		return {};
	}
}
