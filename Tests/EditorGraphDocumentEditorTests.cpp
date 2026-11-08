#include "TestAssetSupport.h"

#include "Editor/EditorGraphDocumentEditor.h"
#include "Editor/EditorGraphModel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "../PulseForge/Vendor/imgui-node-editor/imgui_node_editor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <utility>

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

	void TestGraphCanvasNodeCreationRequestPolling(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		int BeginCalls = 0;
		int EndCalls = 0;
		int LinkQueries = 0;
		int NodeQueries = 0;
		int LinkAccepts = 0;
		int LinkRejects = 0;
		int NodeAccepts = 0;
		int NodeRejects = 0;
		bool LinkPending = false;
		bool NodePending = false;
		bool SourceAvailable = true;
		const auto Frame = [&](bool Active)
		{
			return GraphCanvasDetail::ProcessEditorGraphCanvasCreateRequestFrame(
				[&] { ++BeginCalls; return Active; },
				[&] { ++EndCalls; },
				[&]() -> std::optional<EditorGraphCanvasLinkRequest>
				{
					++LinkQueries;
					if (!std::exchange(LinkPending, false)) return std::nullopt;
					return EditorGraphCanvasLinkRequest{ 1, 2 };
				},
				[](const EditorGraphCanvasLinkRequest&) { return EditorGraphConnectionDecision{ false,
					"incompatible", {}, {} }; },
				[&] { ++LinkAccepts; return true; },
				[&] { ++LinkRejects; },
				[&]() -> std::optional<uintptr_t>
				{
					++NodeQueries;
					if (!std::exchange(NodePending, false)) return std::nullopt;
					return 77;
				},
				[&](uintptr_t Source) { return Source == 77 && SourceAvailable; },
				[&] { ++NodeAccepts; return false; },
				[&] { ++NodeRejects; });
		};

		const auto Empty = Frame(true);
		PF_CHECK(Tests, !Empty.SawLinkRequest && !Empty.SawNodeRequest && LinkQueries == 1 && NodeQueries == 1);
		NodePending = true;
		const auto Preview = Frame(true);
		PF_CHECK(Tests, Preview.SawNodeRequest && !Preview.AcceptedNode && Preview.SourcePin == 77 && NodeAccepts == 1);
		PF_CHECK(Tests, EndCalls == 2 && LinkQueries == 2 && NodeQueries == 2);
		SourceAvailable = false;
		NodePending = true;
		const auto InvalidSource = Frame(true);
		PF_CHECK(Tests, InvalidSource.SawNodeRequest && !InvalidSource.AcceptedNode && NodeRejects == 1);
		SourceAvailable = true;
		NodePending = true;
		const auto Accepted = GraphCanvasDetail::ProcessEditorGraphCanvasCreateRequestFrame(
			[&] { ++BeginCalls; return true; }, [&] { ++EndCalls; },
			[]() -> std::optional<EditorGraphCanvasLinkRequest> { return std::nullopt; },
			[](const auto&) { return EditorGraphConnectionDecision{}; },
			[] { return false; }, [] {},
			[&]() -> std::optional<uintptr_t> { ++NodeQueries; return std::exchange(NodePending, false) ? std::optional<uintptr_t>{77} : std::nullopt; },
			[&](uintptr_t Source) { return Source == 77 && SourceAvailable; },
			[&] { ++NodeAccepts; return true; }, [&] { ++NodeRejects; });
		PF_CHECK(Tests, Accepted.SawNodeRequest && Accepted.AcceptedNode && Accepted.SourcePin == 77 &&
			NodeAccepts == 2 && EndCalls == 4);
		const int AcceptedBeforeEmptyFrames = NodeAccepts;
		PF_CHECK(Tests, !Frame(true).SawNodeRequest && NodeAccepts == AcceptedBeforeEmptyFrames);
		LinkPending = true;
		const auto RejectedLink = Frame(true);
		PF_CHECK(Tests, RejectedLink.SawLinkRequest && !RejectedLink.LinkDecision.Allowed &&
			LinkRejects == 1 && !RejectedLink.SawNodeRequest);
		NodePending = true;
		const auto Cancelled = Frame(false);
		PF_CHECK(Tests, !Cancelled.SawNodeRequest && NodeRejects == 1 && EndCalls == BeginCalls);
		NodePending = true;
		const auto Fresh = Frame(true);
		PF_CHECK(Tests, Fresh.SawNodeRequest && !Fresh.AcceptedNode && NodeAccepts == 3 && EndCalls == BeginCalls);
		PF_CHECK(Tests, LinkAccepts == 0 && BeginCalls == 8);
	}

	void TestGraphNodeSearchNavigation(TestRunner& Tests)
	{
		using namespace PulseForgeEditor::GraphCanvasDetail;
		const auto Wrapped = ResolveNodeSearchNavigation({ 3, 2, false, false, true, false, false });
		PF_CHECK(Tests, Wrapped.SelectedResult == 0 && !Wrapped.ActivateResult && !Wrapped.Cancelled);
		const auto Filtered = ResolveNodeSearchNavigation({ 1, 2, true, false, false, true, false });
		PF_CHECK(Tests, Filtered.SelectedResult == 0 && Filtered.ActivateResult == 0);
		const auto EmptyEnter = ResolveNodeSearchNavigation({ 0, 1, false, false, false, true, false });
		PF_CHECK(Tests, EmptyEnter.SelectedResult == 0 && !EmptyEnter.ActivateResult && !EmptyEnter.Cancelled);
		const auto Cancel = ResolveNodeSearchNavigation({ 2, 1, false, false, false, true, true });
		PF_CHECK(Tests, Cancel.Cancelled && !Cancel.ActivateResult);
		const auto CancelEmpty = ResolveNodeSearchNavigation({ 0, 0, false, false, false, true, true });
		PF_CHECK(Tests, CancelEmpty.Cancelled && !CancelEmpty.ActivateResult);
		const auto FreshResults = ResolveNodeSearchNavigation({ 2, 0, false, false, false, true, false });
		PF_CHECK(Tests, FreshResults.ActivateResult == 0 && !FreshResults.Cancelled);
	}

	void TestGraphCanvasCoordinateOwnership(TestRunner& Tests)
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
		std::string CanvasSettings = "{\"view\":{\"scroll\":{\"x\":37.0,\"y\":-21.0},\"zoom\":0.65}}";
		NodeEditor::Config Config;
		Config.SettingsFile = nullptr;
		Config.UserPointer = &CanvasSettings;
		Config.LoadSettings = [](char* Data, void* UserPointer) -> size_t
		{
			const auto& Settings = *static_cast<const std::string*>(UserPointer);
			if (!Data) return Settings.size();
			std::copy(Settings.begin(), Settings.end(), Data);
			return Settings.size();
		};
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
		const auto RecreateCanvas = [&](std::string Settings)
		{
			CanvasSettings = std::move(Settings);
			NodeEditor::SetCurrentEditor(Canvas);
			NodeEditor::DestroyEditor(Canvas);
			NodeEditor::SetCurrentEditor(nullptr);
			Canvas = NodeEditor::CreateEditor(&Config);
			PF_CHECK(Tests, Canvas != nullptr);
		};

		const auto GraphID = UUID::Generate();
		PF_CHECK(Tests, GraphID.has_value());
		if (!GraphID) return;
		const EditorGraphCanvasDocumentContext Context{ {}, 9, "coordinate-test-host", *GraphID };
		const ImVec2 RawScreenPoint(412.0f, 438.0f);
		std::optional<EditorGraphPendingPinRelease> Pending;
		for (size_t Index = 0; Index < 2; ++Index)
		{
			IO.AddMousePosEvent(RawScreenPoint.x, RawScreenPoint.y);
			if (Index == 0) IO.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
			else IO.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(96.0f, 64.0f));
			ImGui::SetNextWindowSize(IO.DisplaySize);
			ImGui::Begin("GraphCanvasCoordinateHost", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			NodeEditor::SetCurrentEditor(Canvas);
			const EditorGraphScreenPoint ScreenPoint{ IO.MousePos.x, IO.MousePos.y };
			const EditorGraphScreenPoint ScreenClickPoint{ IO.MouseClickedPos[ImGuiMouseButton_Right].x,
				IO.MouseClickedPos[ImGuiMouseButton_Right].y };
			NodeEditor::Begin("##CoordinateCanvas", ImVec2(0.0f, 0.0f));
			const float ExpectedScale = Index == 0 ? 0.65f : 1.45f;
			PF_CHECK(Tests, std::abs(1.0f / NodeEditor::GetCurrentZoom() - ExpectedScale) < 0.01f);

			const ImVec2 LocalMouse = IO.MousePos;
			const auto ConvertedMouse = GraphCanvasDetail::ConvertEditorGraphScreenPoint(ScreenPoint,
				[](EditorGraphScreenPoint Point)
				{
					const ImVec2 CanvasPoint = NodeEditor::ScreenToCanvas(
						ImVec2(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
					return GraphCanvasDetail::MakeEditorGraphCanvasPoint(CanvasPoint.x, CanvasPoint.y);
				});
			PF_CHECK(Tests, std::abs(ConvertedMouse.X - LocalMouse.x) < 0.01 &&
				std::abs(ConvertedMouse.Y - LocalMouse.y) < 0.01);

			const ImVec2 LocalClick = IO.MouseClickedPos[ImGuiMouseButton_Right];
			const auto ClickedCanvasPoint = GraphCanvasDetail::MakeEditorGraphCanvasPoint(LocalClick.x, LocalClick.y);
			const auto ConvertedClick = GraphCanvasDetail::ConvertEditorGraphScreenPoint(ScreenClickPoint,
				[](EditorGraphScreenPoint Point)
				{
					const ImVec2 CanvasPoint = NodeEditor::ScreenToCanvas(
						ImVec2(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
					return GraphCanvasDetail::MakeEditorGraphCanvasPoint(CanvasPoint.x, CanvasPoint.y);
				});
			PF_CHECK(Tests, std::abs(ClickedCanvasPoint.X - ConvertedClick.X) < 0.01 &&
				std::abs(ClickedCanvasPoint.Y - ConvertedClick.Y) < 0.01);

			const auto OldDoubleConverted = GraphCanvasDetail::ConvertEditorGraphScreenPoint(
				{ LocalMouse.x, LocalMouse.y }, [](EditorGraphScreenPoint Point)
				{
					const ImVec2 CanvasPoint = NodeEditor::ScreenToCanvas(
						ImVec2(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
					return GraphCanvasDetail::MakeEditorGraphCanvasPoint(CanvasPoint.x, CanvasPoint.y);
				});
			PF_CHECK(Tests, std::abs(OldDoubleConverted.X - ConvertedMouse.X) > 1.0 ||
				std::abs(OldDoubleConverted.Y - ConvertedMouse.Y) > 1.0);

			NodeEditor::BeginNode(NodeEditor::NodeId(static_cast<uintptr_t>(800 + Index)));
			ImGui::TextUnformatted("Pin geometry");
			const auto SubmitMarker = [](uintptr_t PinID, NodeEditor::PinKind Kind, ImVec2 Cursor)
			{
				ImGui::SetCursorPos(Cursor);
				NodeEditor::BeginPin(NodeEditor::PinId(PinID), Kind);
				ImGui::Dummy(ImVec2(14.0f, 14.0f));
				const ImVec2 Minimum = ImGui::GetItemRectMin();
				const ImVec2 Maximum = ImGui::GetItemRectMax();
				const ImVec2 Center((Minimum.x + Maximum.x) * 0.5f, (Minimum.y + Maximum.y) * 0.5f);
				const auto Geometry = GraphCanvasDetail::MakePinMarkerGeometry(
					GraphCanvasDetail::MakeEditorGraphCanvasPoint(Center.x, Center.y), 14.0, 14.0);
				GraphCanvasDetail::ApplyPinMarkerGeometry(Geometry,
					[](EditorGraphCanvasPoint Min, EditorGraphCanvasPoint Max)
					{
						NodeEditor::PinRect(ImVec2(static_cast<float>(Min.X), static_cast<float>(Min.Y)),
							ImVec2(static_cast<float>(Max.X), static_cast<float>(Max.Y)));
					}, [](EditorGraphCanvasPoint Anchor)
					{
						const ImVec2 Point(static_cast<float>(Anchor.X), static_cast<float>(Anchor.Y));
						NodeEditor::PinPivotRect(Point, Point);
					});
				NodeEditor::EndPin();
				return Geometry;
			};
			const auto InputGeometry = SubmitMarker(901 + Index, NodeEditor::PinKind::Input, ImVec2(20.0f, 54.0f));
			const auto OutputGeometry = SubmitMarker(902 + Index, NodeEditor::PinKind::Output, ImVec2(132.0f, 54.0f));
			ImGui::SetCursorPos(ImVec2(40.0f, 80.0f));
			ImGui::SetNextItemWidth(100.0f);
		double PropertyValue = 1.0;
		ImGui::InputDouble("##PinGeometryProperty", &PropertyValue);
		const ImVec2 PropertyMinimum = ImGui::GetItemRectMin();
		const ImVec2 PropertyMaximum = ImGui::GetItemRectMax();
		const ImVec2 InputScreen = NodeEditor::CanvasToScreen(ImVec2(
			static_cast<float>(InputGeometry.Anchor.X), static_cast<float>(InputGeometry.Anchor.Y)));
		const ImVec2 OutputScreen = NodeEditor::CanvasToScreen(ImVec2(
			static_cast<float>(OutputGeometry.Anchor.X), static_cast<float>(OutputGeometry.Anchor.Y)));
		NodeEditor::EndNode();
		PF_CHECK(Tests, std::abs(InputGeometry.Maximum.X - InputGeometry.Minimum.X - 14.0) < 0.01 &&
			std::abs(InputGeometry.Maximum.Y - InputGeometry.Minimum.Y - 14.0) < 0.01);
		PF_CHECK(Tests, InputGeometry.Maximum.X < OutputGeometry.Minimum.X && OutputScreen.x > InputScreen.x);
		PF_CHECK(Tests, PropertyMaximum.y <= InputGeometry.Minimum.Y ||
			PropertyMinimum.y >= InputGeometry.Maximum.Y);

			const UUID SourcePin{ 0x43414E5641530000ull, Index + 1 };
			Pending = EditorGraphPendingPinRelease{ true, Context, SourcePin, ConvertedMouse };
			NodeEditor::End();
			NodeEditor::SetCurrentEditor(nullptr);
			ImGui::End();
			ImGui::Render();
			if (Index == 0)
			{
				// The acceptance frame changes both pointer and view. The captured graph point remains fixed.
				RecreateCanvas("{\"view\":{\"scroll\":{\"x\":-80.0,\"y\":56.0},\"zoom\":1.8}}");
				IO.AddMousePosEvent(900.0f, 650.0f);
				ImGui::NewFrame();
				ImGui::SetNextWindowPos(ImVec2(96.0f, 64.0f));
				ImGui::SetNextWindowSize(IO.DisplaySize);
				ImGui::Begin("GraphCanvasCoordinateAcceptHost", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
				NodeEditor::SetCurrentEditor(Canvas);
				NodeEditor::Begin("##CoordinateCanvas", ImVec2(0.0f, 0.0f));
				PF_CHECK(Tests, std::abs(1.0f / NodeEditor::GetCurrentZoom() - 1.8f) < 0.01f);
				const auto Accepted = GraphCanvasDetail::ConsumePinRelease(*Pending, Context, SourcePin);
				PF_CHECK(Tests, Accepted && Accepted->SourcePin == SourcePin &&
					std::abs(Accepted->Position.X - ConvertedMouse.X) < 0.01 &&
					std::abs(Accepted->Position.Y - ConvertedMouse.Y) < 0.01);
				PF_CHECK(Tests, !Pending->Valid && !GraphCanvasDetail::ConsumePinRelease(*Pending, Context, SourcePin));
				EditorGraphPendingPinRelease WrongContext{ true, Context, SourcePin, ConvertedMouse };
				auto ReplacedContext = Context;
				++ReplacedContext.SessionGeneration;
				PF_CHECK(Tests, !GraphCanvasDetail::ConsumePinRelease(WrongContext, ReplacedContext, SourcePin) &&
					!WrongContext.Valid);
				EditorGraphPendingPinRelease WrongSource{ true, Context,
					UUID{ 0x43414E5641530000ull, 99 }, ConvertedMouse };
				PF_CHECK(Tests, !GraphCanvasDetail::ConsumePinRelease(WrongSource, Context, SourcePin) &&
					!WrongSource.Valid);
				NodeEditor::End();
				NodeEditor::SetCurrentEditor(nullptr);
				ImGui::End();
				ImGui::Render();
				RecreateCanvas("{\"view\":{\"scroll\":{\"x\":-24.0,\"y\":83.0},\"zoom\":1.45}}");
			}
		}
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
		int NodeQueryCalls = 0;
		int NodeQueriesThisFrame = 0;
		int PreviewFrames = 0;
		int RejectedFrames = 0;
		int AcceptedRequests = 0;
		int AcceptedNodeRequests = 0;
		int BackgroundMenuRequests = 0;
		int UnlinkRequests = 0;
		uintptr_t LastHoveredPin = 0;
		std::optional<EditorGraphVector3> AcceptedNodeDrop;
		std::optional<EditorGraphVector3> CreatedCanvasNodePosition;
		std::optional<EditorGraphVector3> CreatedCanvasNodeReadback;
		const EditorGraphCanvasDocumentContext CanvasContext{ {}, 17, "graph-link-protocol-host", *GraphID };
		EditorGraphPendingPinRelease PendingPinRelease;
		std::optional<EditorGraphScreenPoint> ReleaseScreenPosition;
		std::optional<EditorGraphCanvasPoint> BackgroundCanvasPosition;
		std::optional<EditorGraphCanvasPoint> ExpectedBackgroundCanvasPosition;
		std::optional<EditorGraphCanvasPoint> CapturedReleasePosition;
		const auto Frame = [&](std::optional<ImVec2> MousePosition = std::nullopt,
			std::optional<bool> LeftDown = std::nullopt, bool PressEscape = false,
			std::optional<bool> AltDown = std::nullopt, std::optional<bool> RightDown = std::nullopt)
		{
			QueriesThisFrame = 0;
			NodeQueriesThisFrame = 0;
			if (MousePosition) IO.AddMousePosEvent(MousePosition->x, MousePosition->y);
			if (LeftDown)
			{
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, *LeftDown);
				IO.MouseDown[ImGuiMouseButton_Left] = *LeftDown;
			}
			if (RightDown)
			{
				IO.AddMouseButtonEvent(ImGuiMouseButton_Right, *RightDown);
				IO.MouseDown[ImGuiMouseButton_Right] = *RightDown;
			}
			if (PressEscape) IO.AddKeyEvent(ImGuiKey_Escape, true);
			if (AltDown) IO.AddKeyEvent(static_cast<ImGuiKey>(ImGuiMod_Alt), *AltDown);
			IO.DisplaySize = ImVec2(1280.0f, 720.0f);
			IO.DeltaTime = 1.0f / 60.0f;
			ImGui::NewFrame();
			if (AltDown) IO.KeyAlt = *AltDown;
			ImGui::SetNextWindowPos(ImVec2(96.0f, 64.0f));
			ImGui::SetNextWindowSize(IO.DisplaySize);
			ImGui::Begin("GraphLinkProtocolHost", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			const ImVec2 RightClickScreenAtFrameStart = IO.MouseClickedPos[ImGuiMouseButton_Right];
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
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false))
			{
				PendingPinRelease = {};
				ReleaseScreenPosition.reset();
			}
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				ReleaseScreenPosition = EditorGraphScreenPoint{ IO.MousePos.x, IO.MousePos.y };
			NodeEditor::SetCurrentEditor(Canvas);
			NodeEditor::Begin("##GraphLinkProtocolCanvas", ImVec2(0.0f, 0.0f));
			if (ReleaseScreenPosition)
			{
				const auto ScreenRelease = *ReleaseScreenPosition;
				const auto CapturedPoint = GraphCanvasDetail::ConvertEditorGraphScreenPoint(ScreenRelease,
					[](EditorGraphScreenPoint Point)
					{
						const ImVec2 Local = NodeEditor::ScreenToCanvas(
							ImVec2(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
						return GraphCanvasDetail::MakeEditorGraphCanvasPoint(Local.x, Local.y);
					});
				PendingPinRelease = { true, CanvasContext, {}, CapturedPoint };
				CapturedReleasePosition = CapturedPoint;
				ReleaseScreenPosition.reset();
			}
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
			if (CreatedCanvasNodePosition)
			{
				if (NeedsNodePositions)
					NodeEditor::SetNodePosition(NodeEditor::NodeId(44), ImVec2(
						static_cast<float>(CreatedCanvasNodePosition->X),
						static_cast<float>(CreatedCanvasNodePosition->Y)));
				NodeEditor::BeginNode(NodeEditor::NodeId(44));
				ImGui::TextUnformatted("Created node");
				NodeEditor::EndNode();
			}

			const auto Result = PulseForgeEditor::GraphCanvasDetail::ProcessEditorGraphCanvasCreateRequestFrame(
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
				[] { NodeEditor::RejectNewItem(); },
				[&]() -> std::optional<uintptr_t>
				{
					++NodeQueryCalls;
					++NodeQueriesThisFrame;
					NodeEditor::PinId SourcePin;
					if (!NodeEditor::QueryNewNode(&SourcePin)) return std::nullopt;
					return SourcePin.Get();
				},
				[&](uintptr_t SourceHandle) { return Identities.contains(SourceHandle); },
				[] { return NodeEditor::AcceptNewItem(); },
				[] { NodeEditor::RejectNewItem(); });
			PF_CHECK(Tests, QueriesThisFrame <= 1);
			PF_CHECK(Tests, NodeQueriesThisFrame <= 1);
			if (Result.SawNodeRequest && PendingPinRelease.Valid && PendingPinRelease.SourcePin.IsNil())
				if (const auto Source = Identities.find(Result.SourcePin); Source != Identities.end())
					PendingPinRelease.SourcePin = Source->second;
			if (Result.SawLinkRequest && Result.LinkDecision.Allowed && !Result.AcceptedLink) ++PreviewFrames;
			if (Result.SawLinkRequest && !Result.LinkDecision.Allowed) ++RejectedFrames;
			if (Result.AcceptedLink)
			{
				++AcceptedRequests;
				(void)PulseForgeEditor::ConnectEditorGraphPins(
					History, Schema, Result.LinkDecision.OutputPin, Result.LinkDecision.InputPin);
			}
			if (Result.SawNodeRequest && Result.AcceptedNode)
			{
				++AcceptedNodeRequests;
				const auto Source = Identities.find(Result.SourcePin);
				PF_CHECK(Tests, Source != Identities.end());
				if (Source != Identities.end())
				{
					const auto Captured = GraphCanvasDetail::ConsumePinRelease(
						PendingPinRelease, CanvasContext, Source->second);
					PF_CHECK(Tests, Captured.has_value());
					if (Captured)
					{
						AcceptedNodeDrop = GraphCanvasDetail::ToEditorGraphPosition(Captured->Position);
						PF_CHECK(Tests, std::isfinite(AcceptedNodeDrop->X) && std::isfinite(AcceptedNodeDrop->Y));
						PF_CHECK(Tests, CapturedReleasePosition &&
							AcceptedNodeDrop->X == std::floor(CapturedReleasePosition->X) &&
							AcceptedNodeDrop->Y == std::floor(CapturedReleasePosition->Y));
					}
				}
			}
			NodeEditor::Suspend();
			const bool BackgroundMenuRequested = NodeEditor::ShowBackgroundContextMenu();
			NodeEditor::Resume();
			if (BackgroundMenuRequested)
			{
				const ImVec2 LocalClick = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Right];
				BackgroundCanvasPosition = GraphCanvasDetail::MakeEditorGraphCanvasPoint(LocalClick.x, LocalClick.y);
				const ImVec2 Expected = NodeEditor::ScreenToCanvas(RightClickScreenAtFrameStart);
				ExpectedBackgroundCanvasPosition = GraphCanvasDetail::MakeEditorGraphCanvasPoint(Expected.x, Expected.y);
			}
			NodeEditor::End();
			if (CreatedCanvasNodePosition)
			{
				const ImVec2 Readback = NodeEditor::GetNodePosition(NodeEditor::NodeId(44));
				CreatedCanvasNodeReadback = EditorGraphVector3{ Readback.x, Readback.y, 0.0 };
			}
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
			if (BackgroundMenuRequested) ++BackgroundMenuRequests;
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

		const int PriorNodeAccepts = AcceptedNodeRequests;
		const ImVec2 EmptyCanvasPosition(1030.0f, 630.0f);
		BeginDrag(ScalarPosition, EmptyCanvasPosition);
		Frame(EmptyCanvasPosition, false);
		Frame(ImVec2(1120.0f, 500.0f));
		Frame();
		PF_CHECK(Tests, AcceptedNodeRequests == PriorNodeAccepts + 1 && AcceptedNodeDrop.has_value());
		const auto* SourceValue = FindPin(History.Graph(), "material.scalar", "value");
		PF_CHECK(Tests, SourceValue != nullptr);
		const auto BeforeAutoCreate = History.Graph();
		if (SourceValue && AcceptedNodeDrop)
		{
			const auto CompatibleActions = DiscoverEditorGraphNodeActions(History.Graph(), Schema, SourceValue->ID);
			const auto MultiplyAAction = std::find_if(CompatibleActions.begin(), CompatibleActions.end(), [](const auto& Action)
				{ return Action.NodeTypeKey == "material.multiply" && Action.ConnectPinKey == "a"; });
			PF_CHECK(Tests, MultiplyAAction != CompatibleActions.end());
			if (MultiplyAAction != CompatibleActions.end())
			{
				auto Created = CreateEditorGraphNodeAndConnect(History, Schema, MultiplyAAction->NodeTypeKey,
					*MultiplyAAction->ConnectPinKey, SourceValue->ID, AcceptedNodeDrop->X, AcceptedNodeDrop->Y);
				PF_CHECK(Tests, Created.has_value() && History.Graph().Nodes.size() == BeforeAutoCreate.Nodes.size() + 1 &&
					History.Graph().Connections.size() == BeforeAutoCreate.Connections.size() + 1);
				if (Created)
				{
					const auto AfterAutoCreate = History.Graph();
					const auto CreatedNode = std::find_if(AfterAutoCreate.Nodes.begin(), AfterAutoCreate.Nodes.end(),
						[&Created](const auto& Node) { return Node.ID == Created->NodeID; });
					PF_CHECK(Tests, CreatedNode != AfterAutoCreate.Nodes.end() &&
						CreatedNode->X == std::floor(AcceptedNodeDrop->X) &&
						CreatedNode->Y == std::floor(AcceptedNodeDrop->Y));
					PF_CHECK(Tests, History.Undo() && History.Graph() == BeforeAutoCreate);
					PF_CHECK(Tests, History.Redo() && History.Graph() == AfterAutoCreate);
					CreatedCanvasNodePosition = *AcceptedNodeDrop;
					NeedsNodePositions = true;
					Frame();
					PF_CHECK(Tests, CreatedCanvasNodeReadback &&
						CreatedCanvasNodeReadback->X == AcceptedNodeDrop->X &&
						CreatedCanvasNodeReadback->Y == AcceptedNodeDrop->Y);
					Frame();
					PF_CHECK(Tests, CreatedCanvasNodeReadback &&
						CreatedCanvasNodeReadback->X == AcceptedNodeDrop->X &&
						CreatedCanvasNodeReadback->Y == AcceptedNodeDrop->Y);
				}
			}
		}

		const int PreviousBackgroundRequests = BackgroundMenuRequests;
		const ImVec2 BackgroundPosition(1110.0f, 610.0f);
		Frame(BackgroundPosition, std::nullopt, false, std::nullopt, true);
		Frame(ImVec2(1160.0f, 645.0f));
		Frame(ImVec2(1160.0f, 645.0f), std::nullopt, false, std::nullopt, false);
		Frame();
		PF_CHECK(Tests, BackgroundMenuRequests == PreviousBackgroundRequests);
		Frame(BackgroundPosition, std::nullopt, false, std::nullopt, true);
		Frame(BackgroundPosition, std::nullopt, false, std::nullopt, false);
		Frame();
		PF_CHECK(Tests, BackgroundMenuRequests == PreviousBackgroundRequests + 1);
		PF_CHECK(Tests, BackgroundCanvasPosition.has_value() && ExpectedBackgroundCanvasPosition.has_value());
		if (BackgroundCanvasPosition && ExpectedBackgroundCanvasPosition)
		{
			PF_CHECK(Tests, std::abs(BackgroundCanvasPosition->X - ExpectedBackgroundCanvasPosition->X) < 0.01 &&
				std::abs(BackgroundCanvasPosition->Y - ExpectedBackgroundCanvasPosition->Y) < 0.01);
		}
		for (int Index = 0; Index < 3; ++Index) Frame();
		PF_CHECK(Tests, BackgroundMenuRequests == PreviousBackgroundRequests + 1);
		Frame(BackgroundPosition, std::nullopt, false, std::nullopt, true);
		Frame(BackgroundPosition, std::nullopt, false, std::nullopt, false);
		Frame();
		PF_CHECK(Tests, BackgroundMenuRequests == PreviousBackgroundRequests + 2);
	}
}

void RunEditorGraphDocumentEditorTests(TestRunner& Tests)
{
	TestGraphDocumentLifecycle(Tests);
	TestGraphDocumentSaveRevisionAccounting(Tests);
	TestGraphCanvasCreateRequestPolling(Tests);
	TestGraphCanvasNodeCreationRequestPolling(Tests);
	TestGraphNodeSearchNavigation(Tests);
	TestGraphCanvasCoordinateOwnership(Tests);
	TestAltPinUnlinkInputOwnership(Tests);
	TestGraphStraightenShortcutOwnership(Tests);
	TestPinnedCanvasLinkGesture(Tests);
}
