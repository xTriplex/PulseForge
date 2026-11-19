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
#include <stop_token>
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

	struct GltfVertexInfluences final
	{
		std::array<uint16_t, 4> JointIndices{};
		std::array<float, 4> Weights{};
	};

	struct ImportedGltfSkeletonNode final
	{
		std::string StableKey;
		std::string Name;
		int32_t ParentNodeIndex = -1;
		bool IsJoint = false;
		// Column-major local bind transform; non-joint ancestors are retained in this node array.
		std::array<float, 16> LocalBindMatrix{};
	};

	enum class GltfAnimationPath : uint8_t
	{
		Translation,
		Rotation,
		Scale
	};

	enum class GltfAnimationInterpolation : uint8_t
	{
		Step,
		Linear
	};

	struct ImportedGltfAnimationChannel final
	{
		std::string TargetNodeKey;
		GltfAnimationPath Path = GltfAnimationPath::Translation;
		GltfAnimationInterpolation Interpolation = GltfAnimationInterpolation::Linear;
		std::vector<float> Times;
		std::vector<std::array<float, 4>> Values;
	};

	struct ImportedGltfAnimationEventMarker final
	{
		// Stable identity derived from source asset ID, clip key and the source-authored marker key.
		std::string StableKey;
		std::string Name;
		float TimeSeconds = 0.0f;
		friend bool operator==(const ImportedGltfAnimationEventMarker&, const ImportedGltfAnimationEventMarker&) = default;
	};

	struct ImportedGltfAnimationClip final
	{
		std::string StableKey;
		std::string Name;
		float Duration = 0.0f;
		std::vector<ImportedGltfAnimationChannel> Channels;
		std::vector<ImportedGltfAnimationEventMarker> EventMarkers;
	};

	struct ImportedGltfSkeletalMesh final
	{
		ImportedGltfMesh Geometry;
		std::vector<ImportedGltfSkeletonNode> SkeletonNodes;
		// Index into SkeletonNodes for the mesh node whose local/world transform frames skinning.
		uint16_t MeshNodeIndex = 0;
		// Skin-local joint order maps each JOINTS_0 value into SkeletonNodes.
		std::vector<uint16_t> JointNodeIndices;
		// Same order as JointNodeIndices, stored separately from node bind transforms.
		std::vector<std::array<float, 16>> InverseBindMatrices;
		std::vector<GltfVertexInfluences> VertexInfluences;
		std::vector<ImportedGltfAnimationClip> Animations;
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

	struct CookedGraphMaterialProduct final
	{
		struct Parameter final
		{
			UUID ID;
			std::string Name;
			float DefaultValue = 0.0f;
			friend bool operator==(const Parameter&, const Parameter&) = default;
		};

		uint32_t ArtifactVersion = 2;
		std::string RecipeFingerprint;
		std::vector<Parameter> Parameters;
		std::vector<std::byte> SpirV;
		CookedMaterialProduct Material;
	};

	namespace Detail
	{
		inline constexpr uint32_t MeshProductSchemaVersion = 1;
		inline constexpr uint32_t SkeletalMeshProductSchemaVersion = 3;
		inline constexpr uint32_t TextureProductSchemaVersion = 1;
		inline constexpr uint32_t EnvironmentProductSchemaVersion = 1;
		inline constexpr uint32_t MaterialProductSchemaVersion = 1;
		inline constexpr uint32_t GraphMaterialProductSchemaVersion = 2;

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
			const ImportedGltfMesh& Mesh, std::stop_token CancellationToken = {});
		[[nodiscard]] PULSEFORGE_API std::expected<ImportedGltfMesh, std::string> DecodeMeshProduct(
			std::span<const std::byte> Bytes);
		[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, std::string> EncodeSkeletalMeshProduct(
			const ImportedGltfSkeletalMesh& Mesh, std::stop_token CancellationToken = {});
		[[nodiscard]] PULSEFORGE_API std::expected<ImportedGltfSkeletalMesh, std::string> DecodeSkeletalMeshProduct(
			std::span<const std::byte> Bytes);
		[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, std::string> EncodeTextureProduct(
			const PreparedTextureCpuProduct& Texture, std::stop_token CancellationToken = {});
		[[nodiscard]] PULSEFORGE_API std::expected<PreparedTextureCpuProduct, std::string> DecodeTextureProduct(
			std::span<const std::byte> Bytes);
		[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, std::string> EncodeMaterialProduct(
			const CookedMaterialProduct& Material);
		[[nodiscard]] PULSEFORGE_API std::expected<CookedMaterialProduct, std::string> DecodeMaterialProduct(
			std::span<const std::byte> Bytes);
		[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::byte>, std::string> EncodeGraphMaterialProduct(
			const CookedGraphMaterialProduct& Material);
		[[nodiscard]] PULSEFORGE_API std::expected<CookedGraphMaterialProduct, std::string> DecodeGraphMaterialProduct(
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
				const EnvironmentProcessingDesc& Description,
				std::stop_token CancellationToken = {});
			[[nodiscard]] static std::expected<EnvironmentLightingData, std::string> DecodeEnvironment(
				std::span<const std::byte> Payload,
				const EnvironmentProcessingDesc& Description);
			[[nodiscard]] static std::expected<DecodedEnvironmentCpuProduct, std::string> DecodeEnvironment(
				std::span<const std::byte> Payload);
			[[nodiscard]] static std::expected<std::vector<std::byte>, std::string> EncodeBrdfLut(
				const ImportedFloatImage& Image,
				uint32_t Samples,
				std::stop_token CancellationToken = {});
			[[nodiscard]] static std::expected<ImportedFloatImage, std::string> DecodeBrdfLut(
				std::span<const std::byte> Payload,
				uint32_t ExpectedSize,
				uint32_t ExpectedSamples);
		};
	} // namespace Detail
} // namespace PulseForge
