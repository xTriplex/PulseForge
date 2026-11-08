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
		EditorGraphCanvasPoint Position;
	};
	struct PendingScreenPinRelease
	{
		bool Valid = false;
		EditorGraphCanvasDocumentContext Context;
		EditorGraphScreenPoint Position;
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
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> HiddenPins;
		uint64_t NextHandle = 1;
		bool NeedsCanvasSync = true;
		bool WasFocused = false;
		bool NodeMoveTransaction = false;
		PulseForge::UUID LastHoveredPin;
		bool LastPinWidgetHovered = false;
		PendingNodeSearch NodeSearch;
		PendingScreenPinRelease ScreenPinRelease;
		EditorGraphPendingPinRelease PinRelease;
		EditorGraphPropertyEditCoordinator PropertyEdits;
		bool CapturePresentation = false;
		std::vector<EditorGraphCanvasNodePresentation> Presentation;
	};

	std::string FitGraphPresentationText(std::string_view Text, float MaximumWidth)
	{
		if (ImGui::CalcTextSize(Text.data(), Text.data() + Text.size()).x <= MaximumWidth)
			return std::string(Text);
		constexpr std::string_view Ellipsis = "…";
		std::string Result(Text);
		while (!Result.empty() && ImGui::CalcTextSize((Result + std::string(Ellipsis)).c_str()).x > MaximumWidth)
		{
			size_t Cut = Result.size() - 1;
			while (Cut > 0 && (static_cast<unsigned char>(Result[Cut]) & 0xC0) == 0x80) --Cut;
			Result.resize(Cut);
		}
		Result += Ellipsis;
		return Result;
	}

	EditorGraphCanvasDocumentContext MakeCanvasDocumentContext(const EditorDocumentSession& Session,
		PulseForge::UUID GraphID)
	{
		return { Session.Identity, Session.SessionGeneration, Session.HostID, GraphID };
	}

	EditorGraphCanvasPinMarkerGeometry DrawGraphPinMarker(const EditorGraphPin& Pin, bool Connected,
		std::string_view Tooltip)
	{
		constexpr float MarkerSize = 14.0f;
		const float VerticalInset = std::max((ImGui::GetFrameHeight() - MarkerSize) * 0.5f, 0.0f);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + VerticalInset);
		ImGui::Dummy(ImVec2(MarkerSize, MarkerSize));
		const ImVec2 Minimum = ImGui::GetItemRectMin();
		const ImVec2 Maximum = ImGui::GetItemRectMax();
		const ImVec2 Center((Minimum.x + Maximum.x) * 0.5f, (Minimum.y + Maximum.y) * 0.5f);
		const auto Geometry = GraphCanvasDetail::MakePinMarkerGeometry(
			GraphCanvasDetail::MakeEditorGraphCanvasPoint(Center.x, Center.y), MarkerSize, MarkerSize);
		auto* DrawList = ImGui::GetWindowDrawList();
		const bool IsExecution = Pin.Type == EditorGraphPinType::Execution;
		const ImU32 Color = ImGui::GetColorU32(IsExecution ? ImGuiCol_PlotHistogram : ImGuiCol_Text);
		const float Radius = MarkerSize * 0.32f;
		if (IsExecution)
		{
			const ImVec2 Top(Center.x, Center.y - Radius);
			const ImVec2 Right(Center.x + Radius, Center.y);
			const ImVec2 Bottom(Center.x, Center.y + Radius);
			const ImVec2 Left(Center.x - Radius, Center.y);
			if (Connected)
				DrawList->AddQuadFilled(Top, Right, Bottom, Left, Color);
			else DrawList->AddQuad(Top, Right, Bottom, Left, Color, 2.0f);
		}
		else if (Connected)
			DrawList->AddCircleFilled(Center, Radius, Color, 16);
		else DrawList->AddCircle(Center, Radius, Color, 16, 2.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%.*s", static_cast<int>(Tooltip.size()), Tooltip.data());

		GraphCanvasDetail::ApplyPinMarkerGeometry(Geometry,
			[](EditorGraphCanvasPoint Minimum, EditorGraphCanvasPoint Maximum)
			{
				NodeEditor::PinRect(ImVec2(static_cast<float>(Minimum.X), static_cast<float>(Minimum.Y)),
					ImVec2(static_cast<float>(Maximum.X), static_cast<float>(Maximum.Y)));
			}, [](EditorGraphCanvasPoint Anchor)
			{
				const ImVec2 Position(static_cast<float>(Anchor.X), static_cast<float>(Anchor.Y));
				NodeEditor::PinPivotRect(Position, Position);
			});
		return Geometry;
	}

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
		State.ScreenPinRelease = {};
		State.PinRelease = {};
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

	const EditorGraphPinDefinition* FindPinDefinition(const EditorGraphSchema& Schema,
		std::string_view NodeType, std::string_view PinKey)
	{
		const auto* Node = FindNodeDefinition(Schema, NodeType);
		if (!Node) return nullptr;
		const auto It = std::find_if(Node->Pins.begin(), Node->Pins.end(), [PinKey](const auto& Pin)
			{ return Pin.Key == PinKey; });
		return It == Node->Pins.end() ? nullptr : &*It;
	}

	std::string ReadableNodeLabel(const EditorGraphSchema& Schema, std::string_view Key)
	{
		const auto* Definition = FindNodeDefinition(Schema, Key);
		return Definition ? Definition->Label : std::string(Key);
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
		EditorDocumentEditorRegistry& Editors, std::function<void()> PresentationChanged)
		: m_Documents(Documents), m_Editors(Editors), m_PresentationChanged(std::move(PresentationChanged))
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

	std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> EditorGraphDocumentEditor::CapturePinVisibility(
		const EditorDocumentSession& Session) const
	{
		const auto* State = dynamic_cast<const GraphDocumentState*>(Session.EditorState.get());
		if (!State) return {};
		std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> Settings;
		for (const auto& Node : State->History.Graph().Nodes)
			for (const auto& Pin : Node.Pins)
				if (State->HiddenPins.contains(Pin.ID))
					Settings.push_back({ Session.Identity, State->History.Graph().ID, Node.ID, Pin.ID });
		std::sort(Settings.begin(), Settings.end(), [](const auto& Left, const auto& Right)
		{
			if (Left.NodeID != Right.NodeID) return Left.NodeID.ToString() < Right.NodeID.ToString();
			return Left.PinID.ToString() < Right.PinID.ToString();
		});
		return Settings;
	}

	std::vector<EditorDocumentWorkspaceState::GraphPinVisibility>
		EditorGraphDocumentEditor::CaptureWorkspacePinVisibility() const
	{
		auto Settings = m_PinVisibilitySettings;
		std::sort(Settings.begin(), Settings.end(), [](const auto& Left, const auto& Right)
		{
			if (Left.Document.Asset != Right.Document.Asset)
				return Left.Document.Asset.ToString() < Right.Document.Asset.ToString();
			if (Left.NodeID != Right.NodeID) return Left.NodeID.ToString() < Right.NodeID.ToString();
			return Left.PinID.ToString() < Right.PinID.ToString();
		});
		return Settings;
	}

	void EditorGraphDocumentEditor::SetWorkspacePinVisibility(
		std::vector<EditorDocumentWorkspaceState::GraphPinVisibility> Settings)
	{
		m_PinVisibilitySettings = std::move(Settings);
	}

	void EditorGraphDocumentEditor::RestorePinVisibility(EditorDocumentSession& Session,
		const std::vector<EditorDocumentWorkspaceState::GraphPinVisibility>& Settings)
	{
		auto* State = GetState(Session);
		if (!State) return;
		State->HiddenPins.clear();
		const auto& Graph = State->History.Graph();
		for (const auto& Setting : Settings)
		{
			if (Setting.Document != Session.Identity || Setting.GraphID != Graph.ID) continue;
			const auto Node = std::find_if(Graph.Nodes.begin(), Graph.Nodes.end(), [&Setting](const auto& Value)
				{ return Value.ID == Setting.NodeID; });
			if (Node == Graph.Nodes.end()) continue;
			const auto Pin = std::find_if(Node->Pins.begin(), Node->Pins.end(), [&Setting](const auto& Value)
				{ return Value.ID == Setting.PinID; });
			if (Pin == Node->Pins.end()) continue;
			const auto* Definition = FindPinDefinition(State->Schema, Node->TypeKey, Pin->Key);
			if (Definition && Definition->Hideable && !Definition->Required) State->HiddenPins.insert(Pin->ID);
		}
		std::erase_if(m_PinVisibilitySettings, [&Session, &Graph](const auto& Setting)
			{ return Setting.Document == Session.Identity && Setting.GraphID == Graph.ID; });
		const auto Current = CapturePinVisibility(Session);
		m_PinVisibilitySettings.insert(m_PinVisibilitySettings.end(), Current.begin(), Current.end());
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

	void EditorGraphDocumentEditor::SetGraphForTesting(EditorDocumentSession& Session,
		EditorGraph Graph, EditorGraphSchema Schema)
	{
		auto* State = GetState(Session);
		if (!State || Graph.ID != Session.Identity.Asset) return;
		State->History.Reset(std::move(Graph));
		State->Schema = std::move(Schema);
		State->Handles.clear();
		State->Identities.clear();
		State->NodeSearch = {};
		State->ScreenPinRelease = {};
		State->PinRelease = {};
		State->NeedsCanvasSync = true;
	}

	void EditorGraphDocumentEditor::CapturePresentationForTesting(EditorDocumentSession& Session, bool Capture)
	{
		if (auto* State = GetState(Session))
		{
			State->CapturePresentation = Capture;
			State->Presentation.clear();
		}
	}

	void EditorGraphDocumentEditor::SetCanvasSettingsForTesting(EditorDocumentSession& Session, std::string Settings)
	{
		if (auto* State = GetState(Session))
		{
			State->CanvasSettings = std::move(Settings);
			CancelCanvasInteraction(*State);
		}
	}

	std::vector<EditorGraphCanvasNodePresentation> EditorGraphDocumentEditor::GetPresentationForTesting(
		EditorDocumentSession& Session)
	{
		const auto* State = GetState(Session);
		return State ? State->Presentation : std::vector<EditorGraphCanvasNodePresentation>{};
	}

	void EditorGraphDocumentEditor::SetPinHiddenForTesting(EditorDocumentSession& Session,
		PulseForge::UUID PinID, bool Hidden)
	{
		if (auto* State = GetState(Session))
		{
			for (const auto& Node : State->History.Graph().Nodes)
				for (const auto& Pin : Node.Pins)
					if (Pin.ID == PinID)
					{
						if (Hidden) State->HiddenPins.insert(PinID);
						else State->HiddenPins.erase(PinID);
						const auto Setting = std::find_if(m_PinVisibilitySettings.begin(), m_PinVisibilitySettings.end(),
							[&Session, &State, PinID](const auto& Candidate)
								{ return Candidate.Document == Session.Identity && Candidate.GraphID == State->History.Graph().ID &&
									Candidate.PinID == PinID; });
						if (Hidden && Setting == m_PinVisibilitySettings.end())
							m_PinVisibilitySettings.push_back({ Session.Identity, State->History.Graph().ID, Node.ID, Pin.ID });
						else if (!Hidden && Setting != m_PinVisibilitySettings.end()) m_PinVisibilitySettings.erase(Setting);
						return;
					}
		}
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
		if (State->CapturePresentation) State->Presentation.clear();
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
			State->ScreenPinRelease = {};
			State->PinRelease = {};
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
		const ImVec2 ScreenMousePositionAtFrameStart = Input.MousePos;
		const bool OtherInputOwner = ImGui::IsAnyItemActive();
		const auto CanvasContext = MakeCanvasDocumentContext(Session, State->History.Graph().ID);
		const auto ContextMatches = [&CanvasContext](const EditorGraphCanvasDocumentContext& Captured)
		{
			return Captured.Document == CanvasContext.Document &&
				Captured.SessionGeneration == CanvasContext.SessionGeneration &&
				Captured.HostID == CanvasContext.HostID && Captured.GraphID == CanvasContext.GraphID;
		};
		if (!Focused || (State->PinRelease.Valid && !ContextMatches(State->PinRelease.Context)) ||
			(State->ScreenPinRelease.Valid && !ContextMatches(State->ScreenPinRelease.Context)))
		{
			State->ScreenPinRelease = {};
			State->PinRelease = {};
		}
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false))
		{
			State->ScreenPinRelease = {};
			State->PinRelease = {};
		}
		if (Focused && !CancelGesture && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			State->ScreenPinRelease = { true, CanvasContext,
				{ ScreenMousePositionAtFrameStart.x, ScreenMousePositionAtFrameStart.y } };
		}
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
		std::vector<PinAnchorMeasurement> PinAnchorCanvasPositions;
		struct PinVisibilityChange { PulseForge::UUID PinID; bool Hidden = false; };
		std::vector<PinVisibilityChange> PendingVisibilityChanges;
		bool PropertyWidgetHovered = false;
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> IncidentConnections;
		for (const auto& Connection : State->History.Graph().Connections)
		{
			++IncidentConnections[Connection.OutputPin];
			++IncidentConnections[Connection.InputPin];
		}
		const auto ConnectionCount = [&IncidentConnections](PulseForge::UUID PinID)
		{
			const auto Found = IncidentConnections.find(PinID);
			return Found == IncidentConnections.end() ? size_t{} : Found->second;
		};
		if (const auto ActivePin = State->PropertyEdits.ActivePin(); ActivePin && State->PropertyEdits.HasActiveEdit())
		{
			for (const auto& Node : State->History.Graph().Nodes)
			{
				const auto Pin = std::find_if(Node.Pins.begin(), Node.Pins.end(), [ActivePin](const auto& Candidate)
					{ return Candidate.ID == *ActivePin; });
				if (Pin == Node.Pins.end()) continue;
				const auto* Definition = FindPinDefinition(State->Schema, Node.TypeKey, Pin->Key);
				if (Definition && (!ShouldShowEditorGraphPinDefault(*Pin, *Definition, ConnectionCount(Pin->ID)) ||
					(State->HiddenPins.contains(Pin->ID) && ConnectionCount(Pin->ID) == 0)))
				{
					(void)FinishPropertyEdit(*State, m_Documents, Session);
				}
				break;
			}
		}
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

		const ImVec2 CanvasScreenOrigin = ImGui::GetCursorScreenPos();
		const ImVec2 CanvasScreenSize = ImGui::GetContentRegionAvail();
		NodeEditor::Begin("##PulseForgeGraphCanvas", ImVec2(0.0f, 0.0f));
		if (State->ScreenPinRelease.Valid)
		{
			const auto& ScreenRelease = State->ScreenPinRelease.Position;
			const bool InsideCanvas = ScreenRelease.X >= CanvasScreenOrigin.x &&
				ScreenRelease.X <= CanvasScreenOrigin.x + CanvasScreenSize.x &&
				ScreenRelease.Y >= CanvasScreenOrigin.y &&
				ScreenRelease.Y <= CanvasScreenOrigin.y + CanvasScreenSize.y;
			if (InsideCanvas)
			{
				const auto CanvasRelease = GraphCanvasDetail::ConvertEditorGraphScreenPoint(ScreenRelease,
					[](EditorGraphScreenPoint Screen)
					{
						const ImVec2 Position = NodeEditor::ScreenToCanvas(
							ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y)));
						return EditorGraphCanvasPoint{ Position.x, Position.y };
					});
				if (std::isfinite(CanvasRelease.X) && std::isfinite(CanvasRelease.Y))
					State->PinRelease = { true, State->ScreenPinRelease.Context, {}, CanvasRelease };
				else State->PinRelease = {};
			}
			else State->PinRelease = {};
			State->ScreenPinRelease = {};
		}
		if (RequestSearchFromToolbar)
		{
			const ImVec2 CanvasCenterScreen(CanvasScreenOrigin.x + CanvasScreenSize.x * 0.5f,
				CanvasScreenOrigin.y + CanvasScreenSize.y * 0.5f);
			const auto CanvasCenter = GraphCanvasDetail::ConvertEditorGraphScreenPoint(
				{ CanvasCenterScreen.x, CanvasCenterScreen.y }, [](EditorGraphScreenPoint Screen)
				{
					const ImVec2 Position = NodeEditor::ScreenToCanvas(
						ImVec2(static_cast<float>(Screen.X), static_cast<float>(Screen.Y)));
					return EditorGraphCanvasPoint{ Position.x, Position.y };
				});
			QueueNodeSearch(*State, Session, std::nullopt,
				GraphCanvasDetail::ToEditorGraphPosition(CanvasCenter));
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
			EditorGraphCanvasNodePresentation Presentation;
			Presentation.NodeID = Node.ID;
			const std::string NodeLabel = ReadableNodeLabel(State->Schema, Node.TypeKey);
			const std::string PopupID = "GraphPinVisibility##" + Node.ID.ToString();
			const std::string HeaderDisplay = FitGraphPresentationText(NodeLabel, 420.0f);
			ImGui::TextUnformatted(HeaderDisplay.c_str());
			if (HeaderDisplay != NodeLabel && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", NodeLabel.c_str());
			ImGui::SameLine();
			size_t HiddenCount = 0;
			for (const auto& Pin : Node.Pins)
				if (State->HiddenPins.contains(Pin.ID) && ConnectionCount(Pin.ID) == 0)
					++HiddenCount;
			const std::string VisibilityLabel = HiddenCount ? "Pins +" + std::to_string(HiddenCount) : "Pins";
			if (ImGui::SmallButton((VisibilityLabel + "##" + Node.ID.ToString()).c_str()))
				ImGui::OpenPopup(PopupID.c_str());
			if (ImGui::BeginPopup(PopupID.c_str()))
			{
				if (ImGui::MenuItem("Show All Pins"))
					for (const auto& Pin : Node.Pins)
						PendingVisibilityChanges.push_back({ Pin.ID, false });
				if (ImGui::MenuItem("Hide Unused Pins"))
					for (const auto& Pin : Node.Pins)
						if (const auto* Definition = FindPinDefinition(State->Schema, Node.TypeKey, Pin.Key);
							Definition && CanAutoHideEditorGraphPin(Pin, *Definition, ConnectionCount(Pin.ID)))
							PendingVisibilityChanges.push_back({ Pin.ID, true });
				if (ImGui::MenuItem("Reset Pin Visibility"))
					for (const auto& Pin : Node.Pins)
						PendingVisibilityChanges.push_back({ Pin.ID, false });
				if (ImGui::BeginMenu("Pins"))
				{
					for (const auto& Pin : Node.Pins)
					{
						const auto* Definition = FindPinDefinition(State->Schema, Node.TypeKey, Pin.Key);
						if (!Definition) continue;
						const bool Connected = ConnectionCount(Pin.ID) != 0;
						const bool Visible = Connected || Definition->Required || !Definition->Hideable ||
							!State->HiddenPins.contains(Pin.ID);
						const bool CanToggle = CanHideEditorGraphPin(*Definition) && !Connected;
						const std::string StateLabel = Connected ? " — connected; always visible" :
							Definition->Required ? " — required" : Definition->Hideable ? "" : " — fixed visible";
						const std::string Label = (Definition->Label.empty() ? Pin.Key : Definition->Label) +
							"  [" + PinTypeLabel(Pin.Type) + "]" + StateLabel + "##" + Pin.ID.ToString();
						if (ImGui::MenuItem(Label.c_str(), nullptr, Visible, CanToggle))
							PendingVisibilityChanges.push_back({ Pin.ID, Visible });
					}
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
			const ImVec2 PinRowsOrigin = ImGui::GetCursorPos();
			struct PinRow final
			{
				const EditorGraphPin* Pin = nullptr;
				const EditorGraphPinDefinition* Definition = nullptr;
				std::string Label;
				float LabelWidth = 0.0f;
				float ControlWidth = 0.0f;
				bool Connected = false;
				bool ShowDefault = false;
			};
			std::vector<PinRow> Inputs;
			std::vector<PinRow> Outputs;
			float InputContentWidth = 0.0f;
			float OutputContentWidth = 0.0f;
			constexpr float MarkerSize = 14.0f;
			constexpr float MarkerGap = 8.0f;
			constexpr float SidePadding = 12.0f;
			constexpr float CenterGap = 18.0f;
			constexpr float MaxInputLabelWidth = 190.0f;
			constexpr float MaxOutputLabelWidth = 260.0f;
			const auto MeasureLabel = [](std::string_view Text, float Limit)
				{ return std::min(ImGui::CalcTextSize(Text.data(), Text.data() + Text.size()).x, Limit); };
			for (const auto& Pin : Node.Pins)
			{
				const auto* Definition = FindPinDefinition(State->Schema, Node.TypeKey, Pin.Key);
				if (!Definition) continue;
				const size_t Incidents = ConnectionCount(Pin.ID);
				const bool Connected = Incidents != 0;
				const bool Visible = Connected || Definition->Required || !Definition->Hideable ||
					!State->HiddenPins.contains(Pin.ID);
				if (!Visible) continue;
				PinRow Row;
				Row.Pin = &Pin;
				Row.Definition = Definition;
				Row.Connected = Connected;
				Row.ShowDefault = Pin.Direction == EditorGraphPinDirection::Input && Pin.Type != EditorGraphPinType::Execution &&
					ShouldShowEditorGraphPinDefault(Pin, *Definition, Incidents);
				const std::string_view Label = Definition->Label.empty() ? std::string_view(Pin.Key) :
					std::string_view(Definition->Label);
				Row.Label = std::string(Label) + "  [" + PinTypeLabel(Pin.Type) + "]";
				Row.LabelWidth = MeasureLabel(Row.Label, Pin.Direction == EditorGraphPinDirection::Input
					? MaxInputLabelWidth : MaxOutputLabelWidth);
				if (Row.ShowDefault)
				{
					switch (Pin.Type)
					{
					case EditorGraphPinType::Float: Row.ControlWidth = 128.0f; break;
					case EditorGraphPinType::Vector3: Row.ControlWidth = 204.0f; break;
				case EditorGraphPinType::Boolean: Row.ControlWidth = ImGui::GetFrameHeight(); break;
				case EditorGraphPinType::String: Row.ControlWidth = 168.0f; break;
				case EditorGraphPinType::Execution: break;
				}
			}
			if (Pin.Direction == EditorGraphPinDirection::Input)
			{
				InputContentWidth = std::max(InputContentWidth, MarkerSize + MarkerGap + Row.LabelWidth +
					(Row.ShowDefault ? MarkerGap + Row.ControlWidth : 0.0f));
				Inputs.push_back(std::move(Row));
			}
			else
			{
				OutputContentWidth = std::max(OutputContentWidth, Row.LabelWidth + MarkerGap + MarkerSize);
				Outputs.push_back(std::move(Row));
			}
			}
			const float TitleWidth = MeasureLabel(NodeLabel, 420.0f) + (HiddenCount ? 84.0f : 44.0f);
			const float InputColumnWidth = Inputs.empty() ? 0.0f : InputContentWidth;
			const float OutputColumnWidth = Outputs.empty() ? 0.0f : OutputContentWidth;
			const float ColumnsWidth = InputColumnWidth + OutputColumnWidth +
				(!Inputs.empty() && !Outputs.empty() ? CenterGap : 0.0f);
			const float NodeContentWidth = (std::max)({ 176.0f, TitleWidth + SidePadding * 2.0f,
				ColumnsWidth + SidePadding * 2.0f });
			Presentation.InputColumnWidth = InputColumnWidth;
			Presentation.OutputColumnWidth = OutputColumnWidth;
			const float RowHeight = (std::max)(ImGui::GetFrameHeight(), MarkerSize + 6.0f);
			const float RowSpacing = ImGui::GetStyle().ItemSpacing.y;
			const float RowsHeight = static_cast<float>(std::max(Inputs.size(), Outputs.size())) * (RowHeight + RowSpacing);
			const float LeftMarkerX = PinRowsOrigin.x + SidePadding;
			const float InputLabelX = LeftMarkerX + MarkerSize + MarkerGap;
			const float RightMarkerX = PinRowsOrigin.x + NodeContentWidth - SidePadding - MarkerSize;
			const auto DrawLabel = [](const std::string& Label, float X, float Y, float Width)
			{
				const std::string Display = FitGraphPresentationText(Label, Width);
				ImGui::SetCursorPos(ImVec2(X, Y));
				const ImVec2 Minimum = ImGui::GetCursorScreenPos();
				ImGui::PushClipRect(Minimum, ImVec2(Minimum.x + Width, Minimum.y + ImGui::GetFrameHeight()), true);
				ImGui::TextUnformatted(Display.c_str());
				ImGui::PopClipRect();
				if (ImGui::IsItemHovered() && Display != Label)
					ImGui::SetTooltip("%s", Label.c_str());
				return EditorGraphCanvasRect{
					{ Minimum.x, Minimum.y }, { Minimum.x + Width, Minimum.y + ImGui::GetFrameHeight() } };
			};
			const auto DrawRow = [&](const PinRow& Row, size_t Index)
			{
				const auto& Pin = *Row.Pin;
				const bool IsInput = Pin.Direction == EditorGraphPinDirection::Input;
				const float Y = PinRowsOrigin.y + static_cast<float>(Index) * (RowHeight + RowSpacing);
				const float MarkerX = IsInput ? LeftMarkerX : RightMarkerX;
				const uintptr_t PinHandle = RegisterHandle(Pin.ID);
				if (!PinHandle) return;
				const std::string Direction = IsInput ? "Input" : "Output";
				const std::string Tooltip = (Row.Definition->Label.empty() ? Pin.Key : Row.Definition->Label) +
					" (" + Pin.Key + ") · " + Direction + " · " + PinTypeLabel(Pin.Type) + " · " +
					std::to_string(ConnectionCount(Pin.ID)) + " connection(s)";
				ImGui::SetCursorPos(ImVec2(MarkerX, Y + (RowHeight - ImGui::GetFrameHeight()) * 0.5f));
				NodeEditor::BeginPin(NodeEditor::PinId(PinHandle), IsInput
					? NodeEditor::PinKind::Input : NodeEditor::PinKind::Output);
				const auto Marker = DrawGraphPinMarker(Pin, Row.Connected, Tooltip);
				NodeEditor::EndPin();
				if (std::isfinite(Marker.Anchor.X) && std::isfinite(Marker.Anchor.Y))
					PinAnchorCanvasPositions.push_back({ Node.ID, Pin.ID, Marker.Anchor });
				EditorGraphCanvasPinPresentation PinPresentation;
				PinPresentation.PinID = Pin.ID;
				PinPresentation.Direction = Pin.Direction;
				PinPresentation.Connected = Row.Connected;
				PinPresentation.Marker = Marker;
				if (IsInput)
				{
					const float AvailableLabelWidth = Row.ShowDefault
						? (std::max)(Row.LabelWidth, InputContentWidth - MarkerSize - MarkerGap - MarkerGap - Row.ControlWidth)
						: Row.LabelWidth;
					PinPresentation.Label = DrawLabel(Row.Label, InputLabelX,
						Y + (RowHeight - ImGui::GetFrameHeight()) * 0.5f, AvailableLabelWidth);
					if (Row.ShowDefault)
					{
						const float ControlX = InputLabelX + AvailableLabelWidth + MarkerGap;
						ImGui::SetCursorPos(ImVec2(ControlX, Y));
						ImGui::SetNextItemWidth(Row.ControlWidth);
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
						if (State->CapturePresentation)
						{
							const ImVec2 Minimum = ImGui::GetItemRectMin();
							const ImVec2 Maximum = ImGui::GetItemRectMax();
							PinPresentation.PropertyControl = EditorGraphCanvasRect{
								{ Minimum.x, Minimum.y }, { Maximum.x, Maximum.y } };
						}
						PropertyEvents.push_back({ Pin.ID, std::move(Value), Activated, Changed, Deactivated });
					}
				}
				else
				{
					const float LabelX = RightMarkerX - MarkerGap - Row.LabelWidth;
					PinPresentation.Label = DrawLabel(Row.Label, LabelX,
						Y + (RowHeight - ImGui::GetFrameHeight()) * 0.5f, Row.LabelWidth);
				}
				if (State->CapturePresentation) Presentation.Pins.push_back(std::move(PinPresentation));
			};
			for (size_t Index = 0; Index < Inputs.size(); ++Index) DrawRow(Inputs[Index], Index);
			for (size_t Index = 0; Index < Outputs.size(); ++Index)
				DrawRow(Outputs[Index], Index);
			ImGui::SetCursorPos(PinRowsOrigin);
			ImGui::Dummy(ImVec2(NodeContentWidth, RowsHeight));
			NodeEditor::EndNode();
			if (State->CapturePresentation)
			{
				const ImVec2 Position = NodeEditor::GetNodePosition(CanvasNode);
				const ImVec2 ScreenSize = NodeEditor::GetNodeSize(CanvasNode);
				const float Zoom = NodeEditor::GetCurrentZoom();
				Presentation.Position = { Position.x, Position.y };
				Presentation.Size = { ScreenSize.x, ScreenSize.y };
				Presentation.Zoom = Zoom;
				State->Presentation.push_back(std::move(Presentation));
			}
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
		if (CreateRequest.SawNodeRequest && State->PinRelease.Valid && State->PinRelease.SourcePin.IsNil())
		{
			if (const auto Identity = State->Identities.find(CreateRequest.SourcePin); Identity != State->Identities.end())
				State->PinRelease.SourcePin = Identity->second;
			else
			{
				State->Error = "The source pin for node creation is no longer available";
				State->ScreenPinRelease = {};
				State->PinRelease = {};
			}
		}
		if (CreateRequest.SawLinkRequest)
		{
			State->ScreenPinRelease = {};
			State->PinRelease = {};
		}
		if (CreateRequest.SawNodeRequest && !CreateRequest.AcceptedNode && CreateRequest.SourcePin != 0 &&
			!State->Identities.contains(CreateRequest.SourcePin))
		{
			State->Error = "The source pin for node creation is no longer available";
			State->ScreenPinRelease = {};
			State->PinRelease = {};
		}
		std::optional<PulseForge::UUID> AcceptedNodeSource;
		std::optional<EditorGraphVector3> AcceptedNodePlacement;
		if (CreateRequest.AcceptedNode)
		{
			if (const auto Identity = State->Identities.find(CreateRequest.SourcePin); Identity != State->Identities.end())
			{
				const auto CapturedPlacement = GraphCanvasDetail::ConsumePinRelease(
					State->PinRelease, CanvasContext, Identity->second);
				if (CapturedPlacement && std::isfinite(CapturedPlacement->Position.X) &&
					std::isfinite(CapturedPlacement->Position.Y))
				{
					AcceptedNodeSource = Identity->second;
					AcceptedNodePlacement = GraphCanvasDetail::ToEditorGraphPosition(CapturedPlacement->Position);
				}
				else State->Error = "The pin-drop release point is unavailable for this graph session";
			}
			else State->Error = "The source pin for node creation is no longer available";
			State->ScreenPinRelease = {};
			State->PinRelease = {};
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
			const double CanvasX = std::floor(Node.X);
			const double CanvasY = std::floor(Node.Y);
			if (std::isfinite(Position.x) && std::isfinite(Position.y) &&
				(std::abs(static_cast<double>(Position.x) - CanvasX) > 0.01 ||
					std::abs(static_cast<double>(Position.y) - CanvasY) > 0.01))
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
			// Resume() has re-entered canvas-local space, including for MouseClickedPos.
			const ImVec2 LocalPlacement = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Right];
			const auto Placement = GraphCanvasDetail::MakeEditorGraphCanvasPoint(LocalPlacement.x, LocalPlacement.y);
			if (std::isfinite(Placement.X) && std::isfinite(Placement.Y))
			{
				QueueNodeSearch(*State, Session, std::nullopt,
					GraphCanvasDetail::ToEditorGraphPosition(Placement));
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
			for (const auto& Measurement : PinAnchorCanvasPositions)
				if (const auto It = NodeCanvasY.find(Measurement.NodeID); It != NodeCanvasY.end())
					Anchors.push_back({ Measurement.NodeID, Measurement.PinID,
						Measurement.Position.Y - It->second });
			const auto Positions = CalculateEditorGraphStraightenedLayout(LayoutNodes, Anchors,
				State->History.Graph().Connections, SelectedNodeIDs);
			if (!Positions.empty())
			{
				auto CanvasPositions = Positions;
				for (auto& [NodeID, Position] : CanvasPositions)
				{
					(void)NodeID;
					Position.X = std::floor(Position.X);
					Position.Y = std::floor(Position.Y);
				}
				const EditorGraph BeforeLayout = State->History.Graph();
				auto Moved = MoveEditorGraphNodes(State->History, State->Schema, CanvasPositions);
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
		bool PresentationChanged = false;
		for (const auto& Change : PendingVisibilityChanges)
		{
			const auto Node = std::find_if(State->History.Graph().Nodes.begin(), State->History.Graph().Nodes.end(),
				[&Change](const auto& Candidate)
				{
					return std::any_of(Candidate.Pins.begin(), Candidate.Pins.end(), [&Change](const auto& Pin)
						{ return Pin.ID == Change.PinID; });
				});
			if (Node == State->History.Graph().Nodes.end()) continue;
			const auto Pin = std::find_if(Node->Pins.begin(), Node->Pins.end(), [&Change](const auto& Candidate)
				{ return Candidate.ID == Change.PinID; });
			if (Pin == Node->Pins.end()) continue;
			const auto* Definition = FindPinDefinition(State->Schema, Node->TypeKey, Pin->Key);
			if (!Definition || !CanHideEditorGraphPin(*Definition)) continue;
			if (Change.Hidden) PresentationChanged |= State->HiddenPins.insert(Pin->ID).second;
			else PresentationChanged |= State->HiddenPins.erase(Pin->ID) != 0;
			const auto ExistingSetting = std::find_if(m_PinVisibilitySettings.begin(), m_PinVisibilitySettings.end(),
				[&Session, &State, &Change](const auto& Setting)
					{ return Setting.Document == Session.Identity && Setting.GraphID == State->History.Graph().ID &&
						Setting.PinID == Change.PinID; });
			if (Change.Hidden && ExistingSetting == m_PinVisibilitySettings.end())
				m_PinVisibilitySettings.push_back({ Session.Identity, State->History.Graph().ID, Node->ID, Pin->ID });
			else if (!Change.Hidden && ExistingSetting != m_PinVisibilitySettings.end())
				m_PinVisibilitySettings.erase(ExistingSetting);
		}
		if (PresentationChanged && m_PresentationChanged) m_PresentationChanged();
	}
}
