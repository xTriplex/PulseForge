#include "TestEditorSupport.h"
#include "Editor/EditorDocumentWorkspaceStore.h"
#include "Editor/EditorWorkspaceFrameState.h"
#include "Editor/EditorDocumentWorkspaceRead.h"

#include <sstream>

namespace
{
	class WorkspaceReadFailureBuffer final : public std::streambuf
	{
	protected:
		std::streamsize xsgetn(char*, std::streamsize) override { throw std::ios_base::failure("injected workspace read failure"); }
	};

	void TestEditorDocumentWorkspaceCodec(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Project = UUID::Generate();
		const auto MaterialA = UUID::Generate();
		const auto MaterialB = UUID::Generate();
		PF_CHECK(Tests, Project && MaterialA && MaterialB);
		if (!Project || !MaterialA || !MaterialB) return;

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
		State.RememberedPlacements.emplace_back(
			EditorDocumentKind::Material, EditorDocumentPlacement{ EditorDocumentHostRole::Native, Native.Bounds, std::nullopt });
		State.RememberedPlacements.emplace_back(
			EditorDocumentKind::Texture, EditorDocumentPlacement{ EditorDocumentHostRole::Main, Native.Bounds, std::nullopt });
		State.FocusedHostID = Native.ID;
		const auto Serialized = EditorDocumentWorkspaceCodec::Serialize(State);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;

		const auto Restored = EditorDocumentWorkspaceCodec::Deserialize(*Serialized, *Project);
		PF_CHECK(Tests, Restored.has_value());
		if (Restored)
		{
			PF_CHECK(Tests, Restored->Hosts.size() == 2 && Restored->Hosts[1].Documents.size() == 2);
			PF_CHECK(Tests, Restored->MaterialDetailsAsset == MaterialA);
			PF_CHECK(Tests, Restored->Hosts[1].ActiveDocument == Blue);
			PF_CHECK(Tests, Restored->Hosts[1].Bounds.X == -1920.0f && Restored->Hosts[1].Bounds.Maximized);
			PF_CHECK(Tests, Restored->FocusedHostID == Native.ID && Restored->RememberedPlacements.size() == 2);
			const auto NativePlacement = std::find_if(Restored->RememberedPlacements.begin(), Restored->RememberedPlacements.end(),
				[](const auto& Entry) { return Entry.first == EditorDocumentKind::Material; });
			const auto MainPlacement = std::find_if(Restored->RememberedPlacements.begin(), Restored->RememberedPlacements.end(),
				[](const auto& Entry) { return Entry.first == EditorDocumentKind::Texture; });
			PF_CHECK(Tests, NativePlacement != Restored->RememberedPlacements.end() &&
								NativePlacement->second.Role == EditorDocumentHostRole::Native &&
								NativePlacement->second.Bounds.MonitorName == "Display-Left" && !NativePlacement->second.LiveHostID);
			PF_CHECK(Tests, MainPlacement != Restored->RememberedPlacements.end() &&
								MainPlacement->second.Role == EditorDocumentHostRole::Main &&
								MainPlacement->second.Bounds.MonitorName == "Display-Left" && MainPlacement->second.Bounds.Maximized);
			const auto Candidates = GetMaterialDetailsMigrationCandidates(*Restored);
			PF_CHECK(Tests, Candidates.size() == 2 && Candidates[0] == *MaterialB && Candidates[1] == *MaterialA);
		}
		auto LegacyWorkspace = nlohmann::json::parse(*Serialized);
		LegacyWorkspace["version"] = 2;
		LegacyWorkspace.erase("rememberedPlacements");
		LegacyWorkspace["rememberedHosts"] =
			nlohmann::json::array({ { { "kind", static_cast<uint8_t>(EditorDocumentKind::Material) }, { "host", Native.ID } },
				{ { "kind", static_cast<uint8_t>(EditorDocumentKind::Texture) }, { "host", "native:missing" } } });
		const auto MigratedLegacy = EditorDocumentWorkspaceCodec::Deserialize(LegacyWorkspace.dump(), *Project);
		PF_CHECK(Tests, MigratedLegacy.has_value());
		if (MigratedLegacy)
		{
			PF_CHECK(Tests, MigratedLegacy->Version == 3 && MigratedLegacy->MaterialDetailsAsset == MaterialA);
			PF_CHECK(Tests, MigratedLegacy->Hosts[1].Documents.size() == 2);
			PF_CHECK(Tests,
				MigratedLegacy->RememberedPlacements.size() == 1 && MigratedLegacy->RememberedPlacements.front().second.Bounds.Maximized);
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
			const auto MigratedMainStrip = EditorDocumentWorkspaceCodec::Deserialize(LegacyMainStrip.dump(), *Project);
			PF_CHECK(Tests, MigratedMainStrip.has_value());
			if (MigratedMainStrip)
			{
				const auto Candidates = GetMaterialDetailsMigrationCandidates(*MigratedMainStrip);
				PF_CHECK(Tests, Candidates.size() == 2 && Candidates[0] == *MaterialB && Candidates[1] == *MaterialA);
				PF_CHECK(Tests, MigratedMainStrip->Hosts.size() == 1 && MigratedMainStrip->Hosts[0].Documents[0] == Level);
			}
		}
		const auto OtherProject = UUID::Generate();
		PF_CHECK(Tests, OtherProject && !EditorDocumentWorkspaceCodec::Deserialize(*Serialized, *OtherProject));
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize("{not-json", *Project));

		auto Duplicate = nlohmann::json::parse(*Serialized);
		Duplicate["hosts"][1]["documents"].push_back(Duplicate["hosts"][1]["documents"][0]);
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize(Duplicate.dump(), *Project));
		auto InvalidBounds = nlohmann::json::parse(*Serialized);
		InvalidBounds["hosts"][1]["bounds"]["width"] = 0.0f;
		const auto RecoveredBounds = EditorDocumentWorkspaceCodec::Deserialize(InvalidBounds.dump(), *Project);
		PF_CHECK(Tests, RecoveredBounds && !RecoveredBounds->Hosts[1].Bounds.HasValue && RecoveredBounds->Hosts[1].Documents.size() == 2);
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
		if (!ProjectID || !MaterialID) return;

		std::error_code Error;
		const std::filesystem::path Root = std::filesystem::temp_directory_path(Error) /
										   std::filesystem::u8path("PulseForgeWorkspaceStore café with spaces-" + ProjectID->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return;
		std::filesystem::create_directories(Root, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
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
		const std::string CorruptContents = "{not-json";
		EditorDocumentWorkspaceStore Recovery;
		Recovery.Configure(Path);
		const auto Rejected = Recovery.Load(*ProjectID);
		PF_CHECK(Tests, Rejected && Rejected->Result == EditorDocumentWorkspaceLoadResult::Status::Rejected);
		PF_CHECK(Tests, Rejected && !Rejected->Error.empty() && Recovery.HasPendingChanges());
		std::filesystem::path RecoveryPath = Path;
		RecoveryPath += ".recovery";
		PF_CHECK(Tests, std::filesystem::exists(RecoveryPath));
		PF_CHECK(Tests, Rejected && Rejected->RecoveryPath == RecoveryPath && Rejected->RecoveryError.empty());
		{
			std::ifstream RecoveryInput(RecoveryPath, std::ios::binary);
			const std::string Preserved{ std::istreambuf_iterator<char>(RecoveryInput), std::istreambuf_iterator<char>() };
			PF_CHECK(Tests, Preserved == CorruptContents);
		}
		const auto Rebuilt = Recovery.SaveIfDue(Workspace);
		PF_CHECK(Tests, Rebuilt && *Rebuilt && !Recovery.HasPendingChanges());
		const auto RestoredAfterRecovery = Recovery.Load(*ProjectID);
		PF_CHECK(Tests, RestoredAfterRecovery && RestoredAfterRecovery->Result == EditorDocumentWorkspaceLoadResult::Status::Restored);

		const std::filesystem::path ReadFailurePath = Root / "read-failure.json";
		std::filesystem::create_directory(ReadFailurePath, Error);
		PF_CHECK(Tests, !Error);
		EditorDocumentWorkspaceStore FailedRead;
		FailedRead.Configure(ReadFailurePath);
		const auto ReadFailure = FailedRead.Load(*ProjectID);
		PF_CHECK(Tests, !ReadFailure);
		std::filesystem::remove_all(ReadFailurePath, Error);
		PF_CHECK(Tests, !Error);
		const auto SerializedWorkspace = EditorDocumentWorkspaceCodec::Serialize(Workspace);
		PF_CHECK(Tests, SerializedWorkspace.has_value());
		if (!SerializedWorkspace) return;
		{
			std::ofstream Previous(ReadFailurePath, std::ios::binary | std::ios::trunc);
			Previous.write(SerializedWorkspace->data(), static_cast<std::streamsize>(SerializedWorkspace->size()));
			PF_CHECK(Tests, static_cast<bool>(Previous));
		}
		const auto BlockedReadSave = FailedRead.SaveNow(Workspace);
		PF_CHECK(Tests, !BlockedReadSave && FailedRead.HasPendingChanges());
		{
			std::ifstream Previous(ReadFailurePath, std::ios::binary);
			const std::string PreviousContents{ std::istreambuf_iterator<char>(Previous), std::istreambuf_iterator<char>() };
			PF_CHECK(Tests, PreviousContents == *SerializedWorkspace);
		}
		const auto RetriedRead = FailedRead.Load(*ProjectID);
		PF_CHECK(Tests, RetriedRead && RetriedRead->Result == EditorDocumentWorkspaceLoadResult::Status::Restored);

		const std::filesystem::path UnpreservedPath = Root / "unpreserved.json";
		std::filesystem::path UnusableRecoveryPath = UnpreservedPath;
		UnusableRecoveryPath += ".recovery";
		{
			std::ofstream Corrupt(UnpreservedPath, std::ios::binary | std::ios::trunc);
			Corrupt << CorruptContents;
			PF_CHECK(Tests, static_cast<bool>(Corrupt));
		}
		std::filesystem::create_directory(UnusableRecoveryPath, Error);
		PF_CHECK(Tests, !Error);
		EditorDocumentWorkspaceStore BlockedRecovery;
		BlockedRecovery.Configure(UnpreservedPath);
		const auto Unpreserved = BlockedRecovery.Load(*ProjectID);
		PF_CHECK(Tests, Unpreserved && Unpreserved->Result == EditorDocumentWorkspaceLoadResult::Status::Rejected);
		PF_CHECK(Tests, Unpreserved && !Unpreserved->RecoveryError.empty());
		PF_CHECK(Tests, !BlockedRecovery.IsSaveDue());
		const auto UnsafeReplacement = BlockedRecovery.SaveNow(Workspace);
		PF_CHECK(Tests, !UnsafeReplacement && BlockedRecovery.HasPendingChanges());
		{
			std::ifstream Original(UnpreservedPath, std::ios::binary);
			const std::string Preserved{ std::istreambuf_iterator<char>(Original), std::istreambuf_iterator<char>() };
			PF_CHECK(Tests, Preserved == CorruptContents);
		}
		std::filesystem::remove_all(UnusableRecoveryPath, Error);
		PF_CHECK(Tests, !Error);
		const auto RetryRecovery = BlockedRecovery.Load(*ProjectID);
		PF_CHECK(Tests, RetryRecovery && RetryRecovery->RecoveryError.empty());
		const auto RetryRebuild = BlockedRecovery.SaveIfDue(Workspace);
		PF_CHECK(Tests, RetryRebuild && *RetryRebuild);

		const std::filesystem::path BlockedPath = Root / "blocked.json";
		std::filesystem::path BlockedTemporaryPath = BlockedPath;
		BlockedTemporaryPath += ".tmp";
		{
			std::ofstream Previous(BlockedPath, std::ios::binary | std::ios::trunc);
			Previous << "previous destination";
			PF_CHECK(Tests, static_cast<bool>(Previous));
		}
		std::filesystem::create_directory(BlockedTemporaryPath, Error);
		PF_CHECK(Tests, !Error);
		EditorDocumentWorkspaceStore FailedWrite;
		FailedWrite.Configure(BlockedPath);
		const auto WriteFailure = FailedWrite.SaveNow(Workspace);
		PF_CHECK(Tests, !WriteFailure && FailedWrite.HasPendingChanges());
		{
			std::ifstream Previous(BlockedPath, std::ios::binary);
			const std::string PreviousContents{ std::istreambuf_iterator<char>(Previous), std::istreambuf_iterator<char>() };
			PF_CHECK(Tests, PreviousContents == "previous destination");
		}
		PF_CHECK(Tests, std::filesystem::is_directory(BlockedTemporaryPath));
		std::filesystem::remove_all(BlockedTemporaryPath, Error);
		PF_CHECK(Tests, !Error);
		const auto RetriedWrite = FailedWrite.SaveNow(Workspace);
		PF_CHECK(Tests, RetriedWrite && *RetriedWrite && !FailedWrite.HasPendingChanges());
		PF_CHECK(Tests, !std::filesystem::exists(BlockedTemporaryPath));
	}

	void TestEditorDocumentWorkspaceCompleteRead(TestRunner& Tests)
	{
		using namespace PulseForgeEditor::Detail;
		std::string Complete("complete\0bytes", 14);
		std::istringstream CompleteInput(Complete);
		std::string CompleteBuffer(Complete.size(), '\0');
		PF_CHECK(Tests, ReadCompleteWorkspaceBytes(CompleteInput, CompleteBuffer).has_value());
		PF_CHECK(Tests, CompleteBuffer == Complete);

		std::istringstream ShortInput("short");
		std::string ShortBuffer(8, '\x7f');
		const auto ShortRead = ReadCompleteWorkspaceBytes(ShortInput, ShortBuffer);
		PF_CHECK(Tests, !ShortRead);
		PF_CHECK(Tests, !ShortRead && ShortRead.error().find("expected 8 bytes but received 5") != std::string::npos);

		WorkspaceReadFailureBuffer FailureBuffer;
		std::istream FailedInput(&FailureBuffer);
		std::string FailedBuffer(4, '\0');
		const auto IoFailure = ReadCompleteWorkspaceBytes(FailedInput, FailedBuffer);
		PF_CHECK(Tests, !IoFailure);
		PF_CHECK(Tests, !IoFailure && IoFailure.error().find("I/O error") != std::string::npos);
	}

	void TestEditorWorkspaceFrameState(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Project = UUID::Generate();
		const auto OtherProject = UUID::Generate();
		const auto Asset = UUID::Generate();
		PF_CHECK(Tests, Project && OtherProject && Asset);
		if (!Project || !OtherProject || !Asset) return;

		const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
		const EditorDocumentIdentity Material{ *Project, *Asset, EditorDocumentKind::Material };
		const EditorDocumentIdentity OtherProjectMaterial{ *OtherProject, *Asset, EditorDocumentKind::Material };
		const EditorDocumentIdentity Texture{ *Project, *Asset, EditorDocumentKind::Texture };
		const std::string MaterialKey = EditorWorkspaceFrameState::GetWorkspaceKey(Material);
		PF_CHECK(Tests, MaterialKey.starts_with("PulseForge.DocumentWorkspace/"));
		PF_CHECK(Tests,
			EditorWorkspaceFrameState::GetWorkspaceKey(Material) != EditorWorkspaceFrameState::GetWorkspaceKey(OtherProjectMaterial));
		PF_CHECK(Tests, EditorWorkspaceFrameState::GetWorkspaceKey(Material) != EditorWorkspaceFrameState::GetWorkspaceKey(Texture));

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
		const EditorNativeWindowRect BoundedScale = ResolveNativeWindowRect(Saved, { -2000.0f, -100.0f, 3000.0f, 2000.0f }, 4.0f);
		PF_CHECK(Tests, BoundedScale.Width == 1600.0f && BoundedScale.Height == 1200.0f);

		Saved.DpiScale = 0.0f;
		const EditorNativeWindowRect InvalidScale = ResolveNativeWindowRect(Saved, {}, 2.0f);
		PF_CHECK(Tests, InvalidScale.Width == 800.0f && InvalidScale.Height == 600.0f);
		PF_CHECK(Tests, InvalidScale.X == 4000.0f && InvalidScale.Y == -1000.0f);
	}
} // namespace

void RunEditorDocumentWorkspaceTests(TestRunner& Tests)
{
	TestEditorDocumentWorkspaceCodec(Tests);
	TestEditorDocumentWorkspaceStore(Tests);
	TestEditorDocumentWorkspaceCompleteRead(Tests);
	TestEditorWorkspaceFrameState(Tests);
	TestEditorNativeWindowBoundsPolicy(Tests);
}
