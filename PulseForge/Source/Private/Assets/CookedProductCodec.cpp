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
		constexpr uint32_t GraphMaterialCodecVersion = Detail::GraphMaterialProductSchemaVersion;
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

		void AppendString(std::vector<std::byte>& Output, std::string_view Value)
		{
			AppendU32(Output, static_cast<uint32_t>(Value.size()));
			for (const unsigned char Character : Value) Output.push_back(static_cast<std::byte>(Character));
		}

		bool ReadString(std::span<const std::byte> Input, size_t& Offset, std::string& Value, size_t MaximumLength)
		{
			uint32_t Length = 0;
			if (!ReadU32(Input, Offset, Length) || Length > MaximumLength || Offset > Input.size() || Length > Input.size() - Offset)
				return false;
			Value.assign(reinterpret_cast<const char*>(Input.data() + Offset), Length);
			Offset += Length;
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

		bool AppendLevelFloats(std::vector<std::byte>& Payload, const FloatCubeLevel& Level, uint32_t ExpectedSize,
			std::stop_token CancellationToken)
		{
			const uint64_t FloatCount = static_cast<uint64_t>(ExpectedSize) * ExpectedSize * 4;
			if (Level.Size != ExpectedSize) return false;
			for (const auto& Face : Level.Faces)
			{
				if (Face.size() != FloatCount) return false;
				for (size_t Index = 0; Index < Face.size(); ++Index)
				{
					if ((Index & 0xfffu) == 0 && CancellationToken.stop_requested()) return false;
					if (!std::isfinite(Face[Index])) return false;
					AppendFloat(Payload, Face[Index]);
				}
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

	std::expected<std::vector<std::byte>, std::string> Detail::EncodeMeshProduct(
		const ImportedGltfMesh& Mesh, std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested()) return std::unexpected("Mesh product encoding cancelled");
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
		for (size_t VertexIndex = 0; VertexIndex < Mesh.Vertices.size(); ++VertexIndex)
		{
			if ((VertexIndex & 0x3ffu) == 0 && CancellationToken.stop_requested())
				return std::unexpected("Mesh product encoding cancelled");
			const auto& Vertex = Mesh.Vertices[VertexIndex];
			for (float Value : Vertex.Position) AppendFloat(Out, Value);
			for (float Value : Vertex.Color) AppendFloat(Out, Value);
			for (float Value : Vertex.TexCoord) AppendFloat(Out, Value);
			for (float Value : Vertex.Normal) AppendFloat(Out, Value);
			for (float Value : Vertex.Tangent) AppendFloat(Out, Value);
		}
		for (size_t Index = 0; Index < Mesh.Indices.size(); ++Index)
		{
			if ((Index & 0xfffu) == 0 && CancellationToken.stop_requested())
				return std::unexpected("Mesh product encoding cancelled");
			AppendU32(Out, Mesh.Indices[Index]);
		}
		if (CancellationToken.stop_requested()) return std::unexpected("Mesh product encoding cancelled");
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

	std::expected<std::vector<std::byte>, std::string> Detail::EncodeSkeletalMeshProduct(
		const ImportedGltfSkeletalMesh& Mesh, std::stop_token CancellationToken)
	{
		constexpr size_t MaximumSkeletonNodes = 4096;
		constexpr size_t MaximumJoints = 256;
		constexpr size_t MaximumClips = 256;
		constexpr size_t MaximumChannels = 4096;
		constexpr size_t MaximumKeys = 1'000'000;
		if (CancellationToken.stop_requested()) return std::unexpected("Skeletal mesh encoding cancelled");
		auto Geometry = EncodeMeshProduct(Mesh.Geometry, CancellationToken);
		if (!Geometry) return std::unexpected("Skeletal geometry is invalid: " + Geometry.error());
		if (Mesh.SkeletonNodes.empty() || Mesh.SkeletonNodes.size() > MaximumSkeletonNodes ||
			Mesh.JointNodeIndices.empty() || Mesh.JointNodeIndices.size() > MaximumJoints ||
			Mesh.InverseBindMatrices.size() != Mesh.JointNodeIndices.size() ||
			Mesh.VertexInfluences.size() != Mesh.Geometry.Vertices.size() || Mesh.Animations.size() > MaximumClips)
			return std::unexpected("Skeletal mesh structure exceeds schema bounds");

		std::vector<uint8_t> JointFlags(Mesh.SkeletonNodes.size(), 0);
		std::vector<std::string_view> StableKeys;
		StableKeys.reserve(Mesh.SkeletonNodes.size());
		for (size_t Index = 0; Index < Mesh.SkeletonNodes.size(); ++Index)
		{
			const ImportedGltfSkeletonNode& Node = Mesh.SkeletonNodes[Index];
			if (Node.StableKey.empty() || Node.StableKey.size() > 256 || Node.Name.empty() || Node.Name.size() > 4096 ||
				Node.ParentNodeIndex < -1 || Node.ParentNodeIndex >= static_cast<int32_t>(Index) ||
				std::find(StableKeys.begin(), StableKeys.end(), Node.StableKey) != StableKeys.end() ||
				!std::ranges::all_of(Node.LocalBindMatrix, [](float Value) { return std::isfinite(Value); }) ||
				std::abs(Node.LocalBindMatrix[3]) > 1.0e-5f || std::abs(Node.LocalBindMatrix[7]) > 1.0e-5f ||
				std::abs(Node.LocalBindMatrix[11]) > 1.0e-5f || std::abs(Node.LocalBindMatrix[15] - 1.0f) > 1.0e-5f)
				return std::unexpected("Skeleton node identity, hierarchy, or bind transform is invalid");
			StableKeys.push_back(Node.StableKey);
			if (Node.IsJoint) JointFlags[Index] = 1;
		}
		for (const uint16_t NodeIndex : Mesh.JointNodeIndices)
		{
			if (NodeIndex >= Mesh.SkeletonNodes.size() || !Mesh.SkeletonNodes[NodeIndex].IsJoint || JointFlags[NodeIndex] != 1)
				return std::unexpected("Skin joint mapping is invalid or duplicated");
			JointFlags[NodeIndex] = 2;
		}
		if (std::ranges::any_of(JointFlags, [](uint8_t Flag) { return Flag == 1; }))
			return std::unexpected("Skeleton joint flags do not match the skin joint mapping");
		for (const auto& Matrix : Mesh.InverseBindMatrices)
			if (!std::ranges::all_of(Matrix, [](float Value) { return std::isfinite(Value); }) ||
				std::abs(Matrix[3]) > 1.0e-5f || std::abs(Matrix[7]) > 1.0e-5f ||
				std::abs(Matrix[11]) > 1.0e-5f || std::abs(Matrix[15] - 1.0f) > 1.0e-5f)
				return std::unexpected("Inverse bind matrix is non-finite or non-affine");
		for (const GltfVertexInfluences& Influence : Mesh.VertexInfluences)
		{
			float WeightSum = 0.0f;
			for (size_t Component = 0; Component < 4; ++Component)
			{
				if (Influence.JointIndices[Component] >= Mesh.JointNodeIndices.size() ||
					!std::isfinite(Influence.Weights[Component]) || Influence.Weights[Component] < 0.0f ||
					Influence.Weights[Component] > 1.0f)
					return std::unexpected("Vertex joint influence is outside the skin or weight bounds");
				WeightSum += Influence.Weights[Component];
			}
			if (!std::isfinite(WeightSum) || std::abs(WeightSum - 1.0f) > 1.0e-4f)
				return std::unexpected("Vertex influence weights must be normalized");
		}
		uint64_t RequiredBytes = 16ull + Geometry->size();
		const auto AddRequiredBytes = [&](uint64_t Count)
		{
			if (Count > MaximumProductBytes || RequiredBytes > MaximumProductBytes - Count) return false;
			RequiredBytes += Count;
			return true;
		};
		for (const ImportedGltfSkeletonNode& Node : Mesh.SkeletonNodes)
			if (!AddRequiredBytes(4ull + Node.StableKey.size() + 4ull + Node.Name.size() + 4ull + 4ull + 64ull))
				return std::unexpected("Skeletal mesh product exceeds artifact byte limit");
		if (!AddRequiredBytes(4ull + Mesh.JointNodeIndices.size() * 68ull) ||
			!AddRequiredBytes(4ull + Mesh.VertexInfluences.size() * 32ull) || !AddRequiredBytes(4ull))
			return std::unexpected("Skeletal mesh product exceeds artifact byte limit");
		for (const ImportedGltfAnimationClip& Clip : Mesh.Animations)
		{
			if (Clip.Channels.size() > MaximumChannels || !AddRequiredBytes(4ull + Clip.StableKey.size() +
				4ull + Clip.Name.size() + 4ull + 4ull))
				return std::unexpected("Skeletal animation product exceeds artifact byte limit");
			for (const ImportedGltfAnimationChannel& Channel : Clip.Channels)
				if (Channel.Times.size() > MaximumKeys ||
					!AddRequiredBytes(4ull + Channel.TargetNodeKey.size() + 4ull + 4ull + 4ull + Channel.Times.size() * 20ull))
					return std::unexpected("Skeletal animation product exceeds artifact byte limit");
		}

		std::vector<std::byte> Output;
		Output.reserve(static_cast<size_t>(RequiredBytes));
		Output.insert(Output.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'S'}, std::byte{'K'} });
		AppendU32(Output, SkeletalMeshProductSchemaVersion);
		AppendU32(Output, static_cast<uint32_t>(Geometry->size()));
		Output.insert(Output.end(), Geometry->begin(), Geometry->end());
		AppendU32(Output, static_cast<uint32_t>(Mesh.SkeletonNodes.size()));
		for (const ImportedGltfSkeletonNode& Node : Mesh.SkeletonNodes)
		{
			if (CancellationToken.stop_requested()) return std::unexpected("Skeletal mesh encoding cancelled");
			AppendString(Output, Node.StableKey);
			AppendString(Output, Node.Name);
			AppendU32(Output, std::bit_cast<uint32_t>(Node.ParentNodeIndex));
			AppendU32(Output, Node.IsJoint ? 1u : 0u);
			for (const float Value : Node.LocalBindMatrix) AppendFloat(Output, Value);
		}
		AppendU32(Output, static_cast<uint32_t>(Mesh.JointNodeIndices.size()));
		for (const uint16_t Index : Mesh.JointNodeIndices) AppendU32(Output, Index);
		for (const auto& Matrix : Mesh.InverseBindMatrices)
			for (const float Value : Matrix) AppendFloat(Output, Value);
		AppendU32(Output, static_cast<uint32_t>(Mesh.VertexInfluences.size()));
		for (const GltfVertexInfluences& Influence : Mesh.VertexInfluences)
		{
			for (const uint16_t Index : Influence.JointIndices) AppendU32(Output, Index);
			for (const float Weight : Influence.Weights) AppendFloat(Output, Weight);
		}
		AppendU32(Output, static_cast<uint32_t>(Mesh.Animations.size()));
		size_t TotalChannels = 0;
		size_t TotalKeys = 0;
		for (const ImportedGltfAnimationClip& Clip : Mesh.Animations)
		{
			if (Clip.StableKey.empty() || Clip.StableKey.size() > 256 || Clip.Name.empty() || Clip.Name.size() > 4096 ||
				!std::isfinite(Clip.Duration) || Clip.Duration < 0.0f || Clip.Channels.empty() ||
				Clip.Channels.size() > MaximumChannels - TotalChannels)
				return std::unexpected("Animation clip identity or channel bounds are invalid");
			TotalChannels += Clip.Channels.size();
			AppendString(Output, Clip.StableKey);
			AppendString(Output, Clip.Name);
			AppendFloat(Output, Clip.Duration);
			AppendU32(Output, static_cast<uint32_t>(Clip.Channels.size()));
			std::vector<std::pair<std::string_view, GltfAnimationPath>> ChannelKeys;
			for (const ImportedGltfAnimationChannel& Channel : Clip.Channels)
			{
				const size_t Components = Channel.Path == GltfAnimationPath::Rotation ? 4 : 3;
				if (std::find(StableKeys.begin(), StableKeys.end(), Channel.TargetNodeKey) == StableKeys.end() ||
					(Channel.Path != GltfAnimationPath::Translation && Channel.Path != GltfAnimationPath::Rotation &&
						Channel.Path != GltfAnimationPath::Scale) ||
					(Channel.Interpolation != GltfAnimationInterpolation::Step && Channel.Interpolation != GltfAnimationInterpolation::Linear) ||
					Channel.Times.empty() || Channel.Times.size() != Channel.Values.size() ||
					Channel.Times.size() > MaximumKeys - TotalKeys ||
					std::find(ChannelKeys.begin(), ChannelKeys.end(), std::pair{ std::string_view(Channel.TargetNodeKey), Channel.Path }) != ChannelKeys.end())
					return std::unexpected("Animation channel identity, type, or key bounds are invalid");
				ChannelKeys.emplace_back(Channel.TargetNodeKey, Channel.Path);
				TotalKeys += Channel.Times.size();
				AppendString(Output, Channel.TargetNodeKey);
				AppendU32(Output, static_cast<uint32_t>(Channel.Path));
				AppendU32(Output, static_cast<uint32_t>(Channel.Interpolation));
				AppendU32(Output, static_cast<uint32_t>(Channel.Times.size()));
				float PreviousTime = -1.0f;
				for (size_t Key = 0; Key < Channel.Times.size(); ++Key)
				{
					const float Time = Channel.Times[Key];
					if (!std::isfinite(Time) || Time < 0.0f || Time <= PreviousTime || Time > Clip.Duration ||
						!std::ranges::all_of(Channel.Values[Key], [](float Value) { return std::isfinite(Value); }))
						return std::unexpected("Animation key is non-finite, unordered, or outside the clip duration");
					if (Components == 4)
					{
						const auto& Rotation = Channel.Values[Key];
						const float Length = std::sqrt(Rotation[0] * Rotation[0] + Rotation[1] * Rotation[1] +
							Rotation[2] * Rotation[2] + Rotation[3] * Rotation[3]);
						if (!std::isfinite(Length) || std::abs(Length - 1.0f) > 1.0e-3f)
							return std::unexpected("Animation rotation keys must be normalized quaternions");
					}
					AppendFloat(Output, Time);
					for (const float Value : Channel.Values[Key]) AppendFloat(Output, Value);
					PreviousTime = Time;
				}
			}
		}
		if (Output.size() > MaximumProductBytes) return std::unexpected("Skeletal mesh product exceeds artifact byte limit");
		return Output;
	}

	std::expected<ImportedGltfSkeletalMesh, std::string> Detail::DecodeSkeletalMeshProduct(std::span<const std::byte> Input)
	{
		constexpr size_t MaximumSkeletonNodes = 4096;
		constexpr size_t MaximumJoints = 256;
		constexpr size_t MaximumClips = 256;
		constexpr size_t MaximumChannels = 4096;
		constexpr size_t MaximumKeys = 1'000'000;
		if (Input.size() < 16 || Input.size() > MaximumProductBytes || Input[0] != std::byte{'P'} ||
			Input[1] != std::byte{'F'} || Input[2] != std::byte{'S'} || Input[3] != std::byte{'K'})
			return std::unexpected("Skeletal mesh artifact has invalid magic or size");
		try
		{
		size_t Offset = 4;
		uint32_t Version = 0, GeometrySize = 0;
		if (!ReadU32(Input, Offset, Version) || Version != SkeletalMeshProductSchemaVersion ||
			!ReadU32(Input, Offset, GeometrySize) || GeometrySize > Input.size() - Offset)
			return std::unexpected("Skeletal mesh artifact schema or geometry length is invalid");
		auto Geometry = DecodeMeshProduct(Input.subspan(Offset, GeometrySize));
		if (!Geometry) return std::unexpected("Skeletal geometry is invalid: " + Geometry.error());
		Offset += GeometrySize;
		ImportedGltfSkeletalMesh Mesh;
		Mesh.Geometry = std::move(*Geometry);
		uint32_t NodeCount = 0;
		if (!ReadU32(Input, Offset, NodeCount) || NodeCount == 0 || NodeCount > MaximumSkeletonNodes ||
			NodeCount > (Input.size() - Offset) / 80)
			return std::unexpected("Skeleton node count exceeds schema bounds");
		try { Mesh.SkeletonNodes.resize(NodeCount); }
		catch (const std::bad_alloc&) { return std::unexpected("Insufficient memory decoding skeleton nodes"); }
		std::vector<std::string_view> StableKeys;
		StableKeys.reserve(NodeCount);
		for (size_t Index = 0; Index < NodeCount; ++Index)
		{
			ImportedGltfSkeletonNode& Node = Mesh.SkeletonNodes[Index];
			uint32_t ParentBits = 0, IsJoint = 0;
			if (!ReadString(Input, Offset, Node.StableKey, 256) || Node.StableKey.empty() ||
				!ReadString(Input, Offset, Node.Name, 4096) || Node.Name.empty() ||
				std::find(StableKeys.begin(), StableKeys.end(), Node.StableKey) != StableKeys.end() ||
				!ReadU32(Input, Offset, ParentBits) || !ReadU32(Input, Offset, IsJoint) || IsJoint > 1)
				return std::unexpected("Skeleton node identity or fields are invalid");
			Node.ParentNodeIndex = std::bit_cast<int32_t>(ParentBits);
			Node.IsJoint = IsJoint != 0;
			if (Node.ParentNodeIndex < -1 || Node.ParentNodeIndex >= static_cast<int32_t>(Index))
				return std::unexpected("Skeleton parent indices are invalid");
			for (float& Value : Node.LocalBindMatrix)
				if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Skeleton bind transform is non-finite or truncated");
			if (std::abs(Node.LocalBindMatrix[3]) > 1.0e-5f || std::abs(Node.LocalBindMatrix[7]) > 1.0e-5f ||
				std::abs(Node.LocalBindMatrix[11]) > 1.0e-5f || std::abs(Node.LocalBindMatrix[15] - 1.0f) > 1.0e-5f)
				return std::unexpected("Skeleton bind transform is non-affine");
			StableKeys.push_back(Node.StableKey);
		}
		uint32_t JointCount = 0;
		if (!ReadU32(Input, Offset, JointCount) || JointCount == 0 || JointCount > MaximumJoints ||
			JointCount > (Input.size() - Offset) / 68)
			return std::unexpected("Skin joint count exceeds schema bounds");
		std::vector<uint8_t> JointFlags(NodeCount, 0);
		Mesh.JointNodeIndices.resize(JointCount);
		for (uint16_t& Index : Mesh.JointNodeIndices)
		{
			uint32_t Value = 0;
			if (!ReadU32(Input, Offset, Value) || Value >= NodeCount || !Mesh.SkeletonNodes[Value].IsJoint || JointFlags[Value] != 0)
				return std::unexpected("Skin joint mapping is invalid");
			Index = static_cast<uint16_t>(Value);
			JointFlags[Value] = 1;
		}
		for (size_t Index = 0; Index < NodeCount; ++Index)
			if (Mesh.SkeletonNodes[Index].IsJoint != (JointFlags[Index] != 0))
				return std::unexpected("Skeleton joint flags do not match the skin mapping");
		Mesh.InverseBindMatrices.resize(JointCount);
		for (auto& Matrix : Mesh.InverseBindMatrices)
		{
			for (float& Value : Matrix)
				if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Inverse bind matrix is non-finite or truncated");
			if (std::abs(Matrix[3]) > 1.0e-5f || std::abs(Matrix[7]) > 1.0e-5f ||
				std::abs(Matrix[11]) > 1.0e-5f || std::abs(Matrix[15] - 1.0f) > 1.0e-5f)
				return std::unexpected("Inverse bind matrix is non-affine");
		}
		uint32_t InfluenceCount = 0;
		if (!ReadU32(Input, Offset, InfluenceCount) || InfluenceCount != Mesh.Geometry.Vertices.size() ||
			InfluenceCount > (Input.size() - Offset) / 32)
			return std::unexpected("Skeletal influence count does not match geometry");
		Mesh.VertexInfluences.resize(InfluenceCount);
		for (GltfVertexInfluences& Influence : Mesh.VertexInfluences)
		{
			float Sum = 0.0f;
			for (uint16_t& Index : Influence.JointIndices)
			{
				uint32_t Value = 0;
				if (!ReadU32(Input, Offset, Value) || Value >= JointCount)
					return std::unexpected("Skeletal vertex joint index is invalid");
				Index = static_cast<uint16_t>(Value);
			}
			for (float& Weight : Influence.Weights)
			{
				if (!ReadFloat(Input, Offset, Weight) || Weight < 0.0f || Weight > 1.0f)
					return std::unexpected("Skeletal vertex weight is invalid");
				Sum += Weight;
			}
			if (!std::isfinite(Sum) || std::abs(Sum - 1.0f) > 1.0e-4f)
				return std::unexpected("Skeletal vertex weights are not normalized");
		}
		uint32_t ClipCount = 0;
		if (!ReadU32(Input, Offset, ClipCount) || ClipCount > MaximumClips)
			return std::unexpected("Animation clip count exceeds schema bounds");
		Mesh.Animations.resize(ClipCount);
		size_t TotalChannels = 0, TotalKeys = 0;
		for (ImportedGltfAnimationClip& Clip : Mesh.Animations)
		{
			uint32_t ChannelCount = 0;
			if (!ReadString(Input, Offset, Clip.StableKey, 256) || Clip.StableKey.empty() ||
				!ReadString(Input, Offset, Clip.Name, 4096) || Clip.Name.empty() ||
				!ReadFloat(Input, Offset, Clip.Duration) || Clip.Duration < 0.0f || !ReadU32(Input, Offset, ChannelCount) ||
				ChannelCount == 0 || ChannelCount > MaximumChannels - TotalChannels ||
				ChannelCount > (Input.size() - Offset) / 20)
				return std::unexpected("Animation clip fields or channel count are invalid");
			TotalChannels += ChannelCount;
			Clip.Channels.resize(ChannelCount);
			std::vector<std::pair<std::string_view, GltfAnimationPath>> ChannelKeys;
			for (ImportedGltfAnimationChannel& Channel : Clip.Channels)
			{
				uint32_t Path = 0, Interpolation = 0, KeyCount = 0;
				if (!ReadString(Input, Offset, Channel.TargetNodeKey, 256) || Channel.TargetNodeKey.empty() ||
					std::find(StableKeys.begin(), StableKeys.end(), Channel.TargetNodeKey) == StableKeys.end() ||
					!ReadU32(Input, Offset, Path) || Path > static_cast<uint32_t>(GltfAnimationPath::Scale) ||
					!ReadU32(Input, Offset, Interpolation) || Interpolation > static_cast<uint32_t>(GltfAnimationInterpolation::Linear) ||
					!ReadU32(Input, Offset, KeyCount) || KeyCount == 0 || KeyCount > MaximumKeys - TotalKeys ||
					KeyCount > (Input.size() - Offset) / 20)
					return std::unexpected("Animation channel fields or key count are invalid");
				Channel.Path = static_cast<GltfAnimationPath>(Path);
				Channel.Interpolation = static_cast<GltfAnimationInterpolation>(Interpolation);
				if (std::find(ChannelKeys.begin(), ChannelKeys.end(), std::pair{ std::string_view(Channel.TargetNodeKey), Channel.Path }) != ChannelKeys.end())
					return std::unexpected("Animation channel target is duplicated");
				ChannelKeys.emplace_back(Channel.TargetNodeKey, Channel.Path);
				TotalKeys += KeyCount;
				Channel.Times.resize(KeyCount);
				Channel.Values.resize(KeyCount);
				float PreviousTime = -1.0f;
				for (size_t Key = 0; Key < KeyCount; ++Key)
				{
					float& Time = Channel.Times[Key];
					if (!ReadFloat(Input, Offset, Time) || Time < 0.0f || Time <= PreviousTime || Time > Clip.Duration)
						return std::unexpected("Animation key times are invalid");
					for (float& Value : Channel.Values[Key])
						if (!ReadFloat(Input, Offset, Value)) return std::unexpected("Animation key value is non-finite or truncated");
					if (Channel.Path == GltfAnimationPath::Rotation)
					{
						const auto& Rotation = Channel.Values[Key];
						const float Length = std::sqrt(Rotation[0] * Rotation[0] + Rotation[1] * Rotation[1] +
							Rotation[2] * Rotation[2] + Rotation[3] * Rotation[3]);
						if (!std::isfinite(Length) || std::abs(Length - 1.0f) > 1.0e-3f)
							return std::unexpected("Animation rotation key is not normalized");
					}
					PreviousTime = Time;
				}
			}
		}
		if (Offset != Input.size()) return std::unexpected("Skeletal mesh artifact has trailing data");
		return Mesh;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected("Insufficient memory decoding skeletal mesh artifact");
		}
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EncodeTextureProduct(
		const PreparedTextureCpuProduct& Texture, std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested()) return std::unexpected("Texture product encoding cancelled");
		uint64_t TotalPixels = 0;
		for (const auto& Mip : Texture.MipLevels)
		{
			if (CancellationToken.stop_requested()) return std::unexpected("Texture product encoding cancelled");
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
			if (CancellationToken.stop_requested()) return std::unexpected("Texture product encoding cancelled");
			const uint64_t Expected = static_cast<uint64_t>(Width) * Height * 4;
			if (Mip.Width != Width || Mip.Height != Height || Mip.Pixels.size() != Expected)
				return std::unexpected("Texture mip layout is inconsistent");
			AppendU32(Output, Width);
			AppendU32(Output, Height);
			AppendU32(Output, static_cast<uint32_t>(Mip.Pixels.size()));
			for (size_t Index = 0; Index < Mip.Pixels.size(); ++Index)
			{
				if ((Index & 0xffffu) == 0 && CancellationToken.stop_requested())
					return std::unexpected("Texture product encoding cancelled");
				Output.push_back(static_cast<std::byte>(Mip.Pixels[Index]));
			}
			Width = std::max(1u, Width / 2);
			Height = std::max(1u, Height / 2);
		}
		if (Width != 1 || Height != 1 || Output.size() > MaximumProductBytes)
			return std::unexpected("Texture mip chain is incomplete");
		if (CancellationToken.stop_requested()) return std::unexpected("Texture product encoding cancelled");
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

	std::expected<std::vector<std::byte>, std::string> Detail::EncodeGraphMaterialProduct(
		const CookedGraphMaterialProduct& Material)
	{
		if (Material.ArtifactVersion != GraphMaterialCodecVersion || Material.RecipeFingerprint.size() != 32 ||
			Material.SpirV.size() < 20 || Material.SpirV.size() > 16u * 1024u * 1024u || Material.SpirV.size() % 4 != 0 ||
			Material.Parameters.size() > 256 ||
			Material.SpirV[0] != std::byte{ 0x03 } || Material.SpirV[1] != std::byte{ 0x02 } ||
			Material.SpirV[2] != std::byte{ 0x23 } || Material.SpirV[3] != std::byte{ 0x07 })
			return std::unexpected("Graph material product has invalid version, recipe, or SPIR-V data");
		auto MaterialState = Detail::EncodeMaterialProduct(Material.Material);
		if (!MaterialState) return std::unexpected("Graph material render state is invalid: " + MaterialState.error());
		std::vector<std::byte> Output;
		Output.reserve(52 + Material.SpirV.size() + Material.Parameters.size() * 80 + MaterialState->size());
		Output.insert(Output.end(), { std::byte{'P'}, std::byte{'F'}, std::byte{'G'}, std::byte{'M'} });
		AppendU32(Output, Material.ArtifactVersion);
		AppendU32(Output, static_cast<uint32_t>(Material.SpirV.size()));
		for (const char C : Material.RecipeFingerprint) Output.push_back(static_cast<std::byte>(C));
		AppendU32(Output, static_cast<uint32_t>(Material.Parameters.size()));
		for (size_t Index = 0; Index < Material.Parameters.size(); ++Index)
		{
			const auto& Parameter = Material.Parameters[Index];
			const bool ValidName = !Parameter.Name.empty() && Parameter.Name.size() <= 64 &&
				((Parameter.Name.front() >= 'A' && Parameter.Name.front() <= 'Z') ||
				 (Parameter.Name.front() >= 'a' && Parameter.Name.front() <= 'z') || Parameter.Name.front() == '_') &&
				std::ranges::all_of(Parameter.Name.begin() + 1, Parameter.Name.end(), [](char Character)
				{
					return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z') ||
						(Character >= '0' && Character <= '9') || Character == '_';
				});
			if (Parameter.ID.IsNil() || !ValidName || !std::isfinite(Parameter.DefaultValue))
				return std::unexpected("Graph material parameter binding is invalid");
			for (size_t Previous = 0; Previous < Index; ++Previous)
				if (Material.Parameters[Previous].ID == Parameter.ID || Material.Parameters[Previous].Name == Parameter.Name)
					return std::unexpected("Graph material parameter identities and names must be unique");
			AppendU64(Output, Parameter.ID.GetHigh());
			AppendU64(Output, Parameter.ID.GetLow());
			AppendU32(Output, static_cast<uint32_t>(Parameter.Name.size()));
			for (const char Character : Parameter.Name) Output.push_back(static_cast<std::byte>(Character));
			AppendFloat(Output, Parameter.DefaultValue);
		}
		AppendU32(Output, static_cast<uint32_t>(MaterialState->size()));
		Output.insert(Output.end(), MaterialState->begin(), MaterialState->end());
		Output.insert(Output.end(), Material.SpirV.begin(), Material.SpirV.end());
		return Output;
	}

	std::expected<CookedGraphMaterialProduct, std::string> Detail::DecodeGraphMaterialProduct(
		std::span<const std::byte> Input)
	{
		constexpr size_t HeaderBytes = 48;
		constexpr size_t MaximumParameterBytes = 256u * (16u + 4u + 64u + 4u);
		constexpr size_t MaximumMaterialStateBytes = 1024;
		constexpr size_t MaximumPayloadBytes = HeaderBytes + 16u * 1024u * 1024u + MaximumParameterBytes +
			MaximumMaterialStateBytes + sizeof(uint32_t);
		if (Input.size() < HeaderBytes + 20 || Input.size() > MaximumPayloadBytes ||
			Input[0] != std::byte{'P'} || Input[1] != std::byte{'F'} || Input[2] != std::byte{'G'} || Input[3] != std::byte{'M'})
			return std::unexpected("Graph material product has invalid magic or size");
		size_t Offset = 4;
		uint32_t Version = 0, ParameterCount = 0;
		uint32_t SpirVBytes = 0;
		if (!ReadU32(Input, Offset, Version) || (Version != 1 && Version != GraphMaterialCodecVersion) ||
			!ReadU32(Input, Offset, SpirVBytes) ||
			SpirVBytes < 20 || SpirVBytes % 4 != 0)
			return std::unexpected("Graph material product schema or SPIR-V length is unsupported");
		CookedGraphMaterialProduct Product;
		Product.ArtifactVersion = GraphMaterialCodecVersion;
		Product.RecipeFingerprint.reserve(32);
		for (size_t Index = 0; Index < 32; ++Index) Product.RecipeFingerprint.push_back(static_cast<char>(std::to_integer<uint8_t>(Input[Offset++])));
		if (!ReadU32(Input, Offset, ParameterCount) || ParameterCount > 256)
			return std::unexpected("Graph material product parameter count exceeds its bound");
		Product.Parameters.reserve(ParameterCount);
		for (uint32_t Index = 0; Index < ParameterCount; ++Index)
		{
			uint64_t High = 0, Low = 0;
			uint32_t NameBytes = 0;
			if (!ReadU64(Input, Offset, High) || !ReadU64(Input, Offset, Low) || !ReadU32(Input, Offset, NameBytes) ||
				NameBytes == 0 || NameBytes > 64 || Offset > Input.size() || NameBytes > Input.size() - Offset)
				return std::unexpected("Graph material parameter binding is truncated or invalid");
			CookedGraphMaterialProduct::Parameter Parameter;
			Parameter.ID = UUID(High, Low);
			Parameter.Name.assign(reinterpret_cast<const char*>(Input.data() + Offset), NameBytes);
			Offset += NameBytes;
			if (!ReadFloat(Input, Offset, Parameter.DefaultValue) || Parameter.ID.IsNil())
				return std::unexpected("Graph material parameter identity or default is invalid");
			Product.Parameters.push_back(std::move(Parameter));
		}
		if (Version >= 2)
		{
			uint32_t MaterialStateBytes = 0;
			if (!ReadU32(Input, Offset, MaterialStateBytes) || MaterialStateBytes == 0 ||
				MaterialStateBytes > MaximumMaterialStateBytes || Offset > Input.size() || MaterialStateBytes > Input.size() - Offset)
				return std::unexpected("Graph material render-state payload is truncated or exceeds its bound");
			auto MaterialState = Detail::DecodeMaterialProduct(Input.subspan(Offset, MaterialStateBytes));
			if (!MaterialState) return std::unexpected("Graph material render-state payload is invalid: " + MaterialState.error());
			Product.Material = std::move(*MaterialState);
			Offset += MaterialStateBytes;
		}
		if (SpirVBytes != Input.size() - Offset) return std::unexpected("Graph material SPIR-V length does not match the payload");
		Product.SpirV.assign(Input.begin() + static_cast<std::ptrdiff_t>(Offset), Input.end());
		if (Product.RecipeFingerprint.find_first_not_of("0123456789abcdef") != std::string::npos ||
			Product.SpirV[0] != std::byte{ 0x03 } || Product.SpirV[1] != std::byte{ 0x02 } ||
			Product.SpirV[2] != std::byte{ 0x23 } || Product.SpirV[3] != std::byte{ 0x07 })
			return std::unexpected("Graph material product contains invalid recipe or SPIR-V bytes");
		auto Canonical = Detail::EncodeGraphMaterialProduct(Product);
		if (!Canonical) return std::unexpected(Canonical.error());
		return Product;
	}

	std::expected<std::vector<std::byte>, std::string> Detail::EnvironmentLightingArtifactCodec::EncodeEnvironment(
		const EnvironmentLightingData& Data, const EnvironmentProcessingDesc& Description,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested()) return std::unexpected("Environment product encoding cancelled");
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
			if (!AppendLevelFloats(Payload, Data.Environment, Description.EnvironmentFaceSize, CancellationToken) ||
				!AppendLevelFloats(Payload, Data.DiffuseIrradiance, Description.IrradianceFaceSize, CancellationToken))
			{
				if (CancellationToken.stop_requested()) return std::unexpected("Environment product encoding cancelled");
				return std::unexpected("Environment or irradiance cube has an invalid layout or non-finite channel");
			}
			uint32_t MipSize = Description.SpecularFaceSize;
			for (const FloatCubeLevel& Level : Data.PrefilteredSpecular)
			{
				if (!AppendLevelFloats(Payload, Level, MipSize, CancellationToken))
				{
					if (CancellationToken.stop_requested()) return std::unexpected("Environment product encoding cancelled");
					return std::unexpected("Specular cube mip has an invalid layout or non-finite channel");
				}
				MipSize = std::max(1u, MipSize / 2);
			}
			if (CancellationToken.stop_requested()) return std::unexpected("Environment product encoding cancelled");
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
		const ImportedFloatImage& Image, uint32_t Samples, std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested()) return std::unexpected("BRDF LUT encoding cancelled");
		if (Image.Width == 0 || Image.Width > 2048 || Image.Height != Image.Width || Samples == 0 || Samples > 4096 ||
			Image.RGBA32FPixels.size() != static_cast<uint64_t>(Image.Width) * Image.Height * 4 ||
			Image.RGBA32FPixels.size() * sizeof(float) > MaximumEnvironmentPayloadBytes)
			return std::unexpected("BRDF LUT dimensions, sample count, or pixels are invalid");
		for (size_t Index = 0; Index < Image.RGBA32FPixels.size(); ++Index)
		{
			if ((Index & 0xfffu) == 0 && CancellationToken.stop_requested())
				return std::unexpected("BRDF LUT encoding cancelled");
			if (!std::isfinite(Image.RGBA32FPixels[Index]))
				return std::unexpected("BRDF LUT dimensions, sample count, or pixels are invalid");
		}
		try
		{
			std::vector<std::byte> Payload;
			Payload.reserve(4 * sizeof(uint32_t) + Image.RGBA32FPixels.size() * sizeof(float));
			AppendU32(Payload, BrdfCodecVersion);
			AppendU32(Payload, Image.Width);
			AppendU32(Payload, Image.Height);
			AppendU32(Payload, Samples);
			for (size_t Index = 0; Index < Image.RGBA32FPixels.size(); ++Index)
			{
				if ((Index & 0xfffu) == 0 && CancellationToken.stop_requested())
					return std::unexpected("BRDF LUT encoding cancelled");
				AppendFloat(Payload, Image.RGBA32FPixels[Index]);
			}
			if (CancellationToken.stop_requested()) return std::unexpected("BRDF LUT encoding cancelled");
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
