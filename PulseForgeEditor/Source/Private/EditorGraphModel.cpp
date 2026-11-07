#include "Editor/EditorGraphModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace
{
	using namespace PulseForgeEditor;
	using Json = nlohmann::ordered_json;
	constexpr size_t MaximumSourceBytes = 2 * 1024 * 1024;
	constexpr size_t MaximumClipboardBytes = 512 * 1024;
	constexpr size_t MaximumNodes = 2048;
	constexpr size_t MaximumPinsPerNode = 64;
	constexpr size_t MaximumConnections = 8192;
	constexpr double MaximumCoordinate = 1'000'000.0;

	std::string IdText(const PulseForge::UUID& ID)
	{
		return ID.ToString();
	}

	std::expected<PulseForge::UUID, std::string> ReadID(const Json& Value, std::string_view Field)
	{
		if (!Value.is_string()) return std::unexpected(std::string(Field) + " must be a UUID string");
		const auto Parsed = PulseForge::UUID::Parse(Value.get<std::string>());
		if (!Parsed || Parsed->IsNil()) return std::unexpected(std::string(Field) + " must be a non-nil UUID");
		return *Parsed;
	}

	std::expected<uint32_t, std::string> ReadVersion(const Json& Value, std::string_view Field)
	{
		uint64_t FullWidth = 0;
		if (Value.is_number_unsigned()) FullWidth = Value.get<uint64_t>();
		else if (Value.is_number_integer())
		{
			const int64_t Signed = Value.get<int64_t>();
			if (Signed < 0) return std::unexpected(std::string(Field) + " cannot be negative");
			FullWidth = static_cast<uint64_t>(Signed);
		}
		else return std::unexpected(std::string(Field) + " must be an integer");
		if (FullWidth > std::numeric_limits<uint32_t>::max())
			return std::unexpected(std::string(Field) + " is outside the supported integer range");
		return static_cast<uint32_t>(FullWidth);
	}

	const EditorGraphNodeDefinition* FindDefinition(const EditorGraphSchema& Schema, std::string_view Key)
	{
		const auto It = std::find_if(Schema.Nodes.begin(), Schema.Nodes.end(), [Key](const auto& Item)
			{ return Item.TypeKey == Key; });
		return It == Schema.Nodes.end() ? nullptr : &*It;
	}

	const EditorGraphPinDefinition* FindPinDefinition(const EditorGraphNodeDefinition& Definition, std::string_view Key)
	{
		const auto It = std::find_if(Definition.Pins.begin(), Definition.Pins.end(), [Key](const auto& Item)
			{ return Item.Key == Key; });
		return It == Definition.Pins.end() ? nullptr : &*It;
	}

	bool IsFiniteCoordinate(double Value)
	{
		return std::isfinite(Value) && std::abs(Value) <= MaximumCoordinate;
	}

	bool IsValueCompatible(EditorGraphPinType Type, const EditorGraphValue& Value)
	{
		switch (Type)
		{
		case EditorGraphPinType::Float:
			return std::holds_alternative<double>(Value) && std::isfinite(std::get<double>(Value));
		case EditorGraphPinType::Vector3:
			if (!std::holds_alternative<EditorGraphVector3>(Value)) return false;
			{
				const auto& Vector = std::get<EditorGraphVector3>(Value);
				return std::isfinite(Vector.X) && std::isfinite(Vector.Y) && std::isfinite(Vector.Z);
			}
		case EditorGraphPinType::Boolean: return std::holds_alternative<bool>(Value);
		case EditorGraphPinType::String:
			return std::holds_alternative<std::string>(Value) && std::get<std::string>(Value).size() <= 4096;
		case EditorGraphPinType::Execution: return std::holds_alternative<std::monostate>(Value);
		}
		return false;
	}

	Json WriteValue(const EditorGraphValue& Value)
	{
		return std::visit([](const auto& Item) -> Json
		{
			using T = std::decay_t<decltype(Item)>;
			if constexpr (std::is_same_v<T, std::monostate>) return Json{ { "kind", "none" }, { "value", nullptr } };
			else if constexpr (std::is_same_v<T, bool>) return Json{ { "kind", "bool" }, { "value", Item } };
			else if constexpr (std::is_same_v<T, double>) return Json{ { "kind", "float" }, { "value", Item } };
			else if constexpr (std::is_same_v<T, EditorGraphVector3>)
				return Json{ { "kind", "vec3" }, { "value", Json::array({ Item.X, Item.Y, Item.Z }) } };
			else return Json{ { "kind", "string" }, { "value", Item } };
		}, Value);
	}

	std::expected<EditorGraphValue, std::string> ReadValue(const Json& Value)
	{
		if (!Value.is_object() || !Value.contains("kind") || !Value["kind"].is_string() || !Value.contains("value"))
			return std::unexpected("A graph pin default value is malformed");
		const std::string Kind = Value["kind"].get<std::string>();
		const Json& Data = Value["value"];
		if (Kind == "none" && Data.is_null()) return EditorGraphValue{};
		if (Kind == "bool" && Data.is_boolean()) return EditorGraphValue{ Data.get<bool>() };
		if (Kind == "float" && Data.is_number())
		{
			const double Number = Data.get<double>();
			if (std::isfinite(Number)) return EditorGraphValue{ Number };
		}
		if (Kind == "vec3" && Data.is_array() && Data.size() == 3 &&
			std::all_of(Data.begin(), Data.end(), [](const Json& Component) { return Component.is_number() && std::isfinite(Component.get<double>()); }))
			return EditorGraphValue{ EditorGraphVector3{ Data[0].get<double>(), Data[1].get<double>(), Data[2].get<double>() } };
		if (Kind == "string" && Data.is_string()) return EditorGraphValue{ Data.get<std::string>() };
		return std::unexpected("A graph pin default value has an unsupported or invalid type");
	}

	const char* DirectionName(EditorGraphPinDirection Value)
	{
		return Value == EditorGraphPinDirection::Input ? "input" : "output";
	}

	const char* TypeName(EditorGraphPinType Value)
	{
		switch (Value)
		{
		case EditorGraphPinType::Float: return "float";
		case EditorGraphPinType::Vector3: return "vec3";
		case EditorGraphPinType::Boolean: return "bool";
		case EditorGraphPinType::String: return "string";
		case EditorGraphPinType::Execution: return "execution";
		}
		return "invalid";
	}

	std::expected<EditorGraphPinDirection, std::string> ReadDirection(const Json& Value)
	{
		if (!Value.is_string()) return std::unexpected("Graph pin direction must be a string");
		const std::string Name = Value.get<std::string>();
		if (Name == "input") return EditorGraphPinDirection::Input;
		if (Name == "output") return EditorGraphPinDirection::Output;
		return std::unexpected("Graph pin direction is unsupported");
	}

	std::expected<EditorGraphPinType, std::string> ReadType(const Json& Value)
	{
		if (!Value.is_string()) return std::unexpected("Graph pin type must be a string");
		const std::string Name = Value.get<std::string>();
		if (Name == "float") return EditorGraphPinType::Float;
		if (Name == "vec3") return EditorGraphPinType::Vector3;
		if (Name == "bool") return EditorGraphPinType::Boolean;
		if (Name == "string") return EditorGraphPinType::String;
		if (Name == "execution") return EditorGraphPinType::Execution;
		return std::unexpected("Graph pin type is unknown; the graph was not loaded");
	}

	Json SerializeNode(const EditorGraphNode& Node)
	{
		std::vector<const EditorGraphPin*> Pins;
		for (const auto& Pin : Node.Pins) Pins.push_back(&Pin);
		std::sort(Pins.begin(), Pins.end(), [](const auto* A, const auto* B)
			{ return std::tie(A->Key, A->ID) < std::tie(B->Key, B->ID); });
		Json PinRecords = Json::array();
		for (const EditorGraphPin* Pin : Pins)
			PinRecords.push_back(Json{ { "id", IdText(Pin->ID) }, { "key", Pin->Key }, { "direction", DirectionName(Pin->Direction) },
				{ "type", TypeName(Pin->Type) }, { "cardinality", Pin->Cardinality == EditorGraphPinCardinality::Single ? "single" : "multiple" },
				{ "default", WriteValue(Pin->DefaultValue) } });
		return Json{ { "id", IdText(Node.ID) }, { "type", Node.TypeKey }, { "position", Json::array({ Node.X, Node.Y }) },
			{ "pins", std::move(PinRecords) } };
	}

	std::expected<EditorGraphNode, std::string> ParseNode(const Json& Record, const EditorGraphSchema& Schema)
	{
		if (!Record.is_object() || !Record.contains("id") || !Record.contains("type") || !Record["type"].is_string() ||
			!Record.contains("position") || !Record["position"].is_array() || Record["position"].size() != 2 ||
			!Record["position"][0].is_number() || !Record["position"][1].is_number() || !Record.contains("pins") || !Record["pins"].is_array())
			return std::unexpected("A graph node record is malformed");
		const std::string TypeKey = Record["type"].get<std::string>();
		const auto* Definition = FindDefinition(Schema, TypeKey);
		if (!Definition) return std::unexpected("Unknown graph node type '" + TypeKey + "'; the graph was not loaded");
		if (Record["pins"].size() > MaximumPinsPerNode)
			return std::unexpected("A graph node has too many pins");
		const auto ID = ReadID(Record["id"], "node id");
		if (!ID) return std::unexpected(ID.error());
		const double X = Record["position"][0].get<double>();
		const double Y = Record["position"][1].get<double>();
		if (!IsFiniteCoordinate(X) || !IsFiniteCoordinate(Y)) return std::unexpected("Graph node position is non-finite or outside the supported range");

		EditorGraphNode Node{ *ID, TypeKey, X, Y, {} };
		std::unordered_set<std::string> Keys;
		for (const Json& PinRecord : Record["pins"])
		{
			if (!PinRecord.is_object() || !PinRecord.contains("id") || !PinRecord.contains("key") || !PinRecord["key"].is_string() ||
				!PinRecord.contains("direction") || !PinRecord.contains("type") || !PinRecord.contains("cardinality") ||
				!PinRecord.contains("default"))
				return std::unexpected("A graph pin record is malformed");
			const auto PinID = ReadID(PinRecord["id"], "pin id");
			const std::string Key = PinRecord["key"].get<std::string>();
			const auto Direction = ReadDirection(PinRecord["direction"]);
			const auto Type = ReadType(PinRecord["type"]);
			const auto Default = ReadValue(PinRecord["default"]);
			if (!PinID || !Direction || !Type || !Default) return std::unexpected("A graph pin contains an invalid field");
			if (!PinRecord["cardinality"].is_string()) return std::unexpected("Graph pin cardinality must be a string");
			const std::string CardinalityText = PinRecord["cardinality"].get<std::string>();
			EditorGraphPinCardinality Cardinality;
			if (CardinalityText == "single") Cardinality = EditorGraphPinCardinality::Single;
			else if (CardinalityText == "multiple") Cardinality = EditorGraphPinCardinality::Multiple;
			else return std::unexpected("Graph pin cardinality is unsupported");
			const auto* Expected = FindPinDefinition(*Definition, Key);
			if (!Expected) return std::unexpected("Unknown pin '" + Key + "' on node type '" + TypeKey + "'; the graph was not loaded");
			if (Expected->Direction != *Direction || Expected->Type != *Type || Expected->Cardinality != Cardinality ||
				!IsValueCompatible(*Type, *Default))
				return std::unexpected("Graph pin data does not match its schema definition");
			if (!Keys.insert(Key).second) return std::unexpected("A graph node has duplicate pin keys");
			Node.Pins.push_back(EditorGraphPin{ *PinID, Key, *Direction, *Type, Cardinality, *Default });
		}
		if (Node.Pins.size() != Definition->Pins.size()) return std::unexpected("A graph node is missing a schema pin");
		return Node;
	}

	Json SerializeConnection(const EditorGraphConnection& Connection)
	{
		return Json{ { "id", IdText(Connection.ID) }, { "output", IdText(Connection.OutputPin) }, { "input", IdText(Connection.InputPin) } };
	}

	const EditorGraphNode* FindNodeForPin(const EditorGraph& Graph, PulseForge::UUID PinID, const EditorGraphPin** OutPin = nullptr)
	{
		for (const auto& Node : Graph.Nodes)
			for (const auto& Pin : Node.Pins)
				if (Pin.ID == PinID)
				{
					if (OutPin) *OutPin = &Pin;
					return &Node;
				}
		return nullptr;
	}

	bool HasPath(const EditorGraph& Graph, PulseForge::UUID FromNode, PulseForge::UUID ToNode,
		bool Execution, std::optional<PulseForge::UUID> OmitConnection,
		std::optional<PulseForge::UUID> ReplacedInput)
	{
		std::vector<PulseForge::UUID> Stack{ FromNode };
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Seen;
		while (!Stack.empty())
		{
			const auto Current = Stack.back();
			Stack.pop_back();
			if (Current == ToNode) return true;
			if (!Seen.insert(Current).second) continue;
			for (const auto& Connection : Graph.Connections)
			{
				if (OmitConnection && Connection.ID == *OmitConnection) continue;
				if (ReplacedInput && Connection.InputPin == *ReplacedInput) continue;
				const EditorGraphPin* Output = nullptr;
				const EditorGraphPin* Input = nullptr;
				const auto* Source = FindNodeForPin(Graph, Connection.OutputPin, &Output);
				const auto* Target = FindNodeForPin(Graph, Connection.InputPin, &Input);
				if (Source && Target && Output && Input &&
					(Output->Type == EditorGraphPinType::Execution) == Execution &&
					Input->Type == Output->Type && Source->ID == Current)
					Stack.push_back(Target->ID);
			}
		}
		return false;
	}

	bool HasCycle(const EditorGraph& Graph, bool Execution)
	{
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> InDegree;
		for (const auto& Node : Graph.Nodes) InDegree.emplace(Node.ID, 0);
		for (const auto& Connection : Graph.Connections)
		{
			const EditorGraphPin* Output = nullptr;
			const EditorGraphPin* Input = nullptr;
			const auto* Source = FindNodeForPin(Graph, Connection.OutputPin, &Output);
			const auto* Target = FindNodeForPin(Graph, Connection.InputPin, &Input);
			if (Source && Target && Output && Input && Output->Direction == EditorGraphPinDirection::Output &&
				Input->Direction == EditorGraphPinDirection::Input && Input->Type == Output->Type &&
				(Output->Type == EditorGraphPinType::Execution) == Execution)
				++InDegree[Target->ID];
		}
		std::queue<PulseForge::UUID> Ready;
		for (const auto& [NodeID, Degree] : InDegree)
			if (Degree == 0) Ready.push(NodeID);
		size_t Visited = 0;
		while (!Ready.empty())
		{
			const auto Current = Ready.front();
			Ready.pop();
			++Visited;
			for (const auto& Connection : Graph.Connections)
			{
				const EditorGraphPin* Output = nullptr;
				const EditorGraphPin* Input = nullptr;
				const auto* Source = FindNodeForPin(Graph, Connection.OutputPin, &Output);
				const auto* Target = FindNodeForPin(Graph, Connection.InputPin, &Input);
				if (Source && Target && Output && Input && Source->ID == Current &&
					Output->Direction == EditorGraphPinDirection::Output && Input->Direction == EditorGraphPinDirection::Input &&
					Input->Type == Output->Type && (Output->Type == EditorGraphPinType::Execution) == Execution &&
					--InDegree[Target->ID] == 0)
					Ready.push(Target->ID);
			}
		}
		return Visited != InDegree.size();
	}

	EditorGraphPinDefinition Pin(std::string Key, std::string Label, EditorGraphPinDirection Direction,
		EditorGraphPinType Type, EditorGraphValue Default = {})
	{
		if (std::holds_alternative<std::monostate>(Default))
		{
			switch (Type)
			{
			case EditorGraphPinType::Float: Default = 0.0; break;
			case EditorGraphPinType::Vector3: Default = EditorGraphVector3{}; break;
			case EditorGraphPinType::Boolean: Default = false; break;
			case EditorGraphPinType::String: Default = std::string{}; break;
			case EditorGraphPinType::Execution: break;
			}
		}
		return { std::move(Key), std::move(Label), Direction, Type,
			Direction == EditorGraphPinDirection::Input ? EditorGraphPinCardinality::Single : EditorGraphPinCardinality::Multiple,
			std::move(Default) };
	}

	size_t EstimateBytes(const EditorGraph& Graph)
	{
		size_t Bytes = sizeof(Graph) + Graph.SchemaKey.size() + Graph.Nodes.size() * sizeof(EditorGraphNode) +
			Graph.Connections.size() * sizeof(EditorGraphConnection);
		for (const auto& Node : Graph.Nodes)
		{
			Bytes += Node.TypeKey.size() + Node.Pins.size() * sizeof(EditorGraphPin);
			for (const auto& PinValue : Node.Pins) Bytes += PinValue.Key.size();
			for (const auto& PinValue : Node.Pins)
				if (std::holds_alternative<std::string>(PinValue.DefaultValue))
					Bytes += std::get<std::string>(PinValue.DefaultValue).size();
		}
		return Bytes;
	}

	std::expected<PulseForge::UUID, std::string> NewID()
	{
		auto Result = PulseForge::UUID::Generate();
		if (!Result) return std::unexpected(Result.error().Message);
		return *Result;
	}
}

namespace PulseForgeEditor
{
	EditorGraphCommandHistory::EditorGraphCommandHistory(EditorGraph InitialGraph)
		: m_Graph(std::move(InitialGraph)), m_SavedGraph(m_Graph)
	{
	}

	bool EditorGraphCommandHistory::BeginTransaction()
	{
		if (m_Transaction) return false;
		m_Transaction = m_Graph;
		return true;
	}

	bool EditorGraphCommandHistory::CommitTransaction(const EditorGraphSchema& Schema, std::string& Error)
	{
		if (!m_Transaction)
		{
			Error = "No graph edit transaction is open";
			return false;
		}
		const auto Diagnostics = ValidateEditorGraph(m_Graph, Schema);
		if (!Diagnostics.empty())
		{
			Error = Diagnostics.front().Message;
			m_Graph = std::move(*m_Transaction);
			m_Transaction.reset();
			return false;
		}
		if (m_Graph != *m_Transaction)
		{
			PushUndo(std::move(*m_Transaction));
			m_Redo.clear();
		}
		m_Transaction.reset();
		return true;
	}

	void EditorGraphCommandHistory::CancelTransaction()
	{
		if (!m_Transaction) return;
		m_Graph = std::move(*m_Transaction);
		m_Transaction.reset();
	}

	void EditorGraphCommandHistory::Reset(EditorGraph SavedGraph)
	{
		m_Graph = std::move(SavedGraph);
		m_SavedGraph = m_Graph;
		m_Undo.clear();
		m_Redo.clear();
		m_Transaction.reset();
	}

	bool EditorGraphCommandHistory::Undo()
	{
		if (!CanUndo()) return false;
		m_Redo.push_back(std::move(m_Graph));
		m_Graph = std::move(m_Undo.back());
		m_Undo.pop_back();
		return true;
	}

	bool EditorGraphCommandHistory::Redo()
	{
		if (!CanRedo()) return false;
		m_Undo.push_back(std::move(m_Graph));
		m_Graph = std::move(m_Redo.back());
		m_Redo.pop_back();
		TrimHistory();
		return true;
	}

	void EditorGraphCommandHistory::MarkSaved()
	{
		if (!m_Transaction) m_SavedGraph = m_Graph;
	}

	bool EditorGraphCommandHistory::Replace(EditorGraph Candidate, const EditorGraphSchema& Schema, std::string& Error)
	{
		if (m_Transaction)
		{
			Error = "Cannot replace a graph while a gesture transaction is active";
			return false;
		}
		const auto Diagnostics = ValidateEditorGraph(Candidate, Schema);
		if (!Diagnostics.empty())
		{
			Error = Diagnostics.front().Message;
			return false;
		}
		if (Candidate == m_Graph) return true;
		PushUndo(std::move(m_Graph));
		m_Redo.clear();
		m_Graph = std::move(Candidate);
		return true;
	}

	bool EditorGraphCommandHistory::Execute(const EditorGraphSchema& Schema,
		const std::function<std::expected<void, std::string>(EditorGraph&)>& Mutation, std::string& Error)
	{
		EditorGraph Candidate = m_Graph;
		auto Mutated = Mutation(Candidate);
		if (!Mutated)
		{
			Error = Mutated.error();
			return false;
		}
		const auto Diagnostics = ValidateEditorGraph(Candidate, Schema);
		if (!Diagnostics.empty())
		{
			Error = Diagnostics.front().Message;
			return false;
		}
		if (Candidate == m_Graph) return true;
		if (!m_Transaction)
		{
			PushUndo(std::move(m_Graph));
			m_Redo.clear();
		}
		m_Graph = std::move(Candidate);
		return true;
	}

	void EditorGraphCommandHistory::PushUndo(EditorGraph Snapshot)
	{
		m_Undo.push_back(std::move(Snapshot));
		TrimHistory();
	}

	void EditorGraphCommandHistory::TrimHistory()
	{
		while (m_Undo.size() + m_Redo.size() > MaximumSnapshots && !m_Undo.empty()) m_Undo.erase(m_Undo.begin());
		size_t Bytes = 0;
		for (const auto& Graph : m_Undo) Bytes += EstimateBytes(Graph);
		for (const auto& Graph : m_Redo) Bytes += EstimateBytes(Graph);
		while (!m_Undo.empty() && Bytes > MaximumHistoryBytes)
		{
			Bytes -= EstimateBytes(m_Undo.front());
			m_Undo.erase(m_Undo.begin());
		}
	}

	std::string EditorGraphPropertyEditCoordinator::FinishActive(EditorGraphCommandHistory& History,
		const EditorGraphSchema& Schema)
	{
		if (!m_ActivePin) return {};
		m_ActivePin.reset();
		if (!History.HasOpenTransaction()) return {};
		std::string Error;
		if (!History.CommitTransaction(Schema, Error)) return Error;
		return {};
	}

	bool EditorGraphPropertyEditCoordinator::Begin(EditorGraphCommandHistory& History, PulseForge::UUID PinID,
		std::string& Error)
	{
		if (m_ActivePin == PinID && History.HasOpenTransaction()) return true;
		if (m_ActivePin)
		{
			Error = "A previous graph property edit could not be finished";
			return false;
		}
		if (History.HasOpenTransaction())
		{
			Error = "Another graph gesture is still active";
			return false;
		}
		if (!History.BeginTransaction())
		{
			Error = "Could not begin the graph property edit";
			return false;
		}
		m_ActivePin = PinID;
		return true;
	}

	EditorGraphPropertyEditResult EditorGraphPropertyEditCoordinator::ProcessFrame(EditorGraphCommandHistory& History,
		const EditorGraphSchema& Schema, const std::vector<EditorGraphPropertyEditEvent>& Events, bool CancelEdit)
	{
		const EditorGraph Before = History.Graph();
		if (CancelEdit) return Cancel(History);

		if (m_ActivePin && std::none_of(Events.begin(), Events.end(), [this](const auto& Event)
			{ return Event.PinID == *m_ActivePin; }))
		{
			if (History.HasOpenTransaction()) History.CancelTransaction();
			m_ActivePin.reset();
			return { History.Graph() != Before, {}, false, History.HasOpenTransaction() };
		}

		std::string Error;
		for (const auto& Event : Events)
		{
			if (Event.Activated && m_ActivePin != Event.PinID)
			{
				if (const auto FinishError = FinishActive(History, Schema); !FinishError.empty() && Error.empty())
					Error = FinishError;
				if (!Begin(History, Event.PinID, Error)) continue;
			}
			if (Event.Changed)
			{
				if (m_ActivePin != Event.PinID)
				{
					if (const auto FinishError = FinishActive(History, Schema); !FinishError.empty() && Error.empty())
						Error = FinishError;
					if (!Begin(History, Event.PinID, Error)) continue;
				}
				auto Changed = SetEditorGraphPinDefaultValue(History, Schema, Event.PinID, Event.Value);
				if (!Changed && Error.empty()) Error = Changed.error();
			}
			if (Event.Deactivated && m_ActivePin == Event.PinID)
				if (const auto FinishError = FinishActive(History, Schema); !FinishError.empty() && Error.empty())
					Error = FinishError;
		}
		return { History.Graph() != Before, std::move(Error), m_ActivePin.has_value(), History.HasOpenTransaction() };
	}

	EditorGraphPropertyEditResult EditorGraphPropertyEditCoordinator::Finish(EditorGraphCommandHistory& History,
		const EditorGraphSchema& Schema)
	{
		const EditorGraph Before = History.Graph();
		auto Error = FinishActive(History, Schema);
		return { Before != History.Graph(), std::move(Error), m_ActivePin.has_value(), History.HasOpenTransaction() };
	}

	EditorGraphPropertyEditResult EditorGraphPropertyEditCoordinator::Cancel(EditorGraphCommandHistory& History)
	{
		const EditorGraph Before = History.Graph();
		if (m_ActivePin && History.HasOpenTransaction()) History.CancelTransaction();
		m_ActivePin.reset();
		return { History.Graph() != Before, {}, m_ActivePin.has_value(), History.HasOpenTransaction() };
	}

	EditorGraphSchema MakeMaterialExpressionFixtureSchema()
	{
		return EditorGraphSchema{
			"pulseforge.material-expression-fixture", 1,
			EditorGraphCyclePolicy::Reject, EditorGraphCyclePolicy::Reject, true,
			{
				{ "material.scalar", "Scalar", { Pin("value", "Value", EditorGraphPinDirection::Output, EditorGraphPinType::Float) } },
				{ "material.multiply", "Multiply", {
					Pin("a", "A", EditorGraphPinDirection::Input, EditorGraphPinType::Float, 0.0),
					Pin("b", "B", EditorGraphPinDirection::Input, EditorGraphPinType::Float, 1.0),
					Pin("result", "Result", EditorGraphPinDirection::Output, EditorGraphPinType::Float) } },
				{ "material.color", "Color", { Pin("color", "Color", EditorGraphPinDirection::Output, EditorGraphPinType::Vector3,
					EditorGraphVector3{ 0.8, 0.2, 0.1 }) } },
				{ "material.output", "Material Output", { Pin("surface", "Surface", EditorGraphPinDirection::Input,
					EditorGraphPinType::Vector3, EditorGraphVector3{ 0.5, 0.5, 0.5 }) } }
			} };
	}

	EditorGraphSchema MakeGameplayEventFixtureSchema()
	{
		return EditorGraphSchema{
			"pulseforge.gameplay-event-fixture", 1,
			EditorGraphCyclePolicy::Reject, EditorGraphCyclePolicy::Allow, true,
			{
				{ "gameplay.event.begin", "Begin Event", { Pin("event", "Begin", EditorGraphPinDirection::Output, EditorGraphPinType::Execution) } },
				{ "gameplay.flow.branch", "Branch", {
					Pin("exec-in", "In", EditorGraphPinDirection::Input, EditorGraphPinType::Execution),
					Pin("condition", "Condition", EditorGraphPinDirection::Input, EditorGraphPinType::Boolean, false),
					Pin("true", "True", EditorGraphPinDirection::Output, EditorGraphPinType::Execution),
					Pin("false", "False", EditorGraphPinDirection::Output, EditorGraphPinType::Execution) } },
				{ "gameplay.action.log", "Log Message", {
					Pin("exec-in", "In", EditorGraphPinDirection::Input, EditorGraphPinType::Execution),
					Pin("message", "Message", EditorGraphPinDirection::Input, EditorGraphPinType::String, std::string{}) } }
			} };
	}

	std::expected<EditorGraph, std::string> CreateEditorGraph(const EditorGraphSchema& Schema, PulseForge::UUID GraphID)
	{
		if (Schema.Key.empty() || Schema.Version == 0 || GraphID.IsNil())
			return std::unexpected("Graph schema and identity must be valid");
		return EditorGraph{ GraphID, Schema.Key, Schema.Version, {}, {} };
	}

	std::expected<EditorGraphNode, std::string> CreateEditorGraphNode(
		const EditorGraphSchema& Schema, std::string_view TypeKey, double X, double Y)
	{
		const auto* Definition = FindDefinition(Schema, TypeKey);
		if (!Definition) return std::unexpected("The requested node type is not allowed by this graph schema");
		if (!IsFiniteCoordinate(X) || !IsFiniteCoordinate(Y)) return std::unexpected("Node position is outside the supported finite range");
		auto NodeID = NewID();
		if (!NodeID) return std::unexpected(NodeID.error());
		EditorGraphNode Node{ *NodeID, std::string(TypeKey), X, Y, {} };
		Node.Pins.reserve(Definition->Pins.size());
		for (const auto& PinDefinition : Definition->Pins)
		{
			auto PinID = NewID();
			if (!PinID) return std::unexpected(PinID.error());
			Node.Pins.push_back(EditorGraphPin{ *PinID, PinDefinition.Key, PinDefinition.Direction,
				PinDefinition.Type, PinDefinition.Cardinality, PinDefinition.DefaultValue });
		}
		return Node;
	}

	std::vector<EditorGraphDiagnostic> ValidateEditorGraph(const EditorGraph& Graph, const EditorGraphSchema& Schema)
	{
		std::vector<EditorGraphDiagnostic> Diagnostics;
		const auto Add = [&Diagnostics](std::string Code, std::string Message, PulseForge::UUID Element = {})
			{ Diagnostics.push_back({ std::move(Code), std::move(Message), Element }); };
		if (Graph.ID.IsNil() || Graph.SchemaKey != Schema.Key || Graph.SchemaVersion != Schema.Version)
			Add("schema", "Graph identity or schema version does not match the active schema", Graph.ID);
		if (Graph.Nodes.size() > MaximumNodes) Add("limit.nodes", "Graph node count exceeds the supported limit", Graph.ID);
		if (Graph.Connections.size() > MaximumConnections) Add("limit.connections", "Graph connection count exceeds the supported limit", Graph.ID);
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> IDs;
		if (!Graph.ID.IsNil()) IDs.insert(Graph.ID);
		std::unordered_map<PulseForge::UUID, const EditorGraphNode*, PulseForge::UUIDHash> Nodes;
		std::unordered_map<PulseForge::UUID, const EditorGraphPin*, PulseForge::UUIDHash> Pins;
		for (const auto& Node : Graph.Nodes)
		{
			if (Node.ID.IsNil() || !IDs.insert(Node.ID).second) Add("identity.node", "Duplicate or nil graph node identity", Node.ID);
			Nodes.emplace(Node.ID, &Node);
			if (!IsFiniteCoordinate(Node.X) || !IsFiniteCoordinate(Node.Y)) Add("layout.position", "Node position is invalid", Node.ID);
			const auto* Definition = FindDefinition(Schema, Node.TypeKey);
			if (!Definition)
			{
				Add("node.unknown", "Unknown graph node type '" + Node.TypeKey + "'", Node.ID);
				continue;
			}
			if (Node.Pins.size() > MaximumPinsPerNode || Node.Pins.size() != Definition->Pins.size())
				Add("pins.count", "Node pin count does not match the schema", Node.ID);
			std::unordered_set<std::string> PinKeys;
			for (const auto& PinValue : Node.Pins)
			{
				if (PinValue.ID.IsNil() || !IDs.insert(PinValue.ID).second) Add("identity.pin", "Duplicate or nil graph pin identity", PinValue.ID);
				Pins.emplace(PinValue.ID, &PinValue);
				const auto* Expected = FindPinDefinition(*Definition, PinValue.Key);
				if (!Expected || Expected->Direction != PinValue.Direction || Expected->Type != PinValue.Type ||
					Expected->Cardinality != PinValue.Cardinality || !IsValueCompatible(PinValue.Type, PinValue.DefaultValue))
					Add("pin.schema", "Graph pin does not match its stable schema definition", PinValue.ID);
				if (!PinKeys.insert(PinValue.Key).second) Add("pin.duplicate", "Node contains a duplicate pin key", PinValue.ID);
			}
		}
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> InputCounts;
		for (const auto& Connection : Graph.Connections)
		{
			if (Connection.ID.IsNil() || !IDs.insert(Connection.ID).second) Add("identity.connection", "Duplicate or nil graph connection identity", Connection.ID);
			const auto OutputIt = Pins.find(Connection.OutputPin);
			const auto InputIt = Pins.find(Connection.InputPin);
			if (OutputIt == Pins.end() || InputIt == Pins.end())
			{
				Add("connection.dangling", "Connection references a missing pin", Connection.ID);
				continue;
			}
			const auto* OutputNode = FindNodeForPin(Graph, Connection.OutputPin);
			const auto* InputNode = FindNodeForPin(Graph, Connection.InputPin);
			const auto& Output = *OutputIt->second;
			const auto& Input = *InputIt->second;
			if (Output.Direction != EditorGraphPinDirection::Output || Input.Direction != EditorGraphPinDirection::Input ||
				Output.Type != Input.Type || !OutputNode || !InputNode)
				Add("connection.type", "Connection has incompatible direction or pin types", Connection.ID);
			const auto CyclePolicy = Output.Type == EditorGraphPinType::Execution ? Schema.ExecutionCyclePolicy : Schema.DataCyclePolicy;
			if (OutputNode && InputNode && OutputNode->ID == InputNode->ID &&
				(Output.Type != EditorGraphPinType::Execution || CyclePolicy == EditorGraphCyclePolicy::Reject))
				Add("connection.self", "A node cannot connect to itself in the current fixture schemas", Connection.ID);
			if (++InputCounts[Connection.InputPin] > 1 && Input.Cardinality == EditorGraphPinCardinality::Single)
				Add("connection.cardinality", "A single-cardinality input has multiple connections", Connection.ID);
		}
		if (Schema.DataCyclePolicy == EditorGraphCyclePolicy::Reject && HasCycle(Graph, false))
			Add("connection.cycle", "Material/data dependency cycle violates this schema policy", Graph.ID);
		if (Schema.ExecutionCyclePolicy == EditorGraphCyclePolicy::Reject && HasCycle(Graph, true))
			Add("connection.cycle", "Gameplay execution cycle violates this schema policy", Graph.ID);
		return Diagnostics;
	}

	EditorGraphConnectionDecision CanConnectEditorGraphPins(const EditorGraph& Graph, const EditorGraphSchema& Schema,
		PulseForge::UUID FirstPin, PulseForge::UUID SecondPin, std::optional<PulseForge::UUID> ReplacingConnection)
	{
		const EditorGraphPin* A = nullptr;
		const EditorGraphPin* B = nullptr;
		const auto* NodeA = FindNodeForPin(Graph, FirstPin, &A);
		const auto* NodeB = FindNodeForPin(Graph, SecondPin, &B);
		if (!NodeA || !NodeB || !A || !B) return { false, "The dragged pin no longer exists", {}, {} };
		if (A->Direction == B->Direction) return { false, "Connect an output pin to an input pin", {}, {} };
		const bool AIsOutput = A->Direction == EditorGraphPinDirection::Output;
		const auto OutputID = AIsOutput ? FirstPin : SecondPin;
		const auto InputID = AIsOutput ? SecondPin : FirstPin;
		const auto* Output = AIsOutput ? A : B;
		const auto* Input = AIsOutput ? B : A;
		const auto* OutputNode = AIsOutput ? NodeA : NodeB;
		const auto* InputNode = AIsOutput ? NodeB : NodeA;
		if (Output->Type != Input->Type) return { false, "Pin types must match exactly; implicit conversions are not supported", OutputID, InputID };
		const auto CyclePolicy = Output->Type == EditorGraphPinType::Execution ? Schema.ExecutionCyclePolicy : Schema.DataCyclePolicy;
		if (OutputNode->ID == InputNode->ID &&
			(Output->Type != EditorGraphPinType::Execution || CyclePolicy == EditorGraphCyclePolicy::Reject))
			return { false, "A node cannot connect to itself in this fixture schema", OutputID, InputID };
		for (const auto& Existing : Graph.Connections)
		{
			if (ReplacingConnection && Existing.ID == *ReplacingConnection) continue;
			if (Existing.OutputPin == OutputID && Existing.InputPin == InputID)
				return { false, "These pins are already connected", OutputID, InputID };
		}
		if (Input->Cardinality == EditorGraphPinCardinality::Single && !Schema.ReplaceOccupiedInput)
		{
			for (const auto& Existing : Graph.Connections)
				if (Existing.InputPin == InputID && (!ReplacingConnection || Existing.ID != *ReplacingConnection))
					return { false, "This input already has a connection; disconnect it before creating another", OutputID, InputID };
		}
		const auto ReplacedInput = Input->Cardinality == EditorGraphPinCardinality::Single && Schema.ReplaceOccupiedInput
			? std::optional<PulseForge::UUID>{ InputID } : std::nullopt;
		if (CyclePolicy == EditorGraphCyclePolicy::Reject &&
			HasPath(Graph, InputNode->ID, OutputNode->ID, Output->Type == EditorGraphPinType::Execution,
				ReplacingConnection, ReplacedInput))
			return { false, "This connection would create a cycle rejected by the active schema", OutputID, InputID };
		return { true, "Connection is valid", OutputID, InputID };
	}

	std::expected<std::string, std::string> SerializeEditorGraph(const EditorGraph& Graph, const EditorGraphSchema& Schema)
	{
		const auto Diagnostics = ValidateEditorGraph(Graph, Schema);
		if (!Diagnostics.empty()) return std::unexpected(Diagnostics.front().Message);
		std::vector<const EditorGraphNode*> Nodes;
		for (const auto& Node : Graph.Nodes) Nodes.push_back(&Node);
		std::sort(Nodes.begin(), Nodes.end(), [](const auto* A, const auto* B) { return A->ID < B->ID; });
		std::vector<const EditorGraphConnection*> Connections;
		for (const auto& Connection : Graph.Connections) Connections.push_back(&Connection);
		std::sort(Connections.begin(), Connections.end(), [](const auto* A, const auto* B) { return A->ID < B->ID; });
		Json NodeRecords = Json::array();
		for (const auto* Node : Nodes) NodeRecords.push_back(SerializeNode(*Node));
		Json ConnectionRecords = Json::array();
		for (const auto* Connection : Connections) ConnectionRecords.push_back(SerializeConnection(*Connection));
		Json Document{ { "format", "PulseForgeGraph" }, { "version", 1 }, { "graph", IdText(Graph.ID) },
			{ "schema", Graph.SchemaKey }, { "schemaVersion", Graph.SchemaVersion },
			{ "nodes", std::move(NodeRecords) }, { "connections", std::move(ConnectionRecords) } };
		const std::string Text = Document.dump(2) + "\n";
		if (Text.size() > MaximumSourceBytes) return std::unexpected("Serialized graph exceeds the supported size limit");
		return Text;
	}

	std::expected<EditorGraph, std::string> DeserializeEditorGraph(std::string_view Source, const EditorGraphSchema& Schema)
	{
		if (Source.size() > MaximumSourceBytes) return std::unexpected("Graph source exceeds the supported size limit");
		try
		{
			const Json Document = Json::parse(Source);
			if (!Document.is_object() || !Document.contains("format") || !Document["format"].is_string() ||
				Document["format"].get<std::string>() != "PulseForgeGraph" || !Document.contains("version") ||
				!Document.contains("graph") || !Document.contains("schema") || !Document["schema"].is_string() ||
				!Document.contains("schemaVersion") || !Document.contains("nodes") || !Document["nodes"].is_array() ||
				!Document.contains("connections") || !Document["connections"].is_array())
				return std::unexpected("Graph source has an unsupported or malformed top-level structure");
			const auto Version = ReadVersion(Document["version"], "Graph format version");
			const auto SchemaVersion = ReadVersion(Document["schemaVersion"], "Graph schema version");
			if (!Version || *Version != 1) return std::unexpected(Version ? "Unsupported graph format version" : Version.error());
			if (!SchemaVersion || *SchemaVersion != Schema.Version || Document["schema"].get<std::string>() != Schema.Key)
				return std::unexpected(SchemaVersion ? "Graph schema is unknown or unsupported" : SchemaVersion.error());
			if (Document["nodes"].size() > MaximumNodes || Document["connections"].size() > MaximumConnections)
				return std::unexpected("Graph source exceeds a supported record-count limit");
			const auto GraphID = ReadID(Document["graph"], "graph id");
			if (!GraphID) return std::unexpected(GraphID.error());
			EditorGraph Graph{ *GraphID, Schema.Key, Schema.Version, {}, {} };
			Graph.Nodes.reserve(Document["nodes"].size());
			for (const Json& Record : Document["nodes"])
			{
				auto Node = ParseNode(Record, Schema);
				if (!Node) return std::unexpected(Node.error());
				Graph.Nodes.push_back(std::move(*Node));
			}
			Graph.Connections.reserve(Document["connections"].size());
			for (const Json& Record : Document["connections"])
			{
				if (!Record.is_object() || !Record.contains("id") || !Record.contains("output") || !Record.contains("input"))
					return std::unexpected("A graph connection record is malformed");
				const auto ID = ReadID(Record["id"], "connection id");
				const auto Output = ReadID(Record["output"], "connection output pin");
				const auto Input = ReadID(Record["input"], "connection input pin");
				if (!ID || !Output || !Input) return std::unexpected("A graph connection contains an invalid identity");
				Graph.Connections.push_back({ *ID, *Output, *Input });
			}
			const auto Diagnostics = ValidateEditorGraph(Graph, Schema);
			if (!Diagnostics.empty()) return std::unexpected(Diagnostics.front().Message);
			return Graph;
		}
		catch (const Json::exception& Error)
		{
			return std::unexpected(std::string("Graph source is malformed: ") + Error.what());
		}
	}

	EditorGraphClipboard CopyEditorGraphSubgraph(const EditorGraph& Graph, const std::vector<PulseForge::UUID>& NodeIDs)
	{
		EditorGraphClipboard Clipboard{ Graph.SchemaKey, Graph.SchemaVersion, {}, {} };
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Selected(NodeIDs.begin(), NodeIDs.end());
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> IncludedPins;
		for (const auto& Node : Graph.Nodes)
			if (Selected.contains(Node.ID))
			{
				Clipboard.Nodes.push_back(Node);
				for (const auto& PinValue : Node.Pins) IncludedPins.insert(PinValue.ID);
			}
		for (const auto& Connection : Graph.Connections)
			if (IncludedPins.contains(Connection.OutputPin) && IncludedPins.contains(Connection.InputPin))
				Clipboard.Connections.push_back(Connection);
		return Clipboard;
	}

	std::expected<std::string, std::string> SerializeEditorGraphClipboard(const EditorGraphClipboard& Clipboard)
	{
		if (Clipboard.SchemaKey.empty() || Clipboard.Nodes.size() > MaximumNodes || Clipboard.Connections.size() > MaximumConnections)
			return std::unexpected("Graph clipboard is invalid or exceeds its record limit");
		std::vector<const EditorGraphNode*> Nodes;
		for (const auto& Node : Clipboard.Nodes) Nodes.push_back(&Node);
		std::sort(Nodes.begin(), Nodes.end(), [](const auto* A, const auto* B) { return A->ID < B->ID; });
		std::vector<const EditorGraphConnection*> Connections;
		for (const auto& Connection : Clipboard.Connections) Connections.push_back(&Connection);
		std::sort(Connections.begin(), Connections.end(), [](const auto* A, const auto* B) { return A->ID < B->ID; });
		Json NodeRecords = Json::array();
		for (const auto* Node : Nodes) NodeRecords.push_back(SerializeNode(*Node));
		Json ConnectionRecords = Json::array();
		for (const auto* Connection : Connections) ConnectionRecords.push_back(SerializeConnection(*Connection));
		const std::string Text = Json{ { "format", "PulseForgeGraphClipboard" }, { "version", 1 },
			{ "schema", Clipboard.SchemaKey }, { "schemaVersion", Clipboard.SchemaVersion },
			{ "nodes", std::move(NodeRecords) }, { "connections", std::move(ConnectionRecords) } }.dump();
		if (Text.size() > MaximumClipboardBytes) return std::unexpected("Graph clipboard payload exceeds the supported size limit");
		return Text;
	}

	std::expected<void, std::string> PasteEditorGraphClipboard(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view Source, double OffsetX, double OffsetY)
	{
		if (Source.size() > MaximumClipboardBytes || !IsFiniteCoordinate(OffsetX) || !IsFiniteCoordinate(OffsetY))
			return std::unexpected("Graph clipboard payload or paste offset is invalid");
		try
		{
			const Json Document = Json::parse(Source);
			if (!Document.is_object() || !Document.contains("format") || Document["format"] != "PulseForgeGraphClipboard" ||
				!Document.contains("version") || !Document.contains("schema") || !Document["schema"].is_string() ||
				!Document.contains("schemaVersion") || !Document.contains("nodes") || !Document["nodes"].is_array() ||
				!Document.contains("connections") || !Document["connections"].is_array())
				return std::unexpected("Graph clipboard is malformed");
			const auto FormatVersion = ReadVersion(Document["version"], "Clipboard version");
			const auto SchemaVersion = ReadVersion(Document["schemaVersion"], "Clipboard schema version");
			if (!FormatVersion || *FormatVersion != 1 || !SchemaVersion || *SchemaVersion != Schema.Version ||
				Document["schema"].get<std::string>() != Schema.Key)
				return std::unexpected("Clipboard schema or version does not match the destination graph");
			if (Document["nodes"].empty() || Document["nodes"].size() > MaximumNodes || Document["connections"].size() > MaximumConnections)
				return std::unexpected("Graph clipboard has an invalid record count");
			EditorGraphClipboard Parsed{ Schema.Key, Schema.Version, {}, {} };
			for (const Json& Record : Document["nodes"])
			{
				auto Node = ParseNode(Record, Schema);
				if (!Node) return std::unexpected(Node.error());
				Parsed.Nodes.push_back(std::move(*Node));
			}
			for (const Json& Record : Document["connections"])
			{
				if (!Record.is_object() || !Record.contains("id") || !Record.contains("output") || !Record.contains("input"))
					return std::unexpected("Clipboard connection is malformed");
				const auto ID = ReadID(Record["id"], "clipboard connection id");
				const auto Output = ReadID(Record["output"], "clipboard output pin");
				const auto Input = ReadID(Record["input"], "clipboard input pin");
				if (!ID || !Output || !Input) return std::unexpected("Clipboard connection identity is invalid");
				Parsed.Connections.push_back({ *ID, *Output, *Input });
			}
			EditorGraph Candidate = History.Graph();
			EditorGraph Copied{ History.Graph().ID, Schema.Key, Schema.Version, Parsed.Nodes, Parsed.Connections };
			const auto ClipboardDiagnostics = ValidateEditorGraph(Copied, Schema);
			if (!ClipboardDiagnostics.empty()) return std::unexpected("Clipboard subgraph is invalid: " + ClipboardDiagnostics.front().Message);
			std::unordered_map<PulseForge::UUID, PulseForge::UUID, PulseForge::UUIDHash> PinRemapped;
			for (auto& Node : Parsed.Nodes)
			{
				auto NewNode = NewID(); if (!NewNode) return std::unexpected(NewNode.error());
				Node.ID = *NewNode; Node.X += OffsetX; Node.Y += OffsetY;
				for (auto& PinValue : Node.Pins)
				{
					const auto OldPin = PinValue.ID;
					auto NewPin = NewID(); if (!NewPin) return std::unexpected(NewPin.error());
					PinRemapped.emplace(OldPin, *NewPin); PinValue.ID = *NewPin;
				}
				Candidate.Nodes.push_back(std::move(Node));
			}
			for (auto Connection : Parsed.Connections)
			{
				auto NewConnection = NewID(); if (!NewConnection) return std::unexpected(NewConnection.error());
				Connection.ID = *NewConnection;
				Connection.OutputPin = PinRemapped.at(Connection.OutputPin);
				Connection.InputPin = PinRemapped.at(Connection.InputPin);
				Candidate.Connections.push_back(Connection);
			}
			std::string Error;
			if (!History.Replace(std::move(Candidate), Schema, Error)) return std::unexpected(Error);
			return {};
		}
		catch (const Json::exception& Error)
		{
			return std::unexpected(std::string("Graph clipboard is malformed: ") + Error.what());
		}
	}

	std::expected<void, std::string> AddEditorGraphNode(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view TypeKey, double X, double Y)
	{
		auto Node = CreateEditorGraphNode(Schema, TypeKey, X, Y);
		if (!Node) return std::unexpected(Node.error());
		std::string Error;
		if (!History.Execute(Schema, [Node = std::move(*Node)](EditorGraph& Graph) mutable -> std::expected<void, std::string>
			{ Graph.Nodes.push_back(std::move(Node)); return {}; }, Error)) return std::unexpected(Error);
		return {};
	}

	std::expected<void, std::string> DeleteEditorGraphNodes(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		const std::vector<PulseForge::UUID>& NodeIDs)
	{
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Selected(NodeIDs.begin(), NodeIDs.end());
		if (Selected.empty()) return {};
		std::string Error;
		if (!History.Execute(Schema, [Selected = std::move(Selected)](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			for (const auto ID : Selected)
				if (std::none_of(Graph.Nodes.begin(), Graph.Nodes.end(), [ID](const auto& Node) { return Node.ID == ID; }))
					return std::unexpected("A selected graph node no longer exists");
				std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Pins;
				Graph.Nodes.erase(std::remove_if(Graph.Nodes.begin(), Graph.Nodes.end(), [&Selected, &Pins](const auto& Node)
					{ if (!Selected.contains(Node.ID)) return false; for (const auto& PinValue : Node.Pins) Pins.insert(PinValue.ID); return true; }), Graph.Nodes.end());
				Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(), [&Pins](const auto& Connection)
					{ return Pins.contains(Connection.OutputPin) || Pins.contains(Connection.InputPin); }), Graph.Connections.end());
				return {};
			}, Error)) return std::unexpected(Error);
		return {};
	}

	std::expected<void, std::string> ConnectEditorGraphPins(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID FirstPin, PulseForge::UUID SecondPin)
	{
		const auto Decision = CanConnectEditorGraphPins(History.Graph(), Schema, FirstPin, SecondPin);
		if (!Decision.Allowed) return std::unexpected(Decision.Reason);
		auto ID = NewID(); if (!ID) return std::unexpected(ID.error());
		std::string Error;
		if (!History.Execute(Schema, [Decision, ConnectionID = *ID](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			const EditorGraphPin* TargetPin = nullptr;
			(void)FindNodeForPin(Graph, Decision.InputPin, &TargetPin);
			if (TargetPin && TargetPin->Cardinality == EditorGraphPinCardinality::Single)
				Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(), [&Decision](const auto& Existing)
					{ return Existing.InputPin == Decision.InputPin; }), Graph.Connections.end());
			Graph.Connections.push_back({ ConnectionID, Decision.OutputPin, Decision.InputPin });
			return {};
		}, Error))
			return std::unexpected(Error);
		return {};
	}

	std::expected<void, std::string> DisconnectEditorGraphConnection(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID ConnectionID)
	{
		std::string Error;
		if (!History.Execute(Schema, [ConnectionID](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			const auto It = std::find_if(Graph.Connections.begin(), Graph.Connections.end(), [ConnectionID](const auto& Item) { return Item.ID == ConnectionID; });
			if (It == Graph.Connections.end()) return std::unexpected("The graph connection no longer exists");
			Graph.Connections.erase(It); return {};
		}, Error)) return std::unexpected(Error);
		return {};
	}

	std::expected<void, std::string> ReconnectEditorGraphConnection(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID ConnectionID, PulseForge::UUID FirstPin, PulseForge::UUID SecondPin)
	{
		const auto Existing = std::find_if(History.Graph().Connections.begin(), History.Graph().Connections.end(),
			[ConnectionID](const auto& Item) { return Item.ID == ConnectionID; });
		if (Existing == History.Graph().Connections.end()) return std::unexpected("The graph connection no longer exists");
		const auto Decision = CanConnectEditorGraphPins(History.Graph(), Schema, FirstPin, SecondPin, ConnectionID);
		if (!Decision.Allowed) return std::unexpected(Decision.Reason);
		std::string Error;
		if (!History.Execute(Schema, [ConnectionID, Decision](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			auto It = std::find_if(Graph.Connections.begin(), Graph.Connections.end(), [ConnectionID](const auto& Item) { return Item.ID == ConnectionID; });
			if (It == Graph.Connections.end()) return std::unexpected("The graph connection changed before reconnection");
			const EditorGraphPin* TargetPin = nullptr;
			(void)FindNodeForPin(Graph, Decision.InputPin, &TargetPin);
			if (TargetPin && TargetPin->Cardinality == EditorGraphPinCardinality::Single)
				Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(),
					[ConnectionID, &Decision](const auto& Item) { return Item.ID != ConnectionID && Item.InputPin == Decision.InputPin; }),
					Graph.Connections.end());
			It = std::find_if(Graph.Connections.begin(), Graph.Connections.end(), [ConnectionID](const auto& Item) { return Item.ID == ConnectionID; });
			It->OutputPin = Decision.OutputPin;
			It->InputPin = Decision.InputPin;
			return {};
		}, Error)) return std::unexpected(Error);
		return {};
	}

	std::expected<void, std::string> MoveEditorGraphNodes(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		const std::vector<std::pair<PulseForge::UUID, EditorGraphVector3>>& Positions)
	{
		std::unordered_map<PulseForge::UUID, EditorGraphVector3, PulseForge::UUIDHash> NewPositions;
		for (const auto& [ID, Position] : Positions)
		{
			if (!IsFiniteCoordinate(Position.X) || !IsFiniteCoordinate(Position.Y)) return std::unexpected("Graph node movement is non-finite or outside bounds");
			NewPositions.emplace(ID, Position);
		}
		std::string Error;
		if (!History.Execute(Schema, [NewPositions = std::move(NewPositions)](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			for (const auto& [ID, Position] : NewPositions)
			{
				auto It = std::find_if(Graph.Nodes.begin(), Graph.Nodes.end(), [ID](const auto& Node) { return Node.ID == ID; });
				if (It == Graph.Nodes.end()) return std::unexpected("A moved graph node no longer exists");
				It->X = Position.X; It->Y = Position.Y;
			}
			return {};
		}, Error)) return std::unexpected(Error);
		return {};
	}

	std::expected<void, std::string> SetEditorGraphPinDefaultValue(EditorGraphCommandHistory& History,
		const EditorGraphSchema& Schema, PulseForge::UUID PinID, EditorGraphValue Value)
	{
		const EditorGraphPin* ExistingPin = nullptr;
		(void)FindNodeForPin(History.Graph(), PinID, &ExistingPin);
		if (!ExistingPin) return std::unexpected("The graph pin no longer exists");
		if (!IsValueCompatible(ExistingPin->Type, Value)) return std::unexpected("Default value does not match the pin's declared type");
		std::string Error;
		if (!History.Execute(Schema, [PinID, Value = std::move(Value)](EditorGraph& Graph) mutable -> std::expected<void, std::string>
		{
			EditorGraphPin* Target = nullptr;
			for (auto& Node : Graph.Nodes)
				for (auto& PinItem : Node.Pins)
					if (PinItem.ID == PinID) Target = &PinItem;
			if (!Target) return std::unexpected("The graph pin changed before the property edit");
			Target->DefaultValue = std::move(Value);
			return {};
		}, Error)) return std::unexpected(Error);
		return {};
	}
}
