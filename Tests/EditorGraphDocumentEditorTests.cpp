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
#include <tuple>
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

		static EditorGraphReconnectPopupPresentation GetReconnectPopup(
			EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetReconnectPopupForTesting(Session);
		}

		static EditorGraphPinMovePresentation GetPinMove(
			EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetPinMoveForTesting(Session);
		}

		static EditorGraphWireRoutePresentation GetWireRoute(
			EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetWireRouteForTesting(Session);
		}

		static bool IsCanvasInputSuppressed(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.IsCanvasInputSuppressedForTesting(Session);
		}

		static PulseForge::UUID GetLastHoveredPin(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetLastHoveredPinForTesting(Session);
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

		static EditorGraphScreenPoint CanvasToScreen(EditorGraphDocumentEditor& Editor,
			EditorDocumentSession& Session, EditorGraphCanvasPoint CanvasPoint)
		{
			return Editor.CanvasToScreenForTesting(Session, CanvasPoint);
		}

		static PulseForge::UUID GetHoveredConnection(EditorGraphDocumentEditor& Editor, EditorDocumentSession& Session)
		{
			return Editor.GetHoveredConnectionForTesting(Session);
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
			ImGuiKey PressedKey = ImGuiKey_None, bool Control = false, bool RightDown = false)
		{
			IO.DisplaySize = ImVec2(Width, 900.0f);
			IO.AddMousePosEvent(MousePosition.x, MousePosition.y);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, LeftDown);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Right, RightDown);
			IO.AddKeyEvent(ImGuiKey_Escape, Escape);
			IO.AddKeyEvent(ImGuiMod_Ctrl, Control);
			for (const ImGuiKey Key : { ImGuiKey_C, ImGuiKey_V, ImGuiKey_X, ImGuiKey_D, ImGuiKey_Delete })
				IO.AddKeyEvent(Key, Key == PressedKey);
			ImGui::NewFrame();
			if (Host == "GraphShortcutHost" || Host == "GraphReconnectHost") ImGui::SetNextWindowFocus();
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

	void TestGraphCanvasReconnectContextMenu(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.LogFilename = nullptr;
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		IO.DeltaTime = 1.0f / 60.0f;

		TemporaryGraphProject Temporary;
		const auto ProjectID = UUID::Generate();
		PF_CHECK(Tests, ProjectID.has_value());
		if (!ProjectID) return;
		Temporary.Path = std::filesystem::temp_directory_path() / ProjectID->ToString();
		std::error_code FileError;
		std::filesystem::create_directories(Temporary.Path, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		auto Project = PulseForge::Project::Create(Temporary.Path / "GraphReconnectProject.pfproj", "Graph reconnect test");
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
		PF_CHECK(Tests, Documents.Open(Identity, Fixture->ProjectRelativePath, Fixture->DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip).has_value());
		auto* Session = Documents.Find(Identity);
		const EditorDocumentEditorContext LoadContext{ Identity, Project->GetRootPath(), Fixture->ProjectRelativePath,
			Fixture->DisplayName, nullptr };
		PF_CHECK(Tests, Session && Editors.Load(*Session, LoadContext));
		if (!Session) return;

		const auto BaseSchema = MakeMaterialExpressionFixtureSchema();
		auto Graph = CreateEditorGraph(BaseSchema, Fixture->GraphID);
		PF_CHECK(Tests, Graph.has_value());
		if (!Graph) return;
		const auto AddNode = [&](std::string_view Type, double X, double Y)
		{
			auto Node = CreateEditorGraphNode(BaseSchema, Type, X, Y);
			PF_CHECK(Tests, Node.has_value());
			if (!Node) return UUID{};
			const UUID IDValue = Node->ID;
			Graph->Nodes.push_back(std::move(*Node));
			return IDValue;
		};
		const UUID SourceA = AddNode("material.scalar", 100.0, 80.0);
		const UUID SourceB = AddNode("material.scalar", 100.0, 350.0);
		const UUID SourceC = AddNode("material.scalar", 100.0, 620.0);
		const UUID TargetA = AddNode("material.multiply", 480.0, 80.0);
		const UUID TargetB = AddNode("material.multiply", 480.0, 350.0);
		const UUID TargetC = AddNode("material.multiply", 820.0, 80.0);
		const UUID TargetD = AddNode("material.multiply", 820.0, 350.0);
		const UUID VectorTarget = AddNode("material.output", 1120.0, 80.0);
		const auto PinID = [](const EditorGraph& Source, UUID NodeID, std::string_view Key)
		{
			const auto Node = std::find_if(Source.Nodes.begin(), Source.Nodes.end(), [NodeID](const auto& Item)
				{ return Item.ID == NodeID; });
			if (Node == Source.Nodes.end()) return UUID{};
			const auto Pin = std::find_if(Node->Pins.begin(), Node->Pins.end(), [Key](const auto& Item)
				{ return Item.Key == Key; });
			return Pin == Node->Pins.end() ? UUID{} : Pin->ID;
		};
		const UUID SourceAValue = PinID(*Graph, SourceA, "value");
		const UUID SourceBValue = PinID(*Graph, SourceB, "value");
		const UUID SourceCValue = PinID(*Graph, SourceC, "value");
		const UUID TargetAA = PinID(*Graph, TargetA, "a");
		const UUID TargetAB = PinID(*Graph, TargetA, "b");
		const UUID TargetBA = PinID(*Graph, TargetB, "a");
		const UUID TargetCA = PinID(*Graph, TargetC, "a");
		const UUID TargetDA = PinID(*Graph, TargetD, "a");
		const UUID VectorSurface = PinID(*Graph, VectorTarget, "surface");
		PF_CHECK(Tests, !SourceAValue.IsNil() && !SourceBValue.IsNil() && !SourceCValue.IsNil() &&
			!TargetAA.IsNil() && !TargetAB.IsNil() && !TargetBA.IsNil() && !TargetCA.IsNil() &&
			!TargetDA.IsNil() && !VectorSurface.IsNil());
		if (SourceAValue.IsNil() || SourceBValue.IsNil() || SourceCValue.IsNil() || TargetAA.IsNil() ||
			TargetAB.IsNil() || TargetBA.IsNil() || TargetCA.IsNil() || TargetDA.IsNil() || VectorSurface.IsNil()) return;

		EditorGraphCommandHistory Setup(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(Setup, BaseSchema, SourceAValue, TargetAA).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(Setup, BaseSchema, SourceAValue, TargetBA).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(Setup, BaseSchema, SourceCValue, TargetCA).has_value());
		Graph = Setup.Graph();
		const auto LinkID = [&](const EditorGraph& Source, UUID Output, UUID Input)
		{
			const auto Link = std::find_if(Source.Connections.begin(), Source.Connections.end(),
				[Output, Input](const auto& Item) { return Item.OutputPin == Output && Item.InputPin == Input; });
			return Link == Source.Connections.end() ? UUID{} : Link->ID;
		};
		const auto PositionsMatch = [](const EditorGraph& Left, const EditorGraph& Right)
		{
			if (Left.Nodes.size() != Right.Nodes.size()) return false;
			for (const auto& Node : Left.Nodes)
			{
				const auto Other = std::find_if(Right.Nodes.begin(), Right.Nodes.end(), [&Node](const auto& Candidate)
					{ return Candidate.ID == Node.ID; });
				if (Other == Right.Nodes.end() || Node.X != Other->X || Node.Y != Other->Y) return false;
			}
			return true;
		};
		const UUID PrimaryLink = LinkID(*Graph, SourceAValue, TargetAA);
		const UUID FanOutLink = LinkID(*Graph, SourceAValue, TargetBA);
		const UUID OccupiedLink = LinkID(*Graph, SourceCValue, TargetCA);
		PF_CHECK(Tests, !PrimaryLink.IsNil() && !FanOutLink.IsNil() && !OccupiedLink.IsNil());
		const EditorGraph Baseline = *Graph;
		bool AllowUnconnectedSourceAConnection = false;
		bool AllowPinB = false;
		auto Schema = BaseSchema;
		Schema.ReroutePolicyAware = true;
		Schema.ConnectionPolicy = [SourceAValue, &AllowUnconnectedSourceAConnection, &AllowPinB](const EditorGraph&,
			const EditorGraphConnectionPolicyContext& Context)
			-> std::optional<std::string>
		{
			if (Context.Purpose == EditorGraphConnectionPolicyPurpose::ProposedOperation &&
				Context.Output.PinID == SourceAValue && Context.RemovedConnectionIDs.empty() &&
				!AllowUnconnectedSourceAConnection)
				return "This test policy requires the original wire to be removed during preview";
			if (Context.Purpose == EditorGraphConnectionPolicyPurpose::ProposedOperation &&
				Context.Input.PinKey == "b" && !AllowPinB) return "This test schema forbids connections to input b";
			return std::nullopt;
		};
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Baseline, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const auto BaselineBytes = ReadBytes(Project->GetRootPath() / Fixture->ProjectRelativePath);
		PF_CHECK(Tests, BaselineBytes.has_value());
		EditorGraphDocumentEditorTestAccess::CapturePresentation(GraphEditor, *Session, true);

		ImVec2 HostOrigin(73.0f, 37.0f);
		bool FocusReconnectHost = true;
		bool SawGraphNodeSearchPopup = false;
		bool LastSyntheticLeftDragging = false;
		bool LastQueuedCtrlLeftPress = false;
		const auto DrawFrame = [&](ImVec2 Mouse, bool LeftDown = false, bool RightDown = false,
			bool Escape = false, bool Control = false, std::optional<EditorGraphCanvasRect> ScreenClip = std::nullopt,
			bool ProcessQueuedInputTogether = false, bool RDown = false)
		{
			IO.DisplaySize = ImVec2(1440.0f, 900.0f);
			IO.AddMousePosEvent(Mouse.x, Mouse.y);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, LeftDown);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Right, RightDown);
			IO.MouseDown[ImGuiMouseButton_Right] = RightDown;
			IO.AddKeyEvent(ImGuiKey_Escape, Escape);
			IO.AddKeyEvent(ImGuiKey_R, RDown);
			IO.AddKeyEvent(ImGuiMod_Ctrl, Control);
			const bool PreviousInputTrickle = IO.ConfigInputTrickleEventQueue;
			if (ProcessQueuedInputTogether) IO.ConfigInputTrickleEventQueue = false;
			ImGui::NewFrame();
			IO.ConfigInputTrickleEventQueue = PreviousInputTrickle;
			LastQueuedCtrlLeftPress = ImGui::IsKeyDown(ImGuiMod_Ctrl) &&
				ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) &&
				IO.MouseClickedPos[ImGuiMouseButton_Left].x == std::floor(Mouse.x) &&
				IO.MouseClickedPos[ImGuiMouseButton_Left].y == std::floor(Mouse.y);
			IO.MousePos = Mouse;
			IO.MouseDown[ImGuiMouseButton_Left] = LeftDown;
			IO.MouseDown[ImGuiMouseButton_Right] = RightDown;
			IO.KeyCtrl = Control;
			LastSyntheticLeftDragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f);
			auto* EscapeKey = ImGui::GetKeyData(ImGuiKey_Escape);
			EscapeKey->Down = Escape;
			EscapeKey->DownDuration = Escape ? 0.0f : -1.0f;
			auto* RKey = ImGui::GetKeyData(ImGuiKey_R);
			RKey->Down = RDown;
			RKey->DownDuration = RDown ? 0.0f : -1.0f;
			if (FocusReconnectHost)
			{
				ImGui::SetNextWindowFocus();
				FocusReconnectHost = false;
			}
			ImGui::SetNextWindowPos(HostOrigin);
			ImGui::SetNextWindowSize(ImVec2(1367.0f, 850.0f));
			ImGui::Begin("GraphReconnectHost", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			if (ScreenClip)
				ImGui::GetWindowDrawList()->PushClipRect(
					ImVec2(static_cast<float>(ScreenClip->Minimum.X), static_cast<float>(ScreenClip->Minimum.Y)),
					ImVec2(static_cast<float>(ScreenClip->Maximum.X), static_cast<float>(ScreenClip->Maximum.Y)), true);
			EditorGraphDocumentEditorTestAccess::Draw(GraphEditor, *Session);
			SawGraphNodeSearchPopup |= ImGui::IsPopupOpen("GraphNodeSearch");
			if (ScreenClip) ImGui::GetWindowDrawList()->PopClipRect();
			ImGui::End();
			ImGui::Render();
		};
		const auto FindPinScreen = [&](UUID Pin)
		{
			DrawFrame(ImVec2(-1000.0f, -1000.0f));
			const auto Metrics = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
			for (const auto& Node : Metrics)
				for (const auto& PinMetric : Node.Pins)
					if (PinMetric.PinID == Pin)
					{
						const auto Screen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(GraphEditor, *Session,
							PinMetric.Marker.Anchor);
						return ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y));
					}
			return ImVec2(-1000.0f, -1000.0f);
		};
		const auto FindLinkPoint = [&](UUID Connection)
		{
			const auto CurrentGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
			const auto Link = std::find_if(CurrentGraph.Connections.begin(), CurrentGraph.Connections.end(),
				[Connection](const auto& Item) { return Item.ID == Connection; });
			if (Link == CurrentGraph.Connections.end()) return std::optional<ImVec2>{};
			const ImVec2 Start = FindPinScreen(Link->OutputPin);
			const ImVec2 End = FindPinScreen(Link->InputPin);
			const ImVec2 Center((Start.x + End.x) * 0.5f, (Start.y + End.y) * 0.5f);
			for (int Y = -24; Y <= 24; Y += 4)
				for (int X = -16; X <= 16; X += 4)
				{
					const ImVec2 Point(Center.x + static_cast<float>(X), Center.y + static_cast<float>(Y));
					DrawFrame(Point);
					if (EditorGraphDocumentEditorTestAccess::GetHoveredConnection(GraphEditor, *Session) == Connection)
						return std::optional<ImVec2>{ Point };
				}
			return std::optional<ImVec2>{};
		};
		const auto OpenReconnectPopup = [&](UUID Connection)
		{
			const auto Point = FindLinkPoint(Connection);
			PF_CHECK(Tests, Point.has_value());
			if (!Point) return EditorGraphReconnectPopupPresentation{};
			DrawFrame(*Point, false, true);
			DrawFrame(*Point);
			auto Popup = EditorGraphDocumentEditorTestAccess::GetReconnectPopup(GraphEditor, *Session);
			bool SawContextRequest = Popup.LinkContextRequestSeen;
			bool SawPopupOpen = Popup.PopupOpenAfterSubmit;
			for (int Frame = 0; Frame < 4 && (!Popup.PopupVisible || !Popup.PopupScreenBounds ||
				Popup.PopupScreenBounds->Maximum.X - Popup.PopupScreenBounds->Minimum.X < 70.0 ||
				Popup.PopupScreenBounds->Maximum.Y - Popup.PopupScreenBounds->Minimum.Y < 40.0); ++Frame)
			{
				DrawFrame(*Point);
				Popup = EditorGraphDocumentEditorTestAccess::GetReconnectPopup(GraphEditor, *Session);
				SawContextRequest = SawContextRequest || Popup.LinkContextRequestSeen;
				SawPopupOpen = SawPopupOpen || Popup.PopupOpenAfterSubmit;
			}
			PF_CHECK(Tests, SawPopupOpen && Popup.PopupVisible && Popup.ContextValid);
			return Popup;
		};
		const auto OpenTargetSelection = [&](UUID Connection, EditorGraphPinDirection Direction)
		{
			auto Popup = OpenReconnectPopup(Connection);
			const auto Menu = Direction == EditorGraphPinDirection::Input
				? Popup.ReconnectInputBounds : Popup.ReconnectOutputBounds;
			PF_CHECK(Tests, Popup.ContextValid && Popup.PopupVisible && Menu.has_value());
			if (!Menu) return EditorGraphPinMovePresentation{};
			const ImVec2 MenuPoint(static_cast<float>((Menu->Minimum.X + Menu->Maximum.X) * 0.5),
				static_cast<float>((Menu->Minimum.Y + Menu->Maximum.Y) * 0.5));
			DrawFrame(MenuPoint, true);
			DrawFrame(MenuPoint);
			const auto Move = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
			PF_CHECK(Tests, Move.Active && Move.AwaitingTargetClick && Move.Direction == Direction);
			return Move;
		};
		const auto ClickCanvasPin = [&](UUID Pin)
		{
			const ImVec2 Point = FindPinScreen(Pin);
			DrawFrame(Point, true);
			DrawFrame(Point);
			return Point;
		};

		DrawFrame(ImVec2(-1000.0f, -1000.0f));
		auto RejectedPopup = OpenReconnectPopup(PrimaryLink);
		PF_CHECK(Tests, RejectedPopup.ContextValid && RejectedPopup.PopupVisible &&
			RejectedPopup.PopupScreenBounds.has_value());
		PF_CHECK(Tests, RejectedPopup.NoMove && RejectedPopup.NoResize);
		const EditorGraph BeforeRejectedSelection = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const uint64_t RevisionBeforeRejectedSelection = Session->Revision;
		auto RejectedMove = OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Input);
		const ImVec2 IncompatiblePoint = FindPinScreen(VectorSurface);
		DrawFrame(IncompatiblePoint);
		RejectedMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, RejectedMove.Active && !RejectedMove.TargetAllowed &&
			RejectedMove.Feedback.find("exactly") != std::string::npos);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == BeforeRejectedSelection);
		PF_CHECK(Tests, Session->Revision == RevisionBeforeRejectedSelection && !Session->IsDirty());
		DrawFrame(ImVec2(-1000.0f, -1000.0f), false, false, true);
		DrawFrame(ImVec2(-1000.0f, -1000.0f));
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);

		auto DomainMove = OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Input);
		const ImVec2 ForbiddenPoint = FindPinScreen(TargetAB);
		DrawFrame(ForbiddenPoint);
		DomainMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, DomainMove.Active && !DomainMove.TargetAllowed &&
			DomainMove.Feedback.find("This test schema forbids connections to input b") != std::string::npos);
		DrawFrame(ImVec2(-1000.0f, -1000.0f), false, false, true);
		DrawFrame(ImVec2(-1000.0f, -1000.0f));

		// Move one input endpoint while preserving the selected wire and its fan-out sibling.
		(void)OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Input);
		const ImVec2 FreeInputPoint = ClickCanvasPin(TargetDA);
		DrawFrame(FreeInputPoint, false);
		auto AfterInputMove = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterInputMove, SourceAValue, TargetDA) == PrimaryLink);
		PF_CHECK(Tests, LinkID(AfterInputMove, SourceAValue, TargetBA) == FanOutLink);
		PF_CHECK(Tests, LinkID(AfterInputMove, SourceCValue, TargetCA) == OccupiedLink);
		PF_CHECK(Tests, PositionsMatch(AfterInputMove, Baseline));
		PF_CHECK(Tests, Session->IsDirty());
		auto StalePopup = OpenReconnectPopup(PrimaryLink);
		PF_CHECK(Tests, StalePopup.PopupVisible && StalePopup.ContextValid);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline && !Session->IsDirty());
		DrawFrame(ImVec2(600.0f, 500.0f));
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetReconnectPopup(GraphEditor, *Session).PopupVisible);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		AfterInputMove = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterInputMove, SourceAValue, TargetDA) == PrimaryLink && Session->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));

		// Move the output of exactly one shared-output wire; its sibling remains untouched.
		(void)OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Output);
		const ImVec2 ReplacementOutputPoint = ClickCanvasPin(SourceBValue);
		DrawFrame(ReplacementOutputPoint, false);
		auto AfterOutputMove = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterOutputMove, SourceBValue, TargetAA) == PrimaryLink);
		PF_CHECK(Tests, LinkID(AfterOutputMove, SourceAValue, TargetBA) == FanOutLink);
		PF_CHECK(Tests, LinkID(AfterOutputMove, SourceCValue, TargetCA) == OccupiedLink);
		PF_CHECK(Tests, PositionsMatch(AfterOutputMove, Baseline));
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == AfterOutputMove);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));

		// Ctrl-drag on a fan-out output moves every incident wire, even if Ctrl is released before the drag threshold.
		const ImVec2 SourceAOutputPoint = FindPinScreen(SourceAValue);
		const ImVec2 SourceBOutputPoint = FindPinScreen(SourceBValue);
		// Reproduce the reported sequence through the production adapter: the pointer was hovered on
		// the previous frame, then moves one pixel and presses Ctrl+left in the same new frame.
		const ImVec2 FreeInputForWrongDirection = FindPinScreen(TargetDA);
		const auto BeforeWrongDirectionCtrlDrop = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const uint64_t WrongDirectionCtrlRevision = Session->Revision;
		// Let an accidental ordinary Scalar-to-input request succeed so this regression proves
		// ownership, not merely that the schema would reject a leaked create request.
		AllowUnconnectedSourceAConnection = true;
		SawGraphNodeSearchPopup = false;
		const ImVec2 RetrySourceAOutputPoint = FindPinScreen(SourceAValue);
		const ImVec2 RetryOnePixelPressPoint(RetrySourceAOutputPoint.x + 1.0f, RetrySourceAOutputPoint.y);
		DrawFrame(RetrySourceAOutputPoint);
		DrawFrame(RetryOnePixelPressPoint, true, false, false, true, std::nullopt, true);
		PF_CHECK(Tests, LastQueuedCtrlLeftPress);
		auto OnePixelPressMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, OnePixelPressMove.Active && OnePixelPressMove.CtrlDrag &&
			!OnePixelPressMove.Dragging && OnePixelPressMove.SourcePin == SourceAValue &&
			OnePixelPressMove.MovingConnectionCount == 2);
		DrawFrame(FreeInputForWrongDirection, true);
		OnePixelPressMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, OnePixelPressMove.Active && !OnePixelPressMove.TargetAllowed &&
			OnePixelPressMove.Feedback.find("same direction") != std::string::npos);
		// Keep the pointer at the destination long enough that the pinned native link action
		// would complete if this press had escaped move ownership.
		DrawFrame(FreeInputForWrongDirection, true);
		DrawFrame(FreeInputForWrongDirection, true);
		DrawFrame(FreeInputForWrongDirection, false);
		DrawFrame(FreeInputForWrongDirection);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) ==
			BeforeWrongDirectionCtrlDrop && Session->Revision == WrongDirectionCtrlRevision && !Session->IsDirty());
		PF_CHECK(Tests, !SawGraphNodeSearchPopup);
		AllowUnconnectedSourceAConnection = false;

		// A captured move released over blank canvas is consumed and cannot open node search.
		const ImVec2 BlankReleaseSourcePoint = FindPinScreen(SourceAValue);
		const ImVec2 BlankReleasePressPoint(BlankReleaseSourcePoint.x + 1.0f, BlankReleaseSourcePoint.y);
		DrawFrame(BlankReleaseSourcePoint);
		DrawFrame(BlankReleasePressPoint, true, false, false, true);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		const ImVec2 BlankCanvasPoint(HostOrigin.x + 900.0f, HostOrigin.y + 650.0f);
		DrawFrame(BlankCanvasPoint, true);
		const auto BlankPreview = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, BlankPreview.Active && BlankPreview.TargetPin.IsNil());
		DrawFrame(BlankCanvasPoint, false);
		DrawFrame(BlankCanvasPoint);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) ==
			BeforeWrongDirectionCtrlDrop && Session->Revision == WrongDirectionCtrlRevision && !Session->IsDirty());
		PF_CHECK(Tests, !SawGraphNodeSearchPopup);
		// Reset explicitly so a pre-fix ordinary link cannot contaminate later assertions.
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Baseline, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *Session,
			"{\"view\":{\"scroll\":{\"x\":37.0,\"y\":-21.0},\"zoom\":0.85}}");
		// SetGraphForTesting replaces model identities. Let the production adapter
		// synchronize its canvas before reusing screen coordinates for another gesture.
		DrawFrame(ImVec2(-1000.0f, -1000.0f));
		const ImVec2 ResetSourceAOutputPoint = FindPinScreen(SourceAValue);
		const ImVec2 ResetSourceBOutputPoint = FindPinScreen(SourceBValue);
		const ImVec2 FixedInputA = FindPinScreen(TargetAA);
		const ImVec2 FixedInputB = FindPinScreen(TargetBA);
		const auto PreviewNodeMetrics = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
		const auto FindPinOwnerID = [&](UUID PinID)
		{
			for (const auto& Node : Baseline.Nodes)
				if (std::any_of(Node.Pins.begin(), Node.Pins.end(), [PinID](const auto& Pin) { return Pin.ID == PinID; }))
					return Node.ID;
			return UUID{};
		};
		const auto IsInsideNodeBody = [&](EditorGraphScreenPoint Point, UUID NodeID)
		{
			const auto Metric = std::find_if(PreviewNodeMetrics.begin(), PreviewNodeMetrics.end(), [NodeID](const auto& Candidate)
				{ return Candidate.NodeID == NodeID; });
			if (Metric == PreviewNodeMetrics.end()) return false;
			const auto Minimum = EditorGraphDocumentEditorTestAccess::CanvasToScreen(GraphEditor, *Session, Metric->Position);
			const auto Maximum = EditorGraphDocumentEditorTestAccess::CanvasToScreen(GraphEditor, *Session,
				{ Metric->Position.X + Metric->Size.X, Metric->Position.Y + Metric->Size.Y });
			return Point.X > Minimum.X && Point.X < Maximum.X && Point.Y > Minimum.Y && Point.Y < Maximum.Y;
		};
		const auto FindNodeCenterByID = [&](UUID NodeID)
		{
			const auto Metric = std::find_if(PreviewNodeMetrics.begin(), PreviewNodeMetrics.end(), [NodeID](const auto& Candidate)
				{ return Candidate.NodeID == NodeID; });
			if (Metric == PreviewNodeMetrics.end()) return ImVec2(-1000.0f, -1000.0f);
			const auto Screen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(GraphEditor, *Session,
				{ Metric->Position.X + Metric->Size.X * 0.5, Metric->Position.Y + Metric->Size.Y * 0.5 });
			return ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y));
		};
		const ImVec2 PreviewCursorA = FindNodeCenterByID(SourceB);
		const UUID PreviewCursorNodeA = SourceB;
		const auto PreviewCursorNodeB = std::find_if(Baseline.Nodes.begin(), Baseline.Nodes.end(),
			[SourceC](const auto& Node) { return Node.ID == SourceC; });
		const ImVec2 PreviewCursorB = FindNodeCenterByID(SourceC);
		const ImVec2 ResetOnePixelPressPoint(ResetSourceAOutputPoint.x + 1.0f, ResetSourceAOutputPoint.y);

		const auto BeforeCtrlOutput = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const uint64_t CtrlOutputRevision = Session->Revision;
		DrawFrame(ResetSourceAOutputPoint);
		DrawFrame(ResetOnePixelPressPoint, true, false, false, true, std::nullopt, true);
		PF_CHECK(Tests, LastQueuedCtrlLeftPress);
		auto PinMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, PinMove.Active && PinMove.CtrlDrag && !PinMove.Dragging && PinMove.MovingConnectionCount == 2);
		DrawFrame(ResetOnePixelPressPoint, true, false, false, false);
		PinMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, PinMove.Active && PinMove.CtrlDrag && !PinMove.Dragging);
		DrawFrame(PreviewCursorA, true, false, false, false);
		const auto OutputPreviewA = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		DrawFrame(PreviewCursorB, true, false, false, false);
		const auto OutputPreviewB = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, OutputPreviewA.Dragging && OutputPreviewA.PreviewCurves.size() == 2 &&
			OutputPreviewB.Dragging && OutputPreviewB.PreviewCurves.size() == 2);
		PF_CHECK(Tests, OutputPreviewA.PreviewSubmittedOnCanvasUnderlay &&
			OutputPreviewB.PreviewSubmittedOnCanvasUnderlay);
		PF_CHECK(Tests, PreviewCursorA.x > HostOrigin.x && PreviewCursorA.y > HostOrigin.y &&
			PreviewCursorB.x > HostOrigin.x && PreviewCursorB.y > HostOrigin.y);
		if (OutputPreviewA.PreviewCurves.size() == 2 && OutputPreviewB.PreviewCurves.size() == 2)
		{
			PF_CHECK(Tests, PreviewCursorNodeB != Baseline.Nodes.end() &&
				IsInsideNodeBody({ OutputPreviewA.PreviewCurves[0].End.X, OutputPreviewA.PreviewCurves[0].End.Y },
					FindPinOwnerID(TargetAA)) &&
				IsInsideNodeBody({ OutputPreviewA.PreviewCurves[1].End.X, OutputPreviewA.PreviewCurves[1].End.Y },
					FindPinOwnerID(TargetBA)) &&
				IsInsideNodeBody({ PreviewCursorA.x, PreviewCursorA.y }, PreviewCursorNodeA) &&
				IsInsideNodeBody({ PreviewCursorB.x, PreviewCursorB.y }, PreviewCursorNodeB->ID));
			PF_CHECK(Tests, std::abs(OutputPreviewA.PreviewCurves[0].Start.X - PreviewCursorA.x) < 1.0 &&
				std::abs(OutputPreviewA.PreviewCurves[1].Start.X - PreviewCursorA.x) < 1.0);
			PF_CHECK(Tests, std::abs(OutputPreviewB.PreviewCurves[0].Start.X - PreviewCursorB.x) < 1.0 &&
				std::abs(OutputPreviewB.PreviewCurves[1].Start.X - PreviewCursorB.x) < 1.0);
			PF_CHECK(Tests, std::abs(OutputPreviewA.PreviewCurves[0].End.X - FixedInputA.x) < 1.0 &&
				std::abs(OutputPreviewB.PreviewCurves[0].End.X - FixedInputA.x) < 1.0 &&
				std::abs(OutputPreviewA.PreviewCurves[0].End.Y - FixedInputA.y) < 1.0 &&
				std::abs(OutputPreviewB.PreviewCurves[0].End.Y - FixedInputA.y) < 1.0);
			PF_CHECK(Tests, std::abs(OutputPreviewA.PreviewCurves[1].End.X - FixedInputB.x) < 1.0 &&
				std::abs(OutputPreviewB.PreviewCurves[1].End.X - FixedInputB.x) < 1.0 &&
				std::abs(OutputPreviewA.PreviewCurves[1].End.Y - FixedInputB.y) < 1.0 &&
				std::abs(OutputPreviewB.PreviewCurves[1].End.Y - FixedInputB.y) < 1.0);
		}
		DrawFrame(ResetSourceBOutputPoint, true, false, false, false);
		PinMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, PinMove.Active);
		PF_CHECK(Tests, PinMove.Dragging);
		PF_CHECK(Tests, PinMove.TargetAllowed);
		PF_CHECK(Tests, PinMove.MovingConnectionCount == 2);
		PF_CHECK(Tests, PinMove.PreviewDecisionEvaluations == 1);
		DrawFrame(ResetSourceBOutputPoint, false, false, false, false);
		auto AfterCtrlOutput = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterCtrlOutput, SourceBValue, TargetAA) == PrimaryLink &&
			LinkID(AfterCtrlOutput, SourceBValue, TargetBA) == FanOutLink &&
			Session->Revision == CtrlOutputRevision + 1);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == BeforeCtrlOutput);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == AfterCtrlOutput);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));

		// A Ctrl-drag from one input moves that endpoint while preserving its wire identity.
		const ImVec2 InputSourcePoint = FindPinScreen(TargetAA);
		const ImVec2 InputTargetPoint = FindPinScreen(TargetDA);
		const ImVec2 FixedOutputForInputMove = FindPinScreen(SourceAValue);
		DrawFrame(ImVec2(-1000.0f, -1000.0f));
		DrawFrame(ImVec2(InputSourcePoint.x + 1.0f, InputSourcePoint.y), true, false, false, true, std::nullopt, true);
		PF_CHECK(Tests, LastQueuedCtrlLeftPress);
		PinMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, PinMove.Active && PinMove.SourcePin == TargetAA &&
			PinMove.MovingConnectionCount == 1);
		DrawFrame(PreviewCursorA, true);
		const auto InputPreviewA = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		DrawFrame(PreviewCursorB, true);
		const auto InputPreviewB = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, InputPreviewA.PreviewCurves.size() == 1 && InputPreviewB.PreviewCurves.size() == 1 &&
			InputPreviewA.PreviewSubmittedOnCanvasUnderlay && InputPreviewB.PreviewSubmittedOnCanvasUnderlay);
		if (InputPreviewA.PreviewCurves.size() == 1 && InputPreviewB.PreviewCurves.size() == 1)
			PF_CHECK(Tests, std::abs(InputPreviewA.PreviewCurves[0].Start.X - FixedOutputForInputMove.x) < 1.0 &&
				std::abs(InputPreviewB.PreviewCurves[0].Start.X - FixedOutputForInputMove.x) < 1.0 &&
				std::abs(InputPreviewA.PreviewCurves[0].Start.Y - FixedOutputForInputMove.y) < 1.0 &&
				std::abs(InputPreviewB.PreviewCurves[0].Start.Y - FixedOutputForInputMove.y) < 1.0 &&
				std::abs(InputPreviewA.PreviewCurves[0].End.X - PreviewCursorA.x) < 1.0 &&
				std::abs(InputPreviewB.PreviewCurves[0].End.X - PreviewCursorB.x) < 1.0);
		DrawFrame(InputTargetPoint, true, false, false, false);
		PinMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, PinMove.Active && PinMove.Dragging && PinMove.Direction == EditorGraphPinDirection::Input &&
			PinMove.TargetAllowed && PinMove.MovingConnectionCount == 1);
		DrawFrame(InputTargetPoint, false);
		const auto AfterCtrlInput = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterCtrlInput, SourceAValue, TargetDA) == PrimaryLink &&
			LinkID(AfterCtrlInput, SourceAValue, TargetBA) == FanOutLink);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline);

		// A mismatched direction is visible as rejected feedback and dropping cancels without editing.
		const ImVec2 InvalidInputPoint = FindPinScreen(TargetDA);
		DrawFrame(SourceAOutputPoint);
		DrawFrame(SourceAOutputPoint, true, false, false, true);
		DrawFrame(InvalidInputPoint, true, false, false, false);
		PinMove = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, PinMove.Active && !PinMove.TargetAllowed &&
			PinMove.Feedback.find("same direction") != std::string::npos);
		DrawFrame(InvalidInputPoint, false);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline &&
			!Session->IsDirty());

		// Ctrl-click without crossing the drag threshold is a no-op.
		DrawFrame(SourceAOutputPoint);
		DrawFrame(SourceAOutputPoint, true, false, false, true);
		DrawFrame(SourceAOutputPoint, false, false, false, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline &&
			!Session->IsDirty());

		// Escape cancels an active drag and consumes the held button through release.
		DrawFrame(SourceAOutputPoint);
		DrawFrame(SourceAOutputPoint, true, false, false, true);
		DrawFrame(SourceBOutputPoint, true, false, false, false);
		DrawFrame(SourceBOutputPoint, true, false, true, false);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		DrawFrame(SourceBOutputPoint, false);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::IsCanvasInputSuppressed(GraphEditor, *Session));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline);
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, !SawGraphNodeSearchPopup);

		// An ordinary link drag after a cancelled move is accepted by the canvas. Keep a held
		// target frame after the canvas starts its native create action so it can resolve the
		// destination pin before release, as a real pointer drag does.
		const ImVec2 UnconnectedOutputPoint = FindPinScreen(SourceBValue);
		const ImVec2 OrdinaryTargetPoint = FindPinScreen(TargetDA);
		const auto BeforeOrdinaryLink = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const uint64_t OrdinaryLinkRevision = Session->Revision;
		DrawFrame(UnconnectedOutputPoint);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == SourceBValue);
		// A Ctrl press on an unconnected output keeps the canvas's ordinary-link behavior.
		DrawFrame(UnconnectedOutputPoint, false, false, false, true, std::nullopt, true);
		DrawFrame(UnconnectedOutputPoint, true, false, false, true, std::nullopt, true);
		PF_CHECK(Tests, LastQueuedCtrlLeftPress);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == SourceBValue);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		DrawFrame(OrdinaryTargetPoint, true, false, false, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == TargetDA);
		PF_CHECK(Tests, LastSyntheticLeftDragging);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		DrawFrame(OrdinaryTargetPoint, true, false, false, false);
		DrawFrame(OrdinaryTargetPoint, false, false, false, false);
		DrawFrame(OrdinaryTargetPoint);
		const auto AfterOrdinaryLink = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterOrdinaryLink, SourceBValue, TargetDA) != UUID{} &&
			Session->Revision == OrdinaryLinkRevision + 1);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == BeforeOrdinaryLink &&
			!Session->IsDirty());

		// Occupied-input replacement removes the displaced edge in the same command; Undo restores its UUID.
		(void)OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Input);
		const ImVec2 OccupiedInputPoint = ClickCanvasPin(TargetCA);
		DrawFrame(OccupiedInputPoint, false);
		auto AfterReplacement = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterReplacement, SourceAValue, TargetCA) == PrimaryLink);
		PF_CHECK(Tests, LinkID(AfterReplacement, SourceCValue, TargetCA).IsNil());
		PF_CHECK(Tests, AfterReplacement.Connections.size() + 1 == Baseline.Connections.size());
		PF_CHECK(Tests, PositionsMatch(AfterReplacement, Baseline));
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == AfterReplacement);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const auto SavedBytes = ReadBytes(Project->GetRootPath() / Fixture->ProjectRelativePath);
		const auto ReopenedGraph = SavedBytes ? DeserializeEditorGraph(*SavedBytes, BaseSchema)
			: std::expected<EditorGraph, std::string>(std::unexpected("saved graph bytes are unavailable"));
		const auto SameGraphByIdentity = [](const EditorGraph& Left, const EditorGraph& Right)
		{
			if (Left.ID != Right.ID || Left.SchemaKey != Right.SchemaKey || Left.SchemaVersion != Right.SchemaVersion ||
				Left.Nodes.size() != Right.Nodes.size() || Left.Connections.size() != Right.Connections.size()) return false;
			for (const auto& Node : Left.Nodes)
			{
				const auto Other = std::find_if(Right.Nodes.begin(), Right.Nodes.end(), [&Node](const auto& Candidate)
					{ return Candidate.ID == Node.ID; });
				if (Other == Right.Nodes.end() || Node.TypeKey != Other->TypeKey || Node.X != Other->X || Node.Y != Other->Y ||
					Node.Pins.size() != Other->Pins.size()) return false;
				for (const auto& Pin : Node.Pins)
				{
					const auto OtherPin = std::find_if(Other->Pins.begin(), Other->Pins.end(), [&Pin](const auto& Candidate)
						{ return Candidate.ID == Pin.ID; });
					if (OtherPin == Other->Pins.end() || *OtherPin != Pin) return false;
				}
			}
			for (const auto& Connection : Left.Connections)
				if (std::find(Right.Connections.begin(), Right.Connections.end(), Connection) == Right.Connections.end()) return false;
			return true;
		};
		PF_CHECK(Tests, ReopenedGraph && SameGraphByIdentity(*ReopenedGraph, AfterReplacement));

		const auto SetNodeY = [](EditorGraph& TargetGraph, UUID NodeID, double Y)
		{
			const auto Node = std::find_if(TargetGraph.Nodes.begin(), TargetGraph.Nodes.end(),
				[NodeID](const auto& Candidate) { return Candidate.ID == NodeID; });
			if (Node != TargetGraph.Nodes.end()) Node->Y = Y;
		};
		const auto SetNodePosition = [](EditorGraph& TargetGraph, UUID NodeID, double X, double Y)
		{
			const auto Node = std::find_if(TargetGraph.Nodes.begin(), TargetGraph.Nodes.end(),
				[NodeID](const auto& Candidate) { return Candidate.ID == NodeID; });
			if (Node != TargetGraph.Nodes.end()) { Node->X = X; Node->Y = Y; }
		};
		EditorGraph ClippedOutputGraph = Baseline;
		SetNodeY(ClippedOutputGraph, SourceB, -10000.0);
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, ClippedOutputGraph, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		EditorGraphDocumentEditorTestAccess::CapturePresentation(GraphEditor, *Session, true);
		const ImVec2 ClippedOutputPoint = FindPinScreen(SourceBValue);
		PF_CHECK(Tests, ClippedOutputPoint.y < HostOrigin.y);
		const ImVec2 VisibleSourcePoint = FindPinScreen(SourceAValue);
		const EditorGraph BeforeClippedOutputGesture = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const uint64_t ClippedOutputRevision = Session->Revision;
		DrawFrame(VisibleSourcePoint);
		DrawFrame(VisibleSourcePoint, true, false, false, true);
		DrawFrame(ClippedOutputPoint, true);
		const auto ClippedOutputPreview = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, ClippedOutputPreview.Active && ClippedOutputPreview.TargetPin.IsNil() &&
			!ClippedOutputPreview.TargetAllowed);
		DrawFrame(ClippedOutputPoint, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == BeforeClippedOutputGesture &&
			Session->Revision == ClippedOutputRevision && !Session->IsDirty());

		// A cached live hit cannot claim a press if the inherited host clip changes before it.
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Baseline, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const ImVec2 SourcePointBeforeClip = FindPinScreen(SourceAValue);
		DrawFrame(SourcePointBeforeClip);
		const EditorGraphCanvasRect ClipAboveSource{
			{ HostOrigin.x, HostOrigin.y }, { HostOrigin.x + 1367.0, SourcePointBeforeClip.y - 1.0 } };
		const uint64_t SourceClipRevision = Session->Revision;
		DrawFrame(SourcePointBeforeClip, true, false, false, true, ClipAboveSource);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		DrawFrame(SourcePointBeforeClip, false, false, false, false, ClipAboveSource);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline &&
			Session->Revision == SourceClipRevision && !Session->IsDirty());

		EditorGraph ClippedSourceGraph = Baseline;
		SetNodeY(ClippedSourceGraph, SourceA, -10000.0);
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, ClippedSourceGraph, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const ImVec2 ClippedSourcePoint = FindPinScreen(SourceAValue);
		PF_CHECK(Tests, ClippedSourcePoint.y < HostOrigin.y);
		DrawFrame(ClippedSourcePoint, true, false, false, true);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
		DrawFrame(ClippedSourcePoint, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == ClippedSourceGraph &&
			!Session->IsDirty());

		EditorGraph ClippedOccupiedInputGraph = Baseline;
		SetNodeY(ClippedOccupiedInputGraph, TargetC, -10000.0);
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, ClippedOccupiedInputGraph, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const ImVec2 ClippedOccupiedInputPoint = FindPinScreen(TargetCA);
		PF_CHECK(Tests, ClippedOccupiedInputPoint.y < HostOrigin.y);
		const uint64_t ClippedInputRevision = Session->Revision;
		(void)OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Input);
		DrawFrame(ClippedOccupiedInputPoint, true);
		DrawFrame(ClippedOccupiedInputPoint, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == ClippedOccupiedInputGraph &&
			Session->Revision == ClippedInputRevision && !Session->IsDirty());

		// Only the portion of a pin marker inside the inherited ImGui clip can receive a reconnect click.
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Baseline, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *Session,
			"{\"view\":{\"scroll\":{\"x\":37.0,\"y\":-21.0},\"zoom\":0.65}}");
		EditorGraphDocumentEditorTestAccess::CapturePresentation(GraphEditor, *Session, true);
		DrawFrame(ImVec2(-1000.0f, -1000.0f));
		const auto FindMarkerScreenBounds = [&](UUID PinID) -> std::optional<EditorGraphCanvasRect>
		{
			for (const auto& Node : EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session))
				for (const auto& Pin : Node.Pins)
					if (Pin.PinID == PinID)
					{
						const auto Minimum = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
							GraphEditor, *Session, Pin.Marker.Minimum);
						const auto Maximum = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
							GraphEditor, *Session, Pin.Marker.Maximum);
						return EditorGraphCanvasRect{ { Minimum.X, Minimum.Y }, { Maximum.X, Maximum.Y } };
					}
			return std::nullopt;
		};
		const auto PartialClipMarker = FindMarkerScreenBounds(TargetDA);
		PF_CHECK(Tests, PartialClipMarker.has_value());
		if (PartialClipMarker)
		{
			const double MidY = (PartialClipMarker->Minimum.Y + PartialClipMarker->Maximum.Y) * 0.5;
			const EditorGraphCanvasRect LowerHalfClip{
				{ HostOrigin.x, MidY }, { HostOrigin.x + 1367.0, HostOrigin.y + 850.0 } };
			const ImVec2 ClippedPartPoint(
				static_cast<float>((PartialClipMarker->Minimum.X + PartialClipMarker->Maximum.X) * 0.5),
				static_cast<float>(PartialClipMarker->Minimum.Y +
					(PartialClipMarker->Maximum.Y - PartialClipMarker->Minimum.Y) * 0.25));
			const ImVec2 VisiblePartPoint(
				static_cast<float>((PartialClipMarker->Minimum.X + PartialClipMarker->Maximum.X) * 0.5),
				static_cast<float>(PartialClipMarker->Minimum.Y +
					(PartialClipMarker->Maximum.Y - PartialClipMarker->Minimum.Y) * 0.75));
			const uint64_t PartialClipRevision = Session->Revision;
			(void)OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Input);
			DrawFrame(ClippedPartPoint, true, false, false, false, LowerHalfClip);
			DrawFrame(ClippedPartPoint, false, false, false, false, LowerHalfClip);
			PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline &&
				Session->Revision == PartialClipRevision && !Session->IsDirty());

			(void)OpenTargetSelection(PrimaryLink, EditorGraphPinDirection::Input);
			DrawFrame(VisiblePartPoint, true, false, false, false, LowerHalfClip);
			DrawFrame(VisiblePartPoint, false, false, false, false, LowerHalfClip);
			const auto AfterPartialClipReconnect = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
			PF_CHECK(Tests, LinkID(AfterPartialClipReconnect, SourceAValue, TargetDA) == PrimaryLink &&
				LinkID(AfterPartialClipReconnect, SourceAValue, TargetAA).IsNil());
			PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
			PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Baseline);
		}

		// Overlapping pins obey the pinned canvas's reverse node-order hit priority.
		EditorGraph OverlappingTargets = Baseline;
		const auto SourceANode = std::find_if(OverlappingTargets.Nodes.begin(), OverlappingTargets.Nodes.end(),
			[SourceA](const auto& Node) { return Node.ID == SourceA; });
		PF_CHECK(Tests, SourceANode != OverlappingTargets.Nodes.end());
		if (SourceANode != OverlappingTargets.Nodes.end())
			SetNodePosition(OverlappingTargets, SourceB, SourceANode->X, SourceANode->Y);
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, OverlappingTargets, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const ImVec2 OverlappingPinPoint = FindPinScreen(SourceBValue);
		const ImVec2 SingleWireSourcePoint = FindPinScreen(SourceCValue);
		DrawFrame(SingleWireSourcePoint);
		DrawFrame(SingleWireSourcePoint, true, false, false, true);
		DrawFrame(OverlappingPinPoint, true, false, false, false);
		const auto OverlapPreview = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
		PF_CHECK(Tests, OverlapPreview.Active &&
			(OverlapPreview.TargetPin == SourceAValue || OverlapPreview.TargetPin == SourceBValue) && OverlapPreview.TargetAllowed);
		DrawFrame(OverlappingPinPoint, false);
		const auto AfterOverlapMove = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, LinkID(AfterOverlapMove, OverlapPreview.TargetPin, TargetCA) == OccupiedLink &&
			LinkID(AfterOverlapMove, SourceAValue, TargetAA) == PrimaryLink &&
			LinkID(AfterOverlapMove, SourceAValue, TargetBA) == FanOutLink &&
			LinkID(AfterOverlapMove, SourceCValue, TargetCA).IsNil());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == OverlappingTargets);

		// A previously allowed target is not valid after a viewport pan/zoom changes before release.
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Baseline, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *Session,
			"{\"view\":{\"scroll\":{\"x\":37.0,\"y\":-21.0},\"zoom\":0.65}}");
		DrawFrame(ImVec2(-1000.0f, -1000.0f));
		const ImVec2 StaleSourcePoint = FindPinScreen(SourceAValue);
		const ImVec2 StaleTargetPoint = FindPinScreen(SourceBValue);
		const uint64_t StaleGeometryRevision = Session->Revision;
		const auto StaleGeometryGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		DrawFrame(StaleSourcePoint);
		DrawFrame(StaleSourcePoint, true, false, false, true);
		DrawFrame(StaleTargetPoint, true, false, false, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).TargetAllowed);
		EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *Session,
			"{\"view\":{\"scroll\":{\"x\":-220.0,\"y\":180.0},\"zoom\":1.45}}");
		DrawFrame(StaleTargetPoint, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == StaleGeometryGraph &&
			Session->Revision == StaleGeometryRevision && !Session->IsDirty());

		// Double-clicking the canvas-resolved wire inserts a typed reroute through the
		// production adapter and preserves the selected downstream connection identity.
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Baseline, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		DrawFrame(ImVec2(-1000.0f, -1000.0f));
		const auto WirePoint = FindLinkPoint(PrimaryLink);
		PF_CHECK(Tests, WirePoint.has_value());
		if (WirePoint)
		{
			const auto BeforeReroute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
			const auto ExpectedReroutePosition = EditorGraphDocumentEditorTestAccess::ScreenToCanvas(GraphEditor, *Session,
				{ WirePoint->x, WirePoint->y });
			const uint64_t RerouteRevision = Session->Revision;
			DrawFrame(*WirePoint);
			DrawFrame(*WirePoint, true);
			DrawFrame(*WirePoint, false);
			DrawFrame(*WirePoint, true);
			DrawFrame(*WirePoint, false);
			const auto AfterReroute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
			PF_CHECK(Tests, AfterReroute.Nodes.size() == BeforeReroute.Nodes.size() + 1 &&
				AfterReroute.Connections.size() == BeforeReroute.Connections.size() + 1 &&
				Session->Revision == RerouteRevision + 1 && Session->IsDirty());
			DrawFrame(ImVec2(-1000.0f, -1000.0f));
			const auto Downstream = std::find_if(AfterReroute.Connections.begin(), AfterReroute.Connections.end(),
				[PrimaryLink](const auto& Link) { return Link.ID == PrimaryLink; });
			UUID RerouteNodeID;
			UUID RerouteInputID;
			UUID RerouteOutputID;
			UUID RerouteUpstreamID;
			PF_CHECK(Tests, Downstream != AfterReroute.Connections.end() && Downstream->InputPin == TargetAA &&
				Downstream->OutputPin != SourceAValue);
			if (Downstream != AfterReroute.Connections.end())
			{
				const auto RerouteNode = std::find_if(AfterReroute.Nodes.begin(), AfterReroute.Nodes.end(),
					[&](const auto& Node) { return std::any_of(Node.Pins.begin(), Node.Pins.end(),
						[&](const auto& Pin) { return Pin.ID == Downstream->OutputPin; }); });
				PF_CHECK(Tests, RerouteNode != AfterReroute.Nodes.end() && IsEditorGraphRerouteType(RerouteNode->TypeKey));
				if (RerouteNode != AfterReroute.Nodes.end())
				{
					const auto ReroutePresentations = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
					const auto ReroutePresentation = std::find_if(ReroutePresentations.begin(), ReroutePresentations.end(),
						[&](const auto& Candidate) { return Candidate.NodeID == RerouteNode->ID; });
					const auto RerouteInputIt = std::find_if(RerouteNode->Pins.begin(), RerouteNode->Pins.end(),
						[](const auto& Pin) { return Pin.Key == "in"; });
					const auto RerouteOutputIt = std::find_if(RerouteNode->Pins.begin(), RerouteNode->Pins.end(),
						[](const auto& Pin) { return Pin.Key == "out"; });
					const auto* RerouteInput = RerouteInputIt == RerouteNode->Pins.end() ? nullptr : &*RerouteInputIt;
					const auto* RerouteOutput = RerouteOutputIt == RerouteNode->Pins.end() ? nullptr : &*RerouteOutputIt;
					if (RerouteInput && RerouteOutput)
					{
						RerouteNodeID = RerouteNode->ID;
						RerouteInputID = RerouteInput->ID;
						RerouteOutputID = RerouteOutput->ID;
						const auto UpstreamSegment = std::find_if(AfterReroute.Connections.begin(), AfterReroute.Connections.end(),
							[RerouteInput](const auto& Link) { return Link.InputPin == RerouteInput->ID; });
						if (UpstreamSegment != AfterReroute.Connections.end()) RerouteUpstreamID = UpstreamSegment->ID;
					}
					PF_CHECK(Tests, RerouteInput && RerouteOutput &&
						std::abs(RerouteNode->X - ExpectedReroutePosition.X) < 1.0 &&
						std::abs(RerouteNode->Y - ExpectedReroutePosition.Y) < 1.0 &&
						RerouteInput->Type == EditorGraphPinType::Float && RerouteOutput->Type == EditorGraphPinType::Float);
					if (ReroutePresentation != ReroutePresentations.end())
					{
						const auto InputPresentation = std::find_if(ReroutePresentation->Pins.begin(), ReroutePresentation->Pins.end(),
							[&](const auto& Pin) { return Pin.PinID == RerouteInput->ID; });
						const auto OutputPresentation = std::find_if(ReroutePresentation->Pins.begin(), ReroutePresentation->Pins.end(),
							[&](const auto& Pin) { return Pin.PinID == RerouteOutput->ID; });
						PF_CHECK(Tests, ReroutePresentation->IsReroute && ReroutePresentation->Size.X <= 40.0 &&
							ReroutePresentation->Size.Y <= 30.0 && InputPresentation != ReroutePresentation->Pins.end() &&
							OutputPresentation != ReroutePresentation->Pins.end() && !InputPresentation->Label &&
							!OutputPresentation->Label && !InputPresentation->PropertyControl && !OutputPresentation->PropertyControl &&
							InputPresentation->HitBounds && OutputPresentation->HitBounds &&
							InputPresentation->Marker.Anchor.X == OutputPresentation->Marker.Anchor.X &&
							InputPresentation->Marker.Anchor.Y == OutputPresentation->Marker.Anchor.Y &&
							InputPresentation->HitBounds->Maximum.X <= OutputPresentation->HitBounds->Minimum.X);
						if (InputPresentation != ReroutePresentation->Pins.end() &&
							OutputPresentation != ReroutePresentation->Pins.end())
							PF_CHECK(Tests, std::abs(InputPresentation->LinkDirection.X) > 0.99 &&
								std::abs(InputPresentation->LinkDirection.Y) < 0.01 &&
								std::abs(OutputPresentation->LinkDirection.X) > 0.99 &&
								std::abs(OutputPresentation->LinkDirection.Y) < 0.01);
					}
				}
			}
			PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
				EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == BeforeReroute);
			PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
				EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == AfterReroute);
			PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
			const auto RerouteBytes = ReadBytes(Project->GetRootPath() / Fixture->ProjectRelativePath);
			const auto ReopenedReroute = RerouteBytes ? DeserializeEditorGraph(*RerouteBytes, Schema)
				: std::expected<EditorGraph, std::string>(std::unexpected("saved reroute graph bytes are unavailable"));
			EditorGraph ReopenedRerouteBaseline = AfterReroute;
			PF_CHECK(Tests, ReopenedReroute && SameGraphByIdentity(*ReopenedReroute, AfterReroute));
			if (ReopenedReroute)
			{
				ReopenedRerouteBaseline = *ReopenedReroute;
				const auto ReopenedKnotNode = std::find_if(ReopenedReroute->Nodes.begin(), ReopenedReroute->Nodes.end(),
					[](const auto& Candidate) { return IsEditorGraphRerouteType(Candidate.TypeKey); });
				const UUID ReopenedKnotID = ReopenedKnotNode == ReopenedReroute->Nodes.end()
					? UUID{} : ReopenedKnotNode->ID;
				EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, *ReopenedReroute, Schema);
				DrawFrame(ImVec2(-1000.0f, -1000.0f));
				const auto ReopenedPresentation = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
				const auto ReopenedKnot = std::find_if(ReopenedPresentation.begin(), ReopenedPresentation.end(),
					[ReopenedKnotID](const auto& Candidate) { return Candidate.NodeID == ReopenedKnotID; });
				PF_CHECK(Tests, ReopenedKnot != ReopenedPresentation.end() && ReopenedKnot->IsReroute &&
					ReopenedKnot->Size.X <= 40.0 && ReopenedKnot->Size.Y <= 30.0);
			}
			if (!RerouteNodeID.IsNil() && !RerouteInputID.IsNil() && !RerouteOutputID.IsNil() &&
				!RerouteUpstreamID.IsNil())
			{
				EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *Session,
					"{\"view\":{\"scroll\":{\"x\":-34.0,\"y\":0.0},\"zoom\":0.65}}");
				DrawFrame(ImVec2(-1000.0f, -1000.0f));
				const auto HitCenter = [&](UUID PinID)
				{
					const auto Metrics = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
					for (const auto& NodeMetric : Metrics)
						for (const auto& PinMetric : NodeMetric.Pins)
							if (PinMetric.PinID == PinID && PinMetric.HitBounds)
							{
								const EditorGraphCanvasPoint CanvasCenter{
									(PinMetric.HitBounds->Minimum.X + PinMetric.HitBounds->Maximum.X) * 0.5,
									(PinMetric.HitBounds->Minimum.Y + PinMetric.HitBounds->Maximum.Y) * 0.5 };
								const auto Screen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
									GraphEditor, *Session, CanvasCenter);
								return ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y));
							}
					return ImVec2(-1000.0f, -1000.0f);
				};
				const UUID UpstreamID = RerouteUpstreamID;
				AllowPinB = true;
				const ImVec2 InputTarget = HitCenter(RerouteInputID);
				const ImVec2 FreeInput = FindPinScreen(TargetAB);
				DrawFrame(InputTarget);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == RerouteInputID);
				DrawFrame(InputTarget, true, false, false, true);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
				DrawFrame(FreeInput, true, false, false, true);
				const auto InputPinMovePreview = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == TargetAB);
				PF_CHECK(Tests, InputPinMovePreview.Active);
				PF_CHECK(Tests, InputPinMovePreview.TargetPin == TargetAB);
				PF_CHECK(Tests, InputPinMovePreview.TargetAllowed);
				DrawFrame(FreeInput, false, false, false, false);
				const auto AfterInputPinTarget = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
				PF_CHECK(Tests, LinkID(AfterInputPinTarget, SourceAValue, TargetAB) == UpstreamID);
				PF_CHECK(Tests, LinkID(AfterInputPinTarget, RerouteOutputID, TargetAA) == PrimaryLink);
				PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
					EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == ReopenedRerouteBaseline);
				AllowPinB = false;

				const ImVec2 OutputTarget = HitCenter(RerouteOutputID);
				const ImVec2 OtherOutput = FindPinScreen(SourceBValue);
				DrawFrame(OutputTarget);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == RerouteOutputID);
				DrawFrame(OutputTarget, true, false, false, true);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session).Active);
				DrawFrame(OtherOutput, true, false, false, true);
				const auto OutputMovePreview = EditorGraphDocumentEditorTestAccess::GetPinMove(GraphEditor, *Session);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == SourceBValue);
				PF_CHECK(Tests, OutputMovePreview.Active);
				PF_CHECK(Tests, OutputMovePreview.TargetAllowed);
				DrawFrame(OtherOutput, false, false, false, false);
				const auto AfterOutputPinTarget = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
				PF_CHECK(Tests, LinkID(AfterOutputPinTarget, SourceBValue, TargetAA) == PrimaryLink &&
					LinkID(AfterOutputPinTarget, SourceAValue, RerouteInputID) == UpstreamID);
				PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
					EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == ReopenedRerouteBaseline);
				const auto RerouteBeforeMove = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
				const auto RerouteBeforeMoveNode = std::find_if(RerouteBeforeMove.Nodes.begin(), RerouteBeforeMove.Nodes.end(),
					[RerouteNodeID](const auto& Candidate) { return Candidate.ID == RerouteNodeID; });
				const auto ReroutePosition = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
					GraphEditor, *Session, { RerouteBeforeMoveNode->X, RerouteBeforeMoveNode->Y });
				const ImVec2 KnotBodyCenter(static_cast<float>(ReroutePosition.X), static_cast<float>(ReroutePosition.Y));
				const ImVec2 KnotBodyDrop(KnotBodyCenter.x + 112.0f, KnotBodyCenter.y + 84.0f);
				DrawFrame(KnotBodyCenter);
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session).IsNil());
				PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetHoveredConnection(GraphEditor, *Session).IsNil());
				DrawFrame(KnotBodyCenter, true);
				DrawFrame(KnotBodyCenter, false);
				const auto SelectedAtKnotBody = EditorGraphDocumentEditorTestAccess::GetSelectedNodes(GraphEditor, *Session);
				PF_CHECK(Tests, std::find(SelectedAtKnotBody.begin(), SelectedAtKnotBody.end(), RerouteNodeID) !=
					SelectedAtKnotBody.end());
				DrawFrame(KnotBodyCenter, true);
				DrawFrame(KnotBodyDrop, true);
				DrawFrame(KnotBodyDrop, true);
				DrawFrame(KnotBodyDrop, false);
				const auto RerouteAfterMove = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
				const auto RerouteAfterMoveNode = std::find_if(RerouteAfterMove.Nodes.begin(), RerouteAfterMove.Nodes.end(),
					[RerouteNodeID](const auto& Candidate) { return Candidate.ID == RerouteNodeID; });
				PF_CHECK(Tests, RerouteBeforeMoveNode != RerouteBeforeMove.Nodes.end() &&
					RerouteAfterMoveNode != RerouteAfterMove.Nodes.end() &&
					(RerouteBeforeMoveNode->X != RerouteAfterMoveNode->X || RerouteBeforeMoveNode->Y != RerouteAfterMoveNode->Y));
				PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
					EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == ReopenedRerouteBaseline);

				// A vertically aligned reroute chain gets vertical pin tangents while the
				// upstream and downstream ordinary nodes retain their schema-defined rows.
				auto RouteGraph = CreateEditorGraph(Schema, Fixture->GraphID);
				auto RouteSource = CreateEditorGraphNode(Schema, "material.scalar", 100.0, 200.0);
				auto RouteFirst = CreateEditorGraphNode(Schema, "pulseforge.reroute.float", 500.0, 200.0);
				auto RouteSecond = CreateEditorGraphNode(Schema, "pulseforge.reroute.float", 500.0, 330.0);
				auto RouteTarget = CreateEditorGraphNode(Schema, "material.multiply", 500.0, 400.0);
				PF_CHECK(Tests, RouteGraph && RouteSource && RouteFirst && RouteSecond && RouteTarget);
				if (RouteGraph && RouteSource && RouteFirst && RouteSecond && RouteTarget)
				{
					const auto GetNodePinID = [](const EditorGraphNode& Node, std::string_view Key)
					{
						const auto Pin = std::find_if(Node.Pins.begin(), Node.Pins.end(), [Key](const auto& Candidate)
							{ return Candidate.Key == Key; });
						return Pin == Node.Pins.end() ? UUID{} : Pin->ID;
					};
					const UUID RouteFirstID = RouteFirst->ID;
					const UUID RouteSecondID = RouteSecond->ID;
					const UUID RouteSourcePin = GetNodePinID(*RouteSource, "value");
					const UUID RouteFirstIn = GetNodePinID(*RouteFirst, "in");
					const UUID RouteFirstOut = GetNodePinID(*RouteFirst, "out");
					const UUID RouteSecondIn = GetNodePinID(*RouteSecond, "in");
					const UUID RouteSecondOut = GetNodePinID(*RouteSecond, "out");
					const UUID RouteTargetPin = GetNodePinID(*RouteTarget, "a");
					RouteGraph->Nodes.push_back(std::move(*RouteSource));
					RouteGraph->Nodes.push_back(std::move(*RouteFirst));
					RouteGraph->Nodes.push_back(std::move(*RouteSecond));
					RouteGraph->Nodes.push_back(std::move(*RouteTarget));
					EditorGraphCommandHistory RouteHistory(*RouteGraph);
					PF_CHECK(Tests, ConnectEditorGraphPins(RouteHistory, Schema, RouteSourcePin, RouteFirstIn));
					PF_CHECK(Tests, ConnectEditorGraphPins(RouteHistory, Schema, RouteFirstOut, RouteSecondIn));
					PF_CHECK(Tests, ConnectEditorGraphPins(RouteHistory, Schema, RouteSecondOut, RouteTargetPin));
					EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, RouteHistory.Graph(), Schema);
					DrawFrame(ImVec2(-1000.0f, -1000.0f));
					const auto RoutePresentation = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
					const auto GetPinMetric = [&RoutePresentation](UUID NodeID, UUID PinID) -> const EditorGraphCanvasPinPresentation*
					{
						const auto NodeMetric = std::find_if(RoutePresentation.begin(), RoutePresentation.end(),
							[NodeID](const auto& Candidate) { return Candidate.NodeID == NodeID; });
						if (NodeMetric == RoutePresentation.end()) return nullptr;
						const auto PinMetric = std::find_if(NodeMetric->Pins.begin(), NodeMetric->Pins.end(),
							[PinID](const auto& Candidate) { return Candidate.PinID == PinID; });
						return PinMetric == NodeMetric->Pins.end() ? nullptr : &*PinMetric;
					};
					const auto* FirstOutputMetric = GetPinMetric(RouteFirstID, RouteFirstOut);
					const auto* SecondInputMetric = GetPinMetric(RouteSecondID, RouteSecondIn);
					const auto* SecondOutputMetric = GetPinMetric(RouteSecondID, RouteSecondOut);
					PF_CHECK(Tests, FirstOutputMetric && SecondInputMetric && SecondOutputMetric &&
						std::abs(FirstOutputMetric->LinkDirection.X) < 0.01 && FirstOutputMetric->LinkDirection.Y > 0.99 &&
						std::abs(SecondInputMetric->LinkDirection.X) < 0.01 && SecondInputMetric->LinkDirection.Y < -0.99 &&
						std::abs(SecondOutputMetric->LinkDirection.X) < 0.01 && SecondOutputMetric->LinkDirection.Y > 0.99);
				}

				// Execution routes keep horizontal runs horizontal, turn through aligned
				// knots, and return to a horizontal tangent at the downstream node.
				auto ExecutionSchema = MakeGameplayEventFixtureSchema();
				auto ExecutionGraph = CreateEditorGraph(ExecutionSchema, Fixture->GraphID);
				auto EventNode = CreateEditorGraphNode(ExecutionSchema, "gameplay.event.begin", 100.0, 200.0);
				auto EntryKnot = CreateEditorGraphNode(ExecutionSchema,
					"pulseforge.reroute.execution", 400.0, 200.0);
				auto VerticalKnot = CreateEditorGraphNode(ExecutionSchema,
					"pulseforge.reroute.execution", 400.0, 340.0);
				auto ExitKnot = CreateEditorGraphNode(ExecutionSchema,
					"pulseforge.reroute.execution", 700.0, 340.0);
				auto BranchNode = CreateEditorGraphNode(ExecutionSchema, "gameplay.flow.branch", 1000.0, 340.0);
				PF_CHECK(Tests, ExecutionGraph && EventNode && EntryKnot && VerticalKnot && ExitKnot && BranchNode);
				if (ExecutionGraph && EventNode && EntryKnot && VerticalKnot && ExitKnot && BranchNode)
				{
					const auto PinID = [](const EditorGraphNode& Node, std::string_view Key)
					{
						const auto Pin = std::find_if(Node.Pins.begin(), Node.Pins.end(),
							[Key](const auto& Candidate) { return Candidate.Key == Key; });
						return Pin == Node.Pins.end() ? UUID{} : Pin->ID;
					};
					const UUID EntryID = EntryKnot->ID;
					const UUID VerticalID = VerticalKnot->ID;
					const UUID ExitID = ExitKnot->ID;
					const UUID EventOut = PinID(*EventNode, "event");
					const UUID EntryIn = PinID(*EntryKnot, "in");
					const UUID EntryOut = PinID(*EntryKnot, "out");
					const UUID VerticalIn = PinID(*VerticalKnot, "in");
					const UUID VerticalOut = PinID(*VerticalKnot, "out");
					const UUID ExitIn = PinID(*ExitKnot, "in");
					const UUID ExitOut = PinID(*ExitKnot, "out");
					const UUID BranchIn = PinID(*BranchNode, "exec-in");
					ExecutionGraph->Nodes.push_back(std::move(*EventNode));
					ExecutionGraph->Nodes.push_back(std::move(*EntryKnot));
					ExecutionGraph->Nodes.push_back(std::move(*VerticalKnot));
					ExecutionGraph->Nodes.push_back(std::move(*ExitKnot));
					ExecutionGraph->Nodes.push_back(std::move(*BranchNode));
					EditorGraphCommandHistory ExecutionHistory(*ExecutionGraph);
					PF_CHECK(Tests, ConnectEditorGraphPins(ExecutionHistory, ExecutionSchema, EventOut, EntryIn));
					PF_CHECK(Tests, ConnectEditorGraphPins(ExecutionHistory, ExecutionSchema, EntryOut, VerticalIn));
					PF_CHECK(Tests, ConnectEditorGraphPins(ExecutionHistory, ExecutionSchema, VerticalOut, ExitIn));
					PF_CHECK(Tests, ConnectEditorGraphPins(ExecutionHistory, ExecutionSchema, ExitOut, BranchIn));
					EditorGraphDocumentEditorTestAccess::SetGraph(
						GraphEditor, *Session, ExecutionHistory.Graph(), ExecutionSchema);
					DrawFrame(ImVec2(-1000.0f, -1000.0f));
					const auto ExecutionPresentation = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
					const auto Direction = [&ExecutionPresentation](UUID NodeID, UUID PinID)
					{
						const auto Node = std::find_if(ExecutionPresentation.begin(), ExecutionPresentation.end(),
							[NodeID](const auto& Candidate) { return Candidate.NodeID == NodeID; });
						if (Node == ExecutionPresentation.end()) return EditorGraphCanvasPoint{};
						const auto Pin = std::find_if(Node->Pins.begin(), Node->Pins.end(),
							[PinID](const auto& Candidate) { return Candidate.PinID == PinID; });
						return Pin == Node->Pins.end() ? EditorGraphCanvasPoint{} : Pin->LinkDirection;
					};
					const auto EntryOutputDirection = Direction(EntryID, EntryOut);
					const auto VerticalInputDirection = Direction(VerticalID, VerticalIn);
					const auto VerticalOutputDirection = Direction(VerticalID, VerticalOut);
					const auto ExitInputDirection = Direction(ExitID, ExitIn);
					const auto ExitOutputDirection = Direction(ExitID, ExitOut);
					PF_CHECK(Tests, EntryOutputDirection.Y > 0.99);
					PF_CHECK(Tests, VerticalInputDirection.Y < -0.99);
					PF_CHECK(Tests, VerticalOutputDirection.X > 0.99);
					PF_CHECK(Tests, ExitInputDirection.X < -0.99);
					PF_CHECK(Tests, ExitOutputDirection.X > 0.99);
				}
			}
		}
	}

	void TestGraphCanvasWireRouteGesture(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		ImGuiContextScope ImGuiScope;
		PF_CHECK(Tests, ImGuiScope.Context != nullptr);
		if (!ImGuiScope.Context) return;
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.LogFilename = nullptr;
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		IO.DeltaTime = 1.0f / 60.0f;

		TemporaryGraphProject Temporary;
		const auto ProjectID = UUID::Generate();
		PF_CHECK(Tests, ProjectID.has_value());
		if (!ProjectID) return;
		Temporary.Path = std::filesystem::temp_directory_path() / ProjectID->ToString();
		std::error_code FileError;
		std::filesystem::create_directories(Temporary.Path, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		auto Project = PulseForge::Project::Create(Temporary.Path / "GraphWireRouteProject.pfproj", "Graph wire route test");
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
		PF_CHECK(Tests, Documents.Open(Identity, Fixture->ProjectRelativePath, Fixture->DisplayName,
			EditorDocumentOpenPreference::MainDocumentStrip).has_value());
		auto* Session = Documents.Find(Identity);
		const EditorDocumentEditorContext LoadContext{ Identity, Project->GetRootPath(), Fixture->ProjectRelativePath,
			Fixture->DisplayName, nullptr };
		PF_CHECK(Tests, Session && Editors.Load(*Session, LoadContext));
		if (!Session) return;

		const auto Schema = MakeMaterialExpressionFixtureSchema();
		const auto MakeGraph = [&]()
		{
			auto Graph = CreateEditorGraph(Schema, Fixture->GraphID);
			if (!Graph) return Graph;
			for (const auto& [Type, X, Y] : std::array<std::tuple<std::string_view, double, double>, 5>{
				std::tuple{ "material.scalar", 80.0, 160.0 },
				std::tuple{ "material.scalar", 360.0, 440.0 },
				std::tuple{ "material.multiply", 720.0, 250.0 },
				std::tuple{ "material.color", 820.0, 100.0 },
				std::tuple{ "material.output", 1060.0, 100.0 } })
			{
				auto Node = CreateEditorGraphNode(Schema, Type, X, Y);
				if (!Node) return std::expected<EditorGraph, std::string>(std::unexpected(Node.error()));
				Graph->Nodes.push_back(std::move(*Node));
			}
			return Graph;
		};
		auto Graph = MakeGraph();
		PF_CHECK(Tests, Graph.has_value());
		if (!Graph) return;
		const auto PinID = [](const EditorGraph& Source, size_t NodeIndex, std::string_view Key)
		{
			if (NodeIndex >= Source.Nodes.size()) return UUID{};
			const auto Pin = std::find_if(Source.Nodes[NodeIndex].Pins.begin(), Source.Nodes[NodeIndex].Pins.end(),
				[Key](const auto& Candidate) { return Candidate.Key == Key; });
			return Pin == Source.Nodes[NodeIndex].Pins.end() ? UUID{} : Pin->ID;
		};
		const UUID SourceA = PinID(*Graph, 0, "value");
		const UUID SourceB = PinID(*Graph, 1, "value");
		const UUID TargetA = PinID(*Graph, 2, "a");
		const UUID TargetB = PinID(*Graph, 2, "b");
		const UUID VectorTarget = PinID(*Graph, 4, "surface");
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, *Graph, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		EditorGraphDocumentEditorTestAccess::SetCanvasSettings(GraphEditor, *Session,
			"{\"view\":{\"scroll\":{\"x\":-83.0,\"y\":34.0},\"zoom\":0.82}}");
		EditorGraphDocumentEditorTestAccess::CapturePresentation(GraphEditor, *Session, true);

		ImVec2 HostOrigin(73.0f, 37.0f);
		bool SearchOwnsHostFocus = false;
		const auto DrawFrame = [&](ImVec2 Mouse, bool LeftDown = false, bool RDown = false, bool Escape = false)
		{
			IO.DisplaySize = ImVec2(1440.0f, 900.0f);
			IO.AddMousePosEvent(Mouse.x, Mouse.y);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, LeftDown);
			IO.AddKeyEvent(ImGuiKey_R, RDown);
			IO.AddKeyEvent(ImGuiKey_Escape, Escape);
			const bool PreviousInputTrickle = IO.ConfigInputTrickleEventQueue;
			IO.ConfigInputTrickleEventQueue = false;
			ImGui::NewFrame();
			IO.ConfigInputTrickleEventQueue = PreviousInputTrickle;
			IO.MousePos = Mouse;
			IO.MouseDown[ImGuiMouseButton_Left] = LeftDown;
			ImGui::SetNextWindowPos(HostOrigin);
			ImGui::SetNextWindowSize(ImVec2(1367.0f, 850.0f));
			ImGui::Begin("GraphWireRouteHost", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
			if (!SearchOwnsHostFocus) ImGui::SetWindowFocus();
			EditorGraphDocumentEditorTestAccess::Draw(GraphEditor, *Session);
			ImGui::End();
			ImGui::Render();
			SearchOwnsHostFocus = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).ContextualSearchOpen;
		};
		const auto FindPinScreen = [&](UUID PinID)
		{
			DrawFrame(ImVec2(-1000.0f, -1000.0f));
			for (const auto& Node : EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session))
				for (const auto& Pin : Node.Pins)
					if (Pin.PinID == PinID)
					{
						const auto Screen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
							GraphEditor, *Session, Pin.Marker.Anchor);
						return ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y));
					}
			return ImVec2(-1000.0f, -1000.0f);
		};
		const auto ScreenCanvas = [&](ImVec2 Screen)
		{
			return EditorGraphDocumentEditorTestAccess::ScreenToCanvas(GraphEditor, *Session, { Screen.x, Screen.y });
		};
		const auto CurrentPinScreen = [&](UUID PinID)
		{
			for (const auto& Node : EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session))
				for (const auto& Pin : Node.Pins)
					if (Pin.PinID == PinID)
					{
						const auto Screen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
							GraphEditor, *Session, Pin.Marker.Anchor);
						return ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y));
					}
			return ImVec2(-1000.0f, -1000.0f);
		};

		const EditorGraph Before = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const ImVec2 SourceAPoint = FindPinScreen(SourceA);
		const ImVec2 TargetAPoint = FindPinScreen(TargetA);
		const ImVec2 TargetBPoint = FindPinScreen(TargetB);
		const ImVec2 KnotOne(HostOrigin.x + 520.0f, HostOrigin.y + 285.0f);
		const ImVec2 KnotTwo(HostOrigin.x + 600.0f, HostOrigin.y + 390.0f);
		DrawFrame(SourceAPoint);
		DrawFrame(SourceAPoint, true);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).HoveredPin != 0);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).HasPendingStart);
		DrawFrame(KnotOne, true);
		auto DragProbe = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, DragProbe.MouseDragging);
		PF_CHECK(Tests, DragProbe.HoveredPin == 0 && DragProbe.HoveredNode == 0);
		PF_CHECK(Tests, DragProbe.HasPendingStart);
		PF_CHECK(Tests, DragProbe.PointerInCanvasBackground);
		DrawFrame(KnotOne, true, true);
		auto Route = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, Route.KeyPressed);
		PF_CHECK(Tests, Route.InputEligible);
		PF_CHECK(Tests, Route.Focused);
		PF_CHECK(Tests, Route.LeftDown);
		PF_CHECK(Tests, !Route.EscapeDown);
		PF_CHECK(Tests, !Route.TextInputOwned);
		PF_CHECK(Tests, !Route.PopupOwned);
		PF_CHECK(Tests, Route.Active && Route.KnotPositions.size() == 1 &&
			std::abs(Route.KnotPositions.front().X - ScreenCanvas(KnotOne).X) < 0.1 &&
			std::abs(Route.KnotPositions.front().Y - ScreenCanvas(KnotOne).Y) < 0.1 &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Before && !Session->IsDirty());
		DrawFrame(KnotOne, true, true);
		Route = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, Route.Active && Route.KnotPositions.size() == 1);
		DrawFrame(KnotOne, true, false);
		DrawFrame(KnotTwo, true, true);
		Route = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, Route.Active && Route.KnotPositions.size() == 2);
		DrawFrame(KnotTwo, true, false);
		const ImVec2 TailOne(HostOrigin.x + 650.0f, HostOrigin.y + 320.0f);
		DrawFrame(TailOne, true);
		const auto PreviewOne = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		const auto TailOneCanvas = ScreenCanvas(TailOne);
		PF_CHECK(Tests, PreviewOne.Active && PreviewOne.PreviewEndpoints.size() == 6 &&
			std::abs(PreviewOne.PreviewEndpoints.back().X - TailOneCanvas.X) < 0.1 &&
			std::abs(PreviewOne.PreviewEndpoints.back().Y - TailOneCanvas.Y) < 0.1);
		const ImVec2 TailTwo(HostOrigin.x + 670.0f, HostOrigin.y + 345.0f);
		DrawFrame(TailTwo, true);
		const auto PreviewTwo = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		const auto TailTwoCanvas = ScreenCanvas(TailTwo);
		PF_CHECK(Tests, PreviewTwo.PreviewEndpoints.size() == 6 &&
			PreviewTwo.PreviewEndpoints.back().X != PreviewOne.PreviewEndpoints.back().X &&
			std::abs(PreviewTwo.PreviewEndpoints.back().X - TailTwoCanvas.X) < 0.1 &&
			std::abs(PreviewTwo.PreviewEndpoints.back().Y - TailTwoCanvas.Y) < 0.1);
		DrawFrame(TargetAPoint, false);
		const auto RouteRelease = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, !RouteRelease.ReleaseTargetPin.IsNil());
		PF_CHECK(Tests, RouteRelease.ReleaseCommitRequested);
		PF_CHECK(Tests, RouteRelease.ReleaseCommitSucceeded);
		const EditorGraph AfterRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, AfterRoute.Nodes.size() == Before.Nodes.size() + 2 &&
			AfterRoute.Connections.size() == Before.Connections.size() + 3 && Session->IsDirty());
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		const EditorGraph AfterFinalUndo = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, AfterFinalUndo.Nodes.size() == Before.Nodes.size() + 2 &&
			AfterFinalUndo.Connections.size() == Before.Connections.size() + 2);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		const EditorGraph AfterSecondKnotUndo = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, AfterSecondKnotUndo.Nodes.size() == Before.Nodes.size() + 1 &&
			AfterSecondKnotUndo.Connections.size() == Before.Connections.size() + 1);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Before);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
			Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
			Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == AfterRoute);

		// Releasing an R-staged route on a node body commits the knots as an open
		// route, but does not treat the body (including its inline controls) as a
		// search drop or as a final connection target.
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Before, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		DrawFrame(SourceAPoint);
		const auto MultiplyPresentation = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
		ImVec2 MultiplyControlPoint(-1000.0f, -1000.0f);
		for (const auto& Node : MultiplyPresentation)
			if (Node.NodeID == Graph->Nodes[2].ID)
				for (const auto& Pin : Node.Pins)
					if (Pin.PinID == TargetA && Pin.PropertyControl)
						MultiplyControlPoint = ImVec2(
							static_cast<float>((Pin.PropertyControl->Minimum.X + Pin.PropertyControl->Maximum.X) * 0.5),
							static_cast<float>((Pin.PropertyControl->Minimum.Y + Pin.PropertyControl->Maximum.Y) * 0.5));
		PF_CHECK(Tests, MultiplyControlPoint.x > HostOrigin.x && MultiplyControlPoint.y > HostOrigin.y);
		DrawFrame(SourceAPoint, true);
		DrawFrame(KnotOne, true);
		DrawFrame(KnotOne, true, true);
		DrawFrame(KnotOne, true, false);
		DrawFrame(KnotTwo, true);
		DrawFrame(KnotTwo, true, true);
		DrawFrame(KnotTwo, true, false);
		DrawFrame(MultiplyControlPoint, true);
		DrawFrame(MultiplyControlPoint, false);
		const auto BodyDrop = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		const EditorGraph BodyRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, BodyDrop.ReleaseCommitRequested && BodyDrop.ReleaseCommitSucceeded &&
			BodyDrop.ReleaseTargetPin.IsNil() && !BodyDrop.ContextualSearchOpen && BodyDrop.KnotPositions.empty());
		PF_CHECK(Tests, BodyRoute.Nodes.size() == Before.Nodes.size() + 2 &&
			BodyRoute.Connections.size() == Before.Connections.size() + 2 && Session->IsDirty());
		PF_CHECK(Tests, std::none_of(BodyRoute.Connections.begin(), BodyRoute.Connections.end(),
			[&](const auto& Link) { return Link.InputPin == TargetA || Link.InputPin == TargetB; }));
		const auto OriginalMultiply = std::find_if(Before.Nodes.begin(), Before.Nodes.end(),
			[&](const auto& Node) { return Node.ID == Graph->Nodes[2].ID; });
		const auto BodyDropMultiply = std::find_if(BodyRoute.Nodes.begin(), BodyRoute.Nodes.end(),
			[&](const auto& Node) { return Node.ID == Graph->Nodes[2].ID; });
		PF_CHECK(Tests, OriginalMultiply != Before.Nodes.end() && BodyDropMultiply != BodyRoute.Nodes.end() &&
			*OriginalMultiply == *BodyDropMultiply);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo));
		const EditorGraph BodyRouteOneKnot = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, BodyRouteOneKnot.Nodes.size() == Before.Nodes.size() + 1 &&
			BodyRouteOneKnot.Connections.size() == Before.Connections.size() + 1);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == BodyRoute);

		// Input-origin routes use the same body-drop behavior and retain the start
		// pin as the free endpoint. A pin on that same node still wins over its body.
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Before, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const ImVec2 InputBodySourcePoint = FindPinScreen(TargetB);
		DrawFrame(InputBodySourcePoint, true);
		DrawFrame(KnotOne, true, true);
		DrawFrame(KnotOne, true, false);
		DrawFrame(KnotTwo, true);
		DrawFrame(KnotTwo, true, true);
		DrawFrame(KnotTwo, true, false);
		DrawFrame(MultiplyControlPoint, true);
		DrawFrame(MultiplyControlPoint, false);
		const auto InputBodyDrop = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		const EditorGraph InputBodyRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, InputBodyDrop.ReleaseCommitSucceeded && InputBodyDrop.ReleaseTargetPin.IsNil() &&
			!InputBodyDrop.ContextualSearchOpen && InputBodyRoute.Nodes.size() == Before.Nodes.size() + 2 &&
			InputBodyRoute.Connections.size() == Before.Connections.size() + 2);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Before);
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Before, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const ImVec2 SameNodeTargetPoint = CurrentPinScreen(TargetB);
		DrawFrame(SourceAPoint, true);
		DrawFrame(KnotOne, true, true);
		DrawFrame(KnotOne, true, false);
		DrawFrame(KnotTwo, true);
		DrawFrame(KnotTwo, true, true);
		DrawFrame(KnotTwo, true, false);
		DrawFrame(SameNodeTargetPoint, true);
		DrawFrame(SameNodeTargetPoint, false);
		const auto SameNodePinDrop = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		const EditorGraph SameNodeRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, SameNodePinDrop.ReleaseCommitSucceeded && SameNodePinDrop.ReleaseTargetPin == TargetB &&
			!SameNodePinDrop.ContextualSearchOpen);
		PF_CHECK(Tests, SameNodeRoute.Nodes.size() == Before.Nodes.size() + 2 &&
			SameNodeRoute.Connections.size() == Before.Connections.size() + 3 &&
			std::any_of(SameNodeRoute.Connections.begin(), SameNodeRoute.Connections.end(),
				[TargetB](const auto& Link) { return Link.InputPin == TargetB; }));

		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Before, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		// An empty-canvas release after placing a knot publishes an open, typed route. Its
		// free reroute pin remains available to a later ordinary canvas link operation.
		const ImVec2 OpenKnot(HostOrigin.x + 1180.0f, HostOrigin.y + 700.0f);
		const ImVec2 OpenKnotTwo(HostOrigin.x + 1110.0f, HostOrigin.y + 625.0f);
		const ImVec2 FreshOpenSourcePoint = FindPinScreen(SourceA);
		DrawFrame(FreshOpenSourcePoint);
		DrawFrame(FreshOpenSourcePoint, true);
		DrawFrame(OpenKnot, true);
		DrawFrame(OpenKnot, true, true);
		DrawFrame(OpenKnot, true, false);
		DrawFrame(OpenKnotTwo, true);
		DrawFrame(OpenKnotTwo, true, true);
		DrawFrame(OpenKnotTwo, true, false);
		const ImVec2 OpenRouteReleasePoint(HostOrigin.x + 980.0f, HostOrigin.y + 690.0f);
		DrawFrame(OpenRouteReleasePoint, true);
		DrawFrame(OpenRouteReleasePoint, false);
		DrawFrame(OpenRouteReleasePoint);
		const auto SearchTail = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		const auto SearchReleaseCanvas = ScreenCanvas(OpenRouteReleasePoint);
		PF_CHECK(Tests, SearchTail.ContextualSearchOpen &&
			SearchTail.OpenEnded && SearchTail.PreviewEndpoints.size() == 2 &&
			std::abs(SearchTail.PreviewEndpoints.back().X - SearchReleaseCanvas.X) < 0.1 &&
			std::abs(SearchTail.PreviewEndpoints.back().Y - SearchReleaseCanvas.Y) < 0.1 &&
			std::abs(SearchTail.SearchDropPoint.X - SearchReleaseCanvas.X) < 0.1 &&
			std::abs(SearchTail.SearchDropPoint.Y - SearchReleaseCanvas.Y) < 0.1 &&
			std::abs(SearchTail.SearchDropScreen.X - OpenRouteReleasePoint.x) < 0.1 &&
			std::abs(SearchTail.SearchDropScreen.Y - OpenRouteReleasePoint.y) < 0.1);
		const EditorGraph KnotOnlyOpenRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const ImVec2 SearchResultPoint(
			static_cast<float>((SearchTail.FirstSearchResultBounds.Minimum.X + SearchTail.FirstSearchResultBounds.Maximum.X) * 0.5),
			static_cast<float>((SearchTail.FirstSearchResultBounds.Minimum.Y + SearchTail.FirstSearchResultBounds.Maximum.Y) * 0.5));
		DrawFrame(SearchResultPoint);
		const auto BrowsingTail = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, BrowsingTail.ContextualSearchOpen && BrowsingTail.PreviewEndpoints.size() == 2 &&
			std::abs(BrowsingTail.PreviewEndpoints.back().X - SearchReleaseCanvas.X) < 0.1 &&
			std::abs(BrowsingTail.PreviewEndpoints.back().Y - SearchReleaseCanvas.Y) < 0.1);
		DrawFrame(SearchResultPoint, true);
		DrawFrame(SearchResultPoint, false);
		DrawFrame(OpenRouteReleasePoint);
		DrawFrame(OpenRouteReleasePoint);
		const EditorGraph OpenRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::IsCanvasInputSuppressed(GraphEditor, *Session));
		PF_CHECK(Tests, OpenRoute.Nodes.size() == Before.Nodes.size() + 3 &&
			OpenRoute.Connections.size() == Before.Connections.size() + 3 && Session->IsDirty());
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).SearchPlacementAwaitingPinAnchor);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == KnotOnlyOpenRoute);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session).Nodes.size() == Before.Nodes.size() + 1);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Before);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
			Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
			Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == OpenRoute);
		UUID OpenRerouteOutput;
		const EditorGraphNode* LastReroute = nullptr;
		for (auto Node = OpenRoute.Nodes.rbegin(); Node != OpenRoute.Nodes.rend(); ++Node)
			if (IsEditorGraphRerouteType(Node->TypeKey))
			{
				LastReroute = &*Node;
				break;
			}
		if (LastReroute)
		{
			const auto Output = std::find_if(LastReroute->Pins.begin(), LastReroute->Pins.end(),
				[](const auto& Pin) { return Pin.Key == "out"; });
			const auto Input = std::find_if(LastReroute->Pins.begin(), LastReroute->Pins.end(),
				[](const auto& Pin) { return Pin.Key == "in"; });
			const bool HasRerouteInputLink = Input != LastReroute->Pins.end() &&
				std::any_of(OpenRoute.Connections.begin(), OpenRoute.Connections.end(), [&](const auto& Link)
				{
					if (Link.InputPin != Input->ID) return false;
					for (const auto& CandidateNode : OpenRoute.Nodes)
						if (IsEditorGraphRerouteType(CandidateNode.TypeKey) &&
							std::any_of(CandidateNode.Pins.begin(), CandidateNode.Pins.end(),
								[&](const auto& CandidatePin)
								{ return CandidatePin.ID == Link.OutputPin && CandidatePin.Key == "out"; })) return true;
					return Link.OutputPin == SourceA;
				});
			PF_CHECK(Tests, Output != LastReroute->Pins.end() && Input != LastReroute->Pins.end() &&
				HasRerouteInputLink);
			if (Output != LastReroute->Pins.end()) OpenRerouteOutput = Output->ID;
		}
		if (!OpenRerouteOutput.IsNil())
		{
			const auto CreatedLink = std::find_if(OpenRoute.Connections.begin(), OpenRoute.Connections.end(),
				[OpenRerouteOutput](const auto& Link) { return Link.OutputPin == OpenRerouteOutput; });
			PF_CHECK(Tests, CreatedLink != OpenRoute.Connections.end());
			if (CreatedLink != OpenRoute.Connections.end())
			{
				const auto UpdatedPresentation = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
				const auto CreatedPin = std::find_if(UpdatedPresentation.begin(), UpdatedPresentation.end(),
					[&](const auto& Node) { return std::any_of(Node.Pins.begin(), Node.Pins.end(),
						[&](const auto& Pin) { return Pin.PinID == CreatedLink->InputPin; }); });
				PF_CHECK(Tests, CreatedPin != UpdatedPresentation.end());
				if (CreatedPin != UpdatedPresentation.end())
				{
					const auto Pin = std::find_if(CreatedPin->Pins.begin(), CreatedPin->Pins.end(),
						[&](const auto& Candidate) { return Candidate.PinID == CreatedLink->InputPin; });
					PF_CHECK(Tests, Pin != CreatedPin->Pins.end());
					if (Pin != CreatedPin->Pins.end())
					{
						const auto PinScreen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
							GraphEditor, *Session, Pin->Marker.Anchor);
						PF_CHECK(Tests, std::abs(PinScreen.X - OpenRouteReleasePoint.x) < 1.0 &&
							std::abs(PinScreen.Y - OpenRouteReleasePoint.y) < 1.0);
					}
				}
			}
		}
		if (!OpenRerouteOutput.IsNil())
		{
			const ImVec2 RefreshedTargetBPoint = FindPinScreen(TargetB);
			const auto ReroutePresentations = EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session);
			const auto OutputGeometry = std::find_if(ReroutePresentations.begin(), ReroutePresentations.end(),
				[OpenRerouteOutput](const auto& Node)
				{
					return std::any_of(Node.Pins.begin(), Node.Pins.end(), [OpenRerouteOutput](const auto& Pin)
						{ return Pin.PinID == OpenRerouteOutput && Pin.HitBounds.has_value(); });
				});
			ImVec2 OpenOutputPoint(-1000.0f, -1000.0f);
			if (OutputGeometry != ReroutePresentations.end())
				if (const auto Pin = std::find_if(OutputGeometry->Pins.begin(), OutputGeometry->Pins.end(),
					[OpenRerouteOutput](const auto& Candidate) { return Candidate.PinID == OpenRerouteOutput; });
					Pin != OutputGeometry->Pins.end() && Pin->HitBounds)
				{
					const EditorGraphCanvasPoint HitCenter{
						(Pin->HitBounds->Minimum.X + Pin->HitBounds->Maximum.X) * 0.5,
						(Pin->HitBounds->Minimum.Y + Pin->HitBounds->Maximum.Y) * 0.5 };
					const auto Screen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(
						GraphEditor, *Session, HitCenter);
					OpenOutputPoint = ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y));
				}
			DrawFrame(RefreshedTargetBPoint);
			PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == TargetB);
			DrawFrame(OpenOutputPoint);
			PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetLastHoveredPin(GraphEditor, *Session) == OpenRerouteOutput);
			DrawFrame(OpenOutputPoint, true);
			DrawFrame(RefreshedTargetBPoint, true);
			DrawFrame(RefreshedTargetBPoint, true);
			DrawFrame(RefreshedTargetBPoint, false);
			DrawFrame(RefreshedTargetBPoint, false);
			const EditorGraph CompletedOpenRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
			PF_CHECK(Tests, CompletedOpenRoute.Nodes.size() == OpenRoute.Nodes.size());
			PF_CHECK(Tests, CompletedOpenRoute.Connections.size() == OpenRoute.Connections.size() + 1);
			PF_CHECK(Tests,
				std::any_of(CompletedOpenRoute.Connections.begin(), CompletedOpenRoute.Connections.end(),
					[&](const auto& Link) { return Link.OutputPin == OpenRerouteOutput && Link.InputPin == TargetB; }));
			if (CompletedOpenRoute.Connections.size() == OpenRoute.Connections.size() + 1)
			{
				PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
					EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == OpenRoute);
				PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Redo) &&
					EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == CompletedOpenRoute);
			}
		}
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const auto OpenRouteBytes = ReadBytes(Project->GetRootPath() / Fixture->ProjectRelativePath);
		if (OpenRouteBytes)
		{
			const auto OpenRouteRoundTrip = DeserializeEditorGraph(*OpenRouteBytes, Schema);
			const EditorGraph SavedRouteGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
			const bool SameConnections = OpenRouteRoundTrip &&
				OpenRouteRoundTrip->Connections.size() == SavedRouteGraph.Connections.size() &&
				std::all_of(SavedRouteGraph.Connections.begin(), SavedRouteGraph.Connections.end(),
					[&](const auto& Expected)
					{
						return std::find_if(OpenRouteRoundTrip->Connections.begin(), OpenRouteRoundTrip->Connections.end(),
							[&](const auto& Actual)
								{ return Actual.ID == Expected.ID && Actual.OutputPin == Expected.OutputPin &&
									Actual.InputPin == Expected.InputPin; }) != OpenRouteRoundTrip->Connections.end();
					});
			PF_CHECK(Tests, OpenRouteRoundTrip && OpenRouteRoundTrip->Nodes.size() == SavedRouteGraph.Nodes.size() &&
				SameConnections);
		}
		else PF_CHECK(Tests, false);
		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Before, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		const ImVec2 DismissSourcePoint = FindPinScreen(SourceA);
		DrawFrame(DismissSourcePoint);
		DrawFrame(DismissSourcePoint, true);
		DrawFrame(OpenKnot, true);
		DrawFrame(OpenKnot, true, true);
		DrawFrame(OpenKnot, true, false);
		DrawFrame(OpenKnotTwo, true);
		DrawFrame(OpenKnotTwo, true, true);
		DrawFrame(OpenKnotTwo, true, false);
		DrawFrame(OpenRouteReleasePoint, true);
		DrawFrame(OpenRouteReleasePoint, false);
		DrawFrame(OpenRouteReleasePoint);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).ContextualSearchOpen);
		DrawFrame(OpenRouteReleasePoint, false, false, true);
		DrawFrame(OpenRouteReleasePoint);
		const auto DismissedSearchRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, DismissedSearchRoute.Nodes.size() == Before.Nodes.size() + 2 &&
			DismissedSearchRoute.Connections.size() == Before.Connections.size() + 2 && Session->IsDirty());
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).ContextualSearchOpen);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session).Nodes.size() == Before.Nodes.size() + 1 &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session).Connections.size() == Before.Connections.size() + 1);
		PF_CHECK(Tests, Editors.DispatchFocusedCommand(*Session, EditorDocumentCommand::Undo) &&
			EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Before);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		// An incompatible target and an off-canvas release cancel the staged draft without
		// changing graph contents, dirty state, or the clean history's Undo availability.
		const ImVec2 InvalidTargetPoint = FindPinScreen(VectorTarget);
		const ImVec2 InvalidRouteKnot(HostOrigin.x + 1050.0f, HostOrigin.y + 690.0f);
		DrawFrame(SourceAPoint);
		DrawFrame(SourceAPoint, true);
		DrawFrame(InvalidRouteKnot, true);
		DrawFrame(InvalidRouteKnot, true, true);
		DrawFrame(InvalidTargetPoint, true);
		DrawFrame(InvalidTargetPoint, false);
		const auto InvalidRelease = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, InvalidRelease.ReleaseTargetPin == VectorTarget && !InvalidRelease.ReleaseCommitSucceeded);
		const auto AfterInvalidRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, AfterInvalidRoute == Before);
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).Active);
		const ImVec2 OutsideRouteKnot(HostOrigin.x + 1100.0f, HostOrigin.y + 690.0f);
		const ImVec2 FreshOutsideSourcePoint = FindPinScreen(SourceA);
		DrawFrame(FreshOutsideSourcePoint);
		DrawFrame(FreshOutsideSourcePoint, true);
		DrawFrame(OutsideRouteKnot, true);
		DrawFrame(OutsideRouteKnot, true, true);
		const auto OutsideRouteStage = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, OutsideRouteStage.PointerInCanvasBackground);
		PF_CHECK(Tests, OutsideRouteStage.Active);
		DrawFrame(ImVec2(-20.0f, -20.0f), false);
		const auto OutsideRelease = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		const auto OutsideGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		PF_CHECK(Tests, OutsideRelease.ReleaseTargetPin.IsNil() && !OutsideRelease.ReleaseCommitRequested);
		PF_CHECK(Tests, OutsideGraph == Before);
		PF_CHECK(Tests, !Session->IsDirty());
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).Active);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::IsCanvasInputSuppressed(GraphEditor, *Session));
		const ImVec2 InputKnot(HostOrigin.x + 450.0f, HostOrigin.y + 560.0f);
		const ImVec2 InputStartPoint = FindPinScreen(TargetB);
		DrawFrame(InputStartPoint);
		PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).Active);
		DrawFrame(InputStartPoint, true);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).HasPendingStart);
		DrawFrame(InputKnot, true, true);
		const auto InputRouteStage = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, InputRouteStage.Active);
		PF_CHECK(Tests, InputRouteStage.SourcePin == TargetB);
		DrawFrame(InputKnot, true, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).Active);
		const ImVec2 CurrentSourceBPoint = CurrentPinScreen(SourceB);
		DrawFrame(CurrentSourceBPoint, true);
		const auto InputRouteHover = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, InputRouteHover.HoveredPin != 0);
		PF_CHECK(Tests, InputRouteHover.HoveredNode != 0);
		DrawFrame(CurrentSourceBPoint, false);
		const auto InputRouteStatus = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
		PF_CHECK(Tests, InputRouteStatus.ReleaseCommitRequested);
		PF_CHECK(Tests, InputRouteStatus.ReleaseCommitSucceeded);
		const EditorGraph InputRoute = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
		const bool HasInputRouteSourceSegment = std::any_of(InputRoute.Connections.begin(), InputRoute.Connections.end(),
			[SourceB, TargetB](const auto& Link) { return Link.OutputPin == SourceB && Link.InputPin != TargetB; });
		const bool HasInputRouteTargetSegment = std::any_of(InputRoute.Connections.begin(), InputRoute.Connections.end(),
			[TargetB](const auto& Link) { return Link.InputPin == TargetB; });
		PF_CHECK(Tests, InputRoute.Nodes.size() == Before.Nodes.size() + 1);
		PF_CHECK(Tests, InputRoute.Connections.size() == Before.Connections.size() + 2);
		PF_CHECK(Tests, HasInputRouteSourceSegment);
		PF_CHECK(Tests, HasInputRouteTargetSegment);

		EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Before, Schema);
		PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
		DrawFrame(FindPinScreen(SourceA), true);
		DrawFrame(KnotOne, true, true);
		DrawFrame(KnotOne, true, false, true);
		DrawFrame(KnotOne, false);
		PF_CHECK(Tests, EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session) == Before &&
			!Session->IsDirty() && !EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).Active);

		// R is a route waypoint only over empty canvas. Pressing it while a native
		// connection drag is over a node body or pin must leave that native drag
		// available to complete as an ordinary direct connection.
		const auto NodeCenterScreen = [&](UUID NodeID)
		{
			for (const auto& Node : EditorGraphDocumentEditorTestAccess::GetPresentation(GraphEditor, *Session))
				if (Node.NodeID == NodeID)
				{
					const auto Screen = EditorGraphDocumentEditorTestAccess::CanvasToScreen(GraphEditor, *Session,
						{ Node.Position.X + Node.Size.X * 0.5, Node.Position.Y + Node.Size.Y * 0.5 });
					return ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y));
				}
			return ImVec2(-1000.0f, -1000.0f);
		};
		const auto VerifyRDoesNotTakePinOrNodeDrag = [&](bool PressOnNode, UUID DestinationPin)
		{
			EditorGraphDocumentEditorTestAccess::SetGraph(GraphEditor, *Session, Before, Schema);
			PF_CHECK(Tests, GraphEditor.Save(*Session) && !Session->IsDirty());
			const ImVec2 FreshSource = FindPinScreen(SourceA);
			const ImVec2 RPressPoint = PressOnNode ? NodeCenterScreen(Graph->Nodes[2].ID) :
				CurrentPinScreen(DestinationPin);
			DrawFrame(FreshSource);
			DrawFrame(FreshSource, true);
			DrawFrame(RPressPoint, true, true);
			const auto RPress = EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session);
			PF_CHECK(Tests, !RPress.Active && RPress.KnotPositions.empty());
			PF_CHECK(Tests, PressOnNode ? RPress.HoveredNode != 0 : RPress.HoveredPin != 0);
			PF_CHECK(Tests, RPress.KeyPressed && RPress.InputEligible && RPress.HasPendingStart);
			DrawFrame(RPressPoint, true, false);
			// The key did not take over the native drag. Return to its source and
			// release there; this no-op must leave graph content and dirty state alone.
			DrawFrame(FreshSource, true);
			DrawFrame(FreshSource, false);
			const auto UnchangedGraph = EditorGraphDocumentEditorTestAccess::GetGraph(GraphEditor, *Session);
			PF_CHECK(Tests, UnchangedGraph == Before && !Session->IsDirty());
			PF_CHECK(Tests, !EditorGraphDocumentEditorTestAccess::GetWireRoute(GraphEditor, *Session).Active);
		};
		VerifyRDoesNotTakePinOrNodeDrag(true, TargetA);
		VerifyRDoesNotTakePinOrNodeDrag(false, TargetB);
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
	TestGraphCanvasReconnectContextMenu(Tests);
	TestGraphCanvasWireRouteGesture(Tests);
	TestGraphCanvasCreateRequestPolling(Tests);
	TestGraphCanvasNodeCreationRequestPolling(Tests);
	TestGraphNodeSearchNavigation(Tests);
	TestGraphCanvasCoordinateOwnership(Tests);
	TestAltPinUnlinkInputOwnership(Tests);
	TestGraphStraightenShortcutOwnership(Tests);
	TestPinnedCanvasLinkGesture(Tests);
}
