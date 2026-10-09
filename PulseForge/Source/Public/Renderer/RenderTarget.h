#pragma once

#include "Core/Core.h"
#include "Renderer/Graphics.h"
#include "Renderer/Texture.h"

#include <array>
#include <expected>
#include <memory>
#include <string>

namespace PulseForge
{
	struct RenderTargetDesc
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		// None creates a depth-only target; in that case no color texture exists.
		ColorTargetFormat ColorFormat = ColorTargetFormat::RGBA8_Srgb;
		std::string DebugName;
	};

	struct RenderTargetClearValue
	{
		std::array<float, 4> Color = { 0.0f, 0.0f, 0.0f, 1.0f };
		float Depth = 1.0f;
	};

	class PULSEFORGE_API RenderTarget
	{
	public:
		virtual ~RenderTarget() = default;
		[[nodiscard]] virtual const RenderTargetDesc& GetDescription() const noexcept = 0;
		[[nodiscard]] virtual const Texture* GetColorTexture() const noexcept = 0;
		[[nodiscard]] virtual const Texture& GetDepthTexture() const noexcept = 0;
	};

	using RenderTargetHandle = std::unique_ptr<RenderTarget>;

	enum class RenderTargetErrorCode : uint8_t
	{
		InvalidDescription,
		UnsupportedFeature,
		BackendFailure
	};

	struct RenderTargetError
	{
		RenderTargetErrorCode Code;
		std::string Message;
	};

	using RenderTargetCreateResult = std::expected<RenderTargetHandle, RenderTargetError>;

	[[nodiscard]] PULSEFORGE_API std::expected<void, RenderTargetError> ValidateRenderTargetDescription(
		const RenderTargetDesc& Description);
	[[nodiscard]] PULSEFORGE_API std::expected<void, RenderTargetError> ValidateRenderTargetClearValue(
		const RenderTargetClearValue& ClearValue);
}
