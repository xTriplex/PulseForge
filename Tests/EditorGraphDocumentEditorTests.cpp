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
#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	struct EditorGraphDocumentEditorTestAccess final
	{
		static void Draw(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			Editor.DrawWorkspace(Session);
		}


		static void SetGraph(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			EditorGraph Graph, EditorGraphSchema Schema)
		{
			Editor.SetGraphForTesting(Session, std::move(Graph), std::move(Schema));
		}

		static void SetCanvasSettings(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			std::string Settings)
		{
			Editor.SetCanvasSettingsForTesting(Session, std::move(Settings));
		}

		static void CapturePresentation(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session, bool Capture)
		{
			Editor.CapturePresentationForTesting(Session, Capture);
		}

		static std::vector<EditorGraphCanvasNodePresentation> GetPresentation(
			EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetPresentationForTesting(Session);
		}

		static EditorGraphPinVisibilityPopupPresentation GetPinVisibilityPopup(
			EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetPinVisibilityPopupForTesting(Session);
		}

		static void SetPinHidden(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			PulseForge::UUID PinID, bool Hidden)
		{
			Editor.SetPinHiddenForTesting(Session, PinID, Hidden);
		}

		static EditorGraph GetGraph(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetGraphForTesting(Session);
		}

		static void SetSelectedNodes(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			const std::vector<PulseForge::UUID>& NodeIDs)
		{
			Editor.SetSelectedNodesForTesting(Session, NodeIDs);
		}

		static bool HandleCommand(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			EditorDocumentCommand Command)
		{
			return Editor.HandleFocusedCommand(Session, Command);
		}

		static std::vector<PulseForge::UUID> GetSelectedNodes(EditorGraphDocumentEditor& Editor,
			EditorDocumentSession& Session)
		{
			return Editor.GetSelectedNodesForTesting(Session);
		}

		static EditorGraphCanvasPoint ScreenToCanvas(EditorGraphDocumentEditor& Editor,
			EditorDocumentSession& Session, EditorGraphScreenPoint ScreenPoint)
		{
			return Editor.ScreenToCanvasForTesting(Session, ScreenPoint);
		}

		static auto CapturePinVisibility(EditorGraphDocumentEditor& Editor, const EditorDocumentSession& Session)
		{
			return Editor.CapturePinVisibility(Session);
		}

		static void RestorePinVisibility(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			const std::vector<EditorDocumentWorkspaceState::GraphPinVisibility>& Settings)
		{
			Editor.RestorePinVisibility(Session, Settings);
		}

		static auto CaptureWorkspacePinVisibility(EditorGraphDocumentEditor& Editor)
		{
			return Editor.CaptureWorkspacePinVisibility();
		}

		static void SetWorkspacePinVisibility(EditorGraphDocumentEditor& Editor,
			std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> Settings)
		{
			Editor.SetWorkspacePinVisibility(std::move(Settings));
		}

		static EditorGraphPropertyEditResult Process(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session,
			const std::vector<EditorGraphPropertyEditEvent>& Events)
		{
			return Editor.ProcessPropertyEditEvents(Session, Events, false);
		}
	};
}

namespace
{
	std::string GraphTestClipboard;
	bool GraphTestClipboardRejectWrites = false;
	bool GraphShortcutSyntheticCtrlObserved = false;
	bool GraphShortcutSyntheticKeyObserved = false;
	bool GraphShortcutSyntheticHostFocused = false;

	const char* GetGraphTestClipboard(ImGuiContext*)
	{
		return GraphTestClipboard.c_str();
	}

	void SetGraphTestClipboard(ImGuiContext*, const char* Text)
	{
		if (!GraphTestClipboardRejectWrites) GraphTestClipboard = Text ? Text : "";
	}

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
		EditorGraphSchema FutureSchema{ "test.future-document", 3, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true, {
				{ "custom.emitter", "Emitter", { { "text-out", "Text", EditorGraphPinDirection::Output,
					EditorGraphPinType::String, EditorGraphPinCardinality::Multiple, std::string{} } } },
				{ "custom.archive", "Archive", { { "text-in", "Text", EditorGraphPinDirection::Input,
					EditorGraphPinType::String, EditorGraphPinCardinality::Single, std::string{} } } },
				{ "custom.alternate-archive", "Alternate archive", { { "text-in", "Text", EditorGraphPinDirection::Input,
					EditorGraphPinType::String, EditorGraphPinCardinality::Single, std::string{} } } },
				{ "custom.blocked", "Blocked", { { "blocked-in", "Blocked", EditorGraphPinDirection::Input,
					EditorGraphPinType::String, EditorGraphPinCardinality::Single, std::string{} } } }
			} };
		FutureSchema.ConnectionPolicy = [](const EditorGraph&, const EditorGraphConnectionPolicyContext& Context)
			-> std::optional<std::string>
			{
				if (!Context.Output.IsProspective && (!Context.Output.PinID || !Context.Output.OwnerNodeID ||
					Context.Output.OwnerNodeTypeKey != "custom.emitter" || Context.Output.PinKey != "text-out"))
					return "The live output must retain its registered owner and pin identity";
				if (Context.Input.IsProspective
					? (Context.Input.PinID.has_value() || Context.Input.OwnerNodeID.has_value())
					: (!Context.Input.PinID.has_value() || !Context.Input.OwnerNodeID.has_value()))
					return "Prospective pins have no persistent identity; live pins must have one";
				if (Context.Input.OwnerNodeTypeKey == "custom.alternate-archive")
					return "This schema requires the primary archive node";
				return Context.Input.PinKey == "blocked-in"
					? std::optional<std::string>("This document policy rejects the blocked input") : std::nullopt;
			};
		FutureSchema.GraphValidationPolicy = [](const EditorGraph& Graph) -> std::optional<EditorGraphDiagnostic>
			{ return Graph.Nodes.empty() ? std::optional<EditorGraphDiagnostic>(EditorGraphDiagnostic{
				"domain.required", "This graph requires at least one node", Graph.ID }) : std::nullopt; };
		PF_CHECK(Tests, GraphEditor.RegisterSchema(FutureSchema).has_value());
		auto FutureID = UUID::Generate();
		PF_CHECK(Tests, FutureID.has_value());
		std::optional<EditorGraphFixtureLocation> FutureFixture;
		if (FutureID)
		{
			auto FutureGraph = CreateEditorGraph(FutureSchema, *FutureID);
			PF_CHECK(Tests, FutureGraph.has_value());
			if (FutureGraph)
			{
				PF_CHECK(Tests, !ValidateEditorGraph(*FutureGraph, FutureSchema).empty());
				EditorGraphCommandHistory FutureHistory(std::move(*FutureGraph));
				PF_CHECK(Tests, AddEditorGraphNode(FutureHistory, FutureSchema, "custom.emitter", 24.0, 48.0).has_value());
				const auto SourcePin = FutureHistory.Graph().Nodes.front().Pins.front().ID;
				const auto Actions = DiscoverEditorGraphNodeActions(FutureHistory.Graph(), FutureSchema, SourcePin);
				PF_CHECK(Tests, Actions.size() == 1 && Actions.front().NodeTypeKey == "custom.archive");
				const auto BeforeFilteredCreate = FutureHistory.Graph();
				const auto BeforeFilteredRevision = FutureHistory.GraphRevision();
				const bool BeforeFilteredUndo = FutureHistory.CanUndo();
				PF_CHECK(Tests, !CreateEditorGraphNodeAndConnect(FutureHistory, FutureSchema,
					"custom.alternate-archive", "text-in", SourcePin, 80.0, 48.0));
				PF_CHECK(Tests, FutureHistory.Graph() == BeforeFilteredCreate &&
					FutureHistory.GraphRevision() == BeforeFilteredRevision && FutureHistory.CanUndo() == BeforeFilteredUndo);
				PF_CHECK(Tests, AddEditorGraphNode(FutureHistory, FutureSchema, "custom.archive", 120.0, 48.0).has_value());
				const auto ArchivePin = FutureHistory.Graph().Nodes.back().Pins.front().ID;
				PF_CHECK(Tests, ConnectEditorGraphPins(FutureHistory, FutureSchema, SourcePin, ArchivePin).has_value());
				PF_CHECK(Tests, AddEditorGraphNode(FutureHistory, FutureSchema, "custom.blocked", 100.0, 48.0).has_value());
				const auto BlockedPin = FutureHistory.Graph().Nodes.back().Pins.front().ID;
				PF_CHECK(Tests, !ConnectEditorGraphPins(FutureHistory, FutureSchema, SourcePin, BlockedPin));
				auto Bytes = SerializeEditorGraph(FutureHistory.Graph(), FutureSchema);
				PF_CHECK(Tests, Bytes.has_value());
				if (Bytes)
				{
					const auto RelativePath = EditorGraphDocumentEditor::GetFixturePath(*FutureID);
					std::error_code FuturePathError;
					std::filesystem::create_directories((Project->GetRootPath() / RelativePath).parent_path(), FuturePathError);
					PF_CHECK(Tests, !FuturePathError);
					PF_CHECK(Tests, WriteBytes(Project->GetRootPath() / RelativePath, *Bytes));
					FutureFixture = EditorGraphFixtureLocation{ *FutureID, RelativePath, "Custom test graph" };
				}
			}
		}
		PF_CHECK(Tests, GraphEditor.Register().has_value());
		const auto Material = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		const auto Gameplay = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::GameplayEvent);
		PF_CHECK(Tests, Material.has_value() && Gameplay.has_value());
		if (!Material || !Gameplay) return;
		if (FutureFixture)
		{
			const EditorDocumentIdentity FutureIdentity{ Project->GetDescription().ID, FutureFixture->GraphID,
				EditorDocumentKind::Graph };
			auto FutureSessionResult = Documents.Open(FutureIdentity, FutureFixture->ProjectRelativePath,
				FutureFixture->DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
			PF_CHECK(Tests, FutureSessionResult.has_value());
			auto* FutureSession = Documents.Find(FutureIdentity);
			const EditorDocumentEditorContext FutureContext{ FutureIdentity, Project->GetRootPath(),
				FutureFixture->ProjectRelativePath, FutureFixture->DisplayName, nullptr };
			PF_CHECK(Tests, FutureSession && Editors.Load(*FutureSession, FutureContext));
			PF_CHECK(Tests, FutureSession && FutureSession->EditorState && !FutureSession->IsDirty());
			if (FutureSession)
			{
				const auto LoadedGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *FutureSession);
				const auto LoadedSource = std::find_if(LoadedGraph.Nodes.begin(), LoadedGraph.Nodes.end(),
					[](const auto& Node) { return Node.TypeKey == "custom.emitter"; });
				PF_CHECK(Tests, LoadedSource != LoadedGraph.Nodes.end());
				const auto LoadedActions = LoadedSource == LoadedGraph.Nodes.end()
					? std::vector<EditorGraphNodeCreationAction>{}
					: DiscoverEditorGraphNodeActions(LoadedGraph, FutureSchema, LoadedSource->Pins.front().ID);
				PF_CHECK(Tests, LoadedActions.size() == 1 && LoadedActions.front().NodeTypeKey == "custom.archive");
				const UUID StableSourceNode = LoadedSource == LoadedGraph.Nodes.end() ? UUID{} : LoadedSource->ID;
				const auto AddedNode = GraphEditor.AddNode(*FutureSession, "custom.archive", 320.0, 96.0);
				PF_CHECK(Tests, AddedNode && FutureSession->IsDirty());
				const auto EditedGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *FutureSession);
				PF_CHECK(Tests, EditedGraph.Nodes.size() == LoadedGraph.Nodes.size() + 1 &&
					std::any_of(EditedGraph.Nodes.begin(), EditedGraph.Nodes.end(), [&AddedNode](const auto& Node)
						{ return AddedNode && Node.ID == *AddedNode && Node.TypeKey == "custom.archive"; }));
				PF_CHECK(Tests, GraphEditor.Save(*FutureSession) && !FutureSession->IsDirty());
				PF_CHECK(Tests, Editors.Discard(*FutureSession) && !FutureSession->IsDirty());
				const auto ReopenedGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *FutureSession);
				const auto EditedCanonical = SerializeEditorGraph(EditedGraph, FutureSchema);
				const auto ReopenedCanonical = SerializeEditorGraph(ReopenedGraph, FutureSchema);
				PF_CHECK(Tests, EditedCanonical && ReopenedCanonical && *EditedCanonical == *ReopenedCanonical &&
					ReopenedGraph.Connections.size() == EditedGraph.Connections.size() &&
					std::any_of(ReopenedGraph.Nodes.begin(), ReopenedGraph.Nodes.end(), [&StableSourceNode](const auto& Node)
						{ return Node.ID == StableSourceNode; }));
				const auto FuturePath = Project->GetRootPath() / FutureFixture->ProjectRelativePath;
				const auto BeforeBadSchema = ReadBytes(FuturePath);
				if (BeforeBadSchema)
				{
					auto Unsupported = *BeforeBadSchema;
					const auto VersionField = Unsupported.find("\"schemaVersion\": 3");
					PF_CHECK(Tests, VersionField != std::string::npos);
					if (VersionField != std::string::npos) Unsupported.replace(VersionField, std::string("\"schemaVersion\": 3").size(), "\"schemaVersion\": 4");
					PF_CHECK(Tests, WriteBytes(FuturePath, Unsupported));
					EditorDocumentManager RejectionDocuments;
					EditorDocumentEditorRegistry RejectionEditors;
					EditorGraphDocumentEditor RejectionGraphEditor(RejectionDocuments, RejectionEditors);
					PF_CHECK(Tests, RejectionGraphEditor.RegisterSchema(FutureSchema).has_value());
					PF_CHECK(Tests, RejectionGraphEditor.Register().has_value());
					auto RejectionSessionResult = RejectionDocuments.Open(FutureIdentity, FutureFixture->ProjectRelativePath,
						FutureFixture->DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
					auto* RejectionSession = RejectionDocuments.Find(FutureIdentity);
					PF_CHECK(Tests, RejectionSessionResult && RejectionSession && !RejectionEditors.Load(*RejectionSession, FutureContext));
					PF_CHECK(Tests, RejectionSession && !RejectionSession->EditorState);
					PF_CHECK(Tests, WriteBytes(FuturePath, *BeforeBadSchema));
					auto Unknown = *BeforeBadSchema;
					const auto SchemaField = Unknown.find("\"schema\": \"test.future-document\"");
					PF_CHECK(Tests, SchemaField != std::string::npos);
					if (SchemaField != std::string::npos)
						Unknown.replace(SchemaField, std::string("\"schema\": \"test.future-document\"").size(),
							"\"schema\": \"test.unregistered-document\"");
					PF_CHECK(Tests, WriteBytes(FuturePath, Unknown));
					EditorDocumentManager UnknownDocuments;
					EditorDocumentEditorRegistry UnknownEditors;
					EditorGraphDocumentEditor UnknownGraphEditor(UnknownDocuments, UnknownEditors);
					PF_CHECK(Tests, UnknownGraphEditor.RegisterSchema(FutureSchema).has_value());
					PF_CHECK(Tests, UnknownGraphEditor.Register().has_value());
					auto UnknownOpen = UnknownDocuments.Open(FutureIdentity, FutureFixture->ProjectRelativePath,
						FutureFixture->DisplayName, EditorDocumentOpenPreference::MainDocumentStrip);
					auto* UnknownSession = UnknownDocuments.Find(FutureIdentity);
					PF_CHECK(Tests, UnknownOpen && UnknownSession && !UnknownEditors.Load(*UnknownSession, FutureContext));
					PF_CHECK(Tests, UnknownSession && !UnknownSession->EditorState);
					PF_CHECK(Tests, WriteBytes(FuturePath, *BeforeBadSchema));
				}
			}
		}

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

	void TestGraphNodeSizingInProductionAdapter(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.LogFilename = nullptr;
		IO.DeltaTime = 1.0f / 60.0f;
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

		TemporaryGraphProject Temporary;
		const auto ProjectID = UUID::Generate();
		PF_CHECK(Tests, ProjectID.has_value());
		if (!ProjectID) return;
		Temporary.Path = std::filesystem::temp_directory_path() / ProjectID->ToString();
		std::error_code Error;
		std::filesystem::create_directories(Temporary.Path, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto Project = PulseForge::Project::Create(Temporary.Path / "GraphLayoutProject.pfproj", "Graph layout test");
		PF_CHECK(Tests, Project.has_value());
		if (!Project) return;

		EditorDocumentManager Documents;
		EditorDocumentEditorRegistry Editors;
		EditorGraphDocumentEditor GraphEditor(Documents, Editors);
		PF_CHECK(Tests, GraphEditor.Register().has_value());
		const auto MaterialFixture = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::MaterialExpression);
		const auto GameplayFixture = GraphEditor.CreateFixture(Project->GetRootPath(), EditorGraphFixtureKind::GameplayEvent);
		PF_CHECK(Tests, MaterialFixture && GameplayFixture);
		if (!MaterialFixture || !GameplayFixture) return;

		const auto OpenFixture = [&](const EditorGraphFixtureLocation& Fixture)
		{
			const EditorDocumentIdentity Identity{ Project->GetDescription().ID, Fixture.GraphID, EditorDocumentKind::Graph };
			PF_CHECK(Tests, Documents.Open(Identity, Fixture.ProjectRelativePath, Fixture.DisplayName,
				EditorDocumentOpenPreference::MainDocumentStrip).has_value());
			auto* Session = Documents.Find(Identity);
			const EditorDocumentEditorContext Context{ Identity, Project->GetRootPath(), Fixture.ProjectRelativePath,
				Fixture.DisplayName, nullptr };
			const bool Loaded = Session && Editors.Load(*Session, Context);
			PF_CHECK(Tests, Loaded && Session->EditorState);
			return Session;
		};
		auto* MaterialSession = OpenFixture(*MaterialFixture);
		auto* GameplaySession = OpenFixture(*GameplayFixture);
		PF_CHECK(Tests, MaterialSession && GameplaySession);
		if (!MaterialSession || !GameplaySession) return;

		const auto MaterialSource = ReadBytes(Project->GetRootPath() / MaterialFixture->ProjectRelativePath);
		auto MaterialGraph = MaterialSource ? ReadMaterialFixtureGraph(*MaterialSource) : std::nullopt;
		PF_CHECK(Tests, MaterialGraph.has_value());
		if (!MaterialGraph) return;
		auto MaterialSchema = MakeMaterialExpressionFixtureSchema();
		auto Scalar = CreateEditorGraphNode(MaterialSchema, "material.scalar", 760.0, 300.0);
		auto Multiply = CreateEditorGraphNode(MaterialSchema, "material.multiply", 1040.0, 300.0);
		auto IsolatedMultiply = CreateEditorGraphNode(MaterialSchema, "material.multiply", 1400.0, 560.0);
		PF_CHECK(Tests, Scalar && Multiply && IsolatedMultiply);
		if (!Scalar || !Multiply || !IsolatedMultiply) return;
		MaterialGraph->Nodes.push_back(std::move(*Scalar));
		MaterialGraph->Nodes.push_back(std::move(*Multiply));
		const UUID IsolatedNodeID = IsolatedMultiply->ID;
		std::vector<UUID> IsolatedPinIDs;
		for (const auto& Pin : IsolatedMultiply->Pins) IsolatedPinIDs.push_back(Pin.ID);
		MaterialGraph->Nodes.push_back(std::move(*IsolatedMultiply));
		const auto* ScalarValue = FindPin(*MaterialGraph, "material.scalar", "value");
		const auto* MultiplyA = FindPin(*MaterialGraph, "material.multiply", "a");
		PF_CHECK(Tests, ScalarValue && MultiplyA);
		if (!ScalarValue || !MultiplyA) return;
		EditorGraphCommandHistory MaterialHistory(*MaterialGraph);
		PF_CHECK(Tests, ConnectEditorGraphPins(MaterialHistory, MaterialSchema, ScalarValue->ID, MultiplyA->ID));
		MaterialGraph = MaterialHistory.Graph();
		const EditorGraph MaterialGraphForLayout = *MaterialGraph;
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *MaterialSession,
			std::move(*MaterialGraph), MaterialSchema);
		for (const auto PinID : IsolatedPinIDs)
			EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, PinID, true);

		EditorGraphDocumentEditorTestAccess::CapturePresentation(GraphEditor, *MaterialSession, true);
		EditorGraphDocumentEditorTestAccess::CapturePresentation(GraphEditor, *GameplaySession, true);
		const auto DrawFrame = [&](EditorDocumentSession& Session, std::string_view Host, float Width)
		{
			IO.DisplaySize = ImVec2(Width, 900.0f);
			IO.AddMousePosEvent(-1000.0f, -1000.0f);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(IO.DisplaySize);
			const std::string Name(Host);
			ImGui::Begin(Name.c_str(), nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			EditorGraphDocumentEditorTestAccess::Draw(GraphEditor, Session);
			ImGui::End();
			ImGui::Render();
			return Session.Identity == MaterialSession->Identity
				? EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *MaterialSession)
				: EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *GameplaySession);
		};
		const auto DrawInputFrame = [&](EditorDocumentSession& Session, std::string_view Host, float Width,
			ImVec2 HostOrigin, ImVec2 MousePosition, bool LeftDown, bool Escape = false,
			ImGuiKey PressedKey = ImGuiKey_None, bool Control = false)
		{
			IO.DisplaySize = ImVec2(Width, 900.0f);
			IO.AddMousePosEvent(MousePosition.x, MousePosition.y);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, LeftDown);
			IO.AddKeyEvent(ImGuiKey_Escape, Escape);
			IO.AddKeyEvent(ImGuiMod_Ctrl, Control);
			for (const ImGuiKey Key : { ImGuiKey_C, ImGuiKey_V, ImGuiKey_X, ImGuiKey_D, ImGuiKey_Delete })
				IO.AddKeyEvent(Key, Key == PressedKey);
			ImGui::NewFrame();
			if (Host == "GraphShortcutHost") ImGui::SetNextWindowFocus();
			ImGui::SetNextWindowPos(HostOrigin);
			ImGui::SetNextWindowSize(ImVec2(Width - HostOrigin.x, 900.0f - HostOrigin.y));
			const std::string Name(Host);
			ImGui::Begin(Name.c_str(), nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			if (Host == "GraphShortcutHost" && PressedKey != ImGuiKey_None)
			{
				GraphShortcutSyntheticCtrlObserved |= IO.KeyCtrl;
				GraphShortcutSyntheticKeyObserved |= ImGui::IsKeyPressed(PressedKey, false);
				GraphShortcutSyntheticHostFocused |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			}
			EditorGraphDocumentEditorTestAccess::Draw(GraphEditor, Session);
			ImGui::End();
			ImGui::Render();
			return Session.Identity == MaterialSession->Identity
				? EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *MaterialSession)
				: EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *GameplaySession);
		};
		const auto FindPresentation = [](const auto& Metrics, UUID NodeID) -> const EditorGraphCanvasNodePresentation*
		{
			const auto It = std::find_if(Metrics.begin(), Metrics.end(), [NodeID](const auto& Node)
				{ return Node.NodeID == NodeID; });
			return It == Metrics.end() ? nullptr : &*It;
		};
		const auto ValidateMetrics = [&](const auto& Metrics, const EditorGraph& Graph,
			const EditorGraphSchema& Schema, double MaximumWidth)
		{
			PF_CHECK(Tests, Metrics.size() == Graph.Nodes.size());
			for (const auto& Node : Metrics)
			{
				PF_CHECK(Tests, Node.Size.X > 0.0 && Node.Size.X < MaximumWidth && Node.Size.Y > 0.0);
				PF_CHECK(Tests, Node.Zoom > 0.0);
				bool HasInput = false;
				bool HasOutput = false;
				const auto ModelNode = std::find_if(Graph.Nodes.begin(), Graph.Nodes.end(), [&Node](const auto& Candidate)
					{ return Candidate.ID == Node.NodeID; });
				PF_CHECK(Tests, ModelNode != Graph.Nodes.end());
				if (ModelNode == Graph.Nodes.end()) continue;
				const auto NodeDefinitionForOrdering = std::find_if(Schema.Nodes.begin(), Schema.Nodes.end(), [&ModelNode](const auto& Definition)
					{ return Definition.TypeKey == ModelNode->TypeKey; });
				for (const auto& PinMetric : Node.Pins)
				{
					const auto Pin = std::find_if(ModelNode->Pins.begin(), ModelNode->Pins.end(), [&PinMetric](const auto& Candidate)
						{ return Candidate.ID == PinMetric.PinID; });
					PF_CHECK(Tests, Pin != ModelNode->Pins.end());
					if (Pin == ModelNode->Pins.end()) continue;
					const auto& Marker = PinMetric.Marker;
					PF_CHECK(Tests, PinMetric.Visible);
					PF_CHECK(Tests, Marker.Minimum.X >= Node.Position.X - 2.0 &&
						Marker.Maximum.X <= Node.Position.X + Node.Size.X + 2.0 &&
						Marker.Minimum.Y >= Node.Position.Y - 2.0 &&
						Marker.Maximum.Y <= Node.Position.Y + Node.Size.Y + 2.0);
					if (Pin->Direction == EditorGraphPinDirection::Input)
					{
						HasInput = true;
						PF_CHECK(Tests, Marker.Anchor.X < Node.Position.X + Node.Size.X * 0.4);
					}
					else
					{
						HasOutput = true;
						PF_CHECK(Tests, Marker.Anchor.X > Node.Position.X + Node.Size.X * 0.6);
					}
					const size_t Incident = static_cast<size_t>(std::count_if(Graph.Connections.begin(), Graph.Connections.end(),
						[&Pin](const auto& Connection)
							{ return Connection.InputPin == Pin->ID || Connection.OutputPin == Pin->ID; }));
					PF_CHECK(Tests, PinMetric.Connected == (Incident > 0));
					PF_CHECK(Tests, PinMetric.Label.has_value());
					if (PinMetric.Label && Pin->Direction == EditorGraphPinDirection::Input)
						PF_CHECK(Tests, PinMetric.Label->Minimum.X > Marker.Maximum.X);
					if (PinMetric.Label && Pin->Direction == EditorGraphPinDirection::Output)
						PF_CHECK(Tests, PinMetric.Label->Maximum.X < Marker.Minimum.X);
					const auto NodeDefinition = std::find_if(Schema.Nodes.begin(), Schema.Nodes.end(), [&ModelNode](const auto& Definition)
						{ return Definition.TypeKey == ModelNode->TypeKey; });
					const auto PinDefinition = NodeDefinition == Schema.Nodes.end()
						? std::vector<EditorGraphPinDefinition>::const_iterator{}
						: std::find_if(NodeDefinition->Pins.begin(), NodeDefinition->Pins.end(), [&Pin](const auto& Definition)
							{ return Definition.Key == Pin->Key; });
					const bool ShowDefault = Pin->Direction == EditorGraphPinDirection::Input &&
						Pin->Type != EditorGraphPinType::Execution && NodeDefinition != Schema.Nodes.end() &&
						PinDefinition != NodeDefinition->Pins.end() &&
						ShouldShowEditorGraphPinDefault(*Pin, *PinDefinition, Incident);
					if (ShowDefault)
					{
						PF_CHECK(Tests, PinMetric.PropertyControl.has_value());
						if (PinMetric.PropertyControl)
						{
							const auto& Control = *PinMetric.PropertyControl;
							PF_CHECK(Tests, Control.Minimum.X >= Node.Position.X - 2.0 &&
								Control.Maximum.X <= Node.Position.X + Node.Size.X + 2.0 &&
								Control.Minimum.Y >= Node.Position.Y - 2.0 &&
								Control.Maximum.Y <= Node.Position.Y + Node.Size.Y + 2.0);
							const bool OverlapsMarker = Control.Minimum.X < Marker.Maximum.X &&
								Control.Maximum.X > Marker.Minimum.X && Control.Minimum.Y < Marker.Maximum.Y &&
								Control.Maximum.Y > Marker.Minimum.Y;
							PF_CHECK(Tests, !OverlapsMarker);
						}
					}
					else if (Pin->Direction == EditorGraphPinDirection::Input && Pin->Type != EditorGraphPinType::Execution)
						PF_CHECK(Tests, !PinMetric.PropertyControl.has_value());
				}
				if (NodeDefinitionForOrdering != Schema.Nodes.end())
					for (const auto Direction : { EditorGraphPinDirection::Input, EditorGraphPinDirection::Output })
					{
						std::vector<std::string> ExpectedOrder;
						std::vector<std::string> ActualOrder;
						for (const auto& Definition : NodeDefinitionForOrdering->Pins)
							if (Definition.Direction == Direction) ExpectedOrder.push_back(Definition.Key);
						for (const auto& Metric : Node.Pins)
						{
							const auto Pin = std::find_if(ModelNode->Pins.begin(), ModelNode->Pins.end(), [&Metric](const auto& Candidate)
								{ return Candidate.ID == Metric.PinID; });
							if (Pin != ModelNode->Pins.end() && Pin->Direction == Direction) ActualOrder.push_back(Pin->Key);
						}
						PF_CHECK(Tests, ActualOrder == ExpectedOrder);
					}
				if (HasInput && HasOutput)
				{
					const auto Input = std::find_if(Node.Pins.begin(), Node.Pins.end(), [&](const auto& Metric)
					{
						const auto Pin = std::find_if(ModelNode->Pins.begin(), ModelNode->Pins.end(), [&Metric](const auto& Candidate)
							{ return Candidate.ID == Metric.PinID; });
						return Pin != ModelNode->Pins.end() && Pin->Direction == EditorGraphPinDirection::Input;
					});
					const auto Output = std::find_if(Node.Pins.begin(), Node.Pins.end(), [&](const auto& Metric)
					{
						const auto Pin = std::find_if(ModelNode->Pins.begin(), ModelNode->Pins.end(), [&Metric](const auto& Candidate)
							{ return Candidate.ID == Metric.PinID; });
						return Pin != ModelNode->Pins.end() && Pin->Direction == EditorGraphPinDirection::Output;
					});
					PF_CHECK(Tests, Input != Node.Pins.end() && Output != Node.Pins.end() &&
						Input->Marker.Anchor.X < Output->Marker.Anchor.X);
				}
			}
		};

		const auto CheckViewportStability = [&](EditorDocumentSession& Session, const EditorGraph& Graph,
			const EditorGraphSchema& Schema, std::string_view Host, double MaximumWidth)
		{
			EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, Session,
				"{\"view\":{\"scroll\":{\"x\":37.0,\"y\":-21.0},\"zoom\":0.65}}");
			const auto Narrow = DrawFrame(Session, Host, 1280.0f);
			ValidateMetrics(Narrow, Graph, Schema, MaximumWidth);
			const auto NarrowAgain = DrawFrame(Session, Host, 1280.0f);
			ValidateMetrics(NarrowAgain, Graph, Schema, MaximumWidth);
			const auto Wide = DrawFrame(Session, Host, 1920.0f);
			ValidateMetrics(Wide, Graph, Schema, MaximumWidth);
			for (const auto& Node : Narrow)
			{
				const auto* First = FindPresentation(NarrowAgain, Node.NodeID);
				const auto* Resized = FindPresentation(Wide, Node.NodeID);
				PF_CHECK(Tests, First && Resized);
				if (!First || !Resized) continue;
				PF_CHECK(Tests, std::abs(Node.Size.X - First->Size.X) < 1.0 &&
					std::abs(Node.Size.X - Resized->Size.X) < 1.0);
			}

			EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, Session,
				"{\"view\":{\"scroll\":{\"x\":-83.0,\"y\":46.0},\"zoom\":1.45}}");
			const auto Zoomed = DrawFrame(Session, Host, 1280.0f);
			ValidateMetrics(Zoomed, Graph, Schema, MaximumWidth);
			for (const auto& Node : Narrow)
				if (const auto* ChangedView = FindPresentation(Zoomed, Node.NodeID))
					PF_CHECK(Tests, std::abs(Node.Size.X - ChangedView->Size.X) < 1.0);
				else PF_CHECK(Tests, false);
		};

		const auto MaterialBytes = ReadBytes(Project->GetRootPath() / MaterialFixture->ProjectRelativePath);
		const auto MaterialGraphForTest = MaterialBytes ? ReadMaterialFixtureGraph(*MaterialBytes) : std::nullopt;
		PF_CHECK(Tests, MaterialGraphForTest.has_value());
		if (MaterialGraphForTest)
		{
			CheckViewportStability(*MaterialSession, MaterialGraphForLayout, MaterialSchema, "MaterialLayoutHost", 700.0);
			const auto MultiplyNode = std::find_if(MaterialGraphForLayout.Nodes.begin(), MaterialGraphForLayout.Nodes.end(),
				[](const auto& Node) { return Node.TypeKey == "material.multiply"; });
			const auto ScalarNode = std::find_if(MaterialGraphForLayout.Nodes.begin(), MaterialGraphForLayout.Nodes.end(),
				[](const auto& Node) { return Node.TypeKey == "material.scalar"; });
			const auto* UnusedB = FindPin(MaterialGraphForLayout, "material.multiply", "b");
			PF_CHECK(Tests, MultiplyNode != MaterialGraphForLayout.Nodes.end() && ScalarNode != MaterialGraphForLayout.Nodes.end());
			PF_CHECK(Tests, UnusedB != nullptr);
			if (MultiplyNode != MaterialGraphForLayout.Nodes.end() && ScalarNode != MaterialGraphForLayout.Nodes.end() && UnusedB)
			{
				const ImVec2 HostOrigin(37.0f, 29.0f);
				const auto OtherDocumentVisibilityBefore = EditorGraphDocumentEditorTestAccess::CapturePinVisibility(
					GraphEditor, *GameplaySession);
				const bool OtherDocumentDirtyBefore = GameplaySession->IsDirty();
				const auto Center = [](const EditorGraphCanvasRect& Rect)
				{
					return ImVec2(static_cast<float>((Rect.Minimum.X + Rect.Maximum.X) * 0.5),
						static_cast<float>((Rect.Minimum.Y + Rect.Maximum.Y) * 0.5));
				};
				const auto ClickAt = [&](const EditorGraphCanvasRect& Rect)
				{
					const ImVec2 Point = Center(Rect);
					(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, Point, true);
					return DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, Point, false);
				};
				const auto ReadNodeMetrics = [&](UUID NodeID)
				{
					const auto Metrics = DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin,
						ImVec2(-1000.0f, -1000.0f), false);
					const auto* NodeMetrics = FindPresentation(Metrics, NodeID);
					return NodeMetrics ? *NodeMetrics : EditorGraphCanvasNodePresentation{};
				};
				const auto HoverAt = [&](const EditorGraphCanvasRect& Rect)
				{
					const ImVec2 Point = Center(Rect);
					for (int Frame = 0; Frame < 16; ++Frame)
						(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, Point, false);
					return EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
				};
				const auto OpenPopupFor = [&](UUID NodeID)
				{
					const auto NodeMetrics = ReadNodeMetrics(NodeID);
					if (!NodeMetrics.PinsButtonScreenBounds) return EditorGraphPinVisibilityPopupPresentation{};
					ClickAt(*NodeMetrics.PinsButtonScreenBounds);
					return EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
				};
				EditorGraphPinVisibilityPopupPresentation PopupMetrics;
				for (const auto PinID : IsolatedPinIDs)
					EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, PinID, true);
				auto IsolatedMetrics = ReadNodeMetrics(IsolatedNodeID);
			PF_CHECK(Tests, IsolatedMetrics.HiddenPinCount == 0 && IsolatedMetrics.Pins.size() == IsolatedPinIDs.size());
			PF_CHECK(Tests, std::all_of(IsolatedMetrics.Pins.begin(), IsolatedMetrics.Pins.end(),
				[](const auto& Pin) { return Pin.Visible; }));
			PopupMetrics = OpenPopupFor(IsolatedNodeID);
			PF_CHECK(Tests, PopupMetrics.NodeID == IsolatedNodeID && !PopupMetrics.HideUnusedEnabled);
			if (PopupMetrics.PinsSubmenuScreenBounds)
			{
				PopupMetrics = HoverAt(*PopupMetrics.PinsSubmenuScreenBounds);
				PF_CHECK(Tests, PopupMetrics.PinItems.size() == IsolatedPinIDs.size());
				PF_CHECK(Tests, std::all_of(PopupMetrics.PinItems.begin(), PopupMetrics.PinItems.end(),
					[](const auto& Item) { return Item.Visible && !Item.Enabled; }));
			}
			if (PopupMetrics.HideUnusedScreenBounds)
			{
				const auto HideBounds = PopupMetrics.HideUnusedScreenBounds;
				HoverAt(*HideBounds);
				ClickAt(*HideBounds);
				PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
				PF_CHECK(Tests, !PopupMetrics.HideUnusedActivated);
			}
			(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin,
				ImVec2(-1000.0f, -1000.0f), false, true);
			(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin,
				ImVec2(-1000.0f, -1000.0f), false, false);

			// Saved hide preferences become effective only while at least one committed edge exists.
			auto ReconnectedGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *MaterialSession);
			EditorGraphCommandHistory ReconnectedHistory(ReconnectedGraph);
			const auto IsolatedNode = std::find_if(ReconnectedHistory.Graph().Nodes.begin(),
				ReconnectedHistory.Graph().Nodes.end(), [IsolatedNodeID](const auto& Node) { return Node.ID == IsolatedNodeID; });
			const EditorGraphPin* IsolatedInputA = nullptr;
			if (IsolatedNode != ReconnectedHistory.Graph().Nodes.end())
				if (const auto Pin = std::find_if(IsolatedNode->Pins.begin(), IsolatedNode->Pins.end(),
					[](const auto& Candidate) { return Candidate.Key == "a"; }); Pin != IsolatedNode->Pins.end())
					IsolatedInputA = &*Pin;
			PF_CHECK(Tests, IsolatedInputA != nullptr);
			if (IsolatedInputA && ScalarValue)
			{
				PF_CHECK(Tests, ConnectEditorGraphPins(ReconnectedHistory, MaterialSchema,
					ScalarValue->ID, IsolatedInputA->ID).has_value());
				EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *MaterialSession,
					ReconnectedHistory.Graph(), MaterialSchema);
				IsolatedMetrics = ReadNodeMetrics(IsolatedNodeID);
				PF_CHECK(Tests, IsolatedMetrics.HiddenPinCount == IsolatedPinIDs.size() - 1);
				PF_CHECK(Tests, IsolatedMetrics.Pins.size() == 1 && IsolatedMetrics.Pins.front().Connected);
				auto DisconnectedGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *MaterialSession);
				EditorGraphCommandHistory DisconnectedHistory(DisconnectedGraph);
				PF_CHECK(Tests, DisconnectEditorGraphPin(DisconnectedHistory, MaterialSchema,
					IsolatedInputA->ID).has_value());
				EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *MaterialSession,
					DisconnectedHistory.Graph(), MaterialSchema);
				IsolatedMetrics = ReadNodeMetrics(IsolatedNodeID);
				PF_CHECK(Tests, IsolatedMetrics.HiddenPinCount == 0 && IsolatedMetrics.Pins.size() == IsolatedPinIDs.size());
				PF_CHECK(Tests, std::all_of(IsolatedMetrics.Pins.begin(), IsolatedMetrics.Pins.end(),
					[](const auto& Pin) { return Pin.Visible; }));
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::CapturePinVisibility(GraphEditor, *MaterialSession).size() >=
					IsolatedPinIDs.size());
			}
			for (const auto PinID : IsolatedPinIDs)
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, PinID, false);

				PopupMetrics = OpenPopupFor(MultiplyNode->ID);
				PF_CHECK(Tests, PopupMetrics.NodeID == MultiplyNode->ID);
				PF_CHECK(Tests, PopupMetrics.ContextValid && PopupMetrics.OpenSubmitted && PopupMetrics.PopupVisible &&
					PopupMetrics.PopupOpenAfterSubmit && PopupMetrics.PopupScreenBounds.has_value() && PopupMetrics.NoMove);
				PF_CHECK(Tests, PopupMetrics.HideUnusedScreenBounds.has_value() &&
					PopupMetrics.ResetVisibilityScreenBounds.has_value() && PopupMetrics.ShowAllScreenBounds.has_value());
				if (PopupMetrics.PopupScreenBounds && PopupMetrics.TriggerScreenBounds)
				{
					const auto& Popup = *PopupMetrics.PopupScreenBounds;
					const auto& Trigger = *PopupMetrics.TriggerScreenBounds;
					const double GapX = (std::max)({ Trigger.Minimum.X - Popup.Maximum.X,
						Popup.Minimum.X - Trigger.Maximum.X, 0.0 });
					const double GapY = (std::max)({ Trigger.Minimum.Y - Popup.Maximum.Y,
						Popup.Minimum.Y - Trigger.Maximum.Y, 0.0 });
					PF_CHECK(Tests, std::hypot(GapX, GapY) < 260.0);
				}

				if (PopupMetrics.HideUnusedScreenBounds)
				{
					PopupMetrics = HoverAt(*PopupMetrics.HideUnusedScreenBounds);
					PF_CHECK(Tests, PopupMetrics.HideUnusedHovered && PopupMetrics.PopupWindowHovered);
					const auto HideUnusedBounds = PopupMetrics.HideUnusedScreenBounds;
					if (HideUnusedBounds) ClickAt(*HideUnusedBounds);
					PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
					PF_CHECK(Tests, PopupMetrics.HideUnusedActivated);
					const auto Visibility = EditorGraphDocumentEditorTestAccess::CapturePinVisibility(GraphEditor, *MaterialSession);
					PF_CHECK(Tests, std::any_of(Visibility.begin(), Visibility.end(), [MultiplyNode, UnusedB](const auto& Setting)
						{ return Setting.NodeID == MultiplyNode->ID && Setting.PinID == UnusedB->ID; }));
					PF_CHECK(Tests, !MaterialSession->IsDirty());
				}

				// The submenu changes the same stable pin identity; a later Show All restores it.
				PopupMetrics = OpenPopupFor(MultiplyNode->ID);
				PF_CHECK(Tests, PopupMetrics.NodeID == MultiplyNode->ID && PopupMetrics.PopupVisible);
				if (PopupMetrics.PinsSubmenuScreenBounds)
				{
					PopupMetrics = HoverAt(*PopupMetrics.PinsSubmenuScreenBounds);
					PF_CHECK(Tests, PopupMetrics.PinsSubmenuHovered);
					const auto BItem = std::find_if(PopupMetrics.PinItems.begin(), PopupMetrics.PinItems.end(),
						[UnusedB](const auto& Item) { return Item.PinID == UnusedB->ID; });
					PF_CHECK(Tests, BItem != PopupMetrics.PinItems.end());
					if (BItem != PopupMetrics.PinItems.end())
					{
						const auto BBounds = BItem->Bounds;
						HoverAt(BBounds);
						ClickAt(BBounds);
						const auto Visibility = EditorGraphDocumentEditorTestAccess::CapturePinVisibility(GraphEditor, *MaterialSession);
						PF_CHECK(Tests, std::none_of(Visibility.begin(), Visibility.end(), [UnusedB](const auto& Setting)
							{ return Setting.PinID == UnusedB->ID; }));
					}
				}
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, UnusedB->ID, true);
				PopupMetrics = OpenPopupFor(MultiplyNode->ID);
				if (PopupMetrics.ResetVisibilityScreenBounds)
				{
					PopupMetrics = HoverAt(*PopupMetrics.ResetVisibilityScreenBounds);
					const auto ResetBounds = PopupMetrics.ResetVisibilityScreenBounds;
					PF_CHECK(Tests, PopupMetrics.ResetVisibilityHovered);
					if (ResetBounds) ClickAt(*ResetBounds);
					PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
					PF_CHECK(Tests, PopupMetrics.ResetVisibilityActivated);
					PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::CapturePinVisibility(GraphEditor, *MaterialSession).empty());
				}
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, UnusedB->ID, true);
				PopupMetrics = OpenPopupFor(MultiplyNode->ID);
				if (PopupMetrics.ShowAllScreenBounds)
				{
					PopupMetrics = HoverAt(*PopupMetrics.ShowAllScreenBounds);
					const auto ShowAllBounds = PopupMetrics.ShowAllScreenBounds;
					PF_CHECK(Tests, PopupMetrics.ShowAllHovered);
					if (ShowAllBounds) ClickAt(*ShowAllBounds);
					PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
					PF_CHECK(Tests, PopupMetrics.ShowAllActivated);
					PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::CapturePinVisibility(GraphEditor, *MaterialSession).empty());
				}

				// A drag starting within popup padding cannot move the popup or underlying node.
				const auto NodeBeforeDrag = ReadNodeMetrics(MultiplyNode->ID).Position;
				PopupMetrics = OpenPopupFor(MultiplyNode->ID);
				const auto PopupBeforeDrag = PopupMetrics.PopupScreenBounds;
				if (PopupBeforeDrag)
				{
					const ImVec2 PaddingPoint(static_cast<float>(PopupBeforeDrag->Maximum.X - 4.0),
						static_cast<float>(PopupBeforeDrag->Maximum.Y - 4.0));
					const ImVec2 DragPoint(PaddingPoint.x - 40.0f, PaddingPoint.y - 30.0f);
					(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, PaddingPoint, false);
					(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, PaddingPoint, true);
					(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, DragPoint, true);
					(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, DragPoint, false);
					PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
					PF_CHECK(Tests, PopupMetrics.PopupScreenBounds.has_value());
					if (PopupMetrics.PopupScreenBounds)
						PF_CHECK(Tests, std::abs(PopupMetrics.PopupScreenBounds->Minimum.X - PopupBeforeDrag->Minimum.X) < 0.1 &&
							std::abs(PopupMetrics.PopupScreenBounds->Minimum.Y - PopupBeforeDrag->Minimum.Y) < 0.1);
					const auto AfterDrag = ReadNodeMetrics(MultiplyNode->ID);
					PF_CHECK(Tests, std::abs(AfterDrag.Position.X - NodeBeforeDrag.X) < 0.1 &&
						std::abs(AfterDrag.Position.Y - NodeBeforeDrag.Y) < 0.1);
				}
				(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin,
					ImVec2(-1000.0f, -1000.0f), false, true);
				(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin,
					ImVec2(-1000.0f, -1000.0f), false, false);
				PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
				PF_CHECK(Tests, !PopupMetrics.PopupScreenBounds.has_value());

				// Outside dismissal does not reuse its click for an operation on the next node.
				const auto NodeBeforeOutsideClick = ReadNodeMetrics(MultiplyNode->ID).Position;
				PopupMetrics = OpenPopupFor(MultiplyNode->ID);
				const ImVec2 OutsidePoint(120.0f, 180.0f);
				(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, OutsidePoint, true);
				(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, OutsidePoint, false);
				(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin, OutsidePoint, false);
				PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
				PF_CHECK(Tests, !PopupMetrics.PopupScreenBounds.has_value());
				const auto NodeAfterOutsideClick = ReadNodeMetrics(MultiplyNode->ID).Position;
				PF_CHECK(Tests, std::abs(NodeAfterOutsideClick.X - NodeBeforeOutsideClick.X) < 0.1 &&
					std::abs(NodeAfterOutsideClick.Y - NodeBeforeOutsideClick.Y) < 0.1);
				PopupMetrics = OpenPopupFor(ScalarNode->ID);
				PF_CHECK(Tests, PopupMetrics.NodeID == ScalarNode->ID && PopupMetrics.PopupVisible);
				(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin,
					ImVec2(-1000.0f, -1000.0f), false, true);
				(void)DrawInputFrame(*MaterialSession, "PinsPopupHost", 1440.0f, HostOrigin,
					ImVec2(-1000.0f, -1000.0f), false, false);
				PopupMetrics = EditorGraphDocumentEditorTestAccess::GetPinVisibilityPopup(GraphEditor, *MaterialSession);
				PF_CHECK(Tests, !PopupMetrics.PopupScreenBounds.has_value());
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::CapturePinVisibility(GraphEditor, *GameplaySession) ==
					OtherDocumentVisibilityBefore);
				PF_CHECK(Tests, GameplaySession->IsDirty() == OtherDocumentDirtyBefore);
			}
			const auto* ConnectedA = FindPin(MaterialGraphForLayout, "material.multiply", "a");
			PF_CHECK(Tests, UnusedB && ConnectedA);
			if (UnusedB && ConnectedA)
			{
				const auto MultiplyNode = std::find_if(MaterialGraphForLayout.Nodes.begin(), MaterialGraphForLayout.Nodes.end(),
					[UnusedB](const auto& Node)
						{ return std::any_of(Node.Pins.begin(), Node.Pins.end(), [UnusedB](const auto& Pin) { return Pin.ID == UnusedB->ID; }); });
				PF_CHECK(Tests, MultiplyNode != MaterialGraphForLayout.Nodes.end());
				const auto VisibleMetrics = DrawFrame(*MaterialSession, "MaterialVisiblePins", 1280.0f);
				PF_CHECK(Tests, !MaterialSession->IsDirty());
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, UnusedB->ID, true);
				const auto HiddenMetrics = DrawFrame(*MaterialSession, "MaterialHiddenPins", 1280.0f);
				if (MultiplyNode != MaterialGraphForLayout.Nodes.end())
				{
					const auto* VisibleNodeMetrics = FindPresentation(VisibleMetrics, MultiplyNode->ID);
					if (const auto* NodeMetrics = FindPresentation(HiddenMetrics, MultiplyNode->ID))
					{
						PF_CHECK(Tests, VisibleNodeMetrics && VisibleNodeMetrics->Size.Y > NodeMetrics->Size.Y);
						if (VisibleNodeMetrics)
							PF_CHECK(Tests, std::abs(VisibleNodeMetrics->Position.X - NodeMetrics->Position.X) < 0.1 &&
								std::abs(VisibleNodeMetrics->Position.Y - NodeMetrics->Position.Y) < 0.1);
						PF_CHECK(Tests, std::none_of(NodeMetrics->Pins.begin(), NodeMetrics->Pins.end(),
							[UnusedB](const auto& Pin) { return Pin.PinID == UnusedB->ID; }));
						PF_CHECK(Tests, std::any_of(NodeMetrics->Pins.begin(), NodeMetrics->Pins.end(),
							[ConnectedA](const auto& Pin) { return Pin.PinID == ConnectedA->ID && Pin.Connected; }));
					}
					else PF_CHECK(Tests, false);
				}
				const auto HiddenSettings = EditorGraphDocumentEditorTestAccess::CaptureWorkspacePinVisibility(GraphEditor);
				PF_CHECK(Tests, HiddenSettings.size() == 1 && HiddenSettings.front().PinID == UnusedB->ID);
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, UnusedB->ID, false);
				EditorGraphDocumentEditorTestAccess::SetWorkspacePinVisibility(GraphEditor, {});
				EditorGraphDocumentEditorTestAccess::SetWorkspacePinVisibility(GraphEditor, HiddenSettings);
				EditorGraphDocumentEditorTestAccess::RestorePinVisibility(GraphEditor, *MaterialSession, HiddenSettings);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::CapturePinVisibility(GraphEditor, *MaterialSession) == HiddenSettings);
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, UnusedB->ID, false);
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, ConnectedA->ID, true);
				const auto ConnectedHiddenMetrics = DrawFrame(*MaterialSession, "ConnectedPinCannotDisappear", 1280.0f);
				if (MultiplyNode != MaterialGraphForLayout.Nodes.end())
				{
					if (const auto* NodeMetrics = FindPresentation(ConnectedHiddenMetrics, MultiplyNode->ID))
						PF_CHECK(Tests, std::any_of(NodeMetrics->Pins.begin(), NodeMetrics->Pins.end(),
							[ConnectedA](const auto& Pin) { return Pin.PinID == ConnectedA->ID && Pin.Connected; }));
					else PF_CHECK(Tests, false);
				}
				else PF_CHECK(Tests, false);
				EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *MaterialSession, ConnectedA->ID, false);
				PF_CHECK(Tests, !MaterialSession->IsDirty());
			}
			const auto BeforeTransfer = DrawFrame(*MaterialSession, "MaterialBeforeTransfer", 1280.0f);
			const auto NativeHost = Documents.CreateNativeHost(EditorDocumentKind::Graph);
			PF_CHECK(Tests, NativeHost.has_value());
			if (NativeHost)
			{
				PF_CHECK(Tests, Documents.Transfer(MaterialSession->Identity, *NativeHost, 0).has_value());
				auto* TransferredSession = Documents.Find(MaterialSession->Identity);
				PF_CHECK(Tests, TransferredSession != nullptr);
				if (TransferredSession)
				{
					const auto AfterTransfer = DrawFrame(*TransferredSession, "MaterialAfterTransfer", 1920.0f);
					ValidateMetrics(AfterTransfer, MaterialGraphForLayout, MaterialSchema, 700.0);
					for (const auto& Node : BeforeTransfer)
					{
						const auto* Transferred = FindPresentation(AfterTransfer, Node.NodeID);
						PF_CHECK(Tests, Transferred && std::abs(Node.Size.X - Transferred->Size.X) < 1.0);
					}
				}
			}
		}
		const auto GameplayBytes = ReadBytes(Project->GetRootPath() / GameplayFixture->ProjectRelativePath);
		std::optional<EditorGraph> GameplayGraphForTest;
		if (GameplayBytes)
			if (auto Deserialized = DeserializeEditorGraph(*GameplayBytes, MakeGameplayEventFixtureSchema()); Deserialized)
				GameplayGraphForTest = std::move(*Deserialized);
		PF_CHECK(Tests, GameplayGraphForTest.has_value());
		const auto GameplaySchema = MakeGameplayEventFixtureSchema();
		if (GameplayGraphForTest) CheckViewportStability(*GameplaySession, *GameplayGraphForTest, GameplaySchema,
			"GameplayLayoutHost", 700.0);

		EditorGraphSchema TestOnlySchema{ "test.layout-content", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true,
			{
				{ "test.layout.source", "Long Source Node Label for Content-Fit Verification",
					{ { "source_output_with_a_long_stable_pin_key", "Source Output", EditorGraphPinDirection::Output,
						EditorGraphPinType::Float, EditorGraphPinCardinality::Multiple, 0.0 },
						{ "source_boolean_signal", "Boolean Signal", EditorGraphPinDirection::Output,
						EditorGraphPinType::Boolean, EditorGraphPinCardinality::Multiple, false } } },
				{ "test.layout.consumer", "Independent Consumer Node",
					{ { "input_with_a_deliberately_long_stable_key", "Long Input", EditorGraphPinDirection::Input,
						EditorGraphPinType::Float, EditorGraphPinCardinality::Single, 0.0 },
						{ "condition_value", "Condition", EditorGraphPinDirection::Input,
						EditorGraphPinType::Boolean, EditorGraphPinCardinality::Single, false },
						{ "consumer_result", "Consumer Result", EditorGraphPinDirection::Output,
						EditorGraphPinType::Float, EditorGraphPinCardinality::Multiple, 0.0 } } }
			} };
		TestOnlySchema.Nodes[1].Pins[0].Hideable = false;
		TestOnlySchema.Nodes[1].Pins[0].Required = true;
		TestOnlySchema.Nodes[1].Pins[1].DefaultControl = EditorGraphPinDefaultControl::Always;
		auto TestOnlyGraph = CreateEditorGraph(TestOnlySchema, GameplaySession->Identity.Asset);
		PF_CHECK(Tests, TestOnlyGraph.has_value());
		if (!TestOnlyGraph) return;
		auto TestSource = CreateEditorGraphNode(TestOnlySchema, "test.layout.source", 120.0, 120.0);
		auto TestConsumer = CreateEditorGraphNode(TestOnlySchema, "test.layout.consumer", 420.0, 120.0);
		PF_CHECK(Tests, TestSource && TestConsumer);
		if (!TestSource || !TestConsumer) return;
		TestOnlyGraph->Nodes.push_back(std::move(*TestSource));
		TestOnlyGraph->Nodes.push_back(std::move(*TestConsumer));
		EditorGraphCommandHistory TestOnlyHistory(*TestOnlyGraph);
		const auto* TestBooleanOutput = FindPin(TestOnlyHistory.Graph(), "test.layout.source", "source_boolean_signal");
		const auto* TestCondition = FindPin(TestOnlyHistory.Graph(), "test.layout.consumer", "condition_value");
		PF_CHECK(Tests, TestBooleanOutput && TestCondition);
		if (TestBooleanOutput && TestCondition)
			PF_CHECK(Tests, ConnectEditorGraphPins(TestOnlyHistory, TestOnlySchema,
				TestBooleanOutput->ID, TestCondition->ID).has_value());
		const EditorGraph TestOnlyGraphForTest = TestOnlyHistory.Graph();
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *GameplaySession,
			TestOnlyHistory.Graph(), TestOnlySchema);
		const auto* RequiredPin = FindPin(TestOnlyGraphForTest, "test.layout.consumer", "input_with_a_deliberately_long_stable_key");
		PF_CHECK(Tests, RequiredPin != nullptr);
		if (RequiredPin)
		{
			EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *GameplaySession, RequiredPin->ID, true);
			const auto RequiredVisibleMetrics = DrawFrame(*GameplaySession, "RequiredPinVisible", 1280.0f);
			const auto ConsumerNode = std::find_if(TestOnlyGraphForTest.Nodes.begin(), TestOnlyGraphForTest.Nodes.end(),
				[](const auto& Node) { return Node.TypeKey == "test.layout.consumer"; });
			PF_CHECK(Tests, ConsumerNode != TestOnlyGraphForTest.Nodes.end());
			const auto* ConsumerMetrics = ConsumerNode == TestOnlyGraphForTest.Nodes.end()
				? nullptr : FindPresentation(RequiredVisibleMetrics, ConsumerNode->ID);
			PF_CHECK(Tests, ConsumerMetrics && std::any_of(ConsumerMetrics->Pins.begin(), ConsumerMetrics->Pins.end(),
				[RequiredPin](const auto& Pin) { return Pin.PinID == RequiredPin->ID; }));
			EditorGraphDocumentEditorTestAccess::SetPinHidden(GraphEditor, *GameplaySession, RequiredPin->ID, false);
		}
		CheckViewportStability(*GameplaySession, TestOnlyGraphForTest, TestOnlySchema,
			"LongLabelLayoutHost", 700.0);

		// Exercise graph clipboard shortcuts through the registered adapter and pinned canvas input path.
		auto& Platform = ImGui::GetPlatformIO();
		Platform.Platform_GetClipboardTextFn = GetGraphTestClipboard;
		Platform.Platform_SetClipboardTextFn = SetGraphTestClipboard;
		GraphTestClipboard.clear();
		GraphTestClipboardRejectWrites = false;
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *GameplaySession, TestOnlyGraphForTest, TestOnlySchema);
		ImGui::ClearActiveID();
		EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *GameplaySession,
			"{\"view\":{\"scroll\":{\"x\":37.0,\"y\":-21.0},\"zoom\":0.65}}");
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1000.0f, 700.0f), false);
		const UUID SourceNodeID = TestOnlyGraphForTest.Nodes[0].ID;
		const UUID ConsumerNodeID = TestOnlyGraphForTest.Nodes[1].ID;
		EditorGraphDocumentEditorTestAccess::SetSelectedNodes(GraphEditor, *GameplaySession,
			{ SourceNodeID, ConsumerNodeID });
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1000.0f, 700.0f), false, false, ImGuiKey_C, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1000.0f, 700.0f), false, false, ImGuiKey_C, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1000.0f, 700.0f), false);
		const std::string CopiedPayload = GraphTestClipboard;
		PF_CHECK(Tests, GraphShortcutSyntheticCtrlObserved);
		PF_CHECK(Tests, GraphShortcutSyntheticKeyObserved);
		PF_CHECK(Tests, GraphShortcutSyntheticHostFocused);
		PF_CHECK(Tests, CopiedPayload.find("PulseForgeGraphClipboard") != std::string::npos);
		const EditorGraph BeforeShortcutPaste = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession);
		const ImVec2 PasteScreenPoint(1010.0f, 720.0f);
		const auto ExpectedPastePoint = EditorGraphDocumentEditorTestAccess::ScreenToCanvas(GraphEditor, *GameplaySession,
			EditorGraphScreenPoint{ PasteScreenPoint.x, PasteScreenPoint.y });
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			PasteScreenPoint, false, false, ImGuiKey_V, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			PasteScreenPoint, false, false, ImGuiKey_V, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1220.0f, 760.0f), false);
		auto AfterShortcutPaste = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession);
		PF_CHECK(Tests, AfterShortcutPaste.Nodes.size() == BeforeShortcutPaste.Nodes.size() + 2 &&
			AfterShortcutPaste.Connections.size() == BeforeShortcutPaste.Connections.size() + 1);
		std::unordered_set<UUID, UUIDHash> OriginalIDs;
		for (const auto& Node : BeforeShortcutPaste.Nodes) OriginalIDs.insert(Node.ID);
		std::vector<const EditorGraphNode*> PastedShortcutNodes;
		for (const auto& Node : AfterShortcutPaste.Nodes)
			if (!OriginalIDs.contains(Node.ID)) PastedShortcutNodes.push_back(&Node);
		PF_CHECK(Tests, PastedShortcutNodes.size() == 2);
		if (PastedShortcutNodes.size() == 2)
		{
			const auto OriginalSource = std::find_if(BeforeShortcutPaste.Nodes.begin(), BeforeShortcutPaste.Nodes.end(),
				[SourceNodeID](const auto& Node) { return Node.ID == SourceNodeID; });
			const auto OriginalConsumer = std::find_if(BeforeShortcutPaste.Nodes.begin(), BeforeShortcutPaste.Nodes.end(),
				[ConsumerNodeID](const auto& Node) { return Node.ID == ConsumerNodeID; });
			const auto PastedSource = std::find_if(PastedShortcutNodes.begin(), PastedShortcutNodes.end(),
				[](const auto* Node) { return Node->TypeKey == "test.layout.source"; });
			const auto PastedConsumer = std::find_if(PastedShortcutNodes.begin(), PastedShortcutNodes.end(),
				[](const auto* Node) { return Node->TypeKey == "test.layout.consumer"; });
			PF_CHECK(Tests, OriginalSource != BeforeShortcutPaste.Nodes.end() && OriginalConsumer != BeforeShortcutPaste.Nodes.end() &&
				PastedSource != PastedShortcutNodes.end() && PastedConsumer != PastedShortcutNodes.end());
			if (OriginalSource != BeforeShortcutPaste.Nodes.end() && OriginalConsumer != BeforeShortcutPaste.Nodes.end() &&
				PastedSource != PastedShortcutNodes.end() && PastedConsumer != PastedShortcutNodes.end())
			{
				PF_CHECK(Tests, std::abs((*PastedSource)->X - std::floor(ExpectedPastePoint.X)) < 0.01 &&
					std::abs((*PastedSource)->Y - std::floor(ExpectedPastePoint.Y)) < 0.01);
				PF_CHECK(Tests, std::abs(((*PastedConsumer)->X - (*PastedSource)->X) -
					(OriginalConsumer->X - OriginalSource->X)) < 0.01 &&
					std::abs(((*PastedConsumer)->Y - (*PastedSource)->Y) -
					(OriginalConsumer->Y - OriginalSource->Y)) < 0.01);
			}
		}
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetSelectedNodes(GraphEditor, *GameplaySession).size() == 2);

		const auto ClipboardBeforeDuplicate = GraphTestClipboard;
		EditorGraphDocumentEditorTestAccess::SetSelectedNodes(GraphEditor, *GameplaySession,
			{ SourceNodeID, ConsumerNodeID });
		const EditorGraph OtherDocumentBeforeDuplicate = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *MaterialSession);
		const EditorGraph BeforeShortcutDuplicate = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession);
		const ImVec2 DuplicateScreenPoint(1080.0f, 690.0f);
		const auto ExpectedDuplicatePoint = EditorGraphDocumentEditorTestAccess::ScreenToCanvas(GraphEditor,
			*GameplaySession, EditorGraphScreenPoint{ DuplicateScreenPoint.x, DuplicateScreenPoint.y });
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			DuplicateScreenPoint, false, false, ImGuiKey_D, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			DuplicateScreenPoint, false, false, ImGuiKey_D, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1280.0f, 800.0f), false);
		auto AfterShortcutDuplicate = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession);
		PF_CHECK(Tests, AfterShortcutDuplicate.Nodes.size() == AfterShortcutPaste.Nodes.size() + 2 &&
			AfterShortcutDuplicate.Connections.size() == AfterShortcutPaste.Connections.size() + 1 &&
			GraphTestClipboard == ClipboardBeforeDuplicate);
		std::unordered_set<UUID, UUIDHash> IDsBeforeDuplicate;
		for (const auto& Node : AfterShortcutPaste.Nodes) IDsBeforeDuplicate.insert(Node.ID);
		std::vector<const EditorGraphNode*> DuplicatedShortcutNodes;
		for (const auto& Node : AfterShortcutDuplicate.Nodes)
			if (!IDsBeforeDuplicate.contains(Node.ID)) DuplicatedShortcutNodes.push_back(&Node);
		PF_CHECK(Tests, DuplicatedShortcutNodes.size() == 2);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetSelectedNodes(GraphEditor, *GameplaySession).size() == 2);
		if (DuplicatedShortcutNodes.size() == 2)
		{
			const double DuplicatedMinimumX = std::min(DuplicatedShortcutNodes[0]->X, DuplicatedShortcutNodes[1]->X);
			const double DuplicatedMinimumY = std::min(DuplicatedShortcutNodes[0]->Y, DuplicatedShortcutNodes[1]->Y);
			PF_CHECK(Tests, std::abs(DuplicatedMinimumX - std::floor(ExpectedDuplicatePoint.X)) < 0.01 &&
				std::abs(DuplicatedMinimumY - std::floor(ExpectedDuplicatePoint.Y)) < 0.01);
			const auto OriginalSource = std::find_if(BeforeShortcutDuplicate.Nodes.begin(), BeforeShortcutDuplicate.Nodes.end(),
				[SourceNodeID](const auto& Node) { return Node.ID == SourceNodeID; });
			const auto OriginalConsumer = std::find_if(BeforeShortcutDuplicate.Nodes.begin(), BeforeShortcutDuplicate.Nodes.end(),
				[ConsumerNodeID](const auto& Node) { return Node.ID == ConsumerNodeID; });
			const auto DuplicatedSource = std::find_if(DuplicatedShortcutNodes.begin(), DuplicatedShortcutNodes.end(),
				[](const auto* Node) { return Node->TypeKey == "test.layout.source"; });
			const auto DuplicatedConsumer = std::find_if(DuplicatedShortcutNodes.begin(), DuplicatedShortcutNodes.end(),
				[](const auto* Node) { return Node->TypeKey == "test.layout.consumer"; });
			PF_CHECK(Tests, OriginalSource != BeforeShortcutDuplicate.Nodes.end() &&
				OriginalConsumer != BeforeShortcutDuplicate.Nodes.end() &&
				DuplicatedSource != DuplicatedShortcutNodes.end() && DuplicatedConsumer != DuplicatedShortcutNodes.end());
			if (OriginalSource != BeforeShortcutDuplicate.Nodes.end() && OriginalConsumer != BeforeShortcutDuplicate.Nodes.end() &&
				DuplicatedSource != DuplicatedShortcutNodes.end() && DuplicatedConsumer != DuplicatedShortcutNodes.end())
			{
				PF_CHECK(Tests, std::abs(DuplicatedMinimumX - std::floor(std::min(OriginalSource->X,
					OriginalConsumer->X) + 32.0)) > 100.0);
				PF_CHECK(Tests, std::abs(((*DuplicatedSource)->X - (*DuplicatedConsumer)->X) -
					(OriginalSource->X - OriginalConsumer->X)) < 0.01 &&
					std::abs(((*DuplicatedSource)->Y - (*DuplicatedConsumer)->Y) -
					(OriginalSource->Y - OriginalConsumer->Y)) < 0.01);
				const auto StillOriginalSource = std::find_if(AfterShortcutDuplicate.Nodes.begin(), AfterShortcutDuplicate.Nodes.end(),
					[SourceNodeID](const auto& Node) { return Node.ID == SourceNodeID; });
				const auto StillOriginalConsumer = std::find_if(AfterShortcutDuplicate.Nodes.begin(), AfterShortcutDuplicate.Nodes.end(),
					[ConsumerNodeID](const auto& Node) { return Node.ID == ConsumerNodeID; });
				PF_CHECK(Tests, StillOriginalSource != AfterShortcutDuplicate.Nodes.end() &&
					StillOriginalConsumer != AfterShortcutDuplicate.Nodes.end());
				if (StillOriginalSource != AfterShortcutDuplicate.Nodes.end() &&
					StillOriginalConsumer != AfterShortcutDuplicate.Nodes.end())
					PF_CHECK(Tests, *StillOriginalSource == *OriginalSource && *StillOriginalConsumer == *OriginalConsumer);
			}
		}
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *MaterialSession) ==
			OtherDocumentBeforeDuplicate);
		PF_CHECK(Tests, ValidateEditorGraph(AfterShortcutDuplicate, TestOnlySchema).empty());
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::HandleCommand(GraphEditor, *GameplaySession,
			EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutPaste);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::HandleCommand(GraphEditor, *GameplaySession,
			EditorDocumentCommand::Redo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutDuplicate);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1280.0f, 800.0f), false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutDuplicate);

		// Single-node duplicate exercises the same placement path above 1:1 zoom.
		EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *GameplaySession,
			"{\"view\":{\"scroll\":{\"x\":-80.0,\"y\":55.0},\"zoom\":1.4}}");
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1120.0f, 740.0f), false);
		EditorGraphDocumentEditorTestAccess::SetSelectedNodes(GraphEditor, *GameplaySession, { SourceNodeID });
		const ImVec2 SingleDuplicateScreenPoint(1120.0f, 740.0f);
		const auto ExpectedSingleDuplicatePoint = EditorGraphDocumentEditorTestAccess::ScreenToCanvas(GraphEditor,
			*GameplaySession, EditorGraphScreenPoint{ SingleDuplicateScreenPoint.x, SingleDuplicateScreenPoint.y });
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			SingleDuplicateScreenPoint, false, false, ImGuiKey_D, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			SingleDuplicateScreenPoint, false, false, ImGuiKey_D, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1300.0f, 810.0f), false);
		const EditorGraph AfterSingleDuplicate = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession);
		PF_CHECK(Tests, AfterSingleDuplicate.Nodes.size() == AfterShortcutDuplicate.Nodes.size() + 1 &&
			AfterSingleDuplicate.Connections == AfterShortcutDuplicate.Connections &&
			GraphTestClipboard == ClipboardBeforeDuplicate);
		const auto SingleDuplicateNode = std::find_if(AfterSingleDuplicate.Nodes.begin(), AfterSingleDuplicate.Nodes.end(),
			[&](const auto& Node) { return std::none_of(AfterShortcutDuplicate.Nodes.begin(), AfterShortcutDuplicate.Nodes.end(),
				[&](const auto& Existing) { return Existing.ID == Node.ID; }); });
		PF_CHECK(Tests, SingleDuplicateNode != AfterSingleDuplicate.Nodes.end());
		if (SingleDuplicateNode != AfterSingleDuplicate.Nodes.end())
			PF_CHECK(Tests, std::abs(SingleDuplicateNode->X - std::floor(ExpectedSingleDuplicatePoint.X)) < 0.01 &&
				std::abs(SingleDuplicateNode->Y - std::floor(ExpectedSingleDuplicatePoint.Y)) < 0.01);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetSelectedNodes(GraphEditor, *GameplaySession).size() == 1);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::HandleCommand(GraphEditor, *GameplaySession,
			EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutDuplicate);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::HandleCommand(GraphEditor, *GameplaySession,
			EditorDocumentCommand::Redo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterSingleDuplicate);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::HandleCommand(GraphEditor, *GameplaySession,
			EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutDuplicate);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(1300.0f, 810.0f), false);

		// A key chord outside the focused graph's canvas is ignored.
		EditorGraphDocumentEditorTestAccess::SetSelectedNodes(GraphEditor, *GameplaySession, { SourceNodeID });
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(10.0f, 10.0f), false, false, ImGuiKey_D, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(10.0f, 10.0f), false, false, ImGuiKey_D, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(10.0f, 10.0f), false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutDuplicate);

		EditorGraphDocumentEditorTestAccess::SetSelectedNodes(GraphEditor, *GameplaySession,
			{ SourceNodeID, ConsumerNodeID });
		GraphTestClipboard = "preserve-on-failure";
		GraphTestClipboardRejectWrites = true;
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(900.0f, 700.0f), false, false, ImGuiKey_X, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(900.0f, 700.0f), false, false, ImGuiKey_X, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(900.0f, 700.0f), false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutDuplicate &&
			GraphTestClipboard == "preserve-on-failure");
		GraphTestClipboardRejectWrites = false;
		EditorGraphDocumentEditorTestAccess::SetSelectedNodes(GraphEditor, *GameplaySession, { ConsumerNodeID });
		const auto BeforeSuccessfulCut = AfterShortcutDuplicate;
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(900.0f, 700.0f), false, false, ImGuiKey_X, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(900.0f, 700.0f), false, false, ImGuiKey_X, true);
		(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
			ImVec2(900.0f, 700.0f), false);
		const auto AfterShortcutCut = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession);
		PF_CHECK(Tests, AfterShortcutCut.Nodes.size() == AfterShortcutDuplicate.Nodes.size() - 1 &&
			AfterShortcutCut.Connections.size() == AfterShortcutDuplicate.Connections.size() - 1 &&
			GraphTestClipboard.find("PulseForgeGraphClipboard") != std::string::npos);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::HandleCommand(GraphEditor, *GameplaySession,
			EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == BeforeSuccessfulCut);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::HandleCommand(GraphEditor, *GameplaySession,
			EditorDocumentCommand::Redo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == AfterShortcutCut);

		// Add through the registered adapter, then delete through the production canvas
		// request path. Returning to the saved graph must clear the session dirty state.
		const auto DeleteBaseline = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession);
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *GameplaySession, DeleteBaseline, TestOnlySchema);
		(void)Documents.MarkSaved(GameplaySession->Identity, GameplaySession->Revision);
		PF_CHECK(Tests, !GameplaySession->IsDirty());
		const auto AddedForDelete = GraphEditor.AddNode(*GameplaySession, "test.layout.consumer", 760.0, 420.0);
		PF_CHECK(Tests, AddedForDelete.has_value() && GameplaySession->IsDirty());
		if (AddedForDelete)
		{
			(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
				ImVec2(900.0f, 650.0f), false);
			EditorGraphDocumentEditorTestAccess::SetSelectedNodes(GraphEditor, *GameplaySession, { *AddedForDelete });
			(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
				ImVec2(900.0f, 650.0f), false, false, ImGuiKey_Delete, true);
			(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
				ImVec2(900.0f, 650.0f), false, false, ImGuiKey_Delete, true);
			(void)DrawInputFrame(*GameplaySession, "GraphShortcutHost", 1440.0f, ImVec2(29.0f, 41.0f),
				ImVec2(900.0f, 650.0f), false);
			PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *GameplaySession) == DeleteBaseline);
			PF_CHECK(Tests, !GameplaySession->IsDirty());
		}
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

		// Returning an edit to the exact saved graph through the registered Undo route
		// must reconcile the session to clean instead of leaving a false dirty revision.
		Edit = Process(Final, true, true, true);
		PF_CHECK(Tests, Edit.Error.empty() && Edit.GraphChanged && Session->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, SourceValue(ReadBytes(SourcePath)) == std::optional<EditorGraphVector3>(Middle));

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
	TestGraphNodeSizingInProductionAdapter(Tests);
	TestGraphDocumentSaveRevisionAccounting(Tests);
	TestGraphCanvasCreateRequestPolling(Tests);
	TestGraphCanvasNodeCreationRequestPolling(Tests);
	TestGraphNodeSearchNavigation(Tests);
	TestGraphCanvasCoordinateOwnership(Tests);
	TestAltPinUnlinkInputOwnership(Tests);
	TestGraphStraightenShortcutOwnership(Tests);
	TestPinnedCanvasLinkGesture(Tests);
}
