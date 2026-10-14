#include "TestSupport.h"

namespace
{
	void TestGltfMeshImport(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

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
		if (FileError)
			return;

		std::filesystem::path SourcePath = ModelDirectory / "Triangle.gltf";
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
		const auto AppendFloatLE = [&AppendUInt32LE](float Value)
		{
			AppendUInt32LE(std::bit_cast<uint32_t>(Value));
		};
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
		if (!SourceMetadata || !BufferMetadata)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Imported = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported)
			return;

		PF_CHECK(Tests, Imported->Vertices.size() == 3);
		PF_CHECK(Tests, Imported->Indices == std::vector<uint32_t>({ 0, 1, 2 }));
		PF_CHECK(Tests, Imported->Vertices[1].Position[0] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[2].TexCoord[1] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[0].Color[0] == 1.0f && Imported->Vertices[0].Color[1] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[0].Normal[0] == 0.0f && Imported->Vertices[0].Normal[1] == 0.0f &&
			Imported->Vertices[0].Normal[2] == 1.0f);
		for (const GltfMeshVertex& Vertex : Imported->Vertices)
			PF_CHECK(Tests, std::abs(glm::length(glm::vec3(Vertex.Normal[0], Vertex.Normal[1], Vertex.Normal[2])) - 1.0f) < 1.0e-6f);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes.size() == 5);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes[3].Semantic == VertexSemantic::Normal);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes[4].Semantic == VertexSemantic::Tangent);
		for (const GltfMeshVertex& Vertex : Imported->Vertices)
		{
			PF_CHECK(Tests, std::abs(Vertex.Tangent[0] - 1.0f) < 1.0e-5f &&
				std::abs(Vertex.Tangent[1]) < 1.0e-5f && std::abs(Vertex.Tangent[2]) < 1.0e-5f);
			PF_CHECK(Tests, Vertex.Tangent[3] == 1.0f);
		}
		PF_CHECK(Tests, ValidateMeshDescription(Imported->GetMeshDescription()).has_value());

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
		for (const float Value : {
			1.0f, 1.0f, 1.0f, 0.0f,
			0.5f, 0.75f, 1.0f, 0.5f,
			0.25f, 0.5f, 0.75f, 1.0f })
			AppendVertexAlphaFloat(Value);
		for (const uint16_t Value : { 0, 1, 2 })
		{
			VertexAlphaBytes.push_back(static_cast<std::byte>(Value & 0xff));
			VertexAlphaBytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		PF_CHECK(Tests, VertexAlphaBytes.size() == 114);
		{
			std::ofstream Output(VertexAlphaBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(VertexAlphaBytes.data()),
				static_cast<std::streamsize>(VertexAlphaBytes.size()));
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
			Output.write(reinterpret_cast<const char*>(AuthoredTangentBytes.data()),
				static_cast<std::streamsize>(AuthoredTangentBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto AuthoredTangentMetadata = AssetMetadataSerializer::CreateForNewAsset(AuthoredTangentPath);
		const auto AuthoredTangentBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(AuthoredTangentBufferPath);
		PF_CHECK(Tests, AuthoredTangentMetadata.has_value() && AuthoredTangentBufferMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (AuthoredTangentMetadata)
		{
			const auto AuthoredTangent = GltfMeshImporter::ImportStaticPrimitive(
				AuthoredTangentMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, AuthoredTangent.has_value());
			if (AuthoredTangent)
			{
				PF_CHECK(Tests, AuthoredTangent->Vertices.size() == 3);
				for (const GltfMeshVertex& Vertex : AuthoredTangent->Vertices)
					PF_CHECK(Tests, std::abs(Vertex.Tangent[0] - 1.0f) < 1.0e-5f &&
						std::abs(Vertex.Tangent[1]) < 1.0e-5f && std::abs(Vertex.Tangent[2]) < 1.0e-5f &&
						Vertex.Tangent[3] == -1.0f);
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
		for (const float Value : {
			0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 0.0f,
			0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  0.0f, 0.0f })
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
		ExplicitNormalDocument.replace(
			ExplicitNormalDocument.find("{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"),
			std::string("{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}").size(),
			"{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":68,\"byteLength\":36}");
		ExplicitNormalDocument.replace(
			ExplicitNormalDocument.find("{\"bufferView\":2,\"componentType\":5123,"),
			std::string("{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}").size(),
			"{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
			"{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}");
		ExplicitNormalDocument.replace(
			ExplicitNormalDocument.find("\"POSITION\":0,\"TEXCOORD_0\":1"),
			std::string("\"POSITION\":0,\"TEXCOORD_0\":1").size(),
			"\"POSITION\":0,\"TEXCOORD_0\":1,\"NORMAL\":3");
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
			Output.write(reinterpret_cast<const char*>(ExplicitNormalBytes.data()),
				static_cast<std::streamsize>(ExplicitNormalBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto ExplicitNormalMetadata = AssetMetadataSerializer::CreateForNewAsset(ExplicitNormalPath);
		const auto ExplicitNormalBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(ExplicitNormalBufferPath);
		PF_CHECK(Tests, ExplicitNormalMetadata.has_value());
		PF_CHECK(Tests, ExplicitNormalBufferMetadata.has_value());
		const auto ExplicitRegistryResult = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, ExplicitRegistryResult.has_value());
		if (!ExplicitNormalMetadata)
			return;
		const auto ExplicitNormals = GltfMeshImporter::ImportStaticPrimitive(
			ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ExplicitNormals.has_value());
		PF_CHECK(Tests, ExplicitNormals && std::abs(ExplicitNormals->Vertices[0].Normal[2] - 1.0f) < 1.0e-6f);
		std::string InvalidNormalDocument = ExplicitNormalDocument;
		const size_t NormalAccessor = InvalidNormalDocument.find("\"bufferView\":3,\"componentType\":5126");
		const size_t NormalType = InvalidNormalDocument.find("\"type\":\"VEC3\"", NormalAccessor);
		PF_CHECK(Tests, NormalAccessor != std::string::npos && NormalType != std::string::npos);
		if (NormalType == std::string::npos)
			return;
		InvalidNormalDocument.replace(NormalType + std::string("\"type\":\"").size(), 4, "VEC2");
		{
			std::ofstream Output(ExplicitNormalPath, std::ios::binary | std::ios::trunc);
			Output.write(InvalidNormalDocument.data(), static_cast<std::streamsize>(InvalidNormalDocument.size()));
		}
		const auto InvalidNormals = GltfMeshImporter::ImportStaticPrimitive(
			ExplicitNormalMetadata->ID, ProjectRoot, Registry);
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
			Output.write(reinterpret_cast<const char*>(ExplicitNormalBytes.data()),
				static_cast<std::streamsize>(ExplicitNormalBytes.size()));
		}
		const auto NonFiniteNormals = GltfMeshImporter::ImportStaticPrimitive(
			ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !NonFiniteNormals && NonFiniteNormals.error().Code == GltfMeshImportErrorCode::InvalidAccessor);

		const std::filesystem::path RenamedSourcePath = ModelDirectory / "RenamedTriangle.gltf";
		const auto MovedSource = AssetOperations::Move(
			Registry,
			ProjectRoot,
			SourceMetadata->ID,
			"Assets/Models/RenamedTriangle.gltf");
		PF_CHECK(Tests, MovedSource.has_value() && MovedSource->ID == SourceMetadata->ID);
		if (!MovedSource)
			return;
		SourcePath = RenamedSourcePath;
		const auto ImportedAfterMove = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedAfterMove.has_value());
		PF_CHECK(Tests, ImportedAfterMove && ImportedAfterMove->Vertices.size() == 3);

		std::string EmbeddedGltfDocument = GltfDocument;
		const size_t EmbeddedUriPosition = EmbeddedGltfDocument.find("Triangle.bin");
		PF_CHECK(Tests, EmbeddedUriPosition != std::string::npos);
		if (EmbeddedUriPosition == std::string::npos)
			return;
		EmbeddedGltfDocument.replace(
			EmbeddedUriPosition,
			std::string("Triangle.bin").size(),
			"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA");
		const std::filesystem::path EmbeddedSourcePath = ModelDirectory / "Embedded.gltf";
		{
			std::ofstream Output(EmbeddedSourcePath, std::ios::binary | std::ios::trunc);
			Output.write(EmbeddedGltfDocument.data(), static_cast<std::streamsize>(EmbeddedGltfDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto EmbeddedMetadata = AssetMetadataSerializer::CreateForNewAsset(EmbeddedSourcePath);
		PF_CHECK(Tests, EmbeddedMetadata.has_value());
		if (!EmbeddedMetadata)
			return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto ImportedEmbedded = GltfMeshImporter::ImportStaticPrimitive(EmbeddedMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedEmbedded && ImportedEmbedded->Vertices.size() == 3 && ImportedEmbedded->Indices.size() == 3);

		std::string GlbDocument = GltfDocument;
		const size_t ExternalUriPosition = GlbDocument.find("\"uri\":\"Triangle.bin\",");
		PF_CHECK(Tests, ExternalUriPosition != std::string::npos);
		if (ExternalUriPosition == std::string::npos)
			return;
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

		const std::filesystem::path GlbPath = ModelDirectory / "Triangle.glb";
		{
			std::ofstream Output(GlbPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(GlbBytes.data()), static_cast<std::streamsize>(GlbBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto GlbMetadata = AssetMetadataSerializer::CreateForNewAsset(GlbPath);
		PF_CHECK(Tests, GlbMetadata.has_value());
		if (!GlbMetadata)
			return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto ImportedGlb = GltfMeshImporter::ImportStaticPrimitive(GlbMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedGlb.has_value());
		PF_CHECK(Tests, ImportedGlb && ImportedGlb->Vertices.size() == 3 && ImportedGlb->Indices.size() == 3);

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
			Output.write(reinterpret_cast<const char*>(OverpaddedGlb.data()),
				static_cast<std::streamsize>(OverpaddedGlb.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OverpaddedGlbMetadata = AssetMetadataSerializer::CreateForNewAsset(OverpaddedGlbPath);
		PF_CHECK(Tests, OverpaddedGlbMetadata.has_value());
		if (!OverpaddedGlbMetadata)
			return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto OverpaddedGlbImport = GltfMeshImporter::ImportStaticPrimitive(
			OverpaddedGlbMetadata->ID,
			ProjectRoot,
			Registry);
		PF_CHECK(Tests, !OverpaddedGlbImport.has_value());
		PF_CHECK(Tests, !OverpaddedGlbImport &&
			OverpaddedGlbImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string OverflowingAccessor = GltfDocument;
		const size_t AccessorsStart = OverflowingAccessor.find("\"accessors\"");
		const size_t FirstAccessor = OverflowingAccessor.find("{\"bufferView\":0,", AccessorsStart);
		PF_CHECK(Tests, AccessorsStart != std::string::npos && FirstAccessor != std::string::npos);
		if (AccessorsStart == std::string::npos || FirstAccessor == std::string::npos)
			return;
		OverflowingAccessor.replace(
			FirstAccessor,
			std::string("{\"bufferView\":0,").size(),
			"{\"bufferView\":0,\"byteOffset\":18446744073709551615,");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(OverflowingAccessor.data(), static_cast<std::streamsize>(OverflowingAccessor.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OverflowingAccessorImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !OverflowingAccessorImport.has_value());
		PF_CHECK(Tests, !OverflowingAccessorImport &&
			OverflowingAccessorImport.error().Code == GltfMeshImportErrorCode::InvalidAccessor);

		std::string SparseAccessor = GltfDocument;
		const size_t SparseFirstAccessor = SparseAccessor.find("{\"bufferView\":0,", SparseAccessor.find("\"accessors\""));
		SparseAccessor.replace(
			SparseFirstAccessor,
			std::string("{\"bufferView\":0,").size(),
			"{\"bufferView\":0,\"sparse\":{},");
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
		if (AssetHeaderPosition == std::string::npos)
			return;
		RequiredExtension.replace(
			AssetHeaderPosition,
			std::string("\"asset\":{\"version\":\"2.0\"},").size(),
			"\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"EXT_unknown_required\"],");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(RequiredExtension.data(), static_cast<std::streamsize>(RequiredExtension.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto RequiredExtensionImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !RequiredExtensionImport.has_value());
		PF_CHECK(Tests, !RequiredExtensionImport &&
			RequiredExtensionImport.error().Code == GltfMeshImportErrorCode::UnsupportedFeature);

		std::string NullBufferUri = GltfDocument;
		const size_t NullUriPosition = NullBufferUri.find("Triangle.bin");
		PF_CHECK(Tests, NullUriPosition != std::string::npos);
		if (NullUriPosition == std::string::npos)
			return;
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
		if (AmbiguousUriPosition == std::string::npos)
			return;
		AmbiguousBufferUri.replace(AmbiguousUriPosition, std::string("Triangle.bin").size(), "Triangle.bin.");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(AmbiguousBufferUri.data(), static_cast<std::streamsize>(AmbiguousBufferUri.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto AmbiguousUriImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !AmbiguousUriImport.has_value());
		PF_CHECK(Tests, !AmbiguousUriImport &&
			AmbiguousUriImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

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

	void TestImageAssetImport(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeImageImport-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path TextureDirectory = ProjectRoot / "Assets" / "Textures";
		std::filesystem::create_directories(TextureDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		std::vector<uint8_t> Bitmap;
		const auto AppendUInt16 = [&Bitmap](uint16_t Value)
		{
			Bitmap.push_back(static_cast<uint8_t>(Value & 0xff));
			Bitmap.push_back(static_cast<uint8_t>(Value >> 8));
		};
		const auto AppendUInt32 = [&Bitmap](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				Bitmap.push_back(static_cast<uint8_t>((Value >> (Byte * 8)) & 0xff));
		};
		Bitmap.push_back('B');
		Bitmap.push_back('M');
		AppendUInt32(70);
		AppendUInt32(0);
		AppendUInt32(54);
		AppendUInt32(40);
		AppendUInt32(2);
		AppendUInt32(2);
		AppendUInt16(1);
		AppendUInt16(24);
		AppendUInt32(0);
		AppendUInt32(16);
		AppendUInt32(0);
		AppendUInt32(0);
		AppendUInt32(0);
		AppendUInt32(0);
		// BMP storage is bottom-up and BGR: bottom row red/green, top row blue/white.
		Bitmap.insert(Bitmap.end(), { 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0 });
		PF_CHECK(Tests, Bitmap.size() == 70);

		const std::filesystem::path SourcePath = TextureDirectory / "checker.bmp";
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Bitmap.data()), static_cast<std::streamsize>(Bitmap.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Imported = ImageAssetImporter::ImportRGBA8(Metadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported)
			return;
		PF_CHECK(Tests, Imported->Width == 2 && Imported->Height == 2);
		PF_CHECK(Tests, Imported->RGBA8Pixels.size() == 16);
		PF_CHECK(Tests, Imported->RGBA8Pixels == std::vector<uint8_t>({
			0, 0, 255, 255, 255, 255, 255, 255,
			255, 0, 0, 255, 0, 255, 0, 255 }));
		const auto Preview = ImageAssetImporter::DecodePreviewRGBA8(SourcePath, 192);
		PF_CHECK(Tests, Preview && Preview->Width == 2 && Preview->Height == 2);
		PF_CHECK(Tests, Preview && Preview->RGBA8Pixels == Imported->RGBA8Pixels);
		PF_CHECK(Tests, Preview && Preview->SourceFingerprint == Imported->SourceFingerprint);
		const auto ZeroEdgePreview = ImageAssetImporter::DecodePreviewRGBA8(SourcePath, 0);
		PF_CHECK(Tests, !ZeroEdgePreview &&
			ZeroEdgePreview.error().Code == ImageAssetImportErrorCode::ResourceLimitExceeded);

		const auto MissingAssetID = UUID::Generate();
		PF_CHECK(Tests, MissingAssetID.has_value());
		if (MissingAssetID)
		{
			const auto Missing = ImageAssetImporter::ImportRGBA8(*MissingAssetID, ProjectRoot, Registry);
			PF_CHECK(Tests, !Missing && Missing.error().Code == ImageAssetImportErrorCode::AssetNotFound);
		}

		const std::filesystem::path RenamedPath = TextureDirectory / "checker-renamed.bmp";
		const auto Move = AssetOperations::Move(
			Registry,
			ProjectRoot,
			Metadata->ID,
			"Assets/Textures/checker-renamed.bmp");
		PF_CHECK(Tests, Move.has_value() && Move->ID == Metadata->ID);
		if (Move)
		{
			const auto ImportedAfterMove = ImageAssetImporter::ImportRGBA8(Metadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, ImportedAfterMove.has_value());
			PF_CHECK(Tests, ImportedAfterMove && ImportedAfterMove->RGBA8Pixels == Imported->RGBA8Pixels);
		}
		PF_CHECK(Tests, std::filesystem::exists(RenamedPath));
		const auto PreviewAfterMove = ImageAssetImporter::DecodePreviewRGBA8(RenamedPath);
		PF_CHECK(Tests, PreviewAfterMove && PreviewAfterMove->SourceFingerprint == Imported->SourceFingerprint);
		Bitmap.back() ^= 0x7fu;
		{
			std::ofstream Output(RenamedPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Bitmap.data()), static_cast<std::streamsize>(Bitmap.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto ReplacedPreview = ImageAssetImporter::DecodePreviewRGBA8(RenamedPath);
		PF_CHECK(Tests, ReplacedPreview && ReplacedPreview->SourceFingerprint != Imported->SourceFingerprint);

		const std::filesystem::path LargePreviewPath = TextureDirectory / "wide.ppm";
		{
			const std::string Header = "P6\n400 200\n255\n";
			std::vector<uint8_t> Ppm(Header.begin(), Header.end());
			Ppm.reserve(Ppm.size() + 400u * 200u * 3u);
			for (uint32_t Y = 0; Y < 200; ++Y)
				for (uint32_t X = 0; X < 400; ++X)
				{
					Ppm.push_back(static_cast<uint8_t>(X & 0xffu));
					Ppm.push_back(static_cast<uint8_t>(Y & 0xffu));
					Ppm.push_back(127);
				}
			std::ofstream Output(LargePreviewPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Ppm.data()), static_cast<std::streamsize>(Ppm.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(LargePreviewPath).has_value());
		const auto DownsampledPreview = ImageAssetImporter::DecodePreviewRGBA8(LargePreviewPath, 192);
		PF_CHECK(Tests, DownsampledPreview && DownsampledPreview->Width == 192 && DownsampledPreview->Height == 96);
		PF_CHECK(Tests, DownsampledPreview && DownsampledPreview->RGBA8Pixels.size() == 192u * 96u * 4u);

		const std::filesystem::path AlphaPreviewPath = TextureDirectory / "alpha.tga";
		{
			std::array<uint8_t, 26> Tga{};
			Tga[2] = 2;
			Tga[12] = 2;
			Tga[14] = 1;
			Tga[16] = 32;
			Tga[17] = 0x28;
			Tga[18] = 255;	// Blue, fully transparent.
			Tga[23] = 0;
			Tga[24] = 255;
			Tga[25] = 128;
			std::ofstream Output(AlphaPreviewPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Tga.data()), static_cast<std::streamsize>(Tga.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(AlphaPreviewPath).has_value());
		const auto AlphaPreview = ImageAssetImporter::DecodePreviewRGBA8(AlphaPreviewPath);
		PF_CHECK(Tests, AlphaPreview && AlphaPreview->Width == 2 && AlphaPreview->Height == 1);
		PF_CHECK(Tests, AlphaPreview && AlphaPreview->RGBA8Pixels[0] == 0 && AlphaPreview->RGBA8Pixels[2] == 255 &&
			AlphaPreview->RGBA8Pixels[3] == 0 && AlphaPreview->RGBA8Pixels[4] == 255 && AlphaPreview->RGBA8Pixels[7] == 128);

		const std::filesystem::path InvalidImagePath = TextureDirectory / "invalid.bmp";
		{
			std::ofstream Output(InvalidImagePath, std::ios::binary | std::ios::trunc);
			Output.write("not an image", 12);
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto InvalidImageMetadata = AssetMetadataSerializer::CreateForNewAsset(InvalidImagePath);
		PF_CHECK(Tests, InvalidImageMetadata.has_value());
		const auto InvalidPreview = ImageAssetImporter::DecodePreviewRGBA8(InvalidImagePath);
		PF_CHECK(Tests, !InvalidPreview && InvalidPreview.error().Code == ImageAssetImportErrorCode::InvalidImage);
		if (InvalidImageMetadata)
		{
			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			const auto InvalidImage = ImageAssetImporter::ImportRGBA8(InvalidImageMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !InvalidImage && InvalidImage.error().Code == ImageAssetImportErrorCode::InvalidImage);
		}

		const std::filesystem::path HdrPath = TextureDirectory / "tiny.hdr";
		{
			constexpr std::string_view HdrHeader = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
			std::ofstream Output(HdrPath, std::ios::binary | std::ios::trunc);
			Output.write(HdrHeader.data(), static_cast<std::streamsize>(HdrHeader.size()));
			constexpr std::array<uint8_t, 8> RgbePixels = { 128, 64, 32, 130, 128, 128, 128, 129 };
			Output.write(reinterpret_cast<const char*>(RgbePixels.data()), static_cast<std::streamsize>(RgbePixels.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto HdrMetadata = AssetMetadataSerializer::CreateForNewAsset(HdrPath);
		PF_CHECK(Tests, HdrMetadata.has_value());
		if (HdrMetadata)
		{
			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			const auto ImportedHdr = ImageAssetImporter::ImportRGBA32F(HdrMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, ImportedHdr.has_value());
			if (ImportedHdr)
			{
				PF_CHECK(Tests, ImportedHdr->Width == 2 && ImportedHdr->Height == 1);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels.size() == 8);
				PF_CHECK(Tests, std::abs(ImportedHdr->RGBA32FPixels[0] - 2.0f) < 0.001f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[1] > 0.99f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[2] > 0.49f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[3] == 1.0f);
			}
			const auto RejectedLdrAsHdr = ImageAssetImporter::ImportRGBA32F(Metadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !RejectedLdrAsHdr && RejectedLdrAsHdr.error().Code == ImageAssetImportErrorCode::InvalidImage);
		}
	}

	std::vector<std::byte> MakeSilentWave(uint32_t SampleCount)
	{
		constexpr uint32_t Channels = 1;
		constexpr uint32_t SampleRate = 44100;
		constexpr uint16_t BitsPerSample = 16;
		constexpr uint16_t BlockAlign = Channels * (BitsPerSample / 8);
		const uint32_t DataSize = SampleCount * BlockAlign;
		std::vector<std::byte> Data(44 + DataSize);

		auto WriteFourCC = [&Data](size_t Offset, std::string_view Value)
		{
			for (size_t Index = 0; Index < Value.size(); ++Index)
				Data[Offset + Index] = static_cast<std::byte>(static_cast<unsigned char>(Value[Index]));
		};
		auto Write16 = [&Data](size_t Offset, uint16_t Value)
		{
			Data[Offset] = static_cast<std::byte>(Value & 0xff);
			Data[Offset + 1] = static_cast<std::byte>((Value >> 8) & 0xff);
		};
		auto Write32 = [&Data](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Data[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};

		WriteFourCC(0, "RIFF");
		Write32(4, static_cast<uint32_t>(Data.size() - 8));
		WriteFourCC(8, "WAVE");
		WriteFourCC(12, "fmt ");
		Write32(16, 16);
		Write16(20, 1);
		Write16(22, Channels);
		Write32(24, SampleRate);
		Write32(28, SampleRate * BlockAlign);
		Write16(32, BlockAlign);
		Write16(34, BitsPerSample);
		WriteFourCC(36, "data");
		Write32(40, DataSize);
		return Data;
	}

	void TestAudioEngineAndAssetCache(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeAudio-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Audio", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		auto AudioProject = Project::Create(ProjectRoot / "AudioProject.pfproj", "Audio Script Tests");
		PF_CHECK(Tests, AudioProject.has_value());
		if (!AudioProject)
			return;

		const std::vector<std::byte> WaveData = MakeSilentWave(44100 * 5);
		AudioPlayback Playback;
		{
			auto CreatedEngine = AudioEngine::Create({ AudioOutputBackend::Null });
			PF_CHECK(Tests, CreatedEngine.has_value());
			if (!CreatedEngine)
				return;
			AudioEngine Engine = std::move(*CreatedEngine);
			PF_CHECK(Tests, Engine.IsInitialized());

			const std::array<std::byte, 4> InvalidData{};
			const auto InvalidClip = Engine.CreateClip("Invalid", InvalidData);
			PF_CHECK(Tests, !InvalidClip && InvalidClip.error().Code == AudioErrorCode::InvalidClip);
			const auto Clip = Engine.CreateClip("Test WAV", WaveData);
			PF_CHECK(Tests, Clip.has_value());
			if (!Clip)
				return;
			PF_CHECK(Tests, Clip->IsValid() && Clip->GetDebugName() == "Test WAV");

			AudioPlaybackDesc InvalidDescription;
			InvalidDescription.Volume = 1.1f;
			PF_CHECK(Tests, !InvalidDescription.Validate());
			PF_CHECK(Tests, !Engine.Play(*Clip, InvalidDescription));
			AudioPlaybackDesc Description;
			Description.Spatialized = true;
			Description.Position = { 2.0f, 0.0f, -1.0f };
			Description.Volume = 0.5f;
			auto StartedPlayback = Engine.Play(*Clip, Description);
			PF_CHECK(Tests, StartedPlayback.has_value());
			if (!StartedPlayback)
				return;
			Playback = std::move(*StartedPlayback);
			PF_CHECK(Tests, Playback.IsValid());
			auto ConcurrentPlayback = Engine.Play(*Clip, Description);
			PF_CHECK(Tests, ConcurrentPlayback.has_value() && ConcurrentPlayback->IsValid());
			if (ConcurrentPlayback)
				PF_CHECK(Tests, ConcurrentPlayback->Stop().has_value());
			PF_CHECK(Tests, Playback.Pause().has_value());
			PF_CHECK(Tests, Playback.Resume().has_value());
			PF_CHECK(Tests, Playback.SetVolume(0.25f).has_value());
			PF_CHECK(Tests, Playback.SetLooping(true).has_value());
			PF_CHECK(Tests, Playback.SetSpatialized(false).has_value());
			PF_CHECK(Tests, Playback.SetPosition({ 0.0f, 1.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Playback.Stop().has_value());
			PF_CHECK(Tests, Engine.SetListenerPosition({ 0.0f, 0.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Engine.SetListenerDirection({ 0.0f, 0.0f, -1.0f }).has_value());
			PF_CHECK(Tests, !Engine.SetListenerDirection({ 0.0f, 0.0f, 0.0f }));

			AssetRegistry& Registry = AudioProject->GetAssetRegistry();
			auto Imported = AssetOperations::CreateAssetFromBytes(
				Registry,
				ProjectRoot,
				WaveData,
				"Assets/Audio/test.wav");
			PF_CHECK(Tests, Imported.has_value());
			if (!Imported)
				return;

			AudioAssetCache Cache(Engine, ProjectRoot, Registry);
			const auto Loaded = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, Loaded && Loaded->IsValid());
			PF_CHECK(Tests, Cache.GetLoadedCount() == 1);
			const auto CachedAgain = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, CachedAgain && Cache.GetLoadedCount() == 1);

			const auto Moved = AssetOperations::Move(
				Registry,
				ProjectRoot,
				Imported->ID,
				"Assets/Audio/renamed.wav");
			PF_CHECK(Tests, Moved && Moved->ID == Imported->ID);
			Cache.Clear();
			const auto LoadedAfterMove = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, LoadedAfterMove && LoadedAfterMove->IsValid());
			const auto MovedRecord = Registry.Find(Imported->ID);
			PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == "Assets/Audio/renamed.wav");

			Scene AudioScene;
			auto EmitterResult = AudioScene.CreateEntity("Audio emitter and listener");
			PF_CHECK(Tests, EmitterResult.has_value());
			if (!EmitterResult)
				return;
			Entity Emitter = *EmitterResult;
			PF_CHECK(Tests, Emitter.SetAudioSource(AudioSourceComponent{ Imported->ID, 0.5f, true, true, true }).has_value());
			PF_CHECK(Tests, Emitter.SetAudioListener(AudioListenerComponent{}).has_value());

			AudioSceneRuntime Runtime(Engine, Cache);
			PF_CHECK(Tests, Runtime.Start(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetPlaybackCount() == 1);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			Scene OtherAudioScene;
			PF_CHECK(Tests, !Runtime.Advance(OtherAudioScene));
			PF_CHECK(Tests, Runtime.Pause(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.Resume(Emitter.GetUUID()).has_value());

			AudioSourceComponent UpdatedSource{ Imported->ID, 0.25f, false, true, false };
			PF_CHECK(Tests, Emitter.SetAudioSource(UpdatedSource).has_value());
			TransformComponent MovedEmitter;
			MovedEmitter.Translation = { 3.0f, 2.0f, 1.0f };
			PF_CHECK(Tests, Emitter.SetTransform(MovedEmitter).has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.StopPlayback(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 0);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 0);
			PF_CHECK(Tests, Runtime.Play(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);

			std::filesystem::create_directories(ProjectRoot / "Assets" / "Scripts", FileError);
			PF_CHECK(Tests, !FileError);
			if (FileError)
				return;
			constexpr std::string_view AudioControlSource = R"(
function OnUpdate()
  assert(entity:stop_audio())
  assert(entity:play_audio())
  assert(entity:pause_audio())
  assert(entity:resume_audio())
end
)";
			const auto AudioControlBytes = std::as_bytes(std::span(AudioControlSource.data(), AudioControlSource.size()));
			const auto AudioControlAsset = AssetOperations::CreateAssetFromBytes(
				Registry,
				ProjectRoot,
				AudioControlBytes,
				"Assets/Scripts/audio_controls.lua");
			PF_CHECK(Tests, AudioControlAsset.has_value());
			if (!AudioControlAsset)
				return;
			PF_CHECK(Tests, Emitter.SetScript(ScriptComponent{ AudioControlAsset->ID }).has_value());
			ScriptRuntimeServices AudioServices;
			AudioServices.Audio = &Runtime;
			ScriptRuntime AudioControlRuntime(*AudioProject, {}, AudioServices);
			PF_CHECK(Tests, AudioControlRuntime.Start(AudioScene).has_value());
			PF_CHECK(Tests, AudioControlRuntime.Advance(AudioScene, Timestep(1.0 / 60.0)).has_value());
			PF_CHECK(Tests, AudioControlRuntime.GetDiagnostics().empty());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);
			AudioControlRuntime.Stop();
			PF_CHECK(Tests, Emitter.RemoveScript().has_value());

			PF_CHECK(Tests, Emitter.RemoveAudioSource().has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 0);
			Runtime.Stop();
			PF_CHECK(Tests, !Runtime.IsRunning());

			Scene PartialStartScene;
			const UUID ValidEmitterId{ 0x5300000000000000ull, 1 };
			const UUID MissingEmitterId{ 0x5300000000000000ull, 2 };
			const auto ValidEmitterResult = PartialStartScene.CreateEntityWithUUID(ValidEmitterId, "Valid audio source");
			const auto MissingEmitterResult = PartialStartScene.CreateEntityWithUUID(MissingEmitterId, "Missing audio source");
			PF_CHECK(Tests, ValidEmitterResult && MissingEmitterResult);
			if (ValidEmitterResult && MissingEmitterResult)
			{
				PF_CHECK(Tests, ValidEmitterResult->SetAudioSource(AudioSourceComponent{ Imported->ID }).has_value());
				PF_CHECK(Tests, MissingEmitterResult->SetAudioSource(AudioSourceComponent{
					AssetID{ 0x5300000000000000ull, 3 } }).has_value());
				AudioSceneRuntime RollbackRuntime(Engine, Cache);
				const auto FailedStart = RollbackRuntime.Start(PartialStartScene);
				PF_CHECK(Tests, !FailedStart && FailedStart.error().Code == AudioSceneRuntimeErrorCode::AssetLoadFailed);
				PF_CHECK(Tests, !RollbackRuntime.IsRunning() && RollbackRuntime.GetPlaybackCount() == 0);
				PF_CHECK(Tests, MissingEmitterResult->RemoveAudioSource().has_value());
				PF_CHECK(Tests, RollbackRuntime.Start(PartialStartScene).has_value());
				PF_CHECK(Tests, RollbackRuntime.GetPlaybackCount() == 1);
				RollbackRuntime.Stop();
			}

			Scene InvalidListenerScene;
			auto FirstListener = InvalidListenerScene.CreateEntity("First listener");
			auto SecondListener = InvalidListenerScene.CreateEntity("Second listener");
			PF_CHECK(Tests, FirstListener && SecondListener);
			if (FirstListener && SecondListener)
			{
				PF_CHECK(Tests, FirstListener->SetAudioListener(AudioListenerComponent{}).has_value());
				PF_CHECK(Tests, SecondListener->SetAudioListener(AudioListenerComponent{}).has_value());
				AudioSceneRuntime InvalidRuntime(Engine, Cache);
				const auto InvalidStart = InvalidRuntime.Start(InvalidListenerScene);
				PF_CHECK(Tests, !InvalidStart &&
					InvalidStart.error().Code == AudioSceneRuntimeErrorCode::MultiplePrimaryListeners);
				PF_CHECK(Tests, !InvalidRuntime.IsRunning());
			}

			Scene DuplicatedPrimaryListenerScene;
			auto PrimaryListener = DuplicatedPrimaryListenerScene.CreateEntity("Primary listener");
			PF_CHECK(Tests, PrimaryListener.has_value());
			if (PrimaryListener)
			{
				PF_CHECK(Tests, PrimaryListener->SetAudioListener(AudioListenerComponent{ true }).has_value());
				auto DuplicatedListener = DuplicatedPrimaryListenerScene.DuplicateEntity(*PrimaryListener);
				PF_CHECK(Tests, DuplicatedListener.has_value());
				if (DuplicatedListener)
				{
					const auto OriginalListenerComponent = PrimaryListener->GetAudioListener();
					const auto DuplicatedListenerComponent = DuplicatedListener->GetAudioListener();
					PF_CHECK(Tests, OriginalListenerComponent && OriginalListenerComponent->has_value() &&
						OriginalListenerComponent->value().IsPrimary);
					PF_CHECK(Tests, DuplicatedListenerComponent && DuplicatedListenerComponent->has_value() &&
						!DuplicatedListenerComponent->value().IsPrimary);
					AudioSceneRuntime RuntimeAfterListenerDuplication(Engine, Cache);
					PF_CHECK(Tests, RuntimeAfterListenerDuplication.Start(DuplicatedPrimaryListenerScene).has_value());
					PF_CHECK(Tests, RuntimeAfterListenerDuplication.Advance(DuplicatedPrimaryListenerScene).has_value());
				}
			}
		}

		// A playback retains the initialized device state even after its AudioEngine owner goes away.
		PF_CHECK(Tests, Playback.SetVolume(0.75f).has_value());
		PF_CHECK(Tests, Playback.Stop().has_value());
	}
}

void RunImportsTests(TestRunner& Tests)
{
	TestGltfMeshImport(Tests);
	TestImageAssetImport(Tests);
	TestAudioEngineAndAssetCache(Tests);
}
