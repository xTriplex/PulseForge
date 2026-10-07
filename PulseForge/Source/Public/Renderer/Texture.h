#pragma once

#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace PulseForge
{
	enum class TextureFormat : uint8_t
	{
		RGBA8_UNorm,
		RGBA8_Srgb,
		Depth32Float
	};

	enum class TextureUsage : uint8_t
	{
		ShaderResource = 1 << 0,
		ColorAttachment = 1 << 1,
		DepthStencilAttachment = 1 << 2
	};

	[[nodiscard]] constexpr TextureUsage operator|(TextureUsage Left, TextureUsage Right) noexcept
	{
		return static_cast<TextureUsage>(static_cast<uint8_t>(Left) | static_cast<uint8_t>(Right));
	}

	[[nodiscard]] constexpr bool HasTextureUsage(TextureUsage Usage, TextureUsage Required) noexcept
	{
		return (static_cast<uint8_t>(Usage) & static_cast<uint8_t>(Required)) == static_cast<uint8_t>(Required);
	}

	struct TextureDesc
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		TextureFormat Format = TextureFormat::RGBA8_Srgb;
		TextureUsage Usage = TextureUsage::ShaderResource;
		std::string DebugName;
	};

	class PULSEFORGE_API Texture
	{
	public:
		virtual ~Texture() = default;
		[[nodiscard]] virtual const TextureDesc& GetDescription() const noexcept = 0;
	};

	using TextureHandle = std::unique_ptr<Texture>;

	enum class SamplerFilter : uint8_t
	{
		Nearest,
		Linear
	};

	enum class SamplerAddressMode : uint8_t
	{
		Repeat,
		ClampToEdge
	};

	struct SamplerDesc
	{
		SamplerFilter Minification = SamplerFilter::Linear;
		SamplerFilter Magnification = SamplerFilter::Linear;
		SamplerAddressMode AddressU = SamplerAddressMode::Repeat;
		SamplerAddressMode AddressV = SamplerAddressMode::Repeat;
		std::string DebugName;
	};

	class PULSEFORGE_API Sampler
	{
	public:
		virtual ~Sampler() = default;
		[[nodiscard]] virtual const SamplerDesc& GetDescription() const noexcept = 0;
	};

	using SamplerHandle = std::unique_ptr<Sampler>;

	enum class TextureErrorCode : uint8_t
	{
		InvalidDescription,
		InvalidData,
		UnsupportedFeature,
		BackendFailure
	};

	struct TextureError
	{
		TextureErrorCode Code;
		std::string Message;
	};

	using TextureCreateResult = std::expected<TextureHandle, TextureError>;
	using SamplerCreateResult = std::expected<SamplerHandle, TextureError>;

	[[nodiscard]] PULSEFORGE_API std::expected<size_t, TextureError> ValidateTextureUpload(
		const TextureDesc& Description,
		size_t InitialDataSize);
	[[nodiscard]] PULSEFORGE_API std::expected<void, TextureError> ValidateSamplerDescription(const SamplerDesc& Description);
}
