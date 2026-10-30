#include "TestAssetSupport.h"

#include "Assets/PrefabAssetService.h"
#include "Editor/EditorPrefabDocumentEditor.h"
#include "Editor/EditorPrefabInstanceController.h"
#include "Editor/EditorSceneEditController.h"
#include "Editor/EditorSceneHierarchyOperations.h"
#include "Editor/EditorStyle.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorViewportGizmoInteraction.h"
#include "Scene/Scene.h"
#include "Scene/PropertyMetadata.h"
#include "Scene/SceneSerializer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <utility>

namespace
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	constexpr AssetID PrefabAsset{ 0x9100000000000000ull, 91 };
	constexpr UUID SourceRootKey{ 0x9200000000000000ull, 92 };
	constexpr UUID SourceChildKey{ 0x9300000000000000ull, 93 };
	constexpr UUID LocalKey{ 0x9400000000000000ull, 94 };

	[[nodiscard]] const PrefabEntityMapping* FindSourceMapping(
		const PrefabInstanceComponent& Instance, const UUID& SourceKey)
	{
		const auto Mapping = std::find_if(Instance.SourceEntities.begin(), Instance.SourceEntities.end(),
			[&](const PrefabEntityMapping& Candidate) { return Candidate.SourceKey == SourceKey; });
		return Mapping == Instance.SourceEntities.end() ? nullptr : &*Mapping;
	}

	[[nodiscard]] const PrefabLocalEntityMapping* FindLocalMapping(
		const PrefabInstanceComponent& Instance, const UUID& Key)
	{
		const auto Mapping = std::find_if(Instance.LocalEntities.begin(), Instance.LocalEntities.end(),
			[&](const PrefabLocalEntityMapping& Candidate) { return Candidate.LocalKey == Key; });
		return Mapping == Instance.LocalEntities.end() ? nullptr : &*Mapping;
	}

	void TestDuplicateLinkedPrefabSubtreeRemapsInstanceIdentity(TestRunner& Tests)
	{
		Scene SceneValue;
		constexpr UUID RootEntityID{ 0xa100000000000000ull, 101 };
		constexpr UUID ChildEntityID{ 0xa200000000000000ull, 102 };
		constexpr UUID LocalEntityID{ 0xa300000000000000ull, 103 };
		constexpr UUID InstanceID{ 0xa400000000000000ull, 104 };
		const auto Root = SceneValue.CreateEntityWithUUID(RootEntityID, "Linked prefab root");
		const auto Child = SceneValue.CreateEntityWithUUID(ChildEntityID, "Linked source child");
		const auto Local = SceneValue.CreateEntityWithUUID(LocalEntityID, "Local instance entity");
		PF_CHECK(Tests, Root.has_value() && Child.has_value() && Local.has_value());
		if (!Root || !Child || !Local) return;
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Local->SetParent(*Child).has_value());

		PrefabInstanceComponent Instance;
		Instance.InstanceID = InstanceID;
		Instance.SourceAsset = PrefabAsset;
		Instance.SourceRootKey = SourceRootKey;
		Instance.AppliedSourceFingerprint = "editor-duplicate-source-fingerprint";
		Instance.SourceEntities = { { SourceRootKey, RootEntityID }, { SourceChildKey, ChildEntityID } };
		Instance.LocalEntities = { { LocalKey, LocalEntityID } };
		PrefabOverrideRecord LocalRename;
		LocalRename.Entity = { LocalKey, true };
		LocalRename.Operation = PrefabOverrideOperation::Rename;
		LocalRename.Name = "Locally renamed entity";
		Instance.Overrides.push_back(LocalRename);
		PrefabOverrideRecord LocalReparent;
		LocalReparent.Entity = { SourceChildKey, false };
		LocalReparent.Operation = PrefabOverrideOperation::Reparent;
		LocalReparent.Parent = PrefabEntityAddress{ LocalKey, true };
		Instance.Overrides.push_back(LocalReparent);
		Instance.Conflicts.push_back({ { LocalKey, true }, PrefabConflictKind::RemovedEntityHasLocalChildren, {}, {},
			"Local descendant needs review" });
		PF_CHECK(Tests, Root->SetPrefabInstance(Instance).has_value());

		const std::array Roots{ *Root };
		const auto Duplicated = EditorSceneHierarchyOperations::DuplicateSubtrees(SceneValue, Roots);
		if (!Duplicated) std::cerr << "Linked prefab duplicate failed: " << Duplicated.error().Message << '\n';
		PF_CHECK(Tests, Duplicated.has_value() && Duplicated->size() == 1);
		if (!Duplicated || Duplicated->size() != 1) return;
		const auto DuplicatedRoot = SceneValue.FindEntity(Duplicated->front());
		PF_CHECK(Tests, DuplicatedRoot.has_value());
		if (!DuplicatedRoot) return;
		const auto DuplicatedMetadata = DuplicatedRoot->GetPrefabInstance();
		PF_CHECK(Tests, DuplicatedMetadata && DuplicatedMetadata->has_value());
		if (!DuplicatedMetadata || !DuplicatedMetadata->has_value()) return;
		const PrefabInstanceComponent& Copy = **DuplicatedMetadata;

		PF_CHECK(Tests, Copy.InstanceID != InstanceID);
		PF_CHECK(Tests, Copy.SourceAsset == Instance.SourceAsset);
		PF_CHECK(Tests, Copy.SourceRootKey == Instance.SourceRootKey);
		PF_CHECK(Tests, Copy.AppliedSourceFingerprint == Instance.AppliedSourceFingerprint);
		PF_CHECK(Tests, Copy.SourceEntities.size() == Instance.SourceEntities.size());
		PF_CHECK(Tests, Copy.LocalEntities.size() == Instance.LocalEntities.size());
		if (Copy.LocalEntities.size() != 1) return;
		const PrefabEntityMapping* OriginalRootMap = FindSourceMapping(Instance, SourceRootKey);
		const PrefabEntityMapping* OriginalChildMap = FindSourceMapping(Instance, SourceChildKey);
		const PrefabEntityMapping* DuplicatedRootMap = FindSourceMapping(Copy, SourceRootKey);
		const PrefabEntityMapping* DuplicatedChildMap = FindSourceMapping(Copy, SourceChildKey);
		const PrefabLocalEntityMapping* OriginalLocalMap = FindLocalMapping(Instance, LocalKey);
		PF_CHECK(Tests, OriginalRootMap != nullptr && OriginalChildMap != nullptr && OriginalLocalMap != nullptr);
		PF_CHECK(Tests, DuplicatedRootMap != nullptr && DuplicatedChildMap != nullptr);
		if (!OriginalRootMap || !OriginalChildMap || !OriginalLocalMap || !DuplicatedRootMap || !DuplicatedChildMap) return;
		PF_CHECK(Tests, DuplicatedRootMap->SourceKey == OriginalRootMap->SourceKey);
		PF_CHECK(Tests, DuplicatedChildMap->SourceKey == OriginalChildMap->SourceKey);
		PF_CHECK(Tests, DuplicatedRootMap->SceneEntity == DuplicatedRoot->GetUUID());
		PF_CHECK(Tests, DuplicatedRootMap->SceneEntity != OriginalRootMap->SceneEntity);
		PF_CHECK(Tests, DuplicatedChildMap->SceneEntity != OriginalChildMap->SceneEntity);
		PF_CHECK(Tests, DuplicatedChildMap->SceneEntity != DuplicatedRootMap->SceneEntity);
		const PrefabLocalEntityMapping* DuplicatedLocalMap = FindLocalMapping(Copy, LocalKey);
		PF_CHECK(Tests, DuplicatedLocalMap == nullptr);
		PF_CHECK(Tests, Copy.LocalEntities.front().LocalKey != OriginalLocalMap->LocalKey);
		PF_CHECK(Tests, Copy.LocalEntities.front().SceneEntity != OriginalLocalMap->SceneEntity);

		const auto DuplicatedChild = SceneValue.FindEntity(DuplicatedChildMap->SceneEntity);
		const auto DuplicatedLocal = SceneValue.FindEntity(Copy.LocalEntities.front().SceneEntity);
		PF_CHECK(Tests, DuplicatedChild.has_value() && DuplicatedLocal.has_value());
		if (!DuplicatedChild || !DuplicatedLocal) return;
		const auto DuplicatedChildParent = DuplicatedChild->GetParent();
		const auto DuplicatedLocalParent = DuplicatedLocal->GetParent();
		PF_CHECK(Tests, DuplicatedChildParent && DuplicatedChildParent->has_value() && **DuplicatedChildParent == *DuplicatedRoot);
		PF_CHECK(Tests, DuplicatedLocalParent && DuplicatedLocalParent->has_value() && **DuplicatedLocalParent == *DuplicatedChild);

		PF_CHECK(Tests, Copy.Overrides.size() == 2 && Copy.Conflicts.size() == 1);
		if (Copy.Overrides.size() == 2)
		{
			const auto Renamed = std::find_if(Copy.Overrides.begin(), Copy.Overrides.end(),
				[](const PrefabOverrideRecord& Override) { return Override.Operation == PrefabOverrideOperation::Rename; });
			const auto Reparented = std::find_if(Copy.Overrides.begin(), Copy.Overrides.end(),
				[](const PrefabOverrideRecord& Override) { return Override.Operation == PrefabOverrideOperation::Reparent; });
			PF_CHECK(Tests, Renamed != Copy.Overrides.end() && Reparented != Copy.Overrides.end());
			if (Renamed != Copy.Overrides.end())
			{
				PF_CHECK(Tests, Renamed->Entity.IsLocal && Renamed->Entity.Key == Copy.LocalEntities.front().LocalKey);
				PF_CHECK(Tests, Renamed->Name == LocalRename.Name);
			}
			if (Reparented != Copy.Overrides.end())
			{
				PF_CHECK(Tests, !Reparented->Entity.IsLocal && Reparented->Entity.Key == SourceChildKey);
				PF_CHECK(Tests, Reparented->Parent && Reparented->Parent->IsLocal &&
							Reparented->Parent->Key == Copy.LocalEntities.front().LocalKey);
			}
		}
		if (Copy.Conflicts.size() == 1)
			PF_CHECK(Tests, Copy.Conflicts.front().Entity.IsLocal &&
						Copy.Conflicts.front().Entity.Key == Copy.LocalEntities.front().LocalKey);

		const auto Serialized = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		Scene ReopenedScene;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, ReopenedScene).has_value());
		const auto ReopenedDuplicate = ReopenedScene.FindEntity(DuplicatedRoot->GetUUID());
		PF_CHECK(Tests, ReopenedDuplicate.has_value());
		if (!ReopenedDuplicate) return;
		const auto ReopenedMetadata = ReopenedDuplicate->GetPrefabInstance();
		PF_CHECK(Tests, ReopenedMetadata && ReopenedMetadata->has_value());
		if (!ReopenedMetadata || !ReopenedMetadata->has_value()) return;
		PF_CHECK(Tests, (**ReopenedMetadata).InstanceID == Copy.InstanceID);
		PF_CHECK(Tests, (**ReopenedMetadata).SourceAsset == PrefabAsset);
		PF_CHECK(Tests, (**ReopenedMetadata).SourceEntities == Copy.SourceEntities);
		PF_CHECK(Tests, (**ReopenedMetadata).LocalEntities == Copy.LocalEntities);
		PF_CHECK(Tests, (**ReopenedMetadata).Overrides == Copy.Overrides);
		PF_CHECK(Tests, (**ReopenedMetadata).Conflicts == Copy.Conflicts);
	}

	struct TemporaryPrefabDocumentProject final
	{
		std::filesystem::path Root;

		~TemporaryPrefabDocumentProject()
		{
			std::error_code Error;
			if (!Root.empty()) std::filesystem::remove_all(Root, Error);
		}
	};

	[[nodiscard]] std::optional<std::string> ReadBytes(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::nullopt;
		return std::string(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
	}

	void TestPrefabDocumentApplyDraftAndSceneRevertHistory(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto TemporaryID = UUID::Generate();
		PF_CHECK(Tests, TemporaryID.has_value());
		if (!TemporaryID) return;
		TemporaryPrefabDocumentProject Temporary;
		Temporary.Root = std::filesystem::temp_directory_path() / ("PulseForgePrefabDocument-" + TemporaryID->ToString());
		std::error_code FilesystemError;
		std::filesystem::create_directories(Temporary.Root, FilesystemError);
		PF_CHECK(Tests, !FilesystemError);
		if (FilesystemError) return;

		auto ProjectResult = Project::Create(Temporary.Root / "PrefabDocument.pfproj", "Prefab Document Test");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;
		Project& ProjectValue = *ProjectResult;

		Scene SourceScene;
		auto SourceRoot = SourceScene.CreateEntity("Source root");
		auto SourceChild = SourceScene.CreateEntity("Source child");
		PF_CHECK(Tests, SourceRoot.has_value() && SourceChild.has_value());
		if (!SourceRoot || !SourceChild) return;
		PF_CHECK(Tests, SourceChild->SetParent(*SourceRoot).has_value());
		const std::filesystem::path PrefabRelativePath = "Assets/Prefabs/DocumentSource.prefab";
		std::filesystem::create_directories(ProjectValue.GetRootPath() / PrefabRelativePath.parent_path(), FilesystemError);
		PF_CHECK(Tests, !FilesystemError);
		if (FilesystemError) return;
		auto Created = PrefabAssetService::Create(
			ProjectValue.GetAssetRegistry(), ProjectValue.GetRootPath(), PrefabRelativePath, SourceScene, *SourceRoot);
		if (!Created) std::cerr << "Prefab document fixture creation failed: " << Created.error().Message << '\n';
		PF_CHECK(Tests, Created.has_value());
		if (!Created) return;
		const std::filesystem::path PrefabPath = ProjectValue.GetRootPath() / PrefabRelativePath;
		const auto OriginalPrefabBytes = ReadBytes(PrefabPath);
		PF_CHECK(Tests, OriginalPrefabBytes.has_value());
		if (!OriginalPrefabBytes) return;

		Scene Destination;
		auto LinkedRoot = PrefabAssetService::InstantiateLinked(
			Created->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry(), Destination);
		PF_CHECK(Tests, LinkedRoot.has_value());
		if (!LinkedRoot) return;
		auto Metadata = LinkedRoot->GetPrefabInstance();
		PF_CHECK(Tests, Metadata && Metadata->has_value());
		if (!Metadata || !Metadata->has_value()) return;
		const auto ChildMapping = std::find_if((**Metadata).SourceEntities.begin(), (**Metadata).SourceEntities.end(),
			[&](const PrefabEntityMapping& Mapping) { return Mapping.SourceKey == SourceChild->GetUUID(); });
		PF_CHECK(Tests, ChildMapping != (**Metadata).SourceEntities.end());
		if (ChildMapping == (**Metadata).SourceEntities.end()) return;
		auto InstanceChild = Destination.FindEntity(ChildMapping->SceneEntity);
		PF_CHECK(Tests, InstanceChild.has_value());
		if (!InstanceChild) return;
		const glm::vec3 OverrideTranslation{ 4.0f, 2.0f, -1.0f };
		PF_CHECK(Tests, WriteProperty(*InstanceChild, PropertyComponentIds::Transform, PropertyIds::Translation,
			PropertyValue{ OverrideTranslation }).has_value());
		PF_CHECK(Tests, PrefabInstanceReconciler::RecordPropertyOverride(
			*LinkedRoot, *InstanceChild, std::string(PropertyComponentIds::Transform), std::string(PropertyIds::Translation),
			PropertyValue{ OverrideTranslation }).has_value());
		Metadata = LinkedRoot->GetPrefabInstance();
		PF_CHECK(Tests, Metadata && Metadata->has_value());
		if (!Metadata || !Metadata->has_value()) return;
		const PrefabInstanceComponent InstanceIntent = **Metadata;
		const auto DestinationBeforeApply = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, DestinationBeforeApply.has_value());
		if (!DestinationBeforeApply) return;

		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorStyle Style;
		EditorPrefabDocumentEditor PrefabEditor(Documents, Editors, Style);
		PF_CHECK(Tests, PrefabEditor.Register(EditorDocumentKind::Prefab).has_value());
		const EditorDocumentIdentity Identity{
			ProjectValue.GetDescription().ID, Created->ID, EditorDocumentKind::Prefab };
		auto Opened = Documents.Open(Identity, PrefabRelativePath, "DocumentSource.prefab", EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, Opened.has_value());
		if (!Opened) return;
		auto* Session = Documents.Find(Identity);
		const EditorDocumentEditorContext Context{
			Identity, ProjectValue.GetRootPath(), PrefabRelativePath, "DocumentSource.prefab", &ProjectValue.GetAssetRegistry() };
		PF_CHECK(Tests, Session && PrefabEditor.Open(*Session, Context));
		if (!Session) return;

		auto Applied = PrefabEditor.ApplyInstanceOverridesToSourceDraft(
			*Session, InstanceIntent, PrefabEntityAddress{ SourceChild->GetUUID(), false });
		PF_CHECK(Tests, Applied && Applied->Applied == 1 && Session->IsDirty());
		PF_CHECK(Tests, ReadBytes(PrefabPath) == OriginalPrefabBytes);
		const auto DestinationAfterApply = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, DestinationAfterApply.has_value() && DestinationBeforeApply.has_value());
		if (DestinationAfterApply && DestinationBeforeApply)
			PF_CHECK(Tests, *DestinationAfterApply == *DestinationBeforeApply);

		PF_CHECK(Tests, PrefabEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, PrefabEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, PrefabEditor.Save(*Session));
		PF_CHECK(Tests, !Session->IsDirty());
		auto SavedSource = PrefabAssetService::LoadSource(Created->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, SavedSource.has_value());
		if (!SavedSource) return;
		const auto SavedChild = SavedSource->GetScene().FindEntity(SourceChild->GetUUID());
		PF_CHECK(Tests, SavedChild.has_value());
		if (!SavedChild) return;
		const auto SavedTranslation = ReadProperty(*SavedChild, PropertyComponentIds::Transform, PropertyIds::Translation);
		PF_CHECK(Tests, SavedTranslation && glm::all(glm::equal(std::get<glm::vec3>(*SavedTranslation), OverrideTranslation)));

		// Give the current source a distinct value so revert and history must restore actual entity data.
		const glm::vec3 LatestSourceTranslation{ 9.0f, -3.0f, 2.0f };
		auto LatestSourceTransform = SavedChild->GetTransform();
		PF_CHECK(Tests, LatestSourceTransform.has_value());
		if (!LatestSourceTransform) return;
		LatestSourceTransform->Translation = LatestSourceTranslation;
		PF_CHECK(Tests, SavedChild->SetTransform(*LatestSourceTransform).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(
			Created->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry(), *SavedSource).has_value());
		PF_CHECK(Tests, PrefabEditor.Discard(*Session));
		PF_CHECK(Tests, !Session->IsDirty());

		EditorSceneEditController SceneHistory;
		EditorSceneWorkspaceState Workspace;
		SceneHistory.Reset(&Destination);
		const UUID LinkedRootID = LinkedRoot->GetUUID();
		const auto ReadLinkedTranslation = [&](const UUID& RootID) -> std::optional<glm::vec3>
		{
			const auto Root = Destination.FindEntity(RootID);
			if (!Root) return std::nullopt;
			const auto RootMetadata = Root->GetPrefabInstance();
			if (!RootMetadata || !RootMetadata->has_value()) return std::nullopt;
			const auto Mapping = std::find_if((**RootMetadata).SourceEntities.begin(), (**RootMetadata).SourceEntities.end(),
				[&](const PrefabEntityMapping& Candidate) { return Candidate.SourceKey == SourceChild->GetUUID(); });
			if (Mapping == (**RootMetadata).SourceEntities.end()) return std::nullopt;
			const auto Child = Destination.FindEntity(Mapping->SceneEntity);
			if (!Child) return std::nullopt;
			const auto Translation = ReadProperty(*Child, PropertyComponentIds::Transform, PropertyIds::Translation);
			if (!Translation || !std::holds_alternative<glm::vec3>(*Translation)) return std::nullopt;
			return std::get<glm::vec3>(*Translation);
		};
		PrefabOverrideEditSummary RevertedSummary;
		const auto Restore = [&](std::string_view Snapshot)
		{
			return SceneHistory.Restore(Snapshot, Destination, Workspace);
		};
		auto RevertRun = SceneHistory.Run(&Destination, Workspace, false, 0, [] { return 0; },
			"Revert Linked Prefab Property", [&]() -> std::expected<void, SceneError>
			{
				auto Result = PrefabEditor.RevertInstanceOverrides(*Session, Destination, *LinkedRoot,
					PrefabEntityAddress{ SourceChild->GetUUID(), false });
				if (!Result) return std::unexpected(SceneError{ SceneErrorCode::InvalidPrefabInstance, Result.error() });
				RevertedSummary = *Result;
				return {};
			}, Restore);
		PF_CHECK(Tests, RevertRun.has_value() && RevertedSummary.Applied == 1);
		const auto RevertedRoot = Destination.FindEntity(LinkedRoot->GetUUID());
		PF_CHECK(Tests, RevertedRoot.has_value());
		if (!RevertedRoot) return;
		const auto RevertedMetadata = RevertedRoot->GetPrefabInstance();
		PF_CHECK(Tests, RevertedMetadata && RevertedMetadata->has_value() && (**RevertedMetadata).Overrides.empty());
		const auto RevertedChildID = std::find_if((**RevertedMetadata).SourceEntities.begin(), (**RevertedMetadata).SourceEntities.end(),
			[&](const PrefabEntityMapping& Mapping) { return Mapping.SourceKey == SourceChild->GetUUID(); });
		PF_CHECK(Tests, RevertedChildID != (**RevertedMetadata).SourceEntities.end());
		if (RevertedChildID == (**RevertedMetadata).SourceEntities.end()) return;
		const auto RevertedChild = Destination.FindEntity(RevertedChildID->SceneEntity);
		PF_CHECK(Tests, RevertedChild.has_value());
		if (!RevertedChild) return;
		const auto RevertedTranslation = ReadLinkedTranslation(LinkedRootID);
		PF_CHECK(Tests, RevertedTranslation && glm::all(glm::equal(*RevertedTranslation, LatestSourceTranslation)));

		const auto Undone = SceneHistory.Undo(&Destination, Workspace, false, Restore);
		PF_CHECK(Tests, Undone && *Undone);
		const auto UndoRoot = Destination.FindEntity(LinkedRootID);
		PF_CHECK(Tests, UndoRoot.has_value());
		if (UndoRoot)
		{
			const auto UndoMetadata = UndoRoot->GetPrefabInstance();
			PF_CHECK(Tests, UndoMetadata && UndoMetadata->has_value() && (**UndoMetadata).Overrides == InstanceIntent.Overrides);
		}
		const auto UndoTranslation = ReadLinkedTranslation(LinkedRootID);
		PF_CHECK(Tests, UndoTranslation && glm::all(glm::equal(*UndoTranslation, OverrideTranslation)));
		const auto Redone = SceneHistory.Redo(&Destination, Workspace, false, Restore);
		PF_CHECK(Tests, Redone && *Redone);
		const auto RedoRoot = Destination.FindEntity(LinkedRootID);
		PF_CHECK(Tests, RedoRoot.has_value());
		if (RedoRoot)
		{
			const auto RedoMetadata = RedoRoot->GetPrefabInstance();
			PF_CHECK(Tests, RedoMetadata && RedoMetadata->has_value() && (**RedoMetadata).Overrides.empty());
		}
		const auto RedoTranslation = ReadLinkedTranslation(LinkedRootID);
		PF_CHECK(Tests, RedoTranslation && glm::all(glm::equal(*RedoTranslation, LatestSourceTranslation)));

		// Exercise subtree removal at both history owners: the scene command owns the instance
		// removal, while the prefab document owns applying that intent to its source draft.
		const auto RootBeforeRemoval = Destination.FindEntity(LinkedRootID);
		PF_CHECK(Tests, RootBeforeRemoval.has_value());
		if (!RootBeforeRemoval) return;
		const auto RootMetadataBeforeRemoval = RootBeforeRemoval->GetPrefabInstance();
		PF_CHECK(Tests, RootMetadataBeforeRemoval && RootMetadataBeforeRemoval->has_value());
		if (!RootMetadataBeforeRemoval || !RootMetadataBeforeRemoval->has_value()) return;
		const auto ChildMappingBeforeRemoval = std::find_if((**RootMetadataBeforeRemoval).SourceEntities.begin(),
			(**RootMetadataBeforeRemoval).SourceEntities.end(), [&](const PrefabEntityMapping& Mapping)
			{
				return Mapping.SourceKey == SourceChild->GetUUID();
			});
		PF_CHECK(Tests, ChildMappingBeforeRemoval != (**RootMetadataBeforeRemoval).SourceEntities.end());
		if (ChildMappingBeforeRemoval == (**RootMetadataBeforeRemoval).SourceEntities.end()) return;
		const UUID ChildSceneID = ChildMappingBeforeRemoval->SceneEntity;
		const auto ChildBeforeRemoval = Destination.FindEntity(ChildSceneID);
		PF_CHECK(Tests, ChildBeforeRemoval.has_value());
		if (!ChildBeforeRemoval) return;
		auto RemoveInstanceChild = SceneHistory.Run(&Destination, Workspace, false, 0, [] { return 0; },
			"Remove Linked Prefab Child", [&]() -> std::expected<void, SceneError>
			{
				const auto CurrentRoot = Destination.FindEntity(LinkedRootID);
				const auto CurrentChild = Destination.FindEntity(ChildSceneID);
				if (!CurrentRoot || !CurrentChild)
					return std::unexpected(SceneError{ SceneErrorCode::InvalidPrefabInstance, "Linked child disappeared" });
				auto Recorded = PrefabInstanceReconciler::RecordRemoveSourceEntityOverride(*CurrentRoot, *CurrentChild);
				if (!Recorded) return std::unexpected(SceneError{ SceneErrorCode::InvalidPrefabInstance, Recorded.error().Message });
				auto Destroyed = Destination.DestroyEntity(*CurrentChild);
				if (!Destroyed) return std::unexpected(Destroyed.error());
				return {};
			}, Restore);
		PF_CHECK(Tests, RemoveInstanceChild.has_value() && !Destination.FindEntity(ChildSceneID));
		const auto RemovedRoot = Destination.FindEntity(LinkedRootID);
		PF_CHECK(Tests, RemovedRoot.has_value());
		if (!RemovedRoot) return;
		const auto RemovedMetadata = RemovedRoot->GetPrefabInstance();
		PF_CHECK(Tests, RemovedMetadata && RemovedMetadata->has_value() && (**RemovedMetadata).Overrides.size() == 1);
		const auto InstanceUndo = SceneHistory.Undo(&Destination, Workspace, false, Restore);
		PF_CHECK(Tests, InstanceUndo && *InstanceUndo && Destination.FindEntity(ChildSceneID));
		const auto RestoredRoot = Destination.FindEntity(LinkedRootID);
		PF_CHECK(Tests, RestoredRoot.has_value());
		if (!RestoredRoot) return;
		const auto RestoredMetadata = RestoredRoot->GetPrefabInstance();
		PF_CHECK(Tests, RestoredMetadata && RestoredMetadata->has_value() && (**RestoredMetadata).Overrides.empty());
		const auto InstanceRedo = SceneHistory.Redo(&Destination, Workspace, false, Restore);
		PF_CHECK(Tests, InstanceRedo && *InstanceRedo && !Destination.FindEntity(ChildSceneID));
		const auto RedoneRoot = Destination.FindEntity(LinkedRootID);
		PF_CHECK(Tests, RedoneRoot.has_value());
		if (!RedoneRoot) return;
		const auto RedoneMetadata = RedoneRoot->GetPrefabInstance();
		PF_CHECK(Tests, RedoneMetadata && RedoneMetadata->has_value() && (**RedoneMetadata).Overrides.size() == 1);
		PF_CHECK(Tests, SceneHistory.IsDirty());

		const auto SourceBytesBeforeRemoval = ReadBytes(PrefabPath);
		PF_CHECK(Tests, SourceBytesBeforeRemoval.has_value());
		if (!SourceBytesBeforeRemoval) return;
		auto AppliedRemoval = PrefabEditor.ApplyInstanceOverridesToSourceDraft(*Session, **RedoneMetadata);
		PF_CHECK(Tests, AppliedRemoval && AppliedRemoval->Applied == 1 && Session->IsDirty());
		PF_CHECK(Tests, ReadBytes(PrefabPath) == SourceBytesBeforeRemoval);
		auto DraftSummary = PrefabEditor.GetSummary(*Session);
		PF_CHECK(Tests, DraftSummary && DraftSummary->SourceEntityCount == 1 && DraftSummary->CanUndo);
		PF_CHECK(Tests, PrefabEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		auto UndoneDraftSummary = PrefabEditor.GetSummary(*Session);
		PF_CHECK(Tests, UndoneDraftSummary && UndoneDraftSummary->SourceEntityCount == 2 && !Session->IsDirty());
		PF_CHECK(Tests, PrefabEditor.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		auto RedoneDraftSummary = PrefabEditor.GetSummary(*Session);
		PF_CHECK(Tests, RedoneDraftSummary && RedoneDraftSummary->SourceEntityCount == 1 && Session->IsDirty());
		PF_CHECK(Tests, PrefabEditor.Save(*Session));
		PF_CHECK(Tests, SceneHistory.IsDirty());
		auto ReloadedAfterRemoval = PrefabAssetService::LoadSource(
			Created->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, ReloadedAfterRemoval && ReloadedAfterRemoval->GetScene().GetEntityCount() == 1);
		if (ReloadedAfterRemoval)
		{
			PF_CHECK(Tests, ReloadedAfterRemoval->GetScene().FindEntity(SourceRoot->GetUUID()).has_value());
			PF_CHECK(Tests, !ReloadedAfterRemoval->GetScene().FindEntity(SourceChild->GetUUID()).has_value());
		}
	}

	void TestLinkedChildGizmoTransformRecordsOverrideAndHistory(TestRunner& Tests)
	{
		Scene SceneValue;
		const auto Root = SceneValue.CreateEntity("Linked source root");
		const auto Child = SceneValue.CreateEntity("Linked source child");
		PF_CHECK(Tests, Root.has_value() && Child.has_value());
		if (!Root || !Child) return;
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());

		PrefabInstanceComponent Instance;
		Instance.InstanceID = UUID{ 0x9500000000000000ull, 95 };
		Instance.SourceAsset = PrefabAsset;
		Instance.SourceRootKey = SourceRootKey;
		Instance.AppliedSourceFingerprint = "gizmo-source-fingerprint";
		Instance.SourceEntities = { { SourceRootKey, Root->GetUUID() }, { SourceChildKey, Child->GetUUID() } };
		PF_CHECK(Tests, Root->SetPrefabInstance(Instance).has_value());

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().SelectSingle(Child->GetUUID());
		EditorSceneEditController Edits;
		Edits.Reset(&SceneValue);
		EditorViewportGizmoInteraction Interaction;
		const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
		const ViewportImageRect ImageRect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } };
		ViewportGizmoGeometry Geometry;
		Geometry.IsValid = true;
		Geometry.Axes[0] = { .WorldDirection = { 1.0f, 0.0f, 0.0f }, .WorldUnitsPerLocalUnit = 1.0f, .IsValid = true };
		EditorViewportGizmoBeginInput BeginInput;
		BeginInput.Handle = { TransformGizmoOperation::Translate, 0 };
		BeginInput.Geometry = Geometry;
		BeginInput.MousePosition = { 50.0f, 50.0f };
		BeginInput.ImageRect = ImageRect;
		BeginInput.ViewProjection = Projection * View;
		BeginInput.CameraPosition = { 0.0f, 0.0f, 5.0f };
		BeginInput.ViewProjectionValid = true;
		const auto Restore = [&Edits, &SceneValue, &Workspace](std::string_view Snapshot)
			-> std::expected<void, std::string>
		{
			return Edits.Restore(Snapshot, SceneValue, Workspace);
		};

		const auto Began = Interaction.Begin(SceneValue, Workspace, Edits, BeginInput, Restore);
		PF_CHECK(Tests, Began && *Began);
		if (!Began || !*Began) return;
		EditorViewportGizmoUpdateInput UpdateInput;
		UpdateInput.MousePosition = { 75.0f, 25.0f };
		UpdateInput.ImageRect = ImageRect;
		UpdateInput.ViewProjection = BeginInput.ViewProjection;
		UpdateInput.ToolValues.TransformSnapping = true;
		UpdateInput.ToolValues.LocalSpace = true;
		PF_CHECK(Tests, Interaction.Update(&SceneValue, Workspace, Edits, UpdateInput, Restore).has_value());

		const auto UpdatedChild = SceneValue.FindEntity(Child->GetUUID());
		const auto UpdatedRoot = SceneValue.FindEntity(Root->GetUUID());
		PF_CHECK(Tests, UpdatedChild.has_value() && UpdatedRoot.has_value());
		if (!UpdatedChild || !UpdatedRoot) return;
		const auto UpdatedTransform = UpdatedChild->GetTransform();
		const auto UpdatedMetadata = UpdatedRoot->GetPrefabInstance();
		PF_CHECK(Tests, UpdatedTransform && UpdatedMetadata && UpdatedMetadata->has_value() &&
				(**UpdatedMetadata).Overrides.size() == 1);
		if (!UpdatedTransform || !UpdatedMetadata || !UpdatedMetadata->has_value() ||
			(**UpdatedMetadata).Overrides.size() != 1) return;
		const PrefabOverrideRecord& Override = (**UpdatedMetadata).Overrides.front();
		const PrefabEntityAddress ExpectedAddress{ SourceChildKey, false };
		PF_CHECK(Tests, Override.Entity == ExpectedAddress);
		PF_CHECK(Tests, Override.ComponentID == PropertyComponentIds::Transform &&
				Override.PropertyID == PropertyIds::Translation && std::holds_alternative<glm::vec3>(Override.Value));
		if (std::holds_alternative<glm::vec3>(Override.Value))
			PF_CHECK(Tests, glm::all(glm::equal(std::get<glm::vec3>(Override.Value), UpdatedTransform->Translation)));
		PF_CHECK(Tests, Interaction.Complete(&SceneValue, Workspace, Edits, Restore).has_value());
		PF_CHECK(Tests, Edits.CanUndo());

		const auto Undone = Edits.Undo(&SceneValue, Workspace, false, Restore);
		PF_CHECK(Tests, Undone && *Undone);
		const auto UndoRoot = SceneValue.FindEntity(Root->GetUUID());
		const auto UndoChild = SceneValue.FindEntity(Child->GetUUID());
		PF_CHECK(Tests, UndoRoot.has_value() && UndoChild.has_value());
		if (!UndoRoot || !UndoChild) return;
		const auto UndoMetadata = UndoRoot->GetPrefabInstance();
		const auto UndoTransform = UndoChild->GetTransform();
		PF_CHECK(Tests, UndoMetadata && UndoMetadata->has_value() && (**UndoMetadata).Overrides.empty());
		PF_CHECK(Tests, UndoTransform && glm::all(glm::equal(UndoTransform->Translation, glm::vec3(0.0f))));

		const auto Redone = Edits.Redo(&SceneValue, Workspace, false, Restore);
		PF_CHECK(Tests, Redone && *Redone);
		const auto RedoRoot = SceneValue.FindEntity(Root->GetUUID());
		const auto RedoChild = SceneValue.FindEntity(Child->GetUUID());
		PF_CHECK(Tests, RedoRoot.has_value() && RedoChild.has_value());
		if (!RedoRoot || !RedoChild) return;
		const auto RedoMetadata = RedoRoot->GetPrefabInstance();
		const auto RedoTransform = RedoChild->GetTransform();
		PF_CHECK(Tests, RedoMetadata && RedoMetadata->has_value() && (**RedoMetadata).Overrides.size() == 1);
		PF_CHECK(Tests, RedoTransform && glm::length(RedoTransform->Translation) > 1.0e-4f);
	}

	class QueuedPrefabControllerHost final : public EditorPrefabInstanceControllerHost
	{
	public:
		Project* GetPrefabProject() override { return ProjectValue ? &*ProjectValue : nullptr; }
		Scene* GetPrefabScene() const override { return SceneValue; }
		uint64_t GetPrefabSceneSessionGeneration() const override { return SceneSessionGeneration; }
		bool IsPrefabRuntimeActive() const override { return RuntimeActive; }
		void QueuePrefabOperation(std::function<void()> Operation) override { PendingOperations.push_back(std::move(Operation)); }
		std::expected<void, std::string> RunPrefabSceneEdit(
			std::string, std::function<std::expected<void, SceneError>()> Operation) override
		{
			auto Result = Operation();
			if (!Result) return std::unexpected(Result.error().Message);
			return {};
		}
		void UpdatePrefabSceneDirty() override { SceneDirty = true; }
		EditorDocumentManager& GetPrefabDocumentManager() override { return Documents; }
		EditorPrefabDocumentEditor* GetPrefabDocumentEditor() const override { return nullptr; }
		bool EnsurePrefabDocumentOpen(const AssetID&) override { return false; }
		void ReportPrefabError(std::string Message) override { LastError = std::move(Message); }
		void ReportPrefabWarning(std::string Message) override { LastWarning = std::move(Message); }
		void ReportPrefabStatus(std::string Message) override { LastStatus = std::move(Message); }

		void DrainOperations()
		{
			auto Operations = std::move(PendingOperations);
			PendingOperations.clear();
			for (auto& Operation : Operations) Operation();
		}

		std::optional<Project> ProjectValue;
		Scene* SceneValue = nullptr;
		bool RuntimeActive = false;
		bool SceneDirty = false;
		uint64_t SceneSessionGeneration = 1;
		std::vector<std::function<void()>> PendingOperations;
		EditorDocumentManager Documents;
		std::string LastError;
		std::string LastWarning;
		std::string LastStatus;
	};

	void TestPrefabControllerDefersReconcileAndRejectsProjectSupersession(TestRunner& Tests)
	{
		const auto TemporaryID = UUID::Generate();
		PF_CHECK(Tests, TemporaryID.has_value());
		if (!TemporaryID) return;
		TemporaryPrefabDocumentProject Temporary;
		Temporary.Root = std::filesystem::temp_directory_path() / ("PulseForgePrefabController-" + TemporaryID->ToString());
		std::error_code FilesystemError;
		std::filesystem::create_directories(Temporary.Root, FilesystemError);
		PF_CHECK(Tests, !FilesystemError);
		if (FilesystemError) return;

		auto ProjectResult = Project::Create(Temporary.Root / "Controller.pfproj", "Prefab Controller Test");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult) return;
		Project ProjectValue = std::move(*ProjectResult);
		Scene SourceScene;
		auto SourceRoot = SourceScene.CreateEntity("Source root");
		auto SourceChild = SourceScene.CreateEntity("Source child");
		PF_CHECK(Tests, SourceRoot.has_value() && SourceChild.has_value());
		if (!SourceRoot || !SourceChild) return;
		PF_CHECK(Tests, SourceChild->SetParent(*SourceRoot).has_value());
		const auto PrefabPath = std::filesystem::path("Assets/Prefabs/Controller.prefab");
		std::filesystem::create_directories(ProjectValue.GetRootPath() / PrefabPath.parent_path(), FilesystemError);
		PF_CHECK(Tests, !FilesystemError);
		if (FilesystemError) return;
		auto Created = PrefabAssetService::Create(
			ProjectValue.GetAssetRegistry(), ProjectValue.GetRootPath(), PrefabPath, SourceScene, *SourceRoot);
		PF_CHECK(Tests, Created.has_value());
		if (!Created) return;

		Scene Destination;
		auto LinkedRoot = PrefabAssetService::InstantiateLinked(
			Created->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry(), Destination);
		PF_CHECK(Tests, LinkedRoot.has_value());
		if (!LinkedRoot) return;
		auto InstanceMetadata = LinkedRoot->GetPrefabInstance();
		PF_CHECK(Tests, InstanceMetadata && InstanceMetadata->has_value());
		if (!InstanceMetadata || !InstanceMetadata->has_value()) return;
		const auto Mapping = FindSourceMapping(**InstanceMetadata, SourceChild->GetUUID());
		PF_CHECK(Tests, Mapping != nullptr);
		if (!Mapping) return;
		const auto InstanceChild = Destination.FindEntity(Mapping->SceneEntity);
		PF_CHECK(Tests, InstanceChild.has_value());
		if (!InstanceChild) return;
		const glm::vec3 UpdatedTranslation{ 7.0f, -2.0f, 3.0f };
		auto SourceDocument = PrefabAssetService::LoadSource(
			Created->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry());
		PF_CHECK(Tests, SourceDocument.has_value());
		if (!SourceDocument) return;
		auto EditableSourceChild = SourceDocument->GetScene().FindEntity(SourceChild->GetUUID());
		PF_CHECK(Tests, EditableSourceChild.has_value());
		if (!EditableSourceChild) return;
		auto SourceTransform = EditableSourceChild->GetTransform();
		PF_CHECK(Tests, SourceTransform.has_value());
		if (!SourceTransform) return;
		SourceTransform->Translation = UpdatedTranslation;
		PF_CHECK(Tests, EditableSourceChild->SetTransform(*SourceTransform).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(
			Created->ID, ProjectValue.GetRootPath(), ProjectValue.GetAssetRegistry(), *SourceDocument).has_value());

		QueuedPrefabControllerHost Host;
		Host.ProjectValue.emplace(std::move(ProjectValue));
		Host.SceneValue = &Destination;
		EditorPrefabInstanceController Controller(Host);
		auto Planned = Controller.PlanUpdate(*LinkedRoot);
		PF_CHECK(Tests, Planned.has_value() && Host.PendingOperations.size() == 1);
		const auto BeforeQueueDrain = InstanceChild->GetTransform();
		PF_CHECK(Tests, BeforeQueueDrain && glm::all(glm::equal(BeforeQueueDrain->Translation, glm::vec3(0.0f))));
		Host.DrainOperations();
		auto Status = Controller.GetStatus(*LinkedRoot);
		PF_CHECK(Tests, Status && Status->has_value() && (**Status).HasPendingPlan && (**Status).CanApply);

		auto ApplyQueued = Controller.ApplyUpdate(*LinkedRoot);
		PF_CHECK(Tests, ApplyQueued.has_value() && Host.PendingOperations.size() == 1);
		const auto BeforeApplyBoundary = InstanceChild->GetTransform();
		PF_CHECK(Tests, BeforeApplyBoundary && glm::all(glm::equal(BeforeApplyBoundary->Translation, glm::vec3(0.0f))));
		Host.DrainOperations();
		const auto AfterApply = Destination.FindEntity(Mapping->SceneEntity)->GetTransform();
		PF_CHECK(Tests, AfterApply && glm::all(glm::equal(AfterApply->Translation, UpdatedTranslation)));
		PF_CHECK(Tests, Host.SceneDirty && Host.LastError.empty());

		// An exact serialized scene copy carries matching UUIDs and instance metadata, but is a different authoring session.
		PF_CHECK(Tests, Controller.PlanUpdate(*LinkedRoot).has_value());
		Host.DrainOperations();
		const auto CloneBytes = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, CloneBytes.has_value());
		if (!CloneBytes) return;
		Scene OtherScene;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*CloneBytes, OtherScene).has_value());
		const auto ClonedSceneRoot = OtherScene.FindEntity(LinkedRoot->GetUUID());
		PF_CHECK(Tests, ClonedSceneRoot.has_value());
		if (!ClonedSceneRoot) return;
		Host.SceneValue = &OtherScene;
		++Host.SceneSessionGeneration;
		Host.SceneDirty = false;
		const auto WrongSceneApply = Controller.ApplyUpdate(*ClonedSceneRoot);
		PF_CHECK(Tests, !WrongSceneApply && Host.PendingOperations.empty());
		PF_CHECK(Tests, !Host.SceneDirty);
		const auto OtherSceneChild = OtherScene.FindEntity(Mapping->SceneEntity);
		PF_CHECK(Tests, OtherSceneChild.has_value());
		if (OtherSceneChild)
		{
			const auto OtherSceneTransform = OtherSceneChild->GetTransform();
			PF_CHECK(Tests, OtherSceneTransform && glm::all(glm::equal(OtherSceneTransform->Translation, UpdatedTranslation)));
		}
		Host.SceneValue = &Destination;
		++Host.SceneSessionGeneration;

		// A source removed after planning is converted into a recoverable conflict at apply, preserving the materialized scene.
		PF_CHECK(Tests, Controller.PlanUpdate(*LinkedRoot).has_value());
		Host.DrainOperations();
		const std::filesystem::path SourcePath = Host.ProjectValue->GetRootPath() / Created->ProjectRelativePath;
		const auto SourceBytesBeforeRemoval = ReadBytes(SourcePath);
		PF_CHECK(Tests, SourceBytesBeforeRemoval.has_value());
		if (!SourceBytesBeforeRemoval) return;
		std::error_code RemoveSourceError;
		PF_CHECK(Tests, std::filesystem::remove(SourcePath, RemoveSourceError) && !RemoveSourceError);
		if (RemoveSourceError || std::filesystem::exists(SourcePath)) return;
		Host.SceneDirty = false;
		Host.LastWarning.clear();
		PF_CHECK(Tests, Controller.ApplyUpdate(*LinkedRoot).has_value());
		Host.DrainOperations();
		PF_CHECK(Tests, Host.LastError.empty() && Host.LastWarning.find("recoverable scene conflict") != std::string::npos && Host.SceneDirty);
		const auto MissingSourceMetadata = LinkedRoot->GetPrefabInstance();
		PF_CHECK(Tests, MissingSourceMetadata && MissingSourceMetadata->has_value() &&
				std::any_of((**MissingSourceMetadata).Conflicts.begin(), (**MissingSourceMetadata).Conflicts.end(),
					[](const PrefabConflictRecord& Conflict) { return Conflict.Kind == PrefabConflictKind::MissingSourceAsset; }));
		const auto AfterMissingSourceApply = InstanceChild->GetTransform();
		PF_CHECK(Tests, AfterMissingSourceApply && glm::all(glm::equal(AfterMissingSourceApply->Translation, UpdatedTranslation)));
		{
			std::ofstream RestoreSource(SourcePath, std::ios::binary | std::ios::trunc);
			PF_CHECK(Tests, RestoreSource.is_open());
			RestoreSource.write(SourceBytesBeforeRemoval->data(), static_cast<std::streamsize>(SourceBytesBeforeRemoval->size()));
			RestoreSource.close();
			PF_CHECK(Tests, static_cast<bool>(RestoreSource));
		}
		Host.SceneDirty = false;

		// A queued plan belongs to the project that selected it, even if the same scene object remains visible.
		auto ReloadedSource = PrefabAssetService::LoadSource(
			Created->ID, Host.ProjectValue->GetRootPath(), Host.ProjectValue->GetAssetRegistry());
		PF_CHECK(Tests, ReloadedSource.has_value());
		if (!ReloadedSource) return;
		auto ReloadedChild = ReloadedSource->GetScene().FindEntity(SourceChild->GetUUID());
		PF_CHECK(Tests, ReloadedChild.has_value());
		if (!ReloadedChild) return;
		auto ReloadedTransform = ReloadedChild->GetTransform();
		PF_CHECK(Tests, ReloadedTransform.has_value());
		if (!ReloadedTransform) return;
		const glm::vec3 SupersedingTranslation{ -4.0f, 5.0f, 2.0f };
		ReloadedTransform->Translation = SupersedingTranslation;
		PF_CHECK(Tests, ReloadedChild->SetTransform(*ReloadedTransform).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(
			Created->ID, Host.ProjectValue->GetRootPath(), Host.ProjectValue->GetAssetRegistry(), *ReloadedSource).has_value());
		PF_CHECK(Tests, Controller.PlanUpdate(*LinkedRoot).has_value());
		PF_CHECK(Tests, Host.PendingOperations.size() == 1);
		const auto OtherRoot = Temporary.Root / "Other";
		auto OtherProjectResult = Project::Create(OtherRoot / "Other.pfproj", "Other Project");
		PF_CHECK(Tests, OtherProjectResult.has_value());
		if (!OtherProjectResult) return;
		Project OriginalProject = std::move(*Host.ProjectValue);
		Host.ProjectValue.emplace(std::move(*OtherProjectResult));
		Host.DrainOperations();
		const auto AfterSupersession = Destination.FindEntity(Mapping->SceneEntity)->GetTransform();
		PF_CHECK(Tests, AfterSupersession && glm::all(glm::equal(AfterSupersession->Translation, UpdatedTranslation)));
		PF_CHECK(Tests, !Host.LastWarning.empty());

		Project OtherProjectValue = std::move(*Host.ProjectValue);
		Host.ProjectValue.emplace(std::move(OriginalProject));
		auto LatestSource = PrefabAssetService::LoadSource(
			Created->ID, Host.ProjectValue->GetRootPath(), Host.ProjectValue->GetAssetRegistry());
		PF_CHECK(Tests, LatestSource.has_value());
		if (!LatestSource) return;
		auto LatestChild = LatestSource->GetScene().FindEntity(SourceChild->GetUUID());
		PF_CHECK(Tests, LatestChild.has_value());
		if (!LatestChild) return;
		auto LatestTransform = LatestChild->GetTransform();
		PF_CHECK(Tests, LatestTransform.has_value());
		if (!LatestTransform) return;
		const glm::vec3 LatestTranslation{ 1.0f, 8.0f, -6.0f };
		LatestTransform->Translation = LatestTranslation;
		PF_CHECK(Tests, LatestChild->SetTransform(*LatestTransform).has_value());
		PF_CHECK(Tests, PrefabAssetService::SaveSource(
			Created->ID, Host.ProjectValue->GetRootPath(), Host.ProjectValue->GetAssetRegistry(), *LatestSource).has_value());
		PF_CHECK(Tests, Controller.PlanUpdate(*LinkedRoot).has_value());
		Host.DrainOperations();
		Status = Controller.GetStatus(*LinkedRoot);
		PF_CHECK(Tests, Status && Status->has_value() && (**Status).HasPendingPlan);
		Host.ProjectValue.emplace(std::move(OtherProjectValue));
		auto StaleApply = Controller.ApplyUpdate(*LinkedRoot);
		PF_CHECK(Tests, !StaleApply && Host.PendingOperations.empty());
		const auto AfterStaleApply = Destination.FindEntity(Mapping->SceneEntity)->GetTransform();
		PF_CHECK(Tests, AfterStaleApply && glm::all(glm::equal(AfterStaleApply->Translation, UpdatedTranslation)));
	}
} // namespace

void RunPrefabLinkedInstanceEditorTests(TestRunner& Tests)
{
	TestDuplicateLinkedPrefabSubtreeRemapsInstanceIdentity(Tests);
	TestPrefabDocumentApplyDraftAndSceneRevertHistory(Tests);
	TestLinkedChildGizmoTransformRecordsOverrideAndHistory(Tests);
	TestPrefabControllerDefersReconcileAndRejectsProjectSupersession(Tests);
}
