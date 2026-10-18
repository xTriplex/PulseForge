#include "TestEngineSupport.h"

#include "Assets/AssetOperations.h"

#include <cstring>

namespace
{
	void TestGlbJsonReadSkipsBinaryPayload(TestRunner& Tests)
	{
		using namespace PulseForge::Assets::Detail;
		constexpr uint32_t BinaryPayloadBytes = 4u * 1024u * 1024u;
		std::string JsonText = R"({"asset":{"version":"2.0"},"buffers":[]})";
		while (JsonText.size() % 4 != 0)
			JsonText.push_back(' ');

		std::vector<std::byte> Container;
		const auto AppendUInt32 = [&Container](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Container.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
		};
		const uint32_t FileSize = static_cast<uint32_t>(12 + 8 + JsonText.size() + 8 + BinaryPayloadBytes);
		AppendUInt32(GlbMagic);
		AppendUInt32(2);
		AppendUInt32(FileSize);
		AppendUInt32(static_cast<uint32_t>(JsonText.size()));
		AppendUInt32(GlbJsonChunk);
		for (const char Character : JsonText)
			Container.push_back(static_cast<std::byte>(Character));
		AppendUInt32(BinaryPayloadBytes);
		AppendUInt32(GlbBinaryChunk);
		Container.resize(FileSize, std::byte{ 0x5a });

		size_t BytesRead = 0;
		const auto ReadRange = [&Container, &BytesRead](uint64_t Offset, std::span<std::byte> Output)
		{
			if (Offset > Container.size() || Output.size() > Container.size() - static_cast<size_t>(Offset)) return false;
			if (!Output.empty()) std::memcpy(Output.data(), Container.data() + static_cast<size_t>(Offset), Output.size());
			BytesRead += Output.size();
			return true;
		};
		auto ReadJson = ReadGlbJsonChunk(FileSize, 64ull * 1024ull * 1024ull, ReadRange);
		PF_CHECK(Tests, ReadJson.has_value());
		if (ReadJson) PF_CHECK(Tests, *ReadJson == JsonText);
		PF_CHECK(Tests, BytesRead == 12 + 8 + 8 + JsonText.size());
		PF_CHECK(Tests, BytesRead < Container.size());

		std::vector<std::byte> Truncated(16);
		const auto WriteUInt32 = [&Truncated](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Truncated[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};
		WriteUInt32(0, GlbMagic);
		WriteUInt32(4, 2);
		WriteUInt32(8, static_cast<uint32_t>(Truncated.size()));
		const auto ReadTruncated = [&Truncated](uint64_t Offset, std::span<std::byte> Output)
		{
			if (Offset > Truncated.size() || Output.size() > Truncated.size() - static_cast<size_t>(Offset)) return false;
			if (!Output.empty()) std::memcpy(Output.data(), Truncated.data() + static_cast<size_t>(Offset), Output.size());
			return true;
		};
		PF_CHECK(Tests, !InspectGlbLayout(Truncated.size(), 64ull * 1024ull * 1024ull, ReadTruncated));
	}

	void TestGltfMeshImport(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeGltfImport-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path ModelDirectory = ProjectRoot / "Assets" / "Models";
		std::filesystem::create_directories(ModelDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		std::filesystem::path SourcePath = ModelDirectory / "Triangle.GLTF";
		const std::filesystem::path BufferPath = ModelDirectory / "Triangle.bin";
		const std::string GltfDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"Triangle.bin","byteLength":66}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":36},
				{"buffer":0,"byteOffset":36,"byteLength":24},
				{"buffer":0,"byteOffset":60,"byteLength":6}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
				{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
			"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]}]
		})";
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(GltfDocument.data(), static_cast<std::streamsize>(GltfDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		std::vector<std::byte> BufferBytes;
		const auto AppendUInt32LE = [&BufferBytes](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				BufferBytes.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
		};
		const auto AppendFloatLE = [&AppendUInt32LE](float Value) { AppendUInt32LE(std::bit_cast<uint32_t>(Value)); };
		for (const float Value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
			AppendFloatLE(Value);
		for (const float Value : { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f })
			AppendFloatLE(Value);
		for (const uint16_t Value : { 0, 1, 2 })
		{
			BufferBytes.push_back(static_cast<std::byte>(Value & 0xff));
			BufferBytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		PF_CHECK(Tests, BufferBytes.size() == 66);
		{
			std::ofstream Output(BufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(BufferBytes.data()), static_cast<std::streamsize>(BufferBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		const auto SourceMetadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		const auto BufferMetadata = AssetMetadataSerializer::CreateForNewAsset(BufferPath);
		PF_CHECK(Tests, SourceMetadata.has_value() && BufferMetadata.has_value());
		if (!SourceMetadata || !BufferMetadata) return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Imported = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported) return;

		PF_CHECK(Tests, Imported->Vertices.size() == 3);
		PF_CHECK(Tests, Imported->Indices == std::vector<uint32_t>({ 0, 1, 2 }));
		PF_CHECK(Tests, Imported->Vertices[1].Position[0] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[2].TexCoord[1] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[0].Color[0] == 1.0f && Imported->Vertices[0].Color[1] == 1.0f);
		PF_CHECK(Tests,
			Imported->Vertices[0].Normal[0] == 0.0f && Imported->Vertices[0].Normal[1] == 0.0f && Imported->Vertices[0].Normal[2] == 1.0f);
		for (const GltfMeshVertex& Vertex : Imported->Vertices)
			PF_CHECK(Tests, std::abs(glm::length(glm::vec3(Vertex.Normal[0], Vertex.Normal[1], Vertex.Normal[2])) - 1.0f) < 1.0e-6f);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes.size() == 5);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes[3].Semantic == VertexSemantic::Normal);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes[4].Semantic == VertexSemantic::Tangent);
		for (const GltfMeshVertex& Vertex : Imported->Vertices)
		{
			PF_CHECK(Tests, std::abs(Vertex.Tangent[0] - 1.0f) < 1.0e-5f && std::abs(Vertex.Tangent[1]) < 1.0e-5f &&
								std::abs(Vertex.Tangent[2]) < 1.0e-5f);
			PF_CHECK(Tests, Vertex.Tangent[3] == 1.0f);
		}
		PF_CHECK(Tests, ValidateMeshDescription(Imported->GetMeshDescription()).has_value());

		const std::filesystem::path RelocatedDirectory = ProjectRoot / "Assets" / "Relocated";
		std::filesystem::create_directories(RelocatedDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		const auto CrossDirectoryMove = AssetOperations::Move(Registry, ProjectRoot, SourceMetadata->ID, "Assets/Relocated/Triangle.gltf");
		PF_CHECK(Tests, !CrossDirectoryMove && CrossDirectoryMove.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, std::filesystem::is_regular_file(SourcePath) && std::filesystem::is_regular_file(BufferPath));
		const auto CrossDirectoryDuplicate =
			AssetOperations::Duplicate(Registry, ProjectRoot, SourceMetadata->ID, "Assets/Relocated/TriangleCopy.gltf");
		PF_CHECK(Tests, !CrossDirectoryDuplicate && CrossDirectoryDuplicate.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, !std::filesystem::exists(RelocatedDirectory / "TriangleCopy.gltf"));
		const auto BufferMove = AssetOperations::Move(Registry, ProjectRoot, BufferMetadata->ID, "Assets/Relocated/Triangle.bin");
		PF_CHECK(Tests, !BufferMove && BufferMove.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, std::filesystem::is_regular_file(BufferPath));
		const auto ImportAfterRejectedMoves = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportAfterRejectedMoves && ImportAfterRejectedMoves->Indices.size() == 3);

		const std::filesystem::path BundleDirectory = ProjectRoot / "Assets" / "Bundles" / "Ship";
		std::filesystem::create_directories(BundleDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		std::string BundleDocument = GltfDocument;
		const size_t BundleUri = BundleDocument.find("Triangle.bin");
		PF_CHECK(Tests, BundleUri != std::string::npos);
		if (BundleUri == std::string::npos) return;
		BundleDocument.replace(BundleUri, std::string("Triangle.bin").size(), "Bundle.bin");
		const std::filesystem::path BundleSource = BundleDirectory / "Bundle.gltf";
		const std::filesystem::path BundleBuffer = BundleDirectory / "Bundle.bin";
		{
			std::ofstream Output(BundleSource, std::ios::binary | std::ios::trunc);
			Output.write(BundleDocument.data(), static_cast<std::streamsize>(BundleDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		{
			std::ofstream Output(BundleBuffer, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(BufferBytes.data()), static_cast<std::streamsize>(BufferBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto BundleMetadata = AssetMetadataSerializer::CreateForNewAsset(BundleSource);
		const auto BundleBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(BundleBuffer);
		PF_CHECK(Tests, BundleMetadata.has_value() && BundleBufferMetadata.has_value());
		if (!BundleMetadata || !BundleBufferMetadata) return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto BundleBeforeMove = GltfMeshImporter::ImportStaticPrimitive(BundleMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, BundleBeforeMove && BundleBeforeMove->Indices.size() == 3);
		const auto BundleMove = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Bundles/Ship", "Assets/Relocated");
		PF_CHECK(Tests, BundleMove && *BundleMove == std::filesystem::path("Assets/Relocated/Ship"));
		const auto BundleAfterMove = GltfMeshImporter::ImportStaticPrimitive(BundleMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, BundleAfterMove && BundleAfterMove->Indices.size() == 3);
		PF_CHECK(Tests, std::filesystem::is_regular_file(RelocatedDirectory / "Ship" / "Bundle.bin"));

		const std::filesystem::path IncomingDirectory = ProjectRoot / "Assets" / "Incoming";
		std::filesystem::create_directories(IncomingDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		const std::filesystem::path IncomingBuffer = IncomingDirectory / "Incoming.bin";
		{
			std::ofstream Output(IncomingBuffer, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(BufferBytes.data()), static_cast<std::streamsize>(BufferBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		std::string IncomingDocument = GltfDocument;
		const size_t IncomingUri = IncomingDocument.find("Triangle.bin");
		PF_CHECK(Tests, IncomingUri != std::string::npos);
		if (IncomingUri == std::string::npos) return;
		IncomingDocument.replace(IncomingUri, std::string("Triangle.bin").size(), "Incoming/Incoming.bin");
		const std::filesystem::path IncomingSource = ProjectRoot / "Assets" / "IncomingReference.gltf";
		{
			std::ofstream Output(IncomingSource, std::ios::binary | std::ios::trunc);
			Output.write(IncomingDocument.data(), static_cast<std::streamsize>(IncomingDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto IncomingBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(IncomingBuffer);
		const auto IncomingMetadata = AssetMetadataSerializer::CreateForNewAsset(IncomingSource);
		PF_CHECK(Tests, IncomingBufferMetadata.has_value() && IncomingMetadata.has_value());
		if (!IncomingBufferMetadata || !IncomingMetadata) return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, GltfMeshImporter::ImportStaticPrimitive(IncomingMetadata->ID, ProjectRoot, Registry).has_value());
		const auto IncomingFolderMove = AssetOperations::MoveFolder(Registry, ProjectRoot, "Assets/Incoming", "Assets/Relocated");
		PF_CHECK(Tests, !IncomingFolderMove && IncomingFolderMove.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, GltfMeshImporter::ImportStaticPrimitive(IncomingMetadata->ID, ProjectRoot, Registry).has_value());

		const std::filesystem::path VertexAlphaPath = ModelDirectory / "VertexColorAlpha.gltf";
		const std::filesystem::path VertexAlphaBufferPath = ModelDirectory / "VertexColorAlpha.bin";
		const std::string VertexAlphaDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"VertexColorAlpha.bin","byteLength":114}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":36},
				{"buffer":0,"byteOffset":36,"byteLength":24},
				{"buffer":0,"byteOffset":60,"byteLength":48},
				{"buffer":0,"byteOffset":108,"byteLength":6}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
				{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
				{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],
			"meshes":[{"name":"Vertex alpha","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"COLOR_0":2},"indices":3}]}]
		})";
		{
			std::ofstream Output(VertexAlphaPath, std::ios::binary | std::ios::trunc);
			Output.write(VertexAlphaDocument.data(), static_cast<std::streamsize>(VertexAlphaDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		std::vector<std::byte> VertexAlphaBytes;
		const auto AppendVertexAlphaFloat = [&VertexAlphaBytes](float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				VertexAlphaBytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		};
		for (const float Value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
			AppendVertexAlphaFloat(Value);
		for (const float Value : { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f })
			AppendVertexAlphaFloat(Value);
		for (const float Value : { 1.0f, 1.0f, 1.0f, 0.0f, 0.5f, 0.75f, 1.0f, 0.5f, 0.25f, 0.5f, 0.75f, 1.0f })
			AppendVertexAlphaFloat(Value);
		for (const uint16_t Value : { 0, 1, 2 })
		{
			VertexAlphaBytes.push_back(static_cast<std::byte>(Value & 0xff));
			VertexAlphaBytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		PF_CHECK(Tests, VertexAlphaBytes.size() == 114);
		{
			std::ofstream Output(VertexAlphaBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(VertexAlphaBytes.data()), static_cast<std::streamsize>(VertexAlphaBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto VertexAlphaMetadata = AssetMetadataSerializer::CreateForNewAsset(VertexAlphaPath);
		const auto VertexAlphaBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(VertexAlphaBufferPath);
		PF_CHECK(Tests, VertexAlphaMetadata.has_value() && VertexAlphaBufferMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (VertexAlphaMetadata)
		{
			const auto VertexAlpha = GltfMeshImporter::ImportStaticPrimitive(VertexAlphaMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, VertexAlpha.has_value());
			if (VertexAlpha)
			{
				PF_CHECK(Tests, VertexAlpha->Vertices[0].Color[3] == 0.0f);
				PF_CHECK(Tests, VertexAlpha->Vertices[1].Color[0] == 0.5f && VertexAlpha->Vertices[1].Color[1] == 0.75f &&
									VertexAlpha->Vertices[1].Color[3] == 0.5f);
				PF_CHECK(Tests, VertexAlpha->Vertices[2].Color[3] == 1.0f);
				PF_CHECK(Tests, VertexAlpha->GetMeshDescription().VertexLayout.Attributes[1].Format == VertexFormat::Float4);
			}
		}

		const std::filesystem::path AuthoredTangentPath = ModelDirectory / "AuthoredTangent.gltf";
		const std::filesystem::path AuthoredTangentBufferPath = ModelDirectory / "AuthoredTangent.bin";
		const std::string AuthoredTangentDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"AuthoredTangent.bin","byteLength":114}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":36},
				{"buffer":0,"byteOffset":36,"byteLength":24},
				{"buffer":0,"byteOffset":60,"byteLength":48},
				{"buffer":0,"byteOffset":108,"byteLength":6}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
				{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
				{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],
			"meshes":[{"name":"Authored tangent","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TANGENT":2},"indices":3}]}]
		})";
		{
			std::ofstream Output(AuthoredTangentPath, std::ios::binary | std::ios::trunc);
			Output.write(AuthoredTangentDocument.data(), static_cast<std::streamsize>(AuthoredTangentDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		std::vector<std::byte> AuthoredTangentBytes(BufferBytes.begin(), BufferBytes.begin() + 60);
		const auto AppendAuthoredTangentFloat = [&AuthoredTangentBytes](float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				AuthoredTangentBytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		};
		for (const float Value : { 1.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, -1.0f })
			AppendAuthoredTangentFloat(Value);
		AuthoredTangentBytes.insert(AuthoredTangentBytes.end(), BufferBytes.begin() + 60, BufferBytes.end());
		PF_CHECK(Tests, AuthoredTangentBytes.size() == 114);
		{
			std::ofstream Output(AuthoredTangentBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(
				reinterpret_cast<const char*>(AuthoredTangentBytes.data()), static_cast<std::streamsize>(AuthoredTangentBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto AuthoredTangentMetadata = AssetMetadataSerializer::CreateForNewAsset(AuthoredTangentPath);
		const auto AuthoredTangentBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(AuthoredTangentBufferPath);
		PF_CHECK(Tests, AuthoredTangentMetadata.has_value() && AuthoredTangentBufferMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (AuthoredTangentMetadata)
		{
			const auto AuthoredTangent = GltfMeshImporter::ImportStaticPrimitive(AuthoredTangentMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, AuthoredTangent.has_value());
			if (AuthoredTangent)
			{
				PF_CHECK(Tests, AuthoredTangent->Vertices.size() == 3);
				for (const GltfMeshVertex& Vertex : AuthoredTangent->Vertices)
					PF_CHECK(Tests, std::abs(Vertex.Tangent[0] - 1.0f) < 1.0e-5f && std::abs(Vertex.Tangent[1]) < 1.0e-5f &&
										std::abs(Vertex.Tangent[2]) < 1.0e-5f && Vertex.Tangent[3] == -1.0f);
			}
		}

		const std::filesystem::path MirroredPath = ModelDirectory / "MirroredUv.gltf";
		const std::filesystem::path MirroredBufferPath = ModelDirectory / "MirroredUv.bin";
		const std::string MirroredDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"MirroredUv.bin","byteLength":92}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":48},
				{"buffer":0,"byteOffset":48,"byteLength":32},
				{"buffer":0,"byteOffset":80,"byteLength":12}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},
				{"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}],
			"meshes":[{"name":"Mirrored UV seam","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]}]
		})";
		{
			std::ofstream Output(MirroredPath, std::ios::binary | std::ios::trunc);
			Output.write(MirroredDocument.data(), static_cast<std::streamsize>(MirroredDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		std::vector<std::byte> MirroredBytes;
		const auto AppendMirroredFloatLE = [&MirroredBytes](float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				MirroredBytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		};
		for (const float Value :
			{ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f })
			AppendMirroredFloatLE(Value);
		for (const uint16_t Value : { 0, 1, 2, 1, 3, 2 })
		{
			MirroredBytes.push_back(static_cast<std::byte>(Value & 0xff));
			MirroredBytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		PF_CHECK(Tests, MirroredBytes.size() == 92);
		{
			std::ofstream Output(MirroredBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(MirroredBytes.data()), static_cast<std::streamsize>(MirroredBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto MirroredMetadata = AssetMetadataSerializer::CreateForNewAsset(MirroredPath);
		const auto MirroredBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(MirroredBufferPath);
		PF_CHECK(Tests, MirroredMetadata.has_value() && MirroredBufferMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (MirroredMetadata)
		{
			const auto Mirrored = GltfMeshImporter::ImportStaticPrimitive(MirroredMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, Mirrored.has_value());
			if (Mirrored)
			{
				PF_CHECK(Tests, Mirrored->Vertices.size() == 6);
				PF_CHECK(Tests, Mirrored->Indices[0] != Mirrored->Indices[3] && Mirrored->Indices[1] != Mirrored->Indices[5]);
				PF_CHECK(Tests, Mirrored->Vertices[Mirrored->Indices[0]].Tangent[3] == 1.0f);
				PF_CHECK(Tests, Mirrored->Vertices[Mirrored->Indices[3]].Tangent[3] == -1.0f);
				PF_CHECK(Tests, ValidateMeshDescription(Mirrored->GetMeshDescription()).has_value());
			}
		}
		std::vector<std::byte> DegenerateBytes = BufferBytes;
		std::fill_n(DegenerateBytes.begin(), sizeof(float) * 9, std::byte{ 0 });
		{
			std::ofstream Output(BufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(DegenerateBytes.data()), static_cast<std::streamsize>(DegenerateBytes.size()));
		}
		const auto DegenerateImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, DegenerateImport.has_value());
		if (DegenerateImport)
			for (const GltfMeshVertex& Vertex : DegenerateImport->Vertices)
				PF_CHECK(Tests, Vertex.Normal[0] == 0.0f && Vertex.Normal[1] == 0.0f && Vertex.Normal[2] == 1.0f);
		{
			std::ofstream Output(BufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(BufferBytes.data()), static_cast<std::streamsize>(BufferBytes.size()));
		}

		std::string ExplicitNormalDocument = GltfDocument;
		const size_t ExplicitBufferUri = ExplicitNormalDocument.find("Triangle.bin");
		PF_CHECK(Tests, ExplicitBufferUri != std::string::npos);
		if (ExplicitBufferUri != std::string::npos)
			ExplicitNormalDocument.replace(ExplicitBufferUri, std::string("Triangle.bin").size(), "ExplicitNormals.bin");
		ExplicitNormalDocument.replace(ExplicitNormalDocument.find("\"byteLength\":66"), 15, "\"byteLength\":104");
		ExplicitNormalDocument.replace(ExplicitNormalDocument.find("{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"),
			std::string("{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}").size(),
			"{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":68,\"byteLength\":36}");
		ExplicitNormalDocument.replace(ExplicitNormalDocument.find("{\"bufferView\":2,\"componentType\":5123,"),
			std::string("{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}").size(),
			"{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
			"{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}");
		ExplicitNormalDocument.replace(ExplicitNormalDocument.find("\"POSITION\":0,\"TEXCOORD_0\":1"),
			std::string("\"POSITION\":0,\"TEXCOORD_0\":1").size(), "\"POSITION\":0,\"TEXCOORD_0\":1,\"NORMAL\":3");
		const std::filesystem::path ExplicitNormalPath = ModelDirectory / "ExplicitNormals.gltf";
		std::vector<std::byte> ExplicitNormalBytes = BufferBytes;
		BufferBytes.push_back(std::byte{ 0 });
		BufferBytes.push_back(std::byte{ 0 });
		for (const float Value : { 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f })
			AppendFloatLE(Value);
		ExplicitNormalBytes = BufferBytes;
		BufferBytes.resize(66);
		const std::filesystem::path ExplicitNormalBufferPath = ModelDirectory / "ExplicitNormals.bin";
		{
			std::ofstream Output(ExplicitNormalPath, std::ios::binary | std::ios::trunc);
			Output.write(ExplicitNormalDocument.data(), static_cast<std::streamsize>(ExplicitNormalDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		{
			std::ofstream Output(ExplicitNormalBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(
				reinterpret_cast<const char*>(ExplicitNormalBytes.data()), static_cast<std::streamsize>(ExplicitNormalBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto ExplicitNormalMetadata = AssetMetadataSerializer::CreateForNewAsset(ExplicitNormalPath);
		const auto ExplicitNormalBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(ExplicitNormalBufferPath);
		PF_CHECK(Tests, ExplicitNormalMetadata.has_value());
		PF_CHECK(Tests, ExplicitNormalBufferMetadata.has_value());
		const auto ExplicitRegistryResult = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, ExplicitRegistryResult.has_value());
		if (!ExplicitNormalMetadata) return;
		const auto ExplicitNormals = GltfMeshImporter::ImportStaticPrimitive(ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ExplicitNormals.has_value());
		PF_CHECK(Tests, ExplicitNormals && std::abs(ExplicitNormals->Vertices[0].Normal[2] - 1.0f) < 1.0e-6f);
		std::string InvalidNormalDocument = ExplicitNormalDocument;
		const size_t NormalAccessor = InvalidNormalDocument.find("\"bufferView\":3,\"componentType\":5126");
		const size_t NormalType = InvalidNormalDocument.find("\"type\":\"VEC3\"", NormalAccessor);
		PF_CHECK(Tests, NormalAccessor != std::string::npos && NormalType != std::string::npos);
		if (NormalType == std::string::npos) return;
		InvalidNormalDocument.replace(NormalType + std::string("\"type\":\"").size(), 4, "VEC2");
		{
			std::ofstream Output(ExplicitNormalPath, std::ios::binary | std::ios::trunc);
			Output.write(InvalidNormalDocument.data(), static_cast<std::streamsize>(InvalidNormalDocument.size()));
		}
		const auto InvalidNormals = GltfMeshImporter::ImportStaticPrimitive(ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !InvalidNormals && InvalidNormals.error().Code == GltfMeshImportErrorCode::UnsupportedFeature);
		{
			std::ofstream Output(ExplicitNormalPath, std::ios::binary | std::ios::trunc);
			Output.write(ExplicitNormalDocument.data(), static_cast<std::streamsize>(ExplicitNormalDocument.size()));
		}
		ExplicitNormalBytes[68] = static_cast<std::byte>(0x00);
		ExplicitNormalBytes[69] = static_cast<std::byte>(0x00);
		ExplicitNormalBytes[70] = static_cast<std::byte>(0xc0);
		ExplicitNormalBytes[71] = static_cast<std::byte>(0x7f);
		{
			std::ofstream Output(ExplicitNormalBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(
				reinterpret_cast<const char*>(ExplicitNormalBytes.data()), static_cast<std::streamsize>(ExplicitNormalBytes.size()));
		}
		const auto NonFiniteNormals = GltfMeshImporter::ImportStaticPrimitive(ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !NonFiniteNormals && NonFiniteNormals.error().Code == GltfMeshImportErrorCode::InvalidAccessor);

		const std::filesystem::path RenamedSourcePath = ModelDirectory / "RenamedTriangle.gltf";
		const auto MovedSource = AssetOperations::Move(Registry, ProjectRoot, SourceMetadata->ID, "Assets/Models/RenamedTriangle.gltf");
		PF_CHECK(Tests, MovedSource.has_value() && MovedSource->ID == SourceMetadata->ID);
		if (!MovedSource) return;
		SourcePath = RenamedSourcePath;
		const auto ImportedAfterMove = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedAfterMove.has_value());
		PF_CHECK(Tests, ImportedAfterMove && ImportedAfterMove->Vertices.size() == 3);

		std::string EmbeddedGltfDocument = GltfDocument;
		const size_t EmbeddedUriPosition = EmbeddedGltfDocument.find("Triangle.bin");
		PF_CHECK(Tests, EmbeddedUriPosition != std::string::npos);
		if (EmbeddedUriPosition == std::string::npos) return;
		EmbeddedGltfDocument.replace(EmbeddedUriPosition, std::string("Triangle.bin").size(),
			"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/"
			"AAABAAIA");
		const std::filesystem::path EmbeddedSourcePath = ModelDirectory / "Embedded.gltf";
		{
			std::ofstream Output(EmbeddedSourcePath, std::ios::binary | std::ios::trunc);
			Output.write(EmbeddedGltfDocument.data(), static_cast<std::streamsize>(EmbeddedGltfDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto EmbeddedMetadata = AssetMetadataSerializer::CreateForNewAsset(EmbeddedSourcePath);
		PF_CHECK(Tests, EmbeddedMetadata.has_value());
		if (!EmbeddedMetadata) return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto ImportedEmbedded = GltfMeshImporter::ImportStaticPrimitive(EmbeddedMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedEmbedded && ImportedEmbedded->Vertices.size() == 3 && ImportedEmbedded->Indices.size() == 3);
		const auto EmbeddedGltfMove = AssetOperations::Move(Registry, ProjectRoot, EmbeddedMetadata->ID, "Assets/Relocated/Embedded.gltf");
		PF_CHECK(Tests, EmbeddedGltfMove.has_value());
		const auto ImportedMovedEmbedded = GltfMeshImporter::ImportStaticPrimitive(EmbeddedMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedMovedEmbedded && ImportedMovedEmbedded->Vertices.size() == 3 && ImportedMovedEmbedded->Indices.size() == 3);

		std::string GlbDocument = GltfDocument;
		const size_t ExternalUriPosition = GlbDocument.find("\"uri\":\"Triangle.bin\",");
		PF_CHECK(Tests, ExternalUriPosition != std::string::npos);
		if (ExternalUriPosition == std::string::npos) return;
		GlbDocument.erase(ExternalUriPosition, std::string("\"uri\":\"Triangle.bin\",").size());
		while (GlbDocument.size() % 4 != 0)
			GlbDocument.push_back(' ');

		std::vector<std::byte> GlbBytes;
		const auto AppendGlbUInt32 = [&GlbBytes](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				GlbBytes.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
		};
		const uint32_t PaddedBinaryLength = static_cast<uint32_t>((BufferBytes.size() + 3) & ~size_t(3));
		const uint32_t JsonLength = static_cast<uint32_t>(GlbDocument.size());
		AppendGlbUInt32(0x46546C67);
		AppendGlbUInt32(2);
		AppendGlbUInt32(12 + 8 + JsonLength + 8 + PaddedBinaryLength);
		AppendGlbUInt32(JsonLength);
		AppendGlbUInt32(0x4E4F534A);
		for (const char Character : GlbDocument)
			GlbBytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
		AppendGlbUInt32(PaddedBinaryLength);
		AppendGlbUInt32(0x004E4942);
		GlbBytes.insert(GlbBytes.end(), BufferBytes.begin(), BufferBytes.end());
		while (GlbBytes.size() % 4 != 0)
			GlbBytes.push_back(std::byte{ 0 });

		const std::filesystem::path GlbPath = ModelDirectory / "Triangle.GLB";
		{
			std::ofstream Output(GlbPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(GlbBytes.data()), static_cast<std::streamsize>(GlbBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto GlbMetadata = AssetMetadataSerializer::CreateForNewAsset(GlbPath);
		PF_CHECK(Tests, GlbMetadata.has_value());
		if (!GlbMetadata) return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto ImportedGlb = GltfMeshImporter::ImportStaticPrimitive(GlbMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedGlb.has_value());
		PF_CHECK(Tests, ImportedGlb && ImportedGlb->Vertices.size() == 3 && ImportedGlb->Indices.size() == 3);
		const auto EmbeddedGlbMove = AssetOperations::Move(Registry, ProjectRoot, GlbMetadata->ID, "Assets/Relocated/Embedded.glb");
		PF_CHECK(Tests, EmbeddedGlbMove.has_value());
		const auto ImportedMovedGlb = GltfMeshImporter::ImportStaticPrimitive(GlbMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedMovedGlb && ImportedMovedGlb->Vertices.size() == 3 && ImportedMovedGlb->Indices.size() == 3);

		std::string ExternalGlbDocument = GltfDocument;
		while (ExternalGlbDocument.size() % 4 != 0)
			ExternalGlbDocument.push_back(' ');
		std::vector<std::byte> ExternalGlbBytes;
		const auto AppendExternalGlbUInt32 = [&ExternalGlbBytes](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				ExternalGlbBytes.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
		};
		const uint32_t ExternalJsonLength = static_cast<uint32_t>(ExternalGlbDocument.size());
		AppendExternalGlbUInt32(0x46546C67);
		AppendExternalGlbUInt32(2);
		AppendExternalGlbUInt32(12 + 8 + ExternalJsonLength);
		AppendExternalGlbUInt32(ExternalJsonLength);
		AppendExternalGlbUInt32(0x4E4F534A);
		for (const char Character : ExternalGlbDocument)
			ExternalGlbBytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
		const std::filesystem::path ExternalGlbPath = ModelDirectory / "ExternalBuffer.glb";
		{
			std::ofstream Output(ExternalGlbPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(ExternalGlbBytes.data()), static_cast<std::streamsize>(ExternalGlbBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto ExternalGlbMetadata = AssetMetadataSerializer::CreateForNewAsset(ExternalGlbPath);
		PF_CHECK(Tests, ExternalGlbMetadata.has_value());
		if (!ExternalGlbMetadata) return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, GltfMeshImporter::ImportStaticPrimitive(ExternalGlbMetadata->ID, ProjectRoot, Registry).has_value());
		const auto ExternalGlbMove =
			AssetOperations::Move(Registry, ProjectRoot, ExternalGlbMetadata->ID, "Assets/Relocated/ExternalBuffer.glb");
		PF_CHECK(Tests, !ExternalGlbMove && ExternalGlbMove.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, GltfMeshImporter::ImportStaticPrimitive(ExternalGlbMetadata->ID, ProjectRoot, Registry).has_value());

		std::vector<std::byte> OverpaddedGlb = GlbBytes;
		const size_t BinaryChunkHeaderOffset = 20 + JsonLength;
		const auto WriteUInt32LE = [&OverpaddedGlb](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				OverpaddedGlb[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};
		WriteUInt32LE(8, static_cast<uint32_t>(OverpaddedGlb.size() + 4));
		WriteUInt32LE(BinaryChunkHeaderOffset, PaddedBinaryLength + 4);
		OverpaddedGlb.insert(OverpaddedGlb.end(), 4, std::byte{ 0 });
		const std::filesystem::path OverpaddedGlbPath = ModelDirectory / "Overpadded.glb";
		{
			std::ofstream Output(OverpaddedGlbPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(OverpaddedGlb.data()), static_cast<std::streamsize>(OverpaddedGlb.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OverpaddedGlbMetadata = AssetMetadataSerializer::CreateForNewAsset(OverpaddedGlbPath);
		PF_CHECK(Tests, OverpaddedGlbMetadata.has_value());
		if (!OverpaddedGlbMetadata) return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto OverpaddedGlbImport = GltfMeshImporter::ImportStaticPrimitive(OverpaddedGlbMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !OverpaddedGlbImport.has_value());
		PF_CHECK(Tests, !OverpaddedGlbImport && OverpaddedGlbImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string OverflowingAccessor = GltfDocument;
		const size_t AccessorsStart = OverflowingAccessor.find("\"accessors\"");
		const size_t FirstAccessor = OverflowingAccessor.find("{\"bufferView\":0,", AccessorsStart);
		PF_CHECK(Tests, AccessorsStart != std::string::npos && FirstAccessor != std::string::npos);
		if (AccessorsStart == std::string::npos || FirstAccessor == std::string::npos) return;
		OverflowingAccessor.replace(
			FirstAccessor, std::string("{\"bufferView\":0,").size(), "{\"bufferView\":0,\"byteOffset\":18446744073709551615,");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(OverflowingAccessor.data(), static_cast<std::streamsize>(OverflowingAccessor.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OverflowingAccessorImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !OverflowingAccessorImport.has_value());
		PF_CHECK(Tests, !OverflowingAccessorImport && OverflowingAccessorImport.error().Code == GltfMeshImportErrorCode::InvalidAccessor);

		std::string SparseAccessor = GltfDocument;
		const size_t SparseFirstAccessor = SparseAccessor.find("{\"bufferView\":0,", SparseAccessor.find("\"accessors\""));
		SparseAccessor.replace(SparseFirstAccessor, std::string("{\"bufferView\":0,").size(), "{\"bufferView\":0,\"sparse\":{},");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(SparseAccessor.data(), static_cast<std::streamsize>(SparseAccessor.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto SparseAccessorImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !SparseAccessorImport.has_value());
		PF_CHECK(Tests, !SparseAccessorImport && SparseAccessorImport.error().Code == GltfMeshImportErrorCode::UnsupportedFeature);

		std::string RequiredExtension = GltfDocument;
		const size_t AssetHeaderPosition = RequiredExtension.find("\"asset\":{\"version\":\"2.0\"},");
		PF_CHECK(Tests, AssetHeaderPosition != std::string::npos);
		if (AssetHeaderPosition == std::string::npos) return;
		RequiredExtension.replace(AssetHeaderPosition, std::string("\"asset\":{\"version\":\"2.0\"},").size(),
			"\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"EXT_unknown_required\"],");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(RequiredExtension.data(), static_cast<std::streamsize>(RequiredExtension.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto RequiredExtensionImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !RequiredExtensionImport.has_value());
		PF_CHECK(Tests, !RequiredExtensionImport && RequiredExtensionImport.error().Code == GltfMeshImportErrorCode::UnsupportedFeature);

		std::string NullBufferUri = GltfDocument;
		const size_t NullUriPosition = NullBufferUri.find("Triangle.bin");
		PF_CHECK(Tests, NullUriPosition != std::string::npos);
		if (NullUriPosition == std::string::npos) return;
		NullBufferUri.replace(NullUriPosition, std::string("Triangle.bin").size(), "Triangle\\u0000.bin");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(NullBufferUri.data(), static_cast<std::streamsize>(NullBufferUri.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto NullUriImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !NullUriImport.has_value());
		PF_CHECK(Tests, !NullUriImport && NullUriImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string AmbiguousBufferUri = GltfDocument;
		const size_t AmbiguousUriPosition = AmbiguousBufferUri.find("Triangle.bin");
		PF_CHECK(Tests, AmbiguousUriPosition != std::string::npos);
		if (AmbiguousUriPosition == std::string::npos) return;
		AmbiguousBufferUri.replace(AmbiguousUriPosition, std::string("Triangle.bin").size(), "Triangle.bin.");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(AmbiguousBufferUri.data(), static_cast<std::streamsize>(AmbiguousBufferUri.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto AmbiguousUriImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !AmbiguousUriImport.has_value());
		PF_CHECK(Tests, !AmbiguousUriImport && AmbiguousUriImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string TruncatedBufferReference = GltfDocument;
		const size_t BufferLengthPosition = TruncatedBufferReference.find("\"byteLength\":66");
		PF_CHECK(Tests, BufferLengthPosition != std::string::npos);
		if (BufferLengthPosition != std::string::npos)
			TruncatedBufferReference.replace(BufferLengthPosition, std::string("\"byteLength\":66").size(), "\"byteLength\":65");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(TruncatedBufferReference.data(), static_cast<std::streamsize>(TruncatedBufferReference.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto TruncatedImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !TruncatedImport.has_value());
		PF_CHECK(Tests, !TruncatedImport && TruncatedImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string EscapingBufferReference = GltfDocument;
		const size_t UriPosition = EscapingBufferReference.find("Triangle.bin");
		PF_CHECK(Tests, UriPosition != std::string::npos);
		if (UriPosition != std::string::npos)
			EscapingBufferReference.replace(UriPosition, std::string("Triangle.bin").size(), "../../outside.bin");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(EscapingBufferReference.data(), static_cast<std::streamsize>(EscapingBufferReference.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto EscapingImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !EscapingImport.has_value());
		PF_CHECK(Tests, !EscapingImport && EscapingImport.error().Code == GltfMeshImportErrorCode::InvalidProjectPath);
	}
} // namespace

void RunGltfMeshImportTests(TestRunner& Tests)
{
	TestGlbJsonReadSkipsBinaryPayload(Tests);
	TestGltfMeshImport(Tests);
}
