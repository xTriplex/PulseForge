#include "TestEditorSupport.h"

namespace
{
	void TestEditorWorkspacePanelState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		PF_CHECK(Tests, CurrentDefaultEditorLayoutVersion == 4);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("0") == 0);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("1") == 1);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("01") == 1);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("2") == 2);
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("3"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("4294967296"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("remembered"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference(" 1"));
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, true, 2) == 2);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, false, 2) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(false, true, 1) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, true, std::nullopt) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::ContentBrowser) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Console) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Scene) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Inspector) == EditorDefaultDockRegion::Right);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::MaterialDetails) == EditorDefaultDockRegion::Right);
		PF_CHECK(Tests, EditorPanelDescriptors.size() == static_cast<size_t>(EditorPanel::Count));
		constexpr std::array<std::string_view, static_cast<size_t>(EditorPanel::Count)> StablePanelNames = { "Scene Viewport", "Hierarchy",
			"Inspector", "Content Browser", "Console", "Scene", "Material Editor" };

		EditorPanelVisibility Panels;
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::SceneViewport));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Hierarchy));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Inspector));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::ContentBrowser));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Console));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::MaterialDetails));

		for (size_t Index = 0; Index < EditorPanelDescriptors.size(); ++Index)
		{
			const EditorPanelDescriptor& Descriptor = EditorPanelDescriptors[Index];
			PF_CHECK(Tests, Descriptor.WindowName == StablePanelNames[Index]);
			PF_CHECK(Tests, !Descriptor.SettingsKey.empty());
			PF_CHECK(Tests, !Descriptor.WindowName.empty());
			if (Descriptor.Panel == EditorPanel::MaterialDetails)
				PF_CHECK(Tests, Descriptor.IsUtilityPanel && !Descriptor.DefaultVisible && Descriptor.SettingsKey == "MaterialEditor");
			for (size_t OtherIndex = Index + 1; OtherIndex < EditorPanelDescriptors.size(); ++OtherIndex)
			{
				PF_CHECK(Tests, Descriptor.SettingsKey != EditorPanelDescriptors[OtherIndex].SettingsKey);
				PF_CHECK(Tests, Descriptor.WindowName != EditorPanelDescriptors[OtherIndex].WindowName);
			}
		}

		PF_CHECK(Tests, Panels.SetVisible(EditorPanel::Scene, true));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, Panels.SetVisible(EditorPanel::Console, false));
		PF_CHECK(Tests, !Panels.SetVisible(EditorPanel::Console, false));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Console));
		Panels.ResetToDefaults();
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Console));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::MaterialDetails));
		PF_CHECK(Tests, Panels.GetVisibility(EditorPanel::Scene) != nullptr);
		PF_CHECK(Tests, Panels.GetVisibility(EditorPanel::Count) == nullptr);
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Count));
		PF_CHECK(Tests, !Panels.SetVisible(EditorPanel::Count, true));
		PF_CHECK(Tests, !ShouldApplyFirstOpenPanelDocking(true, true));
		PF_CHECK(Tests, !ShouldApplyFirstOpenPanelDocking(false, false));
		PF_CHECK(Tests, ShouldApplyFirstOpenPanelDocking(false, true));
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".material", true, false) == AssetBrowserOpenAction::None);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".material", true, true) == AssetBrowserOpenAction::ShowMaterialDetails);
		// Utility-panel opening is intentionally independent of document-host preferences.
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".scene", true, true) == AssetBrowserOpenAction::OpenScene);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".png", true, true) == AssetBrowserOpenAction::None);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".scene", false, true) == AssetBrowserOpenAction::None);
		const std::string DraftKey = MakeMaterialDraftKey("C:/Project", "material-uuid");
		PF_CHECK(Tests, DraftKey == MakeMaterialDraftKey("C:/Project", "material-uuid"));
		PF_CHECK(Tests, DraftKey != MakeMaterialDraftKey("D:/Project", "material-uuid"));
		PF_CHECK(Tests, DraftKey != MakeMaterialDraftKey("C:/Project", "other-material-uuid"));
	}

	void TestEditorDeferredOperationQueue(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		EditorDeferredOperationQueue Operations;
		std::vector<int> ExecutionOrder;
		Operations.Enqueue([&ExecutionOrder] { ExecutionOrder.push_back(1); });
		Operations.Enqueue(
			[&ExecutionOrder, &Operations]
			{
				ExecutionOrder.push_back(2);
				Operations.Enqueue([&ExecutionOrder] { ExecutionOrder.push_back(4); });
			});

		PF_CHECK(Tests, ExecutionOrder.empty());
		PF_CHECK(Tests, Operations.Size() == 2);
		PF_CHECK(Tests, Operations.ExecuteNext());
		PF_CHECK(Tests, ExecutionOrder == std::vector<int>{ 1 });
		PF_CHECK(Tests, Operations.ExecuteNext());
		PF_CHECK(Tests, ExecutionOrder == (std::vector<int>{ 1, 2 }));
		PF_CHECK(Tests, Operations.Size() == 1);
		PF_CHECK(Tests, Operations.ExecuteNext());
		PF_CHECK(Tests, ExecutionOrder == (std::vector<int>{ 1, 2, 4 }));
		PF_CHECK(Tests, !Operations.ExecuteNext());
		Operations.Enqueue([] {});
		Operations.Clear();
		PF_CHECK(Tests, Operations.Size() == 0);
		PF_CHECK(Tests, !Operations.ExecuteNext());
	}

	void TestEditorAssetDestinationPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const std::filesystem::path Source = "Assets/Materials/Amber.material";
		const auto Extensionless = ResolveAssetDestinationFilename("Amber Copy", Source);
		PF_CHECK(Tests, Extensionless && *Extensionless == "Amber Copy.material");
		const auto MatchingExtension = ResolveAssetDestinationFilename("Amber-Renamed.MATERIAL", Source);
		PF_CHECK(Tests, MatchingExtension && *MatchingExtension == "Amber-Renamed.MATERIAL");
		const auto EmptyName = ResolveAssetDestinationFilename("", Source);
		PF_CHECK(Tests, !EmptyName && EmptyName.error() == AssetDestinationNameError::InvalidFilename);
		const auto RelativePath = ResolveAssetDestinationFilename("nested/Amber.material", Source);
		PF_CHECK(Tests, !RelativePath && RelativePath.error() == AssetDestinationNameError::InvalidFilename);
		const auto InvalidCharacter = ResolveAssetDestinationFilename("Amber?.material", Source);
		PF_CHECK(Tests, !InvalidCharacter && InvalidCharacter.error() == AssetDestinationNameError::InvalidFilename);
		const auto TrailingSpace = ResolveAssetDestinationFilename("Amber .material ", Source);
		PF_CHECK(Tests, !TrailingSpace && TrailingSpace.error() == AssetDestinationNameError::InvalidFilename);
		const auto MismatchedExtension = ResolveAssetDestinationFilename("Amber.png", Source);
		PF_CHECK(Tests, !MismatchedExtension && MismatchedExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		const auto ReservedExtension = ResolveAssetDestinationFilename("Amber.meta", Source);
		PF_CHECK(Tests, !ReservedExtension && ReservedExtension.error() == AssetDestinationNameError::ReservedSidecarExtension);
		const auto DestinationBreadcrumbs = BuildAssetDestinationBreadcrumbs("Assets/Materials/Props");
		PF_CHECK(Tests, DestinationBreadcrumbs.size() == 3);
		PF_CHECK(Tests, DestinationBreadcrumbs[0] == std::filesystem::path("Assets"));
		PF_CHECK(Tests, DestinationBreadcrumbs[1] == std::filesystem::path("Assets/Materials"));
		PF_CHECK(Tests, DestinationBreadcrumbs[2] == std::filesystem::path("Assets/Materials/Props"));
		const auto RootBreadcrumb = BuildAssetDestinationBreadcrumbs("Assets");
		PF_CHECK(Tests, RootBreadcrumb.size() == 1 && RootBreadcrumb.front() == std::filesystem::path("Assets"));
		const auto NewSceneName = ResolveAssetDestinationFilename("Overview", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, NewSceneName && *NewSceneName == "Overview.scene");
		const auto SavedSceneName =
			ResolveAssetDestinationFilename("Overview Copy.scene", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, SavedSceneName && *SavedSceneName == "Overview Copy.scene");
		const auto NewPrefabName = ResolveAssetDestinationFilename("Environment", std::filesystem::path("Assets/Prefabs/NewPrefab.prefab"));
		PF_CHECK(Tests, NewPrefabName && *NewPrefabName == "Environment.prefab");
		const auto WrongSceneExtension =
			ResolveAssetDestinationFilename("Overview.prefab", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, !WrongSceneExtension && WrongSceneExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		const auto WrongPrefabExtension =
			ResolveAssetDestinationFilename("Environment.scene", std::filesystem::path("Assets/Prefabs/NewPrefab.prefab"));
		PF_CHECK(Tests, !WrongPrefabExtension && WrongPrefabExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, false, false, false, false, false) == AssetDestinationConflict::None);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, false, true, true, false) == AssetDestinationConflict::SamePath);
		PF_CHECK(Tests,
			ResolveAssetDestinationConflict(false, true, false, false, false, false) == AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests,
			ResolveAssetDestinationConflict(true, false, true, false, false, false) == AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests,
			ResolveAssetDestinationConflict(false, true, false, true, false, false) == AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests,
			ResolveAssetDestinationConflict(false, true, true, true, false, false, true) == AssetDestinationConflict::CaseOnlyRename);
		PF_CHECK(Tests,
			ResolveAssetDestinationConflict(false, true, true, true, false, false, false) == AssetDestinationConflict::ExistingDestination);
		PF_CHECK(
			Tests, ResolveAssetDestinationConflict(false, false, false, false, false, true) == AssetDestinationConflict::InspectionFailed);
	}

	void TestEditorAssetSelectorPolicy(TestRunner& Tests)
	{
		using PulseForgeEditor::AssetHasExtension;
		using PulseForgeEditor::AssetSelectorMatchesSearch;
		const PulseForge::AssetRecord TextureAsset{ PulseForge::AssetID{}, std::filesystem::path("Assets/Textures/Stone_BaseColor.png") };
		PF_CHECK(Tests, AssetHasExtension(TextureAsset, { ".png", ".jpg" }));
		PF_CHECK(Tests, !AssetHasExtension(TextureAsset, { ".material" }));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", ""));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "stone"));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "TEXTURES"));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Materials/青い.material", "青い"));
		PF_CHECK(Tests, !AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "roughness"));
		PF_CHECK(Tests, !AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "Assets/Models"));
	}

	void TestEditorLayoutOpeningPreferencePersistence(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		PF_CHECK(Tests, ShouldBuildDefaultDockspace(false));
		PF_CHECK(Tests, !ShouldBuildDefaultDockspace(true));

		const auto TestID = PulseForge::UUID::Generate();
		PF_CHECK(Tests, TestID.has_value());
		if (!TestID) return;

		std::error_code Error;
		const std::filesystem::path SettingsDirectory =
			std::filesystem::temp_directory_path() / ("PulseForgeEditorLayoutPreference_" + TestID->ToString());
		const bool DirectoryCreated = std::filesystem::create_directory(SettingsDirectory, Error);
		PF_CHECK(Tests, DirectoryCreated && !Error);
		if (!DirectoryCreated || Error) return;
		struct ScopedDirectoryCleanup
		{
			std::filesystem::path Path;
			~ScopedDirectoryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Path, CleanupError);
			}
		} Cleanup{ SettingsDirectory };

		const wchar_t* ExistingSettingsDirectory = _wgetenv(L"PULSEFORGE_EDITOR_SETTINGS_DIR");
		const std::optional<std::wstring> PreviousSettingsDirectory =
			ExistingSettingsDirectory ? std::optional<std::wstring>(ExistingSettingsDirectory) : std::nullopt;
		const std::wstring SettingsDirectoryValue = SettingsDirectory.native();
		const errno_t EnvironmentResult = _wputenv_s(L"PULSEFORGE_EDITOR_SETTINGS_DIR", SettingsDirectoryValue.c_str());
		PF_CHECK(Tests, EnvironmentResult == 0);
		if (EnvironmentResult != 0) return;
		struct ScopedSettingsEnvironment
		{
			std::optional<std::wstring> PreviousValue;
			~ScopedSettingsEnvironment()
			{
				(void)_wputenv_s(L"PULSEFORGE_EDITOR_SETTINGS_DIR", PreviousValue ? PreviousValue->c_str() : L"");
			}
		} EnvironmentCleanup{ PreviousSettingsDirectory };

		struct ScopedImGuiContext
		{
			ImGuiContext* Previous = ImGui::GetCurrentContext();
			ImGuiContext* Context = ImGui::CreateContext();
			~ScopedImGuiContext()
			{
				if (Context) ImGui::DestroyContext(Context);
				ImGui::SetCurrentContext(Previous);
			}
		};

		const std::filesystem::path SettingsFile = SettingsDirectory / L"imgui.ini";
		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context) return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (!Initialized) return;
			PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == DefaultDocumentOpeningPreference);
			PF_CHECK(Tests, Layout.SetDocumentOpeningPreference(2));
			const auto Saved = Layout.SaveNow();
			PF_CHECK(Tests, Saved.has_value());
		}

		std::ifstream SavedSettingsInput(SettingsFile, std::ios::binary);
		const std::string SavedSettings((std::istreambuf_iterator<char>(SavedSettingsInput)), std::istreambuf_iterator<char>());
		PF_CHECK(Tests, SavedSettings.find("DocumentOpeningPreference=2") != std::string::npos);

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context) return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (Initialized) PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == 2);
		}

		{
			std::ofstream IncompleteSettingsOutput(SettingsFile, std::ios::binary | std::ios::trunc);
			IncompleteSettingsOutput << "[PulseForgeWorkspace][State]\n"
										"StateFormat=1\nDocumentOpeningPreference=1\nHierarchy=0\n";
			PF_CHECK(Tests, static_cast<bool>(IncompleteSettingsOutput));
		}

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context) return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (!Initialized) return;
			PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == 1);
			PF_CHECK(Tests, !Layout.IsPanelVisible(EditorPanel::Hierarchy));
		}

		{
			std::ofstream InvalidSettingsOutput(SettingsFile, std::ios::binary | std::ios::trunc);
			InvalidSettingsOutput << "[PulseForgeWorkspace][State]\n"
									 "StateFormat=99\nDocumentOpeningPreference=1\nHierarchy=0\n";
			PF_CHECK(Tests, static_cast<bool>(InvalidSettingsOutput));
		}

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context) return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (Initialized)
			{
				PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == DefaultDocumentOpeningPreference);
				PF_CHECK(Tests, Layout.IsPanelVisible(EditorPanel::Hierarchy));
			}
		}
	}

	void TestEditorWorkspaceViewHelpers(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", "cube"));
		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", "SAMPLE"));
		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", ""));
		PF_CHECK(Tests, !ContainsCaseInsensitive("Sample Cube", "sphere"));
		PF_CHECK(Tests, !ContainsCaseInsensitive("abc", "abcd"));

		PF_CHECK(Tests, IsManagedAssetInFolder("Assets/Readme.txt", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Props/Cube.glb", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Props/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, IsManagedAssetInFolder("Assets/Models/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Cube.glb", "Assets/Materials"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/ModelsExtra/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Outside/Cube.glb", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/../Outside/Cube.glb", "Assets"));
		PF_CHECK(Tests, IsImmediateChildFolder("Assets/Models", "Assets"));
		PF_CHECK(Tests, IsImmediateChildFolder("Assets/Models/Props", "Assets/Models"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/Models/Props", "Assets"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/ModelsExtra", "Assets/Models"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/../Outside", "Assets"));
		PF_CHECK(Tests, IsManagedAssetInFolderSubtree("Assets/Models/Cube.glb", "Assets"));
		PF_CHECK(Tests, IsManagedAssetInFolderSubtree("Assets/Models/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolderSubtree("Assets/ModelsExtra/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "cube"));
		PF_CHECK(Tests, MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "models/cube"));
		PF_CHECK(Tests, !MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets/Materials", "cube"));
		PF_CHECK(Tests, !MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "missing"));

		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Scenes/Main.scene") == EditorIcon::Scene);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Models/Cube.GLB") == EditorIcon::Cube);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Materials/Default.material") == EditorIcon::Material);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Scripts/Move.lua") == EditorIcon::Script);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Textures/Icon.png") == EditorIcon::Image);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Audio/Click.wav") == EditorIcon::AudioSource);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Other/Readme.txt") == EditorIcon::File);

		PF_CHECK(Tests, IsFolderAncestorOrSelf("Assets/Models/Props", "Assets/Models"));
		PF_CHECK(Tests, IsFolderAncestorOrSelf("Assets/Models", "Assets/Models"));
		PF_CHECK(Tests, !IsFolderAncestorOrSelf("Assets/ModelsExtra", "Assets/Models"));

		const auto FrontFacingAxes = ProjectViewportOrientationAxes({ 0.0f, 0.0f, -1.0f });
		PF_CHECK(Tests, FrontFacingAxes.has_value());
		if (FrontFacingAxes)
		{
			PF_CHECK(Tests, glm::length((*FrontFacingAxes)[0].ScreenDirection - glm::vec2(1.0f, 0.0f)) < 1.0e-5f);
			PF_CHECK(Tests, glm::length((*FrontFacingAxes)[1].ScreenDirection - glm::vec2(0.0f, -1.0f)) < 1.0e-5f);
			PF_CHECK(Tests, std::abs((*FrontFacingAxes)[2].Depth + 1.0f) < 1.0e-5f);
		}
		const auto RotatedAxes = ProjectViewportOrientationAxes(glm::normalize(glm::vec3(1.0f, 1.0f, -1.0f)));
		PF_CHECK(Tests, RotatedAxes.has_value());
		if (RotatedAxes) PF_CHECK(Tests, glm::length((*RotatedAxes)[2].ScreenDirection) > 0.1f);
		PF_CHECK(Tests, !ProjectViewportOrientationAxes(glm::vec3(0.0f)));
	}

	void TestEditorImGuiRendererMath(TestRunner& Tests)
	{
		using namespace PulseForgeEditor::ImGuiRendererMath;
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Position) == 0);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Color) == 1);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::TexCoord) == 2);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Normal) == 3);

		const auto ClippedToOldSwapchain = PulseForge::IntersectScissorRect({ 0, 0, 2560, 1361 }, 1280, 720);
		PF_CHECK(Tests, ClippedToOldSwapchain && ClippedToOldSwapchain->X == 0 && ClippedToOldSwapchain->Y == 0 &&
							ClippedToOldSwapchain->Width == 1280 && ClippedToOldSwapchain->Height == 720);
		const auto PartiallyVisible = PulseForge::IntersectScissorRect({ 1000, 600, 500, 300 }, 1280, 720);
		PF_CHECK(Tests, PartiallyVisible && PartiallyVisible->X == 1000 && PartiallyVisible->Y == 600 && PartiallyVisible->Width == 280 &&
							PartiallyVisible->Height == 120);
		PF_CHECK(Tests, !PulseForge::IntersectScissorRect({ 1280, 0, 1, 1 }, 1280, 720));
		PF_CHECK(Tests, !PulseForge::IntersectScissorRect({ 0, 0, 1, 1 }, 0, 720));

		const glm::vec2 DisplayPosition{ 20.0f, 30.0f };
		const glm::vec2 DisplaySize{ 100.0f, 50.0f };
		const auto TopLeft = PositionToVulkanNdc(DisplayPosition, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, TopLeft && glm::length(*TopLeft - glm::vec2(-1.0f, 1.0f)) < 1.0e-6f);
		const auto Center = PositionToVulkanNdc(DisplayPosition + DisplaySize * 0.5f, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, Center && glm::length(*Center) < 1.0e-6f);
		const auto BottomRight = PositionToVulkanNdc(DisplayPosition + DisplaySize, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, BottomRight && glm::length(*BottomRight - glm::vec2(1.0f, -1.0f)) < 1.0e-6f);
		PF_CHECK(Tests, !PositionToVulkanNdc({ 0.0f, 0.0f }, DisplayPosition, { 0.0f, 50.0f }));
		PF_CHECK(Tests, !PositionToVulkanNdc({ std::numeric_limits<float>::infinity(), 0.0f }, DisplayPosition, DisplaySize));

		const auto Extent = CalculateFramebufferExtent(DisplaySize, { 1.5f, 2.0f });
		PF_CHECK(Tests, Extent && Extent->Width == 150 && Extent->Height == 100);
		const auto FractionalExtent = CalculateFramebufferExtent({ 10.1f, 20.2f }, { 1.5f, 1.25f });
		PF_CHECK(Tests, FractionalExtent && FractionalExtent->Width == 16 && FractionalExtent->Height == 26);
		PF_CHECK(Tests, !CalculateFramebufferExtent({ 0.0f, 1.0f }, { 1.0f, 1.0f }));
		PF_CHECK(Tests, !CalculateFramebufferExtent(DisplaySize, { 1.0f, 0.0f }));
		PF_CHECK(Tests, !CalculateFramebufferExtent({ std::numeric_limits<float>::infinity(), 1.0f }, { 1.0f, 1.0f }));
		const auto LargeExtent = CalculateFramebufferExtent({ 100000.0f, 50000.0f }, { 1.0f, 1.0f });
		PF_CHECK(Tests, LargeExtent && LargeExtent->Width == 100000 && LargeExtent->Height == 50000);

		const FramebufferExtent ScissorExtent{ 200, 100 };
		const glm::vec2 Scale{ 2.0f, 2.0f };
		const auto Full = ClipRectToScissor(DisplayPosition, DisplayPosition + DisplaySize, DisplayPosition, Scale, ScissorExtent);
		PF_CHECK(Tests, Full && *Full && (*Full)->X == 0 && (*Full)->Y == 0 && (*Full)->Width == 200 && (*Full)->Height == 100);

		const auto Fractional = ClipRectToScissor(
			DisplayPosition + glm::vec2(1.2f, 2.4f), DisplayPosition + glm::vec2(5.1f, 6.2f), DisplayPosition, Scale, ScissorExtent);
		PF_CHECK(Tests, Fractional && *Fractional && (*Fractional)->X == 2 && (*Fractional)->Y == 4 && (*Fractional)->Width == 9 &&
							(*Fractional)->Height == 9);

		const auto PartlyOutside = ClipRectToScissor(
			DisplayPosition + glm::vec2(-10.0f, -5.0f), DisplayPosition + glm::vec2(5.0f, 8.0f), DisplayPosition, Scale, ScissorExtent);
		PF_CHECK(Tests, PartlyOutside && *PartlyOutside && (*PartlyOutside)->X == 0 && (*PartlyOutside)->Y == 0 &&
							(*PartlyOutside)->Width == 10 && (*PartlyOutside)->Height == 16);

		const auto FullyOutside = ClipRectToScissor(
			DisplayPosition + glm::vec2(150.0f, 10.0f), DisplayPosition + glm::vec2(160.0f, 20.0f), DisplayPosition, Scale, ScissorExtent);
		PF_CHECK(Tests, FullyOutside && !*FullyOutside);
		PF_CHECK(Tests, !ClipRectToScissor({ 0.0f, 0.0f }, { 1.0f, 1.0f }, {}, { 1.0f, 1.0f }, {}));
	}

	void TestEditorPlatformViewportSubmissionOrder(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		std::vector<int> Events;
		for (int ViewportID : { 1, 3 })
		{
			RenderPlatformViewportSequentially([&] { Events.push_back(ViewportID * 10 + 1); },
				[&] { Events.push_back(ViewportID * 10 + 2); }, [&] { Events.push_back(ViewportID * 10 + 3); },
				[&] { Events.push_back(ViewportID * 10 + 4); });
		}
		const std::vector<int> Expected{ 11, 12, 13, 14, 31, 32, 33, 34 };
		PF_CHECK(Tests, Events == Expected);
	}

	void TestEditorFontAndIconDescriptors(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		int GlfwWindowMarker = 0;
		int NativeWindowMarker = 0;
		const EditorGlfwViewportHandles ViewportHandles{ .GlfwWindow = &GlfwWindowMarker, .NativeWindow = &NativeWindowMarker };
		PF_CHECK(Tests, GetGlfwContextWindow(ViewportHandles) == &GlfwWindowMarker);
		PF_CHECK(Tests, GetGlfwContextWindow(ViewportHandles) != &NativeWindowMarker);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(590.0f) == EditorViewportToolbarLayout::Full);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(589.0f) == EditorViewportToolbarLayout::Compact);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(300.0f) == EditorViewportToolbarLayout::Compact);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(299.0f) == EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(-1.0f) == EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(std::numeric_limits<float>::quiet_NaN()) == EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(100.0f) == EditorStyleMetrics::PropertyLabelWidthMinimum);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(300.0f) == 90.0f);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(1000.0f) == EditorStyleMetrics::PropertyLabelWidthMaximum);
		PF_CHECK(Tests, GetMaterialPropertyLayout(260.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(360.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(500.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(559.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(560.0f) == MaterialPropertyLayout::TwoColumn);
		PF_CHECK(Tests, GetMaterialPropertyLayout(700.0f) == MaterialPropertyLayout::TwoColumn);
		PF_CHECK(Tests, GetMaterialPropertyLabelWidth(700.0f) == 264.0f);
		PF_CHECK(Tests, GetMaterialPropertyLabelWidth(1000.0f) == 264.0f);
		PF_CHECK(Tests, GetMaterialPropertyLayout(-1.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(std::numeric_limits<float>::quiet_NaN()) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(std::numeric_limits<float>::infinity()) == MaterialPropertyLayout::Stacked);
		const MaterialValuePairLayout NarrowMaterialValuePair = GetMaterialValuePairLayout(260.0f, 8.0f);
		PF_CHECK(Tests, NarrowMaterialValuePair.Stacked && NarrowMaterialValuePair.FirstWidth == 260.0f &&
							NarrowMaterialValuePair.SecondWidth == 260.0f);
		const MaterialValuePairLayout RegularMaterialValuePair = GetMaterialValuePairLayout(360.0f, 8.0f);
		PF_CHECK(Tests, !RegularMaterialValuePair.Stacked && RegularMaterialValuePair.FirstWidth == 176.0f &&
							RegularMaterialValuePair.SecondWidth == 176.0f &&
							RegularMaterialValuePair.FirstWidth + RegularMaterialValuePair.SecondWidth + 8.0f <= 360.0f);
		const MaterialValuePairLayout BrowserWidthMaterialValuePair = GetMaterialValuePairLayout(500.0f, 8.0f);
		PF_CHECK(Tests, !BrowserWidthMaterialValuePair.Stacked && BrowserWidthMaterialValuePair.FirstWidth == 246.0f &&
							BrowserWidthMaterialValuePair.SecondWidth == 246.0f &&
							BrowserWidthMaterialValuePair.FirstWidth + BrowserWidthMaterialValuePair.SecondWidth + 8.0f <= 500.0f);
		const MaterialValuePairLayout WideMaterialValuePair = GetMaterialValuePairLayout(700.0f, 8.0f);
		PF_CHECK(Tests,
			!WideMaterialValuePair.Stacked && WideMaterialValuePair.FirstWidth == 346.0f && WideMaterialValuePair.SecondWidth == 346.0f);
		const MaterialValuePairLayout InvalidMaterialValuePair =
			GetMaterialValuePairLayout(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
		PF_CHECK(Tests, InvalidMaterialValuePair.Stacked && InvalidMaterialValuePair.FirstWidth == 0.0f &&
							InvalidMaterialValuePair.SecondWidth == 0.0f);
		for (const MaterialValuePairLayout Layout : { NarrowMaterialValuePair, RegularMaterialValuePair, BrowserWidthMaterialValuePair,
				 WideMaterialValuePair, InvalidMaterialValuePair })
		{
			PF_CHECK(Tests, std::isfinite(Layout.FirstWidth) && Layout.FirstWidth >= 0.0f);
			PF_CHECK(Tests, std::isfinite(Layout.SecondWidth) && Layout.SecondWidth >= 0.0f);
		}
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarHeight == 36.0f);
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarPaddingX == 7.0f);
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarPaddingY == 4.0f);
		const ContentBrowserHorizontalLayout CompactBrowser = GetContentBrowserHorizontalLayout(519.0f);
		PF_CHECK(Tests, !CompactBrowser.ShowFolderTree && CompactBrowser.FolderTreeWidth == 0.0f);
		const ContentBrowserHorizontalLayout MinimumFolderTree = GetContentBrowserHorizontalLayout(520.0f);
		PF_CHECK(Tests, MinimumFolderTree.ShowFolderTree && MinimumFolderTree.FolderTreeWidth == 145.0f);
		const ContentBrowserHorizontalLayout RegularFolderTree = GetContentBrowserHorizontalLayout(800.0f);
		PF_CHECK(Tests, RegularFolderTree.ShowFolderTree && RegularFolderTree.FolderTreeWidth == 192.0f);
		const ContentBrowserHorizontalLayout MaximumFolderTree = GetContentBrowserHorizontalLayout(1200.0f);
		PF_CHECK(Tests, MaximumFolderTree.ShowFolderTree && MaximumFolderTree.FolderTreeWidth == 205.0f);
		const ContentBrowserHorizontalLayout InvalidBrowserWidth =
			GetContentBrowserHorizontalLayout(std::numeric_limits<float>::quiet_NaN());
		PF_CHECK(Tests, !InvalidBrowserWidth.ShowFolderTree && InvalidBrowserWidth.FolderTreeWidth == 0.0f);
		PF_CHECK(Tests, UseCompactVector3PropertyInput(229.0f));
		PF_CHECK(Tests, !UseCompactVector3PropertyInput(230.0f));
		PF_CHECK(Tests, UseCompactVector3PropertyInput(std::numeric_limits<float>::quiet_NaN()));
		const ContentBrowserVerticalLayout NoSelection = GetContentBrowserVerticalLayout(320.0f, false, true);
		PF_CHECK(Tests, NoSelection.AssetItemsHeight == 320.0f && NoSelection.DetailsHeight == 0.0f);
		const ContentBrowserVerticalLayout CompactSelection = GetContentBrowserVerticalLayout(120.0f, true, true);
		PF_CHECK(Tests, CompactSelection.SelectionSummaryHeight == ContentBrowserLayoutMetrics::CompactSelectionSummaryHeight);
		PF_CHECK(Tests, CompactSelection.AssetItemsHeight == 92.0f && CompactSelection.DetailsHeight == 0.0f);
		const ContentBrowserVerticalLayout ExpandedCompactSelection = GetContentBrowserVerticalLayout(120.0f, true, true, true);
		PF_CHECK(Tests, ExpandedCompactSelection.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, ExpandedCompactSelection.DetailsHeight == 0.0f && ExpandedCompactSelection.AssetItemsHeight == 92.0f);
		const ContentBrowserVerticalLayout RegularAssetDetails = GetContentBrowserVerticalLayout(320.0f, true, false);
		PF_CHECK(Tests, RegularAssetDetails.DetailsHeight == ContentBrowserLayoutMetrics::RegularDetailsMaximumHeight);
		PF_CHECK(Tests, RegularAssetDetails.AssetItemsHeight >= ContentBrowserLayoutMetrics::MinimumAssetItemsHeight);
		const ContentBrowserVerticalLayout ShortMaterialSummary = GetContentBrowserVerticalLayout(180.0f, true, true);
		PF_CHECK(Tests, ShortMaterialSummary.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, ShortMaterialSummary.DetailsHeight == 0.0f && ShortMaterialSummary.AssetItemsHeight == 152.0f);
		const ContentBrowserVerticalLayout TallMaterialSummary = GetContentBrowserVerticalLayout(840.0f, true, true);
		PF_CHECK(Tests, TallMaterialSummary.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, TallMaterialSummary.DetailsHeight == 0.0f && TallMaterialSummary.AssetItemsHeight == 812.0f);
		const ContentBrowserVerticalLayout InvalidContentHeight =
			GetContentBrowserVerticalLayout(std::numeric_limits<float>::quiet_NaN(), true, true);
		PF_CHECK(Tests, InvalidContentHeight.AssetItemsHeight == 0.0f && InvalidContentHeight.SelectionSummaryHeight == 0.0f);
		PF_CHECK(Tests, GetEditorEntityIcon({}) == EditorIcon::Entity);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasCamera = true, .HasMesh = true }) == EditorIcon::Camera);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasMesh = true, .HasPointLight = true }) == EditorIcon::Cube);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasSpotLight = true, .HasPointLight = true }) == EditorIcon::SpotLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasPointLight = true, .HasDirectionalLight = true }) == EditorIcon::PointLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasDirectionalLight = true }) == EditorIcon::DirectionalLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasEnvironmentLight = true }) == EditorIcon::Environment);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasAudioSource = true, .HasAudioListener = true }) == EditorIcon::AudioSource);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasAudioListener = true }) == EditorIcon::AudioListener);
		PF_CHECK(Tests, EditorFontDescriptors.size() == 3);
		PF_CHECK(Tests, EditorFontDescriptors[0].Role == EditorFontRole::Interface);
		PF_CHECK(Tests, EditorFontDescriptors[1].Role == EditorFontRole::Emphasis);
		PF_CHECK(Tests, EditorFontDescriptors[2].Role == EditorFontRole::Monospace);
		PF_CHECK(Tests, EditorFontDescriptors[0].RelativePath == "Fonts/Inter-Regular.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[1].RelativePath == "Fonts/Inter-SemiBold.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[2].RelativePath == "Fonts/JetBrainsMono-Regular.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[0].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorFontDescriptors[1].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorFontDescriptors[2].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorIconFontPixelSize == EditorFontDescriptors[0].PixelSize);
		PF_CHECK(Tests, IsValidEditorTextGlyphRanges());
		PF_CHECK(Tests, EditorTextGlyphRanges.back() == 0);
		for (size_t Index = 0; Index + 1 < EditorTextGlyphRanges.size() - 1; Index += 2)
		{
			PF_CHECK(Tests, EditorTextGlyphRanges[Index] <= EditorTextGlyphRanges[Index + 1]);
			if (Index > 0) PF_CHECK(Tests, EditorTextGlyphRanges[Index - 1] < EditorTextGlyphRanges[Index]);
		}
		for (const uint32_t Codepoint : { 0x011e, 0x011f, 0x0130, 0x0131, 0x015e, 0x015f, 0x010c, 0x0141, 0x017d, 0x0150, 0x00c5, 0x00e9,
				 0x00df, 0x20ac, 0x03a9, 0x0416, 0x1e0a, 0x2014 })
			PF_CHECK(Tests, EditorTextGlyphRangeContains(Codepoint));
		PF_CHECK(Tests, !EditorTextGlyphRangeContains(0x4e2d));
		PF_CHECK(Tests, EditorFontRoleHasIcons(EditorFontRole::Interface));
		PF_CHECK(Tests, EditorFontRoleHasIcons(EditorFontRole::Emphasis));
		PF_CHECK(Tests, !EditorFontRoleHasIcons(EditorFontRole::Monospace));

		const std::string_view TwoByteText = "\xc4\x9eX";
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 1) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 2) == 2);
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 3) == 3);
		const std::string_view ThreeByteText = "\xe2\x82\xacX";
		PF_CHECK(Tests, Utf8SafePrefixLength(ThreeByteText, 2) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(ThreeByteText, 3) == 3);
		const std::string_view FourByteText = "\xf0\x9f\x98\x80X";
		PF_CHECK(Tests, Utf8SafePrefixLength(FourByteText, 3) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(FourByteText, 4) == 4);

		for (const EditorColorValue& Color : EditorPalette)
		{
			PF_CHECK(Tests, Color.Red >= 0.0f && Color.Red <= 1.0f);
			PF_CHECK(Tests, Color.Green >= 0.0f && Color.Green <= 1.0f);
			PF_CHECK(Tests, Color.Blue >= 0.0f && Color.Blue <= 1.0f);
			PF_CHECK(Tests, Color.Alpha >= 0.0f && Color.Alpha <= 1.0f);
		}
		constexpr std::array<EditorColorToken, 4> GizmoColorTokens = { EditorColorToken::GizmoAxisX, EditorColorToken::GizmoAxisY,
			EditorColorToken::GizmoAxisZ, EditorColorToken::GizmoHighlight };
		for (const EditorColorToken Token : GizmoColorTokens)
		{
			const EditorColorValue Color = GetEditorColorValue(Token);
			const EditorColorValue SrgbOutput = ConvertEditorColorForOutput(Color, PulseForge::OutputColorEncoding::SrgbAttachment);
			const EditorColorValue UnormOutput = ConvertEditorColorForOutput(Color, PulseForge::OutputColorEncoding::UnormAttachment);
			PF_CHECK(Tests, SrgbOutput.Alpha == Color.Alpha);
			PF_CHECK(Tests, UnormOutput.Red == Color.Red && UnormOutput.Green == Color.Green && UnormOutput.Blue == Color.Blue &&
								UnormOutput.Alpha == Color.Alpha);
		}

		const EditorColorValue DisplayColor{ 0.5f, 0.04045f, 1.0f, 0.37f };
		const EditorColorValue LinearColor = ConvertEditorColorForOutput(DisplayColor, PulseForge::OutputColorEncoding::SrgbAttachment);
		PF_CHECK(Tests, std::abs(LinearColor.Red - 0.21404114f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(LinearColor.Green - 0.003130805f) < 1.0e-7f);
		PF_CHECK(Tests, LinearColor.Blue == 1.0f);
		PF_CHECK(Tests, LinearColor.Alpha == DisplayColor.Alpha);
		const EditorColorValue UnormColor = ConvertEditorColorForOutput(DisplayColor, PulseForge::OutputColorEncoding::UnormAttachment);
		PF_CHECK(Tests, UnormColor.Red == DisplayColor.Red && UnormColor.Green == DisplayColor.Green &&
							UnormColor.Blue == DisplayColor.Blue && UnormColor.Alpha == DisplayColor.Alpha);
		PF_CHECK(Tests,
			SelectEditorUiCompositionPath(PulseForge::OutputColorEncoding::SrgbAttachment) == EditorUiCompositionPath::DirectSrgbSwapchain);
		PF_CHECK(Tests, SelectEditorUiCompositionPath(PulseForge::OutputColorEncoding::UnormAttachment) ==
							EditorUiCompositionPath::SrgbIntermediateToUnormSwapchain);
		PF_CHECK(Tests, !SelectEditorUiCompositionPath(static_cast<PulseForge::OutputColorEncoding>(255)));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(false, 1280, 720, 1280, 720));
		PF_CHECK(Tests, IsEditorUiCompositionTargetReusable(true, 1280, 720, 1280, 720));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(true, 1280, 720, 1280, 721));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(true, 1280, 720, 0, 720));
		PF_CHECK(Tests, LinearToSrgbForEditorUi(0.0f) == 0.0f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(1.0f) == 1.0f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(0.5f) - 0.7353569f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(0.0031308f) - 0.0404499f) < 1.0e-6f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(-1.0f) == 0.0f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(2.0f) == 1.0f);

		const float WhiteOverBlackLinear = BlendSourceOverLinear(1.0f, 0.0f, 0.5f);
		PF_CHECK(Tests, WhiteOverBlackLinear == 0.5f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(WhiteOverBlackLinear) - 0.7353569f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(0.5f - LinearToSrgbForEditorUi(WhiteOverBlackLinear)) > 0.2f);
		PF_CHECK(Tests, BlendSourceOverLinear(1.0f, 0.25f, 0.0f) == 0.25f);
		PF_CHECK(Tests, BlendSourceOverLinear(0.25f, 0.75f, 1.0f) == 0.25f);
		PF_CHECK(Tests, std::abs(BlendSourceOverLinear(0.8f, 0.2f, 0.25f) - 0.35f) < 1.0e-6f);
		const float FirstLayer = BlendSourceOverLinear(0.8f, 0.1f, 0.5f);
		const float OrderedLayers = BlendSourceOverLinear(0.2f, FirstLayer, 0.25f);
		PF_CHECK(Tests, std::abs(OrderedLayers - 0.3875f) < 1.0e-6f);

		std::array<bool, static_cast<size_t>(EditorIcon::Count)> SeenIcons{};
		for (size_t Index = 0; Index < EditorIconDescriptors.size(); ++Index)
		{
			const EditorIconDescriptor& Descriptor = EditorIconDescriptors[Index];
			const size_t IconIndex = static_cast<size_t>(Descriptor.Icon);
			PF_CHECK(Tests, IconIndex < SeenIcons.size());
			if (IconIndex < SeenIcons.size())
			{
				PF_CHECK(Tests, !SeenIcons[IconIndex]);
				SeenIcons[IconIndex] = true;
			}
			PF_CHECK(Tests, !Descriptor.Name.empty());
			PF_CHECK(Tests, Descriptor.Codepoint >= 0xe000 && Descriptor.Codepoint <= 0xf8ff);
			PF_CHECK(Tests, EditorIconGlyphRanges[Index * 2] == Descriptor.Codepoint);
			PF_CHECK(Tests, EditorIconGlyphRanges[Index * 2 + 1] == Descriptor.Codepoint);
			if (Index > 0) PF_CHECK(Tests, EditorIconDescriptors[Index - 1].Codepoint < Descriptor.Codepoint);
			PF_CHECK(Tests, FindEditorIcon(Descriptor.Icon) == &Descriptor);
		}
		PF_CHECK(Tests, std::all_of(SeenIcons.begin(), SeenIcons.end(), [](bool Seen) { return Seen; }));
		PF_CHECK(Tests, EditorIconGlyphRanges.back() == 0);
		PF_CHECK(Tests, FindEditorIcon(EditorIcon::Count) == nullptr);
		const EditorIconDescriptor* SelectIcon = FindEditorIcon(EditorIcon::Select);
		PF_CHECK(Tests, SelectIcon && SelectIcon->Codepoint == 0xf245);
	}

} // namespace

void RunEditorTests(TestRunner& Tests)
{
	TestEditorWorkspacePanelState(Tests);
	TestEditorDeferredOperationQueue(Tests);
	TestEditorAssetDestinationPolicy(Tests);
	TestEditorAssetSelectorPolicy(Tests);
	TestEditorLayoutOpeningPreferencePersistence(Tests);
	TestEditorWorkspaceViewHelpers(Tests);
	TestEditorFontAndIconDescriptors(Tests);
	TestEditorImGuiRendererMath(Tests);
	TestEditorPlatformViewportSubmissionOrder(Tests);
}
