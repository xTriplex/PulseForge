#include "TestAssetSupport.h"

#include "Editor/EditorGraphDocumentEditor.h"
#include "Editor/EditorGraphModel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "../PulseForge/Vendor/imgui-node-editor/imgui_node_editor.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace PulseForgeEditor
{
	struct EditorGraphDocumentEditorTestAccess final
	{
		static EditorGraphPropertyEditResult Process(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			const std::vector<EditorGraphPropertyEditEvent>& Events)
		{
			return Editor.ProcessPropertyEditEvents(Session, Events, false);
		}
	};
}

namespace
{
	struct TemporaryGraphProject final
	{
		std::filesystem::path Path;
		~TemporaryGraphProject()
		{
			std::error_code Error;
			if (!Path.empty()) std::filesystem::remove_all(Path, Error);
		}
	};

	struct ImGuiContextScope final
	{
		ImGuiContext* Context = ImGui::CreateContext();
		~ImGuiContextScope() { if (Context) ImGui::DestroyContext(Context); }
	};

	std::optional<std::string> ReadBytes(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::nullopt;
		return std::string(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
	}

	bool WriteBytes(const std::filesystem::path& Path, std::string_view Bytes)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
		Output.flush();
		return static_cast<bool>(Output);
	}

	const PulseForgeEditor::EditorGraphPin* FindPin(const PulseForgeEditor::EditorGraph& Graph,
		std::string_view NodeKey, std::string_view PinKey)
	{
		for (const auto& Node : Graph.Nodes)
			if (Node.TypeKey == NodeKey)
			{
				const auto It = std::find_if(Node.Pins.begin(), Node.Pins.end(), [PinKey](const auto& Pin) { return Pin.Key == PinKey; });
				if (It != Node.Pins.end()) return &*It;
			}
		return nullptr;
	}

	std::optional<PulseForgeEditor::EditorGraph> ReadMaterialFixtureGraph(std::string_view Source)
	{
		auto Graph = PulseForgeEditor::DeserializeEditorGraph(Source, PulseForgeEditor::MakeMaterialExpressionFixtureSchema());
		return Graph ? std::optional<PulseForgeEditor::EditorGraph>(std::move(*Graph)) : std::nullopt;
	}

	void TestGraphDocumentLifecycle(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		TemporaryGraphProject Temporary;
		const auto ID = UUID::Generate();
		PF_CHECK(Tests, ID.has_value());
		if (!ID) return;
		Temporary.Path = std::filesystem::temp_directory_path() / ID->ToString();
		std::error_code Error;
		std::filesystem::create_directories(Temporary.Path, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto Project = PulseForge::Project::Create(Temporary.Path / "GraphFixtureProject.pfproj", "Graph fixture test");
		PF_CHECK(Tests, Project.has_value());
		if (!Project) return;

		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorGraphDocumentEditor GraphEditor(Documents, Editors);
		PF_CHECK(Tests, GraphEditor.Register().has_value());
		const auto Material = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		const auto Gameplay = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::GameplayEvent);
		PF_CHECK(Tests, Material.has_value() && Gameplay.has_value());
		if (!Material || !Gameplay) return;

		const UUID ProjectID = Project->GetDescription().ID;
		const EditorDocumentIdentity MaterialIdentity{ ProjectID, Material->GraphID, EditorDocumentKind::Graph };
		const EditorDocumentIdentity GameplayIdentity{ ProjectID, Gameplay->GraphID, EditorDocumentKind::Graph };
		const auto OpenGraph = [&](const EditorGraphFixtureLocation& Fixture, const EditorDocumentIdentity& Identity)
		{
			auto Opened = Documents.Open(Identity, Fixture.ProjectRelativePath, Fixture.DisplayName,
				EditorDocumentOpenPreference::MainDocumentStrip);
			PF_CHECK(Tests, Opened.has_value());
			if (!Opened) return static_cast<EditorDocumentSession*>(nullptr);
			auto* Session = Documents.Find(Identity);
			const EditorDocumentEditorContext Context{ Identity, Project->GetRootPath(), Fixture.ProjectRelativePath,
				Fixture.DisplayName, nullptr };
			PF_CHECK(Tests, Session && Editors.Load(*Session, Context));
			return Session;
		};
		auto* MaterialSession = OpenGraph(*Material, MaterialIdentity);
		auto* GameplaySession = OpenGraph(*Gameplay, GameplayIdentity);
		PF_CHECK(Tests, MaterialSession && GameplaySession);
		if (!MaterialSession || !GameplaySession) return;
		PF_CHECK(Tests, MaterialSession->EditorState && GameplaySession->EditorState &&
			MaterialSession->EditorState.get() != GameplaySession->EditorState.get());
		PF_CHECK(Tests, !MaterialSession->IsDirty() && !GameplaySession->IsDirty());
		const auto MaterialState = MaterialSession->EditorState.get();
		const uint64_t MaterialGeneration = MaterialSession->SessionGeneration;
		const auto NativeHost = Documents.CreateNativeHost(EditorDocumentKind::Graph);
		PF_CHECK(Tests, NativeHost.has_value());
		if (!NativeHost) return;
		PF_CHECK(Tests, Documents.Transfer(MaterialIdentity, *NativeHost, 0).has_value());
		PF_CHECK(Tests, MaterialSession->EditorState.get() == MaterialState &&
			MaterialSession->SessionGeneration == MaterialGeneration && MaterialSession->HostID == *NativeHost);

		const auto SourcePath = Project->GetRootPath() / Material->ProjectRelativePath;
		const auto OriginalBytes = ReadBytes(SourcePath);
		PF_CHECK(Tests, OriginalBytes.has_value());
		if (OriginalBytes)
		{
			const std::string ExternalBytes = "\n" + *OriginalBytes;
			PF_CHECK(Tests, WriteBytes(SourcePath, ExternalBytes));
			PF_CHECK(Tests, Documents.MarkEdited(MaterialIdentity));
			PF_CHECK(Tests, !GraphEditor.Save(*MaterialSession));
			PF_CHECK(Tests, ReadBytes(SourcePath) == ExternalBytes && MaterialSession->IsDirty());
			PF_CHECK(Tests, Editors.Discard(*MaterialSession));
			PF_CHECK(Tests, !MaterialSession->IsDirty() && ReadBytes(SourcePath) == ExternalBytes);
		}
		PF_CHECK(Tests, GraphEditor.Save(*GameplaySession) && !GameplaySession->IsDirty());
		PF_CHECK(Tests, ReadBytes(Project->GetRootPath() / Gameplay->ProjectRelativePath).has_value());
	}

	void TestGraphDocumentSaveRevisionAccounting(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		TemporaryGraphProject Temporary;
		const auto ProjectID = UUID::Generate();
		PF_CHECK(Tests, ProjectID.has_value());
		if (!ProjectID) return;
		Temporary.Path = std::filesystem::temp_directory_path() / ProjectID->ToString();
		std::error_code Error;
		std::filesystem::create_directories(Temporary.Path, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto Project = PulseForge::Project::Create(Temporary.Path / "GraphSaveProject.pfproj", "Graph save test");
		PF_CHECK(Tests, Project.has_value());
		if (!Project) return;

		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorGraphDocumentEditor GraphEditor(Documents, Editors);
		PF_CHECK(Tests, GraphEditor.Register().has_value());
		const auto Fixture = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		PF_CHECK(Tests, Fixture.has_value());
		if (!Fixture) return;
		const EditorDocumentIdentity Identity{ Project->GetDescription().ID, Fixture->GraphID, EditorDocumentKind::Graph };
		auto Opened = Documents.Open(Identity, Fixture->ProjectRelativePath, Fixture->DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip);
		PF_CHECK(Tests, Opened.has_value());
		if (!Opened) return;
		auto* Session = Documents.Find(Identity);
		const EditorDocumentEditorContext Context{ Identity, Project->GetRootPath(), Fixture->ProjectRelativePath,
			Fixture->DisplayName, nullptr };
		PF_CHECK(Tests, Session && Editors.Load(*Session, Context));
		if (!Session) return;
		const auto SourcePath = Project->GetRootPath() / Fixture->ProjectRelativePath;
		const auto BaselineBytes = ReadBytes(SourcePath);
		PF_CHECK(Tests, BaselineBytes.has_value());
		if (!BaselineBytes) return;
		auto BaselineGraph = ReadMaterialFixtureGraph(*BaselineBytes);
		PF_CHECK(Tests, BaselineGraph.has_value());
		if (!BaselineGraph) return;
		const auto* Surface = FindPin(*BaselineGraph, "material.output", "surface");
		PF_CHECK(Tests, Surface != nullptr);
		if (!Surface) return;
		const UUID SurfaceID = Surface->ID;
		const auto InitialValue = std::get<EditorGraphVector3>(Surface->DefaultValue);
		const auto Process = [&](EditorGraphValue Value, bool Activated, bool Changed, bool Deactivated)
		{
			return EditorGraphDocumentEditorTestAccess::Process(GraphEditor, *Session,
				{ { SurfaceID, std::move(Value), Activated, Changed, Deactivated } });
		};
		const auto SourceValue = [&](const std::optional<std::string>& Bytes)
		{
			const auto Graph = Bytes ? ReadMaterialFixtureGraph(*Bytes) : std::nullopt;
			const auto* Pin = Graph ? FindPin(*Graph, "material.output", "surface") : nullptr;
			return Pin ? std::optional<EditorGraphVector3>(std::get<EditorGraphVector3>(Pin->DefaultValue)) : std::nullopt;
		};

		// Keep one continuous edit active through several content updates, then save via
		// the registered graph adapter and its real file publication callback.
		const EditorGraphVector3 Middle{ 0.6, 0.6, 0.6 };
		const EditorGraphVector3 Final{ 0.7, 0.8, 0.9 };
		auto Edit = Process(Middle, true, true, false);
		PF_CHECK(Tests, Edit.Error.empty() && Edit.GraphChanged && Edit.PropertyInteractionActive && Edit.TransactionOpen);
		Edit = Process(Final, false, true, false);
		PF_CHECK(Tests, Edit.Error.empty() && Edit.GraphChanged && Edit.PropertyInteractionActive && Edit.TransactionOpen);
		PF_CHECK(Tests, Session->IsDirty());
		const uint64_t RevisionBeforeSave = Session->Revision;
		const uint64_t SavedRevisionBeforeSave = Session->SavedRevision;
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Final));
		PF_CHECK(Tests, Session->Revision == RevisionBeforeSave && Session->SavedRevision == RevisionBeforeSave && !Session->IsDirty());
		PF_CHECK(Tests, SavedRevisionBeforeSave < RevisionBeforeSave);
		const auto Finished = EditorGraphDocumentEditorTestAccess::Process(GraphEditor, *Session, {});
		PF_CHECK(Tests, !Finished.PropertyInteractionActive && !Finished.TransactionOpen);

		// One undo restores the pre-interaction value; redo restores the saved value.
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(InitialValue));
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Final));
		PF_CHECK(Tests, !Session->IsDirty());

		// An unchanged active property on a dirty graph must not advance the document
		// revision while Save is finalizing its interaction.
		Edit = Process(Middle, true, true, true);
		PF_CHECK(Tests, Edit.Error.empty() && Edit.GraphChanged && !Edit.TransactionOpen && Session->IsDirty());
		Edit = Process(Middle, true, false, false);
		PF_CHECK(Tests, Edit.PropertyInteractionActive && Edit.TransactionOpen);
		const uint64_t DirtyNoOpRevision = Session->Revision;
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, Session->Revision == DirtyNoOpRevision && !Session->IsDirty());
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Middle));

		// Create a clean graph with a Redo branch; finishing an unchanged interaction
		// during Save must preserve it.
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, !Session->IsDirty());
		Edit = Process(InitialValue, true, false, false);
		PF_CHECK(Tests, Edit.PropertyInteractionActive && Edit.TransactionOpen && !Session->IsDirty());
		const uint64_t CleanNoOpRevision = Session->Revision;
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, Session->Revision == CleanNoOpRevision && !Session->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, Session->IsDirty());
		PF_CHECK(Tests, GraphEditor.Save(*Session));
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Middle));
		PF_CHECK(Tests, !Session->IsDirty());

		int CloseSaveCalls = 0;
		const auto CloseResult = Documents.Close(Identity, EditorDocumentCloseChoice::Save,
			[&](const EditorDocumentSession&, uint64_t) { ++CloseSaveCalls; return true; },
			[](const EditorDocumentSession&) { return false; });
		PF_CHECK(Tests, CloseResult == EditorDocumentCloseStatus::Closed && CloseSaveCalls == 0);

		// A source conflict still rejects Save without losing the committed draft or
		// replacing the adapter's saved-source baseline.
		const auto ConflictFixture = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		PF_CHECK(Tests, ConflictFixture.has_value());
		if (!ConflictFixture) return;
		const EditorDocumentIdentity ConflictIdentity{ Project->GetDescription().ID, ConflictFixture->GraphID, EditorDocumentKind::Graph };
		PF_CHECK(Tests, Documents.Open(ConflictIdentity, ConflictFixture->ProjectRelativePath, ConflictFixture->DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip).has_value());
		auto* ConflictSession = Documents.Find(ConflictIdentity);
		const EditorDocumentEditorContext ConflictContext{ ConflictIdentity, Project->GetRootPath(), ConflictFixture->ProjectRelativePath,
			ConflictFixture->DisplayName, nullptr };
		PF_CHECK(Tests, ConflictSession && Editors.Load(*ConflictSession, ConflictContext));
		if (!ConflictSession) return;
		const auto ConflictPath = Project->GetRootPath() / ConflictFixture->ProjectRelativePath;
		const auto ConflictBaseline = ReadBytes(ConflictPath);
		const auto ConflictGraph = ConflictBaseline ? ReadMaterialFixtureGraph(*ConflictBaseline) : std::nullopt;
		const auto* ConflictSurface = ConflictGraph ? FindPin(*ConflictGraph, "material.output", "surface") : nullptr;
		PF_CHECK(Tests, ConflictSurface != nullptr);
		if (!ConflictSurface) return;
		const auto ConflictSurfaceID = ConflictSurface->ID;
		const auto ConflictSavedRevision = ConflictSession->SavedRevision;
		auto ConflictEdit = EditorGraphDocumentEditorTestAccess::Process(GraphEditor, *ConflictSession,
			{ { ConflictSurfaceID, EditorGraphVector3{ 0.3, 0.4, 0.5 }, true, true, false } });
		PF_CHECK(Tests, ConflictEdit.PropertyInteractionActive && ConflictSession->IsDirty());
		const std::string ExternalBytes = *ConflictBaseline + "\n";
		PF_CHECK(Tests, WriteBytes(ConflictPath, ExternalBytes));
		PF_CHECK(Tests, !GraphEditor.Save(*ConflictSession));
		PF_CHECK(Tests, ReadBytes(ConflictPath) == ExternalBytes && ConflictSession->IsDirty());
		PF_CHECK(Tests, ConflictSession->SavedRevision == ConflictSavedRevision);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*ConflictSession, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, !ConflictSession->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*ConflictSession, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, ConflictSession->IsDirty());
		PF_CHECK(Tests, Editors.Discard(*ConflictSession) && !ConflictSession->IsDirty());
	}

	void TestGraphCanvasCreateRequestPolling(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		const auto GraphID = UUID::Generate();
		PF_CHECK(Tests, GraphID.has_value());
		if (!GraphID) return;
		auto Schema = MakeMaterialExpressionFixtureSchema();
		auto Graph = CreateEditorGraph(Schema, *GraphID);
		PF_CHECK(Tests, Graph.has_value());
		if (!Graph) return;
		auto Scalar = CreateEditorGraphNode(Schema, "material.scalar", 80.0, 80.0);
		auto Multiply = CreateEditorGraphNode(Schema, "material.multiply", 320.0, 80.0);
		auto Output = CreateEditorGraphNode(Schema, "material.output", 560.0, 80.0);
		PF_CHECK(Tests, Scalar.has_value() && Multiply.has_value() && Output.has_value());
		if (!Scalar || !Multiply || !Output) return;
		Graph->Nodes.push_back(std::move(*Scalar));
		Graph->Nodes.push_back(std::move(*Multiply));
		Graph->Nodes.push_back(std::move(*Output));
		const auto* ScalarValue = FindPin(*Graph, "material.scalar", "value");
		const auto* MultiplyA = FindPin(*Graph, "material.multiply", "a");
		const auto* Surface = FindPin(*Graph, "material.output", "surface");
		PF_CHECK(Tests, ScalarValue && MultiplyA && Surface);
		if (!ScalarValue || !MultiplyA || !Surface) return;

		const EditorGraphCanvasLinkRequest Compatible{ 11, 22 };
		const EditorGraphCanvasLinkRequest Incompatible{ 11, 33 };
		const std::unordered_map<uintptr_t, UUID> Identities{
			{ Compatible.StartPin, ScalarValue->ID }, { Compatible.EndPin, MultiplyA->ID },
			{ Incompatible.EndPin, Surface->ID }
		};
		EditorGraphCommandHistory History(std::move(*Graph));
		int BeginCalls = 0;
		int EndCalls = 0;
		int QueryCalls = 0;
		int AcceptCalls = 0;
		int RejectCalls = 0;
		const auto ProcessFrame = [&](bool Active, std::optional<EditorGraphCanvasLinkRequest> Request, bool Accept)
		{
			int FrameQueryCalls = 0;
			return PulseForgeEditor::GraphCanvasDetail::ProcessEditorGraphCanvasLinkRequestFrame(
				[&] { ++BeginCalls; return Active; },
				[&] { ++EndCalls; },
				[&]() -> std::optional<EditorGraphCanvasLinkRequest>
				{
					++QueryCalls;
					return ++FrameQueryCalls == 1 ? Request : std::nullopt;
				},
				[&](const EditorGraphCanvasLinkRequest& Pins)
				{
					const auto Start = Identities.find(Pins.StartPin);
					const auto End = Identities.find(Pins.EndPin);
					if (Start == Identities.end() || End == Identities.end())
						return EditorGraphConnectionDecision{ false, "Pin identity is unavailable", {}, {} };
					return CanConnectEditorGraphPins(History.Graph(), Schema, Start->second, End->second);
				},
				[&] { ++AcceptCalls; return Accept; },
				[&] { ++RejectCalls; });
		};
		const auto CommitAccepted = [&](const EditorGraphCanvasLinkRequestResult& Result)
		{
			if (!Result.Accepted) return false;
			return ConnectEditorGraphPins(History, Schema, Result.Decision.OutputPin, Result.Decision.InputPin).has_value();
		};

		// No pending item: query once and finish the frame, with no graph command.
		auto Result = ProcessFrame(true, std::nullopt, false);
		PF_CHECK(Tests, !Result.SawRequest && !Result.Accepted && QueryCalls == 1 && EndCalls == 1);
		PF_CHECK(Tests, History.Graph().Connections.empty() && !History.CanUndo());

		// The pinned canvas retains a compatible item during preview and AcceptNewItem()
		// returns false until the release stage; each frame may query this same request once.
		Result = ProcessFrame(true, Compatible, false);
		PF_CHECK(Tests, Result.SawRequest && Result.Decision.Allowed && !Result.Accepted);
		PF_CHECK(Tests, QueryCalls == 2 && AcceptCalls == 1 && RejectCalls == 0 && EndCalls == 2);
		PF_CHECK(Tests, History.Graph().Connections.empty() && !History.CanUndo());
		Result = ProcessFrame(true, Compatible, false);
		PF_CHECK(Tests, Result.SawRequest && Result.Decision.Allowed && !Result.Accepted);
		PF_CHECK(Tests, QueryCalls == 3 && AcceptCalls == 2 && EndCalls == 3);
		PF_CHECK(Tests, History.Graph().Connections.empty() && !History.CanUndo());

		// An incompatible target is rejected once for the frame and leaves graph/history intact.
		const EditorGraph BeforeReject = History.Graph();
		Result = ProcessFrame(true, Incompatible, false);
		PF_CHECK(Tests, Result.SawRequest && !Result.Decision.Allowed && !Result.Accepted);
		PF_CHECK(Tests, QueryCalls == 4 && RejectCalls == 1 && AcceptCalls == 2 && EndCalls == 4);
		PF_CHECK(Tests, History.Graph() == BeforeReject && !History.CanUndo());

		// Escape cancels the canvas gesture. BeginCreate can be inactive, but the pinned
		// API still requires EndCreate to balance BeginCreate; no request is queried or committed.
		Result = ProcessFrame(false, Compatible, true);
		PF_CHECK(Tests, !Result.SawRequest && !Result.Accepted && QueryCalls == 4 && EndCalls == 5);
		PF_CHECK(Tests, History.Graph() == BeforeReject && !History.CanUndo());

		// A fresh valid drag reaches the release stage, commits one model command, then
		// later empty frames cannot replay or duplicate that accepted request.
		Result = ProcessFrame(true, Compatible, true);
		PF_CHECK(Tests, Result.SawRequest && Result.Decision.Allowed && Result.Accepted);
		PF_CHECK(Tests, QueryCalls == 5 && AcceptCalls == 3 && EndCalls == 6);
		PF_CHECK(Tests, CommitAccepted(Result));
		PF_CHECK(Tests, History.Graph().Connections.size() == 1 && History.CanUndo());
		Result = ProcessFrame(true, std::nullopt, false);
		PF_CHECK(Tests, !Result.SawRequest && !Result.Accepted && QueryCalls == 6 && EndCalls == 7);
		PF_CHECK(Tests, History.Graph().Connections.size() == 1 && !History.CanRedo());
		PF_CHECK(Tests, History.Undo() && History.Graph().Connections.empty());
		PF_CHECK(Tests, History.Redo() && History.Graph().Connections.size() == 1);
	}

	void TestAltPinUnlinkInputOwnership(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const PulseForge::UUID Pin{ 0x4752415048000000ull, 707 };
		const auto Resolve = [Pin](bool Focused, bool Alt, bool Clicked, bool TextInput,
			bool Popup, bool Embedded, PulseForge::UUID Hovered = PulseForge::UUID{})
		{
			if (Hovered.IsNil() && Focused && Alt && Clicked && !TextInput && !Popup && !Embedded)
				Hovered = Pin;
			return GraphCanvasDetail::ResolveAltPinUnlink(
				{ Hovered, Focused, Alt, Clicked, TextInput, Popup, Embedded });
		};
		PF_CHECK(Tests, Resolve(true, true, true, false, false, false) == Pin);
		PF_CHECK(Tests, !Resolve(false, true, true, false, false, false));
		PF_CHECK(Tests, !Resolve(true, false, true, false, false, false));
		PF_CHECK(Tests, !Resolve(true, true, false, false, false, false));
		PF_CHECK(Tests, !Resolve(true, true, true, true, false, false));
		PF_CHECK(Tests, !Resolve(true, true, true, false, true, false));
		PF_CHECK(Tests, !Resolve(true, true, true, false, false, true));
		PF_CHECK(Tests, !GraphCanvasDetail::ResolveAltPinUnlink(
			{ {}, true, true, true, false, false, false }));
	}

	void TestGraphStraightenShortcutOwnership(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const EditorGraphStraightenInput Available{ true, true, true, false, false, false, false,
			false, false, false, false };
		PF_CHECK(Tests, GraphCanvasDetail::ShouldStraightenSelection(Available));
		const auto Rejected = [&Available](auto Mutate)
		{
			auto Input = Available;
			Mutate(Input);
			return !GraphCanvasDetail::ShouldStraightenSelection(Input);
		};
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.TextInputOwned = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.OtherInputOwned = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.PopupOpen = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.MouseButtonDown = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.NodeMoveTransaction = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.GraphTransaction = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.PropertyEditActive = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.PropertyWidgetHovered = true; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.Focused = false; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.CanvasActive = false; }));
		PF_CHECK(Tests, Rejected([](auto& Input) { Input.KeyPressed = false; }));
	}

	void TestPinnedCanvasLinkGesture(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		namespace NodeEditor = ax::NodeEditor;

		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.LogFilename = nullptr;
		IO.DisplaySize = ImVec2(1280.0f, 720.0f);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

		const auto GraphID = UUID::Generate();
		PF_CHECK(Tests, GraphID.has_value());
		if (!GraphID) return;
		auto Schema = MakeMaterialExpressionFixtureSchema();
		auto Graph = CreateEditorGraph(Schema, *GraphID);
		PF_CHECK(Tests, Graph.has_value());
		if (!Graph) return;
		auto Scalar = CreateEditorGraphNode(Schema, "material.scalar", 80.0, 80.0);
		auto Multiply = CreateEditorGraphNode(Schema, "material.multiply", 360.0, 80.0);
		auto Output = CreateEditorGraphNode(Schema, "material.output", 640.0, 80.0);
		PF_CHECK(Tests, Scalar.has_value() && Multiply.has_value() && Output.has_value());
		if (!Scalar || !Multiply || !Output) return;
		Graph->Nodes.push_back(std::move(*Scalar));
		Graph->Nodes.push_back(std::move(*Multiply));
		Graph->Nodes.push_back(std::move(*Output));
		const auto* ScalarValue = FindPin(*Graph, "material.scalar", "value");
		const auto* MultiplyA = FindPin(*Graph, "material.multiply", "a");
		const auto* Surface = FindPin(*Graph, "material.output", "surface");
		PF_CHECK(Tests, ScalarValue && MultiplyA && Surface);
		if (!ScalarValue || !MultiplyA || !Surface) return;
		const std::unordered_map<uintptr_t, UUID> Identities{
			{ 11, ScalarValue->ID }, { 22, MultiplyA->ID }, { 33, Surface->ID }
		};
		EditorGraphCommandHistory History(std::move(*Graph));
		NodeEditor::Config Config;
		Config.SettingsFile = nullptr;
		auto* Canvas = NodeEditor::CreateEditor(&Config);
		PF_CHECK(Tests, Canvas != nullptr);
		if (!Canvas) return;
		struct CanvasCleanup final
		{
			NodeEditor::EditorContext*& Canvas;
			~CanvasCleanup()
			{
				if (!Canvas) return;
				NodeEditor::SetCurrentEditor(Canvas);
				NodeEditor::DestroyEditor(Canvas);
				Canvas = nullptr;
				NodeEditor::SetCurrentEditor(nullptr);
			}
		} Cleanup{ Canvas };

		std::unordered_map<uintptr_t, ImVec2> PinPositions;
		int FrameIndex = 0;
		bool NeedsNodePositions = true;
		int QueryCalls = 0;
		int QueriesThisFrame = 0;
		int PreviewFrames = 0;
		int RejectedFrames = 0;
		int AcceptedRequests = 0;
		int UnlinkRequests = 0;
		uintptr_t LastHoveredPin = 0;
		const auto Frame = [&](std::optional<ImVec2> MousePosition = std::nullopt,
			std::optional<bool> LeftDown = std::nullopt, bool PressEscape = false,
			std::optional<bool> AltDown = std::nullopt)
		{
			QueriesThisFrame = 0;
			if (MousePosition) IO.AddMousePosEvent(MousePosition->x, MousePosition->y);
			if (LeftDown)
			{
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, *LeftDown);
				IO.MouseDown[ImGuiMouseButton_Left] = *LeftDown;
			}
			if (PressEscape) IO.AddKeyEvent(ImGuiKey_Escape, true);
			if (AltDown) IO.AddKeyEvent(static_cast<ImGuiKey>(ImGuiMod_Alt), *AltDown);
			IO.DisplaySize = ImVec2(1280.0f, 720.0f);
			IO.DeltaTime = 1.0f / 60.0f;
			ImGui::NewFrame();
			if (AltDown) IO.KeyAlt = *AltDown;
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(IO.DisplaySize);
			ImGui::Begin("GraphLinkProtocolHost", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			const bool CapturedAltClick = AltDown.value_or(false) && LeftDown.value_or(false) &&
				LastHoveredPin != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
			if (IO.KeyAlt && IO.MouseDown[ImGuiMouseButton_Left] && LastHoveredPin != 0)
				ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ImGuiKeyOwner_Any, ImGuiInputFlags_LockUntilRelease);
			if (PressEscape)
			{
				NodeEditor::SetCurrentEditor(Canvas);
				NodeEditor::DestroyEditor(Canvas);
				NodeEditor::SetCurrentEditor(nullptr);
				Canvas = NodeEditor::CreateEditor(&Config);
				NeedsNodePositions = true;
			}
			NodeEditor::SetCurrentEditor(Canvas);
			NodeEditor::Begin("##GraphLinkProtocolCanvas", ImVec2(0.0f, 0.0f));
			const auto DrawPinNode = [&](uintptr_t NodeID, uintptr_t PinID, NodeEditor::PinKind Kind,
				const char* Label, float X)
			{
				if (NeedsNodePositions) NodeEditor::SetNodePosition(NodeEditor::NodeId(NodeID), ImVec2(X, 90.0f));
				NodeEditor::BeginNode(NodeEditor::NodeId(NodeID));
				ImGui::TextUnformatted(Label);
				NodeEditor::BeginPin(NodeEditor::PinId(PinID), Kind);
				ImGui::TextUnformatted("Value");
				const ImVec2 ItemMinimum = ImGui::GetItemRectMin();
				const ImVec2 ItemMaximum = ImGui::GetItemRectMax();
				const ImVec2 Center((ItemMinimum.x + ItemMaximum.x) * 0.5f,
					(ItemMinimum.y + ItemMaximum.y) * 0.5f);
				NodeEditor::EndPin();
				PinPositions[PinID] = NodeEditor::CanvasToScreen(Center);
				NodeEditor::EndNode();
			};
			DrawPinNode(1, 11, NodeEditor::PinKind::Output, "Scalar", 80.0f);
			DrawPinNode(2, 22, NodeEditor::PinKind::Input, "Multiply", 360.0f);
			DrawPinNode(3, 33, NodeEditor::PinKind::Input, "Output", 640.0f);

			const auto Result = PulseForgeEditor::GraphCanvasDetail::ProcessEditorGraphCanvasLinkRequestFrame(
				[] { return NodeEditor::BeginCreate(); },
				[] { NodeEditor::EndCreate(); },
				[&]() -> std::optional<EditorGraphCanvasLinkRequest>
				{
					++QueryCalls;
					++QueriesThisFrame;
					NodeEditor::PinId Start;
					NodeEditor::PinId End;
					if (!NodeEditor::QueryNewLink(&Start, &End)) return std::nullopt;
					return EditorGraphCanvasLinkRequest{ Start.Get(), End.Get() };
				},
				[&](const EditorGraphCanvasLinkRequest& Pins)
				{
					const auto Start = Identities.find(Pins.StartPin);
					const auto End = Identities.find(Pins.EndPin);
					if (Start == Identities.end() || End == Identities.end())
						return EditorGraphConnectionDecision{ false, "Pin identity is unavailable", {}, {} };
					return CanConnectEditorGraphPins(History.Graph(), Schema, Start->second, End->second);
				},
				[] { return NodeEditor::AcceptNewItem(); },
				[] { NodeEditor::RejectNewItem(); });
			PF_CHECK(Tests, QueriesThisFrame <= 1);
			if (Result.SawRequest && Result.Decision.Allowed && !Result.Accepted) ++PreviewFrames;
			if (Result.SawRequest && !Result.Decision.Allowed) ++RejectedFrames;
			if (Result.Accepted)
			{
				++AcceptedRequests;
				(void)PulseForgeEditor::ConnectEditorGraphPins(
					History, Schema, Result.Decision.OutputPin, Result.Decision.InputPin);
			}
			NodeEditor::End();
			const auto HoveredPin = NodeEditor::GetHoveredPin();
			LastHoveredPin = HoveredPin.Get();
			const auto HoveredIdentity = Identities.find(LastHoveredPin);
			const UUID HoveredPinID = HoveredIdentity == Identities.end() ? UUID{} : HoveredIdentity->second;
			if (const auto Unlink = GraphCanvasDetail::ResolveAltPinUnlink({ HoveredPinID,
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows), IO.KeyAlt, CapturedAltClick,
				IO.WantTextInput, ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup), false }))
			{
				++UnlinkRequests;
				(void)DisconnectEditorGraphPin(History, Schema, *Unlink);
			}
			NeedsNodePositions = false;
			NodeEditor::SetCurrentEditor(nullptr);
			ImGui::End();
			ImGui::Render();
			if (ImDrawData* DrawData = ImGui::GetDrawData(); DrawData && DrawData->Textures)
				for (ImTextureData* Texture : *DrawData->Textures)
					if (Texture->Status == ImTextureStatus_WantCreate || Texture->Status == ImTextureStatus_WantUpdates)
					{
						Texture->SetTexID(static_cast<ImTextureID>(Texture->UniqueID + 1));
						Texture->SetStatus(ImTextureStatus_OK);
					}
			++FrameIndex;
		};
		const auto BeginDrag = [&](ImVec2 Start, ImVec2 End)
		{
			Frame(Start);
			Frame();
			Frame(std::nullopt, true);
			Frame();
			for (int Step = 1; Step <= 12; ++Step)
			{
				const float T = static_cast<float>(Step) / 12.0f;
				Frame(ImVec2(Start.x + (End.x - Start.x) * T, Start.y + (End.y - Start.y) * T));
			}
			Frame(End);
		};
		const auto ReleaseDrag = [&](ImVec2 At)
		{
			Frame(At, false);
			Frame();
			Frame();
		};
		const auto Drag = [&](ImVec2 Start, ImVec2 End)
		{
			BeginDrag(Start, End);
			Frame(std::nullopt, false);
			Frame();
			Frame();
		};

		for (int Index = 0; Index < 3; ++Index) Frame();
		PF_CHECK(Tests, PinPositions.contains(11) && PinPositions.contains(22) && PinPositions.contains(33));
		if (!PinPositions.contains(11) || !PinPositions.contains(22) || !PinPositions.contains(33)) return;
		const auto ScalarPosition = PinPositions.at(11);
		const auto MultiplyPosition = PinPositions.at(22);
		const auto OutputPosition = PinPositions.at(33);
		BeginDrag(ScalarPosition, MultiplyPosition);
		PF_CHECK(Tests, PreviewFrames > 0 && AcceptedRequests == 0 && History.Graph().Connections.empty());
		Frame(ImVec2(1000.0f, 650.0f));
		ReleaseDrag(ImVec2(1000.0f, 650.0f));
		BeginDrag(ScalarPosition, OutputPosition);
		PF_CHECK(Tests, RejectedFrames > 0 && AcceptedRequests == 0 && History.Graph().Connections.empty());
		Frame(std::nullopt, std::nullopt, true);
		ReleaseDrag(OutputPosition);
		PF_CHECK(Tests, AcceptedRequests == 0 && History.Graph().Connections.empty());

		BeginDrag(ScalarPosition, MultiplyPosition);
		Frame(std::nullopt, std::nullopt, true);
		ReleaseDrag(MultiplyPosition);
		PF_CHECK(Tests, AcceptedRequests == 0 && History.Graph().Connections.empty());

		const int PreviousAccepts = AcceptedRequests;
		Drag(ScalarPosition, MultiplyPosition);
		PF_CHECK(Tests, AcceptedRequests == PreviousAccepts + 1);
		PF_CHECK(Tests, History.Graph().Connections.size() == 1 && History.CanUndo());
		const int CompletedQueries = QueryCalls;
		for (int Index = 0; Index < 5; ++Index) Frame();
		PF_CHECK(Tests, AcceptedRequests == PreviousAccepts + 1 && QueryCalls == CompletedQueries);
		PF_CHECK(Tests, History.Undo() && History.Graph().Connections.empty());
		PF_CHECK(Tests, History.Redo() && History.Graph().Connections.size() == 1);
		const int AcceptedBeforeUnlink = AcceptedRequests;
		Frame(ScalarPosition);
		Frame(ScalarPosition, true, false, true);
		PF_CHECK(Tests, UnlinkRequests == 1 && History.Graph().Connections.empty());
		Frame(ScalarPosition, false, false, false);
		PF_CHECK(Tests, AcceptedRequests == AcceptedBeforeUnlink);
		PF_CHECK(Tests, History.Undo() && History.Graph().Connections.size() == 1);
		PF_CHECK(Tests, History.Redo() && History.Graph().Connections.empty());
		Drag(ScalarPosition, MultiplyPosition);
		PF_CHECK(Tests, AcceptedRequests == AcceptedBeforeUnlink + 1 && History.Graph().Connections.size() == 1);
	}
}

void RunEditorGraphDocumentEditorTests(TestRunner& Tests)
{
	TestGraphDocumentLifecycle(Tests);
	TestGraphDocumentSaveRevisionAccounting(Tests);
	TestGraphCanvasCreateRequestPolling(Tests);
	TestAltPinUnlinkInputOwnership(Tests);
	TestGraphStraightenShortcutOwnership(Tests);
	TestPinnedCanvasLinkGesture(Tests);
}
