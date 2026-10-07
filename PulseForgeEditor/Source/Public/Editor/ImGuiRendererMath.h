#pragma once

#include "Renderer/Graphics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>

#include <glm/glm.hpp>

namespace PulseForgeEditor::ImGuiRendererMath
{
	struct FramebufferExtent
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	[[nodiscard]] inline std::expected<FramebufferExtent, std::string> CalculateFramebufferExtent(
		const glm::vec2& DisplaySize,
		const glm::vec2& FramebufferScale)
	{
		if (!std::isfinite(DisplaySize.x) || !std::isfinite(DisplaySize.y) ||
			!std::isfinite(FramebufferScale.x) || !std::isfinite(FramebufferScale.y) ||
			DisplaySize.x <= 0.0f || DisplaySize.y <= 0.0f ||
			FramebufferScale.x <= 0.0f || FramebufferScale.y <= 0.0f)
			return std::unexpected("Dear ImGui draw data has an invalid display size or framebuffer scale");

		const double Width = std::ceil(static_cast<double>(DisplaySize.x) * FramebufferScale.x);
		const double Height = std::ceil(static_cast<double>(DisplaySize.y) * FramebufferScale.y);
		const double MaximumExtent = static_cast<double>((std::numeric_limits<int>::max)());
		if (!std::isfinite(Width) || !std::isfinite(Height) || Width <= 0.0 || Height <= 0.0 ||
			Width > MaximumExtent || Height > MaximumExtent)
			return std::unexpected("Dear ImGui draw data has an invalid framebuffer extent");

		return FramebufferExtent{ static_cast<uint32_t>(Width), static_cast<uint32_t>(Height) };
	}

	[[nodiscard]] inline std::optional<glm::vec2> PositionToVulkanNdc(
		const glm::vec2& Position,
		const glm::vec2& DisplayPosition,
		const glm::vec2& DisplaySize)
	{
		if (!std::isfinite(Position.x) || !std::isfinite(Position.y) ||
			!std::isfinite(DisplayPosition.x) || !std::isfinite(DisplayPosition.y) ||
			!std::isfinite(DisplaySize.x) || !std::isfinite(DisplaySize.y) ||
			DisplaySize.x <= 0.0f || DisplaySize.y <= 0.0f)
			return std::nullopt;

		const glm::vec2 Relative = Position - DisplayPosition;
		// NVRHI's Vulkan DX-coordinate viewport maps framebuffer top to NDC +1 on Y.
		const glm::vec2 Ndc{
			2.0f * Relative.x / DisplaySize.x - 1.0f,
			1.0f - 2.0f * Relative.y / DisplaySize.y
		};
		return std::isfinite(Ndc.x) && std::isfinite(Ndc.y)
			? std::optional<glm::vec2>{ Ndc }
			: std::nullopt;
	}

	[[nodiscard]] inline std::expected<std::optional<PulseForge::ScissorRect>, std::string> ClipRectToScissor(
		const glm::vec2& ClipMinimum,
		const glm::vec2& ClipMaximum,
		const glm::vec2& DisplayPosition,
		const glm::vec2& FramebufferScale,
		FramebufferExtent Extent)
	{
		if (Extent.Width == 0 || Extent.Height == 0 ||
			!std::isfinite(ClipMinimum.x) || !std::isfinite(ClipMinimum.y) ||
			!std::isfinite(ClipMaximum.x) || !std::isfinite(ClipMaximum.y) ||
			!std::isfinite(DisplayPosition.x) || !std::isfinite(DisplayPosition.y) ||
			!std::isfinite(FramebufferScale.x) || !std::isfinite(FramebufferScale.y) ||
			FramebufferScale.x <= 0.0f || FramebufferScale.y <= 0.0f)
			return std::unexpected("Dear ImGui draw command has an invalid clip rectangle or framebuffer extent");

		const double MinX = (static_cast<double>(ClipMinimum.x) - DisplayPosition.x) * FramebufferScale.x;
		const double MinY = (static_cast<double>(ClipMinimum.y) - DisplayPosition.y) * FramebufferScale.y;
		const double MaxX = (static_cast<double>(ClipMaximum.x) - DisplayPosition.x) * FramebufferScale.x;
		const double MaxY = (static_cast<double>(ClipMaximum.y) - DisplayPosition.y) * FramebufferScale.y;
		if (!std::isfinite(MinX) || !std::isfinite(MinY) || !std::isfinite(MaxX) || !std::isfinite(MaxY))
			return std::unexpected("Dear ImGui draw command contains a non-finite clip rectangle");

		const double TargetWidth = Extent.Width;
		const double TargetHeight = Extent.Height;
		const double Left = std::clamp(std::floor(MinX), 0.0, TargetWidth);
		const double Top = std::clamp(std::floor(MinY), 0.0, TargetHeight);
		const double Right = std::clamp(std::ceil(MaxX), 0.0, TargetWidth);
		const double Bottom = std::clamp(std::ceil(MaxY), 0.0, TargetHeight);
		if (Right <= Left || Bottom <= Top)
			return std::optional<PulseForge::ScissorRect>{};

		return std::optional<PulseForge::ScissorRect>{ PulseForge::ScissorRect{
			static_cast<uint32_t>(Left),
			static_cast<uint32_t>(Top),
			static_cast<uint32_t>(Right - Left),
			static_cast<uint32_t>(Bottom - Top)
		} };
	}
}
