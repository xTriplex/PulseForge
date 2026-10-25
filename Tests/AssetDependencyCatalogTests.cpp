#include "TestAssetSupport.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetTypes.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <set>
#include <vector>

namespace
{
	using namespace PulseForge;

	class AssetDependencyFixture final
	{
	public:
		AssetDependencyFixture()
		{
			std::error_code Error;
			const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(Error);
			if (Error) return;

			const auto Identifier = UUID::Generate();
			if (!Identifier) return;
			ProjectID = *Identifier;
			Root = TemporaryDirectory / ("PulseForgeDependencyCatalog-" + ProjectID.ToString());
			std::filesystem::create_directories(Root / "Assets", Error);
			Valid = !Error;
		}

		~AssetDependencyFixture()
		{
			std::error_code Error;
			if (!Root.empty()) std::filesystem::remove_all(Root, Error);
		}

		[[nodiscard]] std::optional<AssetID> WriteManaged(std::string_view RelativePath, std::string_view Data)
		{
			return WriteManagedPath(std::filesystem::path(RelativePath), Data);
		}

		[[nodiscard]] std::optional<AssetID> WriteManagedPath(const std::filesystem::path& RelativePath, std::string_view Data)
		{
			const std::filesystem::path Path = Root / RelativePath;
			std::error_code Error;
			std::filesystem::create_directories(Path.parent_path(), Error);
			if (Error || !WriteBytes(Path, Data)) return std::nullopt;
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			if (!Metadata) return std::nullopt;
			return Metadata->ID;
		}

		[[nodiscard]] static bool WriteBytes(const std::filesystem::path& Path, std::string_view Data)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
			Output.close();
			return static_cast<bool>(Output);
		}

		UUID ProjectID;
		std::filesystem::path Root;
		bool Valid = false;
	};

	void AppendUInt32LE(std::vector<std::byte>& Bytes, uint32_t Value)
	{
		for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
			Bytes.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
	}

	void AppendFloatLE(std::vector<std::byte>& Bytes, float Value)
	{
		AppendUInt32LE(Bytes, std::bit_cast<uint32_t>(Value));
	}

	[[nodiscard]] std::vector<std::byte> MakeTriangleBuffer()
	{
		std::vector<std::byte> Bytes;
		for (const float Value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
			AppendFloatLE(Bytes, Value);
		for (const float Value : { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f })
			AppendFloatLE(Bytes, Value);
		for (const uint16_t Value : { 0, 1, 2 })
		{
			Bytes.push_back(static_cast<std::byte>(Value & 0xff));
			Bytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		return Bytes;
	}

	[[nodiscard]] std::string MakeGltfDocument(std::optional<std::string_view> BufferURI)
	{
		const std::string Buffer =
			BufferURI ? std::string("{\"uri\":\"") + std::string(*BufferURI) + "\",\"byteLength\":66}" : "{\"byteLength\":66}";
		return std::string(R"({
			"asset":{"version":"2.0"},
			"buffers":[)") +
			   Buffer + R"(],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":36},
				{"buffer":0,"byteOffset":36,"byteLength":24},
				{"buffer":0,"byteOffset":60,"byteLength":6}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
				{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
			"images":[{"uri":"Images/not-consumed.bin"},{"uri":"Images/missing.bin"}],
			"meshes":[
				{"name":"Duplicate label","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]},
				{"name":"Duplicate label","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]}
			]
		})";
	}

	[[nodiscard]] std::vector<std::byte> MakeGlbDocument(std::string Json, const std::vector<std::byte>& Binary)
	{
		while (Json.size() % 4 != 0)
			Json.push_back(' ');
		const size_t PaddedBinarySize = (Binary.size() + 3u) & ~size_t{ 3u };
		std::vector<std::byte> Bytes;
		Bytes.reserve(12 + 8 + Json.size() + 8 + PaddedBinarySize);
		AppendUInt32LE(Bytes, 0x46546C67);
		AppendUInt32LE(Bytes, 2);
		AppendUInt32LE(Bytes, static_cast<uint32_t>(12 + 8 + Json.size() + 8 + PaddedBinarySize));
		AppendUInt32LE(Bytes, static_cast<uint32_t>(Json.size()));
		AppendUInt32LE(Bytes, 0x4E4F534A);
		for (const unsigned char Character : Json)
			Bytes.push_back(static_cast<std::byte>(Character));
		AppendUInt32LE(Bytes, static_cast<uint32_t>(PaddedBinarySize));
		AppendUInt32LE(Bytes, 0x004E4942);
		Bytes.insert(Bytes.end(), Binary.begin(), Binary.end());
		Bytes.resize(Bytes.size() + PaddedBinarySize - Binary.size(), std::byte{ 0 });
		return Bytes;
	}

	[[nodiscard]] std::string EdgeKey(const AssetDependencyEdge& Edge)
	{
		return Edge.Referrer.Asset.ToString() + "|" + Edge.Referrer.SourceFileKey + "|" + Edge.Referrer.SubresourceKey + "|" +
			   Edge.Target.Asset.ToString() + "|" + Edge.Target.SourceFileKey + "|" + Edge.Target.SubresourceKey + "|" +
			   std::to_string(static_cast<unsigned int>(Edge.Kind)) + "|" + std::to_string(static_cast<unsigned int>(Edge.Status)) + "|" +
			   Edge.Location + "|" + Edge.OriginalReference;
	}

	[[nodiscard]] AssetDependencyNode ManagedNode(const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Asset)
	{
		const auto Record = Snapshot.GetRegistry().Find(Asset);
		if (!Record) return { Asset, {}, {} };
		const auto UTF8Path = Record->ProjectRelativePath.generic_u8string();
		return { Asset, std::string(reinterpret_cast<const char*>(UTF8Path.data()), UTF8Path.size()), {} };
	}

	[[nodiscard]] bool HasDependency(
		const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Referrer, const AssetID& Target, AssetDependencyEdgeKind Kind)
	{
		const AssetDependencyNode Node = ManagedNode(Snapshot, Referrer);
		const auto Dependencies = Snapshot.GetDirectDependencies(Node);
		return std::find_if(Dependencies.begin(), Dependencies.end(), [&](const AssetDependencyEdge& Edge)
				   { return Edge.Target.Asset == Target && Edge.Kind == Kind && Edge.Status == AssetDependencyStatus::Resolved; }) !=
			   Dependencies.end();
	}

	[[nodiscard]] bool HasSourceDependency(
		const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Referrer, std::string_view TargetPath, AssetDependencyEdgeKind Kind)
	{
		const auto Dependencies = Snapshot.GetDirectDependencies(ManagedNode(Snapshot, Referrer));
		return std::find_if(Dependencies.begin(), Dependencies.end(),
				   [&](const AssetDependencyEdge& Edge)
				   {
					   return Edge.Target.Asset.IsNil() && Edge.Target.SourceFileKey == TargetPath && Edge.Kind == Kind &&
							  Edge.Status == AssetDependencyStatus::Resolved;
				   }) != Dependencies.end();
	}

	[[nodiscard]] bool HasDependencyStatus(
		const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Referrer, const AssetID& Target, AssetDependencyStatus Status)
	{
		return std::find_if(Snapshot.GetEdges().begin(), Snapshot.GetEdges().end(), [&](const AssetDependencyEdge& Edge)
				   { return Edge.Referrer.Asset == Referrer && Edge.Target.Asset == Target && Edge.Status == Status; }) !=
			   Snapshot.GetEdges().end();
	}

	[[nodiscard]] const AssetImportObservation* FindObservation(const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Asset)
	{
		const auto It = std::find_if(Snapshot.GetImportObservations().begin(), Snapshot.GetImportObservations().end(),
			[&](const AssetImportObservation& Observation) { return Observation.Asset == Asset; });
		return It == Snapshot.GetImportObservations().end() ? nullptr : &*It;
	}

	void TestRealFormatExtractionAndStableQueries(TestRunner& Tests)
	{
		AssetDependencyFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		const std::string TextureDocument = "P3\n1 1\n255\n128 128 128\n";
		const auto TextureID = Fixture.WriteManaged("Assets/Textures/albedo.ppm", TextureDocument);
		const auto BinaryBytes = MakeTriangleBuffer();
		const std::string BufferText(reinterpret_cast<const char*>(BinaryBytes.data()), BinaryBytes.size());
		const std::filesystem::path BufferRelativePath = u8"Assets/Models/Buffers/Mesh \u2603.bin";
		const auto BufferPathUtf8 = BufferRelativePath.generic_u8string();
		const std::string BufferRelativeKey(reinterpret_cast<const char*>(BufferPathUtf8.data()), BufferPathUtf8.size());
		const auto BufferID = Fixture.WriteManagedPath(BufferRelativePath, BufferText);
		const std::string BufferRelativeURI = "Buffers/Mesh%20%E2%98%83.bin";
		const auto UnsupportedImageID =
			Fixture.WriteManaged("Assets/Models/Images/not-consumed.bin", "not decoded by the static mesh importer");
		const auto MeshID = Fixture.WriteManaged("Assets/Models/Triangle.gltf", MakeGltfDocument(BufferRelativeURI));
		const std::vector<std::byte> InitialGlbBytes = MakeGlbDocument(MakeGltfDocument(std::nullopt), BinaryBytes);
		const std::string InitialGlbText(reinterpret_cast<const char*>(InitialGlbBytes.data()), InitialGlbBytes.size());
		const auto GlbID = Fixture.WriteManaged("Assets/Models/Embedded.glb", InitialGlbText);
		PF_CHECK(Tests, TextureID && BufferID && UnsupportedImageID && MeshID && GlbID);
		if (!TextureID || !BufferID || !UnsupportedImageID || !MeshID || !GlbID) return;

		MaterialAssetDesc Material;
		Material.BaseColorTexture = *TextureID;
		const auto MaterialBytes = MaterialAssetSerializer::Serialize(Material);
		PF_CHECK(Tests, MaterialBytes.has_value());
		if (!MaterialBytes) return;
		const auto MaterialID = Fixture.WriteManaged("Assets/Materials/Surface.material", *MaterialBytes);
		PF_CHECK(Tests, MaterialID.has_value());
		if (!MaterialID) return;

		Scene SourceScene;
		const auto SceneEntity = SourceScene.CreateEntity("Dependency fixture");
		PF_CHECK(Tests, SceneEntity.has_value());
		if (!SceneEntity) return;
		PF_CHECK(Tests, SceneEntity->SetMeshRenderer(MeshRendererComponent{ *MeshID, *MaterialID }).has_value());
		const std::string SceneRelativePath = "Assets/Scenes/Reference.scene";
		const std::filesystem::path ScenePath = Fixture.Root / SceneRelativePath;
		std::error_code FileError;
		std::filesystem::create_directories(ScenePath.parent_path(), FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		PF_CHECK(Tests, SceneSerializer::SaveToFile(SourceScene, ScenePath).has_value());
		const auto SceneID = AssetMetadataSerializer::CreateForNewAsset(ScenePath);
		PF_CHECK(Tests, SceneID.has_value());
		if (!SceneID) return;

		const auto MissingAssetID = UUID::Generate();
		PF_CHECK(Tests, MissingAssetID.has_value());
		if (!MissingAssetID) return;
		Scene InvalidReferencesScene;
		const auto InvalidReferencesEntity = InvalidReferencesScene.CreateEntity("Invalid reference fixture");
		PF_CHECK(Tests, InvalidReferencesEntity.has_value());
		if (!InvalidReferencesEntity) return;
		PF_CHECK(Tests, InvalidReferencesEntity->SetMeshRenderer(MeshRendererComponent{ *TextureID, *MissingAssetID }).has_value());
		const std::filesystem::path InvalidReferencesPath = Fixture.Root / "Assets/Scenes/InvalidReferences.scene";
		PF_CHECK(Tests, SceneSerializer::SaveToFile(InvalidReferencesScene, InvalidReferencesPath).has_value());
		const auto InvalidReferencesID = AssetMetadataSerializer::CreateForNewAsset(InvalidReferencesPath);
		PF_CHECK(Tests, InvalidReferencesID.has_value());
		if (!InvalidReferencesID) return;

		const std::string PrefabRelativePath = "Assets/Prefabs/Reference.prefab";
		const std::filesystem::path PrefabPath = Fixture.Root / PrefabRelativePath;
		std::filesystem::create_directories(PrefabPath.parent_path(), FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		PF_CHECK(Tests, PrefabSerializer::SaveToFile(SourceScene, *SceneEntity, PrefabPath).has_value());
		const auto PrefabID = AssetMetadataSerializer::CreateForNewAsset(PrefabPath);
		PF_CHECK(Tests, PrefabID.has_value());
		if (!PrefabID) return;

		const auto Built = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 1);
		PF_CHECK(Tests, Built.has_value());
		if (!Built) return;
		const AssetDependencyCatalogSnapshot& Snapshot = **Built;
		PF_CHECK(Tests, Snapshot.GetProjectID() == Fixture.ProjectID);
		PF_CHECK(Tests, Snapshot.GetGeneration() == 1);
		PF_CHECK(Tests, Snapshot.GetRegistry().Find(*MeshID).has_value());
		PF_CHECK(Tests, Snapshot.GetRegistry().Find(*GlbID).has_value());
		PF_CHECK(Tests, HasDependency(Snapshot, *MaterialID, *TextureID, AssetDependencyEdgeKind::RuntimeReference));
		const auto MaterialDependencies = Snapshot.GetDirectDependencies(ManagedNode(Snapshot, *MaterialID));
		PF_CHECK(Tests, MaterialDependencies.size() == 1);
		PF_CHECK(Tests, std::none_of(MaterialDependencies.begin(), MaterialDependencies.end(),
							[](const AssetDependencyEdge& Edge) { return Edge.Status == AssetDependencyStatus::Missing; }));
		PF_CHECK(Tests, HasDependencyStatus(Snapshot, InvalidReferencesID->ID, *TextureID, AssetDependencyStatus::WrongType));
		PF_CHECK(Tests, HasDependencyStatus(Snapshot, InvalidReferencesID->ID, *MissingAssetID, AssetDependencyStatus::Missing));
		PF_CHECK(Tests, HasDependency(Snapshot, SceneID->ID, *MeshID, AssetDependencyEdgeKind::RuntimeReference));
		PF_CHECK(Tests, HasDependency(Snapshot, SceneID->ID, *MaterialID, AssetDependencyEdgeKind::RuntimeReference));
		PF_CHECK(Tests, HasDependency(Snapshot, PrefabID->ID, *MeshID, AssetDependencyEdgeKind::RuntimeReference));
		PF_CHECK(Tests, HasDependency(Snapshot, PrefabID->ID, *MaterialID, AssetDependencyEdgeKind::RuntimeReference));
		PF_CHECK(Tests, HasSourceDependency(Snapshot, *MeshID, BufferRelativeKey, AssetDependencyEdgeKind::ImportInput));
		const auto MeshEdges = Snapshot.GetDirectDependencies(ManagedNode(Snapshot, *MeshID));
		PF_CHECK(Tests,
			std::any_of(MeshEdges.begin(), MeshEdges.end(), [](const AssetDependencyEdge& Edge)
				{ return Edge.OriginalReference == "Images/not-consumed.bin" && Edge.Status == AssetDependencyStatus::Unsupported; }));
		PF_CHECK(Tests, std::any_of(MeshEdges.begin(), MeshEdges.end(), [](const AssetDependencyEdge& Edge)
							{ return Edge.OriginalReference == "Images/missing.bin" && Edge.Status == AssetDependencyStatus::Missing; }));
		const AssetImportObservation* InitialMeshObservation = FindObservation(Snapshot, *MeshID);
		const AssetImportObservation* InitialGlbObservation = FindObservation(Snapshot, *GlbID);
		PF_CHECK(Tests, InitialMeshObservation != nullptr && InitialGlbObservation != nullptr);
		if (!InitialMeshObservation || !InitialGlbObservation) return;
		PF_CHECK(Tests, InitialMeshObservation->InputFingerprint.Algorithm == InitialGlbObservation->InputFingerprint.Algorithm);
		PF_CHECK(Tests, InitialMeshObservation->ResultState == AssetImportResultState::NoSuccessfulResultRecorded);
		PF_CHECK(Tests, InitialGlbObservation->ResultState == AssetImportResultState::NoSuccessfulResultRecorded);
		PF_CHECK(Tests, !InitialMeshObservation->SuccessfulResultFingerprint.has_value());

		const auto TextureReferencers = Snapshot.GetDirectReferencers(AssetDependencyNode{ *TextureID, {}, {} });
		PF_CHECK(Tests, !TextureReferencers.empty());
		PF_CHECK(Tests, std::any_of(TextureReferencers.begin(), TextureReferencers.end(),
							[&](const AssetDependencyEdge& Edge) { return Edge.Referrer.Asset == *MaterialID; }));
		const auto MeshReferencers = Snapshot.GetDirectReferencers(AssetDependencyNode{ *MeshID, {}, {} });
		PF_CHECK(Tests, MeshReferencers.size() == 2);
		PF_CHECK(Tests, std::count_if(MeshReferencers.begin(), MeshReferencers.end(), [&](const AssetDependencyEdge& Edge)
							{ return Edge.Referrer.Asset == SceneID->ID || Edge.Referrer.Asset == PrefabID->ID; }) == 2);

		const auto MeshSubresources = std::find_if(Snapshot.GetSubresources().begin(), Snapshot.GetSubresources().end(),
			[&](const AssetSubresourceRecord& Record) { return Record.Identity.Asset == *MeshID; });
		const auto SubresourceCount = std::count_if(Snapshot.GetSubresources().begin(), Snapshot.GetSubresources().end(),
			[&](const AssetSubresourceRecord& Record) { return Record.Identity.Asset == *MeshID; });
		PF_CHECK(Tests, SubresourceCount == 2);
		PF_CHECK(Tests, MeshSubresources != Snapshot.GetSubresources().end());
		if (SubresourceCount == 2 && MeshSubresources != Snapshot.GetSubresources().end())
		{
			PF_CHECK(Tests, !MeshSubresources->Identity.SubresourceKey.empty());
			PF_CHECK(Tests, MeshSubresources->Identity.SubresourceKey != std::next(MeshSubresources)->Identity.SubresourceKey);
		}

		const auto Rebuilt = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 2);
		PF_CHECK(Tests, Rebuilt.has_value());
		if (!Rebuilt) return;
		std::vector<std::string> FirstEdgeKeys;
		std::vector<std::string> SecondEdgeKeys;
		for (const AssetDependencyEdge& Edge : Snapshot.GetEdges())
			FirstEdgeKeys.push_back(EdgeKey(Edge));
		for (const AssetDependencyEdge& Edge : (*Rebuilt)->GetEdges())
			SecondEdgeKeys.push_back(EdgeKey(Edge));
		PF_CHECK(Tests, FirstEdgeKeys == SecondEdgeKeys);
		const std::set<std::string> UniqueEdgeKeys(FirstEdgeKeys.begin(), FirstEdgeKeys.end());
		PF_CHECK(Tests, UniqueEdgeKeys.size() == FirstEdgeKeys.size());
		PF_CHECK(Tests, Snapshot.GetSubresources().size() == (*Rebuilt)->GetSubresources().size());
		for (size_t Index = 0; Index < Snapshot.GetSubresources().size() && Index < (*Rebuilt)->GetSubresources().size(); ++Index)
			PF_CHECK(Tests, Snapshot.GetSubresources()[Index].Identity == (*Rebuilt)->GetSubresources()[Index].Identity);

		std::error_code FingerprintError;
		const std::filesystem::path BufferPath = Fixture.Root / BufferRelativePath;
		const auto BufferTimestamp = std::filesystem::last_write_time(BufferPath, FingerprintError);
		PF_CHECK(Tests, !FingerprintError);
		if (FingerprintError) return;
		std::vector<std::byte> ChangedBuffer = BinaryBytes;
		ChangedBuffer.front() ^= std::byte{ 1 };
		const std::string ChangedBufferText(reinterpret_cast<const char*>(ChangedBuffer.data()), ChangedBuffer.size());
		PF_CHECK(Tests, AssetDependencyFixture::WriteBytes(BufferPath, ChangedBufferText));
		std::filesystem::last_write_time(BufferPath, BufferTimestamp, FingerprintError);
		PF_CHECK(Tests, !FingerprintError);
		const auto BufferChangedSnapshot = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 3);
		PF_CHECK(Tests, BufferChangedSnapshot.has_value());
		if (!BufferChangedSnapshot) return;
		const AssetImportObservation* ChangedMeshObservation = FindObservation(**BufferChangedSnapshot, *MeshID);
		PF_CHECK(Tests, ChangedMeshObservation != nullptr);
		if (ChangedMeshObservation) PF_CHECK(Tests, ChangedMeshObservation->InputFingerprint != InitialMeshObservation->InputFingerprint);

		std::vector<std::byte> ChangedGlbBytes = InitialGlbBytes;
		ChangedGlbBytes[ChangedGlbBytes.size() - 3] ^= std::byte{ 1 };
		const std::string ChangedGlbText(reinterpret_cast<const char*>(ChangedGlbBytes.data()), ChangedGlbBytes.size());
		const std::filesystem::path GlbPath = Fixture.Root / "Assets/Models/Embedded.glb";
		const auto GlbTimestamp = std::filesystem::last_write_time(GlbPath, FileError);
		PF_CHECK(Tests, !FileError);
		PF_CHECK(Tests, AssetDependencyFixture::WriteBytes(GlbPath, ChangedGlbText));
		std::filesystem::last_write_time(GlbPath, GlbTimestamp, FileError);
		PF_CHECK(Tests, !FileError);
		const auto GlbChangedSnapshot = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 4);
		PF_CHECK(Tests, GlbChangedSnapshot.has_value());
		if (!GlbChangedSnapshot) return;
		const AssetImportObservation* ChangedGlbObservation = FindObservation(**GlbChangedSnapshot, *GlbID);
		PF_CHECK(Tests, ChangedGlbObservation != nullptr);
		if (ChangedGlbObservation) PF_CHECK(Tests, ChangedGlbObservation->InputFingerprint != InitialGlbObservation->InputFingerprint);
	}

	void TestCatalogPublicationAndFailureRetention(TestRunner& Tests)
	{
		AssetDependencyFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const auto TextureID = Fixture.WriteManaged("Assets/Textures/only.ppm", "P3\n1 1\n255\n1 2 3\n");
		PF_CHECK(Tests, TextureID.has_value());
		if (!TextureID) return;

		const auto FirstRequestID = UUID::Generate();
		PF_CHECK(Tests, FirstRequestID.has_value());
		if (!FirstRequestID) return;
		AssetDependencyCatalog Catalog;
		const auto FirstTicket = Catalog.BeginRequest(*FirstRequestID);
		const auto FirstCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, *FirstRequestID, FirstTicket.Generation);
		PF_CHECK(Tests, FirstCandidate.has_value());
		if (!FirstCandidate) return;
		PF_CHECK(Tests, Catalog.Publish(FirstTicket, *FirstCandidate));
		const auto RetainedSnapshot = Catalog.GetSnapshot();
		PF_CHECK(Tests, RetainedSnapshot == *FirstCandidate);
		PF_CHECK(Tests, RetainedSnapshot && RetainedSnapshot->GetRegistry().Find(*TextureID).has_value());

		const auto SupersededTicket = Catalog.BeginRequest(*FirstRequestID);
		PF_CHECK(Tests, Catalog.GetSnapshot() == RetainedSnapshot);
		PF_CHECK(Tests, Catalog.IsLatestRequest(SupersededTicket));
		PF_CHECK(Tests, !Catalog.IsLatestRequest(FirstTicket));
		PF_CHECK(Tests, !Catalog.Publish(FirstTicket, *FirstCandidate));
		PF_CHECK(Tests, Catalog.GetSnapshot() == RetainedSnapshot);

		const auto NewCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, *FirstRequestID, SupersededTicket.Generation);
		PF_CHECK(Tests, NewCandidate.has_value());
		if (!NewCandidate) return;
		PF_CHECK(Tests, Catalog.Publish(SupersededTicket, *NewCandidate));
		PF_CHECK(Tests, Catalog.GetSnapshot() == *NewCandidate);
		PF_CHECK(Tests, RetainedSnapshot->GetGeneration() == FirstTicket.Generation);
		PF_CHECK(Tests, RetainedSnapshot->GetRegistry().Find(*TextureID).has_value());

		const auto FailedTicket = Catalog.BeginRequest(*FirstRequestID);
		const std::filesystem::path MalformedMaterial = Fixture.Root / "Assets/Materials/Broken.material";
		std::error_code MetadataError;
		std::filesystem::create_directories(MalformedMaterial.parent_path(), MetadataError);
		PF_CHECK(Tests, !MetadataError);
		if (MetadataError) return;
		PF_CHECK(Tests, AssetDependencyFixture::WriteBytes(MalformedMaterial, "{ malformed material"));
		const auto MalformedMetadata = AssetMetadataSerializer::CreateForNewAsset(MalformedMaterial);
		PF_CHECK(Tests, MalformedMetadata.has_value());
		if (!MalformedMetadata) return;
		const auto FailedCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, *FirstRequestID, FailedTicket.Generation);
		PF_CHECK(Tests, !FailedCandidate.has_value());
		PF_CHECK(Tests, Catalog.GetSnapshot() == *NewCandidate);
		PF_CHECK(Tests, Catalog.GetSnapshot()->GetGeneration() == SupersededTicket.Generation);
		PF_CHECK(Tests, Catalog.GetSnapshot()->GetRegistry().Find(*TextureID).has_value());
		std::filesystem::remove(MalformedMaterial, MetadataError);
		PF_CHECK(Tests, !MetadataError);
		std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(MalformedMaterial), MetadataError);
		PF_CHECK(Tests, !MetadataError);

		const auto SecondProjectID = UUID::Generate();
		PF_CHECK(Tests, SecondProjectID.has_value());
		if (!SecondProjectID) return;
		const auto SecondProjectTicket = Catalog.BeginRequest(*SecondProjectID);
		PF_CHECK(Tests, !Catalog.GetSnapshot());
		PF_CHECK(Tests, !Catalog.Publish(FailedTicket, *NewCandidate));
		const auto SecondProjectCandidate =
			AssetDependencyCatalogBuilder::Build(Fixture.Root, *SecondProjectID, SecondProjectTicket.Generation);
		PF_CHECK(Tests, SecondProjectCandidate.has_value());
		if (!SecondProjectCandidate) return;
		PF_CHECK(Tests, Catalog.Publish(SecondProjectTicket, *SecondProjectCandidate));
		PF_CHECK(Tests, Catalog.GetSnapshot()->GetProjectID() == *SecondProjectID);
		PF_CHECK(Tests, RetainedSnapshot->GetProjectID() == *FirstRequestID);
	}
} // namespace

void RunAssetDependencyCatalogTests(TestRunner& Tests)
{
	TestRealFormatExtractionAndStableQueries(Tests);
	TestCatalogPublicationAndFailureRetention(Tests);
}
