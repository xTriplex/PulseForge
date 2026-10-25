#include "TestAssetSupport.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetDependencyExtractor.h"
#include "Assets/GltfMeshImportCoordinator.h"
#include "Assets/GltfMeshImporter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace PulseForge;

	class DependencyCorrectionFixture final
	{
	public:
		DependencyCorrectionFixture()
		{
			const auto Identifier = UUID::Generate();
			if (!Identifier) return;
			ProjectID = *Identifier;
			std::error_code Error;
			Root = std::filesystem::temp_directory_path(Error) / ("PulseForgeDependencyCorrection-" + ProjectID.ToString());
			if (Error) return;
			std::filesystem::create_directories(Root / "Assets", Error);
			Valid = !Error;
		}

		~DependencyCorrectionFixture()
		{
			std::error_code Error;
			if (!Root.empty()) std::filesystem::remove_all(Root, Error);
		}

		[[nodiscard]] std::optional<AssetID> WriteManaged(std::string_view Relative, std::string_view Bytes)
		{
			const std::filesystem::path Path = Root / std::filesystem::u8path(Relative);
			std::error_code Error;
			std::filesystem::create_directories(Path.parent_path(), Error);
			if (Error || !WriteFile(Path, Bytes)) return std::nullopt;
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			if (!Metadata) return std::nullopt;
			return Metadata->ID;
		}

		[[nodiscard]] std::optional<AssetID> WriteMesh(std::string_view Relative, std::string_view BufferUri)
		{
			return WriteManaged(Relative, MakeGltf(BufferUri));
		}

		[[nodiscard]] std::filesystem::path Absolute(std::string_view Relative) const
		{
			return Root / std::filesystem::u8path(Relative);
		}

		[[nodiscard]] AssetRegistry BuildRegistry() const
		{
			AssetRegistry Registry;
			if (!Registry.Rebuild(Root)) return {};
			return Registry;
		}

		[[nodiscard]] static bool WriteFile(const std::filesystem::path& Path, std::string_view Bytes)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
			Output.close();
			return static_cast<bool>(Output);
		}

		[[nodiscard]] static std::string MakeGltf(std::string_view BufferUri)
		{
			return std::string(R"({"asset":{"version":"2.0"},"buffers":[{"uri":")") + std::string(BufferUri) +
				R"(","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
		}

		[[nodiscard]] static std::string MakeTriangleBytes(uint8_t FinalByte = 0)
		{
			std::string Bytes;
			const auto Append32 = [&Bytes](uint32_t Value)
			{
				for (unsigned Shift = 0; Shift < 32; Shift += 8) Bytes.push_back(static_cast<char>((Value >> Shift) & 0xff));
			};
			for (const std::array<float, 3> Position : { std::array<float, 3>{ 0.0f, 0.0f, 0.0f },
					 std::array<float, 3>{ 1.0f, 0.0f, 0.0f }, std::array<float, 3>{ 0.0f, 1.0f, 0.0f } })
				for (const float Value : Position) Append32(std::bit_cast<uint32_t>(Value));
			for (const uint16_t Index : { uint16_t{ 0 }, uint16_t{ 1 }, uint16_t{ 2 } })
			{
				Bytes.push_back(static_cast<char>(Index & 0xff));
				Bytes.push_back(static_cast<char>(Index >> 8));
			}
			Bytes.back() = static_cast<char>(FinalByte);
			return Bytes;
		}

		[[nodiscard]] std::optional<AssetID> WriteMaterial(std::string_view Relative, const AssetID& Texture)
		{
			MaterialAssetDesc Description;
			Description.BaseColorTexture = Texture;
			const auto Data = MaterialAssetSerializer::Serialize(Description);
			if (!Data) return std::nullopt;
			return WriteManaged(Relative, *Data);
		}

		[[nodiscard]] std::optional<AssetID> WriteScene(std::string_view Relative, const AssetID& Mesh, const AssetID& Material)
		{
			Scene Source;
			const auto Object = Source.CreateEntity("Dependency consumer");
			if (!Object || !Object->SetMeshRenderer(MeshRendererComponent{ Mesh, Material })) return std::nullopt;
			const std::filesystem::path Path = Absolute(Relative);
			std::error_code Error;
			std::filesystem::create_directories(Path.parent_path(), Error);
			if (Error || !SceneSerializer::SaveToFile(Source, Path)) return std::nullopt;
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			if (!Metadata) return std::nullopt;
			return Metadata->ID;
		}

		UUID ProjectID;
		std::filesystem::path Root;
		bool Valid = false;
	};

	[[nodiscard]] AssetDependencyNode NodeFor(const AssetID& Asset)
	{
		return { Asset, {}, {} };
	}

	[[nodiscard]] const AssetSubresourceRecord* FindPrimitive(
		const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Asset, uint32_t Mesh, uint32_t Primitive)
	{
		const std::string Key = "mesh/" + std::to_string(Mesh) + "/primitive/" + std::to_string(Primitive);
		const auto It = std::find_if(Snapshot.GetSubresources().begin(), Snapshot.GetSubresources().end(),
			[&](const AssetSubresourceRecord& Record) { return Record.Identity.Asset == Asset && Record.Identity.SubresourceKey == Key; });
		return It == Snapshot.GetSubresources().end() ? nullptr : &*It;
	}

	[[nodiscard]] bool HasImportWork(const AssetDependencyChangeImpact& Impact, const AssetID& Asset,
		AssetDependencyImpactReason Reason)
	{
		return std::find(Impact.ImportWork.begin(), Impact.ImportWork.end(), AssetDependencyImpactEntry{ Asset, Reason }) !=
			Impact.ImportWork.end();
	}

	[[nodiscard]] bool HasRuntimeRefresh(const AssetDependencyChangeImpact& Impact, const AssetID& Asset,
		AssetDependencyImpactReason Reason)
	{
		return std::find(Impact.RuntimeRefresh.begin(), Impact.RuntimeRefresh.end(), AssetDependencyImpactEntry{ Asset, Reason }) !=
			Impact.RuntimeRefresh.end();
	}

	[[nodiscard]] bool ReplaceOnce(std::string& Text, std::string_view Before, std::string_view After)
	{
		const size_t Position = Text.find(Before);
		if (Position == std::string::npos) return false;
		Text.replace(Position, Before.size(), After);
		return true;
	}

	[[nodiscard]] bool SameReceipt(const AssetImportSuccessReceipt& First, const AssetImportSuccessReceipt& Second)
	{
		return First.Recipe.SourceAsset == Second.Recipe.SourceAsset &&
			First.Recipe.SubresourceKey == Second.Recipe.SubresourceKey && First.Recipe.ImporterID == Second.Recipe.ImporterID &&
			First.Recipe.ImporterVersion == Second.Recipe.ImporterVersion &&
			First.Recipe.OutputSchemaVersion == Second.Recipe.OutputSchemaVersion &&
			First.Recipe.EffectiveSettings == Second.Recipe.EffectiveSettings && First.Recipe.TargetID == Second.Recipe.TargetID &&
			First.Recipe.Inputs == Second.Recipe.Inputs && First.Recipe.InputFingerprint == Second.Recipe.InputFingerprint &&
			First.ResultFingerprint == Second.ResultFingerprint && First.ResultIdentity == Second.ResultIdentity;
	}

	void TestSharedBufferImpactAndPrimitiveInputs(TestRunner& Tests)
	{
		DependencyCorrectionFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		const std::string InitialBuffer = DependencyCorrectionFixture::MakeTriangleBytes();
		const auto SharedBuffer = Fixture.WriteManaged("Assets/Models/Shared.bin", InitialBuffer);
		const auto OtherBuffer = Fixture.WriteManaged("Assets/Models/Other.bin", InitialBuffer);
		const auto MeshA = Fixture.WriteMesh("Assets/Models/A.gltf", "Shared.bin");
		const auto MeshB = Fixture.WriteMesh("Assets/Models/B.gltf", "Shared.bin");
		const auto MeshC = Fixture.WriteMesh("Assets/Models/C.gltf", "Other.bin");
		const auto Texture = Fixture.WriteManaged("Assets/Textures/T.ppm", "P3\n1 1\n255\n1 2 3\n");
		const auto UnusedTexture = Fixture.WriteManaged("Assets/Textures/Unused.ppm", "P3\n1 1\n255\n8 8 8\n");
		PF_CHECK(Tests, SharedBuffer && OtherBuffer && MeshA && MeshB && MeshC && Texture && UnusedTexture);
		if (!SharedBuffer || !OtherBuffer || !MeshA || !MeshB || !MeshC || !Texture || !UnusedTexture) return;
		const auto Material = Fixture.WriteMaterial("Assets/Materials/M.material", *Texture);
		PF_CHECK(Tests, Material.has_value());
		if (!Material) return;
		const auto SceneAsset = Fixture.WriteScene("Assets/Scenes/S.scene", *MeshA, *Material);
		PF_CHECK(Tests, SceneAsset.has_value());
		if (!SceneAsset) return;
		const auto AlternateMesh = Fixture.WriteMesh("Assets/Models/Alternate.gltf", "Other.bin");
		PF_CHECK(Tests, AlternateMesh.has_value());
		if (!AlternateMesh) return;

		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Fixture.Absolute("Assets/Models/Shared.bin"), InitialBuffer));
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Fixture.Absolute("Assets/Models/Other.bin"), InitialBuffer));
		const auto Before = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 1);
		PF_CHECK(Tests, Before.has_value());
		if (!Before) return;

		const AssetSubresourceRecord* Primitive = FindPrimitive(**Before, *MeshA, 0, 0);
		PF_CHECK(Tests, Primitive != nullptr);
		if (!Primitive) return;
		PF_CHECK(Tests, Primitive->Classification == AssetSubresourceClassification::SupportedCandidate);
		const auto PrimitiveInputs = (*Before)->GetDirectDependencies(Primitive->Identity);
		PF_CHECK(Tests, PrimitiveInputs.size() == 1);
		if (PrimitiveInputs.size() == 1)
		{
			PF_CHECK(Tests, PrimitiveInputs.front().Kind == AssetDependencyEdgeKind::ImportInput);
			PF_CHECK(Tests, PrimitiveInputs.front().Target.SourceFileKey == "Assets/Models/Shared.bin");
			PF_CHECK(Tests, PrimitiveInputs.front().InheritedSourceInputs);
		}
		PF_CHECK(Tests, Fixture.BuildRegistry().Find(*OtherBuffer).has_value());

		std::error_code Error;
		const auto OriginalTime = std::filesystem::last_write_time(Fixture.Absolute("Assets/Models/Shared.bin"), Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		std::string ChangedBuffer = InitialBuffer;
		ChangedBuffer.back() = static_cast<char>(ChangedBuffer.back() ^ 0x01);
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Fixture.Absolute("Assets/Models/Shared.bin"), ChangedBuffer));
		std::filesystem::last_write_time(Fixture.Absolute("Assets/Models/Shared.bin"), OriginalTime, Error);
		PF_CHECK(Tests, !Error);
		const auto After = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 2);
		PF_CHECK(Tests, After.has_value());
		if (!After) return;
		const AssetDependencyChangeImpact Impact = ComputeAssetDependencyChangeImpact(**Before, **After);
		PF_CHECK(Tests, Impact.ImportWork.size() == 2);
		PF_CHECK(Tests, HasImportWork(Impact, *MeshA, AssetDependencyImpactReason::InputChanged));
		PF_CHECK(Tests, HasImportWork(Impact, *MeshB, AssetDependencyImpactReason::InputChanged));
		PF_CHECK(Tests, !std::any_of(Impact.ImportWork.begin(), Impact.ImportWork.end(),
			[&](const AssetDependencyImpactEntry& Entry) { return Entry.Asset == *MeshC; }));
		PF_CHECK(Tests, std::find(Impact.UnchangedAssets.begin(), Impact.UnchangedAssets.end(), *MeshC) != Impact.UnchangedAssets.end());
		PF_CHECK(Tests, HasRuntimeRefresh(Impact, *SceneAsset, AssetDependencyImpactReason::RuntimeReferenceChanged));
		PF_CHECK(Tests, std::none_of(Impact.ImportWork.begin(), Impact.ImportWork.end(),
			[&](const AssetDependencyImpactEntry& Entry) { return Entry.Asset == *SceneAsset; }));

		const std::string TextureBefore = "P3\n1 1\n255\n1 2 3\n";
		const auto TextureTime = std::filesystem::last_write_time(Fixture.Absolute("Assets/Textures/T.ppm"), Error);
		PF_CHECK(Tests, !Error);
		std::ifstream TextureInput(Fixture.Absolute("Assets/Textures/T.ppm"), std::ios::binary);
		const std::string TextureOriginal(std::istreambuf_iterator<char>(TextureInput), {});
		PF_CHECK(Tests, TextureOriginal == TextureBefore);
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Fixture.Absolute("Assets/Textures/T.ppm"), "P3\n1 1\n255\n3 2 1\n"));
		std::filesystem::last_write_time(Fixture.Absolute("Assets/Textures/T.ppm"), TextureTime, Error);
		PF_CHECK(Tests, !Error);
		std::ifstream TextureChangedInput(Fixture.Absolute("Assets/Textures/T.ppm"), std::ios::binary);
		const std::string TextureChangedBytes(std::istreambuf_iterator<char>(TextureChangedInput), {});
		PF_CHECK(Tests, TextureChangedBytes == "P3\n1 1\n255\n3 2 1\n");
		const auto TextureChanged = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 3);
		PF_CHECK(Tests, TextureChanged.has_value());
		if (!TextureChanged) return;
		const AssetDependencyChangeImpact TextureImpact = ComputeAssetDependencyChangeImpact(**After, **TextureChanged);
		PF_CHECK(Tests, TextureImpact.ImportWork.empty());
		PF_CHECK(Tests, HasRuntimeRefresh(TextureImpact, *Material, AssetDependencyImpactReason::RuntimeReferenceChanged));
		PF_CHECK(Tests, HasRuntimeRefresh(TextureImpact, *SceneAsset, AssetDependencyImpactReason::RuntimeReferenceChanged));
		PF_CHECK(Tests, TextureImpact.RuntimeRefresh.size() == 2);
		PF_CHECK(Tests, !std::any_of(TextureImpact.RuntimeRefresh.begin(), TextureImpact.RuntimeRefresh.end(),
			[&](const AssetDependencyImpactEntry& Entry) { return Entry.Asset == *Texture; }));

		const std::filesystem::path UnusedTexturePath = Fixture.Absolute("Assets/Textures/Unused.ppm");
		const auto UnusedTextureTime = std::filesystem::last_write_time(UnusedTexturePath, Error);
		PF_CHECK(Tests, !Error);
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(UnusedTexturePath, "P3\n1 1\n255\n9 9 9\n"));
		std::filesystem::last_write_time(UnusedTexturePath, UnusedTextureTime, Error);
		PF_CHECK(Tests, !Error);
		const auto UnrelatedChanged = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 4);
		PF_CHECK(Tests, UnrelatedChanged.has_value());
		if (!UnrelatedChanged) return;
		const AssetDependencyChangeImpact UnrelatedImpact =
			ComputeAssetDependencyChangeImpact(**TextureChanged, **UnrelatedChanged);
		PF_CHECK(Tests, UnrelatedImpact.ImportWork.empty());
		PF_CHECK(Tests, UnrelatedImpact.RuntimeRefresh.empty());
		PF_CHECK(Tests, std::find(UnrelatedImpact.UnchangedAssets.begin(), UnrelatedImpact.UnchangedAssets.end(), *MeshA) !=
			UnrelatedImpact.UnchangedAssets.end());
		PF_CHECK(Tests, std::find(UnrelatedImpact.UnchangedAssets.begin(), UnrelatedImpact.UnchangedAssets.end(), *MeshC) !=
			UnrelatedImpact.UnchangedAssets.end());
	}

	void TestBuildRejectsSameSizeChangesAfterExtraction(TestRunner& Tests)
	{
		DependencyCorrectionFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const auto TextureA = Fixture.WriteManaged("Assets/Textures/A.ppm", "P3\n1 1\n255\n1 2 3\n");
		const auto TextureB = Fixture.WriteManaged("Assets/Textures/B.ppm", "P3\n1 1\n255\n4 5 6\n");
		const auto Buffer = Fixture.WriteManaged("Assets/Models/Triangle.bin", DependencyCorrectionFixture::MakeTriangleBytes());
		const auto Mesh = Fixture.WriteMesh("Assets/Models/Triangle.gltf", "Triangle.bin");
		PF_CHECK(Tests, TextureA && TextureB && Buffer && Mesh);
		if (!TextureA || !TextureB || !Buffer || !Mesh) return;
		const auto Material = Fixture.WriteMaterial("Assets/Materials/M.material", *TextureA);
		PF_CHECK(Tests, Material.has_value());
		if (!Material) return;
		const auto SceneAsset = Fixture.WriteScene("Assets/Scenes/S.scene", *Mesh, *Material);
		PF_CHECK(Tests, SceneAsset.has_value());
		if (!SceneAsset) return;
		const auto AlternateMesh = Fixture.WriteMesh("Assets/Models/Alternate.gltf", "Triangle.bin");
		PF_CHECK(Tests, AlternateMesh.has_value());
		if (!AlternateMesh) return;
		const auto BalancedA = Fixture.WriteMaterial("Assets/Materials/BalancedA.material", *TextureA);
		const auto BalancedB = Fixture.WriteMaterial("Assets/Materials/BalancedB.material", *TextureB);
		PF_CHECK(Tests, BalancedA && BalancedB);
		if (!BalancedA || !BalancedB) return;
		const std::filesystem::path BalancedAPath = Fixture.Absolute("Assets/Materials/BalancedA.material");
		const std::filesystem::path BalancedBPath = Fixture.Absolute("Assets/Materials/BalancedB.material");
		std::ifstream BalancedAInput(BalancedAPath, std::ios::binary);
		std::ifstream BalancedBInput(BalancedBPath, std::ios::binary);
		const std::string BalancedABase(std::istreambuf_iterator<char>(BalancedAInput), {});
		const std::string BalancedBBase(std::istreambuf_iterator<char>(BalancedBInput), {});
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(BalancedAPath, BalancedABase + "\n"));
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(BalancedBPath, BalancedBBase + "\n"));
		const std::string BalancedAOriginal = BalancedABase + "\n";
		const std::string BalancedBOriginal = BalancedBBase + "\n";

		const auto Baseline = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 1);
		PF_CHECK(Tests, Baseline.has_value());
		if (!Baseline) return;
		AssetDependencyCatalog Catalog;
		const AssetDependencyCatalogTicket BaselineTicket = Catalog.BeginRequest(Fixture.ProjectID);
		PF_CHECK(Tests, Catalog.Publish(BaselineTicket, *Baseline));
		const auto Retained = Catalog.GetSnapshot();
		PF_CHECK(Tests, Retained == *Baseline);
		if (!Retained) return;
		const auto OldSnapshotStillReferences = [&](const AssetID& Owner, const AssetID& Target)
		{
			return std::any_of(Retained->GetEdges().begin(), Retained->GetEdges().end(),
				[&](const AssetDependencyEdge& Edge) { return Edge.Referrer.Asset == Owner && Edge.Target.Asset == Target; });
		};

		const std::filesystem::path MaterialPath = Fixture.Absolute("Assets/Materials/M.material");
		const auto MaterialOriginalTime = std::filesystem::last_write_time(MaterialPath);
		std::ifstream MaterialInput(MaterialPath, std::ios::binary);
		const std::string MaterialOriginal(std::istreambuf_iterator<char>(MaterialInput), {});
		std::string MaterialMutation = MaterialOriginal;
		PF_CHECK(Tests, ReplaceOnce(MaterialMutation, TextureA->ToString(), TextureB->ToString()));
		bool MaterialHookRan = false;
		bool MaterialMutationWritten = false;
		AssetDependencyCatalogBuildOptions MaterialOptions;
		MaterialOptions.BeforeFinalInputValidation = [&]
		{
			MaterialHookRan = true;
			MaterialMutationWritten = DependencyCorrectionFixture::WriteFile(MaterialPath, MaterialMutation);
			std::error_code Ignored;
			std::filesystem::last_write_time(MaterialPath, MaterialOriginalTime, Ignored);
		};
		const auto MaterialRejected = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 2, MaterialOptions);
		PF_CHECK(Tests, MaterialHookRan && MaterialMutationWritten);
		PF_CHECK(Tests, !MaterialRejected);
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		PF_CHECK(Tests, OldSnapshotStillReferences(*Material, *TextureA));
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(MaterialPath, MaterialOriginal));
		std::error_code Error;
		std::filesystem::last_write_time(MaterialPath, MaterialOriginalTime, Error);
		PF_CHECK(Tests, !Error);

		const std::filesystem::path ScenePath = Fixture.Absolute("Assets/Scenes/S.scene");
		const auto SceneOriginalTime = std::filesystem::last_write_time(ScenePath, Error);
		PF_CHECK(Tests, !Error);
		std::ifstream SceneInput(ScenePath, std::ios::binary);
		const std::string SceneOriginal(std::istreambuf_iterator<char>(SceneInput), {});
		std::string SceneMutation = SceneOriginal;
		PF_CHECK(Tests, ReplaceOnce(SceneMutation, Mesh->ToString(), AlternateMesh->ToString()));
		bool SceneHookRan = false;
		bool SceneMutationWritten = false;
		AssetDependencyCatalogBuildOptions SceneOptions;
		SceneOptions.BeforeFinalInputValidation = [&]
		{
			SceneHookRan = true;
			SceneMutationWritten = DependencyCorrectionFixture::WriteFile(ScenePath, SceneMutation);
			std::error_code Ignored;
			std::filesystem::last_write_time(ScenePath, SceneOriginalTime, Ignored);
		};
		const auto SceneRejected = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 3, SceneOptions);
		PF_CHECK(Tests, SceneHookRan && SceneMutationWritten);
		PF_CHECK(Tests, !SceneRejected);
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		PF_CHECK(Tests, OldSnapshotStillReferences(*SceneAsset, *Mesh));
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(ScenePath, SceneOriginal));
		std::filesystem::last_write_time(ScenePath, SceneOriginalTime, Error);
		PF_CHECK(Tests, !Error);

		const std::filesystem::path BufferPath = Fixture.Absolute("Assets/Models/Triangle.bin");
		const auto BufferOriginalTime = std::filesystem::last_write_time(BufferPath, Error);
		PF_CHECK(Tests, !Error);
		std::string BufferMutation = DependencyCorrectionFixture::MakeTriangleBytes();
		BufferMutation.front() = static_cast<char>(BufferMutation.front() ^ 0x01);
		bool BufferHookRan = false;
		bool BufferMutationWritten = false;
		AssetDependencyCatalogBuildOptions BufferOptions;
		BufferOptions.BeforeFinalInputValidation = [&]
		{
			BufferHookRan = true;
			BufferMutationWritten = DependencyCorrectionFixture::WriteFile(BufferPath, BufferMutation);
			std::error_code Ignored;
			std::filesystem::last_write_time(BufferPath, BufferOriginalTime, Ignored);
		};
		const auto BufferRejected = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 4, BufferOptions);
		PF_CHECK(Tests, BufferHookRan && BufferMutationWritten);
		PF_CHECK(Tests, !BufferRejected);
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		PF_CHECK(Tests, OldSnapshotStillReferences(*SceneAsset, *Mesh));
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(BufferPath, DependencyCorrectionFixture::MakeTriangleBytes()));
		std::filesystem::last_write_time(BufferPath, BufferOriginalTime, Error);
		PF_CHECK(Tests, !Error);

		const auto Sidecar = AssetMetadataSerializer::GetSidecarPath(Fixture.Absolute("Assets/Models/Triangle.gltf"));
		const auto SidecarOriginalTime = std::filesystem::last_write_time(Sidecar, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		std::ifstream SidecarInput(Sidecar, std::ios::binary);
		const std::string SidecarOriginal(std::istreambuf_iterator<char>(SidecarInput), {});
		std::string SidecarMutation = SidecarOriginal;
		const auto ReplacementID = UUID::Generate();
		PF_CHECK(Tests, ReplacementID.has_value());
		if (!ReplacementID) return;
		PF_CHECK(Tests, ReplaceOnce(SidecarMutation, Mesh->ToString(), ReplacementID->ToString()));
		bool SidecarHookRan = false;
		bool SidecarMutationWritten = false;
		AssetDependencyCatalogBuildOptions SidecarOptions;
		SidecarOptions.BeforeFinalInputValidation = [&]
		{
			SidecarHookRan = true;
			SidecarMutationWritten = DependencyCorrectionFixture::WriteFile(Sidecar, SidecarMutation);
			std::error_code Ignored;
			std::filesystem::last_write_time(Sidecar, SidecarOriginalTime, Ignored);
		};
		const auto SidecarRejected = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 5, SidecarOptions);
		PF_CHECK(Tests, SidecarHookRan && SidecarMutationWritten);
		PF_CHECK(Tests, !SidecarRejected);
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		PF_CHECK(Tests, Retained->GetRegistry().Find(*Mesh).has_value());
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Sidecar, SidecarOriginal));
		std::filesystem::last_write_time(Sidecar, SidecarOriginalTime, Error);
		PF_CHECK(Tests, !Error);

		const auto BalancedATime = std::filesystem::last_write_time(BalancedAPath, Error);
		PF_CHECK(Tests, !Error);
		const auto BalancedBTime = std::filesystem::last_write_time(BalancedBPath, Error);
		PF_CHECK(Tests, !Error);
		bool BalancedHookRan = false;
		bool BalancedMutationWritten = false;
		AssetDependencyCatalogBuildOptions BalancedOptions;
		BalancedOptions.BeforeFinalInputValidation = [&]
		{
			BalancedHookRan = true;
			BalancedMutationWritten = DependencyCorrectionFixture::WriteFile(BalancedAPath, BalancedAOriginal + "\n") &&
				DependencyCorrectionFixture::WriteFile(BalancedBPath, BalancedBBase);
			std::error_code Ignored;
			std::filesystem::last_write_time(BalancedAPath, BalancedATime, Ignored);
			std::filesystem::last_write_time(BalancedBPath, BalancedBTime, Ignored);
		};
		PF_CHECK(Tests, (BalancedAOriginal.size() + BalancedBOriginal.size()) ==
			((BalancedAOriginal.size() + 1) + (BalancedBOriginal.size() - 1)));
		const auto BalancedRejected = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 6, BalancedOptions);
		PF_CHECK(Tests, BalancedHookRan && BalancedMutationWritten);
		PF_CHECK(Tests, !BalancedRejected);
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(BalancedAPath, BalancedAOriginal));
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(BalancedBPath, BalancedBOriginal));
		std::filesystem::last_write_time(BalancedAPath, BalancedATime, Error);
		PF_CHECK(Tests, !Error);
		std::filesystem::last_write_time(BalancedBPath, BalancedBTime, Error);
		PF_CHECK(Tests, !Error);
	}

	void TestPublishRejectsCandidateWithChangedInputs(TestRunner& Tests)
	{
		DependencyCorrectionFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const auto TextureA = Fixture.WriteManaged("Assets/Textures/A.ppm", "P3\n1 1\n255\n1 2 3\n");
		const auto TextureB = Fixture.WriteManaged("Assets/Textures/B.ppm", "P3\n1 1\n255\n4 5 6\n");
		PF_CHECK(Tests, TextureA && TextureB);
		if (!TextureA || !TextureB) return;
		const auto Material = Fixture.WriteMaterial("Assets/Materials/M.material", *TextureA);
		PF_CHECK(Tests, Material.has_value());
		if (!Material) return;

		AssetDependencyCatalog Catalog;
		const AssetDependencyCatalogTicket BaselineTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto Baseline = AssetDependencyCatalogBuilder::Build(
			Fixture.Root, Fixture.ProjectID, BaselineTicket.Generation);
		PF_CHECK(Tests, Baseline.has_value());
		if (!Baseline) return;
		PF_CHECK(Tests, Catalog.Publish(BaselineTicket, *Baseline));
		const auto Retained = Catalog.GetSnapshot();
		PF_CHECK(Tests, Retained == *Baseline);
		if (!Retained) return;
		const auto HasOriginalReference = [&]
		{
			return std::any_of(Retained->GetEdges().begin(), Retained->GetEdges().end(),
				[&](const AssetDependencyEdge& Edge)
				{
					return Edge.Referrer.Asset == *Material && Edge.Target.Asset == *TextureA;
				});
		};
		PF_CHECK(Tests, HasOriginalReference());

		const AssetDependencyCatalogTicket CandidateTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto Candidate = AssetDependencyCatalogBuilder::Build(
			Fixture.Root, Fixture.ProjectID, CandidateTicket.Generation);
		PF_CHECK(Tests, Candidate.has_value());
		if (!Candidate) return;

		const std::filesystem::path MaterialPath = Fixture.Absolute("Assets/Materials/M.material");
		std::error_code Error;
		const auto OriginalTime = std::filesystem::last_write_time(MaterialPath, Error);
		PF_CHECK(Tests, !Error);
		std::ifstream MaterialInput(MaterialPath, std::ios::binary);
		const std::string Original(std::istreambuf_iterator<char>(MaterialInput), {});
		std::string Mutation = Original;
		PF_CHECK(Tests, ReplaceOnce(Mutation, TextureA->ToString(), TextureB->ToString()));
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(MaterialPath, Mutation));
		std::filesystem::last_write_time(MaterialPath, OriginalTime, Error);
		PF_CHECK(Tests, !Error);

		PF_CHECK(Tests, !Catalog.Publish(CandidateTicket, *Candidate));
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		PF_CHECK(Tests, HasOriginalReference());
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(MaterialPath, Original));
		std::filesystem::last_write_time(MaterialPath, OriginalTime, Error);
		PF_CHECK(Tests, !Error);
	}

	void TestPrimitiveClassificationTracksImporterSupport(TestRunner& Tests)
	{
		DependencyCorrectionFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		PF_CHECK(Tests, Fixture.WriteManaged("Assets/Models/Triangle.bin", DependencyCorrectionFixture::MakeTriangleBytes()).has_value());
		const auto Valid = Fixture.WriteMesh("Assets/Models/Valid.gltf", "Triangle.bin");
		std::string MissingAttributes = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(MissingAttributes, R"("attributes":{"POSITION":0},)", ""));
		const auto NoAttributes = Fixture.WriteManaged("Assets/Models/NoAttributes.gltf", MissingAttributes);
		std::string BadAccessor = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(BadAccessor, R"("POSITION":0)", R"("POSITION":90)"));
		const auto InvalidAccessor = Fixture.WriteManaged("Assets/Models/InvalidAccessor.gltf", BadAccessor);
		std::string BadMode = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(BadMode, R"("indices":1)", R"("indices":1,"mode":1)"));
		const auto UnsupportedMode = Fixture.WriteManaged("Assets/Models/UnsupportedMode.gltf", BadMode);
		std::string MissingPosition = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(MissingPosition, R"("POSITION":0)", R"("NORMAL":0)"));
		const auto NoPosition = Fixture.WriteManaged("Assets/Models/NoPosition.gltf", MissingPosition);
		std::string BadBufferView = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(BadBufferView, R"("bufferView":0)", R"("bufferView":9)"));
		const auto InvalidBufferView = Fixture.WriteManaged("Assets/Models/InvalidBufferView.gltf", BadBufferView);
		std::string BadBase64 = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(BadBase64, "Triangle.bin", "data:application/octet-stream;base64,A$=="));
		const auto MalformedBase64 = Fixture.WriteManaged("Assets/Models/MalformedBase64.gltf", BadBase64);
		std::string ShortEmbedded = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(ShortEmbedded, "Triangle.bin", "data:application/octet-stream;base64,AA=="));
		const auto ShortEmbeddedData = Fixture.WriteManaged("Assets/Models/ShortEmbedded.gltf", ShortEmbedded);
		std::string RequiredExtension = DependencyCorrectionFixture::MakeGltf("Triangle.bin");
		PF_CHECK(Tests, ReplaceOnce(RequiredExtension, R"("asset":{"version":"2.0"},)",
			R"("asset":{"version":"2.0"},"extensionsRequired":["EXT_unknown_required"],)"));
		const auto UnknownExtension = Fixture.WriteManaged("Assets/Models/UnknownExtension.gltf", RequiredExtension);
		PF_CHECK(Tests, Valid && NoAttributes && InvalidAccessor && UnsupportedMode && NoPosition && InvalidBufferView &&
			MalformedBase64 && ShortEmbeddedData && UnknownExtension);
		if (!Valid || !NoAttributes || !InvalidAccessor || !UnsupportedMode || !NoPosition || !InvalidBufferView ||
			!MalformedBase64 || !ShortEmbeddedData || !UnknownExtension) return;
		const auto Built = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 1);
		PF_CHECK(Tests, Built.has_value());
		if (!Built) return;
		const auto* ValidPrimitive = FindPrimitive(**Built, *Valid, 0, 0);
		const auto* MissingPrimitive = FindPrimitive(**Built, *NoAttributes, 0, 0);
		const auto* AccessorPrimitive = FindPrimitive(**Built, *InvalidAccessor, 0, 0);
		const auto* ModePrimitive = FindPrimitive(**Built, *UnsupportedMode, 0, 0);
		const auto* NoPositionPrimitive = FindPrimitive(**Built, *NoPosition, 0, 0);
		const auto* BufferViewPrimitive = FindPrimitive(**Built, *InvalidBufferView, 0, 0);
		const auto* Base64Primitive = FindPrimitive(**Built, *MalformedBase64, 0, 0);
		const auto* ShortDataPrimitive = FindPrimitive(**Built, *ShortEmbeddedData, 0, 0);
		const auto* ExtensionPrimitive = FindPrimitive(**Built, *UnknownExtension, 0, 0);
		PF_CHECK(Tests, ValidPrimitive && ValidPrimitive->Classification == AssetSubresourceClassification::SupportedCandidate);
		PF_CHECK(Tests, MissingPrimitive && MissingPrimitive->Classification == AssetSubresourceClassification::Invalid);
		PF_CHECK(Tests, AccessorPrimitive && AccessorPrimitive->Classification == AssetSubresourceClassification::Invalid);
		PF_CHECK(Tests, ModePrimitive && ModePrimitive->Classification == AssetSubresourceClassification::Unsupported);
		PF_CHECK(Tests, NoPositionPrimitive && NoPositionPrimitive->Classification == AssetSubresourceClassification::Invalid);
		PF_CHECK(Tests, BufferViewPrimitive && BufferViewPrimitive->Classification == AssetSubresourceClassification::Invalid);
		PF_CHECK(Tests, Base64Primitive && Base64Primitive->Classification == AssetSubresourceClassification::Invalid);
		PF_CHECK(Tests, ShortDataPrimitive && ShortDataPrimitive->Classification == AssetSubresourceClassification::Invalid);
		PF_CHECK(Tests, ExtensionPrimitive && ExtensionPrimitive->Classification == AssetSubresourceClassification::Unsupported);
		AssetRegistry Registry = Fixture.BuildRegistry();
		PF_CHECK(Tests, GltfMeshImporter::ImportStaticPrimitive(*Valid, Fixture.Root, Registry).has_value());
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*NoAttributes, Fixture.Root, Registry));
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*InvalidAccessor, Fixture.Root, Registry));
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*UnsupportedMode, Fixture.Root, Registry));
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*NoPosition, Fixture.Root, Registry));
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*InvalidBufferView, Fixture.Root, Registry));
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*MalformedBase64, Fixture.Root, Registry));
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*ShortEmbeddedData, Fixture.Root, Registry));
		PF_CHECK(Tests, !GltfMeshImporter::ImportStaticPrimitive(*UnknownExtension, Fixture.Root, Registry));
	}

	void TestSettingsCompatibilityAndImportIdentity(TestRunner& Tests)
	{
		DependencyCorrectionFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const std::string Buffer = DependencyCorrectionFixture::MakeTriangleBytes();
		PF_CHECK(Tests, Fixture.WriteManaged("Assets/Models/Triangle.bin", Buffer).has_value());
		const auto Mesh = Fixture.WriteMesh("Assets/Models/Triangle.gltf", "Triangle.bin");
		PF_CHECK(Tests, Mesh.has_value());
		if (!Mesh) return;
		const std::filesystem::path Sidecar = AssetMetadataSerializer::GetSidecarPath(Fixture.Absolute("Assets/Models/Triangle.gltf"));
		const std::string LegacySidecar = "{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
			Mesh->ToString() + "\",\"importSettings\":{}}";
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Sidecar, LegacySidecar));
		std::ifstream BeforeInput(Sidecar, std::ios::binary);
		const std::string BeforeBytes(std::istreambuf_iterator<char>(BeforeInput), {});
		BeforeInput.close();
		const auto LegacyMetadata = AssetMetadataSerializer::LoadFromFile(Sidecar);
		PF_CHECK(Tests, LegacyMetadata && LegacyMetadata->ID == *Mesh && LegacyMetadata->ImportSettingsJson == "{}");
		std::ifstream AfterInput(Sidecar, std::ios::binary);
		const std::string AfterBytes(std::istreambuf_iterator<char>(AfterInput), {});
		AfterInput.close();
		PF_CHECK(Tests, BeforeBytes == AfterBytes);

		const auto DefaultSettings = GltfMeshImportSettings::Parse("{}");
		const auto ExplicitDefault = GltfMeshImportSettings::Parse(
			R"({"gltfStaticMesh":{"version":1,"missingNormalsPolicy":"GenerateSmooth"}})");
		PF_CHECK(Tests, DefaultSettings && ExplicitDefault);
		if (!DefaultSettings || !ExplicitDefault) return;
		const auto DefaultCanonical = DefaultSettings->Canonicalize();
		const auto ExplicitCanonical = ExplicitDefault->Canonicalize();
		PF_CHECK(Tests, DefaultCanonical && ExplicitCanonical && *DefaultCanonical == *ExplicitCanonical);

		AssetRegistry Registry = Fixture.BuildRegistry();
		const auto DefaultProduct = GltfMeshImporter::ProcessStaticPrimitive(*Mesh, Fixture.Root, Registry);
		PF_CHECK(Tests, DefaultProduct.has_value());
		if (!DefaultProduct) return;
		PF_CHECK(Tests, DefaultProduct->Mesh.Vertices.size() == 3);
		PF_CHECK(Tests, DefaultProduct->Recipe.SourceAsset == *Mesh);
		const AssetImportRecipe DefaultRecipe = DefaultProduct->Recipe;
		const std::string ExplicitDefaultObject =
			R"({"gltfStaticMesh":{"version":1,"missingNormalsPolicy":"GenerateSmooth"}})";
		PF_CHECK(Tests, AssetMetadataSerializer::SaveImportSettings(Sidecar, *LegacyMetadata, ExplicitDefaultObject).has_value());
		const auto ExplicitDefaultProduct = GltfMeshImporter::ProcessStaticPrimitive(*Mesh, Fixture.Root, Registry);
		PF_CHECK(Tests, ExplicitDefaultProduct.has_value());
		if (!ExplicitDefaultProduct) return;
		PF_CHECK(Tests, ExplicitDefaultProduct->Recipe.EffectiveSettings == DefaultRecipe.EffectiveSettings);
		PF_CHECK(Tests, ExplicitDefaultProduct->Recipe.InputFingerprint == DefaultRecipe.InputFingerprint);
		const auto SuccessfulImport = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(*Mesh, Fixture.Root, Registry);
		PF_CHECK(Tests, SuccessfulImport.has_value());
		if (!SuccessfulImport) return;
		PF_CHECK(Tests, SuccessfulImport->Receipt.Recipe.SourceAsset == *Mesh);
		PF_CHECK(Tests, SuccessfulImport->Receipt.Recipe.SubresourceKey == "mesh/0/primitive/0");
		PF_CHECK(Tests, !SuccessfulImport->Receipt.ResultFingerprint.DigestHex.empty());
		const std::filesystem::path ReceiptPath =
			AssetImportProvenanceSerializer::GetSidecarPath(Fixture.Absolute("Assets/Models/Triangle.gltf"));
		const auto PersistedReceipt = AssetImportProvenanceSerializer::Load(ReceiptPath);
		PF_CHECK(Tests, PersistedReceipt.has_value());
		if (!PersistedReceipt) return;
		PF_CHECK(Tests, PersistedReceipt->Recipe.InputFingerprint == SuccessfulImport->Receipt.Recipe.InputFingerprint);
		const auto CatalogWithReceipt = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 1);
		PF_CHECK(Tests, CatalogWithReceipt.has_value());
		if (!CatalogWithReceipt) return;
		const auto ReceiptObservation = std::find_if((*CatalogWithReceipt)->GetImportObservations().begin(),
			(*CatalogWithReceipt)->GetImportObservations().end(), [&](const AssetImportObservation& Observation)
			{ return Observation.Asset == *Mesh; });
		PF_CHECK(Tests, ReceiptObservation != (*CatalogWithReceipt)->GetImportObservations().end());
		if (ReceiptObservation != (*CatalogWithReceipt)->GetImportObservations().end())
		{
			PF_CHECK(Tests, ReceiptObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
			PF_CHECK(Tests, ReceiptObservation->LastSuccessfulResult.has_value());
			PF_CHECK(Tests, ReceiptObservation->ArtifactAvailability == AssetImportArtifactAvailability::NotTracked);
		}
		const auto* ImportedPrimitive = FindPrimitive(**CatalogWithReceipt, *Mesh, 0, 0);
		PF_CHECK(Tests, ImportedPrimitive && ImportedPrimitive->Classification == AssetSubresourceClassification::Imported);

		std::ifstream ReceiptInput(ReceiptPath, std::ios::binary);
		const std::string ReceiptBytesBeforeFailure(std::istreambuf_iterator<char>(ReceiptInput), {});
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Fixture.Absolute("Assets/Models/Triangle.gltf"), "{ malformed"));
		const auto FailedReimport = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(*Mesh, Fixture.Root, Registry);
		PF_CHECK(Tests, !FailedReimport);
		if (!FailedReimport)
			PF_CHECK(Tests, FailedReimport.error().Code == GltfMeshImportCoordinatorErrorCode::ImportFailed);
		const auto PreservedReceipt = AssetImportProvenanceSerializer::Load(ReceiptPath);
		PF_CHECK(Tests, PreservedReceipt && SameReceipt(*PreservedReceipt, *PersistedReceipt));
		std::ifstream ReceiptAfterFailureInput(ReceiptPath, std::ios::binary);
		const std::string ReceiptBytesAfterFailure(std::istreambuf_iterator<char>(ReceiptAfterFailureInput), {});
		PF_CHECK(Tests, ReceiptBytesAfterFailure == ReceiptBytesBeforeFailure);
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Fixture.Absolute("Assets/Models/Triangle.gltf"),
			DependencyCorrectionFixture::MakeGltf("Triangle.bin")));
		const std::string ValidBuffer = DependencyCorrectionFixture::MakeTriangleBytes();
		PF_CHECK(Tests, DependencyCorrectionFixture::WriteFile(Fixture.Absolute("Assets/Models/Triangle.bin"), ValidBuffer));
		const auto BeforeSettingsChange = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 1);
		PF_CHECK(Tests, BeforeSettingsChange.has_value());
		if (!BeforeSettingsChange) return;

		const std::string RejectNormalsSettings = R"({"gltfStaticMesh":{"version":1,"missingNormalsPolicy":"Reject"}})";
		PF_CHECK(Tests, AssetMetadataSerializer::SaveImportSettings(Sidecar, *LegacyMetadata, RejectNormalsSettings).has_value());
		const auto ChangedMetadata = AssetMetadataSerializer::LoadFromFile(Sidecar);
		PF_CHECK(Tests, ChangedMetadata && ChangedMetadata->ID == *Mesh);
		if (!ChangedMetadata) return;
		const auto RejectedProduct = GltfMeshImporter::ProcessStaticPrimitive(*Mesh, Fixture.Root, Registry);
		PF_CHECK(Tests, !RejectedProduct.has_value());
		const auto ReceiptAfterSettingsFailure = AssetImportProvenanceSerializer::Load(ReceiptPath);
		PF_CHECK(Tests, ReceiptAfterSettingsFailure && SameReceipt(*ReceiptAfterSettingsFailure, *PersistedReceipt));
		const auto CurrentRecipeSettings = GltfMeshImportSettings::Parse(ChangedMetadata->ImportSettingsJson);
		PF_CHECK(Tests, CurrentRecipeSettings && CurrentRecipeSettings->NormalsPolicy == MissingNormalsPolicy::Reject);
		const auto AfterSettingsChange = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 2);
		PF_CHECK(Tests, AfterSettingsChange.has_value());
		if (!AfterSettingsChange) return;
		const AssetDependencyChangeImpact SettingsImpact =
			ComputeAssetDependencyChangeImpact(**BeforeSettingsChange, **AfterSettingsChange);
		PF_CHECK(Tests, SettingsImpact.ImportWork.size() == 1);
		PF_CHECK(Tests, HasImportWork(SettingsImpact, *Mesh, AssetDependencyImpactReason::SettingsChanged));
		const auto DefaultObservation = std::find_if((*BeforeSettingsChange)->GetImportObservations().begin(),
			(*BeforeSettingsChange)->GetImportObservations().end(), [&](const AssetImportObservation& Observation)
			{ return Observation.Asset == *Mesh; });
		const auto RejectObservation = std::find_if((*AfterSettingsChange)->GetImportObservations().begin(),
			(*AfterSettingsChange)->GetImportObservations().end(), [&](const AssetImportObservation& Observation)
			{ return Observation.Asset == *Mesh; });
		PF_CHECK(Tests, DefaultObservation != (*BeforeSettingsChange)->GetImportObservations().end());
		PF_CHECK(Tests, RejectObservation != (*AfterSettingsChange)->GetImportObservations().end());
		if (DefaultObservation != (*BeforeSettingsChange)->GetImportObservations().end() &&
			RejectObservation != (*AfterSettingsChange)->GetImportObservations().end())
		{
			PF_CHECK(Tests, DefaultObservation->EffectiveSettings == *DefaultCanonical);
			PF_CHECK(Tests, RejectObservation->EffectiveSettings != DefaultObservation->EffectiveSettings);
			PF_CHECK(Tests, DefaultObservation->ImporterVersion == RejectObservation->ImporterVersion);
			PF_CHECK(Tests, DefaultObservation->OutputSchemaVersion == RejectObservation->OutputSchemaVersion);
			PF_CHECK(Tests, DefaultObservation->ResultState == AssetImportResultState::SuccessfulInputsMatch);
			PF_CHECK(Tests, RejectObservation->ResultState == AssetImportResultState::SuccessfulInputsDiffer);
			PF_CHECK(Tests, RejectObservation->LastSuccessfulResult.has_value());
			PF_CHECK(Tests, RejectObservation->ArtifactAvailability == AssetImportArtifactAvailability::NotTracked);
		}
		PF_CHECK(Tests, !DefaultRecipe.InputFingerprint.DigestHex.empty());
		const AssetRegistry ReopenedRegistry = Fixture.BuildRegistry();
		const auto ReopenedCatalog = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 3);
		PF_CHECK(Tests, ReopenedRegistry.Find(*Mesh).has_value());
		PF_CHECK(Tests, ReopenedCatalog.has_value());
		if (ReopenedCatalog)
		{
			const auto ReopenedObservation = std::find_if((*ReopenedCatalog)->GetImportObservations().begin(),
				(*ReopenedCatalog)->GetImportObservations().end(), [&](const AssetImportObservation& Observation)
				{ return Observation.Asset == *Mesh; });
			PF_CHECK(Tests, ReopenedObservation != (*ReopenedCatalog)->GetImportObservations().end());
			if (ReopenedObservation != (*ReopenedCatalog)->GetImportObservations().end() &&
				RejectObservation != (*AfterSettingsChange)->GetImportObservations().end())
			{
				PF_CHECK(Tests, ReopenedObservation->EffectiveSettings == RejectObservation->EffectiveSettings);
				PF_CHECK(Tests, ReopenedObservation->ResultState == AssetImportResultState::SuccessfulInputsDiffer);
				PF_CHECK(Tests, ReopenedObservation->LastSuccessfulResult.has_value());
			}
		}

		const std::filesystem::path FailedReceiptSource = Fixture.Absolute("Assets/Models/ReceiptFailure.gltf");
		const auto FailedReceiptAsset = Fixture.WriteMesh("Assets/Models/ReceiptFailure.gltf", "Triangle.bin");
		PF_CHECK(Tests, FailedReceiptAsset.has_value());
		if (!FailedReceiptAsset) return;
		const AssetRegistry FreshRegistry = Fixture.BuildRegistry();
		const std::filesystem::path FailedReceiptPath = AssetImportProvenanceSerializer::GetSidecarPath(FailedReceiptSource);
		std::error_code Error;
		std::filesystem::create_directory(FailedReceiptPath, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto ReceiptWriteFailure = GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
			*FailedReceiptAsset, Fixture.Root, FreshRegistry);
		PF_CHECK(Tests, !ReceiptWriteFailure);
		if (!ReceiptWriteFailure)
			PF_CHECK(Tests, ReceiptWriteFailure.error().Code == GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed);
		std::filesystem::remove(FailedReceiptPath, Error);
		PF_CHECK(Tests, !Error);
	}
} // namespace

void RunAssetDependencyCorrectionTests(TestRunner& Tests)
{
	TestSharedBufferImpactAndPrimitiveInputs(Tests);
	TestBuildRejectsSameSizeChangesAfterExtraction(Tests);
	TestPublishRejectsCandidateWithChangedInputs(Tests);
	TestPrimitiveClassificationTracksImporterSupport(Tests);
	TestSettingsCompatibilityAndImportIdentity(Tests);
}
