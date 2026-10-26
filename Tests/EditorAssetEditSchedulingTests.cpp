#include "TestEditorSupport.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetEditTransaction.h"
#include "Assets/Detail/AssetEditRecoveryJournal.h"
#include "Assets/MaterialAssetService.h"
#include "Assets/AssetMetadata.h"
#include "Assets/SceneAssetService.h"
#include "Editor/EditorAssetEditScheduler.h"
#include "Editor/EditorMaterialDraftController.h"
#include "Editor/EditorProjectOpening.h"
#include "Editor/EditorSceneEditController.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	class Fixture final
	{
	public:
		Fixture()
		{
			std::error_code Error;
			const UUID ID = *UUID::Generate();
			ProjectID = *UUID::Generate();
			Root = std::filesystem::temp_directory_path(Error) / ("PulseForgeAssetEditSchedule-" + ID.ToString());
			if (Error) return;
			std::filesystem::create_directories(Root / "Assets/Models", Error);
			if (Error) return;
			std::filesystem::create_directories(Root / "Assets/Other", Error);
			if (Error) return;
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
			if (!WriteBytes(Root / "Assets/Models/Triangle.bin", Bytes)) return;
			const auto BufferMetadata = AssetMetadataSerializer::CreateForNewAsset(Root / "Assets/Models/Triangle.bin");
			const std::string Document =
				R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Triangle.bin","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
			if (!BufferMetadata || !WriteText(Root / "Assets/Models/Triangle.gltf", Document)) return;
			const auto MeshMetadata = AssetMetadataSerializer::CreateForNewAsset(Root / "Assets/Models/Triangle.gltf");
			if (!MeshMetadata) return;
			MeshID = MeshMetadata->ID;
			AssetRegistry InitialRegistry;
			if (!InitialRegistry.Rebuild(Root)) return;
			Registry = std::move(InitialRegistry);
			const auto Built = AssetDependencyCatalogBuilder::Build(Root, ProjectID, 1);
			if (!Built) return;
			const auto Planned = AssetEditTransaction::PlanMove(**Built, MeshID, "Assets/Other/Triangle.gltf");
			if (!Planned) return;
			Plan = *Planned;
			Valid = true;
		}

		~Fixture()
		{
			if (Root.empty()) return;
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}

		static bool WriteText(const std::filesystem::path& Path, std::string_view Text)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
			Output.close();
			return static_cast<bool>(Output);
		}

		static bool WriteBytes(const std::filesystem::path& Path, std::span<const std::byte> Bytes)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			Output.close();
			return static_cast<bool>(Output);
		}

		std::filesystem::path Root;
		UUID ProjectID;
		AssetID MeshID;
		AssetEditTransaction::PlanPtr Plan;
		AssetRegistry Registry;
		bool Valid = false;
	};

	void TestAcceptedAssetPlanWaitsForSafeUpdate(TestRunner& Tests)
	{
		Fixture Value;
		PF_CHECK(Tests, Value.Valid);
		if (!Value.Valid) return;

		EditorDeferredOperationQueue Operations;
		const std::filesystem::path Original = Value.Root / "Assets/Models/Triangle.gltf";
		const std::filesystem::path Destination = Value.Root / "Assets/Other/Triangle.gltf";
		const std::filesystem::path NormalizedRoot = std::filesystem::absolute(Value.Root).lexically_normal();
		bool ContextCurrent = true;
		bool FinishedGesture = false;
		bool Applied = false;
		size_t ApplyCount = 0;
		std::vector<std::string> Order;

		// This is the production scheduler used by EditorApplication's confirmation callback.
		EditorAssetEditScheduler::Enqueue(Operations, Value.Plan,
			{ [&](const EditorAssetEditExecutionContext& Context)
				{
					Order.push_back("context");
					return ContextCurrent && Context.ProjectID == Value.ProjectID && Context.ProjectRoot == NormalizedRoot &&
						   Context.Generation == Value.Plan->GetCatalogGeneration();
				},
				[&]
				{
					Order.push_back("finish-edit");
					FinishedGesture = true;
					return true;
				},
				[&](AssetEditTransaction::PlanPtr Plan)
				{
					Order.push_back("apply");
					++ApplyCount;
					Applied =
						AssetEditTransaction::ApplyMove(*Plan, Value.Root, Value.ProjectID, Plan->GetCatalogGeneration(), Value.Registry)
							.has_value();
				},
				[&] { Order.push_back("stale"); } });

		// Render/UI acceptance only retained the immutable plan in the existing update queue.
		PF_CHECK(Tests, Operations.Size() == 1);
		PF_CHECK(Tests, std::filesystem::exists(Original));
		PF_CHECK(Tests, !std::filesystem::exists(Destination));
		PF_CHECK(Tests, Value.Registry.Find(Value.MeshID)->ProjectRelativePath == "Assets/Models/Triangle.gltf");
		PF_CHECK(Tests, !FinishedGesture && !Applied && ApplyCount == 0 && Order.empty());

		// The editor executes one operation in its pre-render safe update phase.
		PF_CHECK(Tests, Operations.ExecuteNext());
		PF_CHECK(Tests, Order == (std::vector<std::string>{ "context", "finish-edit", "apply" }));
		PF_CHECK(Tests, FinishedGesture && Applied && ApplyCount == 1);
		PF_CHECK(Tests, !std::filesystem::exists(Original) && std::filesystem::exists(Destination));
		PF_CHECK(Tests, Value.Registry.Find(Value.MeshID)->ProjectRelativePath == "Assets/Other/Triangle.gltf");
		PF_CHECK(Tests, !Operations.ExecuteNext() && ApplyCount == 1);
	}

	void TestCancelledOrSupersededAssetPlanDoesNotMutate(TestRunner& Tests)
	{
		{
			Fixture Value;
			PF_CHECK(Tests, Value.Valid);
			if (!Value.Valid) return;
			EditorDeferredOperationQueue Operations;
			const auto Original = Value.Root / "Assets/Models/Triangle.gltf";
			const auto Destination = Value.Root / "Assets/Other/Triangle.gltf";
			size_t ApplyCount = 0;
			bool StaleCalled = false;
			EditorAssetEditScheduler::Enqueue(Operations, Value.Plan,
				{ [](const EditorAssetEditExecutionContext&) { return false; }, [] { return true; },
					[&](AssetEditTransaction::PlanPtr) { ++ApplyCount; }, [&] { StaleCalled = true; } });
			PF_CHECK(Tests, Operations.Size() == 1 && std::filesystem::exists(Original));
			PF_CHECK(Tests, Operations.ExecuteNext());
			PF_CHECK(Tests, StaleCalled && ApplyCount == 0);
			PF_CHECK(Tests, std::filesystem::exists(Original) && !std::filesystem::exists(Destination));
		}

		{
			Fixture Value;
			PF_CHECK(Tests, Value.Valid);
			if (!Value.Valid) return;
			EditorDeferredOperationQueue Operations;
			const auto Original = Value.Root / "Assets/Models/Triangle.gltf";
			const auto Destination = Value.Root / "Assets/Other/Triangle.gltf";
			size_t ApplyCount = 0;
			EditorAssetEditScheduler::Enqueue(Operations, Value.Plan,
				{ [](const EditorAssetEditExecutionContext&) { return true; }, [] { return false; },
					[&](AssetEditTransaction::PlanPtr) { ++ApplyCount; }, {} });
			PF_CHECK(Tests, std::filesystem::exists(Original) && !std::filesystem::exists(Destination));
			PF_CHECK(Tests, Operations.ExecuteNext());
			PF_CHECK(Tests, ApplyCount == 0);
			PF_CHECK(Tests, std::filesystem::exists(Original) && !std::filesystem::exists(Destination));
		}

		// Explicit Cancel closes the UI and discards m_PendingAssetMovePlan without enqueueing this scheduler.
	}

	void TestDirtySceneAndMaterialHistorySurviveScheduledMoves(TestRunner& Tests)
	{
		const auto TemporaryID = UUID::Generate();
		PF_CHECK(Tests, TemporaryID.has_value());
		if (!TemporaryID) return;
		std::error_code Error;
		const std::filesystem::path Root =
			std::filesystem::temp_directory_path(Error) / ("PulseForgeDirtyAssetMove-" + TemporaryID->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return;
		struct Cleanup final
		{
			std::filesystem::path Root;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Root, Error);
			}
		} CleanupProject{ Root };

		auto CreatedProject = Project::Create(Root / "DirtyMove.pfproj", "Dirty Asset Move Test");
		PF_CHECK(Tests, CreatedProject.has_value());
		if (!CreatedProject) return;
		Project& ProjectValue = *CreatedProject;
		std::filesystem::create_directories(Root / "Assets/Scenes", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		std::filesystem::create_directories(Root / "Assets/Materials", Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;

		Scene SceneValue;
		PF_CHECK(Tests, SceneValue.CreateEntity("Saved baseline entity").has_value());
		auto SceneAsset = SceneAssetService::Create(ProjectValue.GetAssetRegistry(), Root, "Assets/Scenes/Open.scene", SceneValue);
		PF_CHECK(Tests, SceneAsset.has_value());
		if (!SceneAsset) return;
		const std::filesystem::path OriginalScenePath = Root / SceneAsset->ProjectRelativePath;
		const std::filesystem::path MovedScenePath = Root / "Assets/Scenes/Renamed.scene";
		EditorSceneWorkspaceState Workspace;
		EditorSceneEditController SceneEdits;
		SceneEdits.Reset(&SceneValue);
		const EditorSceneEditController::RestoreCallback RestoreScene = [&](std::string_view State) -> std::expected<void, std::string>
		{ return SceneEdits.Restore(State, SceneValue, Workspace); };
		PF_CHECK(Tests, SceneEdits.Begin(&SceneValue, Workspace, false, "Add unsaved scene entity", RestoreScene).has_value());
		const auto UnsavedEntity = SceneValue.CreateEntity("Unsaved draft entity");
		PF_CHECK(Tests, UnsavedEntity.has_value());
		if (!UnsavedEntity) return;
		const UUID UnsavedEntityID = UnsavedEntity->GetUUID();
		PF_CHECK(Tests, SceneEdits.IsActive() && !std::filesystem::exists(MovedScenePath));

		auto SceneSnapshot = AssetDependencyCatalogBuilder::Build(Root, ProjectValue.GetDescription().ID, 31);
		PF_CHECK(Tests, SceneSnapshot.has_value());
		if (!SceneSnapshot) return;
		auto ScenePlan = AssetEditTransaction::PlanMove(**SceneSnapshot, SceneAsset->ID, "Assets/Scenes/Renamed.scene");
		PF_CHECK(Tests, ScenePlan.has_value());
		if (!ScenePlan) return;
		EditorDeferredOperationQueue SceneOperations;
		bool SceneApplied = false;
		EditorAssetEditScheduler::Enqueue(SceneOperations, *ScenePlan,
			{ [&](const EditorAssetEditExecutionContext& Context)
				{
					return Context.ProjectID == ProjectValue.GetDescription().ID && Context.Generation == 31 &&
						   Context.ProjectRoot == std::filesystem::absolute(Root).lexically_normal();
				},
				[&]
				{
					if (SceneEdits.IsActive() && !SceneEdits.Finish(&SceneValue, Workspace, RestoreScene)) return false;
					return true;
				},
				[&](AssetEditTransaction::PlanPtr Accepted)
				{
					SceneApplied = AssetEditTransaction::ApplyPlan(
						*Accepted, Root, ProjectValue.GetDescription().ID, 31, ProjectValue.GetAssetRegistry())
									   .has_value();
				},
				{} });
		PF_CHECK(Tests, std::filesystem::exists(OriginalScenePath) && !std::filesystem::exists(MovedScenePath));
		PF_CHECK(Tests, SceneEdits.IsActive() && !SceneApplied);
		PF_CHECK(Tests, SceneOperations.ExecuteNext() && SceneApplied);
		PF_CHECK(Tests, !SceneEdits.IsActive() && SceneEdits.IsDirty() && SceneEdits.CanUndo());
		PF_CHECK(Tests, std::filesystem::exists(MovedScenePath) && !std::filesystem::exists(OriginalScenePath));
		PF_CHECK(Tests, ProjectValue.GetAssetRegistry().Find(SceneAsset->ID)->ProjectRelativePath == "Assets/Scenes/Renamed.scene");
		const auto SceneUndo = SceneEdits.Undo(&SceneValue, Workspace, false, RestoreScene);
		PF_CHECK(Tests, SceneUndo && *SceneUndo && !SceneValue.FindEntity(UnsavedEntityID));
		const auto SceneRedo = SceneEdits.Redo(&SceneValue, Workspace, false, RestoreScene);
		PF_CHECK(Tests, SceneRedo && *SceneRedo && SceneValue.FindEntity(UnsavedEntityID));
		PF_CHECK(Tests, SceneAssetService::Save(SceneAsset->ID, Root, ProjectValue.GetAssetRegistry(), SceneValue).has_value());
		PF_CHECK(Tests, !std::filesystem::exists(OriginalScenePath) && std::filesystem::exists(MovedScenePath));
		Scene ReopenedScene;
		PF_CHECK(Tests, SceneAssetService::Load(SceneAsset->ID, Root, ProjectValue.GetAssetRegistry(), ReopenedScene).has_value());
		PF_CHECK(Tests, ReopenedScene.FindEntity(UnsavedEntityID).has_value());

		MaterialAssetDesc MaterialDescription;
		MaterialDescription.BaseColorFactor = { 0.2f, 0.3f, 0.4f, 1.0f };
		auto Material =
			MaterialAssetService::Create(ProjectValue.GetAssetRegistry(), Root, "Assets/Materials/Draft.material", MaterialDescription);
		PF_CHECK(Tests, Material.has_value());
		if (!Material) return;
		EditorDocumentManager Documents;
		const EditorDocumentIdentity MaterialIdentity{ ProjectValue.GetDescription().ID, Material->ID, EditorDocumentKind::Material };
		PF_CHECK(Tests,
			Documents
				.Open(MaterialIdentity, Material->ProjectRelativePath, "Draft.material", EditorDocumentOpenPreference::MainDocumentStrip)
				.has_value());
		EditorMaterialDraftController Drafts;
		auto MaterialDraft = Drafts.LoadDraft(ProjectValue, Material->ID);
		PF_CHECK(Tests, MaterialDraft.has_value());
		if (!MaterialDraft) return;
		PF_CHECK(Tests, Drafts.BeginEdit(ProjectValue, Material->ID, MaterialDraft->get()));
		MaterialDraft->get().BaseColorFactor.g = 0.82f;
		PF_CHECK(Tests, Documents.MarkEdited(MaterialIdentity));
		const auto MaterialSnapshot = AssetDependencyCatalogBuilder::Build(Root, ProjectValue.GetDescription().ID, 32);
		PF_CHECK(Tests, MaterialSnapshot.has_value());
		if (!MaterialSnapshot) return;
		const auto MaterialPlan = AssetEditTransaction::PlanMove(**MaterialSnapshot, Material->ID, "Assets/Materials/Renamed.material");
		PF_CHECK(Tests, MaterialPlan.has_value());
		if (!MaterialPlan) return;
		EditorDeferredOperationQueue MaterialOperations;
		bool MaterialApplied = false;
		EditorAssetEditScheduler::Enqueue(MaterialOperations, *MaterialPlan,
			{ [&](const EditorAssetEditExecutionContext& Context)
				{ return Context.ProjectID == ProjectValue.GetDescription().ID && Context.Generation == 32; },
				[&]
				{
					Drafts.FinalizePendingEdits(false);
					return true;
				},
				[&](AssetEditTransaction::PlanPtr Accepted)
				{
					const auto Applied = AssetEditTransaction::ApplyPlan(
						*Accepted, Root, ProjectValue.GetDescription().ID, 32, ProjectValue.GetAssetRegistry());
					MaterialApplied = Applied.has_value();
					if (MaterialApplied)
						MaterialApplied =
							Documents.UpdateAssetLocation(MaterialIdentity, "Assets/Materials/Renamed.material", "Renamed.material");
				},
				{} });
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID) && !Drafts.CanUndo(ProjectValue, Material->ID));
		PF_CHECK(Tests, MaterialOperations.ExecuteNext() && MaterialApplied);
		PF_CHECK(Tests, Drafts.IsDirty(ProjectValue, Material->ID) && Drafts.CanUndo(ProjectValue, Material->ID));
		PF_CHECK(Tests, Documents.Find(MaterialIdentity)->IsDirty());
		PF_CHECK(Tests, Documents.Find(MaterialIdentity)->ProjectRelativePath == "Assets/Materials/Renamed.material");
		PF_CHECK(Tests, !std::filesystem::exists(Root / "Assets/Materials/Draft.material"));
		const auto MaterialUndo = Drafts.ApplyHistoryStep(ProjectValue, Material->ID, false, Documents);
		PF_CHECK(Tests, MaterialUndo && *MaterialUndo);
		PF_CHECK(Tests, std::abs(Drafts.FindDraft(ProjectValue, Material->ID)->BaseColorFactor.g - 0.3f) < 1.0e-5f);
		const auto MaterialRedo = Drafts.ApplyHistoryStep(ProjectValue, Material->ID, true, Documents);
		PF_CHECK(Tests, MaterialRedo && *MaterialRedo && Drafts.IsDirty(ProjectValue, Material->ID));
		const auto MaterialSaved = Drafts.Save(ProjectValue, Material->ID, Documents);
		PF_CHECK(Tests, MaterialSaved && *MaterialSaved == Root / "Assets/Materials/Renamed.material");
		PF_CHECK(Tests, !std::filesystem::exists(Root / "Assets/Materials/Draft.material"));
		const auto ReopenedMaterial = MaterialAssetService::Load(Material->ID, Root, ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, ReopenedMaterial && std::abs(ReopenedMaterial->BaseColorFactor.g - 0.82f) < 1.0e-5f);
	}

	void TestDirtyReferrerBlocksScheduledRewrite(TestRunner& Tests)
	{
		Fixture Value;
		PF_CHECK(Tests, Value.Valid);
		if (!Value.Valid) return;
		const auto BufferMetadata =
			AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(Value.Root / "Assets/Models/Triangle.bin"));
		PF_CHECK(Tests, BufferMetadata.has_value());
		if (!BufferMetadata) return;
		const std::string ReferrerDocument =
			R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../Models/Triangle.bin","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
		const std::filesystem::path ReferrerSource = Value.Root / "Assets/Other/Incoming.gltf";
		PF_CHECK(Tests, Fixture::WriteText(ReferrerSource, ReferrerDocument));
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(ReferrerSource).has_value());
		PF_CHECK(Tests, Value.Registry.Rebuild(Value.Root).has_value());
		std::error_code DirectoryError;
		std::filesystem::create_directories(Value.Root / "Assets/Relocated", DirectoryError);
		PF_CHECK(Tests, !DirectoryError);
		const auto Snapshot = AssetDependencyCatalogBuilder::Build(Value.Root, Value.ProjectID, 41);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const auto Plan = AssetEditTransaction::PlanMove(**Snapshot, BufferMetadata->ID, "Assets/Relocated/Triangle.bin");
		PF_CHECK(Tests, Plan.has_value() && !(*Plan)->GetRewrittenSources().empty());
		if (!Plan || (*Plan)->GetRewrittenSources().empty()) return;
		EditorDocumentManager Documents;
		const AssetID Referrer = (*Plan)->GetRewrittenSources().front().Asset;
		const std::filesystem::path ReferrerPath = (*Plan)->GetRewrittenSources().front().ProjectRelativePath;
		const EditorDocumentIdentity DirtyReferrer{ Value.ProjectID, Referrer, EditorDocumentKind::Texture };
		PF_CHECK(Tests,
			Documents.Open(DirtyReferrer, ReferrerPath, ReferrerPath.filename().string(), EditorDocumentOpenPreference::MainDocumentStrip)
				.has_value());
		PF_CHECK(Tests, Documents.MarkEdited(DirtyReferrer));
		const std::array<EditorDocumentIdentity, 1> DirtyDocuments{ DirtyReferrer };
		const auto Conflict = FindDirtyAssetEditDocumentConflict(**Plan, DirtyDocuments);
		PF_CHECK(Tests, Conflict && Conflict->Asset == Referrer && Conflict->RewrittenSource == ReferrerPath);
		const auto OriginalBytes = Value.Root / "Assets/Models/Triangle.bin";
		const auto OriginalBuffer = std::filesystem::file_size(OriginalBytes);
		EditorDeferredOperationQueue Operations;
		bool Applied = false;
		bool RejectedDirty = false;
		EditorAssetEditScheduler::Enqueue(Operations, *Plan,
			{ [](const EditorAssetEditExecutionContext&) { return true; }, [] { return true; },
				[&](AssetEditTransaction::PlanPtr Accepted)
				{
					if (FindDirtyAssetEditDocumentConflict(*Accepted, DirtyDocuments))
					{
						RejectedDirty = true;
						return;
					}
					Applied = AssetEditTransaction::ApplyPlan(*Accepted, Value.Root, Value.ProjectID, 41, Value.Registry).has_value();
				},
				{} });
		PF_CHECK(Tests, Operations.ExecuteNext() && RejectedDirty && !Applied);
		PF_CHECK(Tests, std::filesystem::exists(OriginalBytes) && !std::filesystem::exists(Value.Root / "Assets/Relocated/Triangle.bin"));
		PF_CHECK(Tests, std::filesystem::file_size(OriginalBytes) == OriginalBuffer);
		PF_CHECK(Tests, Documents.Find(DirtyReferrer)->IsDirty());
	}

	void TestProjectOpenRecoversCommittedAssetEditBeforePublishingProject(TestRunner& Tests)
	{
		const auto TemporaryID = UUID::Generate();
		PF_CHECK(Tests, TemporaryID.has_value());
		if (!TemporaryID) return;
		std::error_code Error;
		const std::filesystem::path Root =
			std::filesystem::temp_directory_path(Error) / ("PulseForgeProjectOpenRecovery-" + TemporaryID->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return;
		struct Cleanup final
		{
			std::filesystem::path Root;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Root, Error);
			}
		} CleanupProject{ Root };

		const std::filesystem::path ProjectFile = Root / "Recovery.pfproj";
		auto Created = Project::Create(ProjectFile, "Recovery Test");
		PF_CHECK(Tests, Created.has_value());
		if (!Created) return;
		const UUID ProjectID = Created->GetDescription().ID;

		const auto OperationID = UUID::Generate();
		PF_CHECK(Tests, OperationID.has_value());
		if (!OperationID) return;
		const std::filesystem::path TransactionDirectory = Root / (".pf-asset-edit-" + OperationID->ToString());
		std::filesystem::create_directory(TransactionDirectory, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const std::u8string RootUtf8 = std::filesystem::absolute(Root).lexically_normal().generic_u8string();
		PulseForge::Assets::Detail::AssetEditRecoveryJournalRecord Record;
		Record.OperationID = *OperationID;
		Record.ProjectID = ProjectID;
		Record.ProjectRootUtf8.assign(reinterpret_cast<const char*>(RootUtf8.data()), RootUtf8.size());
		Record.State = PulseForge::Assets::Detail::AssetEditRecoveryState::Committed;
		const std::filesystem::path JournalPath = TransactionDirectory / "RecoveryRecord.json";
		PF_CHECK(Tests, PulseForge::Assets::Detail::AssetEditRecoveryJournal::WriteAtomic(JournalPath, Record).has_value());

		auto Prepared = PrepareEditorProjectOpen(ProjectFile);
		PF_CHECK(Tests, Prepared.has_value());
		PF_CHECK(Tests, Prepared && Prepared->Project.GetDescription().ID == ProjectID);
		PF_CHECK(Tests, !std::filesystem::exists(TransactionDirectory));
	}
} // namespace

void RunEditorAssetEditSchedulingTests(TestRunner& Tests)
{
	TestAcceptedAssetPlanWaitsForSafeUpdate(Tests);
	TestCancelledOrSupersededAssetPlanDoesNotMutate(Tests);
	TestDirtySceneAndMaterialHistorySurviveScheduledMoves(Tests);
	TestDirtyReferrerBlocksScheduledRewrite(Tests);
	TestProjectOpenRecoversCommittedAssetEditBeforePublishingProject(Tests);
}
