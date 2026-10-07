#include "Core/PulseForgePCH.h"
#include "Renderer/Texture.h"

#include <limits>

namespace PulseForge
{
	namespace
	{
		std::expected<size_t, TextureError> MakeTextureError(TextureErrorCode Code, const char* Message)
		{
			return std::unexpected(TextureError{ Code, Message });
		}

		std::expected<void, TextureError> MakeSamplerError(TextureErrorCode Code, const char* Message)
		{
			return std::unexpected(TextureError{ Code, Message });
		}
	}

	std::expected<size_t, TextureError> ValidateTextureUpload(
		const TextureDesc& Description,
		size_t InitialDataSize)
	{
		if (Description.Width == 0 || Description.Height == 0)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture dimensions must be non-zero");

		constexpr uint8_t SupportedUsageBits =
			static_cast<uint8_t>(TextureUsage::ShaderResource) |
			static_cast<uint8_t>(TextureUsage::ColorAttachment) |
			static_cast<uint8_t>(TextureUsage::DepthStencilAttachment);
		const uint8_t UsageBits = static_cast<uint8_t>(Description.Usage);
		if (UsageBits == 0 || (UsageBits & ~SupportedUsageBits) != 0)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture usage contains no supported usage or unknown flags");

		if (Description.Format == TextureFormat::Depth32Float)
		{
			if (Description.Usage != TextureUsage::DepthStencilAttachment)
				return MakeTextureError(TextureErrorCode::InvalidDescription, "Depth32Float textures require DepthStencilAttachment usage");

			if (InitialDataSize != 0)
				return MakeTextureError(TextureErrorCode::InvalidData, "Depth attachment textures do not accept initial upload data");

			return size_t{ 0 };
		}

		if (Description.Format != TextureFormat::RGBA8_UNorm && Description.Format != TextureFormat::RGBA8_Srgb)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture format is not supported by PulseForge");

		if (HasTextureUsage(Description.Usage, TextureUsage::DepthStencilAttachment))
			return MakeTextureError(TextureErrorCode::InvalidDescription, "RGBA8 textures cannot use DepthStencilAttachment usage");

		const bool IsShaderResource = HasTextureUsage(Description.Usage, TextureUsage::ShaderResource);
		const bool IsColorAttachment = HasTextureUsage(Description.Usage, TextureUsage::ColorAttachment);
		if (!IsShaderResource && !IsColorAttachment)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "RGBA8 textures require ShaderResource or ColorAttachment usage");

		constexpr uint64_t BytesPerPixel = 4;
		const uint64_t Width = Description.Width;
		const uint64_t Height = Description.Height;
		if (Width > std::numeric_limits<uint64_t>::max() / BytesPerPixel / Height)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture dimensions overflow the upload-size representation");

		const uint64_t RequiredBytes = Width * Height * BytesPerPixel;
		if (RequiredBytes > std::numeric_limits<size_t>::max())
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture upload exceeds the addressable memory size");

		if (InitialDataSize == 0 && IsColorAttachment)
			return size_t{ 0 };

		if (InitialDataSize != RequiredBytes)
			return MakeTextureError(TextureErrorCode::InvalidData, "RGBA8 texture upload must provide exactly width * height * 4 bytes");

		return static_cast<size_t>(RequiredBytes);
	}

	std::expected<void, TextureError> ValidateSamplerDescription(const SamplerDesc& Description)
	{
		if ((Description.Minification != SamplerFilter::Nearest && Description.Minification != SamplerFilter::Linear) ||
			(Description.Magnification != SamplerFilter::Nearest && Description.Magnification != SamplerFilter::Linear))
		{
			return MakeSamplerError(TextureErrorCode::InvalidDescription, "Sampler filter must be Nearest or Linear");
		}

		const auto IsValidAddressMode = [](SamplerAddressMode Mode)
		{
			return Mode == SamplerAddressMode::Repeat || Mode == SamplerAddressMode::ClampToEdge;
		};
		if (!IsValidAddressMode(Description.AddressU) || !IsValidAddressMode(Description.AddressV))
			return MakeSamplerError(TextureErrorCode::InvalidDescription, "Sampler address mode must be Repeat or ClampToEdge");

		return {};
	}
}
