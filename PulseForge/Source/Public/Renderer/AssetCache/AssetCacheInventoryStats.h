#pragma once

#include "Renderer/Texture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace PulseForge
{
	struct AssetCacheInventoryStats final
	{
		size_t Entries = 0;
		size_t PublishedResources = 0;
		size_t PendingResources = 0;
		size_t FailedResources = 0;
		uint64_t LogicalPayloadBytes = 0;
		size_t UnknownLogicalPayloads = 0;
	};

	[[nodiscard]] inline uint64_t SaturatingAddResourceBytes(uint64_t Left, uint64_t Right) noexcept
	{
		return Right > std::numeric_limits<uint64_t>::max() - Left
			? std::numeric_limits<uint64_t>::max()
			: Left + Right;
	}

	[[nodiscard]] inline std::optional<uint64_t> EstimateTextureLogicalPayloadBytes(const TextureDesc& Description) noexcept
	{
		if (Description.Width == 0 || Description.Height == 0 || Description.MipLevels == 0) return std::nullopt;
		uint64_t BytesPerPixel = 0;
		switch (Description.Format)
		{
			case TextureFormat::RGBA8_UNorm:
			case TextureFormat::RGBA8_Srgb:
			case TextureFormat::Depth32Float: BytesPerPixel = 4; break;
			case TextureFormat::RGBA16_Float: BytesPerPixel = 8; break;
			case TextureFormat::RGBA32_Float: BytesPerPixel = 16; break;
			default: return std::nullopt;
		}
		uint64_t Slices = 1;
		switch (Description.Dimension)
		{
			case TextureDimension::Texture2D: break;
			case TextureDimension::TextureCube: Slices = 6; break;
			default: return std::nullopt;
		}
		uint64_t Total = 0;
		uint64_t Width = Description.Width;
		uint64_t Height = Description.Height;
		for (uint32_t Mip = 0; Mip < Description.MipLevels; ++Mip)
		{
			if (Width > std::numeric_limits<uint64_t>::max() / Height) return std::nullopt;
			const uint64_t Pixels = Width * Height;
			if (Pixels > std::numeric_limits<uint64_t>::max() / BytesPerPixel / Slices) return std::nullopt;
			Total = SaturatingAddResourceBytes(Total, Pixels * BytesPerPixel * Slices);
			Width = (std::max)(uint64_t{ 1 }, Width / 2);
			Height = (std::max)(uint64_t{ 1 }, Height / 2);
		}
		return Total;
	}
} // namespace PulseForge
