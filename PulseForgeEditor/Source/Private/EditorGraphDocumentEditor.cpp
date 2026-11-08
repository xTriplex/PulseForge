#include "Editor/EditorGraphDocumentEditor.h"

#include "Editor/EditorGraphLayout.h"
#include "Editor/EditorGraphModel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	using namespace PulseForgeEditor;
	namespace NodeEditor = ax::NodeEditor;
	using Json = nlohmann::json;
	constexpr size_t MaximumGraphFileBytes = 2 * 1024 * 1024;
	constexpr size_t MaximumCanvasHandles = 32'768;

	struct GraphDocumentState;
	struct PinAnchorMeasurement
	{
		PulseForge::UUID NodeID;
		PulseForge::UUID PinID;
		double CanvasY = 0.0;
	};
	struct PendingNodeSearch
	{
		bool Active = false;
		bool OpenRequested = false;
		bool PopupOpen = false;
		bool FocusSearch = false;
		EditorDocumentIdentity Document;
		uint64_t SessionGeneration = 0;
		std::string HostID;
		PulseForge::UUID GraphID;
		std::optional<PulseForge::UUID> SourcePin;
		EditorGraphVector3 Placement;
		std::array<char, 256> Search{};
		int SelectedResult = 0;
	};
	bool SaveCanvasSettings(const char* Data, size_t Size, NodeEditor::SaveReasonFlags, void* UserPointer);
	size_t LoadCanvasSettings(char* Data, void* UserPointer);

	struct GraphDocumentState final : EditorDocumentEditorState
	{
		GraphDocumentState(EditorGraph SourceGraph, EditorGraphSchema SourceSchema, std::filesystem::path Path,
			std::string SourceBytes)
			: History(std::move(SourceGraph)), Schema(std::move(SourceSchema)), SourcePath(std::move(Path)),
			  SavedBytes(std::move(SourceBytes))
		{
			Canvas = CreateCanvas();
		}

		NodeEditor::EditorContext* CreateCanvas()
		{
			NodeEditor::Config Configuration;
			Configuration.SettingsFile = nullptr;
			Configuration.UserPointer = this;
			Configuration.SaveSettings = SaveCanvasSettings;
			Configuration.LoadSettings = LoadCanvasSettings;
			return NodeEditor::CreateEditor(&Configuration);
		}

		~GraphDocumentState() noexcept override
		{
			if (Canvas) NodeEditor::DestroyEditor(Canvas);
		}

		EditorGraphCommandHistory History;
		EditorGraphSchema Schema;
		std::filesystem::path SourcePath;
		std::string SavedBytes;
		std::string Error;
		std::string CanvasSettings;
		NodeEditor::EditorContext* Canvas = nullptr;
		std::unordered_map<PulseForge::UUID, uintptr_t, PulseForge::UUIDHash> Handles;
		std::unordered_map<uintptr_t, PulseForge::UUID> Identities;
		uint64_t NextHandle = 1;
		bool NeedsCanvasSync = true;
		bool WasFocused = false;
		bool NodeMoveTransaction = false;
		PulseForge::UUID LastHoveredPin;
		bool LastPinWidgetHovered = false;
		PendingNodeSearch NodeSearch;
		EditorGraphPropertyEditCoordinator PropertyEdits;
	};

	void QueueNodeSearch(GraphDocumentState& State, const EditorDocumentSession& Session,
		std::optional<PulseForge::UUID> SourcePin, EditorGraphVector3 Placement)
	{
		State.NodeSearch = {};
		State.NodeSearch.Active = true;
		State.NodeSearch.OpenRequested = true;
		State.NodeSearch.FocusSearch = true;
		State.NodeSearch.Document = Session.Identity;
		State.NodeSearch.SessionGeneration = Session.SessionGeneration;
		State.NodeSearch.HostID = Session.HostID;
		State.NodeSearch.GraphID = State.History.Graph().ID;
		State.NodeSearch.SourcePin = SourcePin;
		State.NodeSearch.Placement = Placement;
	}

	bool IsNodeSearchSourceAvailable(const GraphDocumentState& State)
	{
		if (!State.NodeSearch.SourcePin) return true;
		for (const auto& Node : State.History.Graph().Nodes)
			if (std::any_of(Node.Pins.begin(), Node.Pins.end(), [&State](const auto& Pin)
				{ return Pin.ID == *State.NodeSearch.SourcePin; })) return true;
		return false;
	}

	bool SaveCanvasSettings(const char* Data, size_t Size, NodeEditor::SaveReasonFlags, void* UserPointer)
	{
		auto* State = static_cast<GraphDocumentState*>(UserPointer);
		if (!State || (!Data && Size != 0)) return false;
		State->CanvasSettings.assign(Data ? Data : "", Size);
		return true;
	}

	size_t LoadCanvasSettings(char* Data, void* UserPointer)
	{
		const auto* State = static_cast<const GraphDocumentState*>(UserPointer);
		if (!State) return 0;
		if (Data && !State->CanvasSettings.empty())
			std::memcpy(Data, State->CanvasSettings.data(), State->CanvasSettings.size());
		return State->CanvasSettings.size();
	}

	void CancelCanvasInteraction(GraphDocumentState& State)
	{
		if (State.Canvas)
		{
			NodeEditor::SetCurrentEditor(State.Canvas);
			NodeEditor::DestroyEditor(State.Canvas);
			State.Canvas = nullptr;
			NodeEditor::SetCurrentEditor(nullptr);
		}
		State.Canvas = State.CreateCanvas();
		State.NeedsCanvasSync = true;
		if (!State.Canvas) State.Error = "Could not restore the graph canvas after cancelling its interaction";
	}

	GraphDocumentState* GetState(EditorDocumentSession& Session)
	{
		return dynamic_cast<GraphDocumentState*>(Session.EditorState.get());
	}

	void ReconcileDirtyState(GraphDocumentState& State, EditorDocumentManager& Documents,
		EditorDocumentSession& Session)
	{
		if (State.History.IsDirty()) (void)Documents.MarkEdited(Session.Identity);
		else (void)Documents.MarkSaved(Session.Identity, Session.Revision);
	}

	EditorGraphPropertyEditResult FinishPropertyEdit(GraphDocumentState& State, EditorDocumentManager& Documents,
		EditorDocumentSession& Session)
	{
		auto Result = State.PropertyEdits.Finish(State.History, State.Schema);
		if (!Result.Error.empty()) State.Error = Result.Error;
		if (Result.GraphChanged)
			ReconcileDirtyState(State, Documents, Session);
		return Result;
	}

	const EditorGraphNodeDefinition* FindNodeDefinition(const EditorGraphSchema& Schema, std::string_view Key)
	{
		const auto It = std::find_if(Schema.Nodes.begin(), Schema.Nodes.end(), [Key](const auto& Node)
			{ return Node.TypeKey == Key; });
		return It == Schema.Nodes.end() ? nullptr : &*It;
	}

	std::string ReadableNodeLabel(const EditorGraphSchema& Schema, std::string_view Key)
	{
		const auto* Definition = FindNodeDefinition(Schema, Key);
		return Definition ? Definition->Label : std::string(Key);
	}

	const EditorGraphPinDefinition* FindPinDefinition(const EditorGraphSchema& Schema,
		std::string_view NodeKey, std::string_view PinKey)
	{
		const auto* Node = FindNodeDefinition(Schema, NodeKey);
		if (!Node) return nullptr;
		const auto It = std::find_if(Node->Pins.begin(), Node->Pins.end(), [PinKey](const auto& Pin)
			{ return Pin.Key == PinKey; });
		return It == Node->Pins.end() ? nullptr : &*It;
	}

	std::expected<std::string, std::string> ReadFile(const std::filesystem::path& Path)
	{
		std::error_code Error;
		const auto Size = std::filesystem::file_size(Path, Error);
		if (Error) return std::unexpected("Could not inspect graph source: " + Error.message());
		if (Size > MaximumGraphFileBytes) return std::unexpected("Graph source exceeds the supported file size");
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::unexpected("Could not open graph source " + Path.string());
		std::string Text(static_cast<size_t>(Size), '\0');
		if (Size && !Input.read(Text.data(), static_cast<std::streamsize>(Size)))
			return std::unexpected("Could not read the complete graph source");
		return Text;
	}

	std::expected<void, std::string> WriteExclusiveFile(const std::filesystem::path& Path, std::string_view Contents)
	{
#if defined(_WIN32)
		HANDLE File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (File == INVALID_HANDLE_VALUE)
			return std::unexpected("Could not exclusively create graph source: " + std::to_string(GetLastError()));
		size_t Offset = 0;
		while (Offset < Contents.size())
		{
			const DWORD Chunk = static_cast<DWORD>(std::min<size_t>(Contents.size() - Offset, std::numeric_limits<DWORD>::max()));
			DWORD Written = 0;
			if (!WriteFile(File, Contents.data() + Offset, Chunk, &Written, nullptr) || Written == 0)
			{
				const auto Code = GetLastError();
				CloseHandle(File);
				DeleteFileW(Path.c_str());
				return std::unexpected("Could not write graph source: " + std::to_string(Code));
			}
			Offset += Written;
		}
		if (!FlushFileBuffers(File))
		{
			const auto Code = GetLastError();
			CloseHandle(File);
			DeleteFileW(Path.c_str());
			return std::unexpected("Could not flush graph source: " + std::to_string(Code));
		}
		CloseHandle(File);
#else
		const int File = ::open(Path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (File < 0) return std::unexpected("Could not exclusively create graph source: " + std::string(std::strerror(errno)));
		size_t Offset = 0;
		while (Offset < Contents.size())
		{
			const ssize_t Written = ::write(File, Contents.data() + Offset, Contents.size() - Offset);
			if (Written <= 0)
			{
				const std::string Message = std::strerror(errno);
				::close(File);
				(void)::unlink(Path.c_str());
				return std::unexpected("Could not write graph source: " + Message);
			}
			Offset += static_cast<size_t>(Written);
		}
		if (::fsync(File) != 0)
		{
			const std::string Message = std::strerror(errno);
			::close(File);
			(void)::unlink(Path.c_str());
			return std::unexpected("Could not flush graph source: " + Message);
		}
		::close(File);
#endif
		return {};
	}

	std::expected<void, std::string> PublishGraphSource(const std::filesystem::path& Destination,
		std::string_view ExpectedSource, std::string_view NewSource, bool CreateOnly)
	{
		std::error_code Error;
		std::filesystem::create_directories(Destination.parent_path(), Error);
		if (Error) return std::unexpected("Could not create graph source directory: " + Error.message());
		if (CreateOnly && std::filesystem::exists(Destination, Error))
			return std::unexpected("A graph fixture already exists at the destination");
		if (Error) return std::unexpected("Could not inspect graph destination: " + Error.message());
		if (!CreateOnly)
		{
			auto Current = ReadFile(Destination);
			if (!Current) return std::unexpected(Current.error());
			if (*Current != ExpectedSource)
				return std::unexpected("Graph source changed outside this document; reload before saving");
		}
		auto TemporaryID = PulseForge::UUID::Generate();
		if (!TemporaryID) return std::unexpected(TemporaryID.error().Message);
		const auto Temporary = Destination.parent_path() /
			(Destination.filename().string() + "." + TemporaryID->ToString() + ".tmp");
		if (auto Written = WriteExclusiveFile(Temporary, NewSource); !Written) return std::unexpected(Written.error());
		if (!CreateOnly)
		{
			auto Current = ReadFile(Destination);
			if (!Current || *Current != ExpectedSource)
			{
				std::filesystem::remove(Temporary, Error);
				return std::unexpected(Current ? "Graph source changed during save; the draft remains dirty" : Current.error());
			}
		}
#if defined(_WIN32)
		const DWORD Flags = MOVEFILE_WRITE_THROUGH | (CreateOnly ? 0 : MOVEFILE_REPLACE_EXISTING);
		if (!MoveFileExW(Temporary.c_str(), Destination.c_str(), Flags))
		{
			const auto Code = GetLastError();
			DeleteFileW(Temporary.c_str());
			return std::unexpected("Could not publish graph source: " + std::to_string(Code));
		}
#else
		if (CreateOnly)
		{
			if (::link(Temporary.c_str(), Destination.c_str()) != 0)
			{
				const std::string Message = std::strerror(errno);
				(void)::unlink(Temporary.c_str());
				return std::unexpected("Could not publish new graph source: " + Message);
			}
			(void)::unlink(Temporary.c_str());
		}
		else if (::rename(Temporary.c_str(), Destination.c_str()) != 0)
		{
			const std::string Message = std::strerror(errno);
			(void)::unlink(Temporary.c_str());
			return std::unexpected("Could not publish graph source: " + Message);
		}
#endif
		return {};
	}

	std::expected<EditorGraphSchema, std::string> ReadSchema(std::string_view Source)
	{
		try
		{
			const Json Document = Json::parse(Source);
			if (!Document.is_object() || !Document.contains("schema") || !Document["schema"].is_string())
				return std::unexpected("Graph source has no valid schema identifier");
			const std::string Key = Document["schema"].get<std::string>();
			if (Key == "pulseforge.material-expression-fixture") return MakeMaterialExpressionFixtureSchema();
			if (Key == "pulseforge.gameplay-event-fixture") return MakeGameplayEventFixtureSchema();
			return std::unexpected("Unknown graph schema '" + Key + "'; this source remains untouched");
		}
		catch (const Json::exception& Error)
		{
			return std::unexpected(std::string("Graph source is malformed: ") + Error.what());
		}
	}

	EditorGraphValue ReadPinValue(const EditorGraphPin& Pin)
	{
		return Pin.DefaultValue;
	}

	const char* PinTypeLabel(EditorGraphPinType Type)
	{
		switch (Type)
		{
		case EditorGraphPinType::Float: return "float";
		case EditorGraphPinType::Vector3: return "vec3";
		case EditorGraphPinType::Boolean: return "bool";
		case EditorGraphPinType::String: return "string";
		case EditorGraphPinType::Execution: return "execution";
		}
		return "unknown";
	}
}

namespace PulseForgeEditor
{
	EditorGraphDocumentEditor::EditorGraphDocumentEditor(EditorDocumentManager& Documents,
		EditorDocumentEditorRegistry& Editors)
		: m_Documents(Documents), m_Editors(Editors)
	{
	}

	std::filesystem::path EditorGraphDocumentEditor::GetFixturePath(const PulseForge::UUID& GraphID)
	{
		return std::filesystem::path(".pulseforge") / "graphs" / (GraphID.ToString() + ".pfg");
	}

	std::expected<EditorGraphFixtureLocation, std::string> EditorGraphDocumentEditor::CreateFixture(
		const std::filesystem::path& ProjectRoot, EditorGraphFixtureKind Kind) const
	{
		if (ProjectRoot.empty()) return std::unexpected("Open a project before creating a graph authoring fixture");
		const EditorGraphSchema Schema = Kind == EditorGraphFixtureKind::MaterialExpression
			? MakeMaterialExpressionFixtureSchema() : MakeGameplayEventFixtureSchema();
		auto GraphID = PulseForge::UUID::Generate();
		if (!GraphID) return std::unexpected(GraphID.error().Message);
		auto Graph = CreateEditorGraph(Schema, *GraphID);
		if (!Graph) return std::unexpected(Graph.error());
		EditorGraphCommandHistory History(std::move(*Graph));
		const auto Add = [&History, &Schema](std::string_view Type, double X)
			{ return AddEditorGraphNode(History, Schema, Type, X, 100.0); };
		if (Kind == EditorGraphFixtureKind::MaterialExpression)
		{
			if (!Add("material.color", 100.0) || !Add("material.output", 420.0))
				return std::unexpected("Could not build the material-expression fixture");
			const auto& Color = History.Graph().Nodes[0];
			const auto& Output = History.Graph().Nodes[1];
			const auto* SourcePin = std::find_if(Color.Pins.begin(), Color.Pins.end(), [](const auto& Pin) { return Pin.Key == "color"; }).operator->();
			const auto* TargetPin = std::find_if(Output.Pins.begin(), Output.Pins.end(), [](const auto& Pin) { return Pin.Key == "surface"; }).operator->();
			if (!ConnectEditorGraphPins(History, Schema, SourcePin->ID, TargetPin->ID))
				return std::unexpected("Could not connect the material-expression fixture");
		}
		else
		{
			if (!Add("gameplay.event.begin", 80.0) || !Add("gameplay.flow.branch", 320.0) || !Add("gameplay.action.log", 600.0))
				return std::unexpected("Could not build the gameplay-event fixture");
			const auto& Event = History.Graph().Nodes[0];
			const auto& Branch = History.Graph().Nodes[1];
			const auto& Action = History.Graph().Nodes[2];
			const auto* EventPin = std::find_if(Event.Pins.begin(), Event.Pins.end(), [](const auto& Pin) { return Pin.Key == "event"; }).operator->();
			const auto* BranchInput = std::find_if(Branch.Pins.begin(), Branch.Pins.end(), [](const auto& Pin) { return Pin.Key == "exec-in"; }).operator->();
			const auto* BranchOutput = std::find_if(Branch.Pins.begin(), Branch.Pins.end(), [](const auto& Pin) { return Pin.Key == "true"; }).operator->();
			const auto* ActionInput = std::find_if(Action.Pins.begin(), Action.Pins.end(), [](const auto& Pin) { return Pin.Key == "exec-in"; }).operator->();
			if (!ConnectEditorGraphPins(History, Schema, EventPin->ID, BranchInput->ID) ||
				!ConnectEditorGraphPins(History, Schema, BranchOutput->ID, ActionInput->ID))
				return std::unexpected("Could not connect the gameplay-event fixture");
		}
		const auto Serialized = SerializeEditorGraph(History.Graph(), Schema);
		if (!Serialized) return std::unexpected(Serialized.error());
		const auto RelativePath = GetFixturePath(*GraphID);
		const auto FullPath = ProjectRoot / RelativePath;
		if (auto Written = PublishGraphSource(FullPath, {}, *Serialized, true); !Written)
			return std::unexpected(Written.error());
		const std::string Display = Kind == EditorGraphFixtureKind::MaterialExpression
			? "Material Expression Fixture — authoring only" : "Gameplay Event Fixture — authoring only";
		return EditorGraphFixtureLocation{ *GraphID, RelativePath, Display };
	}

	std::expected<void, std::string> EditorGraphDocumentEditor::Register()
	{
		if (m_Registered) return std::unexpected("The graph document editor is already registered");
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::Graph;
		Adapter.IsEligible = [this](const auto& Context) { return IsEligible(Context); };
		Adapter.LoadSession = [this](const auto& Context) { return LoadSession(Context); };
		Adapter.DrawWorkspace = [this](auto& Session) { DrawWorkspace(Session); };
		Adapter.Save = [this](auto& Session, uint64_t Revision) { return SaveSession(Session, Revision); };
		Adapter.Discard = [this](auto& Session) { return DiscardSession(Session); };
		Adapter.FocusedCommand = [this](auto& Session, auto Command) { return HandleFocusedCommand(Session, Command); };
		Adapter.AssetLocationChanged = [this](auto& Session, const auto& Path, auto Name)
			{ return UpdateAssetLocation(Session, Path, Name); };
		Adapter.PrepareClose = [this](const auto& Context) { return PrepareClose(Context); };
		auto Registered = m_Editors.Register(std::move(Adapter));
		if (!Registered) return std::unexpected(Registered.error());
		m_Registered = true;
		return {};
	}

	bool EditorGraphDocumentEditor::IsEligible(const EditorDocumentEditorContext& Context) const
	{
		return m_Registered && Context.Identity.Kind == EditorDocumentKind::Graph && !Context.Identity.Asset.IsNil() &&
			!Context.Identity.Project.IsNil() && !Context.ProjectRoot.empty() && Context.ProjectRelativePath == GetFixturePath(Context.Identity.Asset);
	}

	std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> EditorGraphDocumentEditor::LoadSession(
		const EditorDocumentEditorContext& Context) const
	{
		if (!IsEligible(Context)) return std::unexpected("The graph document does not identify a supported project fixture");
		const auto SourcePath = Context.ProjectRoot / Context.ProjectRelativePath;
		auto Source = ReadFile(SourcePath);
		if (!Source) return std::unexpected(Source.error());
		auto Schema = ReadSchema(*Source);
		if (!Schema) return std::unexpected(Schema.error());
		auto Graph = DeserializeEditorGraph(*Source, *Schema);
		if (!Graph) return std::unexpected(Graph.error());
		if (Graph->ID != Context.Identity.Asset) return std::unexpected("Graph source identity does not match the document identity");
		auto State = std::make_unique<GraphDocumentState>(std::move(*Graph), std::move(*Schema), SourcePath, std::move(*Source));
		if (!State->Canvas) return std::unexpected("Could not create an isolated graph canvas context");
		return std::unique_ptr<EditorDocumentEditorState>(std::move(State));
	}

	bool EditorGraphDocumentEditor::Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const
	{
		return m_Registered && Session.Identity.Kind == EditorDocumentKind::Graph && m_Editors.Load(Session, Context);
	}

	bool EditorGraphDocumentEditor::Save(EditorDocumentSession& Session) const
	{
		return m_Registered && Session.Identity.Kind == EditorDocumentKind::Graph && m_Editors.Save(Session);
	}

	bool EditorGraphDocumentEditor::SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision) const
	{
		auto* State = GetState(Session);
		if (!State) return false;
		const auto Finished = FinishPropertyEdit(*State, m_Documents, Session);
		if (!Finished.Error.empty() || State->History.HasOpenTransaction()) return false;
		auto Source = SerializeEditorGraph(State->History.Graph(), State->Schema);
		if (!Source) { State->Error = Source.error(); return false; }
		auto Published = PublishGraphSource(State->SourcePath, State->SavedBytes, *Source, false);
		if (!Published) { State->Error = Published.error(); return false; }
		State->SavedBytes = std::move(*Source);
		State->History.MarkSaved();
		State->Error.clear();
		(void)SavedRevision;
		return true;
	}

	bool EditorGraphDocumentEditor::ReloadState(EditorDocumentSession& Session) const
	{
		auto* State = GetState(Session);
		if (!State) return false;
		auto Source = ReadFile(State->SourcePath);
		if (!Source) { State->Error = Source.error(); return false; }
		auto Schema = ReadSchema(*Source);
		if (!Schema) { State->Error = Schema.error(); return false; }
		auto Graph = DeserializeEditorGraph(*Source, *Schema);
		if (!Graph || Graph->ID != Session.Identity.Asset)
		{
			State->Error = Graph ? "Graph source identity does not match the open document" : Graph.error();
			return false;
		}
		State->History.Reset(std::move(*Graph));
		State->Schema = std::move(*Schema);
		State->PropertyEdits.Reset();
		State->NodeMoveTransaction = false;
		State->SavedBytes = std::move(*Source);
		State->Error.clear();
		State->NeedsCanvasSync = true;
		(void)m_Documents.MarkSaved(Session.Identity, Session.Revision);
		return true;
	}

	bool EditorGraphDocumentEditor::DiscardSession(EditorDocumentSession& Session) const
	{
		return ReloadState(Session);
	}

	bool EditorGraphDocumentEditor::HandleFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command) const
	{
		auto* State = GetState(Session);
		if (!State) return false;
		(void)FinishPropertyEdit(*State, m_Documents, Session);
		if (State->History.HasOpenTransaction()) return false;
		const bool Changed = Command == EditorDocumentCommand::Undo ? State->History.Undo() : State->History.Redo();
		if (!Changed) return true;
		State->NeedsCanvasSync = true;
		State->Error.clear();
		if (State->History.IsDirty()) (void)m_Documents.MarkEdited(Session.Identity);
		else (void)m_Documents.MarkSaved(Session.Identity, Session.Revision);
		return true;
	}

	EditorGraphPropertyEditResult EditorGraphDocumentEditor::ProcessPropertyEditEvents(EditorDocumentSession& Session,
		const std::vector<EditorGraphPropertyEditEvent>& Events, bool CancelGesture)
	{
		auto* State = GetState(Session);
		if (!State) return { false, "Graph document state is unavailable" };
		auto Result = State->PropertyEdits.ProcessFrame(State->History, State->Schema, Events, CancelGesture);
		if (!Result.Error.empty()) State->Error = Result.Error;
		if (Result.GraphChanged)
		{
			if (Result.Error.empty()) State->Error.clear();
			ReconcileDirtyState(*State, m_Documents, Session);
		}
		Result.PropertyInteractionActive = State->PropertyEdits.HasActiveEdit();
		Result.TransactionOpen = State->History.HasOpenTransaction();
		return Result;
	}

	bool EditorGraphDocumentEditor::UpdateAssetLocation(EditorDocumentSession& Session,
		const std::filesystem::path& Path, std::string_view DisplayName) const
	{
		auto* State = GetState(Session);
		if (!State || Path != GetFixturePath(Session.Identity.Asset)) return false;
		Session.DisplayName = std::string(DisplayName);
		return true;
	}

	std::expected<void, std::string> EditorGraphDocumentEditor::PrepareClose(const EditorDocumentCloseContext& Context) const
	{
		if (const auto* State = dynamic_cast<const GraphDocumentState*>(Context.EditorState);
			State && State->History.HasOpenTransaction() && !State->PropertyEdits.HasActiveEdit())
			return std::unexpected("Finish or cancel the active graph gesture before closing this document");
		return {};
	}

	void EditorGraphDocumentEditor::DrawWorkspace(EditorDocumentSession& Session)
	{
		auto* State = GetState(Session);
		if (!State)
		{
			ImGui::TextUnformatted("Graph document state is unavailable.");
			return;
		}
		if (!State->Canvas)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.34f, 0.25f, 1.0f), "%s", State->Error.c_str());
			if (ImGui::Button("Retry Graph Canvas")) CancelCanvasInteraction(*State);
			return;
		}
		const bool Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const bool CancelGesture = (Focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) || (State->WasFocused && !Focused);
		if (CancelGesture)
		{
			if (State->History.HasOpenTransaction())
			{
				State->History.CancelTransaction();
				ReconcileDirtyState(*State, m_Documents, Session);
			}
			State->NodeMoveTransaction = false;
			State->PropertyEdits.Reset();
			CancelCanvasInteraction(*State);
		}
		State->WasFocused = Focused;
		const ImGuiIO& Input = ImGui::GetIO();
		const bool OtherInputOwner = ImGui::IsAnyItemActive();
		const bool SearchContextValid = State->NodeSearch.Active &&
			State->NodeSearch.Document == Session.Identity &&
			State->NodeSearch.SessionGeneration == Session.SessionGeneration &&
			State->NodeSearch.HostID == Session.HostID &&
			State->NodeSearch.GraphID == State->History.Graph().ID && IsNodeSearchSourceAvailable(*State);
		bool SearchPopupActive = State->NodeSearch.Active &&
			(State->NodeSearch.OpenRequested || State->NodeSearch.PopupOpen || ImGui::IsPopupOpen("GraphNodeSearch"));
		const bool CapturedAltPinClick = Focused && !Input.WantTextInput && !OtherInputOwner && !SearchPopupActive &&
			!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) && Input.KeyAlt &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
		if (Focused && !Input.WantTextInput && !State->LastHoveredPin.IsNil() &&
			!OtherInputOwner && !State->LastPinWidgetHovered && Input.KeyAlt &&
			ImGui::IsMouseDown(ImGuiMouseButton_Left))
			// The pin is an invisible item created at canvas End(), so claim its
			// gesture through ImGui's input-owner API before the canvas processes it.
			ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ImGuiKeyOwner_Any, ImGuiInputFlags_LockUntilRelease);

		const bool RequestSearchFromToolbar = ImGui::Button("Add Node") && Focused &&
			!Input.WantTextInput && !OtherInputOwner && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
		ImGui::SameLine();
		if (ImGui::Button("Undo"))
		{
			(void)FinishPropertyEdit(*State, m_Documents, Session);
			if (State->History.Undo())
			{
				State->NeedsCanvasSync = true;
				ReconcileDirtyState(*State, m_Documents, Session);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Redo"))
		{
			(void)FinishPropertyEdit(*State, m_Documents, Session);
			if (State->History.Redo())
			{
				State->NeedsCanvasSync = true;
				ReconcileDirtyState(*State, m_Documents, Session);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Copy Selection"))
		{
			NodeEditor::SetCurrentEditor(State->Canvas);
			const int Count = NodeEditor::GetSelectedObjectCount();
			std::vector<NodeEditor::NodeId> Handles(static_cast<size_t>(std::max(Count, 0)));
			const int Selected = NodeEditor::GetSelectedNodes(Handles.data(), static_cast<int>(Handles.size()));
			std::vector<PulseForge::UUID> NodeIDs;
			for (int Index = 0; Index < Selected; ++Index)
				if (auto It = State->Identities.find(Handles[static_cast<size_t>(Index)].Get()); It != State->Identities.end())
					NodeIDs.push_back(It->second);
			auto Payload = SerializeEditorGraphClipboard(CopyEditorGraphSubgraph(State->History.Graph(), NodeIDs));
			if (Payload) ImGui::SetClipboardText(Payload->c_str());
			else State->Error = Payload.error();
			NodeEditor::SetCurrentEditor(nullptr);
		}
		ImGui::SameLine();
		if (ImGui::Button("Paste Subgraph"))
		{
			const char* Clipboard = ImGui::GetClipboardText();
			if (Clipboard)
			{
				auto Pasted = PasteEditorGraphClipboard(State->History, State->Schema, Clipboard, 48.0, 48.0);
				if (!Pasted) State->Error = Pasted.error();
				else { State->Error.clear(); (void)m_Documents.MarkEdited(Session.Identity); State->NeedsCanvasSync = true; }
			}
		}
		ImGui::TextDisabled("%s · authoring fixture; no compilation or execution", State->Schema.Key.c_str());
		if (!State->Error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.34f, 0.25f, 1.0f), "%s", State->Error.c_str());

		NodeEditor::SetCurrentEditor(State->Canvas);
		auto& CanvasStyle = NodeEditor::GetStyle();
		CanvasStyle.Colors[NodeEditor::StyleColor_NodeBg] = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
		CanvasStyle.Colors[NodeEditor::StyleColor_NodeBorder] = ImGui::GetStyleColorVec4(ImGuiCol_Border);
		CanvasStyle.Colors[NodeEditor::StyleColor_HovNodeBorder] = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
		CanvasStyle.Colors[NodeEditor::StyleColor_SelNodeBorder] = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> LiveIDs;
		const auto RegisterHandle = [State, &LiveIDs](PulseForge::UUID ID) -> uintptr_t
		{
			LiveIDs.insert(ID);
			if (const auto It = State->Handles.find(ID); It != State->Handles.end()) return It->second;
			if (State->Handles.size() >= MaximumCanvasHandles || State->NextHandle > std::numeric_limits<uintptr_t>::max()) return 0;
			const uintptr_t Handle = static_cast<uintptr_t>(State->NextHandle++);
			if (Handle == 0 || Handle == std::numeric_limits<uintptr_t>::max()) return 0;
			State->Handles.emplace(ID, Handle);
			State->Identities.emplace(Handle, ID);
			return Handle;
		};
		std::vector<EditorGraphPropertyEditEvent> PropertyEvents;
		std::vector<PinAnchorMeasurement> PinAnchorScreenY;
		bool PropertyWidgetHovered = false;
		for (const auto& Node : State->History.Graph().Nodes)
		{
			LiveIDs.insert(Node.ID);
			for (const auto& Pin : Node.Pins) LiveIDs.insert(Pin.ID);
		}
		for (const auto& Link : State->History.Graph().Connections) LiveIDs.insert(Link.ID);
		for (auto It = State->Handles.begin(); It != State->Handles.end();)
			if (!LiveIDs.contains(It->first))
			{
				State->Identities.erase(It->second);
				It = State->Handles.erase(It);
			}
			else ++It;

		NodeEditor::Begin("##PulseForgeGraphCanvas", ImVec2(0.0f, 0.0f));
		if (RequestSearchFromToolbar)
		{
			const ImVec2 CanvasOrigin = NodeEditor::CanvasToScreen(ImVec2(0.0f, 0.0f));
			const ImVec2 CanvasSize = NodeEditor::GetScreenSize();
			const ImVec2 CanvasCenterScreen(CanvasOrigin.x + CanvasSize.x * 0.5f,
				CanvasOrigin.y + CanvasSize.y * 0.5f);
			const ImVec2 CanvasCenter = NodeEditor::ScreenToCanvas(CanvasCenterScreen);
			QueueNodeSearch(*State, Session, std::nullopt,
				{ CanvasCenter.x, CanvasCenter.y, 0.0 });
		}
		for (const auto& Node : State->History.Graph().Nodes)
		{
			const uintptr_t NodeHandle = RegisterHandle(Node.ID);
			if (!NodeHandle)
			{
				State->Error = "Graph canvas identity handle limit reached";
				continue;
			}
			const NodeEditor::NodeId CanvasNode(NodeHandle);
			if (State->NeedsCanvasSync) NodeEditor::SetNodePosition(CanvasNode, ImVec2(static_cast<float>(Node.X), static_cast<float>(Node.Y)));
			NodeEditor::BeginNode(CanvasNode);
			ImGui::TextUnformatted(ReadableNodeLabel(State->Schema, Node.TypeKey).c_str());
			for (const auto& Pin : Node.Pins)
			{
				const PulseForge::UUID PinID = Pin.ID;
				const uintptr_t PinHandle = RegisterHandle(Pin.ID);
				if (!PinHandle) continue;
				NodeEditor::BeginPin(NodeEditor::PinId(PinHandle), Pin.Direction == EditorGraphPinDirection::Input
					? NodeEditor::PinKind::Input : NodeEditor::PinKind::Output);
				ImGui::Text("%s  [%s]", Pin.Key.c_str(), PinTypeLabel(Pin.Type));
				if (Pin.Direction == EditorGraphPinDirection::Input && Pin.Type != EditorGraphPinType::Execution)
				{
					ImGui::SetNextItemWidth(128.0f);
					EditorGraphValue Value = ReadPinValue(Pin);
					bool Changed = false;
					const std::string Widget = "##" + Pin.ID.ToString();
					if (Pin.Type == EditorGraphPinType::Float)
					{
						double Number = std::get<double>(Value);
						Changed = ImGui::InputDouble(Widget.c_str(), &Number, 0.1, 1.0, "%.3f");
						if (Changed) Value = Number;
					}
					else if (Pin.Type == EditorGraphPinType::Vector3)
					{
						auto Vector = std::get<EditorGraphVector3>(Value);
						std::array<double, 3> Components{ Vector.X, Vector.Y, Vector.Z };
						Changed = ImGui::InputScalarN(Widget.c_str(), ImGuiDataType_Double, Components.data(),
							static_cast<int>(Components.size()), nullptr, nullptr, "%.3f");
						if (Changed) Value = EditorGraphVector3{ Components[0], Components[1], Components[2] };
					}
					else if (Pin.Type == EditorGraphPinType::Boolean)
					{
						bool Boolean = std::get<bool>(Value);
						Changed = ImGui::Checkbox(Widget.c_str(), &Boolean);
						if (Changed) Value = Boolean;
					}
					else if (Pin.Type == EditorGraphPinType::String)
					{
						std::array<char, 4097> Buffer{};
						const std::string& Text = std::get<std::string>(Value);
						std::copy_n(Text.data(), std::min(Text.size(), Buffer.size() - 1), Buffer.data());
						Changed = ImGui::InputText(Widget.c_str(), Buffer.data(), Buffer.size());
						if (Changed) Value = std::string(Buffer.data());
					}
					const bool Activated = ImGui::IsItemActivated();
					const bool Deactivated = ImGui::IsItemDeactivated();
					PropertyWidgetHovered |= ImGui::IsItemHovered();
					PropertyEvents.push_back({ PinID, std::move(Value), Activated, Changed, Deactivated });
				}
				NodeEditor::EndPin();
				const ImVec2 PinMinimum = ImGui::GetItemRectMin();
				const ImVec2 PinMaximum = ImGui::GetItemRectMax();
				const ImVec2 PinCenterCanvas = NodeEditor::ScreenToCanvas(
					ImVec2((PinMinimum.x + PinMaximum.x) * 0.5f, (PinMinimum.y + PinMaximum.y) * 0.5f));
				if (std::isfinite(PinCenterCanvas.y)) PinAnchorScreenY.push_back({ Node.ID, Pin.ID, PinCenterCanvas.y });
			}
			NodeEditor::EndNode();
		}
		for (const auto& Connection : State->History.Graph().Connections)
		{
			const uintptr_t LinkHandle = RegisterHandle(Connection.ID);
			const uintptr_t OutputHandle = RegisterHandle(Connection.OutputPin);
			const uintptr_t InputHandle = RegisterHandle(Connection.InputPin);
			if (LinkHandle && OutputHandle && InputHandle)
				NodeEditor::Link(NodeEditor::LinkId(LinkHandle), NodeEditor::PinId(OutputHandle), NodeEditor::PinId(InputHandle));
		}

		std::optional<std::pair<PulseForge::UUID, PulseForge::UUID>> CreatedLink;
		const auto CreateRequest = GraphCanvasDetail::ProcessEditorGraphCanvasCreateRequestFrame(
			[] { return NodeEditor::BeginCreate(); },
			[] { NodeEditor::EndCreate(); },
			[]() -> std::optional<EditorGraphCanvasLinkRequest>
			{
				NodeEditor::PinId StartPin;
				NodeEditor::PinId EndPin;
				if (!NodeEditor::QueryNewLink(&StartPin, &EndPin)) return std::nullopt;
				return EditorGraphCanvasLinkRequest{ StartPin.Get(), EndPin.Get() };
			},
			[State](const EditorGraphCanvasLinkRequest& Request)
			{
				const auto Start = State->Identities.find(Request.StartPin);
				const auto End = State->Identities.find(Request.EndPin);
				if (Start == State->Identities.end() || End == State->Identities.end())
					return EditorGraphConnectionDecision{ false, "The dragged pin no longer exists", {}, {} };
				return CanConnectEditorGraphPins(State->History.Graph(), State->Schema, Start->second, End->second);
			},
			[] { return NodeEditor::AcceptNewItem(); },
			[] { NodeEditor::RejectNewItem(); },
			[]() -> std::optional<uintptr_t>
			{
				NodeEditor::PinId SourcePin;
				if (!NodeEditor::QueryNewNode(&SourcePin)) return std::nullopt;
				return SourcePin.Get();
			},
			[State](uintptr_t SourceHandle)
			{
				const auto Identity = State->Identities.find(SourceHandle);
				if (Identity == State->Identities.end()) return false;
				for (const auto& Node : State->History.Graph().Nodes)
					if (std::any_of(Node.Pins.begin(), Node.Pins.end(), [&Identity](const auto& Pin)
						{ return Pin.ID == Identity->second; })) return true;
				return false;
			},
			[] { return NodeEditor::AcceptNewItem(); },
			[] { NodeEditor::RejectNewItem(); });
		if (CreateRequest.SawLinkRequest) State->Error = CreateRequest.LinkDecision.Reason;
		if (CreateRequest.AcceptedLink)
			CreatedLink = std::pair{ CreateRequest.LinkDecision.OutputPin, CreateRequest.LinkDecision.InputPin };
		if (CreateRequest.SawNodeRequest && !CreateRequest.AcceptedNode && CreateRequest.SourcePin != 0 &&
			!State->Identities.contains(CreateRequest.SourcePin))
			State->Error = "The source pin for node creation is no longer available";
		std::optional<PulseForge::UUID> AcceptedNodeSource;
		std::optional<EditorGraphVector3> AcceptedNodePlacement;
		if (CreateRequest.AcceptedNode)
		{
			if (const auto Identity = State->Identities.find(CreateRequest.SourcePin); Identity != State->Identities.end())
			{
				const ImVec2 Placement = NodeEditor::ScreenToCanvas(ImGui::GetIO().MousePos);
				if (std::isfinite(Placement.x) && std::isfinite(Placement.y))
				{
					AcceptedNodeSource = Identity->second;
					AcceptedNodePlacement = EditorGraphVector3{ Placement.x, Placement.y, 0.0 };
				}
				else State->Error = "The node placement point could not be resolved";
			}
			else State->Error = "The source pin for node creation is no longer available";
		}

		std::vector<PulseForge::UUID> DeletedLinks;
		std::vector<PulseForge::UUID> DeletedNodes;
		NodeEditor::BeginDelete();
		NodeEditor::LinkId DeletedLink;
		while (NodeEditor::QueryDeletedLink(&DeletedLink))
		{
			if (auto It = State->Identities.find(DeletedLink.Get()); It != State->Identities.end())
			{
				DeletedLinks.push_back(It->second);
				NodeEditor::AcceptDeletedItem();
			}
			else NodeEditor::RejectDeletedItem();
		}
		NodeEditor::NodeId DeletedNode;
		while (NodeEditor::QueryDeletedNode(&DeletedNode))
		{
			if (auto It = State->Identities.find(DeletedNode.Get()); It != State->Identities.end())
			{
				DeletedNodes.push_back(It->second);
				NodeEditor::AcceptDeletedItem();
			}
			else NodeEditor::RejectDeletedItem();
		}
		NodeEditor::EndDelete();

		bool GraphChanged = false;
		if (CreatedLink)
		{
			std::expected<void, std::string> Result = std::unexpected("Unresolved graph link request");
			if (!DeletedLinks.empty())
				Result = ReconnectEditorGraphConnection(State->History, State->Schema, DeletedLinks.front(),
					CreatedLink->first, CreatedLink->second);
			else Result = ConnectEditorGraphPins(State->History, State->Schema, CreatedLink->first, CreatedLink->second);
			if (!Result) State->Error = Result.error();
			else GraphChanged = true;
		}
		if (!DeletedNodes.empty() || (!DeletedLinks.empty() && !CreatedLink && !CreateRequest.SawLinkRequest))
		{
			const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Nodes(DeletedNodes.begin(), DeletedNodes.end());
			const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Links(DeletedLinks.begin(), DeletedLinks.end());
			const EditorGraph Before = State->History.Graph();
			std::string Error;
			const bool Changed = State->History.Execute(State->Schema, [&Nodes, &Links](EditorGraph& Graph) -> std::expected<void, std::string>
			{
				std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> RemovedPins;
				Graph.Nodes.erase(std::remove_if(Graph.Nodes.begin(), Graph.Nodes.end(), [&Nodes, &RemovedPins](const auto& Node)
					{ if (!Nodes.contains(Node.ID)) return false; for (const auto& Pin : Node.Pins) RemovedPins.insert(Pin.ID); return true; }), Graph.Nodes.end());
				Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(), [&Links, &RemovedPins](const auto& Link)
					{ return Links.contains(Link.ID) || RemovedPins.contains(Link.OutputPin) || RemovedPins.contains(Link.InputPin); }), Graph.Connections.end());
				return {};
			}, Error);
			if (!Changed) State->Error = std::move(Error);
			else GraphChanged |= State->History.Graph() != Before;
		}
		if (GraphChanged)
		{
			State->Error.clear();
			(void)m_Documents.MarkEdited(Session.Identity);
		}

		if (State->NeedsCanvasSync) State->NeedsCanvasSync = false;
		std::vector<std::pair<PulseForge::UUID, EditorGraphVector3>> Positions;
		for (const auto& Node : State->History.Graph().Nodes)
		{
			const auto It = State->Handles.find(Node.ID);
			if (It == State->Handles.end()) continue;
			const auto Position = NodeEditor::GetNodePosition(NodeEditor::NodeId(It->second));
			if (std::isfinite(Position.x) && std::isfinite(Position.y) &&
				(std::abs(static_cast<double>(Position.x) - Node.X) > 0.01 || std::abs(static_cast<double>(Position.y) - Node.Y) > 0.01))
				Positions.emplace_back(Node.ID, EditorGraphVector3{ Position.x, Position.y, 0.0 });
		}
		if (!Positions.empty())
		{
			if (!State->History.HasOpenTransaction())
			{
				(void)State->History.BeginTransaction();
				State->NodeMoveTransaction = true;
			}
			auto Moved = MoveEditorGraphNodes(State->History, State->Schema, Positions);
			if (!Moved) State->Error = Moved.error();
			else (void)m_Documents.MarkEdited(Session.Identity);
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && State->NodeMoveTransaction && State->History.HasOpenTransaction())
		{
			std::string Error;
			if (!State->History.CommitTransaction(State->Schema, Error)) State->Error = std::move(Error);
			if (State->History.IsDirty()) (void)m_Documents.MarkEdited(Session.Identity);
			else (void)m_Documents.MarkSaved(Session.Identity, Session.Revision);
			State->NodeMoveTransaction = false;
		}

		const ImGuiIO& IO = ImGui::GetIO();
		if (Focused && NodeEditor::IsActive() && !IO.WantTextInput && !SearchPopupActive &&
			!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
		{
			const auto CopySelection = [&]
			{
				const int Count = NodeEditor::GetSelectedObjectCount();
				std::vector<NodeEditor::NodeId> Handles(static_cast<size_t>(std::max(Count, 0)));
				const int Selected = Count > 0 ? NodeEditor::GetSelectedNodes(Handles.data(), Count) : 0;
				std::vector<PulseForge::UUID> IDs;
				for (int Index = 0; Index < Selected; ++Index)
					if (const auto It = State->Identities.find(Handles[static_cast<size_t>(Index)].Get());
						It != State->Identities.end())
						IDs.push_back(It->second);
				auto Payload = SerializeEditorGraphClipboard(CopyEditorGraphSubgraph(State->History.Graph(), IDs));
				if (Payload) ImGui::SetClipboardText(Payload->c_str());
			};
			if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelection();
			if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
				if (const char* Clipboard = ImGui::GetClipboardText())
				{
					auto Pasted = PasteEditorGraphClipboard(State->History, State->Schema, Clipboard, 48.0, 48.0);
					if (!Pasted) State->Error = Pasted.error();
					else { State->Error.clear(); (void)m_Documents.MarkEdited(Session.Identity); State->NeedsCanvasSync = true; }
				}
		}
		if (AcceptedNodeSource && AcceptedNodePlacement)
		{
			QueueNodeSearch(*State, Session, AcceptedNodeSource, *AcceptedNodePlacement);
			SearchPopupActive = true;
		}

		bool BackgroundSearchRequested = false;
		NodeEditor::Suspend();
		if (Focused && !Input.WantTextInput && !SearchPopupActive && !OtherInputOwner &&
			!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
			BackgroundSearchRequested = NodeEditor::ShowBackgroundContextMenu();
		NodeEditor::Resume();
		if (BackgroundSearchRequested)
		{
			const ImVec2 Placement = NodeEditor::ScreenToCanvas(
				ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Right]);
			if (std::isfinite(Placement.x) && std::isfinite(Placement.y))
			{
				QueueNodeSearch(*State, Session, std::nullopt, { Placement.x, Placement.y, 0.0 });
				SearchPopupActive = true;
			}
			else State->Error = "The node placement point could not be resolved";
		}

		struct PendingCreation
		{
			EditorGraphNodeCreationAction Action;
			std::optional<PulseForge::UUID> SourcePin;
			EditorGraphVector3 Placement;
		};
		std::optional<PendingCreation> PendingNodeCreation;
		if (State->NodeSearch.Active)
		{
			const bool ContextValid = State->NodeSearch.Document == Session.Identity &&
				State->NodeSearch.SessionGeneration == Session.SessionGeneration &&
				State->NodeSearch.HostID == Session.HostID &&
				State->NodeSearch.GraphID == State->History.Graph().ID && IsNodeSearchSourceAvailable(*State);
			NodeEditor::Suspend();
			if (ContextValid && State->NodeSearch.OpenRequested)
			{
				ImGui::OpenPopup("GraphNodeSearch");
				State->NodeSearch.OpenRequested = false;
				State->NodeSearch.PopupOpen = true;
			}
			const bool PopupVisible = ImGui::BeginPopup("GraphNodeSearch");
			if (PopupVisible)
			{
				if (!ContextValid)
				{
					ImGui::TextUnformatted("This graph context is no longer available.");
					ImGui::CloseCurrentPopup();
					State->NodeSearch = {};
				}
				else
				{
					if (State->NodeSearch.FocusSearch) ImGui::SetKeyboardFocusHere();
					ImGui::SetNextItemWidth(340.0f);
					const bool SearchChanged = ImGui::InputText("##GraphNodeSearchInput",
						State->NodeSearch.Search.data(), State->NodeSearch.Search.size());
					if (SearchChanged) State->NodeSearch.SelectedResult = 0;
					State->NodeSearch.FocusSearch = false;

					const auto Actions = DiscoverEditorGraphNodeActions(State->History.Graph(), State->Schema,
						State->NodeSearch.SourcePin);
					const auto Matches = SearchEditorGraphNodeActions(Actions, State->NodeSearch.Search.data());
					const auto Navigation = GraphCanvasDetail::ResolveNodeSearchNavigation({ Matches.size(),
						State->NodeSearch.SelectedResult, SearchChanged,
						ImGui::IsKeyPressed(ImGuiKey_UpArrow, false),
						ImGui::IsKeyPressed(ImGuiKey_DownArrow, false),
						ImGui::IsKeyPressed(ImGuiKey_Enter, false),
						ImGui::IsKeyPressed(ImGuiKey_Escape, false) });
					State->NodeSearch.SelectedResult = Navigation.SelectedResult;

					std::optional<EditorGraphNodeCreationAction> ChosenAction;
					if (Navigation.ActivateResult) ChosenAction = Matches[*Navigation.ActivateResult];
					if (Actions.empty() && State->NodeSearch.SourcePin)
						ImGui::TextDisabled("No compatible nodes");
					else if (Matches.empty())
						ImGui::TextDisabled("No matching nodes");
					else
					{
						for (size_t Index = 0; Index < Matches.size(); ++Index)
						{
							const auto& Action = Matches[Index];
							std::string Label = Action.NodeLabel;
							if (Action.ConnectPinKey)
								Label += "  →  " + Action.ConnectPinLabel + "  [" + Action.NodeTypeKey + " · " +
									*Action.ConnectPinKey + "]";
							else Label += "  [" + Action.NodeTypeKey + "]";
							const bool Selected = static_cast<int>(Index) == State->NodeSearch.SelectedResult;
							if (ImGui::Selectable(Label.c_str(), Selected))
							{
								State->NodeSearch.SelectedResult = static_cast<int>(Index);
								ChosenAction = Action;
							}
							if (ImGui::IsItemHovered()) State->NodeSearch.SelectedResult = static_cast<int>(Index);
						}
					}
					if (Navigation.Cancelled)
					{
						ImGui::CloseCurrentPopup();
						State->NodeSearch = {};
					}
					else if (ChosenAction)
					{
						PendingNodeCreation = PendingCreation{ *ChosenAction, State->NodeSearch.SourcePin,
							State->NodeSearch.Placement };
						ImGui::CloseCurrentPopup();
						State->NodeSearch = {};
					}
				}
				ImGui::EndPopup();
			}
			NodeEditor::Resume();
			if (State->NodeSearch.Active && !State->NodeSearch.OpenRequested && !ImGui::IsPopupOpen("GraphNodeSearch"))
				State->NodeSearch = {};
		}
		SearchPopupActive = SearchPopupActive || State->NodeSearch.Active || ImGui::IsPopupOpen("GraphNodeSearch");
		NodeEditor::EnableShortcuts(!Input.WantTextInput && !SearchPopupActive &&
			!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup));
		NodeEditor::End();
		NodeEditor::EnableShortcuts(true);

		if (PendingNodeCreation)
		{
			std::expected<void, std::string> Added;
			if (PendingNodeCreation->Action.ConnectPinKey && PendingNodeCreation->SourcePin)
			{
				auto Created = CreateEditorGraphNodeAndConnect(State->History, State->Schema,
					PendingNodeCreation->Action.NodeTypeKey, *PendingNodeCreation->Action.ConnectPinKey,
					*PendingNodeCreation->SourcePin, PendingNodeCreation->Placement.X, PendingNodeCreation->Placement.Y);
				if (!Created) Added = std::unexpected(Created.error());
				else Added = {};
			}
			else Added = AddEditorGraphNode(State->History, State->Schema, PendingNodeCreation->Action.NodeTypeKey,
				PendingNodeCreation->Placement.X, PendingNodeCreation->Placement.Y);
			if (!Added) State->Error = Added.error();
			else
			{
				State->Error.clear();
				(void)m_Documents.MarkEdited(Session.Identity);
				State->NeedsCanvasSync = true;
			}
		}
		const auto HoveredCanvasPin = NodeEditor::GetHoveredPin();
		PulseForge::UUID HoveredPinID;
		if (HoveredCanvasPin.Get() != 0)
			if (const auto It = State->Identities.find(HoveredCanvasPin.Get()); It != State->Identities.end())
				HoveredPinID = It->second;
		const auto UnlinkPin = GraphCanvasDetail::ResolveAltPinUnlink({ HoveredPinID, Focused,
			Input.KeyAlt, CapturedAltPinClick, Input.WantTextInput || OtherInputOwner,
			ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup), PropertyWidgetHovered });
		if (UnlinkPin)
		{
			const EditorGraph BeforeUnlink = State->History.Graph();
			auto Unlinked = DisconnectEditorGraphPin(State->History, State->Schema, *UnlinkPin);
			if (!Unlinked) State->Error = Unlinked.error();
			else if (State->History.Graph() != BeforeUnlink)
			{
				State->Error.clear();
				ReconcileDirtyState(*State, m_Documents, Session);
			}
		}
		State->LastHoveredPin = HoveredPinID;
		State->LastPinWidgetHovered = PropertyWidgetHovered;

		if (GraphCanvasDetail::ShouldStraightenSelection({ Focused, NodeEditor::IsActive(),
			ImGui::IsKeyPressed(ImGuiKey_Q, false), IO.WantTextInput, OtherInputOwner,
			SearchPopupActive || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup), ImGui::IsMouseDown(ImGuiMouseButton_Left),
			State->NodeMoveTransaction, State->History.HasOpenTransaction(),
			State->PropertyEdits.HasActiveEdit(), PropertyWidgetHovered }))
		{
			const int Count = NodeEditor::GetSelectedObjectCount();
			std::vector<NodeEditor::NodeId> SelectedHandles(static_cast<size_t>(std::max(Count, 0)));
			const int SelectedCount = Count > 0 ? NodeEditor::GetSelectedNodes(SelectedHandles.data(), Count) : 0;
			std::vector<PulseForge::UUID> SelectedNodeIDs;
			for (int Index = 0; Index < SelectedCount; ++Index)
				if (const auto It = State->Identities.find(SelectedHandles[static_cast<size_t>(Index)].Get());
					It != State->Identities.end()) SelectedNodeIDs.push_back(It->second);

			std::vector<EditorGraphLayoutNode> LayoutNodes;
			LayoutNodes.reserve(State->History.Graph().Nodes.size());
			std::unordered_map<PulseForge::UUID, double, PulseForge::UUIDHash> NodeCanvasY;
			for (const auto& Node : State->History.Graph().Nodes)
				if (const auto It = State->Handles.find(Node.ID); It != State->Handles.end())
				{
					const auto Position = NodeEditor::GetNodePosition(NodeEditor::NodeId(It->second));
					if (std::isfinite(Position.x) && std::isfinite(Position.y))
					{
						LayoutNodes.push_back({ Node.ID, { Position.x, Position.y, 0.0 } });
						NodeCanvasY.emplace(Node.ID, Position.y);
					}
				}
			std::vector<EditorGraphPinAnchor> Anchors;
			for (const auto& Measurement : PinAnchorScreenY)
				if (const auto It = NodeCanvasY.find(Measurement.NodeID); It != NodeCanvasY.end())
					Anchors.push_back({ Measurement.NodeID, Measurement.PinID, Measurement.CanvasY - It->second });
			const auto Positions = CalculateEditorGraphStraightenedLayout(LayoutNodes, Anchors,
				State->History.Graph().Connections, SelectedNodeIDs);
			if (!Positions.empty())
			{
				const EditorGraph BeforeLayout = State->History.Graph();
				auto Moved = MoveEditorGraphNodes(State->History, State->Schema, Positions);
				if (!Moved) State->Error = Moved.error();
				else if (State->History.Graph() != BeforeLayout)
				{
					State->Error.clear();
					(void)m_Documents.MarkEdited(Session.Identity);
					State->NeedsCanvasSync = true;
				}
			}
		}
		NodeEditor::SetCurrentEditor(nullptr);
		(void)ProcessPropertyEditEvents(Session, PropertyEvents, CancelGesture);
	}
}
