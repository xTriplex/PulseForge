#include "TestAssetSupport.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetEditTransaction.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/Detail/GltfRelocation.h"
#include "Assets/GltfMeshImporter.h"
#include "Assets/MaterialAsset.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
	using namespace PulseForge;

	class AssetEditFixture final
	{
	public:
		AssetEditFixture()
		{
			std::error_code Error;
			const auto Temp = std::filesystem::temp_directory_path(Error);
			if (Error) return;
			const auto ID = UUID::Generate();
			const auto Project = UUID::Generate();
			if (!ID || !Project) return;
			Root = Temp / ("PulseForgeAssetEdit-" + ID->ToString());
			ProjectID = *Project;
			std::filesystem::create_directories(Root / "Assets" / "Models", Error);
			if (Error) return;
			std::filesystem::create_directories(Root / "Assets" / "Other", Error);
			if (Error) return;
			std::filesystem::create_directories(Root / "Assets" / "Relocated", Error);
			Valid = !Error && WriteBinary(Root / "Assets" / "Models" / "Triangle.bin", TriangleBytes());
			if (!Valid) return;
			const auto BufferMetadata = AssetMetadataSerializer::CreateForNewAsset(Root / "Assets" / "Models" / "Triangle.bin");
			Valid = BufferMetadata.has_value();
		}

		~AssetEditFixture()
		{
			if (Root.empty()) return;
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}

		[[nodiscard]] AssetID AddMesh(std::string_view Name)
		{
			const std::filesystem::path Path = Root / "Assets" / "Models" / (std::string(Name) + ".gltf");
			const std::string Document =
				R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Triangle.bin","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
			if (!WriteText(Path, Document)) return {};
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? Metadata->ID : AssetID{};
		}

		[[nodiscard]] std::expected<AssetDependencyCatalog::SnapshotPtr, AssetDependencyCatalogError> BuildSnapshot(
			uint64_t Generation = 1) const
		{
			return AssetDependencyCatalogBuilder::Build(Root, ProjectID, Generation);
		}

		[[nodiscard]] std::expected<void, AssetRegistryError> BuildRegistry(AssetRegistry& Registry) const
		{
			return Registry.Rebuild(Root);
		}

		[[nodiscard]] static bool WriteText(const std::filesystem::path& Path, std::string_view Text)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
			Output.close();
			return static_cast<bool>(Output);
		}

		[[nodiscard]] static bool WriteBinary(const std::filesystem::path& Path, std::span<const std::byte> Bytes)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			Output.close();
			return static_cast<bool>(Output);
		}

		[[nodiscard]] static std::optional<std::string> ReadText(const std::filesystem::path& Path)
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) return std::nullopt;
			std::string Result{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad()) return std::nullopt;
			return Result;
		}

		[[nodiscard]] static std::vector<std::byte> TriangleBytes()
		{
			std::vector<std::byte> Bytes;
			for (float Value : std::array<float, 9>{ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
			{
				const uint32_t Bits = std::bit_cast<uint32_t>(Value);
				for (size_t Index = 0; Index < sizeof(Bits); ++Index)
					Bytes.push_back(static_cast<std::byte>((Bits >> (Index * 8)) & 0xff));
			}
			for (uint16_t Index : std::array<uint16_t, 3>{ 0, 1, 2 })
			{
				Bytes.push_back(static_cast<std::byte>(Index & 0xff));
				Bytes.push_back(static_cast<std::byte>(Index >> 8));
			}
			return Bytes;
		}

		std::filesystem::path Root;
		UUID ProjectID;
		bool Valid = false;
	};

	std::expected<AssetEditTransaction::PlanPtr, AssetEditTransactionError> MakeMovePlan(
		AssetEditFixture& Fixture, const AssetID& Asset, uint64_t Generation = 1)
	{
		auto Snapshot = Fixture.BuildSnapshot(Generation);
		if (!Snapshot)
			return std::unexpected(AssetEditTransactionError{
				AssetEditTransactionErrorCode::InvalidPlan, Snapshot.error().Path, Snapshot.error().Message, std::nullopt });
		return AssetEditTransaction::PlanMove(**Snapshot, Asset, "Assets/Other/Renamed.gltf");
	}

	std::optional<AssetRecord> FindRecordByPath(const AssetRegistry& Registry, const std::filesystem::path& Path)
	{
		const std::vector<AssetRecord> Assets = Registry.GetAssets();
		const auto Found =
			std::find_if(Assets.begin(), Assets.end(), [&](const AssetRecord& Record) { return Record.ProjectRelativePath == Path; });
		return Found == Assets.end() ? std::nullopt : std::optional<AssetRecord>(*Found);
	}

	void TestAssetEditPlanIsReadOnly(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const AssetID Mesh = Fixture.AddMesh("Ship");
		PF_CHECK(Tests, !Mesh.IsNil());
		if (Mesh.IsNil()) return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		const auto Source = Fixture.Root / "Assets/Models/Ship.gltf";
		const auto Sidecar = AssetMetadataSerializer::GetSidecarPath(Source);
		const auto SourceBefore = AssetEditFixture::ReadText(Source);
		const auto SidecarBefore = AssetEditFixture::ReadText(Sidecar);
		auto Snapshot = Fixture.BuildSnapshot(7);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const auto Plan = AssetEditTransaction::PlanMove(**Snapshot, Mesh, "Assets/Other/Ship.gltf");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;

		PF_CHECK(Tests, (*Plan)->GetProjectID() == Fixture.ProjectID);
		PF_CHECK(Tests, (*Plan)->GetCatalogGeneration() == 7);
		PF_CHECK(Tests, (*Plan)->GetMapping().Source == "Assets/Models/Ship.gltf");
		PF_CHECK(Tests, (*Plan)->GetMapping().Destination == "Assets/Other/Ship.gltf");
		PF_CHECK(Tests, !(*Plan)->GetAffectedReferences().empty());
		PF_CHECK(Tests, SourceBefore == AssetEditFixture::ReadText(Source));
		PF_CHECK(Tests, SidecarBefore == AssetEditFixture::ReadText(Sidecar));
		PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root / "Assets/Other/Ship.gltf"));
		PF_CHECK(Tests, Registry.Find(Mesh)->ProjectRelativePath == "Assets/Models/Ship.gltf");
	}

	void TestAssetEditPlanRejectsStaleInputsAndMembership(TestRunner& Tests)
	{
		{
			AssetEditFixture Fixture;
			PF_CHECK(Tests, Fixture.Valid);
			if (!Fixture.Valid) return;
			const AssetID Mesh = Fixture.AddMesh("StaleSource");
			AssetRegistry Registry;
			PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
			auto Plan = MakeMovePlan(Fixture, Mesh);
			PF_CHECK(Tests, Plan.has_value());
			if (!Plan) return;
			const auto Buffer = Fixture.Root / "Assets/Models/Triangle.bin";
			const auto Timestamp = std::filesystem::last_write_time(Buffer);
			auto Bytes = AssetEditFixture::TriangleBytes();
			Bytes[0] = std::byte{ 0x20 };
			PF_CHECK(Tests, AssetEditFixture::WriteBinary(Buffer, Bytes));
			std::filesystem::last_write_time(Buffer, Timestamp);
			const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 1, Registry);
			PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::StaleInputs);
			PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/StaleSource.gltf"));
			PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root / "Assets/Other/Renamed.gltf"));
			PF_CHECK(Tests, Registry.Find(Mesh)->ProjectRelativePath == "Assets/Models/StaleSource.gltf");
		}
		{
			AssetEditFixture Fixture;
			PF_CHECK(Tests, Fixture.Valid);
			if (!Fixture.Valid) return;
			const AssetID Mesh = Fixture.AddMesh("StaleMembership");
			AssetRegistry Registry;
			PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
			auto Plan = MakeMovePlan(Fixture, Mesh);
			PF_CHECK(Tests, Plan.has_value());
			if (!Plan) return;
			const auto Added = Fixture.Root / "Assets/Models/Added.bin";
			PF_CHECK(Tests, AssetEditFixture::WriteText(Added, "new managed member"));
			PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(Added).has_value());
			const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 1, Registry);
			PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::StaleInputs);
			PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/StaleMembership.gltf"));
			PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root / "Assets/Other/Renamed.gltf"));
			PF_CHECK(Tests, Registry.Find(Mesh)->ProjectRelativePath == "Assets/Models/StaleMembership.gltf");
		}
		{
			AssetEditFixture Fixture;
			PF_CHECK(Tests, Fixture.Valid);
			if (!Fixture.Valid) return;
			const AssetID Mesh = Fixture.AddMesh("StaleDestination");
			AssetRegistry Registry;
			PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
			auto Plan = MakeMovePlan(Fixture, Mesh);
			PF_CHECK(Tests, Plan.has_value());
			if (!Plan) return;
			const auto Destination = Fixture.Root / "Assets/Other/Renamed.gltf";
			PF_CHECK(Tests, AssetEditFixture::WriteText(Destination, "external destination writer"));
			const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 1, Registry);
			PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::Conflict);
			PF_CHECK(Tests, AssetEditFixture::ReadText(Destination) == std::optional<std::string>("external destination writer"));
			PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/StaleDestination.gltf"));
			PF_CHECK(Tests, Registry.Find(Mesh)->ProjectRelativePath == "Assets/Models/StaleDestination.gltf");
		}
	}

	void TestAssetEditMoveRewritesAndImportsSharedBuffers(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const AssetID First = Fixture.AddMesh("First");
		const AssetID Second = Fixture.AddMesh("Second");
		PF_CHECK(Tests, !First.IsNil() && !Second.IsNil());
		if (First.IsNil() || Second.IsNil()) return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		auto Snapshot = Fixture.BuildSnapshot(1);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const auto MoveFirstPlan = AssetEditTransaction::PlanMove(**Snapshot, First, "Assets/Other/First.gltf");
		PF_CHECK(Tests, MoveFirstPlan.has_value());
		if (!MoveFirstPlan) return;
		PF_CHECK(Tests, (*MoveFirstPlan)->GetRewrittenSources().size() == 1);
		if (!(*MoveFirstPlan)->GetRewrittenSources().empty())
		{
			const std::string& Rewritten = (*MoveFirstPlan)->GetRewrittenSources().front().Contents;
			PF_CHECK(Tests, Rewritten.find("../Models/Triangle.bin") != std::string::npos);
		}
		const auto FirstMove = AssetEditTransaction::ApplyMove(**MoveFirstPlan, Fixture.Root, Fixture.ProjectID, 1, Registry);
		PF_CHECK(Tests, FirstMove.has_value());
		if (!FirstMove) return;
		PF_CHECK(Tests, Registry.Find(First)->ProjectRelativePath == "Assets/Other/First.gltf");
		const auto FirstImport = GltfMeshImporter::ImportStaticPrimitive(First, Fixture.Root, Registry);
		PF_CHECK(Tests, FirstImport && FirstImport->Vertices.size() == 3 && FirstImport->Indices == std::vector<uint32_t>({ 0, 1, 2 }));

		Snapshot = Fixture.BuildSnapshot(2);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const auto Buffer = FindRecordByPath(Registry, "Assets/Models/Triangle.bin");
		PF_CHECK(Tests, Buffer.has_value());
		if (!Buffer) return;
		const auto BufferPlan = AssetEditTransaction::PlanMove(**Snapshot, Buffer->ID, "Assets/Relocated/Triangle.bin");
		PF_CHECK(Tests, BufferPlan.has_value());
		if (!BufferPlan) return;
		PF_CHECK(Tests, (*BufferPlan)->GetAffectedReferences().size() == 2);
		PF_CHECK(Tests, (*BufferPlan)->GetRewrittenSources().size() == 2);
		const auto BufferMove = AssetEditTransaction::ApplyMove(**BufferPlan, Fixture.Root, Fixture.ProjectID, 2, Registry);
		PF_CHECK(Tests, BufferMove.has_value());
		if (!BufferMove) return;
		PF_CHECK(Tests, Registry.Find(Buffer->ID)->ProjectRelativePath == "Assets/Relocated/Triangle.bin");
		const std::array<std::pair<AssetID, std::filesystem::path>, 2> ImportedSources = {
			std::pair{ First, std::filesystem::path("Assets/Other/First.gltf") },
			std::pair{ Second, std::filesystem::path("Assets/Models/Second.gltf") }
		};
		for (const auto& [ID, Path] : ImportedSources)
		{
			const auto Document = AssetEditFixture::ReadText(Fixture.Root / Path);
			PF_CHECK(Tests, Document && Document->find("../Relocated/Triangle.bin") != std::string::npos);
			const auto Imported = GltfMeshImporter::ImportStaticPrimitive(ID, Fixture.Root, Registry);
			PF_CHECK(Tests, Imported && Imported->Vertices.size() == 3 && Imported->Indices == std::vector<uint32_t>({ 0, 1, 2 }));
		}
	}

	void TestSelectedAssetBatchMovePreservesReferencesAndRollsBack(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const AssetID FirstMesh = Fixture.AddMesh("BatchFirst");
		const std::filesystem::path SecondBufferPath = Fixture.Root / "Assets/Other/Alternate.bin";
		PF_CHECK(Tests, AssetEditFixture::WriteBinary(SecondBufferPath, AssetEditFixture::TriangleBytes()));
		const auto SecondBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(SecondBufferPath);
		const std::filesystem::path SecondMeshPath = Fixture.Root / "Assets/Other/BatchSecond.gltf";
		const std::string SecondMeshDocument =
			R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Alternate.bin","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
		PF_CHECK(Tests, AssetEditFixture::WriteText(SecondMeshPath, SecondMeshDocument));
		const auto SecondMeshMetadata = AssetMetadataSerializer::CreateForNewAsset(SecondMeshPath);
		PF_CHECK(Tests, !FirstMesh.IsNil() && SecondBufferMetadata && SecondMeshMetadata);
		if (FirstMesh.IsNil() || !SecondBufferMetadata || !SecondMeshMetadata) return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		const auto FirstBuffer = FindRecordByPath(Registry, "Assets/Models/Triangle.bin");
		PF_CHECK(Tests, FirstBuffer.has_value());
		if (!FirstBuffer) return;
		auto Snapshot = Fixture.BuildSnapshot(4);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const std::vector<AssetID> Selected{ FirstMesh, FirstBuffer->ID, SecondBufferMetadata->ID };
		const auto DuplicateSelection = AssetEditTransaction::PlanMoveAssets(
			**Snapshot, { FirstBuffer->ID, FirstBuffer->ID }, "Assets/Relocated");
		PF_CHECK(Tests, !DuplicateSelection);
		const auto Plan = AssetEditTransaction::PlanMoveAssets(**Snapshot, Selected, "Assets/Relocated");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, (*Plan)->IsAssetBatchMove());
		PF_CHECK(Tests, (*Plan)->GetPathMappings().size() == 3);
		PF_CHECK(Tests, (*Plan)->GetAffectedReferences().size() == 2);
		PF_CHECK(Tests, (*Plan)->GetRewrittenSources().size() == 2);

		const auto RacedDestination = Fixture.Root / "Assets/Relocated/Triangle.bin";
		PF_CHECK(Tests, AssetEditFixture::WriteText(RacedDestination, "destination writer"));
		const auto RejectedRace = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 4, Registry);
		PF_CHECK(Tests, !RejectedRace && RejectedRace.error().Code == AssetEditTransactionErrorCode::Conflict);
		PF_CHECK(Tests, AssetEditFixture::ReadText(RacedDestination) == std::optional<std::string>("destination writer"));
		PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/Triangle.bin"));
		std::error_code RemoveError;
		std::filesystem::remove(RacedDestination, RemoveError);
		PF_CHECK(Tests, !RemoveError);
		const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 4, Registry);
		PF_CHECK(Tests, Applied && Applied->MovedAssets.size() == 3);
		if (!Applied) return;
		PF_CHECK(Tests, Registry.Find(FirstBuffer->ID)->ProjectRelativePath == "Assets/Relocated/Triangle.bin");
		PF_CHECK(Tests, Registry.Find(SecondBufferMetadata->ID)->ProjectRelativePath == "Assets/Relocated/Alternate.bin");
		PF_CHECK(Tests, Registry.Find(FirstMesh)->ProjectRelativePath == "Assets/Relocated/BatchFirst.gltf");
		PF_CHECK(Tests, Registry.Find(SecondMeshMetadata->ID)->ProjectRelativePath == "Assets/Other/BatchSecond.gltf");
		const auto FirstImport = GltfMeshImporter::ImportStaticPrimitive(FirstMesh, Fixture.Root, Registry);
		const auto SecondImport = GltfMeshImporter::ImportStaticPrimitive(SecondMeshMetadata->ID, Fixture.Root, Registry);
		PF_CHECK(Tests, FirstImport && FirstImport->Vertices.size() == 3);
		PF_CHECK(Tests, SecondImport && SecondImport->Vertices.size() == 3);
		auto Reopened = Fixture.BuildSnapshot(5);
		PF_CHECK(Tests, Reopened.has_value());
		if (Reopened)
		{
			for (const AssetDependencyEdge& Edge : (*Reopened)->GetEdges())
				if (Edge.Kind == AssetDependencyEdgeKind::ImportInput &&
					(Edge.Referrer.Asset == FirstMesh || Edge.Referrer.Asset == SecondMeshMetadata->ID))
					PF_CHECK(Tests, Edge.Status == AssetDependencyStatus::Resolved);
		}

		AssetEditFixture RollbackFixture;
		PF_CHECK(Tests, RollbackFixture.Valid);
		if (!RollbackFixture.Valid) return;
		const AssetID RollbackMesh = RollbackFixture.AddMesh("BatchRollback");
		const auto ExtraBuffer = RollbackFixture.Root / "Assets/Other/Extra.bin";
		PF_CHECK(Tests, AssetEditFixture::WriteBinary(ExtraBuffer, AssetEditFixture::TriangleBytes()));
		const auto ExtraMetadata = AssetMetadataSerializer::CreateForNewAsset(ExtraBuffer);
		AssetRegistry RollbackRegistry;
		PF_CHECK(Tests, ExtraMetadata && RollbackFixture.BuildRegistry(RollbackRegistry).has_value());
		if (!ExtraMetadata) return;
		auto RollbackSnapshot = RollbackFixture.BuildSnapshot(9);
		PF_CHECK(Tests, RollbackSnapshot.has_value());
		if (!RollbackSnapshot) return;
		const auto RollbackBuffer = FindRecordByPath(RollbackRegistry, "Assets/Models/Triangle.bin");
		PF_CHECK(Tests, RollbackBuffer.has_value());
		if (!RollbackBuffer) return;
		const auto RollbackPlan = AssetEditTransaction::PlanMoveAssets(
			**RollbackSnapshot, { RollbackMesh, RollbackBuffer->ID, ExtraMetadata->ID }, "Assets/Relocated");
		PF_CHECK(Tests, RollbackPlan.has_value());
		if (!RollbackPlan) return;
		const auto OriginalMesh = AssetEditFixture::ReadText(RollbackFixture.Root / "Assets/Models/BatchRollback.gltf");
		size_t CompletedAssets = 0;
		AssetEditTransactionOptions Options;
		Options.StageHook = [&CompletedAssets](AssetEditTransactionStage Stage, const std::filesystem::path&)
			-> std::expected<void, std::string>
		{
			if (Stage == AssetEditTransactionStage::AfterAssetMove && ++CompletedAssets == 1)
				return std::unexpected("fail after first selected asset move");
			return {};
		};
		const auto Failed = AssetEditTransaction::ApplyMove(
			**RollbackPlan, RollbackFixture.Root, RollbackFixture.ProjectID, 9, RollbackRegistry, Options);
		PF_CHECK(Tests, !Failed && Failed.error().Code == AssetEditTransactionErrorCode::FilesystemFailure);
		PF_CHECK(Tests, std::filesystem::exists(RollbackFixture.Root / "Assets/Models/Triangle.bin"));
		PF_CHECK(Tests, std::filesystem::exists(ExtraBuffer));
		PF_CHECK(Tests, !std::filesystem::exists(RollbackFixture.Root / "Assets/Relocated/Triangle.bin"));
		PF_CHECK(Tests, !std::filesystem::exists(RollbackFixture.Root / "Assets/Relocated/Extra.bin"));
		PF_CHECK(Tests, OriginalMesh == AssetEditFixture::ReadText(RollbackFixture.Root / "Assets/Models/BatchRollback.gltf"));
		PF_CHECK(Tests, RollbackRegistry.Find(RollbackMesh)->ProjectRelativePath == "Assets/Models/BatchRollback.gltf");
		PF_CHECK(Tests, RollbackRegistry.Find(RollbackBuffer->ID)->ProjectRelativePath == "Assets/Models/Triangle.bin");

		Options.StageHook = [](AssetEditTransactionStage Stage, const std::filesystem::path&)
			-> std::expected<void, std::string>
		{
			if (Stage == AssetEditTransactionStage::AfterReferenceWrite)
				return std::unexpected("fail after rewriting a moved selected source");
			return {};
		};
		const auto FailedReferenceUpdate = AssetEditTransaction::ApplyMove(
			**RollbackPlan, RollbackFixture.Root, RollbackFixture.ProjectID, 9, RollbackRegistry, Options);
		PF_CHECK(Tests, !FailedReferenceUpdate &&
			FailedReferenceUpdate.error().Code == AssetEditTransactionErrorCode::FilesystemFailure);
		PF_CHECK(Tests, std::filesystem::exists(RollbackFixture.Root / "Assets/Models/Triangle.bin"));
		PF_CHECK(Tests, std::filesystem::exists(RollbackFixture.Root / "Assets/Other/Extra.bin"));
		PF_CHECK(Tests, std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(
			RollbackFixture.Root / "Assets/Models/BatchRollback.gltf")));
		PF_CHECK(Tests, std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(
			RollbackFixture.Root / "Assets/Models/Triangle.bin")));
		PF_CHECK(Tests, !std::filesystem::exists(RollbackFixture.Root / "Assets/Relocated/BatchRollback.gltf"));
		PF_CHECK(Tests, !std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(
			RollbackFixture.Root / "Assets/Relocated/BatchRollback.gltf")));
		PF_CHECK(Tests, OriginalMesh == AssetEditFixture::ReadText(RollbackFixture.Root / "Assets/Models/BatchRollback.gltf"));
		PF_CHECK(Tests, RollbackRegistry.Find(RollbackMesh)->ProjectRelativePath == "Assets/Models/BatchRollback.gltf");
	}

	void TestAssetEditRollbackAtMutationStages(TestRunner& Tests)
	{
		for (const AssetEditTransactionStage FailureStage : { AssetEditTransactionStage::AfterReferenceWrite,
				 AssetEditTransactionStage::AfterAssetMove, AssetEditTransactionStage::BeforeRegistryPublication })
		{
			AssetEditFixture Fixture;
			PF_CHECK(Tests, Fixture.Valid);
			if (!Fixture.Valid) continue;
			const AssetID First = Fixture.AddMesh("RollbackFirst");
			const AssetID Second = Fixture.AddMesh("RollbackSecond");
			AssetRegistry Registry;
			PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
			auto Snapshot = Fixture.BuildSnapshot(1);
			PF_CHECK(Tests, Snapshot.has_value());
			if (!Snapshot) continue;
			const auto Buffer = FindRecordByPath(Registry, "Assets/Models/Triangle.bin");
			PF_CHECK(Tests, Buffer.has_value());
			if (!Buffer) continue;
			const auto Plan = AssetEditTransaction::PlanMove(**Snapshot, Buffer->ID, "Assets/Relocated/Triangle.bin");
			PF_CHECK(Tests, Plan.has_value());
			if (!Plan) continue;
			const auto FirstBefore = AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/RollbackFirst.gltf");
			const auto SecondBefore = AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/RollbackSecond.gltf");
			AssetEditTransactionOptions Options;
			bool Injected = false;
			Options.StageHook = [&](AssetEditTransactionStage Stage, const std::filesystem::path&) -> std::expected<void, std::string>
			{
				if (!Injected && Stage == FailureStage)
				{
					Injected = true;
					return std::unexpected("injected stage failure");
				}
				return {};
			};
			const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 1, Registry, Options);
			PF_CHECK(Tests, !Applied && Injected);
			PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::FilesystemFailure);
			PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/Triangle.bin"));
			PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root / "Assets/Relocated/Triangle.bin"));
			PF_CHECK(Tests, FirstBefore == AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/RollbackFirst.gltf"));
			PF_CHECK(Tests, SecondBefore == AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/RollbackSecond.gltf"));
			PF_CHECK(Tests, Registry.Find(Buffer->ID)->ProjectRelativePath == "Assets/Models/Triangle.bin");
			PF_CHECK(Tests, Registry.Find(First)->ProjectRelativePath == "Assets/Models/RollbackFirst.gltf");
			PF_CHECK(Tests, Registry.Find(Second)->ProjectRelativePath == "Assets/Models/RollbackSecond.gltf");
		}
	}

	void TestDestinationRaceNeverOverwrites(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const AssetID Mesh = Fixture.AddMesh("DestinationRace");
		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		auto Plan = MakeMovePlan(Fixture, Mesh);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const auto SourceBefore = AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/DestinationRace.gltf");
		const std::filesystem::path Destination = Fixture.Root / "Assets/Other/Renamed.gltf";
		constexpr std::string_view Sentinel = "created by an independent writer";
		AssetEditTransactionOptions Options;
		Options.StageHook = [&](AssetEditTransactionStage Stage, const std::filesystem::path&) -> std::expected<void, std::string>
		{
			if (Stage == AssetEditTransactionStage::BeforeFileMutation && !AssetEditFixture::WriteText(Destination, Sentinel))
				return std::unexpected("could not create the deterministic destination race");
			return {};
		};
		const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 1, Registry, Options);
		PF_CHECK(Tests, !Applied);
		PF_CHECK(Tests, AssetEditFixture::ReadText(Destination) == std::optional<std::string>(Sentinel));
		PF_CHECK(Tests, AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/DestinationRace.gltf") == SourceBefore);
		PF_CHECK(Tests, Registry.Find(Mesh)->ProjectRelativePath == "Assets/Models/DestinationRace.gltf");
	}

	void TestIncompleteRollbackRetainsRecoveryMaterial(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const AssetID Mesh = Fixture.AddMesh("RollbackConflict");
		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		auto Snapshot = Fixture.BuildSnapshot(2);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const auto Buffer = FindRecordByPath(Registry, "Assets/Models/Triangle.bin");
		PF_CHECK(Tests, Buffer.has_value());
		if (!Buffer) return;
		const auto Plan = AssetEditTransaction::PlanMove(**Snapshot, Buffer->ID, "Assets/Relocated/Triangle.bin");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const std::filesystem::path Referrer = Fixture.Root / "Assets/Models/RollbackConflict.gltf";
		AssetEditTransactionOptions Options;
		bool ExternalEditWritten = false;
		Options.StageHook = [&](AssetEditTransactionStage Stage, const std::filesystem::path&) -> std::expected<void, std::string>
		{
			if (Stage == AssetEditTransactionStage::AfterReferenceWrite && !ExternalEditWritten)
			{
				ExternalEditWritten = AssetEditFixture::WriteText(Referrer, "written by another editor");
				if (!ExternalEditWritten) return std::unexpected("could not inject the external reference edit");
				return std::unexpected("stop after the independent edit");
			}
			return {};
		};
		const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 2, Registry, Options);
		PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::RecoveryRequired);
		if (Applied) return;
		PF_CHECK(Tests, Applied.error().RecoveryPath.has_value());
		PF_CHECK(Tests, Applied.error().RecoveryPath && std::filesystem::exists(*Applied.error().RecoveryPath));
		PF_CHECK(Tests, AssetEditFixture::ReadText(Referrer) == std::optional<std::string>("written by another editor"));
		PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/Triangle.bin"));
		PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root / "Assets/Relocated/Triangle.bin"));
		PF_CHECK(Tests, Registry.Find(Buffer->ID)->ProjectRelativePath == "Assets/Models/Triangle.bin");
		auto BlockedPlan = AssetEditTransaction::PlanMove(**Snapshot, Mesh, "Assets/Other/Conflict.gltf");
		PF_CHECK(Tests, !BlockedPlan && BlockedPlan.error().Code == AssetEditTransactionErrorCode::RecoveryRequired);
	}

	void TestSessionSupersessionAndCaseOnlyMove(TestRunner& Tests)
	{
		{
			AssetEditFixture Fixture;
			PF_CHECK(Tests, Fixture.Valid);
			if (!Fixture.Valid) return;
			const AssetID Mesh = Fixture.AddMesh("Superseded");
			AssetRegistry Registry;
			PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
			auto Plan = MakeMovePlan(Fixture, Mesh, 8);
			PF_CHECK(Tests, Plan.has_value());
			if (!Plan) return;
			const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 9, Registry);
			PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::StaleProject);
			PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/Superseded.gltf"));
		}

		{
			AssetEditFixture Fixture;
			PF_CHECK(Tests, Fixture.Valid);
			if (!Fixture.Valid) return;
			const AssetID Mesh = Fixture.AddMesh("CaseOnly");
			AssetRegistry Registry;
			PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
			auto Snapshot = Fixture.BuildSnapshot(4);
			PF_CHECK(Tests, Snapshot.has_value());
			if (!Snapshot) return;
			auto Plan = AssetEditTransaction::PlanMove(**Snapshot, Mesh, "Assets/Models/caseonly.gltf");
			PF_CHECK(Tests, Plan.has_value());
			if (!Plan) return;
			const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 4, Registry);
			PF_CHECK(Tests, Applied.has_value());
			PF_CHECK(Tests, !FindRecordByPath(Registry, "Assets/Models/CaseOnly.gltf").has_value());
			PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/caseonly.gltf"));
			PF_CHECK(Tests, Registry.Find(Mesh)->ProjectRelativePath == "Assets/Models/caseonly.gltf");
		}
	}

	void TestFolderMoveRewritesBothDependencyDirections(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		std::error_code Error;
		std::filesystem::create_directories(Fixture.Root / "Assets" / "Bundle", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto BundleBuffer = Fixture.Root / "Assets/Bundle/Shared.bin";
		PF_CHECK(Tests, AssetEditFixture::WriteBinary(BundleBuffer, AssetEditFixture::TriangleBytes()));
		const auto BufferMetadata = AssetMetadataSerializer::CreateForNewAsset(BundleBuffer);
		PF_CHECK(Tests, BufferMetadata.has_value());
		if (!BufferMetadata) return;
		const std::string Geometry =
			R"({"asset":{"version":"2.0"},"buffers":[{"uri":"URI","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
		auto AddExternalMesh = [&](const std::filesystem::path& Path, std::string_view Uri)
		{
			std::string Document = Geometry;
			const size_t Position = Document.find("URI");
			Document.replace(Position, 3, Uri);
			if (!AssetEditFixture::WriteText(Path, Document)) return AssetID{};
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? Metadata->ID : AssetID{};
		};
		const AssetID InternalOwner = AddExternalMesh(Fixture.Root / "Assets/Bundle/Inside.gltf", "../Models/Triangle.bin");
		const AssetID ExternalOwner = AddExternalMesh(Fixture.Root / "Assets/Models/Outside.gltf", "../Bundle/Shared.bin");
		PF_CHECK(Tests, !InternalOwner.IsNil() && !ExternalOwner.IsNil());
		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		auto Snapshot = Fixture.BuildSnapshot(5);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		auto Plan = AssetEditTransaction::PlanMoveFolder(**Snapshot, "Assets/Bundle", "Assets/Other/Bundle");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, (*Plan)->GetMovedAssets().size() == 2);
		PF_CHECK(Tests, (*Plan)->GetAffectedReferences().size() == 2);
		AssetEditTransactionOptions FolderOptions;
		const auto Applied = AssetEditTransaction::ApplyFolderMove(**Plan, Fixture.Root, Fixture.ProjectID, 5, Registry);
		PF_CHECK(Tests, Applied.has_value());
		if (!Applied) return;
		PF_CHECK(Tests, Registry.Find(BufferMetadata->ID)->ProjectRelativePath == "Assets/Other/Bundle/Shared.bin");
		PF_CHECK(Tests, Registry.Find(InternalOwner)->ProjectRelativePath == "Assets/Other/Bundle/Inside.gltf");
		PF_CHECK(Tests, Registry.Find(ExternalOwner)->ProjectRelativePath == "Assets/Models/Outside.gltf");
		const auto MovedInside = AssetEditFixture::ReadText(Fixture.Root / "Assets/Other/Bundle/Inside.gltf");
		const auto MovedOutside = AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/Outside.gltf");
		PF_CHECK(Tests, MovedInside && MovedInside->find("../../Models/Triangle.bin") != std::string::npos);
		PF_CHECK(Tests, MovedOutside && MovedOutside->find("../Other/Bundle/Shared.bin") != std::string::npos);
		AssetRegistry Reopened;
		PF_CHECK(Tests, Reopened.Rebuild(Fixture.Root).has_value());
		const auto ImportedInside = GltfMeshImporter::ImportStaticPrimitive(InternalOwner, Fixture.Root, Reopened);
		const auto ImportedOutside = GltfMeshImporter::ImportStaticPrimitive(ExternalOwner, Fixture.Root, Reopened);
		PF_CHECK(Tests, ImportedInside.has_value() && ImportedInside->Indices.size() == 3);
		PF_CHECK(Tests, ImportedOutside.has_value() && ImportedOutside->Indices.size() == 3);
	}

	void TestFolderMoveRollbackKeepsOriginalGraph(TestRunner& Tests)
	{
		for (const AssetEditTransactionStage FailureStage : { AssetEditTransactionStage::AfterReferenceWrite,
				 AssetEditTransactionStage::AfterAssetMove, AssetEditTransactionStage::BeforeRegistryPublication })
		{
			AssetEditFixture Fixture;
			PF_CHECK(Tests, Fixture.Valid);
			if (!Fixture.Valid) continue;
			std::error_code Error;
			std::filesystem::create_directories(Fixture.Root / "Assets" / "Bundle", Error);
			PF_CHECK(Tests, !Error);
			if (Error) continue;
			const auto Buffer = Fixture.Root / "Assets/Bundle/Shared.bin";
			PF_CHECK(Tests, AssetEditFixture::WriteBinary(Buffer, AssetEditFixture::TriangleBytes()));
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Buffer);
			PF_CHECK(Tests, Metadata.has_value());
			if (!Metadata) continue;
			const std::string Document =
				R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../Bundle/Shared.bin","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
			const AssetID Owner = [&]()
			{
				const auto Path = Fixture.Root / "Assets/Models/Incoming.gltf";
				if (!AssetEditFixture::WriteText(Path, Document)) return AssetID{};
				const auto OwnerMetadata = AssetMetadataSerializer::CreateForNewAsset(Path);
				return OwnerMetadata ? OwnerMetadata->ID : AssetID{};
			}();
			PF_CHECK(Tests, !Owner.IsNil());
			AssetRegistry Registry;
			PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
			auto Snapshot = Fixture.BuildSnapshot(6);
			PF_CHECK(Tests, Snapshot.has_value());
			if (!Snapshot) continue;
			auto Plan = AssetEditTransaction::PlanMoveFolder(**Snapshot, "Assets/Bundle", "Assets/Other/Bundle");
			PF_CHECK(Tests, Plan.has_value());
			if (!Plan) continue;
			const auto Before = AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/Incoming.gltf");
			AssetEditTransactionOptions Options;
			bool Injected = false;
			Options.StageHook = [&](AssetEditTransactionStage Stage, const std::filesystem::path&) -> std::expected<void, std::string>
			{
				if (!Injected && Stage == FailureStage)
				{
					Injected = true;
					return std::unexpected("injected folder transaction failure");
				}
				return {};
			};
			const auto Applied = AssetEditTransaction::ApplyFolderMove(**Plan, Fixture.Root, Fixture.ProjectID, 6, Registry, Options);
			PF_CHECK(Tests, !Applied && Injected);
			PF_CHECK(Tests, AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/Incoming.gltf") == Before);
			PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Bundle/Shared.bin"));
			PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root / "Assets/Other/Bundle"));
			PF_CHECK(Tests, Registry.Find(Metadata->ID)->ProjectRelativePath == "Assets/Bundle/Shared.bin");
			PF_CHECK(Tests, Registry.Find(Owner)->ProjectRelativePath == "Assets/Models/Incoming.gltf");
		}
	}

	void TestFolderDuplicatePreservesExternalInputsAndRemapsInternalMaterial(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		if (!Fixture.Valid) return;
		std::error_code Error;
		const std::filesystem::path SourceFolder = Fixture.Root / "Assets/Bundle";
		const std::filesystem::path CopyParent = Fixture.Root / "Assets/Copies";
		std::filesystem::create_directories(SourceFolder, Error);
		if (Error) return;
		std::filesystem::create_directories(CopyParent, Error);
		if (Error) return;
		const std::filesystem::path SharedBuffer = Fixture.Root / "Assets/Shared.bin";
		PF_CHECK(Tests, AssetEditFixture::WriteBinary(SharedBuffer, AssetEditFixture::TriangleBytes()));
		const auto SharedMetadata = AssetMetadataSerializer::CreateForNewAsset(SharedBuffer);
		PF_CHECK(Tests, SharedMetadata.has_value());
		if (!SharedMetadata) return;

		const std::filesystem::path MeshPath = SourceFolder / "Mesh.gltf";
		const std::string MeshDocument =
			R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../Shared.bin","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"name":"Mesh","primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
		PF_CHECK(Tests, AssetEditFixture::WriteText(MeshPath, MeshDocument));
		const auto MeshMetadata = AssetMetadataSerializer::CreateForNewAsset(MeshPath);
		PF_CHECK(Tests, MeshMetadata.has_value());
		if (!MeshMetadata) return;

		const std::filesystem::path TexturePath = SourceFolder / "Albedo.png";
		const std::array<std::byte, 4> TextureBytes{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 } };
		PF_CHECK(Tests, AssetEditFixture::WriteBinary(TexturePath, TextureBytes));
		const auto TextureMetadata = AssetMetadataSerializer::CreateForNewAsset(TexturePath);
		PF_CHECK(Tests, TextureMetadata.has_value());
		if (!TextureMetadata) return;

		MaterialAssetDesc Material;
		Material.BaseColorTexture = TextureMetadata->ID;
		const std::filesystem::path MaterialPath = SourceFolder / "Surface.material";
		PF_CHECK(Tests, MaterialAssetSerializer::SaveToFile(Material, MaterialPath).has_value());
		const auto MaterialMetadata = AssetMetadataSerializer::CreateForNewAsset(MaterialPath);
		PF_CHECK(Tests, MaterialMetadata.has_value());
		if (!MaterialMetadata) return;

		const auto Snapshot = Fixture.BuildSnapshot(11);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const std::filesystem::path SameIdentityPath = Fixture.Root / "SameIdentity.gltf";
		PF_CHECK(Tests, AssetEditFixture::WriteText(SameIdentityPath, MeshDocument));
		const auto SameIdentityDuplicate =
			AssetMetadataSerializer::CreateDuplicateForNewAsset(MeshPath, SameIdentityPath, MeshMetadata->ID);
		PF_CHECK(Tests, !SameIdentityDuplicate && SameIdentityDuplicate.error().Code == AssetMetadataErrorCode::InvalidIdentifier);
		const auto Plan = AssetEditTransaction::PlanDuplicateFolder(**Snapshot, "Assets/Bundle", "Assets/Copies/Bundle");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, (*Plan)->GetIdentityRemap().size() == 3);
		AssetRegistry Registry;
		const auto RegistryBuild = Fixture.BuildRegistry(Registry);
		if (!RegistryBuild)
		{
			for (const AssetRegistryIssue& Issue : RegistryBuild.error().Issues)
			{
				std::fprintf(stderr, "%s\n", Issue.Message.c_str());
			}
		}
		PF_CHECK(Tests, RegistryBuild.has_value());
		const auto Duplicated = AssetEditTransaction::ApplyFolderDuplicate(**Plan, Fixture.Root, Fixture.ProjectID, 11, Registry);
		PF_CHECK(Tests, Duplicated.has_value());
		if (!Duplicated) return;
		PF_CHECK(Tests, Duplicated->CreatedAssets.size() == 3);
		const auto Remap = [&](const AssetID& Original) -> std::optional<AssetID>
		{
			const auto It = std::find_if((*Plan)->GetIdentityRemap().begin(), (*Plan)->GetIdentityRemap().end(),
				[&](const auto& Pair) { return Pair.first == Original; });
			return It == (*Plan)->GetIdentityRemap().end() ? std::nullopt : std::optional<AssetID>(It->second);
		};
		const auto NewMeshID = Remap(MeshMetadata->ID);
		const auto NewTextureID = Remap(TextureMetadata->ID);
		const auto NewMaterialID = Remap(MaterialMetadata->ID);
		PF_CHECK(Tests, NewMeshID && NewTextureID && NewMaterialID);
		if (!NewMeshID || !NewTextureID || !NewMaterialID) return;
		const auto RegisteredCopy = Registry.Find(*NewMeshID);
		PF_CHECK(Tests, RegisteredCopy && RegisteredCopy->ProjectRelativePath == "Assets/Copies/Bundle/Mesh.gltf");
		const auto CopiedMaterial = MaterialAssetSerializer::LoadFromFile(Fixture.Root / "Assets/Copies/Bundle/Surface.material");
		PF_CHECK(Tests, CopiedMaterial && CopiedMaterial->BaseColorTexture == *NewTextureID);
		const auto OriginalMaterial = MaterialAssetSerializer::LoadFromFile(MaterialPath);
		PF_CHECK(Tests, OriginalMaterial && OriginalMaterial->BaseColorTexture == TextureMetadata->ID);
		const auto CopiedMetadata =
			AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(Fixture.Root / "Assets/Copies/Bundle/Mesh.gltf"));
		PF_CHECK(Tests, CopiedMetadata && CopiedMetadata->ID == *NewMeshID);
		PF_CHECK(Tests,
			!std::filesystem::exists(AssetImportProvenanceSerializer::GetSidecarPath(Fixture.Root / "Assets/Copies/Bundle/Mesh.gltf")));
		const auto CopiedSource = AssetEditFixture::ReadText(Fixture.Root / "Assets/Copies/Bundle/Mesh.gltf");
		PF_CHECK(Tests, CopiedSource && CopiedSource->find("../../Shared.bin") != std::string::npos);
		AssetRegistry Reopened;
		PF_CHECK(Tests, Reopened.Rebuild(Fixture.Root).has_value());
		const auto OriginalImport = GltfMeshImporter::ImportStaticPrimitive(MeshMetadata->ID, Fixture.Root, Reopened);
		const auto CopyImport = GltfMeshImporter::ImportStaticPrimitive(*NewMeshID, Fixture.Root, Reopened);
		PF_CHECK(Tests, OriginalImport && CopyImport);
		if (OriginalImport && CopyImport)
		{
			PF_CHECK(Tests, OriginalImport->Vertices.size() == CopyImport->Vertices.size());
			PF_CHECK(Tests, OriginalImport->Indices == CopyImport->Indices);
		}
	}

	void TestAssetDuplicateRebasesExternalGltfBuffers(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const AssetID Original = Fixture.AddMesh("DuplicateSource");
		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		auto Snapshot = Fixture.BuildSnapshot(12);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const auto Plan = AssetEditTransaction::PlanDuplicateAsset(**Snapshot, Original, "Assets/Other/Duplicate.gltf");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		PF_CHECK(Tests, (*Plan)->IsAssetDuplicate());
		PF_CHECK(Tests, (*Plan)->GetIdentityRemap().size() == 1);
		PF_CHECK(Tests, (*Plan)->GetAffectedReferences().size() == 1);
		const AssetID Duplicate = (*Plan)->GetIdentityRemap().front().second;
		const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 12, Registry);
		PF_CHECK(Tests, Applied.has_value());
		if (!Applied) return;
		PF_CHECK(Tests, Applied->CreatedAssets.size() == 1 && Applied->CreatedAssets.front().ID == Duplicate);
		PF_CHECK(Tests, Duplicate != Original);
		PF_CHECK(Tests, Registry.Find(Original)->ProjectRelativePath == "Assets/Models/DuplicateSource.gltf");
		PF_CHECK(Tests, Registry.Find(Duplicate)->ProjectRelativePath == "Assets/Other/Duplicate.gltf");
		const auto OriginalText = AssetEditFixture::ReadText(Fixture.Root / "Assets/Models/DuplicateSource.gltf");
		const auto CopiedText = AssetEditFixture::ReadText(Fixture.Root / "Assets/Other/Duplicate.gltf");
		PF_CHECK(Tests, OriginalText && OriginalText->find("Triangle.bin") != std::string::npos);
		PF_CHECK(Tests, CopiedText && CopiedText->find("../Models/Triangle.bin") != std::string::npos);
		PF_CHECK(
			Tests, !std::filesystem::exists(AssetImportProvenanceSerializer::GetSidecarPath(Fixture.Root / "Assets/Other/Duplicate.gltf")));
		AssetRegistry Reopened;
		PF_CHECK(Tests, Reopened.Rebuild(Fixture.Root).has_value());
		const auto Imported = GltfMeshImporter::ImportStaticPrimitive(Duplicate, Fixture.Root, Reopened);
		PF_CHECK(Tests, Imported && Imported->Vertices.size() == 3 && Imported->Indices == std::vector<uint32_t>({ 0, 1, 2 }));
	}

	void TestAssetDuplicateRollsBackBeforeRegistryPublication(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const AssetID Original = Fixture.AddMesh("DuplicateRollback");
		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		auto Snapshot = Fixture.BuildSnapshot(14);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const auto Plan = AssetEditTransaction::PlanDuplicateAsset(**Snapshot, Original, "Assets/Other/RolledBack.gltf");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const AssetID Duplicate = (*Plan)->GetIdentityRemap().front().second;
		AssetEditTransactionOptions Options;
		Options.StageHook = [](AssetEditTransactionStage Stage, const std::filesystem::path&) -> std::expected<void, std::string>
		{
			if (Stage == AssetEditTransactionStage::AfterAssetMove) return std::unexpected("injected duplicate publication failure");
			return {};
		};
		const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 14, Registry, Options);
		PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::FilesystemFailure);
		PF_CHECK(Tests, std::filesystem::exists(Fixture.Root / "Assets/Models/DuplicateRollback.gltf"));
		PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root / "Assets/Other/RolledBack.gltf"));
		PF_CHECK(Tests, !std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(Fixture.Root / "Assets/Other/RolledBack.gltf")));
		PF_CHECK(Tests, Registry.Find(Original).has_value() && !Registry.Find(Duplicate).has_value());
	}

	void TestInterruptedTransactionBlocksNewPlans(TestRunner& Tests)
	{
		AssetEditFixture Fixture;
		if (!Fixture.Valid) return;
		const AssetID Mesh = Fixture.AddMesh("Interrupted");
		const auto Snapshot = Fixture.BuildSnapshot(13);
		PF_CHECK(Tests, !Mesh.IsNil() && Snapshot.has_value());
		if (Mesh.IsNil() || !Snapshot) return;
		const auto Plan = AssetEditTransaction::PlanMove(**Snapshot, Mesh, "Assets/Other/Interrupted.gltf");
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const auto OperationID = UUID::Generate();
		PF_CHECK(Tests, OperationID.has_value());
		if (!OperationID) return;
		const std::filesystem::path RecoveryDirectory = Fixture.Root / (".pf-asset-edit-" + OperationID->ToString());
		std::error_code Error;
		std::filesystem::create_directory(RecoveryDirectory, Error);
		PF_CHECK(Tests, !Error);
		AssetRegistry Registry;
		PF_CHECK(Tests, Fixture.BuildRegistry(Registry).has_value());
		const auto Applied = AssetEditTransaction::ApplyMove(**Plan, Fixture.Root, Fixture.ProjectID, 13, Registry);
		PF_CHECK(Tests, !Applied && Applied.error().Code == AssetEditTransactionErrorCode::RecoveryRequired);
		PF_CHECK(Tests, Registry.Find(Mesh) && std::filesystem::exists(Fixture.Root / "Assets/Models/Interrupted.gltf"));
		PF_CHECK(Tests, std::filesystem::exists(RecoveryDirectory));
	}

	void AppendUInt32LE(std::vector<std::byte>& Bytes, uint32_t Value)
	{
		for (size_t Index = 0; Index < sizeof(Value); ++Index)
			Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
	}

	uint32_t ReadUInt32LE(std::span<const std::byte> Bytes, size_t Offset)
	{
		return std::to_integer<uint8_t>(Bytes[Offset]) | (static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset + 1])) << 8) |
			   (static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset + 2])) << 16) |
			   (static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset + 3])) << 24);
	}

	void AppendGlbChunk(std::vector<std::byte>& Bytes, uint32_t Type, std::span<const std::byte> Payload)
	{
		const size_t PaddedSize = (Payload.size() + 3u) & ~size_t{ 3u };
		AppendUInt32LE(Bytes, static_cast<uint32_t>(PaddedSize));
		AppendUInt32LE(Bytes, Type);
		Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
		Bytes.resize(Bytes.size() + PaddedSize - Payload.size(),
			Type == Assets::Detail::GlbJsonChunk ? static_cast<std::byte>(' ') : std::byte{ 0 });
	}

	std::vector<std::byte> MakeGlbWithOpaqueChunks(std::string JsonText)
	{
		while (JsonText.size() % 4 != 0)
			JsonText.push_back(' ');
		const std::array<std::byte, 8> Binary{ std::byte{ 0x11 }, std::byte{ 0x22 }, std::byte{ 0x33 }, std::byte{ 0x44 },
			std::byte{ 0x55 }, std::byte{ 0x66 }, std::byte{ 0x77 }, std::byte{ 0x88 } };
		const std::array<std::byte, 4> Opaque{ std::byte{ 0xA1 }, std::byte{ 0xB2 }, std::byte{ 0xC3 }, std::byte{ 0xD4 } };
		std::vector<std::byte> Bytes;
		AppendUInt32LE(Bytes, Assets::Detail::GlbMagic);
		AppendUInt32LE(Bytes, 2);
		AppendUInt32LE(Bytes, 0);
		AppendGlbChunk(Bytes, Assets::Detail::GlbJsonChunk, std::as_bytes(std::span(JsonText.data(), JsonText.size())));
		AppendGlbChunk(Bytes, Assets::Detail::GlbBinaryChunk, Binary);
		AppendGlbChunk(Bytes, 0x12345678u, Opaque);
		const uint32_t FileSize = static_cast<uint32_t>(Bytes.size());
		for (size_t Index = 0; Index < sizeof(FileSize); ++Index)
			Bytes[8 + Index] = static_cast<std::byte>((FileSize >> (Index * 8)) & 0xff);
		return Bytes;
	}

	void TestGltfRelocationEncodingAndRepeatedUris(TestRunner& Tests)
	{
		using namespace Assets::Detail;
		const std::string JsonText =
			R"({"asset":{"version":"2.0"},"buffers":[{"uri":"space%20%CE%A9.bin","byteLength":4},{"uri":"space%20%CE%A9.bin","byteLength":4},{"uri":"data:application/octet-stream;base64,AQID","byteLength":3}],"extras":{"keep":"untouched"}})";
		const std::array<GltfBufferPathMapping, 1> Mappings{
			{ { std::filesystem::u8path("Assets/Models/space Ω.bin"), std::filesystem::u8path("Assets/Textures/naïve map.bin") } }
		};
		const auto Rewritten = RewriteGltfBufferUris(JsonText, "Assets/Models/Mesh.gltf", "Assets/Other/Mesh.gltf", Mappings);
		PF_CHECK(Tests, Rewritten.has_value());
		if (!Rewritten) return;
		const auto Document = nlohmann::ordered_json::parse(*Rewritten);
		PF_CHECK(Tests, Document["buffers"].size() == 3);
		PF_CHECK(Tests, Document["buffers"][0]["uri"] == "../Textures/na%C3%AFve%20map.bin");
		PF_CHECK(Tests, Document["buffers"][1]["uri"] == Document["buffers"][0]["uri"]);
		PF_CHECK(Tests, Document["buffers"][2]["uri"] == "data:application/octet-stream;base64,AQID");
		PF_CHECK(Tests, Document["extras"]["keep"] == "untouched");

		const auto Escaping =
			RewriteGltfBufferUris(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../../../outside.bin","byteLength":1}]})",
				"Assets/Models/Mesh.gltf", "Assets/Other/Mesh.gltf", std::span<const GltfBufferPathMapping>{});
		PF_CHECK(Tests, !Escaping.has_value());
		const auto MalformedEscape = RewriteGltfBufferUris(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Bad%Q0.bin","byteLength":1}]})",
			"Assets/Models/Mesh.gltf", "Assets/Other/Mesh.gltf", std::span<const GltfBufferPathMapping>{});
		PF_CHECK(Tests, !MalformedEscape.has_value());
		const auto UnsupportedScheme =
			RewriteGltfBufferUris(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"https://example.test/data.bin","byteLength":1}]})",
				"Assets/Models/Mesh.gltf", "Assets/Other/Mesh.gltf", std::span<const GltfBufferPathMapping>{});
		PF_CHECK(Tests, !UnsupportedScheme.has_value());
	}

	void TestGlbRelocationPreservesBinaryAndUnknownChunks(TestRunner& Tests)
	{
		using namespace Assets::Detail;
		const std::string JsonText = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"buffer.bin","byteLength":8},{"byteLength":8}]})";
		const std::vector<std::byte> Original = MakeGlbWithOpaqueChunks(JsonText);
		std::vector<std::byte> Rewritten;
		const GlbReadRange ReadRange = [&Original](uint64_t Offset, std::span<std::byte> Output)
		{
			if (Offset > Original.size() || Output.size() > Original.size() - static_cast<size_t>(Offset)) return false;
			std::copy_n(Original.begin() + static_cast<std::ptrdiff_t>(Offset), Output.size(), Output.begin());
			return true;
		};
		const std::array<GltfBufferPathMapping, 1> Mappings{ { { "Assets/Models/buffer.bin", "Assets/Textures/a longer buffer.bin" } } };
		const auto Written = RewriteGlbBufferUris(Original.size(), ReadRange, "Assets/Models/Mesh.glb", "Assets/Other/Mesh.glb", Mappings,
			[&Rewritten](std::span<const std::byte> Bytes) -> std::expected<void, std::string>
			{
				Rewritten.insert(Rewritten.end(), Bytes.begin(), Bytes.end());
				return {};
			});
		PF_CHECK(Tests, Written.has_value());
		PF_CHECK(Tests, Written && *Written == Rewritten.size());
		if (!Written) return;
		PF_CHECK(Tests, ReadUInt32LE(Rewritten, 0) == GlbMagic);
		PF_CHECK(Tests, ReadUInt32LE(Rewritten, 8) == Rewritten.size());

		const GlbReadRange RewrittenRead = [&Rewritten](uint64_t Offset, std::span<std::byte> Output)
		{
			if (Offset > Rewritten.size() || Output.size() > Rewritten.size() - static_cast<size_t>(Offset)) return false;
			std::copy_n(Rewritten.begin() + static_cast<std::ptrdiff_t>(Offset), Output.size(), Output.begin());
			return true;
		};
		const auto NewJson = ReadGlbJsonChunk(Rewritten.size(), 1024 * 1024, RewrittenRead);
		PF_CHECK(Tests, NewJson.has_value());
		if (!NewJson) return;
		const auto NewDocument = nlohmann::ordered_json::parse(*NewJson);
		PF_CHECK(Tests, NewDocument["buffers"][0]["uri"] == "../Textures/a%20longer%20buffer.bin");

		std::vector<std::pair<uint32_t, std::vector<std::byte>>> OriginalOpaqueChunks;
		std::vector<std::pair<uint32_t, std::vector<std::byte>>> RewrittenOpaqueChunks;
		const auto CollectOpaqueChunks =
			[](std::span<const std::byte> Bytes, std::vector<std::pair<uint32_t, std::vector<std::byte>>>& Output)
		{
			uint64_t Offset = 12;
			while (Offset + 8 <= Bytes.size())
			{
				const uint32_t Length = ReadUInt32LE(Bytes, static_cast<size_t>(Offset));
				const uint32_t Type = ReadUInt32LE(Bytes, static_cast<size_t>(Offset + 4));
				const uint64_t Payload = Offset + 8;
				if (Payload + Length > Bytes.size()) return false;
				if (Type != GlbJsonChunk)
					Output.emplace_back(Type, std::vector<std::byte>(Bytes.begin() + static_cast<std::ptrdiff_t>(Payload),
												  Bytes.begin() + static_cast<std::ptrdiff_t>(Payload + Length)));
				Offset = Payload + Length;
			}
			return Offset == Bytes.size();
		};
		PF_CHECK(Tests, CollectOpaqueChunks(Original, OriginalOpaqueChunks));
		PF_CHECK(Tests, CollectOpaqueChunks(Rewritten, RewrittenOpaqueChunks));
		PF_CHECK(Tests, OriginalOpaqueChunks == RewrittenOpaqueChunks);
	}
} // namespace

void RunAssetEditTransactionTests(TestRunner& Tests)
{
	TestAssetEditPlanIsReadOnly(Tests);
	TestAssetEditPlanRejectsStaleInputsAndMembership(Tests);
	TestAssetEditMoveRewritesAndImportsSharedBuffers(Tests);
	TestSelectedAssetBatchMovePreservesReferencesAndRollsBack(Tests);
	TestAssetEditRollbackAtMutationStages(Tests);
	TestDestinationRaceNeverOverwrites(Tests);
	TestIncompleteRollbackRetainsRecoveryMaterial(Tests);
	TestSessionSupersessionAndCaseOnlyMove(Tests);
	TestFolderMoveRewritesBothDependencyDirections(Tests);
	TestFolderMoveRollbackKeepsOriginalGraph(Tests);
	TestFolderDuplicatePreservesExternalInputsAndRemapsInternalMaterial(Tests);
	TestAssetDuplicateRebasesExternalGltfBuffers(Tests);
	TestAssetDuplicateRollsBackBeforeRegistryPublication(Tests);
	TestInterruptedTransactionBlocksNewPlans(Tests);
	TestGltfRelocationEncodingAndRepeatedUris(Tests);
	TestGlbRelocationPreservesBinaryAndUnknownChunks(Tests);
}
