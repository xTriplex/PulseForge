#include "Assets/CookedProductCodec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <span>

namespace PulseForge
{
	namespace
	{
		constexpr uint32_t MeshCodecVersion = Detail::MeshProductSchemaVersion;
		constexpr uint32_t TextureCodecVersion = Detail::TextureProductSchemaVersion;
		constexpr uint32_t MaterialCodecVersion = Detail::MaterialProductSchemaVersion;
		constexpr uint32_t EnvironmentCodecVersion = 1;
		constexpr uint32_t BrdfCodecVersion = 1;
		constexpr uint64_t MaximumEnvironmentPayloadBytes = 64ull * 1024ull * 1024ull;
		constexpr size_t MaximumProductBytes = 256ull * 1024ull * 1024ull;
		constexpr uint32_t MaximumMeshVertices = 4'000'000;
		constexpr uint32_t MaximumMeshIndices = 12'000'000;
		constexpr uint32_t MaximumImagePixels = 32'000'000;

		void AppendU32(std::vector<std::byte>& Output, uint32_t Value)
		{
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Output.push_back(static_cast<std::byte>((Value >> Shift) & 0xffu));
		}

		bool ReadU32(std::span<const std::byte> Input, size_t& Offset, uint32_t& Value)
		{
			if (Offset > Input.size() || Input.size() - Offset < 4) return false;
			Value = 0;
			for (unsigned Index = 0; Index < 4; ++Index)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Input[Offset++])) << (Index * 8);
			return true;
		}

		void AppendFloat(std::vector<std::byte>& Output, float Value)
		{
			AppendU32(Output, std::bit_cast<uint32_t>(Value));
		}

		void AppendU64(std::vector<std::byte>& Output, uint64_t Value)
		{
			for (unsigned Shift = 0; Shift < 64; Shift += 8)
				Output.push_back(static_cast<std::byte>((Value >> Shift) & 0xffu));
		}

		bool ReadU64(std::span<const std::byte> Input, size_t& Offset, uint64_t& Value)
		{
			if (Offset > Input.size() || Input.size() - Offset < 8) return false;
			Value = 0;
			for (unsigned Index = 0; Index < 8; ++Index)
				Value |= static_cast<uint64_t>(std::to_integer<uint8_t>(Input[Offset++])) << (Index * 8);
			return true;
		}

		void AppendAssetID(std::vector<std::byte>& Output, const AssetID& Asset)
		{
			AppendU64(Output, Asset.GetHigh());
			AppendU64(Output, Asset.GetLow());
		}

		bool ReadAssetID(std::span<const std::byte> Input, size_t& Offset, AssetID& Asset)
		{
			uint64_t High = 0, Low = 0;
			if (!ReadU64(Input, Offset, High) || !ReadU64(Input, Offset, Low)) return false;
			Asset = AssetID(High, Low);
			return true;
		}

		template<size_t N>
		bool IsFiniteInRange(const std::array<float, N>& Values, float Minimum, float Maximum)
		{
			return std::ranges::all_of(Values, [=](float Value)
				{ return std::isfinite(Value) && Value >= Minimum && Value <= Maximum; });
		}

		bool ReadFloat(std::span<const std::byte> Input, size_t& Offset, float& Value)
		{
			uint32_t Bits = 0;
			if (!ReadU32(Input, Offset, Bits)) return false;
			Value = std::bit_cast<float>(Bits);
			return std::isfinite(Value);
		}

		uint32_t GetMipCount(uint32_t Size)
		{
			uint32_t Count = 1;
			while (Size > 1)
			{
				Size = std::max(1u, Size / 2);
				++Count;
			}
			return Count;
		}

		uint64_t GetEnvironmentFloatCount(const EnvironmentProcessingDesc& Description)
		{
			uint64_t Count = static_cast<uint64_t>(Description.EnvironmentFaceSize) * Description.EnvironmentFaceSize * 6 * 4;
			Count += static_cast<uint64_t>(Description.IrradianceFaceSize) * Description.IrradianceFaceSize * 6 * 4;
			for (uint32_t Size = Description.SpecularFaceSize;; Size = std::max(1u, Size / 2))
			{
				Count += static_cast<uint64_t>(Size) * Size * 6 * 4;
				if (Size == 1) break;
			}
			return Count;
		}

		bool IsValidEnvironmentDescription(const EnvironmentProcessingDesc& Description)
		{
			return Description.EnvironmentFaceSize > 0 && Description.EnvironmentFaceSize <= 2048 &&
				Description.IrradianceFaceSize > 0 && Description.IrradianceFaceSize <= 2048 &&
				Description.SpecularFaceSize > 0 && Description.SpecularFaceSize <= 2048 &&
				Description.BrdfLutSize > 0 && Description.BrdfLutSize <= 2048 &&
				Description.IrradianceSamples > 0 && Description.IrradianceSamples <= 4096 &&
				Description.PrefilterSamples > 0 && Description.PrefilterSamples <= 4096 &&
				Description.BrdfSamples > 0 && Description.BrdfSamples <= 4096 &&
				GetEnvironmentFloatCount(Description) <= MaximumEnvironmentPayloadBytes / sizeof(float);
		}

		bool AppendLevelFloats(std::vector<std::byte>& Payload, const FloatCubeLevel& Level, uint32_t ExpectedSize)
		{
			const uint64_t FloatCount = static_cast<uint64_t>(ExpectedSize) * ExpectedSize * 4;
			if (Level.Size != ExpectedSize) return false;
			for (const auto& Face : Level.Faces)
			{
				if (Face.size() != FloatCount || !std::ranges::all_of(Face, [](float Value) { return std::isfinite(Value); }))
					return false;
				for (float Value : Face) AppendFloat(Payload, Value);
			}
			return true;
		}

		bool ReadLevelFloats(std::span<const std::byte> Payload, size_t& Offset, FloatCubeLevel& Level, uint32_t Size)
		{
			Level.Size = Size;
			const uint64_t FloatCount = static_cast<uint64_t>(Size) * Size * 4;
			try
			{
				for (auto& Face : Level.Faces) Face.resize(static_cast<size_t>(FloatCount));
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
			for (auto& Face : Level.Faces)
				for (float& Value : Face)
					if (!ReadFloat(Payload, Offset, Value)) return false;
			return true;
		}

		uint32_t FullImageMipCount(uint32_t Width, uint32_t Height)
		{
			uint32_t Count = 1;
			while (Width > 1 || Height > 1)
			{
				Width = std::max(1u, Width / 2);
				Height = std::max(1u, Height / 2);
				++Count;
			}
			return Count;
		}
	} // namespace

	MeshDesc ImportedGltfMesh::GetMeshDescription() const
	{
		MeshDesc Description;
		Description.VertexLayout.Stride = sizeof(GltfMeshVertex);
		Description.VertexLayout.Attributes = {
			{ VertexSemantic::Position, VertexFormat::Float3, offsetof(GltfMeshVertex, Position) },
			{ VertexSemantic::Color, VertexFormat::Float4, offsetof(GltfMeshVertex, Color) },
			{ VertexSemantic::TexCoord, VertexFormat::Float2, offsetof(GltfMeshVertex, TexCoord) },
			{ VertexSemantic::Normal, VertexFormat::Float3, offsetof(GltfMeshVertex, Normal) },
			{ VertexSemantic::Tangent, VertexFormat::Float4, offsetof(GltfMeshVertex, Tangent) }
		};
		Description.VertexData = std::as_bytes(std::span(Vertices));
		Description.Indices = Indices;
		Description.DebugName = Name;
		return Description;
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EncodeMeshProduct(const ImportedGltfMesh& Mesh)
	{
		if (Mesh.Name.size() > 4096 || Mesh.Vertices.empty() || Mesh.Vertices.size() > MaximumMeshVertices ||
			Mesh.Indices.empty() || Mesh.Indices.size() > MaximumMeshIndices || Mesh.Indices.size() % 3 != 0)
			return std::unexpected("Mesh product exceeds artifact bounds");
		const uint64_t Required = 20ull + Mesh.Name.size() + static_cast<uint64_t>(Mesh.Vertices.size()) * 64ull +
			static_cast<uint64_t>(Mesh.Indices.size()) * 4ull;
		if (Required > MaximumProductBytes) return std::unexpected("Mesh product exceeds artifact byte limit");
		std::vector<std::byte> Out;
		Out.reserve(static_cast<size_t>(Required));
		Out.insert(Out.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'M'}, std::byte{'S'} });
		AppendU32(Out, MeshCodecVersion);
		AppendU32(Out, static_cast<uint32_t>(Mesh.Name.size()));
		for (unsigned char Character : Mesh.Name) Out.push_back(static_cast<std::byte>(Character));
		AppendU32(Out, static_cast<uint32_t>(Mesh.Vertices.size()));
		AppendU32(Out, static_cast<uint32_t>(Mesh.Indices.size()));
		for (const auto& Vertex : Mesh.Vertices)
		{
			for (float Value : Vertex.Position) AppendFloat(Out, Value);
			for (float Value : Vertex.Color) AppendFloat(Out, Value);
			for (float Value : Vertex.TexCoord) AppendFloat(Out, Value);
			for (float Value : Vertex.Normal) AppendFloat(Out, Value);
			for (float Value : Vertex.Tangent) AppendFloat(Out, Value);
		}
		for (uint32_t Index : Mesh.Indices) AppendU32(Out, Index);
		return Out;
	}

	std::expected<ImportedGltfMesh, std::string> Detail::DecodeMeshProduct(std::span<const std::byte> Input)
	{
		if (Input.size() < 20 || Input.size() > MaximumProductBytes || Input[0] != std::byte{'P'} ||
			Input[1] != std::byte{'F'} || Input[2] != std::byte{'M'} || Input[3] != std::byte{'S'})
			return std::unexpected("Mesh artifact has invalid magic or size");
		size_t Offset = 4;
		uint32_t Version = 0, NameSize = 0, VertexCount = 0, IndexCount = 0;
		if (!ReadU32(Input, Offset, Version) || Version != MeshCodecVersion || !ReadU32(Input, Offset, NameSize) ||
			NameSize > 4096 || NameSize > Input.size() - Offset)
			return std::unexpected("Mesh artifact schema or name is invalid");
		ImportedGltfMesh Mesh;
		Mesh.Name.assign(reinterpret_cast<const char*>(Input.data() + Offset), NameSize);
		Offset += NameSize;
		if (!ReadU32(Input, Offset, VertexCount) || !ReadU32(Input, Offset, IndexCount) || VertexCount == 0 ||
			VertexCount > MaximumMeshVertices || IndexCount == 0 || IndexCount > MaximumMeshIndices || IndexCount % 3 != 0)
			return std::unexpected("Mesh artifact geometry counts are invalid");
		const uint64_t PayloadBytes = static_cast<uint64_t>(VertexCount) * 64ull + static_cast<uint64_t>(IndexCount) * 4ull;
		if (PayloadBytes != Input.size() - Offset) return std::unexpected("Mesh artifact geometry length is inconsistent");
		Mesh.Vertices.resize(VertexCount);
		for (auto& Vertex : Mesh.Vertices)
		{
			for (float& Value : Vertex.Position) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh position is non-finite");
			for (float& Value : Vertex.Color) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh color is non-finite");
			for (float& Value : Vertex.TexCoord) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh UV is non-finite");
			for (float& Value : Vertex.Normal) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh normal is non-finite");
			for (float& Value : Vertex.Tangent) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Mesh tangent is non-finite");
		}
		Mesh.Indices.resize(IndexCount);
		for (uint32_t& Index : Mesh.Indices)
			if (!ReadU32(Input, Offset, Index) || Index >= VertexCount) return std::unexpected("Mesh index is out of range");
		return Mesh;
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EncodeTextureProduct(const PreparedTextureCpuProduct& Texture)
	{
		uint64_t TotalPixels = 0;
		for (const auto& Mip : Texture.MipLevels)
		{
			if (Mip.Pixels.size() > MaximumProductBytes - TotalPixels)
				return std::unexpected("Texture product exceeds artifact byte limit");
			TotalPixels += Mip.Pixels.size();
		}
		if (Texture.Width == 0 || Texture.Height == 0 || Texture.Width > 32768 || Texture.Height > 32768 ||
			static_cast<uint64_t>(Texture.Width) * Texture.Height > MaximumImagePixels ||
			(Texture.ColorSpace != ImageColorSpace::Linear && Texture.ColorSpace != ImageColorSpace::Srgb) ||
			Texture.MipLevels.size() != FullImageMipCount(Texture.Width, Texture.Height) || TotalPixels > MaximumProductBytes - 256)
			return std::unexpected("Texture product exceeds artifact bounds");
		std::vector<std::byte> Output;
		Output.reserve(24 + static_cast<size_t>(TotalPixels) + Texture.MipLevels.size() * 12);
		Output.insert(Output.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'T'}, std::byte{'X'} });
		AppendU32(Output, TextureCodecVersion);
		AppendU32(Output, static_cast<uint32_t>(Texture.ColorSpace));
		AppendU32(Output, Texture.Width);
		AppendU32(Output, Texture.Height);
		AppendU32(Output, static_cast<uint32_t>(Texture.MipLevels.size()));
		uint32_t Width = Texture.Width, Height = Texture.Height;
		for (const auto& Mip : Texture.MipLevels)
		{
			const uint64_t Expected = static_cast<uint64_t>(Width) * Height * 4;
			if (Mip.Width != Width || Mip.Height != Height || Mip.Pixels.size() != Expected)
				return std::unexpected("Texture mip layout is inconsistent");
			AppendU32(Output, Width);
			AppendU32(Output, Height);
			AppendU32(Output, static_cast<uint32_t>(Mip.Pixels.size()));
			for (uint8_t Value : Mip.Pixels) Output.push_back(static_cast<std::byte>(Value));
			Width = std::max(1u, Width / 2);
			Height = std::max(1u, Height / 2);
		}
		if (Width != 1 || Height != 1 || Output.size() > MaximumProductBytes)
			return std::unexpected("Texture mip chain is incomplete");
		return Output;
	}

	std::expected<Detail::PreparedTextureCpuProduct, std::string> Detail::DecodeTextureProduct(std::span<const std::byte> Input)
	{
		if (Input.size() < 12) return std::unexpected("Texture product header is truncated");
		size_t Offset = 8;
		uint32_t ColorSpace = 0;
		if (!ReadU32(Input, Offset, ColorSpace) || ColorSpace > static_cast<uint32_t>(ImageColorSpace::Srgb))
			return std::unexpected("Texture product color-space value is invalid");
		if (Input.size() < 24 || Input.size() > MaximumProductBytes || Input[0] != std::byte{'P'} ||
			Input[1] != std::byte{'F'} || Input[2] != std::byte{'T'} || Input[3] != std::byte{'X'})
			return std::unexpected("Texture artifact magic or size is invalid");
		Offset = 4;
		uint32_t Version = 0, EncodedColorSpace = 0, Count = 0;
		PreparedTextureCpuProduct Product;
		if (!ReadU32(Input, Offset, Version) || Version != TextureCodecVersion || !ReadU32(Input, Offset, EncodedColorSpace) ||
			EncodedColorSpace != ColorSpace || !ReadU32(Input, Offset, Product.Width) || !ReadU32(Input, Offset, Product.Height) ||
			!ReadU32(Input, Offset, Count) || Product.Width == 0 || Product.Height == 0 || Product.Width > 32768 ||
			Product.Height > 32768 || static_cast<uint64_t>(Product.Width) * Product.Height > MaximumImagePixels ||
			Count != FullImageMipCount(Product.Width, Product.Height))
			return std::unexpected("Texture artifact schema, format or dimensions are invalid");
		Product.ColorSpace = static_cast<ImageColorSpace>(ColorSpace);
		uint32_t Width = Product.Width, Height = Product.Height;
		Product.MipLevels.reserve(Count);
		for (uint32_t Index = 0; Index < Count; ++Index)
		{
			uint32_t MipWidth = 0, MipHeight = 0, ByteCount = 0;
			const uint64_t Expected = static_cast<uint64_t>(Width) * Height * 4;
			if (!ReadU32(Input, Offset, MipWidth) || !ReadU32(Input, Offset, MipHeight) || !ReadU32(Input, Offset, ByteCount) ||
				MipWidth != Width || MipHeight != Height || ByteCount != Expected || Offset > Input.size() ||
				Input.size() - Offset < ByteCount)
				return std::unexpected("Texture artifact mip is malformed");
			PreparedMipLevel Mip{ Width, Height, std::vector<uint8_t>(ByteCount) };
			for (uint8_t& Value : Mip.Pixels) Value = std::to_integer<uint8_t>(Input[Offset++]);
			Product.MipLevels.push_back(std::move(Mip));
			Width = std::max(1u, Width / 2);
			Height = std::max(1u, Height / 2);
		}
		if (Width != 1 || Height != 1 || Offset != Input.size())
			return std::unexpected("Texture artifact has trailing or incomplete data");
		return Product;
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EncodeMaterialProduct(const CookedMaterialProduct& Material)
	{
		if (!IsFiniteInRange(Material.BaseColorFactor, 0.0f, 1.0f) ||
			!IsFiniteInRange(Material.EmissiveFactor, 0.0f, 10000.0f) ||
			!std::isfinite(Material.MetallicFactor) || Material.MetallicFactor < 0.0f || Material.MetallicFactor > 1.0f ||
			!std::isfinite(Material.RoughnessFactor) || Material.RoughnessFactor < 0.0f || Material.RoughnessFactor > 1.0f ||
			!std::isfinite(Material.NormalScale) || Material.NormalScale < 0.0f || Material.NormalScale > 4.0f ||
			!std::isfinite(Material.OcclusionStrength) || Material.OcclusionStrength < 0.0f || Material.OcclusionStrength > 1.0f ||
			!std::isfinite(Material.AlphaCutoff) || Material.AlphaCutoff < 0.0f || Material.AlphaCutoff > 1.0f ||
			(Material.AlphaMode != CookedMaterialAlphaMode::Opaque && Material.AlphaMode != CookedMaterialAlphaMode::Mask &&
				Material.AlphaMode != CookedMaterialAlphaMode::Blend))
			return std::unexpected("Material product contains invalid factor or enum values");

		try
		{
			std::vector<std::byte> Output;
			Output.reserve(144);
			Output.insert(Output.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'M'}, std::byte{'T'} });
			AppendU32(Output, MaterialCodecVersion);
			AppendAssetID(Output, Material.BaseColorTexture);
			AppendAssetID(Output, Material.MetallicRoughnessTexture);
			AppendAssetID(Output, Material.NormalTexture);
			AppendAssetID(Output, Material.OcclusionTexture);
			AppendAssetID(Output, Material.EmissiveTexture);
			for (float Value : Material.BaseColorFactor) AppendFloat(Output, Value);
			AppendFloat(Output, Material.MetallicFactor);
			AppendFloat(Output, Material.RoughnessFactor);
			AppendFloat(Output, Material.NormalScale);
			AppendFloat(Output, Material.OcclusionStrength);
			for (float Value : Material.EmissiveFactor) AppendFloat(Output, Value);
			AppendU32(Output, static_cast<uint32_t>(Material.AlphaMode));
			AppendFloat(Output, Material.AlphaCutoff);
			AppendU32(Output, Material.DoubleSided ? 1u : 0u);
			return Output;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded material product payload");
		}
	}

	std::expected<CookedMaterialProduct, std::string> Detail::DecodeMaterialProduct(std::span<const std::byte> Input)
	{
		constexpr size_t EncodedSize = 144;
		if (Input.size() != EncodedSize || Input[0] != std::byte{'P'} || Input[1] != std::byte{'F'} ||
			Input[2] != std::byte{'M'} || Input[3] != std::byte{'T'})
			return std::unexpected("Material product has invalid magic or size");
		size_t Offset = 4;
		uint32_t Version = 0;
		CookedMaterialProduct Product;
		if (!ReadU32(Input, Offset, Version) || Version != MaterialCodecVersion ||
			!ReadAssetID(Input, Offset, Product.BaseColorTexture) ||
			!ReadAssetID(Input, Offset, Product.MetallicRoughnessTexture) ||
			!ReadAssetID(Input, Offset, Product.NormalTexture) || !ReadAssetID(Input, Offset, Product.OcclusionTexture) ||
			!ReadAssetID(Input, Offset, Product.EmissiveTexture))
			return std::unexpected("Material product schema or texture references are invalid");
		for (float& Value : Product.BaseColorFactor) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Material base-color factor is invalid");
		if (!ReadFloat(Input, Offset, Product.MetallicFactor) || !ReadFloat(Input, Offset, Product.RoughnessFactor) ||
			!ReadFloat(Input, Offset, Product.NormalScale) || !ReadFloat(Input, Offset, Product.OcclusionStrength))
			return std::unexpected("Material scalar factor is invalid");
		for (float& Value : Product.EmissiveFactor) if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Material emissive factor is invalid");
		uint32_t AlphaMode = 0, DoubleSided = 0;
		if (!ReadU32(Input, Offset, AlphaMode) || AlphaMode > static_cast<uint32_t>(CookedMaterialAlphaMode::Blend) ||
			!ReadFloat(Input, Offset, Product.AlphaCutoff) || !ReadU32(Input, Offset, DoubleSided) || DoubleSided > 1 ||
			Offset != Input.size())
			return std::unexpected("Material enum, flag, or trailing data is invalid");
		Product.AlphaMode = static_cast<CookedMaterialAlphaMode>(AlphaMode);
		Product.DoubleSided = DoubleSided != 0;
		if (!IsFiniteInRange(Product.BaseColorFactor, 0.0f, 1.0f) || !IsFiniteInRange(Product.EmissiveFactor, 0.0f, 10000.0f) ||
			Product.MetallicFactor < 0.0f || Product.MetallicFactor > 1.0f || Product.RoughnessFactor < 0.0f ||
			Product.RoughnessFactor > 1.0f || Product.NormalScale < 0.0f || Product.NormalScale > 4.0f ||
			Product.OcclusionStrength < 0.0f || Product.OcclusionStrength > 1.0f || Product.AlphaCutoff < 0.0f ||
			Product.AlphaCutoff > 1.0f)
			return std::unexpected("Material product factors are outside supported ranges");
		return Product;
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EnvironmentLightingArtifactCodec::EncodeEnvironment(
		const EnvironmentLightingData& Data, const EnvironmentProcessingDesc& Description)
	{
		if (!IsValidEnvironmentDescription(Description))
			return std::unexpected("Environment artifact processing dimensions or samples exceed bounds");
		const uint32_t MipCount = GetMipCount(Description.SpecularFaceSize);
		if (Data.PrefilteredSpecular.size() != MipCount)
			return std::unexpected("Environment artifact has an invalid specular mip count");
		try
		{
			const uint64_t FloatCount = GetEnvironmentFloatCount(Description);
			std::vector<std::byte> Payload;
			Payload.reserve(9 * sizeof(uint32_t) + static_cast<size_t>(FloatCount) * sizeof(float));
			AppendU32(Payload, EnvironmentCodecVersion);
			AppendU32(Payload, Description.EnvironmentFaceSize);
			AppendU32(Payload, Description.IrradianceFaceSize);
			AppendU32(Payload, Description.SpecularFaceSize);
			AppendU32(Payload, Description.BrdfLutSize);
			AppendU32(Payload, Description.IrradianceSamples);
			AppendU32(Payload, Description.PrefilterSamples);
			AppendU32(Payload, Description.BrdfSamples);
			AppendU32(Payload, MipCount);
			if (!AppendLevelFloats(Payload, Data.Environment, Description.EnvironmentFaceSize) ||
				!AppendLevelFloats(Payload, Data.DiffuseIrradiance, Description.IrradianceFaceSize))
				return std::unexpected("Environment or irradiance cube has an invalid layout or non-finite channel");
			uint32_t MipSize = Description.SpecularFaceSize;
			for (const FloatCubeLevel& Level : Data.PrefilteredSpecular)
			{
				if (!AppendLevelFloats(Payload, Level, MipSize))
					return std::unexpected("Specular cube mip has an invalid layout or non-finite channel");
				MipSize = std::max(1u, MipSize / 2);
			}
			return Payload;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded environment artifact payload");
		}
	}

	std::expected<EnvironmentLightingData, std::string> Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(
		std::span<const std::byte> Payload, const EnvironmentProcessingDesc& Description)
	{
		if (!IsValidEnvironmentDescription(Description))
			return std::unexpected("Environment artifact processing dimensions or samples exceed bounds");
		const uint64_t ExpectedBytes = 9 * sizeof(uint32_t) + GetEnvironmentFloatCount(Description) * sizeof(float);
		if (Payload.size() != ExpectedBytes || Payload.size() > MaximumEnvironmentPayloadBytes)
			return std::unexpected("Environment artifact payload size does not match its declared processing layout");
		size_t Offset = 0;
		std::array<uint32_t, 9> Fields{};
		for (uint32_t& Field : Fields)
			if (!ReadU32(Payload, Offset, Field)) return std::unexpected("Environment artifact header is truncated");
		const std::array<uint32_t, 9> ExpectedFields{ EnvironmentCodecVersion, Description.EnvironmentFaceSize,
			Description.IrradianceFaceSize, Description.SpecularFaceSize, Description.BrdfLutSize,
			Description.IrradianceSamples, Description.PrefilterSamples, Description.BrdfSamples,
			GetMipCount(Description.SpecularFaceSize) };
		if (Fields != ExpectedFields)
			return std::unexpected("Environment artifact codec or processing schema is incompatible");
		EnvironmentLightingData Data;
		if (!ReadLevelFloats(Payload, Offset, Data.Environment, Description.EnvironmentFaceSize) ||
			!ReadLevelFloats(Payload, Offset, Data.DiffuseIrradiance, Description.IrradianceFaceSize))
			return std::unexpected("Environment artifact has an invalid cube layout or non-finite channel");
		try
		{
			Data.PrefilteredSpecular.reserve(Fields[8]);
			uint32_t MipSize = Description.SpecularFaceSize;
			for (uint32_t Mip = 0; Mip < Fields[8]; ++Mip)
			{
				auto& Level = Data.PrefilteredSpecular.emplace_back();
				if (!ReadLevelFloats(Payload, Offset, Level, MipSize))
					return std::unexpected("Environment artifact has an invalid specular mip layout or channel");
				MipSize = std::max(1u, MipSize / 2);
			}
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded environment artifact result");
		}
		if (Offset != Payload.size()) return std::unexpected("Environment artifact contains trailing data");
		return Data;
	}

	std::expected<Detail::DecodedEnvironmentCpuProduct, std::string> Detail::EnvironmentLightingArtifactCodec::DecodeEnvironment(
		std::span<const std::byte> Payload)
	{
		if (Payload.size() < 9 * sizeof(uint32_t) || Payload.size() > MaximumEnvironmentPayloadBytes)
			return std::unexpected("Environment product is truncated or exceeds its size limit");
		std::array<uint32_t, 9> Fields{};
		size_t Offset = 0;
		for (uint32_t& Field : Fields)
			if (!ReadU32(Payload, Offset, Field)) return std::unexpected("Environment product header is truncated");
		if (Fields[0] != EnvironmentCodecVersion)
			return std::unexpected("Environment product codec version is unsupported");
		EnvironmentProcessingDesc Description{ Fields[1], Fields[2], Fields[3], Fields[4], Fields[5], Fields[6], Fields[7] };
		if (!IsValidEnvironmentDescription(Description) || Fields[8] != GetMipCount(Description.SpecularFaceSize))
			return std::unexpected("Environment product processing descriptor is invalid");
		auto Data = DecodeEnvironment(Payload, Description);
		if (!Data) return std::unexpected(Data.error());
		return Detail::DecodedEnvironmentCpuProduct{ std::move(*Data), Description };
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EnvironmentLightingArtifactCodec::EncodeBrdfLut(
		const ImportedFloatImage& Image, uint32_t Samples)
	{
		if (Image.Width == 0 || Image.Width > 2048 || Image.Height != Image.Width || Samples == 0 || Samples > 4096 ||
			Image.RGBA32FPixels.size() != static_cast<uint64_t>(Image.Width) * Image.Height * 4 ||
			Image.RGBA32FPixels.size() * sizeof(float) > MaximumEnvironmentPayloadBytes ||
			!std::ranges::all_of(Image.RGBA32FPixels, [](float Value) { return std::isfinite(Value); }))
			return std::unexpected("BRDF LUT dimensions, sample count, or pixels are invalid");
		try
		{
			std::vector<std::byte> Payload;
			Payload.reserve(4 * sizeof(uint32_t) + Image.RGBA32FPixels.size() * sizeof(float));
			AppendU32(Payload, BrdfCodecVersion);
			AppendU32(Payload, Image.Width);
			AppendU32(Payload, Image.Height);
			AppendU32(Payload, Samples);
			for (float Value : Image.RGBA32FPixels) AppendFloat(Payload, Value);
			return Payload;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded BRDF LUT artifact payload");
		}
	}

	std::expected<ImportedFloatImage, std::string> Detail::EnvironmentLightingArtifactCodec::DecodeBrdfLut(
		std::span<const std::byte> Payload, uint32_t ExpectedSize, uint32_t ExpectedSamples)
	{
		if (ExpectedSize == 0 || ExpectedSize > 2048 || ExpectedSamples == 0 || ExpectedSamples > 4096)
			return std::unexpected("Requested BRDF LUT dimensions or sample count exceed bounds");
		const uint64_t FloatCount = static_cast<uint64_t>(ExpectedSize) * ExpectedSize * 4;
		if (FloatCount * sizeof(float) > MaximumEnvironmentPayloadBytes ||
			Payload.size() != 4 * sizeof(uint32_t) + FloatCount * sizeof(float))
			return std::unexpected("BRDF LUT artifact payload size is invalid");
		size_t Offset = 0;
		std::array<uint32_t, 4> Fields{};
		for (uint32_t& Field : Fields)
			if (!ReadU32(Payload, Offset, Field)) return std::unexpected("BRDF LUT artifact header is truncated");
		if (Fields != std::array<uint32_t, 4>{ BrdfCodecVersion, ExpectedSize, ExpectedSize, ExpectedSamples })
			return std::unexpected("BRDF LUT artifact schema does not match the request");
		ImportedFloatImage Image;
		Image.Width = ExpectedSize;
		Image.Height = ExpectedSize;
		try
		{
			Image.RGBA32FPixels.resize(static_cast<size_t>(FloatCount));
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Could not allocate bounded BRDF LUT artifact result");
		}
		for (float& Value : Image.RGBA32FPixels)
			if (!ReadFloat(Payload, Offset, Value)) return std::unexpected("BRDF LUT artifact has a non-finite or truncated channel");
		return Image;
	}
} // namespace PulseForge
