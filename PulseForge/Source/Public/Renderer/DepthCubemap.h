#pragma once

#include "Core/Core.h"
#include "Renderer/RenderTarget.h"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace PulseForge
{
	class PULSEFORGE_API DepthCubemap
	{
	public:
		virtual ~DepthCubemap() = default;
		[[nodiscard]] virtual const Texture& GetTexture() const noexcept = 0;
		[[nodiscard]] virtual const RenderTarget& GetFaceTarget(uint32_t Face) const = 0;
		[[nodiscard]] virtual uint32_t GetResolution() const noexcept = 0;
	};

	using DepthCubemapHandle = std::unique_ptr<DepthCubemap>;

	enum class DepthCubemapErrorCode : uint8_t
	{
		InvalidDescription,
		UnsupportedFeature,
		BackendFailure
	};

	struct DepthCubemapError
	{
		DepthCubemapErrorCode Code;
		std::string Message;
	};

	using DepthCubemapCreateResult = std::expected<DepthCubemapHandle, DepthCubemapError>;
}
