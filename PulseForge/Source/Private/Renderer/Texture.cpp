#include "Core/PulseForgePCH.h"
#include "Renderer/Texture.h"

#include <limits>
#include <algorithm>
#include <cmath>

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

		std::expected<void, TextureError> MakeTextureValidationError(TextureErrorCode Code, const char* Message)
		{
			return std::unexpected(TextureError{ Code, Message });
		}
	}

	std::expected<size_t, TextureError> ValidateTextureUpload(
		const TextureDesc& Description,
		size_t InitialDataSize)
	{
		if (Description.Dimension != TextureDimension::Texture2D || Description.MipLevels != 1)
			return MakeTextureError(TextureErrorCode::InvalidDescription,
				"Contiguous uploads support one 2D mip; use explicit subresource uploads for cubes or mip chains");
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

		if (Description.Format != TextureFormat::RGBA8_UNorm && Description.Format != TextureFormat::RGBA8_Srgb &&
			Description.Format != TextureFormat::RGBA32_Float)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture format is not supported by PulseForge");

		if (HasTextureUsage(Description.Usage, TextureUsage::DepthStencilAttachment))
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Color textures cannot use DepthStencilAttachment usage");

		const bool IsShaderResource = HasTextureUsage(Description.Usage, TextureUsage::ShaderResource);
		const bool IsColorAttachment = HasTextureUsage(Description.Usage, TextureUsage::ColorAttachment);
		if (!IsShaderResource && !IsColorAttachment)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "RGBA8 textures require ShaderResource or ColorAttachment usage");

		const uint64_t BytesPerPixel = Description.Format == TextureFormat::RGBA32_Float ? 16 : 4;
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
			return MakeTextureError(TextureErrorCode::InvalidData, "Texture upload must provide exactly width * height * bytes-per-pixel");

		return static_cast<size_t>(RequiredBytes);
	}

	std::expected<void, TextureError> ValidateTextureUpload(
		const TextureDesc& Description,
		std::span<const TextureSubresourceData> InitialData)
	{
		if (Description.Width == 0 || Description.Height == 0)
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Texture dimensions must be non-zero");
		if (Description.Dimension != TextureDimension::Texture2D && Description.Dimension != TextureDimension::TextureCube)
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Texture dimension is not supported");
		if (Description.Dimension == TextureDimension::TextureCube && Description.Width != Description.Height)
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Cubemap faces must be square");

		const uint32_t LargestDimension = std::max(Description.Width, Description.Height);
		uint32_t MaximumMipLevels = 1;
		for (uint32_t MipDimension = LargestDimension; MipDimension > 1; MipDimension >>= 1)
			++MaximumMipLevels;
		if (Description.MipLevels == 0 || Description.MipLevels > MaximumMipLevels)
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Texture mip count is outside the legal dimension range");

		constexpr uint8_t SupportedUsageBits =
			static_cast<uint8_t>(TextureUsage::ShaderResource) |
			static_cast<uint8_t>(TextureUsage::ColorAttachment) |
			static_cast<uint8_t>(TextureUsage::DepthStencilAttachment);
		const uint8_t UsageBits = static_cast<uint8_t>(Description.Usage);
		if (UsageBits == 0 || (UsageBits & ~SupportedUsageBits) != 0)
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Texture usage contains no supported usage or unknown flags");

		if (Description.Format == TextureFormat::Depth32Float)
		{
			if (Description.Dimension != TextureDimension::Texture2D || Description.MipLevels != 1 ||
				Description.Usage != TextureUsage::DepthStencilAttachment)
				return MakeTextureValidationError(TextureErrorCode::InvalidDescription,
					"Depth32Float textures require a single-mip Texture2D depth attachment");
			if (!InitialData.empty())
				return MakeTextureValidationError(TextureErrorCode::InvalidData, "Depth attachment textures do not accept initial upload data");
			return {};
		}
		if (Description.Format != TextureFormat::RGBA8_UNorm && Description.Format != TextureFormat::RGBA8_Srgb &&
			Description.Format != TextureFormat::RGBA32_Float)
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Texture format is not supported by PulseForge");
		if (HasTextureUsage(Description.Usage, TextureUsage::DepthStencilAttachment))
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Color textures cannot use DepthStencilAttachment usage");
		if (!HasTextureUsage(Description.Usage, TextureUsage::ShaderResource) &&
			!HasTextureUsage(Description.Usage, TextureUsage::ColorAttachment))
			return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Color textures require ShaderResource or ColorAttachment usage");
		if (Description.Dimension == TextureDimension::TextureCube &&
			HasTextureUsage(Description.Usage, TextureUsage::ColorAttachment))
			return MakeTextureValidationError(TextureErrorCode::UnsupportedFeature, "Cubemap color attachments are not exposed by this API");
		if (InitialData.empty())
			return {};

		const uint32_t ArraySlices = Description.Dimension == TextureDimension::TextureCube ? 6u : 1u;
		const size_t ExpectedSubresources = static_cast<size_t>(Description.MipLevels) * ArraySlices;
		if (InitialData.size() != ExpectedSubresources)
			return MakeTextureValidationError(TextureErrorCode::InvalidData, "Initialized textures require every mip and cube face exactly once");
		std::vector<bool> Seen(ExpectedSubresources, false);
		const size_t BytesPerPixel = Description.Format == TextureFormat::RGBA32_Float ? 16 : 4;
		for (const TextureSubresourceData& Subresource : InitialData)
		{
			if (Subresource.MipLevel >= Description.MipLevels || Subresource.ArraySlice >= ArraySlices)
				return MakeTextureValidationError(TextureErrorCode::InvalidData, "Texture upload mip or array slice is out of range");
			const size_t Index = static_cast<size_t>(Subresource.MipLevel) * ArraySlices + Subresource.ArraySlice;
			if (Seen[Index])
				return MakeTextureValidationError(TextureErrorCode::InvalidData, "Texture upload contains a duplicate subresource");
			Seen[Index] = true;
			const size_t MipWidth = std::max(1u, Description.Width >> Subresource.MipLevel);
			const size_t MipHeight = std::max(1u, Description.Height >> Subresource.MipLevel);
			if (MipWidth > std::numeric_limits<size_t>::max() / BytesPerPixel)
				return MakeTextureValidationError(TextureErrorCode::InvalidDescription, "Texture row size exceeds addressable memory");
			const size_t PackedRowPitch = MipWidth * BytesPerPixel;
			const size_t RowPitch = Subresource.RowPitch == 0 ? PackedRowPitch : Subresource.RowPitch;
			if (RowPitch < PackedRowPitch || MipHeight > std::numeric_limits<size_t>::max() / RowPitch ||
				Subresource.Data.size() != RowPitch * MipHeight)
				return MakeTextureValidationError(TextureErrorCode::InvalidData, "Texture subresource row pitch or data size is invalid");
		}
		return {};
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
		if (Description.Mip != SamplerMipFilter::None && Description.Mip != SamplerMipFilter::Linear)
			return MakeSamplerError(TextureErrorCode::InvalidDescription, "Sampler mip filter must be None or Linear");

		return {};
	}
}
