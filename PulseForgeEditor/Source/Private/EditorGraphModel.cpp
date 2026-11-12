#include "Editor/EditorGraphModel.h"
#include "Editor/EditorGraphLayout.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
	constexpr size_t MaximumSourceBytes = EditorGraphMaximumSourceBytes;
	constexpr size_t MaximumClipboardBytes = 512 * 1024;
	constexpr size_t MaximumRoutePreparationBytes = 4 * EditorGraphCommandHistory::MaximumHistoryBytes;
	constexpr size_t MaximumNodes = EditorGraphMaximumNodes;
	constexpr size_t MaximumPinsPerNode = 64;
	constexpr size_t MaximumConnections = EditorGraphMaximumConnections;
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

	std::string_view GraphRerouteTypeKey(EditorGraphPinType Type)
	{
		switch (Type)
		{
		case EditorGraphPinType::Float: return "pulseforge.reroute.float";
		case EditorGraphPinType::Vector3: return "pulseforge.reroute.vector3";
		case EditorGraphPinType::Boolean: return "pulseforge.reroute.boolean";
		case EditorGraphPinType::String: return "pulseforge.reroute.string";
		case EditorGraphPinType::Execution: return "pulseforge.reroute.execution";
		}
		return {};
	}

	EditorGraphPinDefinition Pin(std::string Key, std::string Label, EditorGraphPinDirection Direction,
		EditorGraphPinType Type, EditorGraphValue Default = {});

	const std::array<EditorGraphNodeDefinition, 5>& GraphRerouteDefinitions()
	{
		static const std::array<EditorGraphNodeDefinition, 5> Definitions = []
		{
			std::array<EditorGraphNodeDefinition, 5> Result;
			const std::array Types = { EditorGraphPinType::Float, EditorGraphPinType::Vector3,
				EditorGraphPinType::Boolean, EditorGraphPinType::String, EditorGraphPinType::Execution };
			for (size_t Index = 0; Index < Types.size(); ++Index)
			{
				const auto Type = Types[Index];
				auto Input = Pin("in", "In", EditorGraphPinDirection::Input, Type);
				auto Output = Pin("out", "Out", EditorGraphPinDirection::Output, Type);
				Input.DefaultControl = EditorGraphPinDefaultControl::Never;
				Output.DefaultControl = EditorGraphPinDefaultControl::Never;
				Input.Hideable = Output.Hideable = false;
				Input.Required = Output.Required = true;
				Result[Index] = { std::string(GraphRerouteTypeKey(Type)), "Reroute", { std::move(Input), std::move(Output) } };
			}
			return Result;
		}();
		return Definitions;
	}

	const EditorGraphNodeDefinition* FindDefinition(const EditorGraphSchema& Schema, std::string_view Key)
	{
		const auto It = std::find_if(Schema.Nodes.begin(), Schema.Nodes.end(), [Key](const auto& Item)
			{ return Item.TypeKey == Key; });
		if (It != Schema.Nodes.end()) return &*It;
		if ((!Schema.ConnectionPolicy && !Schema.GraphValidationPolicy) || Schema.ReroutePolicyAware)
			for (const auto& Definition : GraphRerouteDefinitions())
				if (Definition.TypeKey == Key && std::find(Schema.RerouteTypes.begin(), Schema.RerouteTypes.end(),
					Definition.Pins.front().Type) != Schema.RerouteTypes.end()) return &Definition;
		return nullptr;
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

	bool IsValidUtf8(std::string_view Text)
	{
		const auto Continuation = [&Text](size_t Index)
		{
			return Index < Text.size() && (static_cast<unsigned char>(Text[Index]) & 0xC0) == 0x80;
		};
		for (size_t Index = 0; Index < Text.size();)
		{
			const auto First = static_cast<unsigned char>(Text[Index]);
			if (First <= 0x7F) { ++Index; continue; }
			if (First >= 0xC2 && First <= 0xDF)
			{
				if (!Continuation(Index + 1)) return false;
				Index += 2;
				continue;
			}
			if (First >= 0xE0 && First <= 0xEF)
			{
				if (Index + 2 >= Text.size() || !Continuation(Index + 2)) return false;
				const auto Second = static_cast<unsigned char>(Text[Index + 1]);
				if ((First == 0xE0 && (Second < 0xA0 || Second > 0xBF)) ||
					(First == 0xED && (Second < 0x80 || Second > 0x9F)) ||
					((First != 0xE0 && First != 0xED) && (Second < 0x80 || Second > 0xBF))) return false;
				Index += 3;
				continue;
			}
			if (First >= 0xF0 && First <= 0xF4)
			{
				if (Index + 3 >= Text.size() || !Continuation(Index + 2) || !Continuation(Index + 3)) return false;
				const auto Second = static_cast<unsigned char>(Text[Index + 1]);
				if ((First == 0xF0 && (Second < 0x90 || Second > 0xBF)) ||
					(First == 0xF4 && (Second < 0x80 || Second > 0x8F)) ||
					((First != 0xF0 && First != 0xF4) && (Second < 0x80 || Second > 0xBF))) return false;
				Index += 4;
				continue;
			}
			return false;
		}
		return true;
	}

	bool IsValidPersistedSchemaText(const EditorGraphSchema& Schema)
	{
		if (!IsValidUtf8(Schema.Key)) return false;
		for (const auto& Definition : Schema.Nodes)
		{
			if (!IsValidUtf8(Definition.TypeKey) || !IsValidUtf8(Definition.Label)) return false;
			for (const auto& PinDefinition : Definition.Pins)
				if (!IsValidUtf8(PinDefinition.Key) || !IsValidUtf8(PinDefinition.Label)) return false;
		}
		return true;
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
			return std::holds_alternative<std::string>(Value) && std::get<std::string>(Value).size() <= 4096 &&
				IsValidUtf8(std::get<std::string>(Value));
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

	struct PinOwner
	{
		const EditorGraphNode* Node = nullptr;
		const EditorGraphPin* Pin = nullptr;
	};
	struct IndexedEdge
	{
		PulseForge::UUID Connection;
		PulseForge::UUID InputPin;
		PulseForge::UUID Target;
		EditorGraphPinType Type;
	};
	struct GraphIndex
	{
		std::unordered_map<PulseForge::UUID, PinOwner, PulseForge::UUIDHash> Pins;
		std::unordered_map<PulseForge::UUID, std::vector<IndexedEdge>, PulseForge::UUIDHash> Outgoing;
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> IncidentCounts;
		std::unordered_map<PulseForge::UUID, std::vector<PulseForge::UUID>, PulseForge::UUIDHash> IncidentConnections;
	};

	GraphIndex BuildGraphIndex(const EditorGraph& Graph, EditorGraphWorkStats* Stats = nullptr,
		std::optional<PulseForge::UUID> OmitConnection = std::nullopt,
		std::optional<PulseForge::UUID> ReplacedInput = std::nullopt)
	{
		GraphIndex Index;
		for (const auto& Node : Graph.Nodes)
		{
			if (Stats) ++Stats->NodeVisits;
			for (const auto& Pin : Node.Pins)
			{
				if (Stats) ++Stats->PinVisits;
				Index.Pins.try_emplace(Pin.ID, PinOwner{ &Node, &Pin });
			}
		}
		for (const auto& Connection : Graph.Connections)
		{
			if (Stats) ++Stats->ConnectionVisits;
			++Index.IncidentCounts[Connection.OutputPin];
			++Index.IncidentCounts[Connection.InputPin];
			Index.IncidentConnections[Connection.OutputPin].push_back(Connection.ID);
			Index.IncidentConnections[Connection.InputPin].push_back(Connection.ID);
			if ((OmitConnection && Connection.ID == *OmitConnection) ||
				(ReplacedInput && Connection.InputPin == *ReplacedInput)) continue;
			const auto Output = Index.Pins.find(Connection.OutputPin);
			const auto Input = Index.Pins.find(Connection.InputPin);
			if (Output == Index.Pins.end() || Input == Index.Pins.end()) continue;
			if (Output->second.Pin->Direction != EditorGraphPinDirection::Output ||
				Input->second.Pin->Direction != EditorGraphPinDirection::Input ||
				Output->second.Pin->Type != Input->second.Pin->Type) continue;
			Index.Outgoing[Output->second.Node->ID].push_back(
				{ Connection.ID, Connection.InputPin, Input->second.Node->ID, Output->second.Pin->Type });
		}
		return Index;
	}

	EditorGraphPolicyEndpoint MakePolicyEndpoint(const GraphIndex& Index, PulseForge::UUID PinID)
	{
		const auto It = Index.Pins.find(PinID);
		if (It == Index.Pins.end()) return {};
		const auto& Owner = *It->second.Node;
		const auto& PinValue = *It->second.Pin;
		return { false, PinValue.ID, Owner.ID, Owner.TypeKey, PinValue.Key, PinValue.Direction, PinValue.Type,
			PinValue.Cardinality, PinValue.DefaultValue };
	}

	EditorGraphPolicyEndpoint MakeProspectivePolicyEndpoint(const EditorGraphNodeDefinition& Owner,
		const EditorGraphPinDefinition& Pin)
	{
		return { true, std::nullopt, std::nullopt, Owner.TypeKey, Pin.Key, Pin.Direction, Pin.Type,
			Pin.Cardinality, Pin.DefaultValue };
	}

	bool IsRemoved(std::span<const PulseForge::UUID> Removed, PulseForge::UUID ID)
	{
		return !Removed.empty() && std::find(Removed.begin(), Removed.end(), ID) != Removed.end();
	}

	void AppendOccupiedInputReplacements(const GraphIndex& Index, PulseForge::UUID InputID,
		bool InputIsSingle, bool ReplacementAllowed, std::vector<PulseForge::UUID>& Removed)
	{
		if (!InputIsSingle || !ReplacementAllowed) return;
		const auto It = Index.IncidentConnections.find(InputID);
		if (It == Index.IncidentConnections.end()) return;
		for (const auto ID : It->second)
			if (!IsRemoved(Removed, ID)) Removed.push_back(ID);
	}

	const EditorGraphNode* FindNodeForPin(const EditorGraph& Graph, PulseForge::UUID PinID,
		const EditorGraphPin** OutPin = nullptr)
	{
		const auto Index = BuildGraphIndex(Graph);
		const auto It = Index.Pins.find(PinID);
		if (It == Index.Pins.end()) return nullptr;
		if (OutPin) *OutPin = It->second.Pin;
		return It->second.Node;
	}

	bool HasPath(const GraphIndex& Index, PulseForge::UUID FromNode, PulseForge::UUID ToNode,
		bool Execution, std::optional<PulseForge::UUID> ReplacedInput = std::nullopt,
		EditorGraphWorkStats* Stats = nullptr)
	{
		std::vector<PulseForge::UUID> Stack{ FromNode };
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Seen;
		while (!Stack.empty())
		{
			const auto Current = Stack.back();
			Stack.pop_back();
			if (Current == ToNode) return true;
			if (!Seen.insert(Current).second) continue;
			const auto Edges = Index.Outgoing.find(Current);
			if (Edges == Index.Outgoing.end()) continue;
			for (const auto& Edge : Edges->second)
			{
				if (Stats) ++Stats->TraversedEdges;
				if (ReplacedInput && Edge.InputPin == *ReplacedInput) continue;
				if ((Edge.Type == EditorGraphPinType::Execution) == Execution) Stack.push_back(Edge.Target);
			}
		}
		return false;
	}

	bool HasCycle(const EditorGraph& Graph, const GraphIndex& Index, bool Execution, EditorGraphWorkStats* Stats = nullptr)
	{
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> InDegree;
		for (const auto& Node : Graph.Nodes) InDegree.emplace(Node.ID, 0);
		for (const auto& [Source, Edges] : Index.Outgoing)
		{
			(void)Source;
			for (const auto& Edge : Edges)
				if ((Edge.Type == EditorGraphPinType::Execution) == Execution) ++InDegree[Edge.Target];
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
			const auto Edges = Index.Outgoing.find(Current);
			if (Edges == Index.Outgoing.end()) continue;
			for (const auto& Edge : Edges->second)
			{
				if ((Edge.Type == EditorGraphPinType::Execution) != Execution) continue;
				if (Stats) ++Stats->TraversedEdges;
				if (--InDegree[Edge.Target] == 0) Ready.push(Edge.Target);
			}
		}
		return Visited != InDegree.size();
	}

	struct RerouteTopologyIssue final
	{
		bool OnlyCycle = false;
		PulseForge::UUID SingleOutputFanout;
	};

	RerouteTopologyIssue InspectRerouteTopology(const EditorGraph& Graph,
		std::span<const PulseForge::UUID> RemovedConnectionIDs = {},
		std::optional<std::pair<PulseForge::UUID, PulseForge::UUID>> Proposed = std::nullopt,
		std::optional<EditorGraphPinCardinality> ProspectiveOutputCardinality = std::nullopt,
		EditorGraphWorkStats* Stats = nullptr)
	{
		if (Stats) ++Stats->RerouteTopologyBuilds;
		std::unordered_map<PulseForge::UUID, const EditorGraphNode*, PulseForge::UUIDHash> PinOwners;
		std::unordered_map<PulseForge::UUID, PulseForge::UUID, PulseForge::UUIDHash> RerouteOutputs;
		std::unordered_map<PulseForge::UUID, EditorGraphPinCardinality, PulseForge::UUIDHash> OutputCardinality;
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> RerouteNodes;
		EditorGraphNode ProspectiveNode;
		for (const auto& Node : Graph.Nodes)
		{
			const bool IsReroute = IsEditorGraphRerouteType(Node.TypeKey);
			if (IsReroute) RerouteNodes.insert(Node.ID);
			for (const auto& PinValue : Node.Pins)
			{
				PinOwners.emplace(PinValue.ID, &Node);
				if (PinValue.Direction == EditorGraphPinDirection::Output)
				{
					OutputCardinality.emplace(PinValue.ID, PinValue.Cardinality);
					if (IsReroute && PinValue.Key == "out") RerouteOutputs.emplace(Node.ID, PinValue.ID);
				}
			}
		}
		if (ProspectiveOutputCardinality)
		{
			ProspectiveNode.TypeKey = "<prospective>";
			PinOwners.emplace(PulseForge::UUID{}, &ProspectiveNode);
			OutputCardinality.emplace(PulseForge::UUID{}, *ProspectiveOutputCardinality);
		}
		std::unordered_map<PulseForge::UUID, std::vector<PulseForge::UUID>, PulseForge::UUIDHash> Outgoing;
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> RerouteInDegree;
		for (const auto NodeID : RerouteNodes) RerouteInDegree.emplace(NodeID, 0);
		const auto AddEdge = [&](PulseForge::UUID OutputPin, PulseForge::UUID InputPin)
		{
			Outgoing[OutputPin].push_back(InputPin);
			const auto OutputOwner = PinOwners.find(OutputPin);
			const auto InputOwner = PinOwners.find(InputPin);
			if (OutputOwner != PinOwners.end() && InputOwner != PinOwners.end() &&
				RerouteNodes.contains(OutputOwner->second->ID) && RerouteNodes.contains(InputOwner->second->ID))
				++RerouteInDegree[InputOwner->second->ID];
		};
		for (const auto& Connection : Graph.Connections)
			if (std::find(RemovedConnectionIDs.begin(), RemovedConnectionIDs.end(), Connection.ID) == RemovedConnectionIDs.end())
				AddEdge(Connection.OutputPin, Connection.InputPin);
		if (Proposed) AddEdge(Proposed->first, Proposed->second);

		std::vector<PulseForge::UUID> Ready;
		for (const auto& [NodeID, Degree] : RerouteInDegree)
			if (Degree == 0) Ready.push_back(NodeID);
		size_t Visited = 0;
		while (!Ready.empty())
		{
			const auto NodeID = Ready.back();
			Ready.pop_back();
			++Visited;
			const auto Output = RerouteOutputs.find(NodeID);
			if (Output == RerouteOutputs.end()) continue;
			const auto Edges = Outgoing.find(Output->second);
			if (Edges == Outgoing.end()) continue;
			for (const auto InputPin : Edges->second)
			{
				const auto Owner = PinOwners.find(InputPin);
				if (Owner != PinOwners.end() && RerouteNodes.contains(Owner->second->ID) &&
					--RerouteInDegree[Owner->second->ID] == 0)
					Ready.push_back(Owner->second->ID);
			}
		}
		RerouteTopologyIssue Result;
		Result.OnlyCycle = Visited != RerouteNodes.size();

		for (const auto& [OutputPin, Cardinality] : OutputCardinality)
		{
			if (Cardinality != EditorGraphPinCardinality::Single) continue;
			const auto Owner = PinOwners.find(OutputPin);
			if (Owner == PinOwners.end() || RerouteNodes.contains(Owner->second->ID)) continue;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Consumers;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> VisitedReroutes;
			std::vector<PulseForge::UUID> PendingInputs;
			if (const auto Edges = Outgoing.find(OutputPin); Edges != Outgoing.end())
				PendingInputs.insert(PendingInputs.end(), Edges->second.begin(), Edges->second.end());
			while (!PendingInputs.empty())
			{
				const auto InputPin = PendingInputs.back();
				PendingInputs.pop_back();
				if (InputPin.IsNil())
				{
					Consumers.insert(InputPin);
					continue;
				}
				const auto InputOwner = PinOwners.find(InputPin);
				if (InputOwner == PinOwners.end()) continue;
				if (!RerouteNodes.contains(InputOwner->second->ID))
				{
					Consumers.insert(InputPin);
					continue;
				}
				if (!VisitedReroutes.insert(InputOwner->second->ID).second) continue;
				const auto RerouteOutput = RerouteOutputs.find(InputOwner->second->ID);
				if (RerouteOutput == RerouteOutputs.end()) continue;
				if (const auto Edges = Outgoing.find(RerouteOutput->second); Edges != Outgoing.end())
					PendingInputs.insert(PendingInputs.end(), Edges->second.begin(), Edges->second.end());
			}
			if (Consumers.size() > 1)
			{
				Result.SingleOutputFanout = OutputPin;
				return Result;
			}
		}
		return Result;
	}

	EditorGraphPinDefinition Pin(std::string Key, std::string Label, EditorGraphPinDirection Direction,
		EditorGraphPinType Type, EditorGraphValue Default)
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

	std::expected<void, std::string> EnsureGraphSourceIsWritable(const EditorGraph& Graph,
		const EditorGraphSchema& Schema)
	{
		// Every serialized user string can expand by at most six bytes per input
		// byte under JSON escaping. Include deliberately generous per-record fixed
		// overhead (UUIDs, keys, indentation, separators and numeric fields). The
		// bound avoids materializing JSON for ordinary edits and route prefixes; only
		// states near the source cap take the exact codec path.
		size_t UpperBound = 1024;
		const auto AddBounded = [&UpperBound](size_t Amount)
		{
			if (Amount > EditorGraphMaximumSourceBytes || UpperBound > EditorGraphMaximumSourceBytes - Amount)
				UpperBound = EditorGraphMaximumSourceBytes + 1;
			else UpperBound += Amount;
		};
		const auto AddEscapedText = [&AddBounded](size_t Length)
		{
			AddBounded(Length > EditorGraphMaximumSourceBytes / 6
				? EditorGraphMaximumSourceBytes + 1 : Length * 6);
		};
		AddEscapedText(Schema.Key.size());
		for (const auto& Node : Graph.Nodes)
		{
			AddBounded(1024);
			AddEscapedText(Node.TypeKey.size());
			for (const auto& PinValue : Node.Pins)
			{
				AddBounded(1024);
				AddEscapedText(PinValue.Key.size());
				if (std::holds_alternative<std::string>(PinValue.DefaultValue))
					AddEscapedText(std::get<std::string>(PinValue.DefaultValue).size());
			}
		}
		AddBounded(Graph.Connections.size() * 512);
		if (UpperBound <= EditorGraphMaximumSourceBytes) return {};
		const auto Source = SerializeEditorGraph(Graph, Schema);
		if (!Source) return std::unexpected(Source.error());
		return {};
	}
}

namespace PulseForgeEditor
{
	std::expected<void, std::string> EditorGraphSchemaRegistry::Register(EditorGraphSchema Schema)
	{
		if (Schema.Key.empty() || Schema.Version == 0)
			return std::unexpected("A graph schema requires a stable key and nonzero version");
		if (!IsValidPersistedSchemaText(Schema))
			return std::unexpected("Graph schema keys and labels must contain valid UTF-8");
		if (Schema.Nodes.empty()) return std::unexpected("A graph schema must provide a node catalog");
		if ((!Schema.RerouteTypes.empty()) && (Schema.ConnectionPolicy || Schema.GraphValidationPolicy) &&
			!Schema.ReroutePolicyAware)
			return std::unexpected("A schema with custom policy callbacks must explicitly implement reroute transparency before opting in");
		{
			std::unordered_set<EditorGraphPinType> Types;
			for (const auto Type : Schema.RerouteTypes)
				if (!Types.insert(Type).second)
					return std::unexpected("A graph schema lists the same reroute pin type more than once");
		}
		std::unordered_set<std::string> NodeKeys;
		for (const auto& Node : Schema.Nodes)
		{
			if (Node.TypeKey.empty() || !NodeKeys.insert(Node.TypeKey).second)
				return std::unexpected("A graph schema contains an empty or duplicate node type key");
			if (std::any_of(GraphRerouteDefinitions().begin(), GraphRerouteDefinitions().end(), [&Node](const auto& Definition)
				{ return Definition.TypeKey == Node.TypeKey; }))
				return std::unexpected("Graph node keys cannot reuse reserved typed-reroute identities");
			std::unordered_set<std::string> PinKeys;
			for (const auto& Pin : Node.Pins)
				if (Pin.Key.empty() || !PinKeys.insert(Pin.Key).second || !IsValueCompatible(Pin.Type, Pin.DefaultValue))
					return std::unexpected("A graph schema contains an invalid or duplicate pin definition");
		}
		if (m_Schemas.contains(Schema.Key))
			return std::unexpected("A graph schema with this stable key is already registered");
		m_Schemas.emplace(Schema.Key, std::move(Schema));
		return {};
	}

	std::expected<EditorGraphSchema, std::string> EditorGraphSchemaRegistry::Resolve(
		std::string_view Key, uint32_t Version) const
	{
		const auto It = m_Schemas.find(std::string(Key));
		if (It == m_Schemas.end()) return std::unexpected("Unknown graph schema '" + std::string(Key) + "'");
		if (It->second.Version != Version)
			return std::unexpected("Graph schema '" + std::string(Key) + "' does not support version " + std::to_string(Version));
		return It->second;
	}

	bool EditorGraphSchemaRegistry::Contains(std::string_view Key) const
	{
		return m_Schemas.contains(std::string(Key));
	}

	bool ShouldShowEditorGraphPinDefault(const EditorGraphPin& Pin,
		const EditorGraphPinDefinition& Definition, size_t IncidentConnectionCount) noexcept
	{
		return Definition.DefaultControl == EditorGraphPinDefaultControl::Always ||
			(Definition.DefaultControl == EditorGraphPinDefaultControl::WhenDisconnected && IncidentConnectionCount == 0);
	}

	bool CanHideEditorGraphPin(const EditorGraphPinDefinition& Definition) noexcept
	{
		return Definition.Hideable && !Definition.Required;
	}

	bool CanAutoHideEditorGraphPin(const EditorGraphPin& Pin,
		const EditorGraphPinDefinition& Definition, size_t IncidentConnectionCount, bool NodeConnected) noexcept
	{
		if (!NodeConnected || !CanHideEditorGraphPin(Definition) || IncidentConnectionCount != 0) return false;
		if (Pin.Direction == EditorGraphPinDirection::Input && Pin.DefaultValue != Definition.DefaultValue)
			return false;
		return true;
	}

	bool IsEditorGraphNodeConnected(const EditorGraph& Graph, PulseForge::UUID NodeID)
	{
		const auto Node = std::find_if(Graph.Nodes.begin(), Graph.Nodes.end(), [NodeID](const auto& Candidate)
			{ return Candidate.ID == NodeID; });
		if (Node == Graph.Nodes.end()) return false;
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> PinIDs;
		PinIDs.reserve(Node->Pins.size());
		for (const auto& Pin : Node->Pins) PinIDs.insert(Pin.ID);
		return std::any_of(Graph.Connections.begin(), Graph.Connections.end(), [&PinIDs](const auto& Connection)
			{ return PinIDs.contains(Connection.OutputPin) || PinIDs.contains(Connection.InputPin); });
	}

	bool IsEditorGraphRerouteType(std::string_view TypeKey)
	{
		return std::any_of(GraphRerouteDefinitions().begin(), GraphRerouteDefinitions().end(), [TypeKey](const auto& Definition)
			{ return Definition.TypeKey == TypeKey; });
	}

	const EditorGraphNodeDefinition* FindEditorGraphNodeDefinition(const EditorGraphSchema& Schema, std::string_view TypeKey)
	{
		return FindDefinition(Schema, TypeKey);
	}

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
			BumpGraphRevision();
			return false;
		}
		if (auto Writable = EnsureGraphSourceIsWritable(m_Graph, Schema); !Writable)
		{
			Error = Writable.error();
			m_Graph = std::move(*m_Transaction);
			m_Transaction.reset();
			BumpGraphRevision();
			return false;
		}
		if (m_Graph != *m_Transaction)
		{
			PushUndo(std::move(*m_Transaction));
			m_Redo.clear();
			BumpGraphRevision();
		}
		m_Transaction.reset();
		return true;
	}

	void EditorGraphCommandHistory::CancelTransaction()
	{
		if (!m_Transaction) return;
		m_Graph = std::move(*m_Transaction);
		m_Transaction.reset();
		BumpGraphRevision();
	}

	void EditorGraphCommandHistory::Reset(EditorGraph SavedGraph)
	{
		m_Graph = std::move(SavedGraph);
		m_SavedGraph = m_Graph;
		m_Undo.clear();
		m_Redo.clear();
		m_Transaction.reset();
		BumpGraphRevision();
	}

	bool EditorGraphCommandHistory::Undo()
	{
		if (!CanUndo()) return false;
		m_Redo.push_back(std::move(m_Graph));
		m_Graph = std::move(m_Undo.back());
		m_Undo.pop_back();
		BumpGraphRevision();
		return true;
	}

	bool EditorGraphCommandHistory::Redo()
	{
		if (!CanRedo()) return false;
		m_Undo.push_back(std::move(m_Graph));
		m_Graph = std::move(m_Redo.back());
		m_Redo.pop_back();
		TrimHistory();
		BumpGraphRevision();
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
		if (auto Writable = EnsureGraphSourceIsWritable(Candidate, Schema); !Writable)
		{
			Error = Writable.error();
			return false;
		}
		if (Candidate == m_Graph) return true;
		PushUndo(std::move(m_Graph));
		m_Redo.clear();
		m_Graph = std::move(Candidate);
		BumpGraphRevision();
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
		if (auto Writable = EnsureGraphSourceIsWritable(Candidate, Schema); !Writable)
		{
			Error = Writable.error();
			return false;
		}
		if (Candidate == m_Graph) return true;
		if (!m_Transaction)
		{
			PushUndo(std::move(m_Graph));
			m_Redo.clear();
		}
		m_Graph = std::move(Candidate);
		BumpGraphRevision();
		return true;
	}

	bool EditorGraphCommandHistory::ExecuteSequence(const EditorGraphSchema& Schema,
		std::span<const EditorGraph> Candidates, std::string& Error)
	{
		if (Candidates.size() > MaximumSnapshots)
		{
			Error = "The route needs more Undo steps than the history snapshot limit can retain";
			return false;
		}
		try
		{
			std::vector<EditorGraph> OwnedCandidates(Candidates.begin(), Candidates.end());
			return ExecuteSequence(Schema, std::move(OwnedCandidates), Error);
		}
		catch (const std::bad_alloc&)
		{
			Error = "The graph edit sequence could not reserve its validation workspace";
			return false;
		}
	}

	bool EditorGraphCommandHistory::CanAdmitRoute(size_t PlannedSteps, size_t RouteSnapshotBytes,
		size_t CandidatePlanBytes, size_t PlanningGraphBytes, std::string& Error) const
	{
		if (m_Transaction)
		{
			Error = "Cannot plan a graph route while another graph edit transaction is active";
			return false;
		}
		if (PlannedSteps > MaximumSnapshots || RouteSnapshotBytes > MaximumHistoryBytes)
		{
			Error = PlannedSteps > MaximumSnapshots ?
				"The route needs more Undo steps than the history snapshot limit can retain" :
				"The route's required Undo snapshots exceed the graph history memory budget";
			return false;
		}
		const size_t AvailableSnapshots = MaximumSnapshots - PlannedSteps;
		size_t RetainedStart = m_Undo.size();
		size_t RetainedBytes = RouteSnapshotBytes;
		while (RetainedStart > 0 && m_Undo.size() - RetainedStart < AvailableSnapshots)
		{
			const size_t NextBytes = EstimateBytes(m_Undo[RetainedStart - 1]);
			if (NextBytes > MaximumHistoryBytes - RetainedBytes) break;
			RetainedBytes += NextBytes;
			--RetainedStart;
		}
		size_t PeakPreparationBytes = EstimateBytes(m_Graph);
		const auto AddPreparationBytes = [&PeakPreparationBytes](size_t Amount)
		{
			if (Amount > MaximumRoutePreparationBytes ||
				PeakPreparationBytes > MaximumRoutePreparationBytes - Amount) return false;
			PeakPreparationBytes += Amount;
			return true;
		};
		for (const auto& Snapshot : m_Undo)
			if (!AddPreparationBytes(EstimateBytes(Snapshot)))
			{
				Error = "The route cannot be prepared within the bounded history allocation budget";
				return false;
			}
		if (!AddPreparationBytes(PlanningGraphBytes) || !AddPreparationBytes(CandidatePlanBytes) ||
			!AddPreparationBytes(RetainedBytes))
		{
			Error = "The route cannot be prepared within the bounded history allocation budget";
			return false;
		}
		return true;
	}

	bool EditorGraphCommandHistory::ExecuteSequence(const EditorGraphSchema& Schema,
		std::vector<EditorGraph>&& Candidates, std::string& Error)
	{
		if (m_Transaction)
		{
			Error = "Cannot publish a graph edit sequence while a transaction is active";
			return false;
		}
		if (Candidates.empty()) return true;
		if (Candidates.size() > MaximumSnapshots)
		{
			Error = "The route needs more Undo steps than the history snapshot limit can retain";
			return false;
		}

		const EditorGraph* Previous = &m_Graph;
		size_t RouteStateBytes = EstimateBytes(m_Graph);
		size_t SmallestRouteStateBytes = RouteStateBytes;
		size_t CandidatePlanBytes = 0;
		for (const auto& Candidate : Candidates)
		{
			if (Candidate == *Previous)
			{
				Error = "A graph edit sequence contains an empty history step";
				return false;
			}
			const auto Diagnostics = ValidateEditorGraph(Candidate, Schema);
			if (!Diagnostics.empty())
			{
				Error = Diagnostics.front().Message;
				return false;
			}
			if (auto Writable = EnsureGraphSourceIsWritable(Candidate, Schema); !Writable)
			{
				Error = Writable.error();
				return false;
			}
			const size_t CandidateBytes = EstimateBytes(Candidate);
			if (CandidateBytes > MaximumRoutePreparationBytes - CandidatePlanBytes)
			{
				Error = "The route's temporary planning snapshots exceed the bounded preparation budget";
				return false;
			}
			CandidatePlanBytes += CandidateBytes;
			if (CandidateBytes > std::numeric_limits<size_t>::max() - RouteStateBytes)
			{
				Error = "The route's required Undo snapshots exceed the graph history memory budget";
				return false;
			}
			RouteStateBytes += CandidateBytes;
			SmallestRouteStateBytes = std::min(SmallestRouteStateBytes, CandidateBytes);
			Previous = &Candidate;
		}
		// At each Undo/Redo position, one route state is live and all others are
		// retained across the Undo and Redo stacks. The worst position leaves the
		// smallest state live. Older unrelated Undo entries are added below.
		const size_t RouteSnapshotBytes = RouteStateBytes - SmallestRouteStateBytes;
		if (RouteSnapshotBytes > MaximumHistoryBytes)
		{
			Error = "The route's required Undo snapshots exceed the graph history memory budget";
			return false;
		}

		// Retain the newest unrelated snapshots that fit, while reserving capacity
		// for every route step. Prepare the graph and history off to the side so an
		// allocation failure cannot publish a partial route or consume Redo.
		const size_t RouteSnapshotCount = Candidates.size();
		const size_t AvailableSnapshots = MaximumSnapshots - RouteSnapshotCount;
		size_t RetainedStart = m_Undo.size();
		size_t RetainedBytes = RouteSnapshotBytes;
		while (RetainedStart > 0 && m_Undo.size() - RetainedStart < AvailableSnapshots)
		{
			const size_t NextBytes = EstimateBytes(m_Undo[RetainedStart - 1]);
			if (NextBytes > MaximumHistoryBytes - RetainedBytes) break;
			RetainedBytes += NextBytes;
			--RetainedStart;
		}
		size_t PeakPreparationBytes = EstimateBytes(m_Graph);
		const auto AddPreparationBytes = [&PeakPreparationBytes](size_t Amount)
		{
			if (Amount > MaximumRoutePreparationBytes ||
				PeakPreparationBytes > MaximumRoutePreparationBytes - Amount) return false;
			PeakPreparationBytes += Amount;
			return true;
		};
		for (const auto& Snapshot : m_Undo)
			if (!AddPreparationBytes(EstimateBytes(Snapshot)))
			{
				Error = "The route cannot be prepared within the bounded history allocation budget";
				return false;
			}
		if (!AddPreparationBytes(CandidatePlanBytes) || !AddPreparationBytes(RetainedBytes))
		{
			Error = "The route cannot be prepared within the bounded history allocation budget";
			return false;
		}
		try
		{
			std::vector<EditorGraph> NextUndo;
			NextUndo.reserve(m_Undo.size() - RetainedStart + RouteSnapshotCount);
			NextUndo.insert(NextUndo.end(), m_Undo.begin() + static_cast<std::ptrdiff_t>(RetainedStart), m_Undo.end());
			NextUndo.push_back(m_Graph);
			for (size_t Index = 0; Index + 1 < Candidates.size(); ++Index)
				NextUndo.push_back(std::move(Candidates[Index]));
			EditorGraph NextGraph = std::move(Candidates.back());
			m_Undo.swap(NextUndo);
			m_Redo.clear();
			m_Graph = std::move(NextGraph);
		}
		catch (const std::bad_alloc&)
		{
			Error = "The route could not reserve its complete Undo history; the graph was not changed";
			return false;
		}
		BumpGraphRevision();
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
		auto Schema = EditorGraphSchema{
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
		Schema.RerouteTypes = { EditorGraphPinType::Float, EditorGraphPinType::Vector3 };
		return Schema;
	}

	EditorGraphSchema MakeGameplayEventFixtureSchema()
	{
		auto Schema = EditorGraphSchema{
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
		Schema.RerouteTypes = { EditorGraphPinType::Execution };
		return Schema;
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

	std::vector<EditorGraphDiagnostic> ValidateEditorGraph(const EditorGraph& Graph, const EditorGraphSchema& Schema,
		EditorGraphWorkStats* Stats)
	{
		if (Stats) *Stats = {};
		const GraphIndex Index = BuildGraphIndex(Graph, Stats);
		std::vector<EditorGraphDiagnostic> Diagnostics;
		const auto Add = [&Diagnostics](std::string Code, std::string Message, PulseForge::UUID Element = {})
			{ Diagnostics.push_back({ std::move(Code), std::move(Message), Element }); };
		if (!IsValidPersistedSchemaText(Schema))
			Add("text.utf8", "Graph schema keys and labels must contain valid UTF-8", Graph.ID);
		if (Graph.ID.IsNil() || Graph.SchemaKey != Schema.Key || Graph.SchemaVersion != Schema.Version)
			Add("schema", "Graph identity or schema version does not match the active schema", Graph.ID);
		if (!IsValidUtf8(Graph.SchemaKey)) Add("text.utf8", "Graph schema key must contain valid UTF-8", Graph.ID);
		if (Graph.Nodes.size() > MaximumNodes) Add("limit.nodes", "Graph node count exceeds the supported limit", Graph.ID);
		if (Graph.Connections.size() > MaximumConnections) Add("limit.connections", "Graph connection count exceeds the supported limit", Graph.ID);
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> IDs;
		if (!Graph.ID.IsNil()) IDs.insert(Graph.ID);
		std::unordered_map<PulseForge::UUID, const EditorGraphNode*, PulseForge::UUIDHash> Nodes;
		std::unordered_map<PulseForge::UUID, const EditorGraphPin*, PulseForge::UUIDHash> Pins;
		for (const auto& Node : Graph.Nodes)
		{
			if (Node.ID.IsNil() || !IDs.insert(Node.ID).second) Add("identity.node", "Duplicate or nil graph node identity", Node.ID);
			if (!IsValidUtf8(Node.TypeKey)) Add("text.utf8", "Graph node type key must contain valid UTF-8", Node.ID);
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
				if (!IsValidUtf8(PinValue.Key)) Add("text.utf8", "Graph pin key must contain valid UTF-8", PinValue.ID);
				if (std::holds_alternative<std::string>(PinValue.DefaultValue) &&
					!IsValidUtf8(std::get<std::string>(PinValue.DefaultValue)))
					Add("text.utf8", "Graph string defaults must contain valid UTF-8", PinValue.ID);
				Pins.emplace(PinValue.ID, &PinValue);
				const auto* Expected = FindPinDefinition(*Definition, PinValue.Key);
				if (!Expected || Expected->Direction != PinValue.Direction || Expected->Type != PinValue.Type ||
					Expected->Cardinality != PinValue.Cardinality || !IsValueCompatible(PinValue.Type, PinValue.DefaultValue))
					Add("pin.schema", "Graph pin does not match its stable schema definition", PinValue.ID);
				if (!PinKeys.insert(PinValue.Key).second) Add("pin.duplicate", "Node contains a duplicate pin key", PinValue.ID);
			}
		}
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> InputCounts;
		std::unordered_map<PulseForge::UUID, size_t, PulseForge::UUIDHash> OutputCounts;
		std::unordered_set<std::string> EndpointPairs;
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
			const auto OutputOwner = Index.Pins.find(Connection.OutputPin);
			const auto InputOwner = Index.Pins.find(Connection.InputPin);
			const auto& Output = *OutputIt->second;
			const auto& Input = *InputIt->second;
			if (Output.Direction != EditorGraphPinDirection::Output || Input.Direction != EditorGraphPinDirection::Input ||
				Output.Type != Input.Type || OutputOwner == Index.Pins.end() || InputOwner == Index.Pins.end())
				Add("connection.type", "Connection has incompatible direction or pin types", Connection.ID);
			const auto CyclePolicy = Output.Type == EditorGraphPinType::Execution ? Schema.ExecutionCyclePolicy : Schema.DataCyclePolicy;
			if (OutputOwner != Index.Pins.end() && InputOwner != Index.Pins.end() &&
				OutputOwner->second.Node->ID == InputOwner->second.Node->ID &&
				(Output.Type != EditorGraphPinType::Execution || CyclePolicy == EditorGraphCyclePolicy::Reject))
				Add("connection.self", "A node cannot connect to itself in the current fixture schemas", Connection.ID);
			const size_t OutputCount = ++OutputCounts[Connection.OutputPin];
			const size_t InputCount = ++InputCounts[Connection.InputPin];
			if (OutputCount > 1 && Output.Cardinality == EditorGraphPinCardinality::Single)
				Add("connection.cardinality", "A single-cardinality output has multiple connections", Connection.ID);
			if (InputCount > 1 && Input.Cardinality == EditorGraphPinCardinality::Single)
				Add("connection.cardinality", "A single-cardinality input has multiple connections", Connection.ID);
			const auto PairKey = Connection.OutputPin.ToString() + ":" + Connection.InputPin.ToString();
			if (!EndpointPairs.insert(PairKey).second)
				Add("connection.duplicate", "Duplicate connections between the same endpoint pins are unsupported", Connection.ID);
			if (Schema.ConnectionPolicy && Output.Direction == EditorGraphPinDirection::Output &&
				Input.Direction == EditorGraphPinDirection::Input && Output.Type == Input.Type &&
				OutputOwner != Index.Pins.end() && InputOwner != Index.Pins.end())
			{
				const std::array Removed{ Connection.ID };
				EditorGraphConnectionPolicyContext Context;
				Context.Purpose = EditorGraphConnectionPolicyPurpose::StoredConnectionValidation;
				Context.Output = MakePolicyEndpoint(Index, Connection.OutputPin);
				Context.Input = MakePolicyEndpoint(Index, Connection.InputPin);
				Context.RemovedConnectionIDs = Removed;
				Context.ProposedConnectionID = Connection.ID;
				Context.ProposedConnectionIsStored = true;
				if (auto Reason = Schema.ConnectionPolicy(Graph, Context))
					Add("connection.schema", *Reason, Connection.ID);
			}
		}
		if (Schema.DataCyclePolicy == EditorGraphCyclePolicy::Reject && HasCycle(Graph, Index, false, Stats))
			Add("connection.cycle", "Material/data dependency cycle violates this schema policy", Graph.ID);
		if (Schema.ExecutionCyclePolicy == EditorGraphCyclePolicy::Reject && HasCycle(Graph, Index, true, Stats))
			Add("connection.cycle", "Gameplay execution cycle violates this schema policy", Graph.ID);
		const auto RerouteTopology = InspectRerouteTopology(Graph);
		if (RerouteTopology.OnlyCycle)
			Add("connection.reroute_cycle", "A reroute-only cycle has no real graph producer", Graph.ID);
		if (!RerouteTopology.SingleOutputFanout.IsNil())
			Add("connection.reroute_cardinality", "A single-cardinality output cannot fan out through reroutes",
				RerouteTopology.SingleOutputFanout);
		if (Schema.GraphValidationPolicy)
			if (auto Diagnostic = Schema.GraphValidationPolicy(Graph))
				Diagnostics.push_back(std::move(*Diagnostic));
		return Diagnostics;
	}

	EditorGraphConnectionDecision CanConnectEditorGraphPins(const EditorGraph& Graph, const EditorGraphSchema& Schema,
		PulseForge::UUID FirstPin, PulseForge::UUID SecondPin, std::optional<PulseForge::UUID> ReplacingConnection,
		EditorGraphWorkStats* Stats)
	{
		if (Stats) *Stats = {};
		const GraphIndex Index = BuildGraphIndex(Graph, Stats, ReplacingConnection);
		const auto AIt = Index.Pins.find(FirstPin);
		const auto BIt = Index.Pins.find(SecondPin);
		const auto* NodeA = AIt == Index.Pins.end() ? nullptr : AIt->second.Node;
		const auto* NodeB = BIt == Index.Pins.end() ? nullptr : BIt->second.Node;
		const auto* A = AIt == Index.Pins.end() ? nullptr : AIt->second.Pin;
		const auto* B = BIt == Index.Pins.end() ? nullptr : BIt->second.Pin;
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
		if (ReplacingConnection && std::none_of(Graph.Connections.begin(), Graph.Connections.end(),
			[ReplacingConnection](const auto& Existing) { return Existing.ID == *ReplacingConnection; }))
			return { false, "The connection being replaced no longer exists", OutputID, InputID };
		const auto CyclePolicy = Output->Type == EditorGraphPinType::Execution ? Schema.ExecutionCyclePolicy : Schema.DataCyclePolicy;
		if (OutputNode->ID == InputNode->ID &&
			(Output->Type != EditorGraphPinType::Execution || CyclePolicy == EditorGraphCyclePolicy::Reject))
			return { false, "A node cannot connect to itself in this fixture schema", OutputID, InputID };
		std::vector<PulseForge::UUID> RemovedConnectionIDs;
		if (ReplacingConnection) RemovedConnectionIDs.push_back(*ReplacingConnection);
		for (const auto& Existing : Graph.Connections)
			if (!IsRemoved(RemovedConnectionIDs, Existing.ID) && Existing.OutputPin == OutputID && Existing.InputPin == InputID)
				return { false, "These pins are already connected", OutputID, InputID };
		AppendOccupiedInputReplacements(Index, InputID,
			Input->Cardinality == EditorGraphPinCardinality::Single, Schema.ReplaceOccupiedInput, RemovedConnectionIDs);
		if (Output->Cardinality == EditorGraphPinCardinality::Single)
			for (const auto& Existing : Graph.Connections)
				if (Existing.OutputPin == OutputID && !IsRemoved(RemovedConnectionIDs, Existing.ID))
					return { false, "This output already has a connection and does not allow fan-out", OutputID, InputID };
		if (Input->Cardinality == EditorGraphPinCardinality::Single && !Schema.ReplaceOccupiedInput)
		{
			for (const auto& Existing : Graph.Connections)
				if (Existing.InputPin == InputID && !IsRemoved(RemovedConnectionIDs, Existing.ID))
					return { false, "This input already has a connection; disconnect it before creating another", OutputID, InputID };
		}
		const size_t EffectiveConnectionCount = Graph.Connections.size() - RemovedConnectionIDs.size();
		if (EffectiveConnectionCount >= MaximumConnections)
			return { false, "The graph has reached its supported connection limit", OutputID, InputID };
		if (Schema.ConnectionPolicy)
		{
			EditorGraphConnectionPolicyContext Context;
			Context.Purpose = EditorGraphConnectionPolicyPurpose::ProposedOperation;
			Context.Output = MakePolicyEndpoint(Index, OutputID);
			Context.Input = MakePolicyEndpoint(Index, InputID);
			Context.RemovedConnectionIDs = RemovedConnectionIDs;
			Context.ProposedConnectionID = ReplacingConnection;
			Context.ProposedConnectionIsStored = false;
			if (auto Reason = Schema.ConnectionPolicy(Graph, Context))
				return { false, std::move(*Reason), OutputID, InputID };
		}
		const auto ReplacedInput = Input->Cardinality == EditorGraphPinCardinality::Single && Schema.ReplaceOccupiedInput
			? std::optional<PulseForge::UUID>{ InputID } : std::nullopt;
		if (CyclePolicy == EditorGraphCyclePolicy::Reject &&
			HasPath(Index, InputNode->ID, OutputNode->ID, Output->Type == EditorGraphPinType::Execution,
				ReplacedInput, Stats))
			return { false, "This connection would create a cycle rejected by the active schema", OutputID, InputID };
		const auto RerouteTopology = InspectRerouteTopology(Graph, RemovedConnectionIDs,
			std::pair{ OutputID, InputID });
		if (RerouteTopology.OnlyCycle)
			return { false, "This connection would create a reroute-only cycle with no real graph producer", OutputID, InputID };
		if (!RerouteTopology.SingleOutputFanout.IsNil())
			return { false, "This connection would exceed a single-cardinality output through a reroute", OutputID, InputID };
		return { true, "Connection is valid", OutputID, InputID, std::move(RemovedConnectionIDs) };
	}

	std::vector<EditorGraphNodeCreationAction> DiscoverEditorGraphNodeActions(const EditorGraph& Graph,
		const EditorGraphSchema& Schema, std::optional<PulseForge::UUID> SourcePinID, EditorGraphWorkStats* Stats)
	{
		if (Stats) *Stats = {};
		std::vector<EditorGraphNodeCreationAction> Actions;
		if (Graph.SchemaKey != Schema.Key || Graph.SchemaVersion != Schema.Version) return Actions;
		if (Graph.Nodes.size() >= MaximumNodes) return Actions;
		GraphIndex Index;
		const EditorGraphPin* SourcePin = nullptr;
		const EditorGraphNode* SourceNode = nullptr;
		if (SourcePinID)
		{
			Index = BuildGraphIndex(Graph, Stats);
			const auto Source = Index.Pins.find(*SourcePinID);
			if (Source == Index.Pins.end()) return Actions;
			SourcePin = Source->second.Pin;
			SourceNode = Source->second.Node;
		}
		std::array<std::optional<RerouteTopologyIssue>, 4> TopologyResults;
		const auto GetTopology = [&](bool CandidateIsOutput, EditorGraphPinCardinality CandidateCardinality,
			std::span<const PulseForge::UUID> RemovedConnectionIDs) -> const RerouteTopologyIssue&
		{
			const size_t CacheIndex = (CandidateIsOutput ? 2u : 0u) +
				(CandidateCardinality == EditorGraphPinCardinality::Single ? 1u : 0u);
			if (!TopologyResults[CacheIndex])
			{
				const auto ProposedTopology = CandidateIsOutput
					? std::pair{ PulseForge::UUID{}, *SourcePinID }
					: std::pair{ *SourcePinID, PulseForge::UUID{} };
				TopologyResults[CacheIndex] = InspectRerouteTopology(Graph, RemovedConnectionIDs, ProposedTopology,
					CandidateIsOutput ? std::optional{ CandidateCardinality } : std::nullopt, Stats);
			}
			return *TopologyResults[CacheIndex];
		};

		for (const auto& Definition : Schema.Nodes)
		{
			if (!SourcePinID)
			{
				Actions.push_back({ Definition.TypeKey, Definition.Label, std::nullopt, {} });
				continue;
			}

			for (const auto& Pin : Definition.Pins)
			{
				if (Pin.Direction == SourcePin->Direction || Pin.Type != SourcePin->Type) continue;
				const auto* SourceNodeDefinition = FindDefinition(Schema, SourceNode->TypeKey);
				const auto* SourceDefinition = SourceNodeDefinition
					? FindPinDefinition(*SourceNodeDefinition, SourcePin->Key) : nullptr;
				if (!SourceDefinition) continue;
				const bool CandidateIsOutput = Pin.Direction == EditorGraphPinDirection::Output;
				const auto& OutputDefinition = CandidateIsOutput ? Pin : *SourceDefinition;
				const auto& InputDefinition = CandidateIsOutput ? *SourceDefinition : Pin;
				std::vector<PulseForge::UUID> RemovedConnectionIDs;
				if (CandidateIsOutput && SourcePin->Direction == EditorGraphPinDirection::Input)
					AppendOccupiedInputReplacements(Index, *SourcePinID,
						SourcePin->Cardinality == EditorGraphPinCardinality::Single, Schema.ReplaceOccupiedInput,
						RemovedConnectionIDs);
				if (SourcePin->Cardinality == EditorGraphPinCardinality::Single)
				{
					const bool IsOccupied = Index.IncidentCounts.contains(*SourcePinID) && Index.IncidentCounts.at(*SourcePinID) != 0;
					if (IsOccupied && !(SourcePin->Direction == EditorGraphPinDirection::Input && Schema.ReplaceOccupiedInput)) continue;
				}
				if (Graph.Connections.size() - RemovedConnectionIDs.size() >= MaximumConnections) continue;
				const auto& Topology = GetTopology(CandidateIsOutput, Pin.Cardinality, RemovedConnectionIDs);
				if (Topology.OnlyCycle || !Topology.SingleOutputFanout.IsNil()) continue;
				if (Schema.ConnectionPolicy)
				{
					EditorGraphConnectionPolicyContext Context;
					Context.Purpose = EditorGraphConnectionPolicyPurpose::NodeActionDiscovery;
					Context.Output = CandidateIsOutput
						? MakeProspectivePolicyEndpoint(Definition, Pin) : MakePolicyEndpoint(Index, *SourcePinID);
					Context.Input = CandidateIsOutput
						? MakePolicyEndpoint(Index, *SourcePinID) : MakeProspectivePolicyEndpoint(Definition, Pin);
					Context.RemovedConnectionIDs = RemovedConnectionIDs;
					if (Schema.ConnectionPolicy(Graph, Context)) continue;
				}
				Actions.push_back({ Definition.TypeKey, Definition.Label, Pin.Key, Pin.Label });
			}
		}

		std::sort(Actions.begin(), Actions.end(), [](const auto& Left, const auto& Right)
		{
			if (Left.NodeTypeKey != Right.NodeTypeKey) return Left.NodeTypeKey < Right.NodeTypeKey;
			return Left.ConnectPinKey.value_or(std::string{}) < Right.ConnectPinKey.value_or(std::string{});
		});
		return Actions;
	}

	std::vector<EditorGraphNodeCreationAction> SearchEditorGraphNodeActions(
		const std::vector<EditorGraphNodeCreationAction>& Actions, std::string_view Query)
	{
		const auto Fold = [](std::string_view Value)
		{
			std::string Result(Value);
			for (char& Character : Result)
				if (Character >= 'A' && Character <= 'Z') Character = static_cast<char>(Character - 'A' + 'a');
			return Result;
		};
		const std::string Needle = Fold(Query);
		const auto MatchScore = [&Needle, &Fold](const EditorGraphNodeCreationAction& Action) -> int
		{
			if (Needle.empty()) return 0;
			const std::string Label = Fold(Action.NodeLabel);
			const std::string Type = Fold(Action.NodeTypeKey);
			const std::string PinLabel = Fold(Action.ConnectPinLabel);
			const std::string PinKey = Action.ConnectPinKey ? Fold(*Action.ConnectPinKey) : std::string{};
			if (Label == Needle) return 0;
			if (Type == Needle) return 1;
			if (Label.starts_with(Needle)) return 2;
			if (Type.starts_with(Needle)) return 3;
			if (Label.find(Needle) != std::string::npos) return 4;
			if (Type.find(Needle) != std::string::npos) return 5;
			if (!PinLabel.empty() && PinLabel.find(Needle) != std::string::npos) return 6;
			if (!PinKey.empty() && PinKey.find(Needle) != std::string::npos) return 7;
			return std::numeric_limits<int>::max();
		};
		std::vector<std::pair<int, EditorGraphNodeCreationAction>> Ranked;
		for (const auto& Action : Actions)
		{
			const int Score = MatchScore(Action);
			if (Score != std::numeric_limits<int>::max()) Ranked.emplace_back(Score, Action);
		}
		std::sort(Ranked.begin(), Ranked.end(), [&Fold](const auto& Left, const auto& Right)
		{
			if (Left.first != Right.first) return Left.first < Right.first;
			const std::string LeftLabel = Fold(Left.second.NodeLabel);
			const std::string RightLabel = Fold(Right.second.NodeLabel);
			if (LeftLabel != RightLabel) return LeftLabel < RightLabel;
			if (Left.second.NodeTypeKey != Right.second.NodeTypeKey)
				return Left.second.NodeTypeKey < Right.second.NodeTypeKey;
			return Left.second.ConnectPinKey.value_or(std::string{}) < Right.second.ConnectPinKey.value_or(std::string{});
		});
		std::vector<EditorGraphNodeCreationAction> Results;
		Results.reserve(Ranked.size());
		for (auto& [Score, Action] : Ranked)
		{
			(void)Score;
			Results.push_back(std::move(Action));
		}
		return Results;
	}

	std::expected<std::string, std::string> SerializeEditorGraph(const EditorGraph& Graph, const EditorGraphSchema& Schema)
	{
		if (!IsValidPersistedSchemaText(Schema))
			return std::unexpected("Graph schema keys and labels must contain valid UTF-8");
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
		try
		{
			const std::string Text = Document.dump(2) + "\n";
			if (Text.size() > MaximumSourceBytes)
				return std::unexpected("Serialized graph exceeds the 16 MiB supported source limit");
			return Text;
		}
		catch (const Json::exception& Error)
		{
			return std::unexpected(std::string("Graph source cannot be encoded as valid UTF-8 JSON: ") + Error.what());
		}
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
		if (!IsValidUtf8(Clipboard.SchemaKey))
			return std::unexpected("Graph clipboard schema key must contain valid UTF-8");
		for (const auto& Node : Clipboard.Nodes)
		{
			if (!IsValidUtf8(Node.TypeKey)) return std::unexpected("Graph clipboard node type key must contain valid UTF-8");
			for (const auto& PinValue : Node.Pins)
			{
				if (!IsValidUtf8(PinValue.Key)) return std::unexpected("Graph clipboard pin key must contain valid UTF-8");
				if (std::holds_alternative<std::string>(PinValue.DefaultValue) &&
					(!IsValidUtf8(std::get<std::string>(PinValue.DefaultValue)) ||
						std::get<std::string>(PinValue.DefaultValue).size() > 4096))
					return std::unexpected("Graph clipboard string defaults must be valid UTF-8 and at most 4096 bytes");
			}
		}
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
		try
		{
			const std::string Text = Json{ { "format", "PulseForgeGraphClipboard" }, { "version", 1 },
				{ "schema", Clipboard.SchemaKey }, { "schemaVersion", Clipboard.SchemaVersion },
				{ "nodes", std::move(NodeRecords) }, { "connections", std::move(ConnectionRecords) } }.dump();
			if (Text.size() > MaximumClipboardBytes) return std::unexpected("Graph clipboard payload exceeds the supported size limit");
			return Text;
		}
		catch (const Json::exception& Error)
		{
			return std::unexpected(std::string("Graph clipboard cannot be encoded as valid UTF-8 JSON: ") + Error.what());
		}
	}

	std::expected<std::vector<PulseForge::UUID>, std::string> PasteEditorGraphClipboard(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view Source, double TargetGroupOriginX, double TargetGroupOriginY)
	{
		if (Source.size() > MaximumClipboardBytes || !IsFiniteCoordinate(TargetGroupOriginX) ||
			!IsFiniteCoordinate(TargetGroupOriginY))
			return std::unexpected("Graph clipboard payload or paste placement target is invalid");
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
			double MinimumX = Parsed.Nodes.front().X;
			double MinimumY = Parsed.Nodes.front().Y;
			for (const auto& Node : Parsed.Nodes)
			{
				MinimumX = std::min(MinimumX, Node.X);
				MinimumY = std::min(MinimumY, Node.Y);
			}
			const double DeltaX = std::floor(TargetGroupOriginX) - MinimumX;
			const double DeltaY = std::floor(TargetGroupOriginY) - MinimumY;
			std::unordered_map<PulseForge::UUID, PulseForge::UUID, PulseForge::UUIDHash> PinRemapped;
			std::vector<PulseForge::UUID> CreatedNodeIDs;
			CreatedNodeIDs.reserve(Parsed.Nodes.size());
			for (auto& Node : Parsed.Nodes)
			{
				auto NewNode = NewID(); if (!NewNode) return std::unexpected(NewNode.error());
				Node.ID = *NewNode; Node.X += DeltaX; Node.Y += DeltaY;
				CreatedNodeIDs.push_back(Node.ID);
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
			return CreatedNodeIDs;
		}
		catch (const Json::exception& Error)
		{
			return std::unexpected(std::string("Graph clipboard is malformed: ") + Error.what());
		}
	}

	std::expected<std::vector<PulseForge::UUID>, std::string> DuplicateEditorGraphSubgraph(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		const std::vector<PulseForge::UUID>& NodeIDs, double TargetGroupOriginX, double TargetGroupOriginY)
	{
		if (NodeIDs.empty()) return std::vector<PulseForge::UUID>{};
		if (!IsFiniteCoordinate(TargetGroupOriginX) || !IsFiniteCoordinate(TargetGroupOriginY))
			return std::unexpected("Graph duplicate placement target is invalid");
		const auto Clipboard = CopyEditorGraphSubgraph(History.Graph(), NodeIDs);
		if (Clipboard.Nodes.empty()) return std::vector<PulseForge::UUID>{};
		const auto Payload = SerializeEditorGraphClipboard(Clipboard);
		if (!Payload) return std::unexpected(Payload.error());
		return PasteEditorGraphClipboard(History, Schema, *Payload, TargetGroupOriginX, TargetGroupOriginY);
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

	std::expected<EditorGraphNodeCreationResult, std::string> CreateEditorGraphNodeAndConnect(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view TypeKey, std::string_view NewNodePinKey,
		PulseForge::UUID SourcePinID, double X, double Y)
	{
		if (SourcePinID.IsNil()) return std::unexpected("A source pin is required for automatic connection");
		auto Node = CreateEditorGraphNode(Schema, TypeKey, X, Y);
		if (!Node) return std::unexpected(Node.error());
		const auto NewPin = std::find_if(Node->Pins.begin(), Node->Pins.end(), [NewNodePinKey](const auto& Pin)
			{ return Pin.Key == NewNodePinKey; });
		if (NewPin == Node->Pins.end()) return std::unexpected("The selected node pin no longer exists in the active schema");
		auto ConnectionID = NewID();
		if (!ConnectionID) return std::unexpected(ConnectionID.error());
		const EditorGraphNodeCreationResult Result{ Node->ID, NewPin->ID, *ConnectionID };
		std::string Error;
		if (!History.Execute(Schema,
			[Node = std::move(*Node), SourcePinID, NewPinID = Result.ConnectedPinID.value(),
				ConnectionID = Result.ConnectionID.value(), &Schema](EditorGraph& Graph) mutable -> std::expected<void, std::string>
			{
				Graph.Nodes.push_back(std::move(Node));
				const auto Decision = CanConnectEditorGraphPins(Graph, Schema, SourcePinID, NewPinID);
				if (!Decision.Allowed) return std::unexpected(Decision.Reason);
				Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(),
					[&Decision](const auto& Existing) { return IsRemoved(Decision.RemovedConnectionIDs, Existing.ID); }),
					Graph.Connections.end());
				Graph.Connections.push_back({ ConnectionID, Decision.OutputPin, Decision.InputPin });
				return {};
			}, Error))
			return std::unexpected(Error);
		return Result;
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
		auto ID = NewID(); if (!ID) return std::unexpected(ID.error());
		std::string Error;
		if (!History.Execute(Schema, [&Schema, FirstPin, SecondPin, ConnectionID = *ID](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			const auto Decision = CanConnectEditorGraphPins(Graph, Schema, FirstPin, SecondPin);
			if (!Decision.Allowed) return std::unexpected(Decision.Reason);
			Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(), [&Decision](const auto& Existing)
				{ return IsRemoved(Decision.RemovedConnectionIDs, Existing.ID); }), Graph.Connections.end());
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

	std::expected<void, std::string> DisconnectEditorGraphPin(EditorGraphCommandHistory& History,
		const EditorGraphSchema& Schema, PulseForge::UUID PinID)
	{
		const auto PinExists = std::any_of(History.Graph().Nodes.begin(), History.Graph().Nodes.end(),
			[PinID](const auto& Node)
				{ return std::any_of(Node.Pins.begin(), Node.Pins.end(), [PinID](const auto& Pin) { return Pin.ID == PinID; }); });
		if (!PinExists) return std::unexpected("The graph pin no longer exists");
		const auto HasConnections = std::any_of(History.Graph().Connections.begin(), History.Graph().Connections.end(),
			[PinID](const auto& Connection) { return Connection.OutputPin == PinID || Connection.InputPin == PinID; });
		if (!HasConnections) return {};
		std::string Error;
		if (!History.Execute(Schema, [PinID](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(),
				[PinID](const auto& Connection) { return Connection.OutputPin == PinID || Connection.InputPin == PinID; }),
				Graph.Connections.end());
			return {};
		}, Error)) return std::unexpected(Error);
		return {};
	}

	std::expected<void, std::string> ReconnectEditorGraphConnection(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID ConnectionID, PulseForge::UUID FirstPin, PulseForge::UUID SecondPin)
	{
		std::string Error;
		if (!History.Execute(Schema, [&Schema, ConnectionID, FirstPin, SecondPin](EditorGraph& Graph) -> std::expected<void, std::string>
		{
			auto It = std::find_if(Graph.Connections.begin(), Graph.Connections.end(), [ConnectionID](const auto& Item) { return Item.ID == ConnectionID; });
			if (It == Graph.Connections.end()) return std::unexpected("The graph connection changed before reconnection");
			const auto Decision = CanConnectEditorGraphPins(Graph, Schema, FirstPin, SecondPin, ConnectionID);
			if (!Decision.Allowed) return std::unexpected(Decision.Reason);
			Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(), [&Decision, ConnectionID](const auto& Item)
				{ return Item.ID != ConnectionID && IsRemoved(Decision.RemovedConnectionIDs, Item.ID); }), Graph.Connections.end());
			It = std::find_if(Graph.Connections.begin(), Graph.Connections.end(), [ConnectionID](const auto& Item) { return Item.ID == ConnectionID; });
			if (It == Graph.Connections.end()) return std::unexpected("The connection being reconnected was unexpectedly removed");
			It->OutputPin = Decision.OutputPin;
			It->InputPin = Decision.InputPin;
			return {};
		}, Error)) return std::unexpected(Error);
		return {};
	}

	EditorGraphPinMoveDecision CanMoveEditorGraphPinConnections(const EditorGraph& Graph,
		const EditorGraphSchema& Schema, PulseForge::UUID SourcePin, PulseForge::UUID TargetPin,
		std::span<const EditorGraphConnection> CapturedConnections, EditorGraphWorkStats* Stats,
		bool RequireCompleteSourceSet)
	{
		if (Stats) *Stats = {};
		if (Graph.SchemaKey != Schema.Key || Graph.SchemaVersion != Schema.Version)
			return { false, false, "The active graph schema changed", {}, {} };
		const GraphIndex Index = BuildGraphIndex(Graph, Stats);
		const auto Source = Index.Pins.find(SourcePin);
		const auto Target = Index.Pins.find(TargetPin);
		if (Source == Index.Pins.end() || Target == Index.Pins.end())
			return { false, false, "The source or destination pin no longer exists", {}, {} };
		if (Source->second.Pin->Direction != Target->second.Pin->Direction)
			return { false, false, "Move a pin to another pin with the same direction", {}, {} };
		if (CapturedConnections.empty())
			return { false, false, "The source pin has no connections to move", {}, {} };

		std::unordered_map<PulseForge::UUID, EditorGraphConnection, PulseForge::UUIDHash> CapturedByID;
		for (const auto& Connection : CapturedConnections)
			if (Connection.ID.IsNil() || !CapturedByID.emplace(Connection.ID, Connection).second)
				return { false, false, "The captured connection set is invalid", {}, {} };
		std::vector<EditorGraphConnection> CurrentConnections;
		for (const auto& Connection : Graph.Connections)
			if (Connection.OutputPin == SourcePin || Connection.InputPin == SourcePin)
				CurrentConnections.push_back(Connection);
		if ((RequireCompleteSourceSet && CurrentConnections.size() != CapturedByID.size()) ||
			std::any_of(CurrentConnections.begin(), CurrentConnections.end(), [&CapturedByID](const auto& Connection)
			{
				const auto Captured = CapturedByID.find(Connection.ID);
				return Captured != CapturedByID.end() && Captured->second != Connection;
			}))
			return { false, false, "The source pin's connection set changed during this gesture", {}, {} };
		for (const auto& [ConnectionID, Captured] : CapturedByID)
			if (std::none_of(CurrentConnections.begin(), CurrentConnections.end(), [ConnectionID, &Captured](const auto& Connection)
				{ return Connection.ID == ConnectionID && Connection == Captured; }))
				return { false, false, "The selected connection changed during this gesture", {}, {} };

		std::vector<PulseForge::UUID> MovedIDs;
		MovedIDs.reserve(CapturedByID.size());
		for (const auto& [ConnectionID, Connection] : CapturedByID)
		{
			(void)Connection;
			MovedIDs.push_back(ConnectionID);
		}
		std::sort(MovedIDs.begin(), MovedIDs.end());
		if (SourcePin == TargetPin)
			return { true, true, "Release on a different pin to move these connections", std::move(MovedIDs), {} };

		EditorGraph Candidate = Graph;
		std::vector<PulseForge::UUID> RemovedIDs = MovedIDs;
		std::vector<PulseForge::UUID> DisplacedIDs;
		if (Target->second.Pin->Direction == EditorGraphPinDirection::Input &&
			Target->second.Pin->Cardinality == EditorGraphPinCardinality::Single && Schema.ReplaceOccupiedInput)
		{
			for (const auto& Connection : Graph.Connections)
				if (Connection.InputPin == TargetPin && !CapturedByID.contains(Connection.ID))
				{
					DisplacedIDs.push_back(Connection.ID);
					RemovedIDs.push_back(Connection.ID);
				}
		}

		std::vector<EditorGraphConnectionPolicyContext::ProposedConnection> Proposals;
		Proposals.reserve(CapturedConnections.size());
		for (const auto& Original : CapturedConnections)
		{
			auto Proposed = Original;
			if (Source->second.Pin->Direction == EditorGraphPinDirection::Output)
				Proposed.OutputPin = TargetPin;
			else
				Proposed.InputPin = TargetPin;
			const auto Output = Index.Pins.find(Proposed.OutputPin);
			const auto Input = Index.Pins.find(Proposed.InputPin);
			if (Output == Index.Pins.end() || Input == Index.Pins.end())
				return { false, false, "A connection endpoint no longer exists", {}, {} };
			if (Output->second.Pin->Direction != EditorGraphPinDirection::Output ||
				Input->second.Pin->Direction != EditorGraphPinDirection::Input)
				return { false, false, "Move a pin to another pin with the same direction", {}, {} };
			if (Output->second.Pin->Type != Input->second.Pin->Type)
				return { false, false, "Pin types must match exactly; implicit conversions are not supported", {}, {} };
			Proposals.push_back({ Proposed.ID, MakePolicyEndpoint(Index, Proposed.OutputPin),
				MakePolicyEndpoint(Index, Proposed.InputPin) });
			auto Live = std::find_if(Candidate.Connections.begin(), Candidate.Connections.end(), [&Original](const auto& Item)
				{ return Item.ID == Original.ID; });
			if (Live == Candidate.Connections.end())
				return { false, false, "The captured connection disappeared before preview", {}, {} };
			*Live = Proposed;
		}
		if (!DisplacedIDs.empty())
			Candidate.Connections.erase(std::remove_if(Candidate.Connections.begin(), Candidate.Connections.end(),
				[&DisplacedIDs](const auto& Connection) { return IsRemoved(DisplacedIDs, Connection.ID); }),
				Candidate.Connections.end());

		if (Schema.ConnectionPolicy)
			for (const auto& Proposal : Proposals)
			{
				EditorGraphConnectionPolicyContext Context;
				Context.Purpose = EditorGraphConnectionPolicyPurpose::ProposedOperation;
				Context.Output = Proposal.Output;
				Context.Input = Proposal.Input;
				Context.RemovedConnectionIDs = RemovedIDs;
				Context.ProposedConnections = Proposals;
				Context.ProposedConnectionID = Proposal.ConnectionID;
				if (auto Reason = Schema.ConnectionPolicy(Graph, Context))
					return { false, false, std::move(*Reason), {}, {} };
			}

		const auto Diagnostics = ValidateEditorGraph(Candidate, Schema);
		if (!Diagnostics.empty())
			return { false, false, Diagnostics.front().Message, {}, {} };
		return { true, false, "All attached connections can move to this pin", std::move(MovedIDs),
			std::move(DisplacedIDs) };
	}

	std::expected<void, std::string> MoveEditorGraphPinConnections(EditorGraphCommandHistory& History,
		const EditorGraphSchema& Schema, PulseForge::UUID SourcePin, PulseForge::UUID TargetPin,
		std::span<const EditorGraphConnection> CapturedConnections, bool RequireCompleteSourceSet)
	{
		const std::vector<EditorGraphConnection> Captured(CapturedConnections.begin(), CapturedConnections.end());
		std::string Error;
		if (!History.Execute(Schema, [&Schema, SourcePin, TargetPin, Captured, RequireCompleteSourceSet](EditorGraph& Graph)
			-> std::expected<void, std::string>
		{
			const auto Decision = CanMoveEditorGraphPinConnections(Graph, Schema, SourcePin, TargetPin,
				Captured, nullptr, RequireCompleteSourceSet);
			if (!Decision.Allowed) return std::unexpected(Decision.Reason);
			if (Decision.NoOp) return {};
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Moved(Decision.MovedConnectionIDs.begin(),
				Decision.MovedConnectionIDs.end());
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Displaced(
				Decision.DisplacedConnectionIDs.begin(), Decision.DisplacedConnectionIDs.end());
			for (auto& Connection : Graph.Connections)
				if (Moved.contains(Connection.ID))
				{
					if (Connection.OutputPin == SourcePin) Connection.OutputPin = TargetPin;
					else if (Connection.InputPin == SourcePin) Connection.InputPin = TargetPin;
					else return std::unexpected("The captured source pin is no longer an endpoint of its connection");
				}
			Graph.Connections.erase(std::remove_if(Graph.Connections.begin(), Graph.Connections.end(),
				[&Displaced](const auto& Connection) { return Displaced.contains(Connection.ID); }), Graph.Connections.end());
			return {};
		}, Error))
			return std::unexpected(Error);
		return {};
	}

	std::expected<PulseForge::UUID, std::string> InsertEditorGraphReroute(EditorGraphCommandHistory& History,
		const EditorGraphSchema& Schema, PulseForge::UUID ConnectionID, double X, double Y)
	{
		const auto& Before = History.Graph();
		const auto Existing = std::find_if(Before.Connections.begin(), Before.Connections.end(),
			[ConnectionID](const auto& Connection) { return Connection.ID == ConnectionID; });
		if (Existing == Before.Connections.end()) return std::unexpected("The selected wire no longer exists");
		if (!IsFiniteCoordinate(X) || !IsFiniteCoordinate(Y)) return std::unexpected("Reroute position is outside the supported range");
		if ((Schema.ConnectionPolicy || Schema.GraphValidationPolicy) && !Schema.ReroutePolicyAware)
			return std::unexpected("This schema has custom connection rules but has not declared transparent reroute semantics");
		const EditorGraphPin* Output = nullptr;
		const EditorGraphPin* Input = nullptr;
		for (const auto& Node : Before.Nodes)
			for (const auto& PinValue : Node.Pins)
			{
				if (PinValue.ID == Existing->OutputPin) Output = &PinValue;
				if (PinValue.ID == Existing->InputPin) Input = &PinValue;
			}
		if (!Output || !Input || Output->Type != Input->Type || Output->Direction != EditorGraphPinDirection::Output ||
			Input->Direction != EditorGraphPinDirection::Input)
			return std::unexpected("The selected wire endpoints are invalid");
		if (std::find(Schema.RerouteTypes.begin(), Schema.RerouteTypes.end(), Output->Type) == Schema.RerouteTypes.end())
			return std::unexpected("This graph schema does not support reroutes for the selected wire type");
		const auto* RerouteDefinition = FindDefinition(Schema, GraphRerouteTypeKey(Output->Type));
		if (!RerouteDefinition) return std::unexpected("The schema does not provide a compatible reroute definition");
		auto RerouteResult = CreateEditorGraphNode(Schema, RerouteDefinition->TypeKey, X, Y);
		if (!RerouteResult) return std::unexpected(RerouteResult.error());
		EditorGraphNode Reroute = std::move(*RerouteResult);
		const auto RerouteInput = std::find_if(Reroute.Pins.begin(), Reroute.Pins.end(),
			[](const auto& PinValue) { return PinValue.Key == "in"; });
		const auto RerouteOutput = std::find_if(Reroute.Pins.begin(), Reroute.Pins.end(),
			[](const auto& PinValue) { return PinValue.Key == "out"; });
		if (RerouteInput == Reroute.Pins.end() || RerouteOutput == Reroute.Pins.end())
			return std::unexpected("The reroute schema definition is incomplete");
		auto UpstreamID = PulseForge::UUID::Generate();
		if (!UpstreamID) return std::unexpected(UpstreamID.error().Message);
		const PulseForge::UUID RerouteID = Reroute.ID;
		const PulseForge::UUID RerouteInputID = RerouteInput->ID;
		const PulseForge::UUID RerouteOutputID = RerouteOutput->ID;
		const PulseForge::UUID OriginalOutput = Existing->OutputPin;
		const PulseForge::UUID OriginalInput = Existing->InputPin;
		std::string Error;
		if (!History.Execute(Schema, [=, Reroute = std::move(Reroute)](EditorGraph& Graph) mutable
			-> std::expected<void, std::string>
		{
			auto Link = std::find_if(Graph.Connections.begin(), Graph.Connections.end(),
				[ConnectionID](const auto& Candidate) { return Candidate.ID == ConnectionID; });
			if (Link == Graph.Connections.end() || Link->OutputPin != OriginalOutput || Link->InputPin != OriginalInput)
				return std::unexpected("The selected wire changed before reroute insertion");
			if (Graph.Nodes.size() >= EditorGraphMaximumNodes || Graph.Connections.size() >= EditorGraphMaximumConnections)
				return std::unexpected("The graph has no capacity for a reroute insertion");
			Link->OutputPin = RerouteOutputID;
			Graph.Nodes.push_back(std::move(Reroute));
			Graph.Connections.push_back({ *UpstreamID, OriginalOutput, RerouteInputID });
			return {};
		}, Error)) return std::unexpected(Error);
		return RerouteID;
	}

	namespace
	{
		EditorGraphWireRouteDecision BuildEditorGraphWireRoute(EditorGraph& Candidate, const EditorGraphSchema& Schema,
			PulseForge::UUID StartPinID, std::span<const EditorGraphVector3> KnotPositions,
			std::optional<PulseForge::UUID> EndPinID, std::vector<EditorGraph>* HistorySteps = nullptr,
			EditorGraphWorkStats* Stats = nullptr, EditorGraphWireRouteAdmission* Admission = nullptr,
			const EditorGraphCommandHistory* History = nullptr)
		{
			EditorGraphWireRouteDecision Result;
			Result.OpenEnded = !EndPinID.has_value();
			if (HistorySteps) HistorySteps->clear();
			const auto Fail = [&Result](std::string Reason)
			{
				Result.Allowed = false;
				Result.Reason = std::move(Reason);
				return Result;
			};
			if (KnotPositions.empty()) return Fail("Place at least one reroute before completing a graph route");
			const size_t PlannedSteps = KnotPositions.size() + (EndPinID ? 1 : 0);
			EditorGraphWireRouteAdmission WorkingAdmission = Admission ? *Admission : EditorGraphWireRouteAdmission{};
			if (!WorkingAdmission.Initialized)
			{
				WorkingAdmission.TotalStateBytes = EstimateBytes(Candidate);
				WorkingAdmission.SmallestStateBytes = WorkingAdmission.TotalStateBytes;
				WorkingAdmission.Initialized = true;
			}
			if (WorkingAdmission.Steps > EditorGraphCommandHistory::MaximumSnapshots ||
				PlannedSteps > EditorGraphCommandHistory::MaximumSnapshots - WorkingAdmission.Steps)
				return Fail("The route needs more Undo steps than the history snapshot limit can retain");
			const size_t TotalPlannedSteps = WorkingAdmission.Steps + PlannedSteps;
			if (Candidate.SchemaKey != Schema.Key || Candidate.SchemaVersion != Schema.Version)
				return Fail("The graph does not match the active schema");
			if (KnotPositions.size() > EditorGraphMaximumNodes ||
				Candidate.Nodes.size() > EditorGraphMaximumNodes - KnotPositions.size())
				return Fail("The graph has no capacity for the requested reroute chain");
			if (HistorySteps && PlannedSteps > 0)
				HistorySteps->reserve(PlannedSteps);
			if ((Schema.ConnectionPolicy || Schema.GraphValidationPolicy) && !Schema.ReroutePolicyAware)
				return Fail("This schema has custom connection rules but has not declared transparent reroute semantics");
			for (const auto& Position : KnotPositions)
				if (!IsFiniteCoordinate(Position.X) || !IsFiniteCoordinate(Position.Y))
					return Fail("A reroute position is non-finite or outside the supported range");

			const EditorGraphPin* StartPin = nullptr;
			if (!FindNodeForPin(Candidate, StartPinID, &StartPin) || !StartPin)
				return Fail("The starting pin no longer exists");
			const EditorGraphPinDirection StartDirection = StartPin->Direction;
			const EditorGraphPinType RouteType = StartPin->Type;
			const auto RerouteType = GraphRerouteTypeKey(RouteType);
			if (std::find(Schema.RerouteTypes.begin(), Schema.RerouteTypes.end(), RouteType) == Schema.RerouteTypes.end() ||
				!FindDefinition(Schema, RerouteType))
				return Fail("The active graph schema does not support reroutes for this pin type");

			const EditorGraphPin* EndPin = nullptr;
			if (EndPinID)
			{
				if (*EndPinID == StartPinID || !FindNodeForPin(Candidate, *EndPinID, &EndPin) || !EndPin)
					return Fail("The destination pin no longer exists");
				if (EndPin->Direction == StartDirection)
					return Fail("A completed route must end at a pin with the opposite direction");
				if (EndPin->Type != RouteType)
					return Fail("The destination pin has an incompatible type for this reroute chain");
			}

			struct ReroutePins final { PulseForge::UUID Input; PulseForge::UUID Output; };
			std::vector<ReroutePins> Pins;
			std::vector<EditorGraphNode> RerouteNodes;
			Pins.reserve(KnotPositions.size());
			RerouteNodes.reserve(KnotPositions.size());
			Result.RerouteNodeIDs.reserve(KnotPositions.size());
			for (const auto& Position : KnotPositions)
			{
				auto Node = CreateEditorGraphNode(Schema, RerouteType, Position.X, Position.Y);
				if (!Node) return Fail(Node.error());
				const auto Input = std::find_if(Node->Pins.begin(), Node->Pins.end(),
					[](const auto& Pin) { return Pin.Key == "in" && Pin.Direction == EditorGraphPinDirection::Input; });
				const auto Output = std::find_if(Node->Pins.begin(), Node->Pins.end(),
					[](const auto& Pin) { return Pin.Key == "out" && Pin.Direction == EditorGraphPinDirection::Output; });
				if (Input == Node->Pins.end() || Output == Node->Pins.end() ||
					Input->Type != RouteType || Output->Type != RouteType)
					return Fail("The registered reroute definition does not preserve the route pin type");
				Pins.push_back({ Input->ID, Output->ID });
				Result.RerouteNodeIDs.push_back(Node->ID);
				RerouteNodes.push_back(std::move(*Node));
			}

			Result.ConnectionIDs.reserve(KnotPositions.size() + (EndPinID ? 1 : 0));
			size_t RouteStateBytes = WorkingAdmission.TotalStateBytes;
			size_t SmallestRouteStateBytes = WorkingAdmission.SmallestStateBytes;
			size_t CandidatePlanBytes = WorkingAdmission.PlannedSnapshotBytes;
			const auto RecordState = [&]() -> std::optional<std::string>
			{
				if (const auto Diagnostics = ValidateEditorGraph(Candidate, Schema); !Diagnostics.empty())
					return Diagnostics.front().Message;
				if (auto Writable = EnsureGraphSourceIsWritable(Candidate, Schema); !Writable)
					return Writable.error();
				if (Stats) ++Stats->RoutePrefixStatesValidated;
				const size_t StateBytes = EstimateBytes(Candidate);
				if (CandidatePlanBytes > MaximumRoutePreparationBytes ||
					StateBytes > MaximumRoutePreparationBytes - CandidatePlanBytes)
					return "The route's temporary planning snapshots exceed the bounded preparation budget";
				CandidatePlanBytes += StateBytes;
				if (StateBytes > std::numeric_limits<size_t>::max() - RouteStateBytes)
					return "The route's required Undo snapshots exceed the graph history memory budget";
				RouteStateBytes += StateBytes;
				SmallestRouteStateBytes = std::min(SmallestRouteStateBytes, StateBytes);
				if (RouteStateBytes - SmallestRouteStateBytes > EditorGraphCommandHistory::MaximumHistoryBytes)
					return "The route's required Undo snapshots exceed the graph history memory budget";
				if (History)
				{
					std::string AdmissionError;
					if (!History->CanAdmitRoute(TotalPlannedSteps,
						RouteStateBytes - SmallestRouteStateBytes,
						CandidatePlanBytes, EstimateBytes(Candidate), AdmissionError)) return AdmissionError;
				}
				if (HistorySteps) HistorySteps->push_back(Candidate);
				++WorkingAdmission.Steps;
				WorkingAdmission.TotalStateBytes = RouteStateBytes;
				WorkingAdmission.SmallestStateBytes = SmallestRouteStateBytes;
				WorkingAdmission.PlannedSnapshotBytes = CandidatePlanBytes;
				return std::nullopt;
			};
			size_t CurrentKnotIndex = 0;
			const auto AddSegment = [&](PulseForge::UUID OutputID, PulseForge::UUID InputID, bool IsFinalSegment = false)
				-> std::optional<std::string>
			{
				const auto Decision = CanConnectEditorGraphPins(Candidate, Schema, OutputID, InputID);
				if (!Decision.Allowed) return Decision.Reason;
				if (Result.OpenEnded && StartDirection == EditorGraphPinDirection::Input &&
					InputID == StartPinID && !Decision.RemovedConnectionIDs.empty())
					return "An unfinished route cannot replace an existing connection";
				for (const auto RemovedID : Decision.RemovedConnectionIDs)
				{
					if (std::find(Result.DisplacedConnectionIDs.begin(), Result.DisplacedConnectionIDs.end(), RemovedID) ==
						Result.DisplacedConnectionIDs.end()) Result.DisplacedConnectionIDs.push_back(RemovedID);
				}
				Candidate.Connections.erase(std::remove_if(Candidate.Connections.begin(), Candidate.Connections.end(),
					[&Decision](const auto& Connection)
					{ return std::find(Decision.RemovedConnectionIDs.begin(), Decision.RemovedConnectionIDs.end(),
						Connection.ID) != Decision.RemovedConnectionIDs.end(); }), Candidate.Connections.end());
				if (Candidate.Connections.size() >= EditorGraphMaximumConnections)
					return "The graph has no capacity for another route connection";
				auto ConnectionID = PulseForge::UUID::Generate();
				if (!ConnectionID) return ConnectionID.error().Message;
				Candidate.Connections.push_back({ *ConnectionID, OutputID, InputID });
				Result.ConnectionIDs.push_back(*ConnectionID);
				if (!IsFinalSegment)
				{
					const bool IsUndoSnapshot = EndPinID.has_value() || CurrentKnotIndex + 1 < KnotPositions.size();
					if (IsUndoSnapshot)
						if (const auto Error = RecordState()) return Error;
				}
				return std::nullopt;
			};

			for (size_t Index = 0; Index < Pins.size(); ++Index)
			{
				CurrentKnotIndex = Index;
				Candidate.Nodes.push_back(std::move(RerouteNodes[Index]));
				const PulseForge::UUID OutputID = StartDirection == EditorGraphPinDirection::Output
					? (Index == 0 ? StartPinID : Pins[Index - 1].Output) : Pins[Index].Output;
				const PulseForge::UUID InputID = StartDirection == EditorGraphPinDirection::Output
					? Pins[Index].Input : (Index == 0 ? StartPinID : Pins[Index - 1].Input);
				const bool IsOpenRouteFinalSegment = !EndPinID && Index + 1 == Pins.size();
				if (const auto Error = AddSegment(OutputID, InputID, IsOpenRouteFinalSegment)) return Fail(*Error);
			}
			if (EndPinID)
			{
				const PulseForge::UUID OutputID = StartDirection == EditorGraphPinDirection::Output
					? Pins.back().Output : *EndPinID;
				const PulseForge::UUID InputID = StartDirection == EditorGraphPinDirection::Output
					? *EndPinID : Pins.back().Input;
				if (const auto Error = AddSegment(OutputID, InputID, true)) return Fail(*Error);
			}
			if (const auto Error = RecordState()) return Fail(*Error);
			if (HistorySteps) HistorySteps->back() = std::move(Candidate);
			if (Admission) *Admission = WorkingAdmission;
			Result.Allowed = true;
			Result.Reason.clear();
			return Result;
		}
	}

	EditorGraphWireRouteDecision CanCreateEditorGraphWireRoute(const EditorGraph& Graph,
		const EditorGraphSchema& Schema, PulseForge::UUID StartPin,
		std::span<const EditorGraphVector3> KnotPositions, std::optional<PulseForge::UUID> EndPin,
		EditorGraph* PlannedGraph, EditorGraphWorkStats* Stats, EditorGraphWireRouteAdmission* Admission,
		const EditorGraphCommandHistory* History)
	{
		EditorGraph Candidate;
		try { Candidate = Graph; }
		catch (const std::bad_alloc&)
		{
			EditorGraphWireRouteDecision Failure;
			Failure.OpenEnded = !EndPin.has_value();
			Failure.Reason = "The route could not reserve its bounded planning graph";
			return Failure;
		}
		auto Result = BuildEditorGraphWireRoute(Candidate, Schema, StartPin, KnotPositions, EndPin,
			nullptr, Stats, Admission, History);
		if (!Result.Allowed) return Result;
		if (PlannedGraph) *PlannedGraph = std::move(Candidate);
		Result.Reason.clear();
		return Result;
	}

	std::expected<EditorGraphWireRouteDecision, std::string> CreateEditorGraphWireRoute(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema, PulseForge::UUID StartPin,
		std::span<const EditorGraphVector3> KnotPositions, std::optional<PulseForge::UUID> EndPin,
		EditorGraphWorkStats* Stats)
	{
		if (History.HasOpenTransaction())
			return std::unexpected("Cannot complete a wire route during another graph edit transaction");
		std::vector<EditorGraph> HistorySteps;
		HistorySteps.reserve(KnotPositions.size() + (EndPin ? 1 : 0));
		EditorGraph FinalCandidate = History.Graph();
		EditorGraphWireRouteDecision Result = BuildEditorGraphWireRoute(
			FinalCandidate, Schema, StartPin, KnotPositions, EndPin, &HistorySteps, Stats, nullptr, &History);
		if (!Result.Allowed) return std::unexpected(Result.Reason);
		if (HistorySteps.size() != KnotPositions.size() + (EndPin ? 1 : 0))
			return std::unexpected("The route did not produce the expected history steps");
		if (!History.ExecuteSequence(Schema, std::move(HistorySteps), Result.Reason))
			return std::unexpected(Result.Reason);
		return Result;
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
		if (ExistingPin->Type == EditorGraphPinType::String && std::holds_alternative<std::string>(Value) &&
			!IsValidUtf8(std::get<std::string>(Value)))
			return std::unexpected("String graph defaults must contain valid UTF-8");
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

	std::vector<std::pair<PulseForge::UUID, EditorGraphVector3>> CalculateEditorGraphStraightenedLayout(
		const std::vector<EditorGraphLayoutNode>& Nodes,
		const std::vector<EditorGraphPinAnchor>& PinAnchors,
		const std::vector<EditorGraphConnection>& Connections,
		const std::vector<PulseForge::UUID>& SelectedNodeIDs)
	{
		std::unordered_map<PulseForge::UUID, EditorGraphLayoutNode, PulseForge::UUIDHash> NodeByID;
		for (const auto& Node : Nodes)
			if (std::isfinite(Node.Position.X) && std::isfinite(Node.Position.Y)) NodeByID.emplace(Node.ID, Node);
		std::vector<PulseForge::UUID> Selected;
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> SelectedSet;
		for (const auto ID : SelectedNodeIDs)
			if (NodeByID.contains(ID) && SelectedSet.insert(ID).second) Selected.push_back(ID);
		std::sort(Selected.begin(), Selected.end(), [&NodeByID](const auto Left, const auto Right)
		{
			const auto& A = NodeByID.at(Left).Position;
			const auto& B = NodeByID.at(Right).Position;
			if (A.X != B.X) return A.X < B.X;
			if (A.Y != B.Y) return A.Y < B.Y;
			return Left < Right;
		});
		if (Selected.size() < 2) return {};

		std::unordered_map<PulseForge::UUID, double, PulseForge::UUIDHash> OffsetByPin;
		std::unordered_map<PulseForge::UUID, PulseForge::UUID, PulseForge::UUIDHash> NodeByPin;
		for (const auto& Anchor : PinAnchors)
			if (std::isfinite(Anchor.OffsetY) && SelectedSet.contains(Anchor.NodeID))
			{
				OffsetByPin.emplace(Anchor.PinID, Anchor.OffsetY);
				NodeByPin.emplace(Anchor.PinID, Anchor.NodeID);
			}

		struct Constraint { PulseForge::UUID Other; double Delta = 0.0; };
		std::unordered_map<PulseForge::UUID, std::vector<Constraint>, PulseForge::UUIDHash> Adjacency;
		for (const auto& Connection : Connections)
		{
			const auto OutputNode = NodeByPin.find(Connection.OutputPin);
			const auto InputNode = NodeByPin.find(Connection.InputPin);
			const auto OutputOffset = OffsetByPin.find(Connection.OutputPin);
			const auto InputOffset = OffsetByPin.find(Connection.InputPin);
			if (OutputNode == NodeByPin.end() || InputNode == NodeByPin.end() ||
				OutputOffset == OffsetByPin.end() || InputOffset == OffsetByPin.end() ||
				OutputNode->second == InputNode->second) continue;
			const double Delta = OutputOffset->second - InputOffset->second;
			Adjacency[OutputNode->second].push_back({ InputNode->second, Delta });
			Adjacency[InputNode->second].push_back({ OutputNode->second, -Delta });
		}
		for (auto& [ID, Edges] : Adjacency)
		{
			(void)ID;
			std::sort(Edges.begin(), Edges.end(), [](const auto& Left, const auto& Right)
			{
				if (Left.Other != Right.Other) return Left.Other < Right.Other;
				return Left.Delta < Right.Delta;
			});
		}

		bool HasInternalConnection = false;
		for (const auto ID : Selected)
			if (const auto It = Adjacency.find(ID); It != Adjacency.end() && !It->second.empty())
				HasInternalConnection = true;

		std::unordered_map<PulseForge::UUID, double, PulseForge::UUIDHash> TargetY;
		if (!HasInternalConnection)
		{
			const auto Reference = *std::min_element(Selected.begin(), Selected.end(), [&NodeByID](const auto Left, const auto Right)
			{
				const auto& A = NodeByID.at(Left).Position;
				const auto& B = NodeByID.at(Right).Position;
				if (A.Y != B.Y) return A.Y < B.Y;
				if (A.X != B.X) return A.X < B.X;
				return Left < Right;
			});
			const double RowY = NodeByID.at(Reference).Position.Y;
			for (const auto ID : Selected) TargetY.emplace(ID, RowY);
		}
		else
		{
			// Stable left-to-right roots and sorted edges make conflicting cycles a
			// bounded, deterministic best effort. First assigned constraint wins.
			for (const auto Root : Selected)
			{
				if (TargetY.contains(Root)) continue;
				TargetY.emplace(Root, NodeByID.at(Root).Position.Y);
				std::vector<PulseForge::UUID> Pending{ Root };
				for (size_t Index = 0; Index < Pending.size(); ++Index)
				{
					const auto Current = Pending[Index];
					const auto Edges = Adjacency.find(Current);
					if (Edges == Adjacency.end()) continue;
					for (const auto& Edge : Edges->second)
						if (!TargetY.contains(Edge.Other))
						{
							TargetY.emplace(Edge.Other, TargetY.at(Current) + Edge.Delta);
							Pending.push_back(Edge.Other);
						}
				}
			}
		}

		std::vector<std::pair<PulseForge::UUID, EditorGraphVector3>> Result;
		for (const auto ID : Selected)
		{
			const auto Position = NodeByID.at(ID).Position;
			const auto NewY = TargetY.at(ID);
			if (std::isfinite(NewY) && std::abs(NewY - Position.Y) > 0.01)
				Result.emplace_back(ID, EditorGraphVector3{ Position.X, NewY, Position.Z });
		}
		return Result;
	}
}
