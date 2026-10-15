#include "TestEditorSupport.h"
#include "Editor/EditorDocumentWorkspaceStore.h"
#include "Editor/EditorWorkspaceFrameState.h"

namespace
{
	void TestEditorDocumentManager(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Project = UUID::Generate();
		const auto OtherProject = UUID::Generate();
		const auto Material = UUID::Generate();
		const auto OtherMaterial = UUID::Generate();
		const auto Scene = UUID::Generate();
		PF_CHECK(Tests, Project && OtherProject && Material && OtherMaterial && Scene);
		if (!Project || !OtherProject || !Material || !OtherMaterial || !Scene)
			return;

		EditorDocumentManager Manager;
		PF_CHECK(Tests, ShouldProcessNativeDocumentHostCloseRequests(false));
		PF_CHECK(Tests, !ShouldProcessNativeDocumentHostCloseRequests(true));
		const std::vector<int> BatchDocuments{ 1, 2, 3, 4 };
		const auto DirtyBatch = FilterDirtyDocumentIdentities(BatchDocuments,
			[](int Document) { return Document == 2 || Document == 4; });
		PF_CHECK(Tests, DirtyBatch.size() == 2 && DirtyBatch[0] == 2 && DirtyBatch[1] == 4);
		const auto CleanBatch = FilterDirtyDocumentIdentities(BatchDocuments,
			[](int) { return false; });
		PF_CHECK(Tests, CleanBatch.empty());
		// Tab capture must start from a hovered press, even when ImGui does not activate the item.
		PF_CHECK(Tests, ShouldCaptureDocumentTabDrag(true, false, true));
		PF_CHECK(Tests, !ShouldCaptureDocumentTabDrag(false, false, true));
		PF_CHECK(Tests, !ShouldCaptureDocumentTabDrag(true, true, true));
		PF_CHECK(Tests, !ShouldCaptureDocumentTabDrag(true, false, false));
		PF_CHECK(Tests, !CanDetachDocumentTabToNativeHost(true, false, false));
		PF_CHECK(Tests, CanDetachDocumentTabToNativeHost(true, false, true));
		PF_CHECK(Tests, CanDetachDocumentTabToNativeHost(false, true, true));
		PF_CHECK(Tests, !CanDetachDocumentTabToNativeHost(false, true, false));
		PF_CHECK(Tests, !CanDetachDocumentTabToNativeHost(false, false, true));
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			false, true, true) == DocumentTabReleaseAction::TransferToStrip);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::TransferToStrip);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, true, false) == DocumentTabReleaseAction::CreateNativeHost);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			CanDetachDocumentTabToNativeHost(false, true, true), true, false) ==
			DocumentTabReleaseAction::CreateNativeHost);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			CanDetachDocumentTabToNativeHost(false, true, false), true, false) ==
			DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			CanDetachDocumentTabToNativeHost(true, false, false), true, false) ==
			DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			false, true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, false, false) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, false, true) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, true, 100.0f, 6.0f,
			true, true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(false, true, false, 100.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, false, false, 100.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 35.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::None);
		const DocumentTabPointerPosition DragStart{ 100.0f, 100.0f };
		const DocumentTabPointerPosition LastPointer{ 117.0f, 112.0f };
		const std::optional<DocumentTabPointerPosition> NoPointer;
		const std::optional<DocumentTabPointerPosition> LastValidPointer = LastPointer;
		PF_CHECK(Tests, IsUsableDocumentTabPointerPosition(LastPointer.X, LastPointer.Y));
		PF_CHECK(Tests, !IsUsableDocumentTabPointerPosition(-100001.0f, -100001.0f));
		const auto ReleasePointer = ResolveDocumentTabReleasePosition(
			-100001.0f, -100001.0f, LastValidPointer);
		PF_CHECK(Tests, ReleasePointer && ReleasePointer->X == LastPointer.X && ReleasePointer->Y == LastPointer.Y);
		PF_CHECK(Tests, !ResolveDocumentTabReleasePosition(-100001.0f, -100001.0f, NoPointer));
		const float LastObservedDragDistance = CalculateDocumentTabDragDistanceSquared(
			DragStart, NoPointer, LastValidPointer, 0.0f);
		PF_CHECK(Tests, LastObservedDragDistance == 433.0f);
		PF_CHECK(Tests, CalculateDocumentTabDragDistanceSquared(
			DragStart, NoPointer, LastValidPointer, 900.0f) == 900.0f);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, LastObservedDragDistance, 6.0f,
			true, true, false) == DocumentTabReleaseAction::CreateNativeHost);
		const EditorDocumentIdentity MaterialA{ *Project, *Material, EditorDocumentKind::Material };
		const EditorDocumentIdentity MaterialB{ *Project, *OtherMaterial, EditorDocumentKind::Material };
		const EditorDocumentIdentity SameNameOtherFolder{ *Project, *Scene, EditorDocumentKind::Material };
		const EditorDocumentIdentity OtherProjectMaterial{ *OtherProject, *Material, EditorDocumentKind::Material };
		const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
		const auto First = Manager.Open(MaterialA, "Assets/Materials/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		const auto Second = Manager.Open(MaterialB, "Assets/Materials/Blue.material", "Blue.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, First && Second && First->Created && Second->Created);
		PF_CHECK(Tests, First && Second && First->HostID != Second->HostID);
		PF_CHECK(Tests, Manager.GetHosts().size() == 3);

		const auto Reopened = Manager.Open(MaterialA, "Assets/OtherFolder/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, Reopened && !Reopened->Created);
		PF_CHECK(Tests, Reopened && Reopened->HostID == First->HostID);
		PF_CHECK(Tests, Manager.GetSessions().size() == 2);
		PF_CHECK(Tests, Manager.Find(MaterialA)->ProjectRelativePath ==
			std::filesystem::path("Assets/OtherFolder/Amber.material"));
		PF_CHECK(Tests, Manager.GetFocusedHost() && Manager.GetFocusedHost()->ID == First->HostID);
		const auto* FocusedMaterialHost = Manager.GetFocusedHost();
		const auto* FocusedMaterialIdentity = FocusedMaterialHost && FocusedMaterialHost->ActiveDocument
			? &*FocusedMaterialHost->ActiveDocument : nullptr;
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(FocusedMaterialIdentity, *Project) ==
			EditorDocumentCommandTarget::Material);
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(nullptr, *Project) == EditorDocumentCommandTarget::Level);
		const EditorDocumentIdentity TextureCommandDocument{ *Project, *Material, EditorDocumentKind::Texture };
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(&TextureCommandDocument, *Project) ==
			EditorDocumentCommandTarget::Unsupported);
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(&OtherProjectMaterial, *Project) ==
			EditorDocumentCommandTarget::Unsupported);
		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t LocationUpdateRevision = Manager.Find(MaterialA)->Revision;
		const bool LocationUpdated = Manager.UpdateAssetLocation(
			MaterialA, "Assets/Renamed/Amber-Renamed.material", "Amber-Renamed.material");
		PF_CHECK(Tests, LocationUpdated);
		PF_CHECK(Tests, Manager.Find(MaterialA)->ProjectRelativePath ==
			std::filesystem::path("Assets/Renamed/Amber-Renamed.material"));
		PF_CHECK(Tests, Manager.Find(MaterialA)->DisplayName == "Amber-Renamed.material");
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == LocationUpdateRevision && Manager.Find(MaterialA)->IsDirty());

		const auto OtherFolderOpen = Manager.Open(SameNameOtherFolder, "Assets/Sub/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::MainDocumentStrip);
		const auto OtherProjectOpen = Manager.Open(OtherProjectMaterial, "Assets/Materials/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, OtherFolderOpen && OtherProjectOpen);
		PF_CHECK(Tests, Manager.GetSessions().size() == 4);
		PF_CHECK(Tests, Manager.Find(MaterialA) != Manager.Find(SameNameOtherFolder));
		PF_CHECK(Tests, Manager.Find(MaterialA) != Manager.Find(OtherProjectMaterial));
		PF_CHECK(Tests, Manager.FindHost(EditorDocumentManager::MainHostID)->Documents.size() == 1);
		PF_CHECK(Tests, OtherProjectOpen && OtherProjectOpen->HostID != OtherFolderOpen->HostID);
		PF_CHECK(Tests, !Manager.Transfer(MaterialA, OtherProjectOpen->HostID, 0));
		if (OtherProjectOpen)
		{
			const auto RejectedRecovery = Manager.RecoverAllToMain(OtherProjectOpen->HostID);
			PF_CHECK(Tests, !RejectedRecovery);
			PF_CHECK(Tests, Manager.Find(OtherProjectMaterial)->HostID == OtherProjectOpen->HostID);
			const auto* RejectedRecoveryHost = Manager.FindHost(OtherProjectOpen->HostID);
			PF_CHECK(Tests, RejectedRecoveryHost && RejectedRecoveryHost->Documents.size() == 1 &&
				RejectedRecoveryHost->Documents.front() == OtherProjectMaterial);
			PF_CHECK(Tests, Manager.FindHost(EditorDocumentManager::MainHostID)->Documents.size() == 1);
		}

		uint64_t TextureIdentity = 90;
		const auto ManagerPreferenceOpen = [&Manager, &Project, &TextureIdentity]()
		{
			const UUID PreferenceAsset(0, TextureIdentity++);
			return Manager.Open({ *Project, PreferenceAsset, EditorDocumentKind::Texture },
				"Assets/Textures/Albedo.png", "Albedo.png",
				EditorDocumentOpenPreference::RememberedPlacement);
		};
		const auto RememberedOne = ManagerPreferenceOpen();
		const auto RememberedTwo = ManagerPreferenceOpen();
		PF_CHECK(Tests, RememberedOne && RememberedTwo);
		PF_CHECK(Tests, RememberedOne && RememberedTwo && RememberedOne->HostID == RememberedTwo->HostID);
		const EditorDocumentIdentity OtherProjectTexture{ *OtherProject, *Scene, EditorDocumentKind::Texture };
		const auto OtherProjectTextureOpen = Manager.Open(OtherProjectTexture, "Assets/Textures/Normal.png", "Normal.png",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, OtherProjectTextureOpen && OtherProjectTextureOpen->HostID != EditorDocumentManager::MainHostID);
		PF_CHECK(Tests, !Manager.Transfer(MaterialA, OtherProjectTextureOpen->HostID, 0));

		const auto RecoveryTextureAID = UUID::Generate();
		const auto RecoveryTextureBID = UUID::Generate();
		PF_CHECK(Tests, RecoveryTextureAID && RecoveryTextureBID);
		if (RecoveryTextureAID && RecoveryTextureBID)
		{
			const EditorDocumentIdentity RecoveryTextureA{ *Project, *RecoveryTextureAID, EditorDocumentKind::Texture };
			const EditorDocumentIdentity RecoveryTextureB{ *Project, *RecoveryTextureBID, EditorDocumentKind::Texture };
			const auto RecoveryAOpen = Manager.Open(RecoveryTextureA, "Assets/Textures/A.png", "A.png",
				EditorDocumentOpenPreference::NewNativeWindow);
			PF_CHECK(Tests, RecoveryAOpen.has_value());
			if (RecoveryAOpen)
			{
				PF_CHECK(Tests, Manager.SetRememberedPlacement(*Project, EditorDocumentKind::Texture,
					EditorDocumentHostRole::Native, {}, RecoveryAOpen->HostID));
				const auto RecoveryBOpen = Manager.Open(RecoveryTextureB, "Assets/Textures/B.png", "B.png",
					EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, RecoveryBOpen && RecoveryBOpen->HostID == RecoveryAOpen->HostID);
				if (RecoveryBOpen)
				{
					PF_CHECK(Tests, Manager.MarkEdited(RecoveryTextureA));
					PF_CHECK(Tests, Manager.MarkEdited(RecoveryTextureB));
					const uint64_t RevisionA = Manager.Find(RecoveryTextureA)->Revision;
					const uint64_t RevisionB = Manager.Find(RecoveryTextureB)->Revision;
					PF_CHECK(Tests, Manager.Activate(RecoveryTextureA));
					const auto RecoveredCount = Manager.RecoverAllToMain(RecoveryAOpen->HostID);
					PF_CHECK(Tests, RecoveredCount && *RecoveredCount == 2);
					PF_CHECK(Tests, Manager.Find(RecoveryTextureA)->HostID == EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, Manager.Find(RecoveryTextureB)->HostID == EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, Manager.Find(RecoveryTextureA)->Revision == RevisionA &&
						Manager.Find(RecoveryTextureA)->IsDirty());
					PF_CHECK(Tests, Manager.Find(RecoveryTextureB)->Revision == RevisionB &&
						Manager.Find(RecoveryTextureB)->IsDirty());
					const auto* MainHost = Manager.FindHost(EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, MainHost && MainHost->ActiveDocument == RecoveryTextureA);
					const auto* RecoveredSource = Manager.FindHost(RecoveryAOpen->HostID);
					PF_CHECK(Tests, RecoveredSource && RecoveredSource->Documents.empty());
					PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(RecoveryAOpen->HostID));
					const auto RecoveryPlacements = Manager.GetRememberedPlacements(*Project);
					const auto TexturePlacement = std::find_if(RecoveryPlacements.begin(), RecoveryPlacements.end(),
						[](const auto& Entry) { return Entry.first == EditorDocumentKind::Texture; });
					PF_CHECK(Tests, TexturePlacement != RecoveryPlacements.end() &&
						TexturePlacement->second.Role == EditorDocumentHostRole::Native &&
						!TexturePlacement->second.LiveHostID);
				}
			}
		}

		const auto DetachAsset = UUID::Generate();
		PF_CHECK(Tests, DetachAsset.has_value());
		if (!DetachAsset)
			return;
		const EditorDocumentIdentity DetachMaterial{ *Project, *DetachAsset, EditorDocumentKind::Material };
		const auto DetachOpen = Manager.Open(DetachMaterial, "Assets/Materials/Detach.material", "Detach.material",
			EditorDocumentOpenPreference::MainDocumentStrip);
		const auto NewMaterialHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
		PF_CHECK(Tests, DetachOpen && DetachOpen->HostID == EditorDocumentManager::MainHostID);
		PF_CHECK(Tests, NewMaterialHost.has_value());
		if (DetachOpen && NewMaterialHost)
		{
			PF_CHECK(Tests, Manager.MarkEdited(DetachMaterial));
			const uint64_t DetachRevision = Manager.Find(DetachMaterial)->Revision;
			const auto RejectedDetach = Manager.Transfer(DetachMaterial, *NewMaterialHost, 0,
				[](const EditorDocumentSession&, const EditorDocumentHost&) { return false; });
			PF_CHECK(Tests, !RejectedDetach);
			PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == EditorDocumentManager::MainHostID);
			PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision);
			PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(*NewMaterialHost));

			const auto PreparedHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
			PF_CHECK(Tests, PreparedHost.has_value());
			if (PreparedHost)
			{
				const auto Detached = Manager.Transfer(DetachMaterial, *PreparedHost, 0,
					[](const EditorDocumentSession& Session, const EditorDocumentHost& Host)
					{
						return Host.Role == EditorDocumentHostRole::Native &&
							Host.PlacementKind == Session.Identity.Kind;
					});
				PF_CHECK(Tests, Detached && Detached->Changed);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == *PreparedHost);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision &&
					Manager.Find(DetachMaterial)->IsDirty());
				const auto Reattached = Manager.Transfer(
					DetachMaterial, EditorDocumentManager::MainHostID, 0);
				PF_CHECK(Tests, Reattached && Reattached->Changed);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == EditorDocumentManager::MainHostID);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision &&
					Manager.Find(DetachMaterial)->IsDirty());

				const auto Redetached = Manager.Transfer(DetachMaterial, *PreparedHost, 0);
				PF_CHECK(Tests, Redetached && Redetached->Changed);
				const auto TearOffHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
				PF_CHECK(Tests, TearOffHost.has_value());
				if (TearOffHost)
				{
					const auto TearOff = Manager.Transfer(DetachMaterial, *TearOffHost, 0);
					PF_CHECK(Tests, TearOff && TearOff->EmptiedSourceHost == *PreparedHost);
					PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == *TearOffHost);
					PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision &&
						Manager.Find(DetachMaterial)->IsDirty());
					PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(*PreparedHost));
				}
			}
		}

		const auto LevelOpen = Manager.Open(Level, {}, "Level Editor", EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, LevelOpen && LevelOpen->HostID == EditorDocumentManager::MainHostID);
		const auto* FocusedLevelHost = Manager.GetFocusedHost();
		const auto* FocusedLevelIdentity = FocusedLevelHost && FocusedLevelHost->ActiveDocument
			? &*FocusedLevelHost->ActiveDocument : nullptr;
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(FocusedLevelIdentity, *Project) ==
			EditorDocumentCommandTarget::Level);
		const auto ProtectedTransfer = Manager.Transfer(Level, First->HostID, 0);
		PF_CHECK(Tests, !ProtectedTransfer);

		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t DirtyRevision = Manager.Find(MaterialA)->Revision;
		const auto RejectedTransfer = Manager.Transfer(MaterialA, Second->HostID, 0,
			[](const EditorDocumentSession&, const EditorDocumentHost&) { return false; });
		PF_CHECK(Tests, !RejectedTransfer);
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == DirtyRevision);

		const auto Moved = Manager.Transfer(MaterialA, Second->HostID, 0,
			[](const EditorDocumentSession&, const EditorDocumentHost&) { return true; });
		PF_CHECK(Tests, Moved && Moved->Changed);
		PF_CHECK(Tests, Moved && Moved->EmptiedSourceHost == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == Second->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == DirtyRevision && Manager.Find(MaterialA)->IsDirty());
		PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(First->HostID));
		PF_CHECK(Tests, !Manager.FindHost(First->HostID));
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->HostID == Second->HostID);

		PF_CHECK(Tests, Manager.MarkSaved(MaterialA, DirtyRevision));
		PF_CHECK(Tests, !Manager.Find(MaterialA)->IsDirty());
		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t LaterRevision = Manager.Find(MaterialA)->Revision;
		const auto Cancelled = Manager.Close(MaterialA, EditorDocumentCloseChoice::Cancel, {}, {});
		PF_CHECK(Tests, Cancelled == EditorDocumentCloseStatus::Cancelled);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->Revision == LaterRevision);
		const auto SaveFailed = Manager.Close(MaterialA, EditorDocumentCloseChoice::Save,
			[](const EditorDocumentSession&, uint64_t) { return false; }, {});
		PF_CHECK(Tests, SaveFailed == EditorDocumentCloseStatus::SaveFailed);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->IsDirty());
		const auto SavedOlderRevision = Manager.Close(MaterialA, EditorDocumentCloseChoice::Save,
			[&Tests, &Manager, &MaterialA](const EditorDocumentSession&, uint64_t Revision)
			{
				PF_CHECK(Tests, Revision == Manager.Find(MaterialA)->Revision);
				PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
				return true;
			}, {});
		PF_CHECK(Tests, SavedOlderRevision == EditorDocumentCloseStatus::StillDirty);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->IsDirty());
		const auto Saved = Manager.Close(MaterialA, EditorDocumentCloseChoice::Save,
			[](const EditorDocumentSession&, uint64_t) { return true; }, {});
		PF_CHECK(Tests, Saved == EditorDocumentCloseStatus::Closed);
		PF_CHECK(Tests, !Manager.Find(MaterialA));

		PF_CHECK(Tests, Manager.MarkEdited(MaterialB));
		const auto Discarded = Manager.Close(MaterialB, EditorDocumentCloseChoice::Discard, {},
			[](const EditorDocumentSession&) { return true; });
		PF_CHECK(Tests, Discarded == EditorDocumentCloseStatus::Closed);
		PF_CHECK(Tests, Manager.Close(Level, EditorDocumentCloseChoice::Discard, {}, {}) ==
			EditorDocumentCloseStatus::Protected);

		const auto RemovedProjectHosts = Manager.ClearProject(*OtherProject);
		PF_CHECK(Tests, RemovedProjectHosts.size() == 2);
		PF_CHECK(Tests, OtherProjectTextureOpen && OtherProjectOpen &&
			std::find(RemovedProjectHosts.begin(), RemovedProjectHosts.end(), OtherProjectTextureOpen->HostID) !=
				RemovedProjectHosts.end() &&
			std::find(RemovedProjectHosts.begin(), RemovedProjectHosts.end(), OtherProjectOpen->HostID) !=
				RemovedProjectHosts.end());
		PF_CHECK(Tests, Manager.GetSessions().size() == 7);
		PF_CHECK(Tests, !Manager.Find(OtherProjectMaterial));
		PF_CHECK(Tests, !Manager.Find(OtherProjectTexture));
		PF_CHECK(Tests, !Manager.FindHost(OtherProjectTextureOpen->HostID));
		PF_CHECK(Tests, OtherProjectOpen && !Manager.FindHost(OtherProjectOpen->HostID));
		PF_CHECK(Tests, !Manager.Find(MaterialA) && !Manager.Find(MaterialB));

		const auto PlacementProject = UUID::Generate();
		const auto PlacementAssetA = UUID::Generate();
		const auto PlacementAssetB = UUID::Generate();
		const auto PlacementAssetC = UUID::Generate();
		PF_CHECK(Tests, PlacementProject && PlacementAssetA && PlacementAssetB && PlacementAssetC);
		if (PlacementProject && PlacementAssetA && PlacementAssetB && PlacementAssetC)
		{
			EditorDocumentManager PlacementManager;
			const EditorDocumentIdentity PlacementA{ *PlacementProject, *PlacementAssetA, EditorDocumentKind::Material };
			const EditorDocumentIdentity PlacementB{ *PlacementProject, *PlacementAssetB, EditorDocumentKind::Material };
			const EditorDocumentIdentity PlacementC{ *PlacementProject, *PlacementAssetC, EditorDocumentKind::Material };
			const auto MainOpen = PlacementManager.Open(PlacementA, "Assets/Materials/A.material", "A.material",
				EditorDocumentOpenPreference::MainDocumentStrip);
			PF_CHECK(Tests, MainOpen && MainOpen->HostID == EditorDocumentManager::MainHostID);
			const auto NativeHost = PlacementManager.CreateNativeHost(EditorDocumentKind::Material);
			PF_CHECK(Tests, NativeHost.has_value());
			EditorDocumentHost::WindowBounds SavedBounds{
				-1700.0f, 120.0f, 880.0f, 720.0f, 1.5f, "Left Display", true, true };
			if (MainOpen && NativeHost)
			{
				PF_CHECK(Tests, PlacementManager.SetHostBounds(*NativeHost, SavedBounds));
				PF_CHECK(Tests, PlacementManager.Transfer(PlacementA, *NativeHost, 0));
				const auto DetachedPlacement = PlacementManager.GetRememberedPlacements(*PlacementProject);
				PF_CHECK(Tests, DetachedPlacement.size() == 1);
				if (!DetachedPlacement.empty())
				{
					PF_CHECK(Tests, DetachedPlacement.front().second.Role == EditorDocumentHostRole::Native);
					PF_CHECK(Tests, !DetachedPlacement.front().second.LiveHostID);
				}
				const auto LiveReuse = PlacementManager.Open(PlacementB,
					"Assets/Materials/B.material", "B.material", EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, LiveReuse && LiveReuse->HostID == *NativeHost);
				PF_CHECK(Tests, PlacementManager.Close(PlacementA, EditorDocumentCloseChoice::Discard, {},
					[](const auto&) { return true; }) == EditorDocumentCloseStatus::Closed);
				if (LiveReuse)
					PF_CHECK(Tests, PlacementManager.Close(PlacementB, EditorDocumentCloseChoice::Discard, {},
						[](const auto&) { return true; }) == EditorDocumentCloseStatus::Closed);
				PF_CHECK(Tests, PlacementManager.RemoveEmptyNativeHost(*NativeHost));
				const auto ClosedPlacement = PlacementManager.GetRememberedPlacements(*PlacementProject);
				PF_CHECK(Tests, ClosedPlacement.size() == 1 && !ClosedPlacement.front().second.LiveHostID &&
					ClosedPlacement.front().second.Bounds.MonitorName == "Left Display" &&
					ClosedPlacement.front().second.Bounds.Maximized);
				const auto RememberedOpen = PlacementManager.Open(PlacementB, "Assets/Materials/B.material", "B.material",
					EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, RememberedOpen && RememberedOpen->HostID != *NativeHost);
				const auto* RestoredNative = RememberedOpen ? PlacementManager.FindHost(RememberedOpen->HostID) : nullptr;
				PF_CHECK(Tests, RestoredNative && RestoredNative->Bounds.X == SavedBounds.X &&
					RestoredNative->Bounds.MonitorName == SavedBounds.MonitorName && RestoredNative->Bounds.Maximized);
				PF_CHECK(Tests, PlacementManager.Transfer(PlacementB, EditorDocumentManager::MainHostID, 0));
				const auto MainRememberedOpen = PlacementManager.Open(PlacementC,
					"Assets/Materials/C.material", "C.material", EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, MainRememberedOpen && MainRememberedOpen->HostID == EditorDocumentManager::MainHostID);
				const auto MainPlacement = PlacementManager.GetRememberedPlacements(*PlacementProject);
				PF_CHECK(Tests, MainPlacement.size() == 1 &&
					MainPlacement.front().second.Role == EditorDocumentHostRole::Main &&
					MainPlacement.front().second.Bounds.MonitorName == "Left Display" &&
					MainPlacement.front().second.Bounds.Maximized);

				const auto IsolatedPlacementProject = UUID::Generate();
				const auto IsolatedPlacementAsset = UUID::Generate();
				PF_CHECK(Tests, IsolatedPlacementProject && IsolatedPlacementAsset);
				if (IsolatedPlacementProject && IsolatedPlacementAsset)
				{
					PF_CHECK(Tests, PlacementManager.SetRememberedPlacement(*IsolatedPlacementProject,
						EditorDocumentKind::Material, EditorDocumentHostRole::Native, SavedBounds));
					const EditorDocumentIdentity IsolatedIdentity{
						*IsolatedPlacementProject, *IsolatedPlacementAsset, EditorDocumentKind::Material };
					const auto IsolatedOpen = PlacementManager.Open(IsolatedIdentity,
						"Assets/Materials/Isolated.material", "Isolated.material",
						EditorDocumentOpenPreference::RememberedPlacement);
					const auto* IsolatedHost = IsolatedOpen
						? PlacementManager.FindHost(IsolatedOpen->HostID) : nullptr;
					PF_CHECK(Tests, IsolatedOpen && IsolatedOpen->HostID != EditorDocumentManager::MainHostID &&
						IsolatedHost && IsolatedHost->Bounds.MonitorName == "Left Display");
					PF_CHECK(Tests, PlacementManager.GetRememberedPlacements(*PlacementProject).front().second.Role ==
						EditorDocumentHostRole::Main);
				}
			}

			const auto CrossProjectA = UUID::Generate();
			const auto CrossProjectB = UUID::Generate();
			const auto CrossProjectAssetA = UUID::Generate();
			const auto CrossProjectAssetB = UUID::Generate();
			PF_CHECK(Tests, CrossProjectA && CrossProjectB && CrossProjectAssetA && CrossProjectAssetB);
			if (CrossProjectA && CrossProjectB && CrossProjectAssetA && CrossProjectAssetB)
			{
				EditorDocumentManager CrossProjectManager;
				const EditorDocumentIdentity ProjectTexture{
					*CrossProjectA, *CrossProjectAssetA, EditorDocumentKind::Texture };
				const EditorDocumentIdentity OtherProjectTexture{
					*CrossProjectB, *CrossProjectAssetB, EditorDocumentKind::Texture };
				const auto OriginalOpen = CrossProjectManager.Open(ProjectTexture, "Assets/Textures/A.png", "A.png",
					EditorDocumentOpenPreference::NewNativeWindow);
				PF_CHECK(Tests, OriginalOpen.has_value());
				if (OriginalOpen)
				{
					PF_CHECK(Tests, CrossProjectManager.Close(ProjectTexture, EditorDocumentCloseChoice::Discard, {}, {}) ==
						EditorDocumentCloseStatus::Closed);
					const auto OtherOpen = CrossProjectManager.Open(OtherProjectTexture,
					"Assets/Textures/B.png", "B.png", EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, OtherOpen && OtherOpen->HostID != OriginalOpen->HostID);
					if (OtherOpen)
					{
						const auto DestinationBefore = CrossProjectManager.FindHost(OriginalOpen->HostID)->Documents;
						const auto SourceBefore = CrossProjectManager.FindHost(OtherOpen->HostID)->Documents;
						const auto FocusedBefore = CrossProjectManager.GetFocusedHost()->ID;
						const auto Rejected = CrossProjectManager.Transfer(
							OtherProjectTexture, OriginalOpen->HostID, 0);
						PF_CHECK(Tests, !Rejected);
						PF_CHECK(Tests, CrossProjectManager.FindHost(OriginalOpen->HostID)->Documents == DestinationBefore &&
							CrossProjectManager.FindHost(OtherOpen->HostID)->Documents == SourceBefore &&
							CrossProjectManager.GetFocusedHost()->ID == FocusedBefore);
					}
				}
			}

			const auto InvalidProject = UUID::Generate();
			const auto InvalidPlacementAsset = UUID::Generate();
			PF_CHECK(Tests, InvalidProject && InvalidPlacementAsset);
			if (InvalidProject && InvalidPlacementAsset)
			{
				EditorDocumentManager InvalidPlacementManager;
				EditorDocumentHost::WindowBounds InvalidBounds;
				InvalidBounds.X = std::numeric_limits<float>::quiet_NaN();
				InvalidBounds.HasValue = true;
				PF_CHECK(Tests, InvalidPlacementManager.SetRememberedPlacement(*InvalidProject,
					EditorDocumentKind::Texture, EditorDocumentHostRole::Native, InvalidBounds));
				const EditorDocumentIdentity InvalidPlacementIdentity{
					*InvalidProject, *InvalidPlacementAsset, EditorDocumentKind::Texture };
				const auto Fallback = InvalidPlacementManager.Open(InvalidPlacementIdentity,
					"Assets/Textures/T.png", "T.png", EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, Fallback && Fallback->HostID != EditorDocumentManager::MainHostID);
				const auto* FallbackHost = Fallback ? InvalidPlacementManager.FindHost(Fallback->HostID) : nullptr;
				PF_CHECK(Tests, FallbackHost && !FallbackHost->Bounds.HasValue);
			}

			EditorDocumentManager InvalidBatchManager;
			const auto BatchAID = UUID::Generate();
			const auto BatchBID = UUID::Generate();
			PF_CHECK(Tests, BatchAID && BatchBID);
			if (BatchAID && BatchBID)
			{
				const EditorDocumentIdentity BatchA{ *PlacementProject, *BatchAID, EditorDocumentKind::Texture };
				const EditorDocumentIdentity BatchB{ *PlacementProject, *BatchBID, EditorDocumentKind::Texture };
				const auto BatchHost = InvalidBatchManager.CreateNativeHost(EditorDocumentKind::Texture);
				const auto BatchOpenA = InvalidBatchManager.Open(BatchA, {}, "A", EditorDocumentOpenPreference::NewNativeWindow);
				const auto BatchOpenB = InvalidBatchManager.Open(BatchB, {}, "B", EditorDocumentOpenPreference::NewNativeWindow);
				PF_CHECK(Tests, BatchHost && BatchOpenA && BatchOpenB);
				if (BatchHost && BatchOpenA && BatchOpenB)
				{
					const auto MoveA = InvalidBatchManager.Transfer(BatchA, *BatchHost, 0);
					const auto MoveB = InvalidBatchManager.Transfer(BatchB, *BatchHost, 1);
					PF_CHECK(Tests, MoveA && MoveB);
					const auto Before = InvalidBatchManager.FindHost(*BatchHost)->Documents;
					InvalidBatchManager.FindHost(*BatchHost)->Documents.push_back(BatchA);
					const auto DuplicateRejected = InvalidBatchManager.RecoverAllToMain(*BatchHost);
					PF_CHECK(Tests, !DuplicateRejected &&
						InvalidBatchManager.FindHost(*BatchHost)->Documents.size() == Before.size() + 1 &&
						InvalidBatchManager.Find(BatchA)->HostID == *BatchHost &&
						InvalidBatchManager.Find(BatchB)->HostID == *BatchHost);
					const auto MainRecoveryRejected = InvalidBatchManager.RecoverAllToMain(
						EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, !MainRecoveryRejected);
				}
			}
		}
	}

	void TestEditorSnapshotHistory(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorSnapshotHistory;
		EditorSnapshotHistory<int> History;
		History.Reset(10);
		PF_CHECK(Tests, !History.IsDirty() && !History.CanUndo() && !History.CanRedo());
		History.Record(11);
		PF_CHECK(Tests, History.IsDirty() && History.GetCurrent() == 11 && History.CanUndo());
		PF_CHECK(Tests, History.Undo());
		PF_CHECK(Tests, History.GetCurrent() == 10 && !History.IsDirty() && History.CanRedo());
		PF_CHECK(Tests, History.Redo());
		History.MarkSaved();
		PF_CHECK(Tests, History.GetCurrent() == 11 && !History.IsDirty());
		PF_CHECK(Tests, History.Undo() && History.IsDirty());
		History.Record(12);
		PF_CHECK(Tests, History.GetCurrent() == 12 && !History.CanRedo() && History.IsDirty());

		History.Reset(0);
		History.Record(1, 3);
		History.MarkSaved();
		History.Record(2, 3);
		History.Record(3, 3);
		PF_CHECK(Tests, History.GetStateCount() == 3 && History.GetCurrent() == 3 && History.IsDirty());
		PF_CHECK(Tests, History.Undo() && History.GetCurrent() == 2 && History.IsDirty());
		PF_CHECK(Tests, History.Undo() && History.GetCurrent() == 1 && !History.IsDirty());
		History.Record(4, 3);
		PF_CHECK(Tests, History.GetStateCount() == 2 && History.GetCurrent() == 4 && !History.CanRedo());
		History.Record(5, 1);
		PF_CHECK(Tests, History.GetStateCount() == 1 && History.GetCurrent() == 5 && History.IsDirty());
	}

	void TestEditorDocumentEditorRegistry(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		struct Model final : EditorDocumentEditorState
		{
			int Value = 0;
			std::vector<int> Undo;
			std::vector<int> Redo;
			bool FailSave = false;
			bool FailClose = false;
			bool EditDuringSave = false;
		};

		const auto Project = UUID::Generate();
		const auto AssetA = UUID::Generate();
		const auto AssetB = UUID::Generate();
		PF_CHECK(Tests, Project && AssetA && AssetB);
		if (!Project || !AssetA || !AssetB)
			return;

		std::unordered_map<UUID, int, UUIDHash> SavedValues;
		EditorDocumentEditorRegistry Registry;
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::Texture;
		Adapter.IsEligible = [](const EditorDocumentEditorContext& Context)
		{
			return Context.Identity.Kind == EditorDocumentKind::Texture &&
				Context.ProjectRelativePath.extension() == ".fixture";
		};
		Adapter.LoadSession = [&SavedValues](const EditorDocumentEditorContext& Context)
			-> std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
		{
			auto State = std::make_unique<Model>();
			if (const auto Saved = SavedValues.find(Context.Identity.Asset); Saved != SavedValues.end())
				State->Value = Saved->second;
			return std::unique_ptr<EditorDocumentEditorState>(std::move(State));
		};
		Adapter.DrawWorkspace = [](EditorDocumentSession&) {};
		Adapter.Save = [&SavedValues](EditorDocumentSession& Session, uint64_t)
		{
			auto* State = dynamic_cast<Model*>(Session.EditorState.get());
			if (!State || State->FailSave)
				return false;
			SavedValues[Session.Identity.Asset] = State->Value;
			if (State->EditDuringSave)
			{
				State->EditDuringSave = false;
				State->Undo.push_back(State->Value);
				++State->Value;
				++Session.Revision;
			}
			return true;
		};
		Adapter.Discard = [&SavedValues](EditorDocumentSession& Session)
		{
			auto* State = dynamic_cast<Model*>(Session.EditorState.get());
			if (!State)
				return false;
			State->Value = SavedValues[Session.Identity.Asset];
			State->Undo.clear();
			State->Redo.clear();
			return true;
		};
		Adapter.FocusedCommand = [](EditorDocumentSession& Session, EditorDocumentCommand Command)
		{
			auto* State = dynamic_cast<Model*>(Session.EditorState.get());
			if (!State)
				return false;
			if (Command == EditorDocumentCommand::Undo && !State->Undo.empty())
			{
				State->Redo.push_back(State->Value);
				State->Value = State->Undo.back();
				State->Undo.pop_back();
				++Session.Revision;
				return true;
			}
			if (Command == EditorDocumentCommand::Redo && !State->Redo.empty())
			{
				State->Undo.push_back(State->Value);
				State->Value = State->Redo.back();
				State->Redo.pop_back();
				++Session.Revision;
				return true;
			}
			return false;
		};
		Adapter.AssetLocationChanged = [](EditorDocumentSession&, const std::filesystem::path&, std::string_view)
		{
			return true;
		};
		Adapter.PrepareClose = [](const EditorDocumentSession& Session) -> std::expected<void, std::string>
		{
			const auto* State = dynamic_cast<const Model*>(Session.EditorState.get());
			if (State && State->FailClose)
				return std::unexpected("close rejected by test fixture");
			return {};
		};
		size_t CloseCount = 0;
		Adapter.Close = [&CloseCount](EditorDocumentSession&) { ++CloseCount; };
		PF_CHECK(Tests, !Registry.IsRegistered(EditorDocumentKind::Texture));
		PF_CHECK(Tests, Registry.Register(Adapter).has_value());
		PF_CHECK(Tests, Registry.IsRegistered(EditorDocumentKind::Texture));
		PF_CHECK(Tests, !Registry.Register(Adapter));

		const EditorDocumentIdentity IdentityA{ *Project, *AssetA, EditorDocumentKind::Texture };
		const EditorDocumentIdentity IdentityB{ *Project, *AssetB, EditorDocumentKind::Texture };
		EditorDocumentEditorContext ContextA{ IdentityA, "Projects/Fixture", "Assets/A.fixture", "A.fixture", nullptr };
		EditorDocumentEditorContext ContextB{ IdentityB, "Projects/Fixture", "Assets/B.fixture", "B.fixture", nullptr };
		PF_CHECK(Tests, Registry.CanOpen(ContextA));
		ContextA.ProjectRelativePath = "Assets/A.png";
		PF_CHECK(Tests, !Registry.CanOpen(ContextA));
		ContextA.ProjectRelativePath = "Assets/A.fixture";

		EditorDocumentManager Manager;
		const auto OpenA = Manager.Open(IdentityA, ContextA.ProjectRelativePath, ContextA.DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip);
		const auto OpenB = Manager.Open(IdentityB, ContextB.ProjectRelativePath, ContextB.DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, OpenA && OpenB);
		auto* SessionA = Manager.Find(IdentityA);
		auto* SessionB = Manager.Find(IdentityB);
		PF_CHECK(Tests, SessionA && SessionB && Registry.Load(*SessionA, ContextA) &&
			Registry.Load(*SessionB, ContextB));
		auto* StateA = SessionA ? dynamic_cast<Model*>(SessionA->EditorState.get()) : nullptr;
		auto* StateB = SessionB ? dynamic_cast<Model*>(SessionB->EditorState.get()) : nullptr;
		PF_CHECK(Tests, StateA && StateB && StateA != StateB);
		if (!StateA || !StateB || !SessionA || !SessionB)
			return;
		StateA->Value = 11;
		StateA->Undo.push_back(0);
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));
		StateB->Value = 22;
		StateB->Undo.push_back(0);
		PF_CHECK(Tests, Manager.MarkEdited(IdentityB));
		StateA->FailSave = true;
		PF_CHECK(Tests, !Registry.Save(*SessionA) && SessionA->IsDirty());
		StateA->FailSave = false;
		StateA->EditDuringSave = true;
		PF_CHECK(Tests, Registry.Save(*SessionA) && SessionA->IsDirty() && SavedValues[*AssetA] == 11);
		PF_CHECK(Tests, Registry.Save(*SessionA) && !SessionA->IsDirty());
		StateA->Value = 99;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));
		PF_CHECK(Tests, Registry.Discard(*SessionA) && StateA->Value == 12 && !SessionA->IsDirty());
		StateA->Undo.push_back(StateA->Value);
		StateA->Value = 0;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));

		const auto NativeHost = Manager.CreateNativeHost(EditorDocumentKind::Texture);
		PF_CHECK(Tests, NativeHost.has_value());
		const EditorDocumentEditorState* OriginalStateAddress = SessionA->EditorState.get();
		if (NativeHost)
		{
			PF_CHECK(Tests, Manager.Transfer(IdentityA, *NativeHost, 0).has_value());
			PF_CHECK(Tests, Manager.Find(IdentityA)->EditorState.get() == OriginalStateAddress && StateA->Value == 0);
		}
		PF_CHECK(Tests, Manager.Activate(IdentityA));
		PF_CHECK(Tests, Registry.DispatchFocusedCommand(*SessionA, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, StateA->Value == 12 && StateB->Value == 22);
		const uint64_t BeforeLocationChangeRevision = SessionA->Revision;
		PF_CHECK(Tests, Registry.UpdateAssetLocation(*SessionA, "Assets/Renamed/A.fixture", "Renamed.fixture"));
		PF_CHECK(Tests, SessionA->ProjectRelativePath == "Assets/Renamed/A.fixture" &&
			SessionA->Revision == BeforeLocationChangeRevision);

		StateA->Value = 13;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA));
		StateA->FailClose = true;
		const std::array<EditorDocumentIdentity, 1> SingleCloseIdentity = { IdentityA };
		size_t DestructiveDiscardCount = 0;
		std::function<void()> PendingDiscardContinuation = [] {};
		std::vector<std::function<void()>> DiscardContinuations;
		const auto RejectedDirtyDiscard = ResolveDocumentCloseBatchWithChanges(
			Manager, Registry, SingleCloseIdentity, true,
			[&]() -> std::expected<void, std::string>
			{
				++DestructiveDiscardCount;
				return Registry.Discard(*SessionA)
					? std::expected<void, std::string>{}
					: std::unexpected("discard rejected by test fixture");
			},
			PendingDiscardContinuation,
			[&DiscardContinuations](std::function<void()> Action)
			{
				DiscardContinuations.push_back(std::move(Action));
			});
		PF_CHECK(Tests, !RejectedDirtyDiscard && DestructiveDiscardCount == 0 && PendingDiscardContinuation);
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && StateA->Value == 13 && SessionA->IsDirty());
		PF_CHECK(Tests, DiscardContinuations.empty() && CloseCount == 0);
		const auto FailedPreparation = Registry.PrepareClose(*SessionA);
		PF_CHECK(Tests, !FailedPreparation && CloseCount == 0 && Manager.Find(IdentityA) == SessionA && StateA->Value == 13);
		StateA->FailClose = false;
		PF_CHECK(Tests, Registry.PrepareClose(*SessionA).has_value() && CloseCount == 0);
		PF_CHECK(Tests, Manager.Close(IdentityA, EditorDocumentCloseChoice::Cancel,
			[&Registry](const auto&, uint64_t) { return false; },
			[&Registry](const auto&) { return false; }) == EditorDocumentCloseStatus::Cancelled);
		PF_CHECK(Tests, Manager.Find(IdentityA) && Manager.Find(IdentityA)->EditorState.get() == OriginalStateAddress);

		EditorDocumentEditorRegistry RestartRegistry;
		PF_CHECK(Tests, RestartRegistry.Register(Adapter).has_value());
		EditorDocumentManager RestartManager;
		const auto RestoredOpen = RestartManager.Open(IdentityA, "Assets/Renamed/A.fixture", "Renamed.fixture",
			EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, RestoredOpen.has_value());
		if (RestoredOpen)
		{
			auto* RestoredSession = RestartManager.Find(IdentityA);
			EditorDocumentEditorContext RestoredContext = ContextA;
			RestoredContext.ProjectRelativePath = "Assets/Renamed/A.fixture";
			PF_CHECK(Tests, RestoredSession && RestartRegistry.Load(*RestoredSession, RestoredContext));
			const auto* RestoredModel = RestoredSession
				? dynamic_cast<const Model*>(RestoredSession->EditorState.get()) : nullptr;
			PF_CHECK(Tests, RestoredModel && RestoredModel->Value == 12);
		}

		PF_CHECK(Tests, Registry.Save(*SessionA));
		StateA->Value = 77;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA) && Registry.Save(*SessionA));
		StateA->Undo.push_back(StateA->Value);
		StateA->Value = 78;
		PF_CHECK(Tests, Manager.MarkEdited(IdentityA) && SessionA->IsDirty());
		PF_CHECK(Tests, Registry.Save(*SessionA) && !SessionA->IsDirty() && SavedValues[*AssetA] == 78);
		StateA->FailClose = true;
		size_t SingleCloseContinuationCount = 0;
		std::function<void()> PendingSingleCloseContinuation = [&SingleCloseContinuationCount]
		{
			++SingleCloseContinuationCount;
		};
		std::vector<std::function<void()>> SingleCloseContinuations;
		const auto FailedSavedDirtyTabClose = ResolveDocumentCloseBatch(
			Manager, Registry, SingleCloseIdentity, PendingSingleCloseContinuation,
			[&SingleCloseContinuations](std::function<void()> Action)
			{
				SingleCloseContinuations.push_back(std::move(Action));
			});
		PF_CHECK(Tests, !FailedSavedDirtyTabClose && PendingSingleCloseContinuation &&
			SingleCloseContinuations.empty() && SingleCloseContinuationCount == 0);
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && SessionA->EditorState.get() == OriginalStateAddress &&
			!SessionA->IsDirty() && StateA->Value == 78 && SavedValues[*AssetA] == 78);
		PendingSingleCloseContinuation = {};
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && StateA->Value == 78 && SingleCloseContinuationCount == 0);
		StateA->FailClose = false;
		PF_CHECK(Tests, Registry.Save(*SessionB));
		if (NativeHost)
		{
			const auto* Host = Manager.FindHost(*NativeHost);
			const size_t DestinationIndex = Host ? Host->Documents.size() : 0;
			PF_CHECK(Tests, Manager.Transfer(IdentityB, *NativeHost, DestinationIndex).has_value());
		}
		StateB->FailClose = true;
		const auto* SharedHostBeforeClose = Manager.FindHost(SessionA->HostID);
		const auto HostOrderBeforeClose = SharedHostBeforeClose ? SharedHostBeforeClose->Documents
			: std::vector<EditorDocumentIdentity>{};
		const auto ActiveBeforeClose = SharedHostBeforeClose ? SharedHostBeforeClose->ActiveDocument
			: std::optional<EditorDocumentIdentity>{};
		const std::array<EditorDocumentIdentity, 2> BatchIdentities = { IdentityA, IdentityB };
		auto RejectedBatch = EditorDocumentCloseBatch::Prepare(Manager, Registry, BatchIdentities);
		PF_CHECK(Tests, !RejectedBatch && CloseCount == 0);
		PF_CHECK(Tests, SharedHostBeforeClose && HostOrderBeforeClose.size() == 2 &&
			SessionA->HostID == SessionB->HostID);
		PF_CHECK(Tests, SavedValues[*AssetA] == 78 && Manager.Find(IdentityA) == SessionA && !SessionA->IsDirty());
		PF_CHECK(Tests, Manager.Find(IdentityB) == SessionB && StateB->Value == 22);
		const auto* SharedHostAfterFailure = Manager.FindHost(SessionA->HostID);
		PF_CHECK(Tests, SharedHostAfterFailure && SharedHostAfterFailure->Documents == HostOrderBeforeClose &&
			SharedHostAfterFailure->ActiveDocument == ActiveBeforeClose);
		size_t ProjectSwitchContinuationCount = 0;
		size_t ShutdownContinuationCount = 0;
		std::vector<std::function<void()>> QueuedContinuations;
		std::function<void()> PendingProjectSwitchContinuation = [&ProjectSwitchContinuationCount]
		{
			++ProjectSwitchContinuationCount;
		};
		const auto EnqueueContinuation = [&QueuedContinuations](std::function<void()> Action)
		{
			QueuedContinuations.push_back(std::move(Action));
		};
		const auto FailedProjectSwitchClose = ResolveDocumentCloseBatch(
			Manager, Registry, BatchIdentities, PendingProjectSwitchContinuation, EnqueueContinuation);
		PF_CHECK(Tests, !FailedProjectSwitchClose && PendingProjectSwitchContinuation && QueuedContinuations.empty());
		PF_CHECK(Tests, ProjectSwitchContinuationCount == 0 && ShutdownContinuationCount == 0 &&
			CloseCount == 0 && Manager.Find(IdentityA) == SessionA &&
			Manager.Find(IdentityB) == SessionB);
		PendingProjectSwitchContinuation = {};
		PF_CHECK(Tests, Manager.Find(IdentityA) == SessionA && Manager.Find(IdentityB) == SessionB &&
			ProjectSwitchContinuationCount == 0);
		std::function<void()> PendingShutdownContinuation = [&ShutdownContinuationCount]
		{
			++ShutdownContinuationCount;
		};
		const auto FailedShutdownClose = ResolveDocumentCloseBatch(
			Manager, Registry, BatchIdentities, PendingShutdownContinuation, EnqueueContinuation);
		PF_CHECK(Tests, !FailedShutdownClose && PendingShutdownContinuation && QueuedContinuations.empty() &&
			ShutdownContinuationCount == 0 && Manager.Find(IdentityA) == SessionA && Manager.Find(IdentityB) == SessionB);
		StateB->FailClose = false;
		const auto RetriedCoordinatedClose = ResolveDocumentCloseBatch(
			Manager, Registry, BatchIdentities, PendingShutdownContinuation, EnqueueContinuation);
		PF_CHECK(Tests, RetriedCoordinatedClose.has_value());
		PF_CHECK(Tests, !PendingShutdownContinuation && QueuedContinuations.size() == 1 &&
			ProjectSwitchContinuationCount == 0 && ShutdownContinuationCount == 0);
		PF_CHECK(Tests, CloseCount == 2 && !Manager.Find(IdentityA) && !Manager.Find(IdentityB));
		QueuedContinuations.front()();
		PF_CHECK(Tests, ShutdownContinuationCount == 1 && ProjectSwitchContinuationCount == 0);
		PendingShutdownContinuation = [&ShutdownContinuationCount] { ShutdownContinuationCount += 100; };
		PendingShutdownContinuation = {};
		PF_CHECK(Tests, QueueDocumentCloseContinuation(true, PendingShutdownContinuation, EnqueueContinuation));
		PF_CHECK(Tests, QueuedContinuations.size() == 1 && ShutdownContinuationCount == 1);
	}

	void TestEditorDocumentWorkspaceCodec(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Project = UUID::Generate();
		const auto MaterialA = UUID::Generate();
		const auto MaterialB = UUID::Generate();
		PF_CHECK(Tests, Project && MaterialA && MaterialB);
		if (!Project || !MaterialA || !MaterialB)
			return;

		const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
		const EditorDocumentIdentity Amber{ *Project, *MaterialA, EditorDocumentKind::Material };
		const EditorDocumentIdentity Blue{ *Project, *MaterialB, EditorDocumentKind::Material };
		EditorDocumentHost Main;
		Main.ID = std::string(EditorDocumentManager::MainHostID);
		Main.Role = EditorDocumentHostRole::Main;
		Main.PlacementKind = EditorDocumentKind::Level;
		Main.Documents = { Level };
		Main.ActiveDocument = Level;
		EditorDocumentHost Native;
		Native.ID = "native:01234567-89ab-cdef-0123-456789abcdef";
		Native.Role = EditorDocumentHostRole::Native;
		Native.PlacementKind = EditorDocumentKind::Material;
		Native.Documents = { Amber, Blue };
		Native.ActiveDocument = Blue;
		Native.Bounds = { -1920.0f, 80.0f, 900.0f, 760.0f, 1.5f, "Display-Left", true, true };

		EditorDocumentWorkspaceState State;
		State.Project = *Project;
		State.MaterialDetailsAsset = *MaterialA;
		State.Hosts = { Main, Native };
		State.RememberedPlacements.emplace_back(EditorDocumentKind::Material,
			EditorDocumentPlacement{ EditorDocumentHostRole::Native, Native.Bounds, std::nullopt });
		State.RememberedPlacements.emplace_back(EditorDocumentKind::Texture,
			EditorDocumentPlacement{ EditorDocumentHostRole::Main, Native.Bounds, std::nullopt });
		State.FocusedHostID = Native.ID;
		const auto Serialized = EditorDocumentWorkspaceCodec::Serialize(State);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;

		const auto Restored = EditorDocumentWorkspaceCodec::Deserialize(*Serialized, *Project);
		PF_CHECK(Tests, Restored.has_value());
		if (Restored)
		{
			PF_CHECK(Tests, Restored->Hosts.size() == 2 && Restored->Hosts[1].Documents.size() == 2);
			PF_CHECK(Tests, Restored->MaterialDetailsAsset == MaterialA);
			PF_CHECK(Tests, Restored->Hosts[1].ActiveDocument == Blue);
			PF_CHECK(Tests, Restored->Hosts[1].Bounds.X == -1920.0f && Restored->Hosts[1].Bounds.Maximized);
			PF_CHECK(Tests, Restored->FocusedHostID == Native.ID && Restored->RememberedPlacements.size() == 2);
			const auto NativePlacement = std::find_if(Restored->RememberedPlacements.begin(),
				Restored->RememberedPlacements.end(), [](const auto& Entry)
				{
					return Entry.first == EditorDocumentKind::Material;
				});
			const auto MainPlacement = std::find_if(Restored->RememberedPlacements.begin(),
				Restored->RememberedPlacements.end(), [](const auto& Entry)
				{
					return Entry.first == EditorDocumentKind::Texture;
				});
			PF_CHECK(Tests, NativePlacement != Restored->RememberedPlacements.end() &&
				NativePlacement->second.Role == EditorDocumentHostRole::Native &&
				NativePlacement->second.Bounds.MonitorName == "Display-Left" &&
				!NativePlacement->second.LiveHostID);
			PF_CHECK(Tests, MainPlacement != Restored->RememberedPlacements.end() &&
				MainPlacement->second.Role == EditorDocumentHostRole::Main &&
				MainPlacement->second.Bounds.MonitorName == "Display-Left" &&
				MainPlacement->second.Bounds.Maximized);
			const auto Candidates = GetMaterialDetailsMigrationCandidates(*Restored);
			PF_CHECK(Tests, Candidates.size() == 2 && Candidates[0] == *MaterialB && Candidates[1] == *MaterialA);
		}
		auto LegacyWorkspace = nlohmann::json::parse(*Serialized);
		LegacyWorkspace["version"] = 2;
		LegacyWorkspace.erase("rememberedPlacements");
		LegacyWorkspace["rememberedHosts"] = nlohmann::json::array({
			{ { "kind", static_cast<uint8_t>(EditorDocumentKind::Material) }, { "host", Native.ID } },
			{ { "kind", static_cast<uint8_t>(EditorDocumentKind::Texture) }, { "host", "native:missing" } }
		});
		const auto MigratedLegacy = EditorDocumentWorkspaceCodec::Deserialize(LegacyWorkspace.dump(), *Project);
		PF_CHECK(Tests, MigratedLegacy.has_value());
		if (MigratedLegacy)
		{
			PF_CHECK(Tests, MigratedLegacy->Version == 3 && MigratedLegacy->MaterialDetailsAsset == MaterialA);
			PF_CHECK(Tests, MigratedLegacy->Hosts[1].Documents.size() == 2);
			PF_CHECK(Tests, MigratedLegacy->RememberedPlacements.size() == 1 &&
				MigratedLegacy->RememberedPlacements.front().second.Bounds.Maximized);
		}

		EditorDocumentWorkspaceState LegacyMainStripState = State;
		LegacyMainStripState.MaterialDetailsAsset.reset();
		LegacyMainStripState.Hosts[0].Documents = { Level, Amber, Blue };
		LegacyMainStripState.Hosts[0].ActiveDocument = Blue;
		LegacyMainStripState.Hosts.erase(LegacyMainStripState.Hosts.begin() + 1);
		LegacyMainStripState.RememberedPlacements.clear();
		LegacyMainStripState.FocusedHostID = std::string(EditorDocumentManager::MainHostID);
		const auto LegacyMainStripJSON = EditorDocumentWorkspaceCodec::Serialize(LegacyMainStripState);
		PF_CHECK(Tests, LegacyMainStripJSON.has_value());
		if (LegacyMainStripJSON)
		{
			auto LegacyMainStrip = nlohmann::json::parse(*LegacyMainStripJSON);
			LegacyMainStrip["version"] = 1;
			LegacyMainStrip.erase("materialDetailsAsset");
			LegacyMainStrip.erase("rememberedPlacements");
			LegacyMainStrip["rememberedHosts"] = nlohmann::json::array();
			const auto MigratedMainStrip = EditorDocumentWorkspaceCodec::Deserialize(
				LegacyMainStrip.dump(), *Project);
			PF_CHECK(Tests, MigratedMainStrip.has_value());
			if (MigratedMainStrip)
			{
				const auto Candidates = GetMaterialDetailsMigrationCandidates(*MigratedMainStrip);
				PF_CHECK(Tests, Candidates.size() == 2 && Candidates[0] == *MaterialB && Candidates[1] == *MaterialA);
				PF_CHECK(Tests, MigratedMainStrip->Hosts.size() == 1 &&
					MigratedMainStrip->Hosts[0].Documents[0] == Level);
			}
		}
		const auto OtherProject = UUID::Generate();
		PF_CHECK(Tests, OtherProject &&
			!EditorDocumentWorkspaceCodec::Deserialize(*Serialized, *OtherProject));
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize("{not-json", *Project));

		auto Duplicate = nlohmann::json::parse(*Serialized);
		Duplicate["hosts"][1]["documents"].push_back(Duplicate["hosts"][1]["documents"][0]);
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize(Duplicate.dump(), *Project));
		auto InvalidBounds = nlohmann::json::parse(*Serialized);
		InvalidBounds["hosts"][1]["bounds"]["width"] = 0.0f;
		const auto RecoveredBounds = EditorDocumentWorkspaceCodec::Deserialize(InvalidBounds.dump(), *Project);
		PF_CHECK(Tests, RecoveredBounds && !RecoveredBounds->Hosts[1].Bounds.HasValue &&
			RecoveredBounds->Hosts[1].Documents.size() == 2);
		auto MissingLevel = nlohmann::json::parse(*Serialized);
		MissingLevel["hosts"][0]["documents"] = nlohmann::json::array();
		MissingLevel["hosts"][0]["active"] = nullptr;
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize(MissingLevel.dump(), *Project));
	}

	void TestEditorDocumentWorkspaceStore(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto ProjectID = UUID::Generate();
		const auto MaterialID = UUID::Generate();
		PF_CHECK(Tests, ProjectID && MaterialID);
		if (!ProjectID || !MaterialID)
			return;

		std::error_code Error;
		const std::filesystem::path Root = std::filesystem::temp_directory_path(Error) /
			("PulseForgeWorkspaceStore-" + ProjectID->ToString());
		PF_CHECK(Tests, !Error);
		if (Error)
			return;
		std::filesystem::create_directories(Root, Error);
		PF_CHECK(Tests, !Error);
		if (Error)
			return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} RemoveRoot{ Root };

		const std::filesystem::path Path = Root / "workspace.json";
		EditorDocumentWorkspaceStore Store;
		Store.Configure(Path);
		const auto Missing = Store.Load(*ProjectID);
		PF_CHECK(Tests, Missing && Missing->Result == EditorDocumentWorkspaceLoadResult::Status::Missing);
		PF_CHECK(Tests, Store.HasPendingChanges());

		const EditorDocumentIdentity Level{ *ProjectID, AssetID{}, EditorDocumentKind::Level };
		EditorDocumentHost Main;
		Main.ID = std::string(EditorDocumentManager::MainHostID);
		Main.Role = EditorDocumentHostRole::Main;
		Main.PlacementKind = EditorDocumentKind::Level;
		Main.Documents = { Level };
		Main.ActiveDocument = Level;
		EditorDocumentWorkspaceState Workspace;
		Workspace.Project = *ProjectID;
		Workspace.Hosts = { Main };
		Workspace.FocusedHostID = Main.ID;
		const auto InitialSave = Store.SaveIfDue(Workspace);
		PF_CHECK(Tests, InitialSave && *InitialSave && !Store.HasPendingChanges());

		EditorDocumentWorkspaceStore Reopened;
		Reopened.Configure(Path);
		const auto Restored = Reopened.Load(*ProjectID);
		PF_CHECK(Tests, Restored && Restored->Result == EditorDocumentWorkspaceLoadResult::Status::Restored);
		PF_CHECK(Tests, Restored && Restored->Workspace && Restored->Workspace->Hosts.size() == 1);

		const auto Start = EditorDocumentWorkspaceStore::Clock::time_point{};
		Reopened.MarkDirty(true, Start);
		PF_CHECK(Tests, !Reopened.IsSaveDue(Start + std::chrono::milliseconds(499)));
		PF_CHECK(Tests, Reopened.IsSaveDue(Start + std::chrono::milliseconds(500)));
		Workspace.MaterialDetailsAsset = *MaterialID;
		const auto TooEarly = Reopened.SaveIfDue(Workspace, Start + std::chrono::milliseconds(499));
		PF_CHECK(Tests, TooEarly && !*TooEarly && Reopened.HasPendingChanges());
		const auto Due = Reopened.SaveIfDue(Workspace, Start + std::chrono::milliseconds(500));
		PF_CHECK(Tests, Due && *Due && !Reopened.HasPendingChanges());
		const auto Updated = Reopened.Load(*ProjectID);
		PF_CHECK(Tests, Updated && Updated->Workspace && Updated->Workspace->MaterialDetailsAsset == MaterialID);

		{
			std::ofstream Corrupt(Path, std::ios::binary | std::ios::trunc);
			Corrupt << "{not-json";
			PF_CHECK(Tests, static_cast<bool>(Corrupt));
		}
		EditorDocumentWorkspaceStore Recovery;
		Recovery.Configure(Path);
		const auto Rejected = Recovery.Load(*ProjectID);
		PF_CHECK(Tests, Rejected && Rejected->Result == EditorDocumentWorkspaceLoadResult::Status::Rejected);
		PF_CHECK(Tests, Rejected && !Rejected->Error.empty() && Recovery.HasPendingChanges());
		const std::filesystem::path RecoveryPath = Path.wstring() + L".recovery";
		PF_CHECK(Tests, std::filesystem::exists(RecoveryPath));
		const auto Rebuilt = Recovery.SaveIfDue(Workspace);
		PF_CHECK(Tests, Rebuilt && *Rebuilt && !Recovery.HasPendingChanges());

		const std::filesystem::path BlockedPath = Root / "blocked.json";
		std::filesystem::create_directory(BlockedPath, Error);
		PF_CHECK(Tests, !Error);
		EditorDocumentWorkspaceStore FailedWrite;
		FailedWrite.Configure(BlockedPath);
		FailedWrite.MarkDirty(false);
		const auto WriteFailure = FailedWrite.SaveNow(Workspace);
		PF_CHECK(Tests, !WriteFailure && FailedWrite.HasPendingChanges());
		PF_CHECK(Tests, std::filesystem::is_directory(BlockedPath));
	}

	void TestEditorNativeWindowBoundsPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		EditorDocumentHost::WindowBounds Saved;
		Saved.X = -1900.0f;
		Saved.Y = -80.0f;
		Saved.Width = 800.0f;
		Saved.Height = 600.0f;
		Saved.DpiScale = 1.0f;
		Saved.HasValue = true;

		const EditorNativeWindowWorkArea LeftMonitor{ -1920.0f, -40.0f, 1920.0f, 1040.0f };
		const EditorNativeWindowRect NegativeCoordinate = ResolveNativeWindowRect(Saved, LeftMonitor, 1.5f);
		PF_CHECK(Tests, NegativeCoordinate.X == -1900.0f && NegativeCoordinate.Y == -40.0f);
		PF_CHECK(Tests, NegativeCoordinate.Width == 1200.0f && NegativeCoordinate.Height == 900.0f);

		Saved.X = 4000.0f;
		Saved.Y = -1000.0f;
		const EditorNativeWindowRect Recovered = ResolveNativeWindowRect(Saved, LeftMonitor, 1.0f);
		PF_CHECK(Tests, Recovered.X == -64.0f && Recovered.Y == -40.0f);
		PF_CHECK(Tests, Recovered.Width == 800.0f && Recovered.Height == 600.0f);

		Saved.DpiScale = 0.5f;
		const EditorNativeWindowRect BoundedScale = ResolveNativeWindowRect(Saved,
			{ -2000.0f, -100.0f, 3000.0f, 2000.0f }, 4.0f);
		PF_CHECK(Tests, BoundedScale.Width == 1600.0f && BoundedScale.Height == 1200.0f);

		Saved.DpiScale = 0.0f;
		const EditorNativeWindowRect InvalidScale = ResolveNativeWindowRect(Saved, {}, 2.0f);
		PF_CHECK(Tests, InvalidScale.Width == 800.0f && InvalidScale.Height == 600.0f);
		PF_CHECK(Tests, InvalidScale.X == 4000.0f && InvalidScale.Y == -1000.0f);
	}

	void TestAssetImportOperation(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		const auto SourceIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier && SourceIdentifier);
		if (!ProjectIdentifier || !SourceIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeImportProject-" + ProjectIdentifier->ToString());
		const std::filesystem::path SourceDirectory = TemporaryDirectory / ("PulseForgeImportSource-" + SourceIdentifier->ToString());
		struct TemporaryDirectoryCleanup
		{
			std::filesystem::path Path;
			~TemporaryDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		};
		TemporaryDirectoryCleanup ProjectCleanup{ ProjectRoot };
		TemporaryDirectoryCleanup SourceCleanup{ SourceDirectory };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Imported", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		std::filesystem::create_directories(SourceDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path SourceFile = SourceDirectory / "image.bin";
		const std::string SourceContents("source bytes\0with binary data", 29);
		{
			std::ofstream Output(SourceFile, std::ios::binary | std::ios::trunc);
			Output.write(SourceContents.data(), static_cast<std::streamsize>(SourceContents.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 0);
		const auto Imported = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/image.bin");
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported)
			return;

		const std::filesystem::path ImportedPath = ProjectRoot / "Assets" / "Imported" / "image.bin";
		const std::filesystem::path ImportedSidecar = AssetMetadataSerializer::GetSidecarPath(ImportedPath);
		const auto ImportedMetadata = AssetMetadataSerializer::LoadFromFile(ImportedSidecar);
		PF_CHECK(Tests, ImportedMetadata && ImportedMetadata->ID == Imported->ID);
		PF_CHECK(Tests, ImportedMetadata && ImportedMetadata->Version == AssetMetadataSerializer::CurrentVersion);
		PF_CHECK(Tests, Imported->ProjectRelativePath == std::filesystem::path("Assets/Imported/image.bin"));
		PF_CHECK(Tests, Registry.Find(Imported->ID).has_value());
		PF_CHECK(Tests, std::filesystem::exists(SourceFile));
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input)
				return std::nullopt;
			std::string Contents{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
				return std::nullopt;
			return Contents;
		};
		PF_CHECK(Tests, ReadFile(ImportedPath) == SourceContents);
		PF_CHECK(Tests, ReadFile(SourceFile) == SourceContents);

		const auto SecondImport = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/second-copy.bin");
		PF_CHECK(Tests, SecondImport.has_value());
		PF_CHECK(Tests, SecondImport && SecondImport->ID != Imported->ID);
		PF_CHECK(Tests, SecondImport && ReadFile(ProjectRoot / SecondImport->ProjectRelativePath) == SourceContents);
		PF_CHECK(Tests, Registry.GetAssetCount() == 2);

		const auto ExistingDestination = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/image.bin");
		PF_CHECK(Tests, !ExistingDestination && ExistingDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, ReadFile(ImportedPath) == SourceContents);

		const auto InvalidTraversal = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/../escaped.bin");
		PF_CHECK(Tests, !InvalidTraversal && InvalidTraversal.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto MissingSource = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceDirectory / "missing.bin",
			"Assets/Imported/missing.bin");
		PF_CHECK(Tests, !MissingSource && MissingSource.error().Code == AssetOperationErrorCode::ImportSourceInvalid);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Imported" / "missing.bin"));

		const std::span<const char> BinaryContents(SourceContents.data(), SourceContents.size());
		const auto Created = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			std::as_bytes(BinaryContents),
			"Assets/Imported/generated.bin");
		PF_CHECK(Tests, Created.has_value());
		PF_CHECK(Tests, Created && Created->ID != Imported->ID && Created->ID != SecondImport->ID);
		const std::filesystem::path CreatedPath = ProjectRoot / "Assets" / "Imported" / "generated.bin";
		PF_CHECK(Tests, ReadFile(CreatedPath) == SourceContents);
		const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(CreatedPath));
		PF_CHECK(Tests, Created && CreatedMetadata && CreatedMetadata->ID == Created->ID);
		PF_CHECK(Tests, Registry.GetAssetCount() == 3);
		PF_CHECK(Tests, Created && Registry.Find(Created->ID).has_value());
		if (Created)
		{
			const auto DuplicateCreation = AssetOperations::CreateAssetFromBytes(
				Registry,
				ProjectRoot,
				std::as_bytes(BinaryContents),
				"Assets/Imported/generated.bin");
			PF_CHECK(Tests, !DuplicateCreation &&
				DuplicateCreation.error().Code == AssetOperationErrorCode::DestinationExists);
			PF_CHECK(Tests, !DuplicateCreation.error().CommittedAsset.has_value());
			PF_CHECK(Tests, ReadFile(CreatedPath) == SourceContents);

			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			PF_CHECK(Tests, Registry.Find(Created->ID).has_value());
		}

		const auto EmptyCreated = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			{},
			"Assets/Imported/empty.bin");
		PF_CHECK(Tests, EmptyCreated.has_value());
		PF_CHECK(Tests, EmptyCreated && std::filesystem::file_size(ProjectRoot / EmptyCreated->ProjectRelativePath) == 0);

		const std::filesystem::path UntrackedFile = ProjectRoot / "Assets" / "Imported" / "untracked.bin";
		{
			std::ofstream Output(UntrackedFile, std::ios::binary | std::ios::trunc);
			Output << "missing sidecar";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto RefusedByRegistry = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/refused.bin");
		PF_CHECK(Tests, !RefusedByRegistry && RefusedByRegistry.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Imported" / "refused.bin"));
	}

	void TestEditorWorkspaceFrameState(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Project = UUID::Generate();
		const auto OtherProject = UUID::Generate();
		const auto Asset = UUID::Generate();
		PF_CHECK(Tests, Project && OtherProject && Asset);
		if (!Project || !OtherProject || !Asset)
			return;

		const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
		const EditorDocumentIdentity Material{ *Project, *Asset, EditorDocumentKind::Material };
		const EditorDocumentIdentity OtherProjectMaterial{ *OtherProject, *Asset, EditorDocumentKind::Material };
		const EditorDocumentIdentity Texture{ *Project, *Asset, EditorDocumentKind::Texture };
		const std::string MaterialKey = EditorWorkspaceFrameState::GetWorkspaceKey(Material);
		PF_CHECK(Tests, MaterialKey.starts_with("PulseForge.DocumentWorkspace/"));
		PF_CHECK(Tests, EditorWorkspaceFrameState::GetWorkspaceKey(Material) !=
			EditorWorkspaceFrameState::GetWorkspaceKey(OtherProjectMaterial));
		PF_CHECK(Tests, EditorWorkspaceFrameState::GetWorkspaceKey(Material) !=
			EditorWorkspaceFrameState::GetWorkspaceKey(Texture));

		EditorWorkspaceFrameState Frame;
		PF_CHECK(Tests, !Frame.BeginFrame(std::nullopt) && Frame.IsLevelWorkspaceActive());
		PF_CHECK(Tests, Frame.BeginFrame(Material) && !Frame.IsLevelWorkspaceActive());
		Frame.MarkDockspaceDrawn(Material);
		Frame.MarkWorkspaceDrawn(Material, "Native/1");
		PF_CHECK(Tests, Frame.WasDockspaceDrawn(Material));
		PF_CHECK(Tests, Frame.WasWorkspaceDrawn(Material, "Native/1"));
		PF_CHECK(Tests, !Frame.WasWorkspaceDrawn(Material, "Main"));
		PF_CHECK(Tests, !Frame.BeginFrame(Material));
		PF_CHECK(Tests, !Frame.WasDockspaceDrawn(Material) && !Frame.WasWorkspaceDrawn(Material, "Native/1"));
		PF_CHECK(Tests, Frame.BeginFrame(Level) && Frame.IsLevelWorkspaceActive());
		PF_CHECK(Tests, Frame.BeginFrame(std::nullopt) && Frame.IsLevelWorkspaceActive());
	}
}

void RunDocumentsTests(TestRunner& Tests)
{
	TestEditorDocumentManager(Tests);
	TestEditorDocumentEditorRegistry(Tests);
	TestEditorSnapshotHistory(Tests);
	TestEditorDocumentWorkspaceCodec(Tests);
	TestEditorDocumentWorkspaceStore(Tests);
	TestEditorWorkspaceFrameState(Tests);
	TestEditorNativeWindowBoundsPolicy(Tests);
	TestAssetImportOperation(Tests);
}
