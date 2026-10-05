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

		if (Description.Format != TextureFormat::RGBA8_UNorm && Description.Format != TextureFormat::RGBA8_Srgb)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture format is not supported by PulseForge");

		constexpr uint64_t BytesPerPixel = 4;
		const uint64_t Width = Description.Width;
		const uint64_t Height = Description.Height;
		if (Width > std::numeric_limits<uint64_t>::max() / BytesPerPixel / Height)
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture dimensions overflow the upload-size representation");

		const uint64_t RequiredBytes = Width * Height * BytesPerPixel;
		if (RequiredBytes > std::numeric_limits<size_t>::max())
			return MakeTextureError(TextureErrorCode::InvalidDescription, "Texture upload exceeds the addressable memory size");

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
