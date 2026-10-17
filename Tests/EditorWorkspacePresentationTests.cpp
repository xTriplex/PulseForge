#include "TestEditorSupport.h"

#include "Editor/EditorMainMenuPanel.h"
#include "Editor/EditorWorkspacePresentation.h"

#include <imgui_internal.h>

#include <algorithm>
#include <string>
#include <unordered_set>

namespace
{
	struct FakeViewportWindow final
	{
		ImVec2 Position;
		ImVec2 Size;
	};

	struct FakeViewportBackend final
	{
		FakeViewportWindow MainWindow{ ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f) };
		ImVec2 MainPosition{ 0.0f, 0.0f };
		ImVec2 MainSize{ 1280.0f, 720.0f };
		std::vector<ImGuiID> CreatedPlatformWindows;
		std::vector<ImGuiID> DestroyedPlatformWindows;
		std::vector<ImGuiID> CreatedPresentationTargets;
		std::vector<ImGuiID> DestroyedPresentationTargets;
	};

	FakeViewportBackend* ActiveViewportBackend = nullptr;

	FakeViewportWindow* GetFakeViewportWindow(ImGuiViewport* Viewport)
	{
		return Viewport && Viewport->PlatformUserData ? static_cast<FakeViewportWindow*>(Viewport->PlatformUserData) : nullptr;
	}

	void CreateFakePlatformWindow(ImGuiViewport* Viewport)
	{
		ActiveViewportBackend->CreatedPlatformWindows.push_back(Viewport->ID);
		Viewport->PlatformUserData = new FakeViewportWindow{ Viewport->Pos, Viewport->Size };
		Viewport->PlatformHandle = Viewport->PlatformUserData;
		Viewport->PlatformHandleRaw = Viewport->PlatformUserData;
	}

	void DestroyFakePlatformWindow(ImGuiViewport* Viewport)
	{
		if (Viewport == ImGui::GetMainViewport())
		{
			Viewport->PlatformUserData = nullptr;
			Viewport->PlatformHandle = nullptr;
			Viewport->PlatformHandleRaw = nullptr;
			return;
		}
		if (auto* Window = GetFakeViewportWindow(Viewport))
		{
			ActiveViewportBackend->DestroyedPlatformWindows.push_back(Viewport->ID);
			delete Window;
		}
		Viewport->PlatformUserData = nullptr;
		Viewport->PlatformHandle = nullptr;
		Viewport->PlatformHandleRaw = nullptr;
	}

	void ShowFakePlatformWindow(ImGuiViewport*)
	{
	}

	void SetFakePlatformPosition(ImGuiViewport* Viewport, ImVec2 Position)
	{
		if (auto* Window = GetFakeViewportWindow(Viewport)) Window->Position = Position;
	}

	ImVec2 GetFakePlatformPosition(ImGuiViewport* Viewport)
	{
		if (Viewport == ImGui::GetMainViewport()) return ActiveViewportBackend->MainPosition;
		if (auto* Window = GetFakeViewportWindow(Viewport)) return Window->Position;
		return Viewport->Pos;
	}

	void SetFakePlatformSize(ImGuiViewport* Viewport, ImVec2 Size)
	{
		if (auto* Window = GetFakeViewportWindow(Viewport)) Window->Size = Size;
	}

	ImVec2 GetFakePlatformSize(ImGuiViewport* Viewport)
	{
		if (Viewport == ImGui::GetMainViewport()) return ActiveViewportBackend->MainSize;
		if (auto* Window = GetFakeViewportWindow(Viewport)) return Window->Size;
		return Viewport->Size;
	}

	void SetFakePlatformFocus(ImGuiViewport*)
	{
	}
	void SetFakePlatformTitle(ImGuiViewport*, const char*)
	{
	}
	void SetFakePlatformAlpha(ImGuiViewport*, float)
	{
	}
	void SetFakePlatformImeData(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*)
	{
	}
	bool IsFakePlatformFocused(ImGuiViewport*)
	{
		return false;
	}
	bool IsFakePlatformMinimized(ImGuiViewport*)
	{
		return false;
	}
	float GetFakePlatformDpiScale(ImGuiViewport*)
	{
		return 1.0f;
	}

	void CreateFakePresentationTarget(ImGuiViewport* Viewport)
	{
		ActiveViewportBackend->CreatedPresentationTargets.push_back(Viewport->ID);
		Viewport->RendererUserData = new ImGuiID(Viewport->ID);
	}

	void DestroyFakePresentationTarget(ImGuiViewport* Viewport)
	{
		if (Viewport->RendererUserData)
		{
			ActiveViewportBackend->DestroyedPresentationTargets.push_back(*static_cast<ImGuiID*>(Viewport->RendererUserData));
			delete static_cast<ImGuiID*>(Viewport->RendererUserData);
		}
		Viewport->RendererUserData = nullptr;
	}

	void SetFakePresentationTargetSize(ImGuiViewport*, ImVec2)
	{
	}

	class ScopedWorkspaceImGuiContext final
	{
	public:
		explicit ScopedWorkspaceImGuiContext(FakeViewportBackend& Backend, std::string_view SavedSettings = {})
			: m_PreviousContext(ImGui::GetCurrentContext()), m_Backend(Backend)
		{
			m_Context = ImGui::CreateContext();
			ImGui::SetCurrentContext(m_Context);
			ActiveViewportBackend = &m_Backend;

			ImGuiIO& IO = ImGui::GetIO();
			IO.IniFilename = nullptr;
			IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
			IO.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports;
			IO.DisplaySize = m_Backend.MainSize;
			IO.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			IO.DeltaTime = 1.0f / 60.0f;
			ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			MainViewport->PlatformUserData = &m_Backend.MainWindow;
			MainViewport->PlatformHandle = &m_Backend.MainWindow;
			MainViewport->PlatformHandleRaw = &m_Backend.MainWindow;
			ImGuiPlatformMonitor MainMonitor;
			MainMonitor.MainSize = MainMonitor.WorkSize = m_Backend.MainSize;
			ImGui::GetPlatformIO().Monitors.push_back(MainMonitor);
			unsigned char* Pixels = nullptr;
			int Width = 0;
			int Height = 0;
			IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
			IO.Fonts->SetTexID(static_cast<ImTextureID>(1));

			ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
			PlatformIO.Platform_CreateWindow = &CreateFakePlatformWindow;
			PlatformIO.Platform_DestroyWindow = &DestroyFakePlatformWindow;
			PlatformIO.Platform_ShowWindow = &ShowFakePlatformWindow;
			PlatformIO.Platform_SetWindowPos = &SetFakePlatformPosition;
			PlatformIO.Platform_GetWindowPos = &GetFakePlatformPosition;
			PlatformIO.Platform_SetWindowSize = &SetFakePlatformSize;
			PlatformIO.Platform_GetWindowSize = &GetFakePlatformSize;
			PlatformIO.Platform_SetWindowFocus = &SetFakePlatformFocus;
			PlatformIO.Platform_GetWindowFocus = &IsFakePlatformFocused;
			PlatformIO.Platform_GetWindowMinimized = &IsFakePlatformMinimized;
			PlatformIO.Platform_GetWindowDpiScale = &GetFakePlatformDpiScale;
			PlatformIO.Platform_SetWindowTitle = &SetFakePlatformTitle;
			PlatformIO.Platform_SetWindowAlpha = &SetFakePlatformAlpha;
			PlatformIO.Platform_SetImeDataFn = &SetFakePlatformImeData;
			PlatformIO.Renderer_CreateWindow = &CreateFakePresentationTarget;
			PlatformIO.Renderer_DestroyWindow = &DestroyFakePresentationTarget;
			PlatformIO.Renderer_SetWindowSize = &SetFakePresentationTargetSize;

			if (!SavedSettings.empty()) ImGui::LoadIniSettingsFromMemory(SavedSettings.data(), SavedSettings.size());
		}

		~ScopedWorkspaceImGuiContext()
		{
			if (m_Context)
			{
				ImGui::SetCurrentContext(m_Context);
				ImGui::DestroyPlatformWindows();
				ImGui::DestroyContext(m_Context);
			}
			ActiveViewportBackend = nullptr;
			ImGui::SetCurrentContext(m_PreviousContext);
		}

		ScopedWorkspaceImGuiContext(const ScopedWorkspaceImGuiContext&) = delete;
		ScopedWorkspaceImGuiContext& operator=(const ScopedWorkspaceImGuiContext&) = delete;

	private:
		ImGuiContext* m_PreviousContext = nullptr;
		ImGuiContext* m_Context = nullptr;
		FakeViewportBackend& m_Backend;
	};

	struct WorkspaceFixtureState final : PulseForgeEditor::EditorDocumentEditorState
	{
		int Value = 17;
		uint32_t DrawCount = 0;
	};

	PulseForgeEditor::EditorDocumentEditorAdapter MakeWorkspaceFixtureAdapter()
	{
		using namespace PulseForgeEditor;
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::Texture;
		Adapter.IsEligible = [](const EditorDocumentEditorContext& Context)
		{ return Context.Identity.Kind == EditorDocumentKind::Texture && Context.ProjectRelativePath.extension() == ".fixture"; };
		Adapter.LoadSession =
			[](const EditorDocumentEditorContext&) -> std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
		{ return std::unique_ptr<EditorDocumentEditorState>(std::make_unique<WorkspaceFixtureState>()); };
		Adapter.DrawWorkspace = [](EditorDocumentSession& Session)
		{
			auto& State = static_cast<WorkspaceFixtureState&>(*Session.EditorState);
			++State.DrawCount;
			ImGui::Text("Fixture value: %d", State.Value);
		};
		Adapter.Save = [](EditorDocumentSession&, uint64_t) { return true; };
		Adapter.Discard = [](EditorDocumentSession&) { return true; };
		Adapter.FocusedCommand = [](EditorDocumentSession&, EditorDocumentCommand) { return true; };
		Adapter.AssetLocationChanged = [](EditorDocumentSession&, const std::filesystem::path&, std::string_view) { return true; };
		Adapter.PrepareClose = [](const EditorDocumentSession&) -> std::expected<void, std::string> { return {}; };
		Adapter.Close = [](EditorDocumentSession&) {};
		return Adapter;
	}

	ImGuiID GetWorkspaceDockspaceID(const PulseForgeEditor::EditorDocumentIdentity& Identity)
	{
		const std::string Key = PulseForgeEditor::EditorWorkspaceFrameState::GetWorkspaceKey(Identity);
		return ImHashStr(Key.c_str());
	}

	void DrawNativeDocumentHost(PulseForgeEditor::EditorWorkspacePresentation& Presentation,
		PulseForgeEditor::EditorDocumentManager& Documents, const PulseForgeEditor::EditorDocumentEditorRegistry& Editors,
		std::string_view HostID, float PositionX)
	{
		const auto* Host = Documents.FindHost(HostID);
		if (!Host || !Host->ActiveDocument) return;
		auto* Session = Documents.Find(*Host->ActiveDocument);
		if (!Session) return;

		const std::string HostWindowName = Session->DisplayName + "###PFDocumentHost_" + Host->ID;
		ImGuiWindowClass HostWindowClass;
		HostWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
		ImGui::SetNextWindowClass(&HostWindowClass);
		ImGui::SetNextWindowPos(ImVec2(PositionX, 120.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(560.0f, 440.0f), ImGuiCond_Always);
		if (ImGui::Begin(HostWindowName.c_str(), nullptr, ImGuiWindowFlags_NoDocking)) Presentation.DrawWorkspaceDockspace(*Session);
		ImGui::End();
		Presentation.DrawWorkspaceContent(*Session, Editors, nullptr, nullptr);
	}

	void DrawMainDocumentWorkspace(PulseForgeEditor::EditorWorkspacePresentation& Presentation,
		PulseForgeEditor::EditorDocumentSession& Session, const PulseForgeEditor::EditorDocumentEditorRegistry& Editors)
	{
		const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowViewport(MainViewport->ID);
		ImGui::SetNextWindowPos(MainViewport->WorkPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(MainViewport->WorkSize, ImGuiCond_Always);
		if (ImGui::Begin("##PFMainDocumentWorkspace", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking))
			Presentation.DrawWorkspaceDockspace(Session);
		ImGui::End();
		Presentation.DrawWorkspaceContent(Session, Editors, nullptr, nullptr);
	}

	void BeginWorkspaceFrame(PulseForgeEditor::EditorWorkspacePresentation& Presentation,
		const std::optional<PulseForgeEditor::EditorDocumentIdentity>& ActiveDocument)
	{
		ImGui::NewFrame();
		(void)Presentation.BeginMainFrame(ActiveDocument);
	}

	void FinishWorkspaceFrame(PulseForgeEditor::EditorWorkspacePresentation& Presentation,
		const PulseForgeEditor::EditorDocumentManager& Documents, const PulseForgeEditor::EditorDocumentEditorRegistry& Editors)
	{
		Presentation.KeepInactiveWorkspacesAlive(Documents, Editors);
		ImGui::Render();
		ImGui::UpdatePlatformWindows();
	}

} // namespace

void RunEditorWorkspacePresentationTests(TestRunner& Tests)
{
	using namespace PulseForgeEditor;
	{
		const PulseForge::UUID Project{ 0x1100220033004400ull, 0x5500660077008800ull };
		const PulseForge::UUID OtherProject{ 0x9900aa00bb00cc00ull, 0xdd00ee00ff001100ull };
		const EditorDocumentIdentity Level{ Project, PulseForge::AssetID{ 1, 10 }, EditorDocumentKind::Level };
		const EditorDocumentIdentity Fixture{ Project, PulseForge::AssetID{ 2, 20 }, EditorDocumentKind::Texture };
		const PulseForge::AssetID Material{ 3, 30 };
		const auto LevelTarget = CaptureEditorCommandContext(Level, Project, false, std::nullopt, std::nullopt, Level, true);
		PF_CHECK(Tests, LevelTarget.Kind == EditorCommandContextKind::Level);
		PF_CHECK(Tests, LevelTarget.Document == Level);
		PF_CHECK(Tests, ResolveEditorCommandContext(LevelTarget, Project, Level, true, false, false).has_value());
		PF_CHECK(Tests, !ResolveEditorCommandContext(LevelTarget, Project, Level, false, false, false));

		const auto DetachedTarget = CaptureEditorCommandContext(Fixture, Project, false, std::nullopt, Fixture, Level, true);
		const auto MainStripTarget = CaptureEditorCommandContext(Fixture, Project, false, std::nullopt, Fixture, Level, true);
		PF_CHECK(Tests, DetachedTarget.Kind == EditorCommandContextKind::RegisteredDocument);
		PF_CHECK(Tests, DetachedTarget.Document == Fixture);
		PF_CHECK(Tests, MainStripTarget.Document == Fixture);

		EditorDocumentManager RoutedDocuments;
		EditorDocumentEditorRegistry RoutedEditors;
		std::optional<EditorDocumentIdentity> CommandRecipient;
		EditorDocumentEditorAdapter FixtureAdapter;
		FixtureAdapter.Kind = EditorDocumentKind::Texture;
		FixtureAdapter.IsEligible = [](const EditorDocumentEditorContext& Context)
		{ return Context.ProjectRelativePath.extension() == ".fixture"; };
		FixtureAdapter.LoadSession =
			[](const EditorDocumentEditorContext&) -> std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
		{ return std::unique_ptr<EditorDocumentEditorState>(std::make_unique<WorkspaceFixtureState>()); };
		FixtureAdapter.DrawWorkspace = [](EditorDocumentSession&) {};
		FixtureAdapter.Save = [](EditorDocumentSession&, uint64_t) { return true; };
		FixtureAdapter.Discard = [](EditorDocumentSession&) { return true; };
		FixtureAdapter.FocusedCommand = [&CommandRecipient](EditorDocumentSession& Session, EditorDocumentCommand)
		{
			CommandRecipient = Session.Identity;
			return true;
		};
		FixtureAdapter.AssetLocationChanged = [](EditorDocumentSession&, const std::filesystem::path&, std::string_view) { return true; };
		FixtureAdapter.PrepareClose = [](const EditorDocumentSession&) -> std::expected<void, std::string> { return {}; };
		FixtureAdapter.Close = [](EditorDocumentSession&) {};
		PF_CHECK(Tests, RoutedEditors.Register(std::move(FixtureAdapter)));
		const auto NativeOpen =
			RoutedDocuments.Open(Fixture, "Assets/Fixture.fixture", "Fixture.fixture", EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, NativeOpen && NativeOpen->HostID != EditorDocumentManager::MainHostID);
		if (!NativeOpen) return;
		const EditorDocumentEditorContext FixtureContext{ Fixture, {}, "Assets/Fixture.fixture", "Fixture.fixture", nullptr };
		auto* OpenedSession = RoutedDocuments.Find(Fixture);
		PF_CHECK(Tests, OpenedSession && RoutedEditors.Load(*OpenedSession, FixtureContext));
		PF_CHECK(Tests, RoutedDocuments.Activate(Fixture));
		const auto NativeCommandTarget = CaptureEditorCommandContext(Fixture, Project, false, std::nullopt, Fixture, Level, true);
		const auto* OriginalSession = RoutedDocuments.Find(Fixture);
		PF_CHECK(Tests, OriginalSession && OriginalSession->HostID == NativeOpen->HostID);
		PF_CHECK(Tests, RoutedDocuments.FocusHost(EditorDocumentManager::MainHostID));

		const EditorMainMenuAction Undo = MakeEditorMainMenuAction(EditorMainMenuActionKind::Undo, NativeCommandTarget);
		const auto ResolvedAfterMainMenuTakesFocus = ResolveEditorCommandContext(Undo.CommandTarget, Project, Level, true, true, false);
		PF_CHECK(Tests, ResolvedAfterMainMenuTakesFocus.has_value());
		PF_CHECK(Tests, ResolvedAfterMainMenuTakesFocus && ResolvedAfterMainMenuTakesFocus->Document == Fixture);
		auto* RoutedSession = ResolvedAfterMainMenuTakesFocus && ResolvedAfterMainMenuTakesFocus->Document
								  ? RoutedDocuments.Find(*ResolvedAfterMainMenuTakesFocus->Document)
								  : nullptr;
		PF_CHECK(Tests, RoutedSession == OriginalSession);
		PF_CHECK(Tests, RoutedSession && RoutedSession->HostID == NativeOpen->HostID);
		PF_CHECK(Tests, RoutedSession && RoutedEditors.DispatchFocusedCommand(*RoutedSession, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, CommandRecipient == Fixture);
		const auto ClosedDocument = ResolveEditorCommandContext(Undo.CommandTarget, Project, Level, true, false, false);
		PF_CHECK(Tests, !ClosedDocument);
		const auto ReplacedProject = ResolveEditorCommandContext(Undo.CommandTarget, OtherProject, Level, true, true, false);
		PF_CHECK(Tests, !ReplacedProject);
		const auto MovedToMain = RoutedDocuments.Transfer(Fixture, EditorDocumentManager::MainHostID, 0);
		PF_CHECK(Tests, MovedToMain.has_value());
		const auto MainPlacementTarget = CaptureEditorCommandContext(Fixture, Project, false, std::nullopt, Fixture, Level, true);
		PF_CHECK(Tests, MainPlacementTarget.Document == Fixture);
		PF_CHECK(Tests, RoutedDocuments.Find(Fixture) == OriginalSession);
		PF_CHECK(Tests, RoutedDocuments.Find(Fixture)->HostID == EditorDocumentManager::MainHostID);
		const auto ResolvedMainPlacement = ResolveEditorCommandContext(MainPlacementTarget, Project, Level, true, true, false);
		PF_CHECK(Tests, ResolvedMainPlacement && ResolvedMainPlacement->Document == Fixture);
		PF_CHECK(Tests, RoutedDocuments.Close(Fixture, EditorDocumentCloseChoice::Cancel, {}, {}) == EditorDocumentCloseStatus::Closed);
		PF_CHECK(Tests, !RoutedDocuments.Find(Fixture));
		PF_CHECK(Tests, !ResolveEditorCommandContext(Undo.CommandTarget, Project, Level, true, false, false));

		const auto MaterialDetailsTarget = CaptureEditorCommandContext(Level, Project, true, Material, std::nullopt, Level, true);
		PF_CHECK(Tests, MaterialDetailsTarget.Kind == EditorCommandContextKind::MaterialDetails);
		PF_CHECK(Tests, MaterialDetailsTarget.MaterialAsset == Material);
		const EditorMainMenuAction MaterialUndo = MakeEditorMainMenuAction(EditorMainMenuActionKind::Undo, MaterialDetailsTarget);
		const auto ResolvedMaterial = ResolveEditorCommandContext(MaterialUndo.CommandTarget, Project, Level, true, false, true);
		PF_CHECK(Tests, ResolvedMaterial && ResolvedMaterial->Kind == EditorCommandContextKind::MaterialDetails);
		PF_CHECK(Tests, !ResolveEditorCommandContext(MaterialUndo.CommandTarget, Project, Level, true, false, false));
		PF_CHECK(Tests, CaptureEditorCommandContext(Fixture, Project, false, std::nullopt, std::nullopt, Level, true).Kind ==
							EditorCommandContextKind::Unavailable);
	}

	{
		FakeViewportBackend Backend;
		ScopedWorkspaceImGuiContext Context(Backend);
		EditorMainMenuPanel Panel;
		EditorStyle Style;
		EditorMainMenuState State;
		State.ProjectOpen = true;
		State.EditMenuEnabled = true;
		State.CanEditScene = true;
		State.CanSaveScene = true;
		State.CanSaveSceneAs = true;
		std::array<PulseForge::AssetRecord, 1> Assets{};
		Assets[0].ID = PulseForge::AssetID{ 12, 34 };
		Assets[0].ProjectRelativePath = "Assets/Main.scene";
		State.Assets = Assets;
		State.ActiveSceneAsset = Assets[0].ID;
		ImGui::NewFrame();
		const auto DrawResult = Panel.Draw(State, Style);
		ImGui::Render();
		ImGui::UpdatePlatformWindows();
		PF_CHECK(Tests, !DrawResult.Action);
		PF_CHECK(Tests, ImGui::GetCurrentContext() != nullptr);
		PF_CHECK(Tests, ImGui::GetPlatformIO().Viewports.Size == 1);
		PF_CHECK(Tests, Backend.CreatedPlatformWindows.empty());
		PF_CHECK(Tests, Backend.CreatedPresentationTargets.empty());
	}

	EditorDocumentManager Documents;
	EditorDocumentEditorRegistry Editors;
	PF_CHECK(Tests, Editors.Register(MakeWorkspaceFixtureAdapter()).has_value());

	std::string SavedWorkspaceLayout;
	{
		FakeViewportBackend Backend;
		ScopedWorkspaceImGuiContext Context(Backend);
		EditorWorkspacePresentation Presentation;

		for (int Frame = 0; Frame < 4; ++Frame)
		{
			BeginWorkspaceFrame(Presentation, std::nullopt);
			FinishWorkspaceFrame(Presentation, Documents, Editors);
		}
		PF_CHECK(Tests, ImGui::GetPlatformIO().Viewports.Size == 1);
		PF_CHECK(Tests, Backend.CreatedPlatformWindows.empty());
		PF_CHECK(Tests, Backend.CreatedPresentationTargets.empty());
		PF_CHECK(Tests, ImGui::FindWindowByName("##PFInactiveDocumentWorkspaceKeepAlive") == nullptr);
	}

	const PulseForge::UUID Project{ 0x1020304050607080ull, 0x1020304050607080ull };
	const EditorDocumentIdentity First{ Project, PulseForge::AssetID{ 1, 11 }, EditorDocumentKind::Texture };
	const EditorDocumentIdentity Second{ Project, PulseForge::AssetID{ 2, 22 }, EditorDocumentKind::Texture };
	const auto FirstOpen = Documents.Open(First, "Assets/First.fixture", "First.fixture", EditorDocumentOpenPreference::NewNativeWindow);
	const auto SecondOpen =
		Documents.Open(Second, "Assets/Second.fixture", "Second.fixture", EditorDocumentOpenPreference::NewNativeWindow);
	PF_CHECK(Tests, FirstOpen.has_value());
	PF_CHECK(Tests, SecondOpen.has_value());
	if (!FirstOpen || !SecondOpen) return;

	const auto LoadWorkspace = [&Editors, &Documents](const EditorDocumentIdentity& Identity, std::string_view Path, std::string_view Name)
	{
		auto* Session = Documents.Find(Identity);
		if (!Session) return false;
		EditorDocumentEditorContext Context{ Identity, "Fixture", std::filesystem::path(Path), std::string(Name), nullptr };
		return Editors.Load(*Session, Context);
	};
	PF_CHECK(Tests, LoadWorkspace(First, "Assets/First.fixture", "First.fixture"));
	PF_CHECK(Tests, LoadWorkspace(Second, "Assets/Second.fixture", "Second.fixture"));

	{
		FakeViewportBackend Backend;
		ScopedWorkspaceImGuiContext Context(Backend);
		EditorWorkspacePresentation Presentation;
		for (int Frame = 0; Frame < 3; ++Frame)
		{
			BeginWorkspaceFrame(Presentation, std::nullopt);
			DrawNativeDocumentHost(Presentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
			DrawNativeDocumentHost(Presentation, Documents, Editors, SecondOpen->HostID, 2050.0f);
			FinishWorkspaceFrame(Presentation, Documents, Editors);
		}
		PF_CHECK(Tests, Backend.CreatedPlatformWindows.size() == 2);
		PF_CHECK(Tests, Backend.CreatedPresentationTargets.size() == 2);
		PF_CHECK(Tests, ImGui::GetPlatformIO().Viewports.Size == 3);
		PF_CHECK(
			Tests, std::find(Backend.CreatedPlatformWindows.begin(), Backend.CreatedPlatformWindows.end(),
					   ImHashStr(("First.fixture###PFDocumentHost_" + FirstOpen->HostID).c_str())) != Backend.CreatedPlatformWindows.end());
		PF_CHECK(Tests,
			std::find(Backend.CreatedPlatformWindows.begin(), Backend.CreatedPlatformWindows.end(),
				ImHashStr(("Second.fixture###PFDocumentHost_" + SecondOpen->HostID).c_str())) != Backend.CreatedPlatformWindows.end());
		PF_CHECK(Tests, ImGui::FindWindowByName("##PFInactiveDocumentWorkspaceKeepAlive") == nullptr);
		PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(First)) != nullptr);
		PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(Second)) != nullptr);

		size_t SettingsSize = 0;
		const char* SettingsData = ImGui::SaveIniSettingsToMemory(&SettingsSize);
		if (SettingsData) SavedWorkspaceLayout.assign(SettingsData, SettingsSize);
		PF_CHECK(Tests, SavedWorkspaceLayout.find("PFDocumentWorkspace_") != std::string::npos);
		PF_CHECK(Tests, SavedWorkspaceLayout.find("PFInactiveDocumentWorkspaceKeepAlive") == std::string::npos);
	}

	FakeViewportBackend RestoredBackend;
	ScopedWorkspaceImGuiContext RestoredContext(RestoredBackend, SavedWorkspaceLayout);
	EditorWorkspacePresentation RestoredPresentation;
	for (int Frame = 0; Frame < 3; ++Frame)
	{
		BeginWorkspaceFrame(RestoredPresentation, std::nullopt);
		DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
		DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, SecondOpen->HostID, 2050.0f);
		FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
	}
	PF_CHECK(Tests, RestoredBackend.CreatedPlatformWindows.size() == 2);
	PF_CHECK(Tests, RestoredBackend.CreatedPresentationTargets.size() == 2);
	PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(First)) != nullptr);
	PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(Second)) != nullptr);

	const auto* FirstSessionBeforeTransfer = Documents.Find(First);
	const auto* SecondSessionBeforeTransfer = Documents.Find(Second);
	auto* FirstState = FirstSessionBeforeTransfer ? FirstSessionBeforeTransfer->EditorState.get() : nullptr;
	auto* SecondState = SecondSessionBeforeTransfer ? SecondSessionBeforeTransfer->EditorState.get() : nullptr;
	PF_CHECK(Tests, FirstState != nullptr && SecondState != nullptr);
	PF_CHECK(Tests, Documents.MarkEdited(Second));
	const uint64_t SecondRevision = Documents.Find(Second)->Revision;
	const auto TransferIntoFirstHost = Documents.Transfer(Second, FirstOpen->HostID, 1);
	PF_CHECK(Tests, TransferIntoFirstHost.has_value());
	PF_CHECK(Tests, Documents.RemoveEmptyNativeHost(SecondOpen->HostID));
	PF_CHECK(Tests, Documents.Find(First)->EditorState.get() == FirstState);
	PF_CHECK(Tests, Documents.Find(Second)->EditorState.get() == SecondState);
	PF_CHECK(Tests, Documents.Find(Second)->Revision == SecondRevision);

	for (int Frame = 0; Frame < 3; ++Frame)
	{
		BeginWorkspaceFrame(RestoredPresentation, std::nullopt);
		DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
		FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
	}
	ImGuiWindow* KeepAlive = ImGui::FindWindowByName("##PFInactiveDocumentWorkspaceKeepAlive");
	PF_CHECK(Tests, KeepAlive != nullptr);
	if (KeepAlive)
	{
		PF_CHECK(Tests, KeepAlive->Viewport == ImGui::GetMainViewport());
		PF_CHECK(Tests, KeepAlive->ViewportId == ImGui::GetMainViewport()->ID);
		PF_CHECK(Tests, KeepAlive->ID == ImHashStr("##PFInactiveDocumentWorkspaceKeepAlive"));
		PF_CHECK(Tests, !KeepAlive->ViewportOwned);
		const ImGuiWindowFlags RequiredFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
											   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
											   ImGuiWindowFlags_NoBackground;
		PF_CHECK(Tests, (KeepAlive->Flags & RequiredFlags) == RequiredFlags);
		PF_CHECK(Tests, std::find(RestoredBackend.CreatedPlatformWindows.begin(), RestoredBackend.CreatedPlatformWindows.end(),
							KeepAlive->ID) == RestoredBackend.CreatedPlatformWindows.end());
		PF_CHECK(Tests, std::find(RestoredBackend.CreatedPresentationTargets.begin(), RestoredBackend.CreatedPresentationTargets.end(),
							KeepAlive->ID) == RestoredBackend.CreatedPresentationTargets.end());
	}
	PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(First)) != nullptr);
	PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(Second)) != nullptr);
	PF_CHECK(Tests, RestoredBackend.CreatedPlatformWindows.size() == 2);
	PF_CHECK(Tests, RestoredBackend.CreatedPresentationTargets.size() == 2);
	PF_CHECK(Tests, RestoredBackend.DestroyedPlatformWindows.size() == 1);
	PF_CHECK(Tests, RestoredBackend.DestroyedPresentationTargets.size() == 1);

	for (const EditorDocumentIdentity& Active : { First, Second, First, Second })
	{
		PF_CHECK(Tests, Documents.Activate(Active));
		BeginWorkspaceFrame(RestoredPresentation, Active);
		PF_CHECK(Tests, !RestoredPresentation.IsLevelWorkspaceActive());
		DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
		FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
		PF_CHECK(Tests, Documents.Find(First)->EditorState.get() == FirstState);
		PF_CHECK(Tests, Documents.Find(Second)->EditorState.get() == SecondState);
		PF_CHECK(Tests, Documents.Find(Second)->Revision == SecondRevision);
		PF_CHECK(Tests, ImGui::GetPlatformIO().Viewports.Size == 2);
	}
	BeginWorkspaceFrame(RestoredPresentation, std::nullopt);
	PF_CHECK(Tests, RestoredPresentation.IsLevelWorkspaceActive());
	DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
	FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
	PF_CHECK(Tests, Documents.Find(First)->EditorState.get() == FirstState);
	PF_CHECK(Tests, Documents.Find(Second)->EditorState.get() == SecondState);
	PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(First)) != nullptr);
	PF_CHECK(Tests, ImGui::DockBuilderGetNode(GetWorkspaceDockspaceID(Second)) != nullptr);

	const auto ReturnToMain = Documents.Transfer(Second, EditorDocumentManager::MainHostID, 0);
	PF_CHECK(Tests, ReturnToMain.has_value());
	BeginWorkspaceFrame(RestoredPresentation, Second);
	PF_CHECK(Tests, !RestoredPresentation.IsLevelWorkspaceActive());
	DrawMainDocumentWorkspace(RestoredPresentation, *Documents.Find(Second), Editors);
	DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
	FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
	PF_CHECK(Tests, ImGui::GetPlatformIO().Viewports.Size == 2);
	PF_CHECK(Tests, Documents.Find(Second)->EditorState.get() == SecondState);
	PF_CHECK(Tests, Documents.Find(Second)->Revision == SecondRevision);

	const auto ReturnToNative = Documents.Transfer(Second, FirstOpen->HostID, 1);
	PF_CHECK(Tests, ReturnToNative.has_value());
	BeginWorkspaceFrame(RestoredPresentation, std::nullopt);
	PF_CHECK(Tests, RestoredPresentation.IsLevelWorkspaceActive());
	DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
	FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
	PF_CHECK(Tests, Documents.Find(Second)->EditorState.get() == SecondState);
	PF_CHECK(Tests, Documents.Find(Second)->Revision == SecondRevision);

	const auto ReturnToMainAgain = Documents.Transfer(Second, EditorDocumentManager::MainHostID, 0);
	PF_CHECK(Tests, ReturnToMainAgain.has_value());
	BeginWorkspaceFrame(RestoredPresentation, Second);
	PF_CHECK(Tests, !RestoredPresentation.IsLevelWorkspaceActive());
	DrawMainDocumentWorkspace(RestoredPresentation, *Documents.Find(Second), Editors);
	DrawNativeDocumentHost(RestoredPresentation, Documents, Editors, FirstOpen->HostID, 1400.0f);
	FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
	PF_CHECK(Tests, Documents.Find(Second)->EditorState.get() == SecondState);
	PF_CHECK(Tests, Documents.Find(Second)->Revision == SecondRevision);

	const auto CloseSecond = Documents.Close(
		Second, EditorDocumentCloseChoice::Save, [](const EditorDocumentSession&, uint64_t) { return true; },
		[](const EditorDocumentSession&) { return true; });
	PF_CHECK(Tests, CloseSecond == EditorDocumentCloseStatus::Closed);
	const auto CloseFirst = Documents.Close(
		First, EditorDocumentCloseChoice::Save, [](const EditorDocumentSession&, uint64_t) { return true; },
		[](const EditorDocumentSession&) { return true; });
	PF_CHECK(Tests, CloseFirst == EditorDocumentCloseStatus::Closed);
	PF_CHECK(Tests, Documents.RemoveEmptyNativeHost(FirstOpen->HostID));
	for (int Frame = 0; Frame < 4; ++Frame)
	{
		BeginWorkspaceFrame(RestoredPresentation, std::nullopt);
		FinishWorkspaceFrame(RestoredPresentation, Documents, Editors);
	}
	PF_CHECK(Tests, ImGui::GetPlatformIO().Viewports.Size == 1);
	PF_CHECK(Tests, RestoredBackend.CreatedPlatformWindows.size() == RestoredBackend.DestroyedPlatformWindows.size());
	PF_CHECK(Tests, RestoredBackend.CreatedPresentationTargets.size() == RestoredBackend.DestroyedPresentationTargets.size());
	if (ImGuiWindow* InactiveWindow = ImGui::FindWindowByName("##PFInactiveDocumentWorkspaceKeepAlive"))
		PF_CHECK(Tests, InactiveWindow->LastFrameActive < ImGui::GetFrameCount());
}
