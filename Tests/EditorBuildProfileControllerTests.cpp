#include "TestEditorSupport.h"

#include "Assets/AssetExportService.h"
#include "Assets/AssetMetadata.h"
#include "Assets/ExportBuildProfile.h"
#include "Assets/Project.h"
#include "Editor/EditorBuildProfileController.h"
#include "Scene/SceneSerializer.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	class TemporaryControllerFixture final
	{
	public:
		TemporaryControllerFixture()
		{
			const auto Identifier = UUID::Generate();
			if (!Identifier) return;
			m_Root = std::filesystem::temp_directory_path() / ("PulseForge Build Profile Controller " + Identifier->ToString());
			std::error_code Error;
			std::filesystem::create_directories(m_Root, Error);
			if (Error) return;
			m_Root = std::filesystem::canonical(m_Root, Error);
			if (Error) return;
			m_ProjectFile = m_Root / "Source" / "ControllerFixture.pfproj";
			m_TemplateRoot = m_Root / "Template";
			m_TemplateDescriptor = m_TemplateRoot / "PulseForgePlayerTemplate.json";
			m_Ready = PrepareProject() && PrepareTemplate();
		}

		~TemporaryControllerFixture()
		{
			if (m_Root.empty()) return;
			std::error_code Error;
			std::filesystem::remove_all(m_Root, Error);
		}

		[[nodiscard]] bool IsReady() const noexcept { return m_Ready; }
		[[nodiscard]] const std::filesystem::path& Root() const noexcept { return m_Root; }
		[[nodiscard]] const std::filesystem::path& ProjectFile() const noexcept { return m_ProjectFile; }
		[[nodiscard]] const std::filesystem::path& TemplateRoot() const noexcept { return m_TemplateRoot; }
		[[nodiscard]] const std::filesystem::path& TemplateDescriptor() const noexcept { return m_TemplateDescriptor; }
		[[nodiscard]] const AssetID& StartupScene() const noexcept { return m_StartupScene; }

		[[nodiscard]] EditorBuildProfileProjectContext ProjectContext() const
		{
			EditorBuildProfileProjectContext Context;
			Context.SavedProjectFile = m_ProjectFile;
			Context.ProjectStartupScene = m_StartupScene;
			return Context;
		}

	private:
		bool PrepareProject()
		{
			auto Created = Project::Create(m_ProjectFile, "Controller fixture");
			if (!Created) return false;
			std::error_code Error;
			std::filesystem::create_directories(m_ProjectFile.parent_path() / "Assets", Error);
			if (Error) return false;

			Scene SceneData;
			auto Serialized = SceneSerializer::Serialize(SceneData);
			if (!Serialized || !WriteTextFile(m_ProjectFile.parent_path() / "Assets" / "Startup.scene", *Serialized)) return false;
			auto Metadata = AssetMetadataSerializer::CreateForNewAsset(m_ProjectFile.parent_path() / "Assets" / "Startup.scene");
			if (!Metadata) return false;
			m_StartupScene = Metadata->ID;

			auto Opened = Project::Open(m_ProjectFile);
			if (!Opened || !Opened->SetStartScene(m_StartupScene)) return false;
			return true;
		}

		bool PrepareTemplate()
		{
			const ExportTemplateDescriptor Descriptor = MakeHostDescriptor();
			for (const std::string& Relative : Descriptor.RuntimeFiles)
			{
				const auto Destination = m_TemplateRoot / std::filesystem::u8path(Relative);
				if (Relative == Descriptor.Entrypoint)
				{
					if (!WriteValidExecutable(Destination)) return false;
				}
				else if (!WriteTextFile(Destination, "template file: " + Relative)) return false;
			}
			auto Serialized = SerializeExportTemplateDescriptor(Descriptor);
			return Serialized && WriteTextFile(m_TemplateDescriptor, *Serialized);
		}

		static ExportTemplateDescriptor MakeHostDescriptor()
		{
			ExportTemplateDescriptor Descriptor;
#if defined(_WIN32)
			Descriptor.TargetPlatform = ExportTargetPlatform::Windows;
			Descriptor.TargetArchitecture = ExportTargetArchitecture::X64;
			Descriptor.Entrypoint = "PulseForgeCookedGame.exe";
			Descriptor.RuntimeFiles = { Descriptor.Entrypoint, "Shaders/Tonemapping.ps.spv" };
#elif defined(__APPLE__)
			Descriptor.TargetPlatform = ExportTargetPlatform::MacOS;
			Descriptor.TargetArchitecture = ExportTargetArchitecture::Arm64;
			Descriptor.Entrypoint = "Contents/MacOS/PulseForgeCookedGame";
			Descriptor.RuntimeFiles = { Descriptor.Entrypoint, "Contents/Resources/Shaders/Tonemapping.ps.spv" };
#else
			Descriptor.TargetPlatform = ExportTargetPlatform::Linux;
			Descriptor.TargetArchitecture = ExportTargetArchitecture::X64;
			Descriptor.Entrypoint = "PulseForgeCookedGame";
			Descriptor.RuntimeFiles = { Descriptor.Entrypoint, "Shaders/Tonemapping.ps.spv" };
#endif
			Descriptor.Configuration = CookedBuildConfiguration::Release;
			Descriptor.RendererBackend = "Vulkan";
			Descriptor.RuntimeABI = std::string(CookedRuntimeABIIdentifier);
			Descriptor.CookManifestSchema = CookedAssetCatalog::ManifestVersion;
			return Descriptor;
		}

		static bool WriteTextFile(const std::filesystem::path& Path, std::string_view Text)
		{
			std::error_code Error;
			std::filesystem::create_directories(Path.parent_path(), Error);
			if (Error) return false;
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
			Output.flush();
			return static_cast<bool>(Output);
		}

		static bool WriteValidExecutable(const std::filesystem::path& Path)
		{
			std::array<unsigned char, 70> Header{};
#if defined(_WIN32)
			Header[0] = 'M';
			Header[1] = 'Z';
			Header[0x3c] = 64;
			Header[64] = 'P';
			Header[65] = 'E';
			Header[68] = 0x64;
			Header[69] = 0x86;
#elif defined(__APPLE__)
			Header[0] = 0xcf;
			Header[1] = 0xfa;
			Header[2] = 0xed;
			Header[3] = 0xfe;
			Header[4] = 0x0c;
			Header[7] = 0x01;
#else
			Header[0] = 0x7f;
			Header[1] = 'E';
			Header[2] = 'L';
			Header[3] = 'F';
			Header[4] = 2;
			Header[5] = 1;
			Header[18] = 62;
#endif
			if (!WriteTextFile(Path, std::string_view(reinterpret_cast<const char*>(Header.data()), Header.size()))) return false;
#if !defined(_WIN32)
			std::error_code Error;
			std::filesystem::permissions(Path,
				std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec |
				std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
				std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
				std::filesystem::perm_options::replace, Error);
			if (Error) return false;
#endif
			return true;
		}

		std::filesystem::path m_Root;
		std::filesystem::path m_ProjectFile;
		std::filesystem::path m_TemplateRoot;
		std::filesystem::path m_TemplateDescriptor;
		AssetID m_StartupScene;
		bool m_Ready = false;
	};

	ExportBuildProfile MakeHostProfile(const AssetID& StartupScene)
	{
		ExportBuildProfile Profile;
		Profile.ProfileID = UUID{ 0x7300000000000000ull, 73 };
		Profile.DisplayName = "Controller export test";
#if defined(_WIN32)
		Profile.TargetPlatform = ExportTargetPlatform::Windows;
		Profile.Architecture = ExportTargetArchitecture::X64;
#elif defined(__APPLE__)
		Profile.TargetPlatform = ExportTargetPlatform::MacOS;
		Profile.Architecture = ExportTargetArchitecture::Arm64;
#else
		Profile.TargetPlatform = ExportTargetPlatform::Linux;
		Profile.Architecture = ExportTargetArchitecture::X64;
#endif
		Profile.Configuration = CookedBuildConfiguration::Release;
		Profile.Renderer = ExportRendererBackend::Vulkan;
		Profile.StartupScene = StartupScene;
		Profile.DefaultOutputPath = std::filesystem::path("Build") / "Exports" / "Controller";
		Profile.Window = ExportWindowSettings{ 960, 540 };
		return Profile;
	}

	EditorBuildProfileAction MakeAction(EditorBuildProfileActionKind Kind)
	{
		return EditorBuildProfileAction{ Kind };
	}

	class FakeExportRunner final : public EditorBuildProfileExportRunner
	{
	public:
		std::expected<uint64_t, AssetExportError> Start(AssetExportRequest Request) override
		{
			++m_StartCount;
			m_Request = std::move(Request);
			m_Snapshot.RequestID = m_RequestID;
			m_Snapshot.Phase = AssetExportPhase::Stage;
			m_Snapshot.Running = true;
			return m_RequestID;
		}

		AssetExportSnapshot GetSnapshot(uint64_t RequestID) const override
		{
			return RequestID == m_RequestID ? m_Snapshot : AssetExportSnapshot{};
		}

		bool RequestCancel(uint64_t RequestID) noexcept override
		{
			if (RequestID != m_RequestID || !m_Snapshot.Running) return false;
			++m_CancelCount;
			m_Snapshot.CancellationRequested = true;
			return true;
		}

		void Shutdown() noexcept override { ++m_ShutdownCount; }

		[[nodiscard]] int StartCount() const noexcept { return m_StartCount; }
		[[nodiscard]] int CancelCount() const noexcept { return m_CancelCount; }
		[[nodiscard]] int ShutdownCount() const noexcept { return m_ShutdownCount; }
		[[nodiscard]] const std::optional<AssetExportRequest>& Request() const noexcept { return m_Request; }

		void Finish() noexcept
		{
			m_Snapshot.Running = false;
			m_Snapshot.Phase = AssetExportPhase::Cancelled;
		}

	private:
		static constexpr uint64_t m_RequestID = 917;
		std::optional<AssetExportRequest> m_Request;
		AssetExportSnapshot m_Snapshot;
		int m_StartCount = 0;
		int m_CancelCount = 0;
		int m_ShutdownCount = 0;
	};

	void SelectTemplate(EditorBuildProfileController& Controller,
		const EditorBuildProfileProjectContext& Context, const std::filesystem::path& Path)
	{
		const auto Intent = Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::SelectTemplate), Context);
		if (Intent) Controller.ResolveDialogResult(*Intent, Path);
	}

	void TestControllerProfileCreateLoadSaveTransitions(TestRunner& Tests)
	{
		TemporaryControllerFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		EditorBuildProfileController Controller;
		auto Context = Fixture.ProjectContext();
		Context.CurrentSceneAsset = UUID{ 0x7400000000000000ull, 74 };

		PF_CHECK(Tests, !Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::NewProfile), Context));
		auto State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, State.Profile.has_value());
		if (!State.Profile) return;
		PF_CHECK(Tests, !State.Profile->ProfileID.IsNil());
		PF_CHECK(Tests, State.Profile->DisplayName == "Build Profile");
		PF_CHECK(Tests, State.Profile->StartupScene == Fixture.StartupScene());
		PF_CHECK(Tests, State.ProjectOpen && State.SavedContentReady);
		PF_CHECK(Tests, State.ProfilePath.empty());

		const auto SaveAs = Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::SaveProfile), Context);
		PF_CHECK(Tests, SaveAs.has_value());
		if (!SaveAs) return;
		PF_CHECK(Tests, SaveAs->Kind == EditorBuildProfileDialogKind::SaveProfileAs);
		PF_CHECK(Tests, SaveAs->InitialDirectory == Fixture.ProjectFile().parent_path());
		PF_CHECK(Tests, SaveAs->SuggestedFilename == "Default.pfbuildprofile");
		const auto ProfilesDirectory = Fixture.Root() / "Profiles";
		std::error_code Error;
		std::filesystem::create_directories(ProfilesDirectory, Error);
		PF_CHECK(Tests, !Error);
		const auto SaveAsPath = ProfilesDirectory / "CreatedProfile";
		Controller.ResolveDialogResult(*SaveAs, SaveAsPath);
		State = Controller.GetPanelState(Context);
		const auto CreatedProfilePath = ProfilesDirectory / "CreatedProfile.pfbuildprofile";
		PF_CHECK(Tests, State.ProfilePath == CreatedProfilePath);
		PF_CHECK(Tests, std::filesystem::exists(CreatedProfilePath));

		ExportBuildProfile ProfileToLoad = MakeHostProfile(Fixture.StartupScene());
		ProfileToLoad.ProfileID = UUID{ 0x7500000000000000ull, 75 };
		ProfileToLoad.DisplayName = "Loaded controller profile";
		const auto LoadPath = ProfilesDirectory / "LoadedProfile.pfbuildprofile";
		PF_CHECK(Tests, ExportBuildProfileSerializer::Save(LoadPath, ProfileToLoad).has_value());
		const auto LoadIntent = Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::LoadProfile), Context);
		PF_CHECK(Tests, LoadIntent.has_value());
		if (!LoadIntent) return;
		PF_CHECK(Tests, LoadIntent->Kind == EditorBuildProfileDialogKind::LoadProfile);
		Controller.ResolveDialogResult(*LoadIntent, LoadPath);
		State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, State.Profile == ProfileToLoad);
		PF_CHECK(Tests, State.ProfilePath == LoadPath);

		Controller.ResolveDialogResult(*LoadIntent, std::nullopt);
		PF_CHECK(Tests, Controller.GetPanelState(Context).Profile == ProfileToLoad);
		ExportBuildProfile EditedProfile = ProfileToLoad;
		EditedProfile.DisplayName = "Edited controller profile";
		Controller.ApplyProfileEdit(EditedProfile);
		PF_CHECK(Tests, Controller.GetPanelState(Context).StatusMessage == "Profile has unsaved edits.");
		PF_CHECK(Tests, !Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::SaveProfile), Context));
		const auto SavedProfile = ExportBuildProfileSerializer::Load(LoadPath);
		PF_CHECK(Tests, SavedProfile.has_value());
		PF_CHECK(Tests, SavedProfile && *SavedProfile == EditedProfile);

		SelectTemplate(Controller, Context, Fixture.TemplateDescriptor());
		State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, State.TemplateDescriptorPath == Fixture.TemplateDescriptor());
		Controller.ProjectChanged("project switched");
		State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, !State.Profile.has_value());
		PF_CHECK(Tests, State.ProfilePath.empty());
		PF_CHECK(Tests, State.TemplateDescriptorPath.empty());
		Controller.Shutdown("test complete");
	}

	void TestControllerStartExportValidationAndRequestConstruction(TestRunner& Tests)
	{
		TemporaryControllerFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		auto Fake = std::make_unique<FakeExportRunner>();
		FakeExportRunner* FakeView = Fake.get();
		EditorBuildProfileController Controller(std::move(Fake));
		const auto Context = Fixture.ProjectContext();
		const ExportBuildProfile Profile = MakeHostProfile(Fixture.StartupScene());
		Controller.ApplyProfileEdit(Profile);

		(void)Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::StartExport), Context);
		auto State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, State.StatusMessage == "Select a compatible local player template before exporting.");
		PF_CHECK(Tests, FakeView->StartCount() == 0);

		const auto MissingDescriptor = Fixture.TemplateRoot() / "MissingTemplate.json";
		SelectTemplate(Controller, Context, MissingDescriptor);
		(void)Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::StartExport), Context);
		State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, !State.StatusMessage.empty());
		PF_CHECK(Tests, State.Export.RequestID == 0);
		PF_CHECK(Tests, FakeView->StartCount() == 0);

		const auto InvalidDescriptor = Fixture.TemplateRoot() / "MalformedTemplate.json";
		{
			std::ofstream Output(InvalidDescriptor, std::ios::binary | std::ios::trunc);
			Output << "{\"format\":\"PulseForgePlayerTemplate\",\"version\":1";
			Output.flush();
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		SelectTemplate(Controller, Context, InvalidDescriptor);
		(void)Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::StartExport), Context);
		State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, !State.StatusMessage.empty());
		PF_CHECK(Tests, State.Export.RequestID == 0);
		PF_CHECK(Tests, FakeView->StartCount() == 0);

		SelectTemplate(Controller, Context, Fixture.TemplateDescriptor());
		auto DirtyContext = Context;
		DirtyContext.DirtyReason = "The scene has unsaved edits.";
		(void)Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::StartExport), DirtyContext);
		State = Controller.GetPanelState(DirtyContext);
		PF_CHECK(Tests, State.StatusMessage == DirtyContext.DirtyReason);
		PF_CHECK(Tests, State.Export.RequestID == 0);
		PF_CHECK(Tests, FakeView->StartCount() == 0);

		(void)Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::StartExport), Context);
		State = Controller.GetPanelState(Context);
		PF_CHECK(Tests, FakeView->StartCount() == 1);
		PF_CHECK(Tests, State.Export.Running);
		PF_CHECK(Tests, State.Export.RequestID == 917);
		PF_CHECK(Tests, FakeView->Request().has_value());
		if (FakeView->Request())
		{
			const AssetExportRequest& Request = *FakeView->Request();
			PF_CHECK(Tests, Request.SourceProjectFile == Fixture.ProjectFile());
			PF_CHECK(Tests, Request.Profile == Profile);
			PF_CHECK(Tests, Request.TemplateDescriptor == Fixture.TemplateDescriptor());
			PF_CHECK(Tests, Request.TemplateRoot == Fixture.TemplateRoot());
			PF_CHECK(Tests, !Request.OutputOverride.has_value());
		}
		Controller.Shutdown("test complete");
		PF_CHECK(Tests, FakeView->ShutdownCount() == 1);
	}

	void TestControllerCancellationAndProjectChangeReachActiveExport(TestRunner& Tests)
	{
		TemporaryControllerFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		auto Fake = std::make_unique<FakeExportRunner>();
		FakeExportRunner* FakeView = Fake.get();
		EditorBuildProfileController Controller(std::move(Fake));
		const auto Context = Fixture.ProjectContext();
		Controller.ApplyProfileEdit(MakeHostProfile(Fixture.StartupScene()));
		SelectTemplate(Controller, Context, Fixture.TemplateDescriptor());
		(void)Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::StartExport), Context);
		PF_CHECK(Tests, Controller.GetPanelState(Context).Export.Running);

		(void)Controller.HandleAction(MakeAction(EditorBuildProfileActionKind::CancelExport), Context);
		PF_CHECK(Tests, FakeView->CancelCount() == 1);
		PF_CHECK(Tests, Controller.GetPanelState(Context).Export.CancellationRequested);
		PF_CHECK(Tests, Controller.GetPanelState(Context).StatusMessage == "Export cancellation requested.");

		Controller.ProjectChanged("project switched");
		const auto StateAfterSwitch = Controller.GetPanelState(Context);
		PF_CHECK(Tests, FakeView->CancelCount() == 2);
		PF_CHECK(Tests, StateAfterSwitch.Export.Running);
		PF_CHECK(Tests, StateAfterSwitch.Export.CancellationRequested);
		PF_CHECK(Tests, StateAfterSwitch.StatusMessage == "Export cancellation requested because project switched.");
		PF_CHECK(Tests, !StateAfterSwitch.Profile.has_value());
		PF_CHECK(Tests, StateAfterSwitch.ProfilePath.empty());
		PF_CHECK(Tests, StateAfterSwitch.TemplateDescriptorPath.empty());

		FakeView->Finish();
		Controller.Shutdown("test complete");
		PF_CHECK(Tests, FakeView->ShutdownCount() == 1);
		PF_CHECK(Tests, FakeView->CancelCount() == 2);
	}
} // namespace

void RunEditorBuildProfileControllerTests(TestRunner& Tests)
{
	TestControllerProfileCreateLoadSaveTransitions(Tests);
	TestControllerStartExportValidationAndRequestConstruction(Tests);
	TestControllerCancellationAndProjectChangeReachActiveExport(Tests);
}
