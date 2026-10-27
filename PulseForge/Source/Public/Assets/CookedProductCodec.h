#pragma once

#include "Assets/ImportedImageData.h"
#include "Assets/AssetID.h"
#include "Core/Core.h"
#include "Renderer/MeshData.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	struct AssetContentFingerprint;
	struct DerivedDataArtifactKey;

	// CPU-side mesh data shared by source import and cooked runtime decoding. It owns no renderer resources.
	struct GltfMeshVertex
	{
		float Position[3]{};
		float Color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		float TexCoord[2]{};
		float Normal[3]{ 0.0f, 0.0f, 1.0f };
		std::array<float, 4> Tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
	};

	struct ImportedGltfMesh
	{
		std::string Name;
		std::vector<GltfMeshVertex> Vertices;
		std::vector<uint32_t> Indices;

		// The returned spans borrow from this object's vectors and remain valid until it is modified, moved, or destroyed.
		[[nodiscard]] MeshDesc GetMeshDescription() const;
	};

	enum class CubeFace : uint8_t
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ
	};

	struct FloatCubeLevel
	{
		uint32_t Size = 0;
		// Face order is +X, -X, +Y, -Y, +Z, -Z; each face is row-major RGBA32F.
		std::array<std::vector<float>, 6> Faces;
	};

	struct EnvironmentLightingData
	{
		FloatCubeLevel Environment;
		FloatCubeLevel DiffuseIrradiance;
		// Mip zero is sharp (roughness zero); the last mip is roughness one.
		std::vector<FloatCubeLevel> PrefilteredSpecular;
		ImportedFloatImage BrdfIntegrationLut;
	};

	struct EnvironmentProcessingDesc
	{
		uint32_t EnvironmentFaceSize = 256;
		uint32_t IrradianceFaceSize = 32;
		uint32_t SpecularFaceSize = 128;
		uint32_t BrdfLutSize = 256;
		uint32_t IrradianceSamples = 128;
		uint32_t PrefilterSamples = 128;
		uint32_t BrdfSamples = 128;
	};

	// Renderer-independent, versioned material values used by cooked runtime content.
	enum class CookedMaterialAlphaMode : uint8_t
	{
		Opaque,
		Mask,
		Blend
	};

	struct CookedMaterialProduct final
	{
		AssetID BaseColorTexture;
		AssetID MetallicRoughnessTexture;
		AssetID NormalTexture;
		AssetID OcclusionTexture;
		AssetID EmissiveTexture;
		std::array<float, 4> BaseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
		float MetallicFactor = 0.0f;
		float RoughnessFactor = 1.0f;
		float NormalScale = 1.0f;
		float OcclusionStrength = 1.0f;
		std::array<float, 3> EmissiveFactor{};
		CookedMaterialAlphaMode AlphaMode = CookedMaterialAlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
	};

	namespace Detail
	{
		inline constexpr uint32_t MeshProductSchemaVersion = 1;
		inline constexpr uint32_t TextureProductSchemaVersion = 1;
		inline constexpr uint32_t EnvironmentProductSchemaVersion = 1;
		inline constexpr uint32_t MaterialProductSchemaVersion = 1;

		enum class ImageColorSpace : uint8_t
		{
			Linear,
			Srgb
		};

		struct PreparedMipLevel
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::vector<uint8_t> Pixels;
		};

		struct PreparedTextureCpuProduct
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			ImageColorSpace ColorSpace = ImageColorSpace::Srgb;
			std::vector<PreparedMipLevel> MipLevels;
		};

		[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, std::string> EncodeMeshProduct(
			const ImportedGltfMesh& Mesh);
		[[nodiscard]] PULSEFORGE_API std::expected<ImportedGltfMesh, std::string> DecodeMeshProduct(
			std::span<const std::byte> Bytes);
		[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, std::string> EncodeTextureProduct(
			const PreparedTextureCpuProduct& Texture);
		[[nodiscard]] PULSEFORGE_API std::expected<PreparedTextureCpuProduct, std::string> DecodeTextureProduct(
			std::span<const std::byte> Bytes);
		[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, std::string> EncodeMaterialProduct(
			const CookedMaterialProduct& Material);
		[[nodiscard]] PULSEFORGE_API std::expected<CookedMaterialProduct, std::string> DecodeMaterialProduct(
			std::span<const std::byte> Bytes);

		struct DecodedEnvironmentCpuProduct final
		{
			EnvironmentLightingData Data;
			EnvironmentProcessingDesc Processing;
		};

		class EnvironmentLightingArtifactCodec final
		{
		public:
			[[nodiscard]] static DerivedDataArtifactKey MakeEnvironmentKey(
				const AssetID& Asset,
				const AssetContentFingerprint& SourceContent,
				const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static DerivedDataArtifactKey MakeBrdfLutKey(const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static std::expected<std::vector<std::byte>, std::string> EncodeEnvironment(
				const EnvironmentLightingData& Data,
				const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static std::expected<EnvironmentLightingData, std::string> DecodeEnvironment(
				std::span<const std::byte> Payload,
				const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static std::expected<DecodedEnvironmentCpuProduct, std::string> DecodeEnvironment(
				std::span<const std::byte> Payload);
			[[nodiscard]] static std::expected<std::vector<std::byte>, std::string> EncodeBrdfLut(
				const ImportedFloatImage& Image,
				uint32_t Samples);
			[[nodiscard]] static std::expected<ImportedFloatImage, std::string> DecodeBrdfLut(
				std::span<const std::byte> Payload,
				uint32_t ExpectedSize,
				uint32_t ExpectedSamples);
		};
	} // namespace Detail
} // namespace PulseForge
