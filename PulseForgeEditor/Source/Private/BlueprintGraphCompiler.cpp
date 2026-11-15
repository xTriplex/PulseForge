#include "Editor/BlueprintGraphCompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
	using namespace PulseForge;
	using namespace PulseForgeEditor;

	EditorGraphPinDefinition MakePin(std::string Key, std::string Label, EditorGraphPinDirection Direction,
		EditorGraphPinType Type, EditorGraphValue DefaultValue = {}, bool Required = false)
	{
		return { std::move(Key), std::move(Label), Direction, Type, EditorGraphPinCardinality::Single,
			std::move(DefaultValue), EditorGraphPinDefaultControl::WhenDisconnected, true, Required };
	}

	const EditorGraphPin* FindPin(const EditorGraphNode& Node, std::string_view Key)
	{
		const auto Found = std::find_if(Node.Pins.begin(), Node.Pins.end(),
			[&](const EditorGraphPin& Pin) { return Pin.Key == Key; });
		return Found == Node.Pins.end() ? nullptr : &*Found;
	}

	std::optional<BlueprintEventKind> ResolveEvent(std::string_view TypeKey)
	{
		if (TypeKey == "blueprint.event.on_create") return BlueprintEventKind::OnCreate;
		if (TypeKey == "blueprint.event.on_update") return BlueprintEventKind::OnUpdate;
		if (TypeKey == "blueprint.event.on_trigger_enter") return BlueprintEventKind::OnTriggerEnter;
		if (TypeKey == "blueprint.event.action_pressed") return BlueprintEventKind::OnActionPressed;
		if (TypeKey == "blueprint.event.action_released") return BlueprintEventKind::OnActionReleased;
		if (TypeKey == "blueprint.event.action_cancelled") return BlueprintEventKind::OnActionCancelled;
		if (TypeKey == "blueprint.event.on_timer") return BlueprintEventKind::OnTimer;
		return std::nullopt;
	}

	std::optional<BlueprintGraphDiagnostic> MakeDiagnostic(BlueprintGraphDiagnosticCode Code,
		std::string Message, UUID Node = {}, UUID Pin = {})
	{
		return BlueprintGraphDiagnostic{ Code, Node, Pin, std::move(Message) };
	}

	class EventGraphCompiler final
	{
	public:
		EventGraphCompiler(const EditorGraph& Graph, std::vector<std::string> PropertyIDs)
			: m_PropertyIDs(std::move(PropertyIDs))
		{
			for (const EditorGraphConnection& Connection : Graph.Connections)
			{
				m_Incoming.emplace(Connection.InputPin, Connection.OutputPin);
				m_Outgoing.emplace(Connection.OutputPin, Connection.InputPin);
			}
		for (const EditorGraphNode& Node : Graph.Nodes)
			for (const EditorGraphPin& Pin : Node.Pins) m_PinOwners.emplace(Pin.ID, &Node);
		}

		std::expected<BlueprintEventProgramSource, BlueprintGraphDiagnostic> Compile(
			const EditorGraphNode& EventNode, BlueprintEventKind Event)
		{
			BlueprintEventProgramSource EventProgram;
			EventProgram.Event = Event;
			BlueprintProgramSource& Program = EventProgram.Program;
			Program.ScalarPropertyIds = m_PropertyIDs;

			const bool ActionEvent = Event == BlueprintEventKind::OnActionPressed ||
				Event == BlueprintEventKind::OnActionReleased || Event == BlueprintEventKind::OnActionCancelled;
			const bool TimerEvent = Event == BlueprintEventKind::OnTimer;
			if (ActionEvent || TimerEvent)
			{
				const std::string_view Key = ActionEvent ? "action" : "timer";
				const EditorGraphPin* EventNamePin = FindPin(EventNode, Key);
				const auto* EventName = EventNamePin ? std::get_if<std::string>(&EventNamePin->DefaultValue) : nullptr;
				if (!EventName || EventName->empty())
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidGraph,
						"Action and timer events require a stable event name", EventNode.ID,
						EventNamePin ? EventNamePin->ID : UUID{}));
				if (ActionEvent)
				{
					Program.ActionNames.push_back(*EventName);
					Program.Instructions.push_back({ BlueprintOpcode::JumpIfAction, 0, 2, EventNode.ID });
				}
				else
				{
					Program.TimerNames.push_back(*EventName);
					Program.Instructions.push_back({ BlueprintOpcode::JumpIfTimer, 0, 2, EventNode.ID });
				}
				Program.Instructions.push_back({ BlueprintOpcode::Return, 0, 0, EventNode.ID });
			}

			const EditorGraphPin* ExecOutput = FindPin(EventNode, "exec");
			if (!ExecOutput)
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidGraph,
					"Event node has no execution output", EventNode.ID));
			UUID NextOutput = ExecOutput->ID;
			std::unordered_set<UUID, UUIDHash> VisitedActions;
			for (;;)
			{
				const auto Next = m_Outgoing.find(NextOutput);
				if (Next == m_Outgoing.end()) break;
				const auto Owner = m_PinOwners.find(Next->second);
				const EditorGraphNode* ActionNode = Owner != m_PinOwners.end() ? Owner->second : nullptr;
				if (ActionNode && IsEditorGraphRerouteType(ActionNode->TypeKey))
				{
					if (!VisitedActions.insert(ActionNode->ID).second)
						return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidGraph,
							"Blueprint execution chain contains a reroute cycle", ActionNode->ID));
					if (VisitedActions.size() > 128)
						return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::LimitExceeded,
							"Blueprint execution chain exceeds the 128-node compiler bound", ActionNode->ID));
					const EditorGraphPin* RerouteInput = FindPin(*ActionNode, "in");
					const EditorGraphPin* RerouteOutput = FindPin(*ActionNode, "out");
					if (!RerouteInput || !RerouteOutput || !m_Incoming.contains(RerouteInput->ID))
						return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidGraph,
							"Execution reroute has no connected input", ActionNode->ID,
							RerouteInput ? RerouteInput->ID : UUID{}));
					NextOutput = RerouteOutput->ID;
					continue;
				}
				if (!ActionNode || ActionNode->TypeKey != "blueprint.action.write_scalar")
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::UnsupportedNode,
						"The connected execution node is not supported by the Blueprint compiler",
						ActionNode ? ActionNode->ID : EventNode.ID, Next->second));
				if (!VisitedActions.insert(ActionNode->ID).second)
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidGraph,
						"Blueprint execution chain contains a cycle", ActionNode->ID));
				if (VisitedActions.size() > 128)
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::LimitExceeded,
						"Blueprint execution chain exceeds the 128-node compiler bound", ActionNode->ID));

				const EditorGraphPin* PropertyPin = FindPin(*ActionNode, "property");
				const auto* Property = PropertyPin ? std::get_if<std::string>(&PropertyPin->DefaultValue) : nullptr;
				if (!Property || Property->empty() || Property->size() > BlueprintMaximumPropertyIdentifierLength)
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidProperty,
						"Scalar property identifier is empty or exceeds its length limit", ActionNode->ID,
						PropertyPin ? PropertyPin->ID : UUID{}));
				const auto PropertyIndex = std::find(m_PropertyIDs.begin(), m_PropertyIDs.end(), *Property);
				if (PropertyIndex == m_PropertyIDs.end())
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidProperty,
						"Scalar property identifier is missing from the graph schema", ActionNode->ID,
						PropertyPin->ID));
				const uint16_t PropertySlot = static_cast<uint16_t>(PropertyIndex - m_PropertyIDs.begin());
				const EditorGraphPin* ValuePin = FindPin(*ActionNode, "value");
				if (!ValuePin)
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidGraph,
						"Scalar property node has no value input", ActionNode->ID));
				if (auto Emitted = EmitValue(*ValuePin, Program, 0); !Emitted)
					return std::unexpected(Emitted.error());
				Program.Instructions.push_back({ BlueprintOpcode::StoreScalar, PropertySlot, 0, ActionNode->ID });
				if (auto Emitted = EmitValue(*ValuePin, Program, 0); !Emitted)
					return std::unexpected(Emitted.error());
				Program.Instructions.push_back({ BlueprintOpcode::WriteScalarProperty, PropertySlot, 0, ActionNode->ID });
				const EditorGraphPin* ExecInput = FindPin(*ActionNode, "exec-in");
				const EditorGraphPin* ExecNext = FindPin(*ActionNode, "exec-out");
				if (!ExecInput || !ExecNext || !m_Incoming.contains(ExecInput->ID))
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidGraph,
						"Action node is not connected to its execution predecessor", ActionNode->ID,
						ExecInput ? ExecInput->ID : UUID{}));
				NextOutput = ExecNext->ID;
			}
			Program.Instructions.push_back({ BlueprintOpcode::Return, 0, 0, EventNode.ID });
			if (Program.Instructions.size() > BlueprintMaximumInstructionCount)
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::LimitExceeded,
					"Compiled Blueprint program exceeds the runtime instruction limit", EventNode.ID));
			return EventProgram;
		}

	private:
		std::expected<void, BlueprintGraphDiagnostic> EmitFloat(float Value, UUID SourceNode,
			BlueprintProgramSource& Program)
		{
			if (!std::isfinite(Value))
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
					"Blueprint numeric input must be finite", SourceNode));
			if (Program.Constants.size() >= std::numeric_limits<uint16_t>::max())
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::LimitExceeded,
					"Blueprint constant table exceeds its encoding limit", SourceNode));
			const uint16_t Index = static_cast<uint16_t>(Program.Constants.size());
			Program.Constants.push_back(Value);
			Program.Instructions.push_back({ BlueprintOpcode::PushConstant, Index, 0, SourceNode });
			return {};
		}

		std::expected<void, BlueprintGraphDiagnostic> EmitValue(const EditorGraphPin& Input,
			BlueprintProgramSource& Program, size_t Depth)
		{
			if (Depth > 128)
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::LimitExceeded,
					"Blueprint expression exceeds the 128-node compile depth", {}, Input.ID));
			const auto Connection = m_Incoming.find(Input.ID);
			if (Connection == m_Incoming.end())
			{
				if (const auto* Value = std::get_if<double>(&Input.DefaultValue))
					return EmitFloat(static_cast<float>(*Value), m_PinOwners.at(Input.ID)->ID, Program);
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
					"Blueprint value input has no compatible source or numeric default", {}, Input.ID));
			}
			const auto Owner = m_PinOwners.find(Connection->second);
			const EditorGraphNode* SourceNode = Owner != m_PinOwners.end() ? Owner->second : nullptr;
			if (!SourceNode)
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
					"Blueprint value source node is missing", {}, Connection->second));
			if (IsEditorGraphRerouteType(SourceNode->TypeKey) &&
				SourceNode->TypeKey == "pulseforge.reroute.float")
			{
				if (!m_VisitedExpressions.insert(SourceNode->ID).second)
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
						"Blueprint value expression contains a reroute cycle", SourceNode->ID));
				const EditorGraphPin* RerouteInput = FindPin(*SourceNode, "in");
				if (!RerouteInput || !m_Incoming.contains(RerouteInput->ID))
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
						"Float reroute has no connected input", SourceNode->ID,
						RerouteInput ? RerouteInput->ID : UUID{}));
				auto Emitted = EmitValue(*RerouteInput, Program, Depth + 1);
				m_VisitedExpressions.erase(SourceNode->ID);
				return Emitted;
			}
			if (SourceNode->TypeKey == "blueprint.value.constant_float")
			{
				const EditorGraphPin* Value = FindPin(*SourceNode, "value");
				const auto* Number = Value ? std::get_if<double>(&Value->DefaultValue) : nullptr;
				if (!Number)
					return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
						"Float constant has no numeric value", SourceNode->ID, Value ? Value->ID : UUID{}));
				return EmitFloat(static_cast<float>(*Number), SourceNode->ID, Program);
			}
			if (SourceNode->TypeKey != "blueprint.math.add")
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::UnsupportedNode,
					"The connected Blueprint value node is unsupported", SourceNode->ID, Connection->second));
			if (!m_VisitedExpressions.insert(SourceNode->ID).second)
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
					"Blueprint value expression contains a cycle", SourceNode->ID));
			const EditorGraphPin* Left = FindPin(*SourceNode, "a");
			const EditorGraphPin* Right = FindPin(*SourceNode, "b");
			if (!Left || !Right)
				return std::unexpected(*MakeDiagnostic(BlueprintGraphDiagnosticCode::InvalidExpression,
					"Add node is missing an operand", SourceNode->ID));
			if (auto Emitted = EmitValue(*Left, Program, Depth + 1); !Emitted) return Emitted;
			if (auto Emitted = EmitValue(*Right, Program, Depth + 1); !Emitted) return Emitted;
			m_VisitedExpressions.erase(SourceNode->ID);
			Program.Instructions.push_back({ BlueprintOpcode::Add, 0, 0, SourceNode->ID });
			return {};
		}

		std::vector<std::string> m_PropertyIDs;
		std::unordered_map<UUID, UUID, UUIDHash> m_Incoming;
		std::unordered_map<UUID, UUID, UUIDHash> m_Outgoing;
		std::unordered_map<UUID, const EditorGraphNode*, UUIDHash> m_PinOwners;
		std::unordered_set<UUID, UUIDHash> m_VisitedExpressions;
	};
}

namespace PulseForgeEditor
{
	EditorGraphSchema MakeBlueprintAuthoringSchema()
	{
		EditorGraphSchema Schema{ "pulseforge.blueprint", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Reject, true, {} };
		const std::array<std::pair<std::string_view, std::string_view>, 7> Events{{
			{ "blueprint.event.on_create", "On Create" }, { "blueprint.event.on_update", "On Update" },
			{ "blueprint.event.on_trigger_enter", "On Trigger Enter" },
			{ "blueprint.event.action_pressed", "On Action Pressed" },
			{ "blueprint.event.action_released", "On Action Released" },
			{ "blueprint.event.action_cancelled", "On Action Cancelled" }, { "blueprint.event.on_timer", "On Timer" } }};
		for (const auto& [Key, Label] : Events)
		{
			std::vector<EditorGraphPinDefinition> Pins;
			if (std::string_view(Key).find("action_") != std::string_view::npos)
				Pins.push_back(MakePin("action", "Action", EditorGraphPinDirection::Input,
					EditorGraphPinType::String, std::string("Interact"), true));
			else if (std::string_view(Key) == "blueprint.event.on_timer")
				Pins.push_back(MakePin("timer", "Timer", EditorGraphPinDirection::Input,
					EditorGraphPinType::String, std::string("Default"), true));
			Pins.push_back(MakePin("exec", "Then", EditorGraphPinDirection::Output, EditorGraphPinType::Execution));
			Schema.Nodes.push_back({ std::string(Key), std::string(Label), std::move(Pins) });
		}
		Schema.Nodes.push_back({ "blueprint.value.constant_float", "Float", {
			MakePin("value", "Value", EditorGraphPinDirection::Output, EditorGraphPinType::Float, 0.0) } });
		Schema.Nodes.push_back({ "blueprint.math.add", "Add", {
			MakePin("a", "A", EditorGraphPinDirection::Input, EditorGraphPinType::Float, 0.0),
			MakePin("b", "B", EditorGraphPinDirection::Input, EditorGraphPinType::Float, 0.0),
			MakePin("value", "Value", EditorGraphPinDirection::Output, EditorGraphPinType::Float, 0.0) } });
		Schema.Nodes.push_back({ "blueprint.action.write_scalar", "Write Scalar Property", {
			MakePin("exec-in", "In", EditorGraphPinDirection::Input, EditorGraphPinType::Execution, {}, true),
			MakePin("property", "Property", EditorGraphPinDirection::Input, EditorGraphPinType::String,
				std::string("value")),
			MakePin("value", "Value", EditorGraphPinDirection::Input, EditorGraphPinType::Float, 0.0, true),
			MakePin("exec-out", "Then", EditorGraphPinDirection::Output, EditorGraphPinType::Execution) } });
		Schema.RerouteTypes = { EditorGraphPinType::Execution, EditorGraphPinType::Float };
		return Schema;
	}

	std::expected<VerifiedBlueprintAsset, BlueprintGraphDiagnostic> CompileBlueprintGraph(
		const EditorGraph& Graph, const EditorGraphSchema& Schema)
	{
		const auto GraphDiagnostics = ValidateEditorGraph(Graph, Schema);
		if (!GraphDiagnostics.empty())
			return std::unexpected(BlueprintGraphDiagnostic{ BlueprintGraphDiagnosticCode::InvalidGraph,
				GraphDiagnostics.front().Element, {}, GraphDiagnostics.front().Message });

		std::vector<const EditorGraphNode*> EventNodes;
		std::vector<std::string> PropertyIDs;
		for (const EditorGraphNode& Node : Graph.Nodes)
		{
			if (ResolveEvent(Node.TypeKey)) EventNodes.push_back(&Node);
			else if (Node.TypeKey == "blueprint.action.write_scalar")
			{
				const EditorGraphPin* PropertyPin = FindPin(Node, "property");
				const auto* Property = PropertyPin ? std::get_if<std::string>(&PropertyPin->DefaultValue) : nullptr;
				if (!Property || Property->empty() || Property->size() > BlueprintMaximumPropertyIdentifierLength)
					return std::unexpected(BlueprintGraphDiagnostic{ BlueprintGraphDiagnosticCode::InvalidProperty,
						Node.ID, PropertyPin ? PropertyPin->ID : UUID{},
						"Scalar property identifier is empty or exceeds its length limit" });
				PropertyIDs.push_back(*Property);
			}
			else if (IsEditorGraphRerouteType(Node.TypeKey)) continue;
			else if (Node.TypeKey != "blueprint.value.constant_float" && Node.TypeKey != "blueprint.math.add")
				return std::unexpected(BlueprintGraphDiagnostic{ BlueprintGraphDiagnosticCode::UnsupportedNode,
					Node.ID, {}, "The Blueprint graph contains an unsupported node type" });
		}
		if (EventNodes.empty())
			return std::unexpected(BlueprintGraphDiagnostic{ BlueprintGraphDiagnosticCode::MissingEvent,
				Graph.ID, {}, "Blueprint graph needs at least one supported event node" });
		std::sort(PropertyIDs.begin(), PropertyIDs.end());
		PropertyIDs.erase(std::unique(PropertyIDs.begin(), PropertyIDs.end()), PropertyIDs.end());

		if (PropertyIDs.size() > BlueprintMaximumScalarCount)
			return std::unexpected(BlueprintGraphDiagnostic{ BlueprintGraphDiagnosticCode::LimitExceeded,
				Graph.ID, {}, "Blueprint graph exceeds the runtime scalar-property limit" });
		EventGraphCompiler Compiler(Graph, PropertyIDs);
		std::vector<BlueprintEventProgramSource> Programs;
		Programs.reserve(EventNodes.size());
		for (const EditorGraphNode* EventNode : EventNodes)
		{
			auto Program = Compiler.Compile(*EventNode, *ResolveEvent(EventNode->TypeKey));
			if (!Program) return std::unexpected(Program.error());
			Programs.push_back(std::move(*Program));
		}
		auto Verified = VerifyBlueprintAsset(std::move(Programs));
		if (!Verified)
			return std::unexpected(BlueprintGraphDiagnostic{ BlueprintGraphDiagnosticCode::InvalidProgram,
				Verified.error().SourceNode, {}, Verified.error().Message });
		return std::move(*Verified);
	}
}
