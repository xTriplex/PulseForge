#include "TestEditorSupport.h"

#include "Assets/AssetTypes.h"

namespace
{
	void TestEditorCommandLineAndStartupProjectLoading(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Identifier = UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const std::filesystem::path Root = TemporaryDirectory / ("PulseForge CLI Test " + Identifier->ToString());
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ Root };

		std::filesystem::create_directories(Root, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const std::filesystem::path ProjectFile = Root / "Project With Spaces.pfproj";
		auto Created = Project::Create(ProjectFile, "CLI Project With Spaces");
		PF_CHECK(Tests, Created.has_value());
		if (!Created) return;

		const std::filesystem::path CreatedProjectFile = Root / "Prepared Project" / "Prepared Project.pfproj";
		auto PreparedCreation = PrepareEditorProjectCreation(CreatedProjectFile);
		PF_CHECK(Tests, PreparedCreation.has_value());
		PF_CHECK(Tests, PreparedCreation && PreparedCreation->StartupScene != nullptr);
		PF_CHECK(Tests, PreparedCreation && PreparedCreation->StartupSceneAsset.has_value());
		PF_CHECK(Tests, PreparedCreation && PreparedCreation->Project.GetDescription().Name == "Prepared Project");
		if (PreparedCreation && PreparedCreation->StartupSceneAsset)
		{
			const AssetID MainSceneID = *PreparedCreation->StartupSceneAsset;
			const auto MainSceneRecord = PreparedCreation->Project.GetAssetRegistry().Find(MainSceneID);
			PF_CHECK(Tests, MainSceneRecord.has_value());
			PF_CHECK(Tests, MainSceneRecord && MainSceneRecord->ProjectRelativePath == std::filesystem::path("Assets") / "Main.scene");
			PF_CHECK(Tests, !PreparedCreation->Project.GetDescription().StartScene.has_value());
			const auto StartSceneWarning = CommitPreparedProjectSettings(*PreparedCreation);
			PF_CHECK(Tests, !StartSceneWarning.has_value());
			PF_CHECK(Tests, PreparedCreation->Project.GetDescription().StartScene == MainSceneID);
			const auto Reopened = PrepareEditorProjectOpen(CreatedProjectFile);
			PF_CHECK(Tests, Reopened.has_value());
			PF_CHECK(Tests, Reopened && Reopened->StartupScene != nullptr);
			PF_CHECK(Tests, Reopened && Reopened->StartupSceneAsset == MainSceneID);
		}

		const std::span<const std::string_view> NoArguments;
		const auto DefaultOptions = ParseEditorCommandLine(NoArguments, Root);
		PF_CHECK(Tests, DefaultOptions.has_value());
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->ProjectFile.has_value());
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableEditorRuntimePlayStopTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableSceneViewportNativeSmoke);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->DocumentHostCloseUiTestScenario);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTestFixture);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostResetLayoutTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabTearOffTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabReattachTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabReorderTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostPresentationFailureTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostAcquireOutOfDateTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostImageAllocationFailureTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->ProjectOpenRenderTestFile);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->DiagnosticsCaptureSmokeOutput);

		const std::string AbsoluteProjectPath = ProjectFile.string();
#if defined(_DEBUG) || defined(PF_DEBUG)
		const std::array RuntimePlayStopFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--editor-runtime-play-stop-test") };
		const auto RuntimePlayStopFixtureOptions = ParseEditorCommandLine(RuntimePlayStopFixtureArguments, Root);
		PF_CHECK(Tests, RuntimePlayStopFixtureOptions.has_value());
		PF_CHECK(Tests, RuntimePlayStopFixtureOptions && RuntimePlayStopFixtureOptions->ProjectFile == ProjectFile &&
					RuntimePlayStopFixtureOptions->EnableEditorRuntimePlayStopTest);
		const std::string DiagnosticsTracePath = (Root / "diagnostics-smoke.json").string();
		const std::array DiagnosticsSmokeArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--diagnostics-capture-smoke"), std::string_view(DiagnosticsTracePath) };
		const auto DiagnosticsSmokeOptions = ParseEditorCommandLine(DiagnosticsSmokeArguments, Root);
		PF_CHECK(Tests, DiagnosticsSmokeOptions && DiagnosticsSmokeOptions->ProjectFile == ProjectFile &&
			DiagnosticsSmokeOptions->DiagnosticsCaptureSmokeOutput == std::filesystem::path(DiagnosticsTracePath) &&
			DiagnosticsSmokeOptions->EnableEditorRuntimePlayStopTest);
		const std::array DiagnosticsSmokeWithoutProject = { std::string_view("--diagnostics-capture-smoke"),
			std::string_view(DiagnosticsTracePath) };
		const auto DiagnosticsSmokeWithoutProjectOptions = ParseEditorCommandLine(DiagnosticsSmokeWithoutProject, Root);
		PF_CHECK(Tests, !DiagnosticsSmokeWithoutProjectOptions &&
			DiagnosticsSmokeWithoutProjectOptions.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array DiagnosticsSmokeMissingOutput = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--diagnostics-capture-smoke") };
		const auto DiagnosticsSmokeMissingOutputOptions = ParseEditorCommandLine(DiagnosticsSmokeMissingOutput, Root);
		PF_CHECK(Tests, !DiagnosticsSmokeMissingOutputOptions &&
			DiagnosticsSmokeMissingOutputOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicateRuntimeSmokeArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--editor-runtime-play-stop-test"), std::string_view("--diagnostics-capture-smoke"),
			std::string_view(DiagnosticsTracePath) };
		const auto DuplicateRuntimeSmokeOptions = ParseEditorCommandLine(DuplicateRuntimeSmokeArguments, Root);
		PF_CHECK(Tests, !DuplicateRuntimeSmokeOptions &&
			DuplicateRuntimeSmokeOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array RuntimePlayStopFixtureWithoutProject = { std::string_view("--editor-runtime-play-stop-test") };
		const auto RuntimePlayStopWithoutProjectOptions = ParseEditorCommandLine(RuntimePlayStopFixtureWithoutProject, Root);
		PF_CHECK(Tests, !RuntimePlayStopWithoutProjectOptions &&
					RuntimePlayStopWithoutProjectOptions.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array SceneViewportSmokeArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--scene-viewport-native-smoke") };
		const auto SceneViewportSmokeOptions = ParseEditorCommandLine(SceneViewportSmokeArguments, Root);
		PF_CHECK(Tests, SceneViewportSmokeOptions && SceneViewportSmokeOptions->EnableSceneViewportNativeSmoke &&
					SceneViewportSmokeOptions->ProjectFile == ProjectFile);
		const std::array SceneViewportSmokeWithoutProject = { std::string_view("--scene-viewport-native-smoke") };
		const auto SceneViewportSmokeWithoutProjectOptions = ParseEditorCommandLine(SceneViewportSmokeWithoutProject, Root);
		PF_CHECK(Tests, !SceneViewportSmokeWithoutProjectOptions &&
					SceneViewportSmokeWithoutProjectOptions.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array DuplicateSceneViewportSmoke = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--scene-viewport-native-smoke"), std::string_view("--scene-viewport-native-smoke") };
		const auto DuplicateSceneViewportSmokeOptions = ParseEditorCommandLine(DuplicateSceneViewportSmoke, Root);
		PF_CHECK(Tests, !DuplicateSceneViewportSmokeOptions &&
					DuplicateSceneViewportSmokeOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array DuplicateRuntimePlayStopFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--editor-runtime-play-stop-test"), std::string_view("--editor-runtime-play-stop-test") };
		const auto DuplicateRuntimePlayStopOptions = ParseEditorCommandLine(DuplicateRuntimePlayStopFixture, Root);
		PF_CHECK(Tests, !DuplicateRuntimePlayStopOptions &&
					DuplicateRuntimePlayStopOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ConflictingRuntimePlayStopFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--editor-runtime-play-stop-test"), std::string_view("--project-open-render-test"),
			std::string_view(AbsoluteProjectPath) };
		const auto ConflictingRuntimePlayStopOptions = ParseEditorCommandLine(ConflictingRuntimePlayStopFixture, Root);
		PF_CHECK(Tests, !ConflictingRuntimePlayStopOptions &&
					ConflictingRuntimePlayStopOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);

		const std::array<std::string_view, 4> CloseUiScenarios = { "cancel-save", "discard", "save-retry", "close-retry" };
		for (const std::string_view Scenario : CloseUiScenarios)
		{
			const std::array Arguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
				std::string_view("--document-host-close-ui-test"), Scenario };
			const auto Options = ParseEditorCommandLine(Arguments, Root);
			PF_CHECK(Tests, Options.has_value());
			PF_CHECK(Tests, Options && Options->ProjectFile == ProjectFile && Options->DocumentHostCloseUiTestScenario &&
							*Options->DocumentHostCloseUiTestScenario == Scenario);
		}
		const std::array MissingCloseUiScenario = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-close-ui-test") };
		const auto MissingCloseUiScenarioOptions = ParseEditorCommandLine(MissingCloseUiScenario, Root);
		PF_CHECK(Tests, !MissingCloseUiScenarioOptions &&
					MissingCloseUiScenarioOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array CloseUiWithoutProject = { std::string_view("--document-host-close-ui-test"), std::string_view("cancel-save") };
		const auto CloseUiWithoutProjectOptions = ParseEditorCommandLine(CloseUiWithoutProject, Root);
		PF_CHECK(Tests, !CloseUiWithoutProjectOptions &&
					CloseUiWithoutProjectOptions.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array DuplicateCloseUiOption = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-close-ui-test"), std::string_view("cancel-save"),
			std::string_view("--document-host-close-ui-test"), std::string_view("discard") };
		const auto DuplicateCloseUiOptions = ParseEditorCommandLine(DuplicateCloseUiOption, Root);
		PF_CHECK(Tests, !DuplicateCloseUiOptions && DuplicateCloseUiOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array InvalidCloseUiScenario = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-close-ui-test"), std::string_view("retry") };
		const auto InvalidCloseUiOptions = ParseEditorCommandLine(InvalidCloseUiScenario, Root);
		PF_CHECK(Tests, !InvalidCloseUiOptions && InvalidCloseUiOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
#else
		const std::array SceneViewportSmokeArguments = { std::string_view("--scene-viewport-native-smoke") };
		const auto SceneViewportSmokeOptions = ParseEditorCommandLine(SceneViewportSmokeArguments, Root);
		PF_CHECK(Tests, !SceneViewportSmokeOptions &&
					SceneViewportSmokeOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array RuntimePlayStopFixtureArguments = { std::string_view("--editor-runtime-play-stop-test") };
		const auto RuntimePlayStopFixtureOptions = ParseEditorCommandLine(RuntimePlayStopFixtureArguments, Root);
		PF_CHECK(Tests, !RuntimePlayStopFixtureOptions &&
					RuntimePlayStopFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DiagnosticsSmokeArguments = { std::string_view("--diagnostics-capture-smoke"), std::string_view("trace.json") };
		const auto DiagnosticsSmokeOptions = ParseEditorCommandLine(DiagnosticsSmokeArguments, Root);
		PF_CHECK(Tests, !DiagnosticsSmokeOptions &&
					DiagnosticsSmokeOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array CloseUiFixtureArguments = { std::string_view("--document-host-close-ui-test"),
			std::string_view("cancel-save") };
		const auto CloseUiFixtureOptions = ParseEditorCommandLine(CloseUiFixtureArguments, Root);
		PF_CHECK(Tests, !CloseUiFixtureOptions && CloseUiFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
#endif

		const std::array ProjectOpenRenderTestArguments = { std::string_view("--project-open-render-test"),
			std::string_view(AbsoluteProjectPath) };
		const auto ProjectOpenRenderTestOptions = ParseEditorCommandLine(ProjectOpenRenderTestArguments, Root);
		PF_CHECK(Tests, ProjectOpenRenderTestOptions.has_value());
		PF_CHECK(Tests, ProjectOpenRenderTestOptions && ProjectOpenRenderTestOptions->ProjectOpenRenderTestFile == ProjectFile);
		const std::array MissingProjectOpenRenderTestValue = { std::string_view("--project-open-render-test") };
		const auto MissingProjectOpenRenderTestOptions = ParseEditorCommandLine(MissingProjectOpenRenderTestValue, Root);
		PF_CHECK(Tests, !MissingProjectOpenRenderTestOptions);
		PF_CHECK(Tests, !MissingProjectOpenRenderTestOptions &&
							MissingProjectOpenRenderTestOptions.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array StartupAndProjectOpenRenderTestArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--project-open-render-test"), std::string_view(AbsoluteProjectPath) };
		const auto StartupAndProjectOpenRenderTestOptions = ParseEditorCommandLine(StartupAndProjectOpenRenderTestArguments, Root);
		PF_CHECK(Tests, StartupAndProjectOpenRenderTestOptions.has_value());
		PF_CHECK(Tests, StartupAndProjectOpenRenderTestOptions && StartupAndProjectOpenRenderTestOptions->ProjectFile == ProjectFile &&
							StartupAndProjectOpenRenderTestOptions->ProjectOpenRenderTestFile == ProjectFile);
		const std::array DuplicateProjectOpenRenderTestArguments = { std::string_view("--project-open-render-test"),
			std::string_view(AbsoluteProjectPath), std::string_view("--project-open-render-test"), std::string_view(AbsoluteProjectPath) };
		const auto DuplicateProjectOpenRenderTestOptions = ParseEditorCommandLine(DuplicateProjectOpenRenderTestArguments, Root);
		PF_CHECK(Tests, !DuplicateProjectOpenRenderTestOptions);
		PF_CHECK(Tests, !DuplicateProjectOpenRenderTestOptions &&
							DuplicateProjectOpenRenderTestOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array AbsoluteArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath) };
		const auto AbsoluteOptions = ParseEditorCommandLine(AbsoluteArguments, Root);
		PF_CHECK(Tests, AbsoluteOptions.has_value());
		PF_CHECK(Tests, AbsoluteOptions && AbsoluteOptions->ProjectFile == ProjectFile);

		const std::string RelativeProjectPath = ProjectFile.filename().string();
		const std::array RelativeArguments = { std::string_view("--project"), std::string_view(RelativeProjectPath) };
		const auto RelativeOptions = ParseEditorCommandLine(RelativeArguments, Root);
		PF_CHECK(Tests, RelativeOptions.has_value());
		PF_CHECK(Tests, RelativeOptions && RelativeOptions->ProjectFile == ProjectFile);

		const std::array HostFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture") };
		const auto HostFixtureOptions = ParseEditorCommandLine(HostFixtureArguments, Root);
		PF_CHECK(Tests, HostFixtureOptions.has_value());
		PF_CHECK(Tests, HostFixtureOptions && HostFixtureOptions->EnableDocumentHostTestFixture);
		const std::array HostFixtureWithoutProject = { std::string_view("--document-host-test-fixture") };
		const auto HostFixtureWithoutProjectResult = ParseEditorCommandLine(HostFixtureWithoutProject, Root);
		PF_CHECK(Tests, !HostFixtureWithoutProjectResult);
		PF_CHECK(Tests, !HostFixtureWithoutProjectResult &&
							HostFixtureWithoutProjectResult.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array DocumentWorkspaceSwitchFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-workspace-test-switch") };
		const auto DocumentWorkspaceSwitchFixtureOptions = ParseEditorCommandLine(DocumentWorkspaceSwitchFixtureArguments, Root);
		PF_CHECK(Tests, DocumentWorkspaceSwitchFixtureOptions.has_value());
		PF_CHECK(Tests, DocumentWorkspaceSwitchFixtureOptions && DocumentWorkspaceSwitchFixtureOptions->EnableDocumentWorkspaceSwitchTest);
		const std::array DocumentWorkspaceSwitchWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-workspace-test-switch") };
		const auto DocumentWorkspaceSwitchWithoutFixtureOptions = ParseEditorCommandLine(DocumentWorkspaceSwitchWithoutFixture, Root);
		PF_CHECK(Tests, !DocumentWorkspaceSwitchWithoutFixtureOptions);
		const std::array DuplicateHostFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-fixture") };
		const auto DuplicateHostFixtureOptions = ParseEditorCommandLine(DuplicateHostFixtureArguments, Root);
		PF_CHECK(Tests, !DuplicateHostFixtureOptions);
		PF_CHECK(
			Tests, !DuplicateHostFixtureOptions && DuplicateHostFixtureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ResetLayoutFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-reset-layout-test") };
		const auto ResetLayoutFixtureOptions = ParseEditorCommandLine(ResetLayoutFixtureArguments, Root);
		PF_CHECK(Tests, ResetLayoutFixtureOptions.has_value());
		PF_CHECK(Tests, ResetLayoutFixtureOptions && ResetLayoutFixtureOptions->EnableDocumentHostResetLayoutTest);
		const std::array ResetLayoutWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-reset-layout-test") };
		const auto ResetLayoutWithoutFixtureOptions = ParseEditorCommandLine(ResetLayoutWithoutFixture, Root);
		PF_CHECK(Tests, !ResetLayoutWithoutFixtureOptions);
		PF_CHECK(Tests, !ResetLayoutWithoutFixtureOptions &&
							ResetLayoutWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabTearOffFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-tab-tear-off") };
		const auto TabTearOffFixtureOptions = ParseEditorCommandLine(TabTearOffFixtureArguments, Root);
		PF_CHECK(Tests, TabTearOffFixtureOptions && TabTearOffFixtureOptions->EnableDocumentHostTabTearOffTest);
		const std::array TabTearOffWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-tear-off") };
		const auto TabTearOffWithoutFixtureOptions = ParseEditorCommandLine(TabTearOffWithoutFixture, Root);
		PF_CHECK(Tests, !TabTearOffWithoutFixtureOptions &&
							TabTearOffWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabReattachFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-tab-reattach") };
		const auto TabReattachFixtureOptions = ParseEditorCommandLine(TabReattachFixtureArguments, Root);
		PF_CHECK(Tests, TabReattachFixtureOptions && TabReattachFixtureOptions->EnableDocumentHostTabReattachTest);
		const std::array TabReattachWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-reattach") };
		const auto TabReattachWithoutFixtureOptions = ParseEditorCommandLine(TabReattachWithoutFixture, Root);
		PF_CHECK(Tests, !TabReattachWithoutFixtureOptions &&
							TabReattachWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array ConflictingTabTransferFixtures = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-tab-tear-off"),
			std::string_view("--document-host-test-tab-reattach") };
		const auto ConflictingTabTransferOptions = ParseEditorCommandLine(ConflictingTabTransferFixtures, Root);
		PF_CHECK(Tests,
			!ConflictingTabTransferOptions && ConflictingTabTransferOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabReorderFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-tab-reorder") };
		const auto TabReorderFixtureOptions = ParseEditorCommandLine(TabReorderFixtureArguments, Root);
		PF_CHECK(Tests, TabReorderFixtureOptions && TabReorderFixtureOptions->EnableDocumentHostTabReorderTest);
		const std::array TabReorderWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-reorder") };
		const auto TabReorderWithoutFixtureOptions = ParseEditorCommandLine(TabReorderWithoutFixture, Root);
		PF_CHECK(Tests, !TabReorderWithoutFixtureOptions &&
							TabReorderWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicateTabReorderFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-tab-reorder"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto DuplicateTabReorderOptions = ParseEditorCommandLine(DuplicateTabReorderFixture, Root);
		PF_CHECK(
			Tests, !DuplicateTabReorderOptions && DuplicateTabReorderOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ConflictingTabReorderFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-tab-reattach"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto ConflictingTabReorderOptions = ParseEditorCommandLine(ConflictingTabReorderFixture, Root);
		PF_CHECK(Tests,
			!ConflictingTabReorderOptions && ConflictingTabReorderOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array PresentationFailureFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-presentation-failure") };
		const auto PresentationFailureFixtureOptions = ParseEditorCommandLine(PresentationFailureFixtureArguments, Root);
		PF_CHECK(Tests, PresentationFailureFixtureOptions && PresentationFailureFixtureOptions->EnableDocumentHostPresentationFailureTest);
		const std::array PresentationFailureWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-presentation-failure") };
		const auto PresentationFailureWithoutFixtureOptions = ParseEditorCommandLine(PresentationFailureWithoutFixture, Root);
		PF_CHECK(Tests, !PresentationFailureWithoutFixtureOptions &&
							PresentationFailureWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicatePresentationFailureFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-presentation-failure"),
			std::string_view("--document-host-test-presentation-failure") };
		const auto DuplicatePresentationFailureOptions = ParseEditorCommandLine(DuplicatePresentationFailureFixture, Root);
		PF_CHECK(Tests, !DuplicatePresentationFailureOptions &&
							DuplicatePresentationFailureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ConflictingPresentationFailureFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-presentation-failure"),
			std::string_view("--document-host-reset-layout-test") };
		const auto ConflictingPresentationFailureOptions = ParseEditorCommandLine(ConflictingPresentationFailureFixture, Root);
		PF_CHECK(Tests, !ConflictingPresentationFailureOptions &&
							ConflictingPresentationFailureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array AcquireOutOfDateFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-acquire-out-of-date") };
		const auto AcquireOutOfDateFixtureOptions = ParseEditorCommandLine(AcquireOutOfDateFixtureArguments, Root);
		PF_CHECK(Tests, AcquireOutOfDateFixtureOptions && AcquireOutOfDateFixtureOptions->EnableDocumentHostAcquireOutOfDateTest);
		const std::array AcquireOutOfDateWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto AcquireOutOfDateWithoutFixtureOptions = ParseEditorCommandLine(AcquireOutOfDateWithoutFixture, Root);
		PF_CHECK(Tests, !AcquireOutOfDateWithoutFixtureOptions &&
							AcquireOutOfDateWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicateAcquireOutOfDateFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-acquire-out-of-date"),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto DuplicateAcquireOutOfDateFixtureOptions = ParseEditorCommandLine(DuplicateAcquireOutOfDateFixture, Root);
		PF_CHECK(Tests, !DuplicateAcquireOutOfDateFixtureOptions &&
							DuplicateAcquireOutOfDateFixtureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ImageAllocationFailureFixtureArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-image-allocation-failure") };
		const auto ImageAllocationFailureFixtureOptions = ParseEditorCommandLine(ImageAllocationFailureFixtureArguments, Root);
		PF_CHECK(Tests,
			ImageAllocationFailureFixtureOptions && ImageAllocationFailureFixtureOptions->EnableDocumentHostImageAllocationFailureTest);
		const std::array ImageAllocationFailureWithoutFixture = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ImageAllocationFailureWithoutFixtureOptions = ParseEditorCommandLine(ImageAllocationFailureWithoutFixture, Root);
		PF_CHECK(Tests, !ImageAllocationFailureWithoutFixtureOptions &&
							ImageAllocationFailureWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array ConflictingPresentationFailureFixtures = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"), std::string_view("--document-host-test-acquire-out-of-date"),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ConflictingAllocationFailureOptions = ParseEditorCommandLine(ConflictingPresentationFailureFixtures, Root);
		PF_CHECK(Tests, !ConflictingAllocationFailureOptions &&
							ConflictingAllocationFailureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);

		const std::filesystem::path StressManifest = Root / "stress.manifest";
		{
			std::ofstream Manifest(StressManifest, std::ios::binary | std::ios::trunc);
			Manifest << "fixture|fixture.png\n";
			PF_CHECK(Tests, static_cast<bool>(Manifest));
		}
		const std::string StressManifestPath = StressManifest.string();
		const std::array StressArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--asset-preview-stress"), std::string_view(StressManifestPath), std::string_view("entries") };
		const auto StressOptions = ParseEditorCommandLine(StressArguments, Root);
		PF_CHECK(Tests, StressOptions.has_value());
		PF_CHECK(Tests, StressOptions && StressOptions->AssetPreviewStressManifest == StressManifest);
		PF_CHECK(Tests, StressOptions && StressOptions->AssetPreviewStressBudget == "entries");
		const std::array MissingStressMode = { std::string_view("--asset-preview-stress"), std::string_view(StressManifestPath) };
		const auto MissingStressModeResult = ParseEditorCommandLine(MissingStressMode, Root);
		PF_CHECK(Tests, !MissingStressModeResult);

		const std::array MissingValue = { std::string_view("--project") };
		const auto MissingValueResult = ParseEditorCommandLine(MissingValue, Root);
		PF_CHECK(Tests, !MissingValueResult);
		PF_CHECK(Tests, !MissingValueResult && MissingValueResult.error().Code == EditorCommandLineErrorCode::MissingProjectPath);

		const std::array Unexpected = { std::string_view("--unknown") };
		const auto UnexpectedResult = ParseEditorCommandLine(Unexpected, Root);
		PF_CHECK(Tests, !UnexpectedResult);
		PF_CHECK(Tests, !UnexpectedResult && UnexpectedResult.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);

		const std::array Duplicate = { std::string_view("--project"), std::string_view(AbsoluteProjectPath), std::string_view("--project"),
			std::string_view(AbsoluteProjectPath) };
		const auto DuplicateResult = ParseEditorCommandLine(Duplicate, Root);
		PF_CHECK(Tests, !DuplicateResult);
		PF_CHECK(Tests, !DuplicateResult && DuplicateResult.error().Code == EditorCommandLineErrorCode::DuplicateProjectOption);

		const std::string MissingProjectPath = (Root / "Missing.pfproj").string();
		const std::array InvalidProject = { std::string_view("--project"), std::string_view(MissingProjectPath) };
		const auto InvalidProjectResult = ParseEditorCommandLine(InvalidProject, Root);
		PF_CHECK(Tests, !InvalidProjectResult);
		PF_CHECK(Tests, !InvalidProjectResult && InvalidProjectResult.error().Code == EditorCommandLineErrorCode::InvalidProjectPath);

		auto Loaded = PrepareEditorProjectOpen(ProjectFile);
		PF_CHECK(Tests, Loaded.has_value());
		PF_CHECK(Tests, Loaded && Loaded->Project.GetDescription().Name == "CLI Project With Spaces");
		PF_CHECK(Tests, Loaded && !Loaded->StartupScene && !Loaded->StartupSceneAsset);

		const std::filesystem::path BrokenProjectFile = Root / "Broken Project.pfproj";
		{
			std::ofstream BrokenProject(BrokenProjectFile, std::ios::binary | std::ios::trunc);
			BrokenProject << "{broken";
			PF_CHECK(Tests, static_cast<bool>(BrokenProject));
		}
		const std::string BrokenProjectPath = BrokenProjectFile.string();
		const std::array BrokenArguments = { std::string_view("--project"), std::string_view(BrokenProjectPath) };
		const auto BrokenOptions = ParseEditorCommandLine(BrokenArguments, Root);
		PF_CHECK(Tests, BrokenOptions.has_value());
		if (BrokenOptions)
		{
			const auto BrokenLoad = PrepareEditorProjectOpen(*BrokenOptions->ProjectFile);
			PF_CHECK(Tests, !BrokenLoad);
			PF_CHECK(Tests, !BrokenLoad && BrokenLoad.error().Message.find("could not be opened") != std::string::npos);
		}
	}
	void TestBundledValidationProjectAssetRegistry(TestRunner& Tests)
	{
		using namespace PulseForge;
		const std::filesystem::path RepositoryRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
		const std::filesystem::path ProjectFile = RepositoryRoot / "PulseForgeGame" / "PulseForgeGame.pfproj";
		auto OpenedProject = Project::Open(ProjectFile);
		PF_CHECK(Tests, OpenedProject.has_value());
		if (!OpenedProject) return;

		const std::filesystem::path AssetsRoot = ProjectFile.parent_path() / "Assets";
		const std::vector<AssetRecord> RegisteredAssets = OpenedProject->GetAssetRegistry().GetAssets();
		size_t ManagedSourceCount = 0;
		std::error_code FileError;
		for (std::filesystem::recursive_directory_iterator Iterator(AssetsRoot, FileError), End; !FileError && Iterator != End;
			Iterator.increment(FileError))
		{
			if (!Iterator->is_regular_file(FileError) || FileError || IsMetadataSidecarPath(Iterator->path())) continue;

			++ManagedSourceCount;
			const std::filesystem::path RelativePath = Iterator->path().lexically_relative(ProjectFile.parent_path());
			const auto Record = std::find_if(RegisteredAssets.begin(), RegisteredAssets.end(),
				[&RelativePath](const AssetRecord& Asset) { return Asset.ProjectRelativePath == RelativePath; });
			PF_CHECK(Tests, Record != RegisteredAssets.end());
		}
		PF_CHECK(Tests, !FileError);
		PF_CHECK(Tests, RegisteredAssets.size() == ManagedSourceCount);

		const std::filesystem::path DemoProjectFile =
			RepositoryRoot / "Samples" / "VisualValidation" / "PointLightShadows" / "PointLightShadows.pfproj";
		auto DemoProject = Project::Open(DemoProjectFile);
		PF_CHECK(Tests, DemoProject.has_value());
		if (!DemoProject) return;
		PF_CHECK(Tests, DemoProject->GetDescription().StartScene.has_value());
		if (!DemoProject->GetDescription().StartScene) return;
		const auto StartupRecord = DemoProject->GetAssetRegistry().Find(*DemoProject->GetDescription().StartScene);
		PF_CHECK(Tests, StartupRecord.has_value());
		PF_CHECK(Tests, StartupRecord && StartupRecord->ProjectRelativePath.filename() == "PointLightShadowsOverview.scene");
		size_t LoadedDemoSceneCount = 0;
		for (const AssetRecord& Asset : DemoProject->GetAssetRegistry().GetAssets())
		{
			if (!HasAssetTypeExtension(Asset.ProjectRelativePath, AssetFileType::Scene)) continue;

			Scene LoadedDemoScene;
			PF_CHECK(Tests, SceneAssetService::Load(Asset.ID, DemoProject->GetRootPath(), DemoProject->GetAssetRegistry(), LoadedDemoScene)
								.has_value());
			const auto ReferenceIssues = AssetReferenceValidator::Validate(LoadedDemoScene, DemoProject->GetAssetRegistry());
			PF_CHECK(Tests, ReferenceIssues.has_value());
			PF_CHECK(Tests, ReferenceIssues && ReferenceIssues->empty());
			++LoadedDemoSceneCount;
		}
		PF_CHECK(Tests, LoadedDemoSceneCount == 4);

		const std::filesystem::path EditorWorkflowProjectFile =
			RepositoryRoot / "Samples" / "VisualValidation" / "EditorWorkflow" / "EditorWorkflow.pfproj";
		auto EditorWorkflowProject = Project::Open(EditorWorkflowProjectFile);
		PF_CHECK(Tests, EditorWorkflowProject.has_value());
		if (!EditorWorkflowProject) return;
		PF_CHECK(Tests, EditorWorkflowProject->GetDescription().StartScene.has_value());
		if (!EditorWorkflowProject->GetDescription().StartScene) return;
		const auto EditorWorkflowStartup =
			EditorWorkflowProject->GetAssetRegistry().Find(*EditorWorkflowProject->GetDescription().StartScene);
		PF_CHECK(Tests, EditorWorkflowStartup.has_value());
		PF_CHECK(Tests, EditorWorkflowStartup && EditorWorkflowStartup->ProjectRelativePath.filename() == "EditorWorkflowOverview.scene");
		Scene EditorWorkflowScene;
		PF_CHECK(Tests, SceneAssetService::Load(*EditorWorkflowProject->GetDescription().StartScene, EditorWorkflowProject->GetRootPath(),
							EditorWorkflowProject->GetAssetRegistry(), EditorWorkflowScene)
							.has_value());
		const auto EditorWorkflowReferences =
			AssetReferenceValidator::Validate(EditorWorkflowScene, EditorWorkflowProject->GetAssetRegistry());
		PF_CHECK(Tests, EditorWorkflowReferences.has_value());
		PF_CHECK(Tests, EditorWorkflowReferences && EditorWorkflowReferences->empty());
		PF_CHECK(Tests, EditorWorkflowScene.GetEntityCount() == 13);
		const std::vector<AssetRecord> WorkflowAssets = EditorWorkflowProject->GetAssetRegistry().GetAssets();
		size_t WorkflowMaterialCount = 0;
		for (const AssetRecord& Asset : WorkflowAssets)
			if (HasAssetTypeExtension(Asset.ProjectRelativePath, AssetFileType::Material))
			{
				const auto Material =
					MaterialAssetService::Load(Asset.ID, EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry());
				PF_CHECK(Tests, Material.has_value());
				++WorkflowMaterialCount;
			}
		PF_CHECK(Tests, WorkflowMaterialCount == 4);
		const auto WorkflowMesh = std::find_if(WorkflowAssets.begin(), WorkflowAssets.end(),
			[](const AssetRecord& Asset) { return Asset.ProjectRelativePath == std::filesystem::path("Assets/Models/WorkflowCube.gltf"); });
		PF_CHECK(Tests, WorkflowMesh != WorkflowAssets.end());
		if (WorkflowMesh != WorkflowAssets.end())
		{
			const auto ImportedWorkflowMesh = GltfMeshImporter::ImportStaticPrimitive(
				WorkflowMesh->ID, EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry());
			PF_CHECK(Tests, ImportedWorkflowMesh.has_value());
			PF_CHECK(Tests, ImportedWorkflowMesh && !ImportedWorkflowMesh->Vertices.empty() && !ImportedWorkflowMesh->Indices.empty());
		}
	}
} // namespace

void RunEditorProjectLoadingTests(TestRunner& Tests)
{
	TestEditorCommandLineAndStartupProjectLoading(Tests);
	TestBundledValidationProjectAssetRegistry(Tests);
}
