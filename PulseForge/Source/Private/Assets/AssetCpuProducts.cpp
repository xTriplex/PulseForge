#include "Assets/Detail/AssetCpuProducts.h"

#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/ImageAssetImporter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <fstream>
#include <limits>
#include <span>

namespace PulseForge::Detail
{
	namespace
	{
		constexpr uint32_t MeshArtifactVersion = 1;
		constexpr uint32_t TextureArtifactVersion = 1;
		constexpr uint32_t MaximumMeshVertices = 4'000'000;
		constexpr uint32_t MaximumMeshIndices = 12'000'000;
		constexpr uint32_t MaximumImagePixels = 32'000'000;
		constexpr size_t MaximumArtifactBytes = 256ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumImageSourceBytes = 128ull * 1024ull * 1024ull;

		uint64_t TimestampNanoseconds(std::chrono::steady_clock::time_point Timestamp)
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				Timestamp.time_since_epoch()).count());
		}

		AssetCpuPhaseTiming MakeTiming(
			std::chrono::steady_clock::time_point Start,
			std::chrono::steady_clock::time_point End)
		{
			return { TimestampNanoseconds(Start), TimestampNanoseconds(End) };
		}

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

		bool ReadFloat(std::span<const std::byte> Input, size_t& Offset, float& Value)
		{
			uint32_t Bits = 0;
			if (!ReadU32(Input, Offset, Bits)) return false;
			Value = std::bit_cast<float>(Bits);
			return std::isfinite(Value);
		}

		size_t MeshBytes(const ImportedGltfMesh& Mesh) noexcept
		{
			if (Mesh.Vertices.size() > (std::numeric_limits<size_t>::max)() / sizeof(GltfMeshVertex))
				return (std::numeric_limits<size_t>::max)();
			const size_t Vertices = Mesh.Vertices.size() * sizeof(GltfMeshVertex);
			if (Mesh.Indices.size() > ((std::numeric_limits<size_t>::max)() - Vertices) / sizeof(uint32_t))
				return (std::numeric_limits<size_t>::max)();
			return Vertices + Mesh.Indices.size() * sizeof(uint32_t);
		}

		std::expected<std::vector<std::byte>, std::string> EncodeMesh(const ImportedGltfMesh& Mesh)
		{
			if (Mesh.Name.size() > 4096 || Mesh.Vertices.empty() || Mesh.Vertices.size() > MaximumMeshVertices ||
				Mesh.Indices.empty() || Mesh.Indices.size() > MaximumMeshIndices || Mesh.Indices.size() % 3 != 0)
				return std::unexpected("Mesh product exceeds artifact bounds");
			const uint64_t Required = 20ull + Mesh.Name.size() + static_cast<uint64_t>(Mesh.Vertices.size()) * 64ull +
				static_cast<uint64_t>(Mesh.Indices.size()) * 4ull;
			if (Required > MaximumArtifactBytes) return std::unexpected("Mesh product exceeds artifact byte limit");
			std::vector<std::byte> Out;
			Out.reserve(static_cast<size_t>(Required));
			Out.insert(Out.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'M'}, std::byte{'S'} });
			AppendU32(Out, MeshArtifactVersion);
			AppendU32(Out, static_cast<uint32_t>(Mesh.Name.size()));
			for (unsigned char C : Mesh.Name) Out.push_back(static_cast<std::byte>(C));
			AppendU32(Out, static_cast<uint32_t>(Mesh.Vertices.size()));
			AppendU32(Out, static_cast<uint32_t>(Mesh.Indices.size()));
			for (const auto& V : Mesh.Vertices)
			{
				for (float X : V.Position) AppendFloat(Out, X);
				for (float X : V.Color) AppendFloat(Out, X);
				for (float X : V.TexCoord) AppendFloat(Out, X);
				for (float X : V.Normal) AppendFloat(Out, X);
				for (float X : V.Tangent) AppendFloat(Out, X);
			}
			for (uint32_t I : Mesh.Indices) AppendU32(Out, I);
			return Out;
		}

		std::expected<ImportedGltfMesh, std::string> DecodeMesh(std::span<const std::byte> In)
		{
			if (In.size() < 20 || In.size() > MaximumArtifactBytes || In[0] != std::byte{'P'} ||
				In[1] != std::byte{'F'} || In[2] != std::byte{'M'} || In[3] != std::byte{'S'})
				return std::unexpected("Mesh artifact has invalid magic or size");
			size_t At = 4;
			uint32_t Version = 0, NameSize = 0, VertexCount = 0, IndexCount = 0;
			if (!ReadU32(In, At, Version) || Version != MeshArtifactVersion || !ReadU32(In, At, NameSize) ||
				NameSize > 4096 || NameSize > In.size() - At)
				return std::unexpected("Mesh artifact schema or name is invalid");
			ImportedGltfMesh Mesh;
			Mesh.Name.assign(reinterpret_cast<const char*>(In.data() + At), NameSize);
			At += NameSize;
			if (!ReadU32(In, At, VertexCount) || !ReadU32(In, At, IndexCount) || VertexCount == 0 ||
				VertexCount > MaximumMeshVertices || IndexCount == 0 || IndexCount > MaximumMeshIndices || IndexCount % 3 != 0)
				return std::unexpected("Mesh artifact geometry counts are invalid");
			const uint64_t Payload = static_cast<uint64_t>(VertexCount) * 64ull + static_cast<uint64_t>(IndexCount) * 4ull;
			if (Payload != In.size() - At) return std::unexpected("Mesh artifact geometry length is inconsistent");
			Mesh.Vertices.resize(VertexCount);
			for (auto& V : Mesh.Vertices)
			{
				for (float& X : V.Position) if (!ReadFloat(In, At, X)) return std::unexpected("Mesh position is non-finite");
				for (float& X : V.Color) if (!ReadFloat(In, At, X)) return std::unexpected("Mesh color is non-finite");
				for (float& X : V.TexCoord) if (!ReadFloat(In, At, X)) return std::unexpected("Mesh UV is non-finite");
				for (float& X : V.Normal) if (!ReadFloat(In, At, X)) return std::unexpected("Mesh normal is non-finite");
				for (float& X : V.Tangent) if (!ReadFloat(In, At, X)) return std::unexpected("Mesh tangent is non-finite");
			}
			Mesh.Indices.resize(IndexCount);
			for (uint32_t& I : Mesh.Indices)
				if (!ReadU32(In, At, I) || I >= VertexCount) return std::unexpected("Mesh index is out of range");
			return Mesh;
		}

		AssetImportRecipe MakeRecipe(const AssetImportObservation& O)
		{
			return { O.Asset, O.SubresourceKey, O.ImporterID, O.ImporterVersion, O.OutputSchemaVersion,
				O.EffectiveSettings, O.TargetID, O.Inputs, O.InputFingerprint };
		}

		bool SameRecipe(const AssetImportRecipe& A, const AssetImportRecipe& B)
		{
			return A.SourceAsset == B.SourceAsset && A.SubresourceKey == B.SubresourceKey && A.ImporterID == B.ImporterID &&
				A.ImporterVersion == B.ImporterVersion && A.OutputSchemaVersion == B.OutputSchemaVersion &&
				A.EffectiveSettings == B.EffectiveSettings && A.TargetID == B.TargetID && A.Inputs == B.Inputs &&
				A.InputFingerprint == B.InputFingerprint;
		}

		std::optional<DerivedDataArtifactKey> MeshKey(const AssetImportRecipe& R, const AssetID& Asset, std::string_view Settings)
		{
			if (R.SourceAsset != Asset || R.SubresourceKey != "mesh/0/primitive/0" ||
				R.ImporterID != "PulseForge.StaticGltfMeshImporter" || R.ImporterVersion != "1" || R.OutputSchemaVersion != 1 ||
				R.TargetID != "cpu-generic-v1" || R.EffectiveSettings != Settings)
				return std::nullopt;
			auto Fingerprint = ComputeAssetImportInputFingerprint(R);
			if (!Fingerprint || *Fingerprint != R.InputFingerprint) return std::nullopt;
			return DerivedDataArtifactKey{ "gltf-static-mesh", *Fingerprint, MeshArtifactVersion };
		}

		uint32_t FullMipCount(uint32_t W, uint32_t H)
		{
			uint32_t Count = 1;
			while (W > 1 || H > 1) { W = (std::max)(1u, W / 2); H = (std::max)(1u, H / 2); ++Count; }
			return Count;
		}

		size_t TextureBytes(const PreparedTextureCpuProduct& Data) noexcept
		{
			size_t Total = 0;
			for (const auto& Level : Data.MipLevels)
			{
				if (Level.Pixels.size() > (std::numeric_limits<size_t>::max)() - Total) return (std::numeric_limits<size_t>::max)();
				Total += Level.Pixels.size();
			}
			return Total;
		}

		std::expected<std::vector<std::byte>, std::string> ReadImage(const std::filesystem::path& Path)
		{
			std::error_code Ec;
			const auto Size = std::filesystem::file_size(Path, Ec);
			if (Ec || Size == 0 || Size > MaximumImageSourceBytes || Size > std::numeric_limits<size_t>::max() ||
				Size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				return std::unexpected("Image source is unavailable or exceeds its size limit");
			std::vector<std::byte> Bytes(static_cast<size_t>(Size));
			std::ifstream In(Path, std::ios::binary);
			if (!In.is_open()) return std::unexpected("Image source could not be opened");
			In.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!In || In.bad()) return std::unexpected("Image source could not be read completely");
			return Bytes;
		}

		std::array<uint64_t, 2> ImporterFingerprint(std::span<const std::byte> Bytes)
		{
			uint64_t A = 14695981039346656037ull, B = 7809847782465536322ull;
			for (std::byte Byte : Bytes)
			{
				const uint8_t X = std::to_integer<uint8_t>(Byte);
				A = (A ^ X) * 1099511628211ull;
				B = (B ^ (X + 0x9du)) * 14029467366897019727ull;
				B ^= B >> 29;
			}
			return { A, B };
		}

		DerivedDataArtifactKey TextureKey(const AssetContentFingerprint& Source, ImageColorSpace ColorSpace)
		{
			AssetFingerprintBuilder B("PulseForge.TextureMipProduct.v1");
			B.UpdateText("sourceAlgorithm", Source.Algorithm);
			B.UpdateText("sourceDigest", Source.DigestHex);
			B.UpdateText("decoder", "PulseForge.ImageRGBA8Importer.v1");
			B.UpdateText("mipAlgorithm", "box-filter-srgb-linear-v1");
			B.UpdateText("format", std::to_string(static_cast<uint8_t>(ColorSpace)));
			return { "image-rgba8-mips", B.Finish(), TextureArtifactVersion };
		}

		float SrgbToLinear(uint8_t V)
		{
			const float E = static_cast<float>(V) / 255.0f;
			return E <= 0.04045f ? E / 12.92f : std::pow((E + 0.055f) / 1.055f, 2.4f);
		}

		uint8_t LinearToSrgb(float V)
		{
			V = std::clamp(V, 0.0f, 1.0f);
			const float E = V <= 0.0031308f ? V * 12.92f : 1.055f * std::pow(V, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(E * 255.0f));
		}

		std::vector<PreparedMipLevel> MakeMips(uint32_t W, uint32_t H, std::span<const uint8_t> Pixels, bool Srgb)
		{
			std::vector<PreparedMipLevel> Levels;
			Levels.push_back({ W, H, { Pixels.begin(), Pixels.end() } });
			while (W > 1 || H > 1)
			{
				const auto& Previous = Levels.back();
				const uint32_t NW = (std::max)(1u, W / 2), NH = (std::max)(1u, H / 2);
				PreparedMipLevel Next{ NW, NH, std::vector<uint8_t>(static_cast<size_t>(NW) * NH * 4) };
				for (uint32_t Y = 0; Y < NH; ++Y)
					for (uint32_t X = 0; X < NW; ++X)
					{
						const uint32_t X0 = static_cast<uint32_t>((static_cast<uint64_t>(X) * W) / NW);
						const uint32_t X1 = static_cast<uint32_t>((static_cast<uint64_t>(X + 1) * W) / NW);
						const uint32_t Y0 = static_cast<uint32_t>((static_cast<uint64_t>(Y) * H) / NH);
						const uint32_t Y1 = static_cast<uint32_t>((static_cast<uint64_t>(Y + 1) * H) / NH);
						std::array<float, 4> Sum{}; uint32_t N = 0;
						for (uint32_t SY = Y0; SY < Y1; ++SY)
							for (uint32_t SX = X0; SX < X1; ++SX)
							{
								const size_t At = (static_cast<size_t>(SY) * W + SX) * 4;
								for (size_t C = 0; C < 4; ++C)
									Sum[C] += Srgb && C < 3 ? SrgbToLinear(Previous.Pixels[At + C]) : Previous.Pixels[At + C] / 255.0f;
								++N;
							}
						const size_t Dst = (static_cast<size_t>(Y) * NW + X) * 4;
						for (size_t C = 0; C < 4; ++C)
						{
							const float Avg = Sum[C] / static_cast<float>(N);
							Next.Pixels[Dst + C] = Srgb && C < 3 ? LinearToSrgb(Avg) : static_cast<uint8_t>(std::lround(Avg * 255.0f));
						}
					}
				Levels.push_back(std::move(Next)); W = NW; H = NH;
			}
			return Levels;
		}

		std::expected<std::vector<std::byte>, std::string> EncodeTexture(const PreparedTextureCpuProduct& D)
		{
			if (D.Width == 0 || D.Height == 0 || D.Width > 32768 || D.Height > 32768 ||
				static_cast<uint64_t>(D.Width) * D.Height > MaximumImagePixels ||
				(D.ColorSpace != ImageColorSpace::Linear && D.ColorSpace != ImageColorSpace::Srgb) ||
				D.MipLevels.size() != FullMipCount(D.Width, D.Height) || TextureBytes(D) > MaximumArtifactBytes - 256)
				return std::unexpected("Texture product exceeds artifact bounds");
			std::vector<std::byte> Out; Out.reserve(24 + TextureBytes(D) + D.MipLevels.size() * 12);
			Out.insert(Out.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'T'}, std::byte{'X'} });
			AppendU32(Out, TextureArtifactVersion); AppendU32(Out, static_cast<uint32_t>(D.ColorSpace));
			AppendU32(Out, D.Width); AppendU32(Out, D.Height); AppendU32(Out, static_cast<uint32_t>(D.MipLevels.size()));
			uint32_t W = D.Width, H = D.Height;
			for (const auto& M : D.MipLevels)
			{
				const uint64_t Expected = static_cast<uint64_t>(W) * H * 4;
				if (M.Width != W || M.Height != H || M.Pixels.size() != Expected) return std::unexpected("Texture mip layout is inconsistent");
				AppendU32(Out, W); AppendU32(Out, H); AppendU32(Out, static_cast<uint32_t>(M.Pixels.size()));
				for (uint8_t X : M.Pixels) Out.push_back(static_cast<std::byte>(X));
				W = (std::max)(1u, W / 2); H = (std::max)(1u, H / 2);
			}
			if (W != 1 || H != 1 || Out.size() > MaximumArtifactBytes) return std::unexpected("Texture mip chain is incomplete");
			return Out;
		}

		std::expected<PreparedTextureCpuProduct, std::string> DecodeTexture(std::span<const std::byte> In, ImageColorSpace ColorSpace)
		{
			if (In.size() < 24 || In.size() > MaximumArtifactBytes || In[0] != std::byte{'P'} || In[1] != std::byte{'F'} ||
				In[2] != std::byte{'T'} || In[3] != std::byte{'X'}) return std::unexpected("Texture artifact magic or size is invalid");
			size_t At = 4; uint32_t Version = 0, F = 0, Count = 0; PreparedTextureCpuProduct D;
			if (!ReadU32(In, At, Version) || Version != TextureArtifactVersion || !ReadU32(In, At, F) ||
				F != static_cast<uint32_t>(ColorSpace) || !ReadU32(In, At, D.Width) || !ReadU32(In, At, D.Height) ||
				!ReadU32(In, At, Count) || D.Width == 0 || D.Height == 0 || D.Width > 32768 || D.Height > 32768 ||
				static_cast<uint64_t>(D.Width) * D.Height > MaximumImagePixels || Count != FullMipCount(D.Width, D.Height))
				return std::unexpected("Texture artifact schema, format or dimensions are invalid");
			D.ColorSpace = ColorSpace; uint32_t W = D.Width, H = D.Height; D.MipLevels.reserve(Count);
			for (uint32_t I = 0; I < Count; ++I)
			{
				uint32_t MW = 0, MH = 0, Bytes = 0; const uint64_t Expected = static_cast<uint64_t>(W) * H * 4;
				if (!ReadU32(In, At, MW) || !ReadU32(In, At, MH) || !ReadU32(In, At, Bytes) || MW != W || MH != H ||
					Bytes != Expected || At > In.size() || In.size() - At < Bytes) return std::unexpected("Texture artifact mip is malformed");
				PreparedMipLevel M{ W, H, std::vector<uint8_t>(Bytes) };
				for (uint8_t& X : M.Pixels) X = std::to_integer<uint8_t>(In[At++]);
				D.MipLevels.push_back(std::move(M)); W = (std::max)(1u, W / 2); H = (std::max)(1u, H / 2);
			}
			if (W != 1 || H != 1 || At != In.size()) return std::unexpected("Texture artifact has trailing or incomplete data");
			return D;
		}
	}

	std::expected<AssetCpuProductResult<ImportedGltfMesh>, std::string> PrepareMeshCpuProduct(
		const AssetID& Asset, const std::filesystem::path& Root, const AssetRegistry& Registry, std::string_view Settings,
		DerivedDataArtifactStore* Store, std::stop_token Stop, MeshProcessor Processor)
	{
		const auto LookupStart = std::chrono::steady_clock::now();
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled before inspection");
		std::optional<AssetImportRecipe> Observed;
		std::optional<DerivedDataArtifactKey> Key;
		std::vector<AssetDependencyInputReceipt> ReadSet;
		if (Store)
		{
			auto Inspection = AssetDependencyExtractor::ExtractAsset(Root, Asset, Registry);
			if (Inspection)
			{
				auto O = std::find_if(Inspection->ImportObservations.begin(), Inspection->ImportObservations.end(),
					[](const auto& X) { return X.SubresourceKey == "mesh/0/primitive/0"; });
				auto S = std::find_if(Inspection->Subresources.begin(), Inspection->Subresources.end(),
					[](const auto& X) { return X.Identity.SubresourceKey == "mesh/0/primitive/0"; });
				if (O != Inspection->ImportObservations.end() && S != Inspection->Subresources.end() &&
					(S->Classification == AssetSubresourceClassification::SupportedCandidate || S->Classification == AssetSubresourceClassification::Imported) &&
					O->EffectiveSettings == Settings)
				{
					Observed = MakeRecipe(*O); Key = MeshKey(*Observed, Asset, Settings); ReadSet = std::move(Inspection->ReadSet);
					if (Key)
					{
						auto Cached = Store->Read(*Key);
						if (Cached.Status == DerivedDataArtifactReadStatus::Hit)
							if (auto Product = DecodeMesh(Cached.Payload); Product && AssetDependencyExtractor::ValidateInputReceipts(Root, ReadSet))
							{
								const auto LookupEnd = std::chrono::steady_clock::now();
								AssetCpuProductResult<ImportedGltfMesh> Result{ std::move(*Product), AssetCpuProductCacheOutcome::ArtifactHit,
									static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count()), 0, 0 };
								Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
								return Result;
							}
					}
				}
			}
		}
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled before import");
		if (!Processor)
			Processor = [](const AssetID& ID, const std::filesystem::path& Project, const AssetRegistry& Assets)
				-> std::expected<GltfMeshImportProduct, std::string>
			{
				auto Product = GltfMeshImporter::ProcessStaticPrimitive(ID, Project, Assets);
				if (!Product) return std::unexpected(Product.error().Message);
				return std::move(*Product);
			};
		const auto LookupEnd = std::chrono::steady_clock::now();
		const uint64_t LookupMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count());
		const auto ProcessStart = std::chrono::steady_clock::now();
		auto Product = Processor(Asset, Root, Registry);
		const auto ProcessEnd = std::chrono::steady_clock::now();
		const uint64_t ProcessMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(ProcessEnd - ProcessStart).count());
		if (!Product) return std::unexpected(Product.error());
		if (Product->Recipe.EffectiveSettings != Settings) return std::unexpected("Mesh import settings changed during preparation");
		if (Stop.stop_requested()) return std::unexpected("Mesh preparation cancelled after import");
		AssetCpuProductCacheOutcome Outcome = AssetCpuProductCacheOutcome::ProcessedStoreUnavailable;
		uint64_t WriteMicroseconds = 0;
		auto WriteStart = std::chrono::steady_clock::time_point{};
		auto WriteEnd = WriteStart;
		if (Store && Observed && Key && SameRecipe(Product->Recipe, *Observed) &&
			AssetDependencyExtractor::ValidateInputReceipts(Root, ReadSet))
		{
			WriteStart = std::chrono::steady_clock::now();
			if (auto Bytes = EncodeMesh(Product->Mesh); Bytes)
			{
				const auto Write = Store->Write(*Key, *Bytes);
				if (Write.Status == DerivedDataArtifactWriteStatus::Written || Write.Status == DerivedDataArtifactWriteStatus::ExistingValid)
					Outcome = AssetCpuProductCacheOutcome::ProcessedAndStored;
			}
			WriteEnd = std::chrono::steady_clock::now();
			WriteMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(WriteEnd - WriteStart).count());
		}
		AssetCpuProductResult<ImportedGltfMesh> Result{ std::move(Product->Mesh), Outcome,
			LookupMicroseconds, ProcessMicroseconds, WriteMicroseconds };
		Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
		Result.Processing = MakeTiming(ProcessStart, ProcessEnd);
		if (WriteEnd > WriteStart) Result.CacheWrite = MakeTiming(WriteStart, WriteEnd);
		return Result;
	}

	std::expected<AssetCpuProductResult<PreparedTextureCpuProduct>, std::string> PrepareTextureCpuProduct(
		const AssetID& Asset, const std::filesystem::path& Root, const AssetRegistry& Registry, ImageColorSpace ColorSpace,
		DerivedDataArtifactStore* Store, std::stop_token Stop, ImageProcessor Processor)
	{
		const auto LookupStart = std::chrono::steady_clock::now();
		if (ColorSpace != ImageColorSpace::Linear && ColorSpace != ImageColorSpace::Srgb) return std::unexpected("Unsupported image color space");
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before source read");
		auto Record = Registry.Find(Asset);
		if (!Record) return std::unexpected("Image asset is absent from captured registry");
		auto Resolved = AssetPathResolver::ResolveManagedSourcePath(Root, Record->ProjectRelativePath);
		if (!Resolved) return std::unexpected(Resolved.error().Message);
		auto Source = ReadImage(*Resolved);
		if (!Source) return std::unexpected(Source.error());
		const AssetContentFingerprint Fingerprint = ComputeAssetContentFingerprint(*Source);
		const auto RawFingerprint = ImporterFingerprint(*Source);
		const DerivedDataArtifactKey Key = TextureKey(Fingerprint, ColorSpace);
		std::vector<std::byte>().swap(*Source);
		if (Store)
		{
			auto Cached = Store->Read(Key);
			if (Cached.Status == DerivedDataArtifactReadStatus::Hit)
				if (auto Product = DecodeTexture(Cached.Payload, ColorSpace))
				{
					auto Current = ReadImage(*Resolved);
					if (!Current || ComputeAssetContentFingerprint(*Current) != Fingerprint) return std::unexpected("Image source changed during cache lookup");
					const auto LookupEnd = std::chrono::steady_clock::now();
					AssetCpuProductResult<PreparedTextureCpuProduct> Result{ std::move(*Product), AssetCpuProductCacheOutcome::ArtifactHit,
						static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count()), 0, 0 };
					Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
					return Result;
				}
		}
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled before decode");
		if (!Processor)
			Processor = [](const AssetID& ID, const std::filesystem::path& Project, const AssetRegistry& Assets)
				-> std::expected<ImportedImage, std::string>
			{
				auto Image = ImageAssetImporter::ImportRGBA8(ID, Project, Assets);
				if (!Image) return std::unexpected(Image.error().Message);
				return std::move(*Image);
			};
		const auto LookupEnd = std::chrono::steady_clock::now();
		const uint64_t LookupMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(LookupEnd - LookupStart).count());
		const auto ProcessStart = std::chrono::steady_clock::now();
		auto Image = Processor(Asset, Root, Registry);
		if (!Image) return std::unexpected(Image.error());
		if (Image->SourceFingerprint != RawFingerprint) return std::unexpected("Image changed between key observation and decoding");
		PreparedTextureCpuProduct Product;
		Product.Width = Image->Width; Product.Height = Image->Height; Product.ColorSpace = ColorSpace;
		Product.MipLevels = MakeMips(Image->Width, Image->Height, Image->RGBA8Pixels, ColorSpace == ImageColorSpace::Srgb);
		const auto ProcessEnd = std::chrono::steady_clock::now();
		const uint64_t ProcessMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(ProcessEnd - ProcessStart).count());
		if (Stop.stop_requested()) return std::unexpected("Texture preparation cancelled after mip processing");
		auto Current = ReadImage(*Resolved);
		if (!Current || ComputeAssetContentFingerprint(*Current) != Fingerprint) return std::unexpected("Image source changed during CPU processing");
		AssetCpuProductCacheOutcome Outcome = AssetCpuProductCacheOutcome::ProcessedStoreUnavailable;
		uint64_t WriteMicroseconds = 0;
		auto WriteStart = std::chrono::steady_clock::time_point{};
		auto WriteEnd = WriteStart;
		if (Store)
		{
			WriteStart = std::chrono::steady_clock::now();
			if (auto Bytes = EncodeTexture(Product); Bytes)
			{
				const auto Write = Store->Write(Key, *Bytes);
				if (Write.Status == DerivedDataArtifactWriteStatus::Written || Write.Status == DerivedDataArtifactWriteStatus::ExistingValid)
					Outcome = AssetCpuProductCacheOutcome::ProcessedAndStored;
			}
			WriteEnd = std::chrono::steady_clock::now();
			WriteMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(WriteEnd - WriteStart).count());
		}
		AssetCpuProductResult<PreparedTextureCpuProduct> Result{ std::move(Product), Outcome,
			LookupMicroseconds, ProcessMicroseconds, WriteMicroseconds };
		Result.ObservationAndLookup = MakeTiming(LookupStart, LookupEnd);
		Result.Processing = MakeTiming(ProcessStart, ProcessEnd);
		if (WriteEnd > WriteStart) Result.CacheWrite = MakeTiming(WriteStart, WriteEnd);
		return Result;
	}
} // namespace PulseForge::Detail
