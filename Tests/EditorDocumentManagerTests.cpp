#include "TestEditorSupport.h"

#include <new>

namespace
{
	struct ThrowingCopySnapshot final
	{
		int Value = 0;
		inline static bool FailCopy = false;

		ThrowingCopySnapshot() = default;
		explicit ThrowingCopySnapshot(int ValueIn) : Value(ValueIn) {}
		ThrowingCopySnapshot(const ThrowingCopySnapshot& Other)
		{
			if (FailCopy) throw std::bad_alloc();
			Value = Other.Value;
		}
		ThrowingCopySnapshot(ThrowingCopySnapshot&&) noexcept = default;
		ThrowingCopySnapshot& operator=(const ThrowingCopySnapshot&) = default;
		ThrowingCopySnapshot& operator=(ThrowingCopySnapshot&&) noexcept = default;
	};

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
		if (!Project || !OtherProject || !Material || !OtherMaterial || !Scene) return;

		EditorDocumentManager Manager;
		PF_CHECK(Tests, ShouldProcessNativeDocumentHostCloseRequests(false));
		PF_CHECK(Tests, !ShouldProcessNativeDocumentHostCloseRequests(true));
		const std::vector<int> BatchDocuments{ 1, 2, 3, 4 };
		const auto DirtyBatch = FilterDirtyDocumentIdentities(BatchDocuments, [](int Document) { return Document == 2 || Document == 4; });
		PF_CHECK(Tests, DirtyBatch.size() == 2 && DirtyBatch[0] == 2 && DirtyBatch[1] == 4);
		const auto CleanBatch = FilterDirtyDocumentIdentities(BatchDocuments, [](int) { return false; });
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
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f, false, true, true) ==
							DocumentTabReleaseAction::TransferToStrip);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f, true, true, true) ==
							DocumentTabReleaseAction::TransferToStrip);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f, true, true, false) ==
							DocumentTabReleaseAction::CreateNativeHost);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f, CanDetachDocumentTabToNativeHost(false, true, true),
					   true, false) == DocumentTabReleaseAction::CreateNativeHost);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
							CanDetachDocumentTabToNativeHost(false, true, false), true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
							CanDetachDocumentTabToNativeHost(true, false, false), true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f, false, true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f, true, false, false) == DocumentTabReleaseAction::None);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f, true, false, true) == DocumentTabReleaseAction::None);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(true, true, true, 100.0f, 6.0f, true, true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(false, true, false, 100.0f, 6.0f, true, true, true) == DocumentTabReleaseAction::None);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(true, false, false, 100.0f, 6.0f, true, true, true) == DocumentTabReleaseAction::None);
		PF_CHECK(
			Tests, ResolveDocumentTabReleaseAction(true, true, false, 35.0f, 6.0f, true, true, true) == DocumentTabReleaseAction::None);
		const DocumentTabPointerPosition DragStart{ 100.0f, 100.0f };
		const DocumentTabPointerPosition LastPointer{ 117.0f, 112.0f };
		const std::optional<DocumentTabPointerPosition> NoPointer;
		const std::optional<DocumentTabPointerPosition> LastValidPointer = LastPointer;
		PF_CHECK(Tests, IsUsableDocumentTabPointerPosition(LastPointer.X, LastPointer.Y));
		PF_CHECK(Tests, !IsUsableDocumentTabPointerPosition(-100001.0f, -100001.0f));
		const auto ReleasePointer = ResolveDocumentTabReleasePosition(-100001.0f, -100001.0f, LastValidPointer);
		PF_CHECK(Tests, ReleasePointer && ReleasePointer->X == LastPointer.X && ReleasePointer->Y == LastPointer.Y);
		PF_CHECK(Tests, !ResolveDocumentTabReleasePosition(-100001.0f, -100001.0f, NoPointer));
		const float LastObservedDragDistance = CalculateDocumentTabDragDistanceSquared(DragStart, NoPointer, LastValidPointer, 0.0f);
		PF_CHECK(Tests, LastObservedDragDistance == 433.0f);
		PF_CHECK(Tests, CalculateDocumentTabDragDistanceSquared(DragStart, NoPointer, LastValidPointer, 900.0f) == 900.0f);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, LastObservedDragDistance, 6.0f, true, true, false) ==
							DocumentTabReleaseAction::CreateNativeHost);
		const EditorDocumentIdentity MaterialA{ *Project, *Material, EditorDocumentKind::Material };
		const EditorDocumentIdentity MaterialB{ *Project, *OtherMaterial, EditorDocumentKind::Material };
		const EditorDocumentIdentity SameNameOtherFolder{ *Project, *Scene, EditorDocumentKind::Material };
		const EditorDocumentIdentity OtherProjectMaterial{ *OtherProject, *Material, EditorDocumentKind::Material };
		const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
		const auto First =
			Manager.Open(MaterialA, "Assets/Materials/Amber.material", "Amber.material", EditorDocumentOpenPreference::NewNativeWindow);
		const auto Second =
			Manager.Open(MaterialB, "Assets/Materials/Blue.material", "Blue.material", EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, First && Second && First->Created && Second->Created);
		PF_CHECK(Tests, First && Second && First->HostID != Second->HostID);
		PF_CHECK(Tests, Manager.GetHosts().size() == 3);

		const auto Reopened =
			Manager.Open(MaterialA, "Assets/OtherFolder/Amber.material", "Amber.material", EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, Reopened && !Reopened->Created);
		PF_CHECK(Tests, Reopened && Reopened->HostID == First->HostID);
		PF_CHECK(Tests, Manager.GetSessions().size() == 2);
		PF_CHECK(Tests, Manager.Find(MaterialA)->ProjectRelativePath == std::filesystem::path("Assets/OtherFolder/Amber.material"));
		PF_CHECK(Tests, Manager.GetFocusedHost() && Manager.GetFocusedHost()->ID == First->HostID);
		const auto* FocusedMaterialHost = Manager.GetFocusedHost();
		const auto* FocusedMaterialIdentity =
			FocusedMaterialHost && FocusedMaterialHost->ActiveDocument ? &*FocusedMaterialHost->ActiveDocument : nullptr;
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(FocusedMaterialIdentity, *Project) == EditorDocumentCommandTarget::Material);
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(nullptr, *Project) == EditorDocumentCommandTarget::Level);
		const EditorDocumentIdentity TextureCommandDocument{ *Project, *Material, EditorDocumentKind::Texture };
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(&TextureCommandDocument, *Project) == EditorDocumentCommandTarget::Unsupported);
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(&OtherProjectMaterial, *Project) == EditorDocumentCommandTarget::Unsupported);
		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t LocationUpdateRevision = Manager.Find(MaterialA)->Revision;
		const bool LocationUpdated =
			Manager.UpdateAssetLocation(MaterialA, "Assets/Renamed/Amber-Renamed.material", "Amber-Renamed.material");
		PF_CHECK(Tests, LocationUpdated);
		PF_CHECK(Tests, Manager.Find(MaterialA)->ProjectRelativePath == std::filesystem::path("Assets/Renamed/Amber-Renamed.material"));
		PF_CHECK(Tests, Manager.Find(MaterialA)->DisplayName == "Amber-Renamed.material");
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == LocationUpdateRevision && Manager.Find(MaterialA)->IsDirty());

		const auto OtherFolderOpen = Manager.Open(
			SameNameOtherFolder, "Assets/Sub/Amber.material", "Amber.material", EditorDocumentOpenPreference::MainDocumentStrip);
		const auto OtherProjectOpen = Manager.Open(
			OtherProjectMaterial, "Assets/Materials/Amber.material", "Amber.material", EditorDocumentOpenPreference::NewNativeWindow);
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
			return Manager.Open({ *Project, PreferenceAsset, EditorDocumentKind::Texture }, "Assets/Textures/Albedo.png", "Albedo.png",
				EditorDocumentOpenPreference::RememberedPlacement);
		};
		const auto RememberedOne = ManagerPreferenceOpen();
		const auto RememberedTwo = ManagerPreferenceOpen();
		PF_CHECK(Tests, RememberedOne && RememberedTwo);
		PF_CHECK(Tests, RememberedOne && RememberedTwo && RememberedOne->HostID == RememberedTwo->HostID);
		const EditorDocumentIdentity OtherProjectTexture{ *OtherProject, *Scene, EditorDocumentKind::Texture };
		const auto OtherProjectTextureOpen =
			Manager.Open(OtherProjectTexture, "Assets/Textures/Normal.png", "Normal.png", EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, OtherProjectTextureOpen && OtherProjectTextureOpen->HostID != EditorDocumentManager::MainHostID);
		PF_CHECK(Tests, !Manager.Transfer(MaterialA, OtherProjectTextureOpen->HostID, 0));

		const auto RecoveryTextureAID = UUID::Generate();
		const auto RecoveryTextureBID = UUID::Generate();
		PF_CHECK(Tests, RecoveryTextureAID && RecoveryTextureBID);
		if (RecoveryTextureAID && RecoveryTextureBID)
		{
			const EditorDocumentIdentity RecoveryTextureA{ *Project, *RecoveryTextureAID, EditorDocumentKind::Texture };
			const EditorDocumentIdentity RecoveryTextureB{ *Project, *RecoveryTextureBID, EditorDocumentKind::Texture };
			const auto RecoveryAOpen =
				Manager.Open(RecoveryTextureA, "Assets/Textures/A.png", "A.png", EditorDocumentOpenPreference::NewNativeWindow);
			PF_CHECK(Tests, RecoveryAOpen.has_value());
			if (RecoveryAOpen)
			{
				PF_CHECK(Tests, Manager.SetRememberedPlacement(
									*Project, EditorDocumentKind::Texture, EditorDocumentHostRole::Native, {}, RecoveryAOpen->HostID));
				const auto RecoveryBOpen =
					Manager.Open(RecoveryTextureB, "Assets/Textures/B.png", "B.png", EditorDocumentOpenPreference::RememberedPlacement);
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
					PF_CHECK(Tests, Manager.Find(RecoveryTextureA)->Revision == RevisionA && Manager.Find(RecoveryTextureA)->IsDirty());
					PF_CHECK(Tests, Manager.Find(RecoveryTextureB)->Revision == RevisionB && Manager.Find(RecoveryTextureB)->IsDirty());
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
		if (!DetachAsset) return;
		const EditorDocumentIdentity DetachMaterial{ *Project, *DetachAsset, EditorDocumentKind::Material };
		const auto DetachOpen = Manager.Open(
			DetachMaterial, "Assets/Materials/Detach.material", "Detach.material", EditorDocumentOpenPreference::MainDocumentStrip);
		const auto NewMaterialHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
		PF_CHECK(Tests, DetachOpen && DetachOpen->HostID == EditorDocumentManager::MainHostID);
		PF_CHECK(Tests, NewMaterialHost.has_value());
		if (DetachOpen && NewMaterialHost)
		{
			PF_CHECK(Tests, Manager.MarkEdited(DetachMaterial));
			const uint64_t DetachRevision = Manager.Find(DetachMaterial)->Revision;
			const auto RejectedDetach = Manager.Transfer(
				DetachMaterial, *NewMaterialHost, 0, [](const EditorDocumentSession&, const EditorDocumentHost&) { return false; });
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
					{ return Host.Role == EditorDocumentHostRole::Native && Host.PlacementKind == Session.Identity.Kind; });
				PF_CHECK(Tests, Detached && Detached->Changed);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == *PreparedHost);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision && Manager.Find(DetachMaterial)->IsDirty());
				const auto Reattached = Manager.Transfer(DetachMaterial, EditorDocumentManager::MainHostID, 0);
				PF_CHECK(Tests, Reattached && Reattached->Changed);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == EditorDocumentManager::MainHostID);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision && Manager.Find(DetachMaterial)->IsDirty());

				const auto Redetached = Manager.Transfer(DetachMaterial, *PreparedHost, 0);
				PF_CHECK(Tests, Redetached && Redetached->Changed);
				const auto TearOffHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
				PF_CHECK(Tests, TearOffHost.has_value());
				if (TearOffHost)
				{
					const auto TearOff = Manager.Transfer(DetachMaterial, *TearOffHost, 0);
					PF_CHECK(Tests, TearOff && TearOff->EmptiedSourceHost == *PreparedHost);
					PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == *TearOffHost);
					PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision && Manager.Find(DetachMaterial)->IsDirty());
					PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(*PreparedHost));
				}
			}
		}

		const auto LevelOpen = Manager.Open(Level, {}, "Level Editor", EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, LevelOpen && LevelOpen->HostID == EditorDocumentManager::MainHostID);
		const auto* FocusedLevelHost = Manager.GetFocusedHost();
		const auto* FocusedLevelIdentity =
			FocusedLevelHost && FocusedLevelHost->ActiveDocument ? &*FocusedLevelHost->ActiveDocument : nullptr;
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(FocusedLevelIdentity, *Project) == EditorDocumentCommandTarget::Level);
		const auto ProtectedTransfer = Manager.Transfer(Level, First->HostID, 0);
		PF_CHECK(Tests, !ProtectedTransfer);

		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t DirtyRevision = Manager.Find(MaterialA)->Revision;
		const auto RejectedTransfer =
			Manager.Transfer(MaterialA, Second->HostID, 0, [](const EditorDocumentSession&, const EditorDocumentHost&) { return false; });
		PF_CHECK(Tests, !RejectedTransfer);
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == DirtyRevision);

		const auto Moved =
			Manager.Transfer(MaterialA, Second->HostID, 0, [](const EditorDocumentSession&, const EditorDocumentHost&) { return true; });
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
		const auto SaveFailed =
			Manager.Close(MaterialA, EditorDocumentCloseChoice::Save, [](const EditorDocumentSession&, uint64_t) { return false; }, {});
		PF_CHECK(Tests, SaveFailed == EditorDocumentCloseStatus::SaveFailed);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->IsDirty());
		const auto SavedOlderRevision = Manager.Close(MaterialA, EditorDocumentCloseChoice::Save,
			[&Tests, &Manager, &MaterialA](const EditorDocumentSession&, uint64_t Revision)
			{
				PF_CHECK(Tests, Revision == Manager.Find(MaterialA)->Revision);
				PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
				return true;
			},
			{});
		PF_CHECK(Tests, SavedOlderRevision == EditorDocumentCloseStatus::StillDirty);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->IsDirty());
		const auto Saved =
			Manager.Close(MaterialA, EditorDocumentCloseChoice::Save, [](const EditorDocumentSession&, uint64_t) { return true; }, {});
		PF_CHECK(Tests, Saved == EditorDocumentCloseStatus::Closed);
		PF_CHECK(Tests, !Manager.Find(MaterialA));

		PF_CHECK(Tests, Manager.MarkEdited(MaterialB));
		const auto Discarded =
			Manager.Close(MaterialB, EditorDocumentCloseChoice::Discard, {}, [](const EditorDocumentSession&) { return true; });
		PF_CHECK(Tests, Discarded == EditorDocumentCloseStatus::Closed);
		PF_CHECK(Tests, Manager.Close(Level, EditorDocumentCloseChoice::Discard, {}, {}) == EditorDocumentCloseStatus::Protected);

		const auto RemovedProjectHosts = Manager.ClearProject(*OtherProject);
		PF_CHECK(Tests, RemovedProjectHosts.size() == 2);
		PF_CHECK(Tests,
			OtherProjectTextureOpen && OtherProjectOpen &&
				std::find(RemovedProjectHosts.begin(), RemovedProjectHosts.end(), OtherProjectTextureOpen->HostID) !=
					RemovedProjectHosts.end() &&
				std::find(RemovedProjectHosts.begin(), RemovedProjectHosts.end(), OtherProjectOpen->HostID) != RemovedProjectHosts.end());
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
			const auto MainOpen = PlacementManager.Open(
				PlacementA, "Assets/Materials/A.material", "A.material", EditorDocumentOpenPreference::MainDocumentStrip);
			PF_CHECK(Tests, MainOpen && MainOpen->HostID == EditorDocumentManager::MainHostID);
			const auto NativeHost = PlacementManager.CreateNativeHost(EditorDocumentKind::Material);
			PF_CHECK(Tests, NativeHost.has_value());
			EditorDocumentHost::WindowBounds SavedBounds{ -1700.0f, 120.0f, 880.0f, 720.0f, 1.5f, "Left Display", true, true };
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
				const auto LiveReuse = PlacementManager.Open(
					PlacementB, "Assets/Materials/B.material", "B.material", EditorDocumentOpenPreference::RememberedPlacement);
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
				const auto RememberedOpen = PlacementManager.Open(
					PlacementB, "Assets/Materials/B.material", "B.material", EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, RememberedOpen && RememberedOpen->HostID != *NativeHost);
				const auto* RestoredNative = RememberedOpen ? PlacementManager.FindHost(RememberedOpen->HostID) : nullptr;
				PF_CHECK(Tests, RestoredNative && RestoredNative->Bounds.X == SavedBounds.X &&
									RestoredNative->Bounds.MonitorName == SavedBounds.MonitorName && RestoredNative->Bounds.Maximized);
				PF_CHECK(Tests, PlacementManager.Transfer(PlacementB, EditorDocumentManager::MainHostID, 0));
				const auto MainRememberedOpen = PlacementManager.Open(
					PlacementC, "Assets/Materials/C.material", "C.material", EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, MainRememberedOpen && MainRememberedOpen->HostID == EditorDocumentManager::MainHostID);
				const auto MainPlacement = PlacementManager.GetRememberedPlacements(*PlacementProject);
				PF_CHECK(Tests, MainPlacement.size() == 1 && MainPlacement.front().second.Role == EditorDocumentHostRole::Main &&
									MainPlacement.front().second.Bounds.MonitorName == "Left Display" &&
									MainPlacement.front().second.Bounds.Maximized);

				const auto IsolatedPlacementProject = UUID::Generate();
				const auto IsolatedPlacementAsset = UUID::Generate();
				PF_CHECK(Tests, IsolatedPlacementProject && IsolatedPlacementAsset);
				if (IsolatedPlacementProject && IsolatedPlacementAsset)
				{
					PF_CHECK(Tests, PlacementManager.SetRememberedPlacement(*IsolatedPlacementProject, EditorDocumentKind::Material,
										EditorDocumentHostRole::Native, SavedBounds));
					const EditorDocumentIdentity IsolatedIdentity{ *IsolatedPlacementProject, *IsolatedPlacementAsset,
						EditorDocumentKind::Material };
					const auto IsolatedOpen = PlacementManager.Open(IsolatedIdentity, "Assets/Materials/Isolated.material",
						"Isolated.material", EditorDocumentOpenPreference::RememberedPlacement);
					const auto* IsolatedHost = IsolatedOpen ? PlacementManager.FindHost(IsolatedOpen->HostID) : nullptr;
					PF_CHECK(Tests, IsolatedOpen && IsolatedOpen->HostID != EditorDocumentManager::MainHostID && IsolatedHost &&
										IsolatedHost->Bounds.MonitorName == "Left Display");
					PF_CHECK(Tests,
						PlacementManager.GetRememberedPlacements(*PlacementProject).front().second.Role == EditorDocumentHostRole::Main);
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
				const EditorDocumentIdentity ProjectTexture{ *CrossProjectA, *CrossProjectAssetA, EditorDocumentKind::Texture };
				const EditorDocumentIdentity OtherProjectTexture{ *CrossProjectB, *CrossProjectAssetB, EditorDocumentKind::Texture };
				const auto OriginalOpen = CrossProjectManager.Open(
					ProjectTexture, "Assets/Textures/A.png", "A.png", EditorDocumentOpenPreference::NewNativeWindow);
				PF_CHECK(Tests, OriginalOpen.has_value());
				if (OriginalOpen)
				{
					PF_CHECK(Tests, CrossProjectManager.Close(ProjectTexture, EditorDocumentCloseChoice::Discard, {}, {}) ==
										EditorDocumentCloseStatus::Closed);
					const auto OtherOpen = CrossProjectManager.Open(
						OtherProjectTexture, "Assets/Textures/B.png", "B.png", EditorDocumentOpenPreference::RememberedPlacement);
					PF_CHECK(Tests, OtherOpen && OtherOpen->HostID != OriginalOpen->HostID);
					if (OtherOpen)
					{
						const auto DestinationBefore = CrossProjectManager.FindHost(OriginalOpen->HostID)->Documents;
						const auto SourceBefore = CrossProjectManager.FindHost(OtherOpen->HostID)->Documents;
						const auto FocusedBefore = CrossProjectManager.GetFocusedHost()->ID;
						const auto Rejected = CrossProjectManager.Transfer(OtherProjectTexture, OriginalOpen->HostID, 0);
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
				PF_CHECK(Tests, InvalidPlacementManager.SetRememberedPlacement(
									*InvalidProject, EditorDocumentKind::Texture, EditorDocumentHostRole::Native, InvalidBounds));
				const EditorDocumentIdentity InvalidPlacementIdentity{ *InvalidProject, *InvalidPlacementAsset,
					EditorDocumentKind::Texture };
				const auto Fallback = InvalidPlacementManager.Open(
					InvalidPlacementIdentity, "Assets/Textures/T.png", "T.png", EditorDocumentOpenPreference::RememberedPlacement);
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
					PF_CHECK(Tests, !DuplicateRejected && InvalidBatchManager.FindHost(*BatchHost)->Documents.size() == Before.size() + 1 &&
										InvalidBatchManager.Find(BatchA)->HostID == *BatchHost &&
										InvalidBatchManager.Find(BatchB)->HostID == *BatchHost);
					const auto MainRecoveryRejected = InvalidBatchManager.RecoverAllToMain(EditorDocumentManager::MainHostID);
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

		EditorSnapshotHistory<ThrowingCopySnapshot> ThrowingHistory;
		ThrowingHistory.Reset(ThrowingCopySnapshot{ 0 });
		ThrowingHistory.Record(ThrowingCopySnapshot{ 1 });
		ThrowingHistory.MarkSaved();
		ThrowingHistory.Record(ThrowingCopySnapshot{ 2 });
		PF_CHECK(Tests, ThrowingHistory.Undo() && ThrowingHistory.GetCurrent().Value == 1 && !ThrowingHistory.IsDirty());
		ThrowingCopySnapshot::FailCopy = true;
		bool SnapshotCopyFailed = false;
		try
		{
			ThrowingHistory.Record(ThrowingCopySnapshot{ 3 });
		}
		catch (const std::bad_alloc&)
		{
			SnapshotCopyFailed = true;
		}
		ThrowingCopySnapshot::FailCopy = false;
		PF_CHECK(Tests, SnapshotCopyFailed);
		PF_CHECK(Tests, ThrowingHistory.GetStateCount() == 3 && ThrowingHistory.GetCursor() == 1 &&
							ThrowingHistory.GetCurrent().Value == 1 && ThrowingHistory.CanRedo() && !ThrowingHistory.IsDirty());
		PF_CHECK(Tests, ThrowingHistory.Redo() && ThrowingHistory.GetCurrent().Value == 2 && ThrowingHistory.IsDirty());
	}
} // namespace

void RunEditorDocumentManagerTests(TestRunner& Tests)
{
	TestEditorDocumentManager(Tests);
}

void RunEditorSnapshotHistoryTests(TestRunner& Tests)
{
	TestEditorSnapshotHistory(Tests);
}
