#include "TestSupport.h"

#include "Renderer/AssetCache/AssetCacheInventoryStats.h"

#include <limits>

void RunAssetCacheInventoryStatsTests(TestRunner& Tests)
{
	using namespace PulseForge;

	TextureDesc MippedTexture;
	MippedTexture.Width = 4;
	MippedTexture.Height = 2;
	MippedTexture.MipLevels = 3;
	MippedTexture.Format = TextureFormat::RGBA8_UNorm;
	const auto MippedBytes = EstimateTextureLogicalPayloadBytes(MippedTexture);
	PF_CHECK(Tests, MippedBytes == 44);

	TextureDesc Cube;
	Cube.Width = 4;
	Cube.Height = 4;
	Cube.MipLevels = 3;
	Cube.Format = TextureFormat::RGBA16_Float;
	Cube.Dimension = TextureDimension::TextureCube;
	const auto CubeBytes = EstimateTextureLogicalPayloadBytes(Cube);
	PF_CHECK(Tests, CubeBytes == 1008);

	TextureDesc Depth;
	Depth.Width = 2;
	Depth.Height = 2;
	Depth.Format = TextureFormat::Depth32Float;
	PF_CHECK(Tests, EstimateTextureLogicalPayloadBytes(Depth) == 16);

	TextureDesc Invalid = MippedTexture;
	Invalid.MipLevels = 0;
	PF_CHECK(Tests, !EstimateTextureLogicalPayloadBytes(Invalid));
	Invalid = MippedTexture;
	Invalid.Width = std::numeric_limits<uint32_t>::max();
	Invalid.Height = std::numeric_limits<uint32_t>::max();
	Invalid.Format = TextureFormat::RGBA32_Float;
	PF_CHECK(Tests, !EstimateTextureLogicalPayloadBytes(Invalid));
	Invalid = MippedTexture;
	Invalid.Format = static_cast<TextureFormat>(0xff);
	PF_CHECK(Tests, !EstimateTextureLogicalPayloadBytes(Invalid));
	PF_CHECK(Tests, SaturatingAddResourceBytes(std::numeric_limits<uint64_t>::max() - 2, 3) ==
		std::numeric_limits<uint64_t>::max());
}
