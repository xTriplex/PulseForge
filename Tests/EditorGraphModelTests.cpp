#include "TestSupport.h"

#include "Editor/EditorGraphModel.h"
#include "Editor/EditorGraphLayout.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_set>

namespace
{
	using namespace PulseForgeEditor;

	PulseForge::UUID ID(uint64_t Value)
	{
		return PulseForge::UUID{ 0x4752415048000000ull, Value };
	}

	EditorGraphNode* FindNode(EditorGraph& Graph, std::string_view Type, size_t Occurrence = 0)
	{
		for (auto& Node : Graph.Nodes)
			if (Node.TypeKey == Type)
				if (Occurrence-- == 0) return &Node;
		return nullptr;
	}

	const EditorGraphPin* FindPin(const EditorGraphNode& Node, std::string_view Key)
	{
		const auto It = std::find_if(Node.Pins.begin(), Node.Pins.end(), [Key](const auto& Pin)
			{ return Pin.Key == Key; });
		return It == Node.Pins.end() ? nullptr : &*It;
	}

	std::expected<EditorGraph, std::string> FixtureGraph(const EditorGraphSchema& Schema)
	{
		return CreateEditorGraph(Schema, ID(1));
	}

	bool AppendNode(EditorGraph& Graph, const EditorGraphSchema& Schema, std::string_view Type, double X)
	{
		auto Node = CreateEditorGraphNode(Schema, Type, X, 32.0);
		if (!Node) return false;
		Graph.Nodes.push_back(std::move(*Node));
		return true;
	}

	EditorGraphSchema MakeMixedTypeCycleSchema()
	{
		const auto ValueFor = [](EditorGraphPinType Type) -> EditorGraphValue
		{
			switch (Type)
			{
			case EditorGraphPinType::Float: return 0.0;
			case EditorGraphPinType::Vector3: return EditorGraphVector3{};
			case EditorGraphPinType::Boolean: return false;
			case EditorGraphPinType::String: return std::string{};
			case EditorGraphPinType::Execution: return {};
			}
			return {};
		};
		const auto Input = [&ValueFor](std::string Key, EditorGraphPinType Type)
			{ return EditorGraphPinDefinition{ std::move(Key), "input", EditorGraphPinDirection::Input, Type,
				EditorGraphPinCardinality::Single, ValueFor(Type) }; };
		const auto Output = [&ValueFor](std::string Key, EditorGraphPinType Type)
			{ return EditorGraphPinDefinition{ std::move(Key), "output", EditorGraphPinDirection::Output, Type,
				EditorGraphPinCardinality::Multiple, ValueFor(Type) }; };
		return { "test.mixed-data-cycle", 1, EditorGraphCyclePolicy::Reject, EditorGraphCyclePolicy::Allow, true,
			{
				{ "test.a", "A", { Input("float-in", EditorGraphPinType::Float), Output("vector-out", EditorGraphPinType::Vector3),
					Input("bool-in", EditorGraphPinType::Boolean) } },
				{ "test.b", "B", { Input("vector-in", EditorGraphPinType::Vector3), Output("float-out", EditorGraphPinType::Float),
					Output("bool-out", EditorGraphPinType::Boolean) } },
				{ "test.c", "C", { Input("bool-in", EditorGraphPinType::Boolean), Output("float-out", EditorGraphPinType::Float) } }
			} };
	}

	const EditorGraphPin* FindPin(const EditorGraph& Graph, std::string_view NodeType, std::string_view Key, size_t Occurrence = 0)
	{
		for (const auto& Node : Graph.Nodes)
			if (Node.TypeKey == NodeType && Occurrence-- == 0)
				return FindPin(Node, Key);
		return nullptr;
	}

	void TestMaterialSchemaAndSerialization(TestRunner& Tests)
	{
		const auto Schema = MakeMaterialExpressionFixtureSchema();
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph.has_value());
		if (!Graph) return;
		PF_CHECK(Tests, AppendNode(*Graph, Schema, "material.scalar", 20.0));
		PF_CHECK(Tests, AppendNode(*Graph, Schema, "material.multiply", 240.0));
		PF_CHECK(Tests, AppendNode(*Graph, Schema, "material.multiply", 440.0));
		PF_CHECK(Tests, ValidateEditorGraph(*Graph, Schema).empty());

		const auto* Scalar = FindPin(*FindNode(*Graph, "material.scalar"), "value");
		auto* MultiplyA = FindNode(*Graph, "material.multiply");
		auto* MultiplyB = FindNode(*Graph, "material.multiply", 1);
		PF_CHECK(Tests, Scalar && MultiplyA && MultiplyB);
		if (!Scalar || !MultiplyA || !MultiplyB) return;
		const auto* InputA = FindPin(*MultiplyA, "a");
		const auto* InputB = FindPin(*MultiplyB, "a");
		const auto* ResultA = FindPin(*MultiplyA, "result");
		const auto* ResultB = FindPin(*MultiplyB, "result");
		PF_CHECK(Tests, InputA && InputB && ResultA && ResultB);
		if (!InputA || !InputB || !ResultA || !ResultB) return;
		const auto InputActions = DiscoverEditorGraphNodeActions(*Graph, Schema, InputA->ID);
		const auto MultiplyResultAction = std::find_if(InputActions.begin(), InputActions.end(), [](const auto& Action)
			{ return Action.NodeTypeKey == "material.multiply" && Action.ConnectPinKey == "result"; });
		PF_CHECK(Tests, MultiplyResultAction != InputActions.end());
		if (MultiplyResultAction != InputActions.end())
		{
			EditorGraphCommandHistory InputOriginHistory(*Graph);
			auto InputOriginCreate = CreateEditorGraphNodeAndConnect(InputOriginHistory, Schema,
				MultiplyResultAction->NodeTypeKey, *MultiplyResultAction->ConnectPinKey, InputA->ID, 760.0, 180.0);
			PF_CHECK(Tests, InputOriginCreate && InputOriginCreate->ConnectedPinID && InputOriginCreate->ConnectionID);
			if (InputOriginCreate && InputOriginCreate->ConnectedPinID && InputOriginCreate->ConnectionID)
			{
				const auto NewNode = std::find_if(InputOriginHistory.Graph().Nodes.begin(), InputOriginHistory.Graph().Nodes.end(),
					[&InputOriginCreate](const auto& Node) { return Node.ID == InputOriginCreate->NodeID; });
				const auto* NewResult = NewNode == InputOriginHistory.Graph().Nodes.end() ? nullptr : FindPin(*NewNode, "result");
				const auto NewLink = std::find_if(InputOriginHistory.Graph().Connections.begin(),
					InputOriginHistory.Graph().Connections.end(), [&InputOriginCreate](const auto& Link)
					{ return Link.ID == *InputOriginCreate->ConnectionID; });
				PF_CHECK(Tests, NewResult && NewResult->Direction == EditorGraphPinDirection::Output &&
					NewResult->Type == EditorGraphPinType::Float);
				PF_CHECK(Tests, NewLink != InputOriginHistory.Graph().Connections.end() && NewResult &&
					NewLink->OutputPin == NewResult->ID && NewLink->InputPin == InputA->ID);
				PF_CHECK(Tests, ValidateEditorGraph(InputOriginHistory.Graph(), Schema).empty());
				const auto AfterInputOriginCreate = InputOriginHistory.Graph();
				PF_CHECK(Tests, InputOriginHistory.Undo() && InputOriginHistory.Graph() == *Graph);
				PF_CHECK(Tests, InputOriginHistory.Redo() && InputOriginHistory.Graph() == AfterInputOriginCreate);
			}
		}
		const auto Decision = CanConnectEditorGraphPins(*Graph, Schema, Scalar->ID, InputA->ID);
		PF_CHECK(Tests, Decision.Allowed && Decision.OutputPin == Scalar->ID && Decision.InputPin == InputA->ID);
		EditorGraphCommandHistory History(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, Scalar->ID, InputA->ID).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, ResultA->ID, InputB->ID).has_value());
		const auto Cycle = CanConnectEditorGraphPins(History.Graph(), Schema, ResultB->ID, InputA->ID);
		PF_CHECK(Tests, !Cycle.Allowed && Cycle.Reason.find("cycle") != std::string::npos);
		const auto BeforeRejectedConnect = History.Graph();
		PF_CHECK(Tests, !ConnectEditorGraphPins(History, Schema, ResultB->ID, InputA->ID));
		PF_CHECK(Tests, History.Graph() == BeforeRejectedConnect);
		PF_CHECK(Tests, History.Graph().Connections.size() == 2);
		PF_CHECK(Tests, History.Undo());
		PF_CHECK(Tests, History.Graph().Connections.size() == 1);
		PF_CHECK(Tests, History.Redo());
		PF_CHECK(Tests, History.Graph().Connections.size() == 2);

		const auto* ScalarB = FindPin(*FindNode(*Graph, "material.scalar"), "value");
		PF_CHECK(Tests, ScalarB != nullptr);
		if (!ScalarB) return;
		const auto OldInputLink = History.Graph().Connections.front().ID;
		const auto BeforeRejectedReconnect = History.Graph();
		PF_CHECK(Tests, !ReconnectEditorGraphConnection(History, Schema, OldInputLink, ResultB->ID, InputA->ID));
		PF_CHECK(Tests, History.Graph() == BeforeRejectedReconnect);
		PF_CHECK(Tests, ReconnectEditorGraphConnection(History, Schema, OldInputLink, ScalarB->ID, InputB->ID).has_value());
		PF_CHECK(Tests, History.Graph().Connections.size() == 1);
		PF_CHECK(Tests, History.Undo());
		PF_CHECK(Tests, History.Graph().Connections.size() == 2);

		const auto Serialized = SerializeEditorGraph(History.Graph(), Schema);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		const auto Again = SerializeEditorGraph(History.Graph(), Schema);
		PF_CHECK(Tests, Again && *Again == *Serialized);
		auto Reordered = History.Graph();
		std::reverse(Reordered.Nodes.begin(), Reordered.Nodes.end());
		for (auto& Node : Reordered.Nodes) std::reverse(Node.Pins.begin(), Node.Pins.end());
		std::reverse(Reordered.Connections.begin(), Reordered.Connections.end());
		const auto Canonical = SerializeEditorGraph(Reordered, Schema);
		PF_CHECK(Tests, Canonical && *Canonical == *Serialized);
		const auto Loaded = DeserializeEditorGraph(*Serialized, Schema);
		const auto LoadedCanonical = Loaded ? SerializeEditorGraph(*Loaded, Schema)
			: std::expected<std::string, std::string>(std::unexpected("Graph load failed"));
		PF_CHECK(Tests, LoadedCanonical && *LoadedCanonical == *Serialized);

		const auto RejectVersion = [&](std::string_view Version)
		{
			std::string Candidate = *Serialized;
			const size_t Field = Candidate.find("\"version\": 1");
			PF_CHECK(Tests, Field != std::string::npos);
			if (Field == std::string::npos) return;
			Candidate.replace(Field, std::string("\"version\": 1").size(), "\"version\": " + std::string(Version));
			PF_CHECK(Tests, !DeserializeEditorGraph(Candidate, Schema));
		};
		for (const std::string_view Version : { "2", "4294967297", "-4294967295", "18446744073709551615",
			"1.0", "\"1\"", "true", "null", "[]", "{}" }) RejectVersion(Version);
		PF_CHECK(Tests, !DeserializeEditorGraph(std::string(2 * 1024 * 1024 + 1, ' '), Schema));

		const auto UsableBeforeFailure = History.Graph();
		std::string Error;
		PF_CHECK(Tests, !History.Replace(*Loaded, MakeGameplayEventFixtureSchema(), Error));
		PF_CHECK(Tests, History.Graph() == UsableBeforeFailure);
		auto DuplicateIdentity = History.Graph();
		DuplicateIdentity.Nodes[1].ID = DuplicateIdentity.Nodes[0].ID;
		PF_CHECK(Tests, !ValidateEditorGraph(DuplicateIdentity, Schema).empty());
		auto DanglingLink = History.Graph();
		DanglingLink.Connections.push_back({ ID(900), ID(901), ID(902) });
		PF_CHECK(Tests, !ValidateEditorGraph(DanglingLink, Schema).empty());

		auto MultiSchema = MakeMaterialExpressionFixtureSchema();
		const auto MultiplyIt = std::find_if(MultiSchema.Nodes.begin(), MultiSchema.Nodes.end(),
			[](const auto& Definition) { return Definition.TypeKey == "material.multiply"; });
		auto* MultiplyDefinition = MultiplyIt == MultiSchema.Nodes.end() ? nullptr : &*MultiplyIt;
		PF_CHECK(Tests, MultiplyDefinition != nullptr);
		if (MultiplyDefinition)
			MultiplyDefinition->Pins[0].Cardinality = EditorGraphPinCardinality::Multiple;
		auto MultiGraph = FixtureGraph(MultiSchema);
		PF_CHECK(Tests, MultiGraph && AppendNode(*MultiGraph, MultiSchema, "material.scalar", 0.0) &&
			AppendNode(*MultiGraph, MultiSchema, "material.scalar", 120.0) &&
			AppendNode(*MultiGraph, MultiSchema, "material.multiply", 240.0));
		if (MultiGraph)
		{
			EditorGraphCommandHistory MultiHistory(*MultiGraph);
			const auto* FirstOutput = FindPin(MultiGraph->Nodes[0], "value");
			const auto* SecondOutput = FindPin(MultiGraph->Nodes[1], "value");
			const auto* SharedInput = FindPin(MultiGraph->Nodes[2], "a");
			PF_CHECK(Tests, FirstOutput && SecondOutput && SharedInput);
			if (FirstOutput && SecondOutput && SharedInput)
			{
				PF_CHECK(Tests, ConnectEditorGraphPins(MultiHistory, MultiSchema, FirstOutput->ID, SharedInput->ID).has_value());
				PF_CHECK(Tests, ConnectEditorGraphPins(MultiHistory, MultiSchema, SecondOutput->ID, SharedInput->ID).has_value());
				PF_CHECK(Tests, MultiHistory.Graph().Connections.size() == 2 && ValidateEditorGraph(MultiHistory.Graph(), MultiSchema).empty());
			}
		}
	}

	void TestGameplaySchemaAndTransactions(TestRunner& Tests)
	{
		const auto Schema = MakeGameplayEventFixtureSchema();
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph.has_value());
		if (!Graph) return;
		PF_CHECK(Tests, AppendNode(*Graph, Schema, "gameplay.event.begin", 20.0));
		PF_CHECK(Tests, AppendNode(*Graph, Schema, "gameplay.flow.branch", 220.0));
		PF_CHECK(Tests, AppendNode(*Graph, Schema, "gameplay.flow.branch", 420.0));
		PF_CHECK(Tests, AppendNode(*Graph, Schema, "gameplay.action.log", 620.0));
		EditorGraphCommandHistory History(*Graph);
		auto* BeginNode = FindNode(*Graph, "gameplay.event.begin");
		auto* BranchA = FindNode(*Graph, "gameplay.flow.branch");
		auto* BranchB = FindNode(*Graph, "gameplay.flow.branch", 1);
		auto* LogNode = FindNode(*Graph, "gameplay.action.log");
		PF_CHECK(Tests, BeginNode && BranchA && BranchB && LogNode);
		if (!BeginNode || !BranchA || !BranchB || !LogNode) return;
		const auto* Begin = FindPin(*BeginNode, "event");
		const auto* BranchAInput = FindPin(*BranchA, "exec-in");
		const auto* BranchAOut = FindPin(*BranchA, "true");
		const auto* BranchBInput = FindPin(*BranchB, "exec-in");
		const auto* BranchBOut = FindPin(*BranchB, "true");
		const auto* LogInput = FindPin(*LogNode, "exec-in");
		const auto* Condition = FindPin(*BranchA, "condition");
		PF_CHECK(Tests, Begin && BranchAInput && BranchAOut && BranchBInput && BranchBOut && LogInput && Condition);
		if (!Begin || !BranchAInput || !BranchAOut || !BranchBInput || !BranchBOut || !LogInput || !Condition) return;
		const auto BranchAID = BranchA->ID;
		const auto BranchBID = BranchB->ID;
		const auto BeginID = Begin->ID;
		const auto BranchAInputID = BranchAInput->ID;
		const auto BranchAOutputID = BranchAOut->ID;
		const auto BranchBInputID = BranchBInput->ID;
		const auto BranchBOutputID = BranchBOut->ID;
		const auto LogInputID = LogInput->ID;
		const auto ConditionID = Condition->ID;
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, BeginID, BranchAInputID).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, BranchAOutputID, BranchBInputID).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, BranchBOutputID, BranchAInputID).has_value());	// Execution cycles are deliberately allowed in this fixture.
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, BranchBOutputID, LogInputID).has_value());
		const auto* TextPin = FindPin(*LogNode, "message");
		PF_CHECK(Tests, TextPin != nullptr);
		if (!TextPin) return;
		const auto TextPinID = TextPin->ID;
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(History, Schema, TextPinID, std::string("authoring fixture" )).has_value());
		PF_CHECK(Tests, !SetEditorGraphPinDefaultValue(History, Schema, ConditionID, 2.0));
		const EditorGraph BeforeMove = History.Graph();
		PF_CHECK(Tests, History.BeginTransaction());
		PF_CHECK(Tests, MoveEditorGraphNodes(History, Schema, {
			{ BranchAID, EditorGraphVector3{ 235.0, 90.0, 0.0 } },
			{ BranchBID, EditorGraphVector3{ 465.0, 160.0, 0.0 } } }).has_value());
		PF_CHECK(Tests, MoveEditorGraphNodes(History, Schema, {
			{ BranchAID, EditorGraphVector3{ 250.0, 100.0, 0.0 } },
			{ BranchBID, EditorGraphVector3{ 480.0, 180.0, 0.0 } } }).has_value());
		std::string Error;
		PF_CHECK(Tests, History.CommitTransaction(Schema, Error));
		const auto AfterMove = History.Graph();
		PF_CHECK(Tests, AfterMove != BeforeMove && History.Undo());
		PF_CHECK(Tests, History.Graph() == BeforeMove);
		PF_CHECK(Tests, History.Redo());
		PF_CHECK(Tests, History.Graph() == AfterMove);
		PF_CHECK(Tests, History.Undo());
		PF_CHECK(Tests, History.BeginTransaction());
		PF_CHECK(Tests, MoveEditorGraphNodes(History, Schema, {
			{ BranchAID, EditorGraphVector3{ 999.0, 999.0, 0.0 } } }).has_value());
		History.CancelTransaction();
		PF_CHECK(Tests, History.Graph() == BeforeMove && History.CanRedo());
		PF_CHECK(Tests, History.Redo() && History.Graph() == AfterMove);
		PF_CHECK(Tests, History.Undo() && History.Graph() == BeforeMove);
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(History, Schema, ConditionID, true).has_value());
		PF_CHECK(Tests, !History.CanRedo());
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(History, Schema, ConditionID, false).has_value());

		const auto Clipboard = CopyEditorGraphSubgraph(History.Graph(), { BranchAID, BranchBID });
		PF_CHECK(Tests, Clipboard.Nodes.size() == 2 && Clipboard.Connections.size() == 2);
		const auto ClipboardText = SerializeEditorGraphClipboard(Clipboard);
		PF_CHECK(Tests, ClipboardText.has_value());
		if (!ClipboardText) return;
		const auto BeforePaste = History.Graph();
		auto FirstPaste = PasteEditorGraphClipboard(History, Schema, *ClipboardText, 40.0, 60.0);
		PF_CHECK(Tests, FirstPaste.has_value() && FirstPaste->size() == 2);
		if (!FirstPaste || FirstPaste->size() != 2) return;
		PF_CHECK(Tests, History.Graph().Nodes.size() == BeforePaste.Nodes.size() + 2);
		PF_CHECK(Tests, History.Graph().Connections.size() == BeforePaste.Connections.size() + 2);
		std::vector<const EditorGraphNode*> FirstPastedNodes;
		for (const auto& Node : History.Graph().Nodes)
			if (std::find(FirstPaste->begin(), FirstPaste->end(), Node.ID) != FirstPaste->end())
				FirstPastedNodes.push_back(&Node);
		PF_CHECK(Tests, FirstPastedNodes.size() == 2);
		if (FirstPastedNodes.size() == 2)
		{
			const auto Left = *std::min_element(FirstPastedNodes.begin(), FirstPastedNodes.end(),
				[](const auto* A, const auto* B) { return A->X < B->X; });
			const auto Right = *std::max_element(FirstPastedNodes.begin(), FirstPastedNodes.end(),
				[](const auto* A, const auto* B) { return A->X < B->X; });
			PF_CHECK(Tests, std::abs(Left->X - 40.0) < 0.001 && std::abs(Left->Y - 60.0) < 0.001);
			PF_CHECK(Tests, std::abs((Right->X - Left->X) - 200.0) < 0.001 &&
				std::abs(Right->Y - Left->Y) < 0.001);
		}
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> ExistingNodeIDs;
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> ExistingPinIDs;
		for (const auto& Node : BeforePaste.Nodes)
		{
			ExistingNodeIDs.insert(Node.ID);
			for (const auto& PinValue : Node.Pins) ExistingPinIDs.insert(PinValue.ID);
		}
		for (const auto& Node : History.Graph().Nodes)
			if (!ExistingNodeIDs.contains(Node.ID))
			{
				for (const auto& PinValue : Node.Pins) PF_CHECK(Tests, !ExistingPinIDs.contains(PinValue.ID));
			}
		const auto PastedGraph = History.Graph();
		PF_CHECK(Tests, History.Undo() && History.Graph() == BeforePaste);
		PF_CHECK(Tests, History.Redo() && History.Graph() == PastedGraph);
		PF_CHECK(Tests, PasteEditorGraphClipboard(History, Schema, *ClipboardText, 80.0, 100.0).has_value());
		PF_CHECK(Tests, History.Graph().Nodes.size() == PastedGraph.Nodes.size() + 2);
		PF_CHECK(Tests, !PasteEditorGraphClipboard(History, MakeMaterialExpressionFixtureSchema(), *ClipboardText, 0.0, 0.0));
		const auto BeforeDuplicate = History.Graph();
		auto Duplicated = DuplicateEditorGraphSubgraph(History, Schema, { BranchAID, BranchBID }, 900.0, 720.0);
		PF_CHECK(Tests, Duplicated.has_value() && Duplicated->size() == 2);
		if (Duplicated && Duplicated->size() == 2)
		{
			const auto DuplicateA = std::find_if(History.Graph().Nodes.begin(), History.Graph().Nodes.end(),
				[&Duplicated](const auto& Node) { return Node.ID == (*Duplicated)[0]; });
			const auto DuplicateB = std::find_if(History.Graph().Nodes.begin(), History.Graph().Nodes.end(),
				[&Duplicated](const auto& Node) { return Node.ID == (*Duplicated)[1]; });
			const auto SourceA = std::find_if(BeforeDuplicate.Nodes.begin(), BeforeDuplicate.Nodes.end(),
				[BranchAID](const auto& Node) { return Node.ID == BranchAID; });
			const auto SourceB = std::find_if(BeforeDuplicate.Nodes.begin(), BeforeDuplicate.Nodes.end(),
				[BranchBID](const auto& Node) { return Node.ID == BranchBID; });
			PF_CHECK(Tests, DuplicateA != History.Graph().Nodes.end() && DuplicateB != History.Graph().Nodes.end());
			PF_CHECK(Tests, SourceA != BeforeDuplicate.Nodes.end() && SourceB != BeforeDuplicate.Nodes.end());
			if (DuplicateA != History.Graph().Nodes.end() && DuplicateB != History.Graph().Nodes.end() &&
				SourceA != BeforeDuplicate.Nodes.end() && SourceB != BeforeDuplicate.Nodes.end())
			{
				const double SourceMinimumX = std::min(SourceA->X, SourceB->X);
				const double SourceMaximumX = std::max(SourceA->X, SourceB->X);
				const double DuplicateMinimumX = std::min(DuplicateA->X, DuplicateB->X);
				const double DuplicateMaximumX = std::max(DuplicateA->X, DuplicateB->X);
				const double DuplicateMinimumY = std::min(DuplicateA->Y, DuplicateB->Y);
				PF_CHECK(Tests, std::abs((DuplicateMaximumX - DuplicateMinimumX) -
					(SourceMaximumX - SourceMinimumX)) < 0.001 &&
					std::abs(DuplicateMinimumX - 900.0) < 0.001 && std::abs(DuplicateMinimumY - 720.0) < 0.001 &&
					std::abs(DuplicateMinimumX - std::floor(SourceMinimumX + 32.0)) > 100.0);
			}
		}
		const auto DuplicatedGraph = History.Graph();
		PF_CHECK(Tests, History.Undo() && History.Graph() == BeforeDuplicate);
		PF_CHECK(Tests, History.Redo() && History.Graph() == DuplicatedGraph);

		const auto BeforeDelete = History.Graph();
		PF_CHECK(Tests, DeleteEditorGraphNodes(History, Schema, { BranchAID }).has_value());
		PF_CHECK(Tests, History.Graph().Connections.size() < BeforeDelete.Connections.size());
		PF_CHECK(Tests, History.Undo());
		PF_CHECK(Tests, History.Graph() == BeforeDelete);
		const auto Current = History.Graph();
		PF_CHECK(Tests, !PasteEditorGraphClipboard(History, Schema, "{\"format\":\"PulseForgeGraphClipboard\",\"version\":4294967297}", 0.0, 0.0));
		PF_CHECK(Tests, History.Graph() == Current);
	}

	void TestPropertyEditCoordinator(TestRunner& Tests)
	{
		EditorGraphSchema Schema{ "test.properties", 1, EditorGraphCyclePolicy::Reject, EditorGraphCyclePolicy::Reject, true,
			{
				{ "test.properties", "Properties", {
					{ "float", "Float", EditorGraphPinDirection::Input, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Single, 1.0 },
					{ "vector", "Vector", EditorGraphPinDirection::Input, EditorGraphPinType::Vector3,
						EditorGraphPinCardinality::Single, EditorGraphVector3{} },
					{ "boolean", "Boolean", EditorGraphPinDirection::Input, EditorGraphPinType::Boolean,
						EditorGraphPinCardinality::Single, false },
					{ "string", "String", EditorGraphPinDirection::Input, EditorGraphPinType::String,
						EditorGraphPinCardinality::Single, std::string{} }
				} }
			} };
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "test.properties", 0.0) &&
			AppendNode(*Graph, Schema, "test.properties", 260.0));
		if (!Graph) return;
		const EditorGraph Initial = *Graph;
		EditorGraphCommandHistory History(*Graph);
		EditorGraphPropertyEditCoordinator Coordinator;
		const auto* FloatPin = FindPin(History.Graph(), "test.properties", "float");
		const auto* VectorPin = FindPin(History.Graph(), "test.properties", "vector");
		const auto* BooleanPin = FindPin(History.Graph(), "test.properties", "boolean");
		const auto* StringPin = FindPin(History.Graph(), "test.properties", "string");
		PF_CHECK(Tests, FloatPin && VectorPin && BooleanPin && StringPin);
		if (!FloatPin || !VectorPin || !BooleanPin || !StringPin) return;
		const auto FloatID = FloatPin->ID;
		const auto VectorID = VectorPin->ID;
		const auto BooleanID = BooleanPin->ID;
		const auto StringID = StringPin->ID;

		// Build a complete frame of copied UUID/value requests while traversing both nodes.
		// The production adapter applies these only after its canvas traversal has ended.
		std::vector<EditorGraphPropertyEditEvent> Frame;
		for (const auto& Node : History.Graph().Nodes)
			for (const auto& PinValue : Node.Pins)
				Frame.push_back({ PinValue.ID, PinValue.DefaultValue, PinValue.ID == FloatID, PinValue.ID == FloatID,
					false });
		const auto FirstEdit = std::find_if(Frame.begin(), Frame.end(), [FloatID](const auto& Event)
			{ return Event.PinID == FloatID; });
		PF_CHECK(Tests, FirstEdit != Frame.end() && Frame.size() == 8);
		if (FirstEdit == Frame.end()) return;
		FirstEdit->Value = 2.0;
		auto Processed = Coordinator.ProcessFrame(History, Schema, Frame, false);
		PF_CHECK(Tests, Processed.GraphChanged && Processed.Error.empty() && History.HasOpenTransaction());
		PF_CHECK(Tests, FindPin(History.Graph(), "test.properties", "float")->DefaultValue == EditorGraphValue{ 2.0 });
		PF_CHECK(Tests, ValidateEditorGraph(History.Graph(), Schema).empty());

		Frame.clear();
		for (const auto& Node : History.Graph().Nodes)
			for (const auto& PinValue : Node.Pins)
				Frame.push_back({ PinValue.ID, PinValue.DefaultValue, false, PinValue.ID == FloatID, false });
		auto LatestFloat = std::find_if(Frame.begin(), Frame.end(), [FloatID](const auto& Event) { return Event.PinID == FloatID; });
		PF_CHECK(Tests, LatestFloat != Frame.end());
		if (LatestFloat != Frame.end()) LatestFloat->Value = 3.0;
		Processed = Coordinator.ProcessFrame(History, Schema, Frame, false);
		PF_CHECK(Tests, Processed.GraphChanged && History.HasOpenTransaction());
		Frame.clear();
		for (const auto& Node : History.Graph().Nodes)
			for (const auto& PinValue : Node.Pins)
				Frame.push_back({ PinValue.ID, PinValue.DefaultValue, false, false, PinValue.ID == FloatID });
		Processed = Coordinator.ProcessFrame(History, Schema, Frame, false);
		const EditorGraph FinalFloat = History.Graph();
		PF_CHECK(Tests, !History.HasOpenTransaction() && History.IsDirty());
		PF_CHECK(Tests, History.Undo() && History.Graph() == Initial);
		PF_CHECK(Tests, History.Redo() && History.Graph() == FinalFloat);

		// An unchanged activation/deactivation must not add history or clear the redo branch.
		PF_CHECK(Tests, History.Undo() && History.CanRedo());
		const auto VectorValue = FindPin(History.Graph(), "test.properties", "vector")->DefaultValue;
		Processed = Coordinator.ProcessFrame(History, Schema,
			{ { VectorID, VectorValue, true, false, false } }, false);
		PF_CHECK(Tests, Processed.Error.empty() && History.HasOpenTransaction());
		Processed = Coordinator.ProcessFrame(History, Schema,
			{ { VectorID, VectorValue, false, false, true } }, false);
		PF_CHECK(Tests, Processed.Error.empty() && !History.HasOpenTransaction() && !History.IsDirty());
		PF_CHECK(Tests, !History.CanUndo() && History.CanRedo());
		PF_CHECK(Tests, SerializeEditorGraph(History.Graph(), Schema).has_value());
		PF_CHECK(Tests, History.Redo() && History.Graph() == FinalFloat);
		PF_CHECK(Tests, History.Undo() && History.CanRedo());

		const auto EditAndUndo = [&](PulseForge::UUID PinID, EditorGraphValue Value)
		{
			const EditorGraph Before = History.Graph();
			std::vector<EditorGraphPropertyEditEvent> Events{ { PinID, Value, true, true, false } };
			auto Result = Coordinator.ProcessFrame(History, Schema, Events, false);
			Events[0].Activated = false;
			Events[0].Changed = false;
			Events[0].Deactivated = true;
			Result = Coordinator.ProcessFrame(History, Schema, Events, false);
			const EditorGraph After = History.Graph();
			PF_CHECK(Tests, Result.Error.empty() && !History.HasOpenTransaction() && After != Before);
			PF_CHECK(Tests, History.Undo() && History.Graph() == Before);
			PF_CHECK(Tests, History.Redo() && History.Graph() == After);
		};
		EditAndUndo(VectorID, EditorGraphVector3{ 1.0, 2.0, 3.0 });
		EditAndUndo(BooleanID, true);
		EditAndUndo(StringID, std::string("edited"));

		const EditorGraph BeforeCancel = History.Graph();
		Processed = Coordinator.ProcessFrame(History, Schema,
			{ { FloatID, 9.0, true, true, false } }, false);
		PF_CHECK(Tests, History.HasOpenTransaction());
		Processed = Coordinator.ProcessFrame(History, Schema, {}, true); // Escape/focus loss cancellation path.
		PF_CHECK(Tests, Processed.GraphChanged && !History.HasOpenTransaction() && !Coordinator.HasActiveEdit());
		PF_CHECK(Tests, History.Graph() == BeforeCancel);

		const EditorGraph BeforeRejected = History.Graph();
		Processed = Coordinator.ProcessFrame(History, Schema,
			{ { FloatID, std::string("wrong type"), true, true, true } }, false);
		PF_CHECK(Tests, !Processed.Error.empty() && !History.HasOpenTransaction());
		PF_CHECK(Tests, History.Graph() == BeforeRejected && ValidateEditorGraph(History.Graph(), Schema).empty());
	}

	void TestReusableGraphLayoutAndPinCommands(TestRunner& Tests)
	{
		const EditorGraphSchema Schema{ "test.named-unlike-fixtures", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true,
			{
				{ "test.source-vertex", "Source Vertex", {
					{ "outgoing-signal", "Signal", EditorGraphPinDirection::Output, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Multiple, 0.0 },
					{ "execution-out", "Pulse", EditorGraphPinDirection::Output, EditorGraphPinType::Execution,
						EditorGraphPinCardinality::Multiple, {} } } },
				{ "test.target-vertex", "Target Vertex", {
					{ "incoming-signal", "Signal In", EditorGraphPinDirection::Input, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Single, 3.0 },
					{ "event-in", "Pulse In", EditorGraphPinDirection::Input, EditorGraphPinType::Execution,
						EditorGraphPinCardinality::Single, {} },
					{ "returned-signal", "Signal Out", EditorGraphPinDirection::Output, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Multiple, 0.0 } } }
			} };
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "test.source-vertex", 10.0) &&
			AppendNode(*Graph, Schema, "test.target-vertex", 240.0) &&
			AppendNode(*Graph, Schema, "test.target-vertex", 460.0) &&
			AppendNode(*Graph, Schema, "test.target-vertex", 680.0));
		if (!Graph) return;
		Graph->Nodes[0].Y = 20.0;
		Graph->Nodes[1].Y = 120.0;
		Graph->Nodes[2].Y = 260.0;
		Graph->Nodes[3].Y = 400.0;
		const auto SourceOutput = Graph->Nodes[0].Pins[0].ID;
		const auto FirstInput = Graph->Nodes[1].Pins[0].ID;
		const auto SecondInput = Graph->Nodes[2].Pins[0].ID;
		const auto FirstOutput = Graph->Nodes[1].Pins[2].ID;
		const auto ThirdInput = Graph->Nodes[3].Pins[0].ID;
		const auto ExecOutput = Graph->Nodes[0].Pins[1].ID;
		const auto ExecInput = Graph->Nodes[1].Pins[1].ID;
		EditorGraphCommandHistory History(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, SourceOutput, FirstInput));
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, SourceOutput, SecondInput));
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, FirstOutput, ThirdInput));
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, ExecOutput, ExecInput));
		const auto BeforeUnlink = History.Graph();
		const auto RemovedIDs = [&]
		{
			std::vector<PulseForge::UUID> IDs;
			for (const auto& Connection : History.Graph().Connections)
				if (Connection.OutputPin == SourceOutput || Connection.InputPin == SourceOutput) IDs.push_back(Connection.ID);
			std::sort(IDs.begin(), IDs.end());
			return IDs;
		}();
		PF_CHECK(Tests, RemovedIDs.size() == 2);
		PF_CHECK(Tests, DisconnectEditorGraphPin(History, Schema, SourceOutput));
		PF_CHECK(Tests, History.Graph().Connections.size() == 2);
		PF_CHECK(Tests, History.Undo() && History.Graph() == BeforeUnlink);
		PF_CHECK(Tests, DisconnectEditorGraphPin(History, Schema, FirstInput));
		PF_CHECK(Tests, History.Graph().Connections.size() == 3);
		PF_CHECK(Tests, History.Undo() && History.Graph() == BeforeUnlink);
		PF_CHECK(Tests, DisconnectEditorGraphPin(History, Schema, ExecInput));
		PF_CHECK(Tests, History.Graph().Connections.size() == 3);
		const auto AfterExecRemoval = History.Graph();
		PF_CHECK(Tests, History.Undo() && History.Graph() == BeforeUnlink);
		PF_CHECK(Tests, History.Redo() && History.Graph() == AfterExecRemoval);
		const auto* DefaultInput = [&]() -> const EditorGraphPin*
			{ return FindPin(History.Graph().Nodes[1], "incoming-signal"); }();
		PF_CHECK(Tests, DefaultInput && DefaultInput->DefaultValue == EditorGraphValue{ 3.0 });
		PF_CHECK(Tests, !DisconnectEditorGraphPin(History, Schema, ID(999999)));
		EditorGraphCommandHistory NoOpHistory(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(NoOpHistory, Schema, SourceOutput, FirstInput));
		PF_CHECK(Tests, NoOpHistory.Undo() && NoOpHistory.CanRedo());
		const auto UnconnectedPin = Graph->Nodes[2].Pins[1].ID;
		const auto NoOpGraph = NoOpHistory.Graph();
		PF_CHECK(Tests, DisconnectEditorGraphPin(NoOpHistory, Schema, UnconnectedPin));
		PF_CHECK(Tests, NoOpHistory.Graph() == NoOpGraph && NoOpHistory.CanRedo());

		const auto Current = History.Graph();
		std::vector<EditorGraphLayoutNode> Nodes;
		for (const auto& Node : Current.Nodes) Nodes.push_back({ Node.ID, { Node.X, Node.Y, 0.0 } });
		const auto SourceNode = Current.Nodes[0].ID;
		const auto SelectedNode = Current.Nodes[1].ID;
		const auto UnselectedNode = Current.Nodes[2].ID;
		const auto ChainEndNode = Current.Nodes[3].ID;
		const auto Connections = Current.Connections;
		const auto Layout = CalculateEditorGraphStraightenedLayout(Nodes,
			{ { SourceNode, SourceOutput, 50.0 }, { SelectedNode, FirstInput, 20.0 },
				{ SelectedNode, FirstOutput, 42.0 }, { UnselectedNode, SecondInput, 20.0 },
				{ ChainEndNode, ThirdInput, 10.0 } }, Connections, { ChainEndNode, SelectedNode, SourceNode });
		PF_CHECK(Tests, Layout.size() == 2);
		const auto SourcePlacement = std::find_if(Layout.begin(), Layout.end(), [SourceNode](const auto& Item) { return Item.first == SourceNode; });
		const auto TargetPlacement = std::find_if(Layout.begin(), Layout.end(), [SelectedNode](const auto& Item) { return Item.first == SelectedNode; });
		const auto ChainEndPlacement = std::find_if(Layout.begin(), Layout.end(), [ChainEndNode](const auto& Item) { return Item.first == ChainEndNode; });
		PF_CHECK(Tests, SourcePlacement == Layout.end() && TargetPlacement != Layout.end() && ChainEndPlacement != Layout.end());
		if (TargetPlacement != Layout.end() && ChainEndPlacement != Layout.end())
		{
			PF_CHECK(Tests, std::abs((Nodes[0].Position.Y + 50.0) - (TargetPlacement->second.Y + 20.0)) < 0.01);
			PF_CHECK(Tests, std::abs((TargetPlacement->second.Y + 42.0) - (ChainEndPlacement->second.Y + 10.0)) < 0.01);
		}
		const auto ReorderedLayout = CalculateEditorGraphStraightenedLayout(Nodes,
			{ { SourceNode, SourceOutput, 50.0 }, { SelectedNode, FirstInput, 20.0 },
				{ SelectedNode, FirstOutput, 42.0 }, { UnselectedNode, SecondInput, 20.0 },
				{ ChainEndNode, ThirdInput, 10.0 } }, Connections, { SourceNode, ChainEndNode, SelectedNode });
		PF_CHECK(Tests, Layout == ReorderedLayout);
		PF_CHECK(Tests, std::none_of(Layout.begin(), Layout.end(), [UnselectedNode](const auto& Item)
			{ return Item.first == UnselectedNode; }));
		const auto AppliedBefore = History.Graph();
		PF_CHECK(Tests, MoveEditorGraphNodes(History, Schema, Layout));
		const auto AppliedAfter = History.Graph();
		PF_CHECK(Tests, History.Graph().Connections == Current.Connections);
		PF_CHECK(Tests, AppliedBefore != AppliedAfter && History.Undo() && History.Graph() == AppliedBefore);
		PF_CHECK(Tests, History.Redo() && History.Graph() == AppliedAfter);
		const auto LayoutSource = SerializeEditorGraph(History.Graph(), Schema);
		const auto LayoutReloaded = LayoutSource ? DeserializeEditorGraph(*LayoutSource, Schema)
			: std::expected<EditorGraph, std::string>(std::unexpected("Graph serialization failed"));
		PF_CHECK(Tests, LayoutReloaded.has_value());
		if (LayoutReloaded)
		{
			for (const auto& Node : History.Graph().Nodes)
			{
				const auto ReloadedNode = std::find_if(LayoutReloaded->Nodes.begin(), LayoutReloaded->Nodes.end(),
					[&Node](const auto& Candidate) { return Candidate.ID == Node.ID; });
				PF_CHECK(Tests, ReloadedNode != LayoutReloaded->Nodes.end() &&
					ReloadedNode->X == Node.X && ReloadedNode->Y == Node.Y);
			}
		}
		std::vector<EditorGraphLayoutNode> CurrentNodes;
		for (const auto& Node : History.Graph().Nodes) CurrentNodes.push_back({ Node.ID, { Node.X, Node.Y, 0.0 } });
		const auto Repeat = CalculateEditorGraphStraightenedLayout(CurrentNodes,
			{ { SourceNode, SourceOutput, 50.0 }, { SelectedNode, FirstInput, 20.0 } },
			History.Graph().Connections, { SourceNode, SelectedNode });
		PF_CHECK(Tests, Repeat.empty());

		const auto Disconnected = CalculateEditorGraphStraightenedLayout(Nodes, {}, {},
			{ UnselectedNode, ChainEndNode, SelectedNode, SourceNode });
		PF_CHECK(Tests, Disconnected.size() == 3);
		for (const auto& [IDValue, Position] : Disconnected)
			if (IDValue != SourceNode) PF_CHECK(Tests, Position.Y == Nodes[0].Position.Y);
		PF_CHECK(Tests, CalculateEditorGraphStraightenedLayout(Nodes, {}, {}, { SourceNode }).empty());
	}

	void TestNodeActionDiscoverySearchAndAtomicAutowire(TestRunner& Tests)
	{
		const EditorGraphSchema Schema{ "test.node-discovery", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true,
			{
				{ "test.origin", "Origin Source", {
					{ "boolean-out", "Boolean Out", EditorGraphPinDirection::Output, EditorGraphPinType::Boolean,
						EditorGraphPinCardinality::Multiple, false },
					{ "numeric-out", "Numeric Out", EditorGraphPinDirection::Output, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Multiple, 1.0 },
					{ "vector-out", "Vector Out", EditorGraphPinDirection::Output, EditorGraphPinType::Vector3,
						EditorGraphPinCardinality::Multiple, EditorGraphVector3{} },
					{ "text-out", "Text Out", EditorGraphPinDirection::Output, EditorGraphPinType::String,
						EditorGraphPinCardinality::Multiple, std::string{} },
					{ "flow-out", "Flow Out", EditorGraphPinDirection::Output, EditorGraphPinType::Execution,
						EditorGraphPinCardinality::Multiple, {} } } },
				{ "test.mixed", "Mixed Branch", {
					{ "choice-first", "Choice One", EditorGraphPinDirection::Input, EditorGraphPinType::Boolean,
						EditorGraphPinCardinality::Single, false },
					{ "choice-second", "Choice Two", EditorGraphPinDirection::Input, EditorGraphPinType::Boolean,
						EditorGraphPinCardinality::Single, true },
					{ "amount", "Amount", EditorGraphPinDirection::Input, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Single, 0.0 },
					{ "direction", "Direction", EditorGraphPinDirection::Input, EditorGraphPinType::Vector3,
						EditorGraphPinCardinality::Single, EditorGraphVector3{} },
					{ "caption", "Caption", EditorGraphPinDirection::Input, EditorGraphPinType::String,
						EditorGraphPinCardinality::Single, std::string{} },
					{ "pulse", "Pulse", EditorGraphPinDirection::Input, EditorGraphPinType::Execution,
						EditorGraphPinCardinality::Single, {} },
					{ "result", "Result", EditorGraphPinDirection::Output, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Multiple, 0.0 } } },
				{ "test.pair", "Number Pair", {
					{ "first", "First Number", EditorGraphPinDirection::Input, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Single, 0.0 },
					{ "second", "Second Number", EditorGraphPinDirection::Input, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Single, 0.0 },
					{ "combined", "Combined", EditorGraphPinDirection::Output, EditorGraphPinType::Float,
						EditorGraphPinCardinality::Multiple, 0.0 } } },
				{ "test.text-origin", "Text Emitter", {
					{ "text", "Text", EditorGraphPinDirection::Output, EditorGraphPinType::String,
						EditorGraphPinCardinality::Multiple, std::string{} } } },
				{ "test.flow-target", "Flow Target", {
					{ "flow-in", "Flow In", EditorGraphPinDirection::Input, EditorGraphPinType::Execution,
						EditorGraphPinCardinality::Single, {} } } }
			} };
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "test.origin", 0.0) &&
			AppendNode(*Graph, Schema, "test.mixed", 200.0));
		if (!Graph) return;
		const auto Original = *Graph;
		const auto* BooleanOut = FindPin(*Graph, "test.origin", "boolean-out");
		const auto* NumberOut = FindPin(*Graph, "test.origin", "numeric-out");
		const auto* VectorOut = FindPin(*Graph, "test.origin", "vector-out");
		const auto* TextOut = FindPin(*Graph, "test.origin", "text-out");
		const auto* FlowOut = FindPin(*Graph, "test.origin", "flow-out");
		const auto* MixedNumber = FindPin(*Graph, "test.mixed", "amount");
		const auto* MixedText = FindPin(*Graph, "test.mixed", "caption");
		PF_CHECK(Tests, BooleanOut && NumberOut && VectorOut && TextOut && FlowOut && MixedNumber && MixedText);
		if (!BooleanOut || !NumberOut || !VectorOut || !TextOut || !FlowOut || !MixedNumber || !MixedText) return;

		const auto BooleanActions = DiscoverEditorGraphNodeActions(*Graph, Schema, BooleanOut->ID);
		PF_CHECK(Tests, BooleanActions == DiscoverEditorGraphNodeActions(*Graph, Schema, BooleanOut->ID));
		EditorGraphSchema ExpandedCatalog = Schema;
		for (size_t Index = 0; Index < 256; ++Index)
			ExpandedCatalog.Nodes.push_back({ "test.discovery.expanded." + std::to_string(Index), "Expanded",
				{ { "in", "Input", EditorGraphPinDirection::Input, EditorGraphPinType::Boolean,
					EditorGraphPinCardinality::Single, false } } });
		EditorGraphWorkStats DiscoveryWork;
		const auto ExpandedActions = DiscoverEditorGraphNodeActions(*Graph, ExpandedCatalog, BooleanOut->ID, &DiscoveryWork);
		PF_CHECK(Tests, ExpandedActions.size() >= 256 && DiscoveryWork.RerouteTopologyBuilds <= 2);
		PF_CHECK(Tests, BooleanActions.size() == 2);
		PF_CHECK(Tests, std::all_of(BooleanActions.begin(), BooleanActions.end(), [](const auto& Action)
			{ return Action.NodeTypeKey == "test.mixed" && Action.ConnectPinKey &&
				(*Action.ConnectPinKey == "choice-first" || *Action.ConnectPinKey == "choice-second"); }));
		const auto NumberActions = DiscoverEditorGraphNodeActions(*Graph, Schema, NumberOut->ID);
		PF_CHECK(Tests, std::count_if(NumberActions.begin(), NumberActions.end(), [](const auto& Action)
			{ return Action.NodeTypeKey == "test.pair" && Action.ConnectPinKey.has_value(); }) == 2);
		const auto VectorActions = DiscoverEditorGraphNodeActions(*Graph, Schema, VectorOut->ID);
		PF_CHECK(Tests, VectorActions.size() == 1 && VectorActions.front().NodeTypeKey == "test.mixed" &&
			VectorActions.front().ConnectPinKey == "direction");
		const auto FlowActions = DiscoverEditorGraphNodeActions(*Graph, Schema, FlowOut->ID);
		PF_CHECK(Tests, std::any_of(FlowActions.begin(), FlowActions.end(), [](const auto& Action)
			{ return Action.NodeTypeKey == "test.flow-target" && Action.ConnectPinKey == "flow-in"; }));
		const auto InputActions = DiscoverEditorGraphNodeActions(*Graph, Schema, MixedText->ID);
		PF_CHECK(Tests, std::any_of(InputActions.begin(), InputActions.end(), [](const auto& Action)
			{ return Action.NodeTypeKey == "test.text-origin" && Action.ConnectPinKey == "text"; }));
		PF_CHECK(Tests, Graph == Original);

		const auto BackgroundActions = DiscoverEditorGraphNodeActions(*Graph, Schema);
		PF_CHECK(Tests, BackgroundActions.size() == Schema.Nodes.size());
		const auto OtherSchema = MakeMaterialExpressionFixtureSchema();
		PF_CHECK(Tests, DiscoverEditorGraphNodeActions(*Graph, OtherSchema).empty());
		PF_CHECK(Tests, SearchEditorGraphNodeActions(BackgroundActions, "mIxEd").size() == 1 &&
			SearchEditorGraphNodeActions(BackgroundActions, "mIxEd").front().NodeTypeKey == "test.mixed");
		PF_CHECK(Tests, SearchEditorGraphNodeActions(NumberActions, "FIRST NUMBER").size() == 1);
		PF_CHECK(Tests, SearchEditorGraphNodeActions(NumberActions, "not a result").empty());
		PF_CHECK(Tests, SearchEditorGraphNodeActions(BackgroundActions, "").size() == BackgroundActions.size());

		EditorGraphCommandHistory CreateOnly(*Graph);
		PF_CHECK(Tests, AddEditorGraphNode(CreateOnly, Schema, "test.pair", 500.0, 60.0));
		PF_CHECK(Tests, CreateOnly.Graph().Nodes.size() == Graph->Nodes.size() + 1 && CreateOnly.CanUndo());
		const auto CreateOnlyGraph = CreateOnly.Graph();
		PF_CHECK(Tests, CreateOnly.Undo() && CreateOnly.Graph() == *Graph);
		PF_CHECK(Tests, CreateOnly.Redo() && CreateOnly.Graph() == CreateOnlyGraph);

		EditorGraphCommandHistory FanoutHistory(*Graph);
		const auto* ExistingFloatInput = FindPin(FanoutHistory.Graph().Nodes[1], "amount");
		PF_CHECK(Tests, ExistingFloatInput &&
			ConnectEditorGraphPins(FanoutHistory, Schema, NumberOut->ID, ExistingFloatInput->ID));
		const auto ExistingFanout = FanoutHistory.Graph().Connections;
		auto FanoutCreate = CreateEditorGraphNodeAndConnect(FanoutHistory, Schema,
			"test.pair", "second", NumberOut->ID, 480.0, 100.0);
		PF_CHECK(Tests, FanoutCreate && FanoutHistory.Graph().Nodes.size() == Graph->Nodes.size() + 1 &&
			FanoutHistory.Graph().Connections.size() == ExistingFanout.size() + 1);
		if (FanoutCreate)
		{
			const auto AfterCreate = FanoutHistory.Graph();
			PF_CHECK(Tests, FanoutCreate->ConnectedPinID && FanoutCreate->ConnectionID &&
				FanoutHistory.Graph().Connections.back().OutputPin == NumberOut->ID &&
				FanoutHistory.Graph().Connections.back().InputPin == *FanoutCreate->ConnectedPinID);
			PF_CHECK(Tests, FanoutHistory.Undo() && FanoutHistory.Graph().Connections == ExistingFanout &&
				FanoutHistory.Graph().Nodes.size() == Graph->Nodes.size());
			PF_CHECK(Tests, FanoutHistory.Redo() && FanoutHistory.Graph() == AfterCreate);
			PF_CHECK(Tests, FanoutHistory.Graph().Nodes.back().ID == FanoutCreate->NodeID &&
				FanoutHistory.Graph().Connections.back().ID == *FanoutCreate->ConnectionID);
			const auto Serialized = SerializeEditorGraph(FanoutHistory.Graph(), Schema);
			const auto Reloaded = Serialized ? DeserializeEditorGraph(*Serialized, Schema)
				: std::expected<EditorGraph, std::string>(std::unexpected("Serialization failed"));
			const auto ReloadedSource = Reloaded ? SerializeEditorGraph(*Reloaded, Schema)
				: std::expected<std::string, std::string>(std::unexpected("Reload failed"));
			PF_CHECK(Tests, ReloadedSource && Serialized && *ReloadedSource == *Serialized);
		}

		EditorGraphCommandHistory ReplacementHistory(*Graph);
		const auto* OldOutput = FindPin(ReplacementHistory.Graph().Nodes[0], "numeric-out");
		const auto* ExistingInput = FindPin(ReplacementHistory.Graph().Nodes[1], "amount");
		const auto OldOutputID = OldOutput ? std::optional{ OldOutput->ID } : std::nullopt;
		const auto ExistingInputID = ExistingInput ? std::optional{ ExistingInput->ID } : std::nullopt;
		PF_CHECK(Tests, OldOutputID && ExistingInputID &&
			ConnectEditorGraphPins(ReplacementHistory, Schema, *OldOutputID, *ExistingInputID));
		const auto BeforeReplacement = ReplacementHistory.Graph();
		const auto OriginalConnection = ReplacementHistory.Graph().Connections.back();
		auto Replacement = CreateEditorGraphNodeAndConnect(ReplacementHistory, Schema,
			"test.pair", "combined", ExistingInputID.value_or(PulseForge::UUID{}), 520.0, 120.0);
		PF_CHECK(Tests, Replacement && ReplacementHistory.Graph().Connections.size() == 1 &&
			ReplacementHistory.Graph().Connections.front().InputPin == ExistingInputID.value_or(PulseForge::UUID{}) &&
			ReplacementHistory.Graph().Connections.front().ID != OriginalConnection.ID);
		if (Replacement)
		{
			const auto AfterReplacement = ReplacementHistory.Graph();
			PF_CHECK(Tests, ReplacementHistory.Undo() && ReplacementHistory.Graph() == BeforeReplacement);
			PF_CHECK(Tests, ReplacementHistory.Redo() && ReplacementHistory.Graph() == AfterReplacement);
		}

		EditorGraphCommandHistory ExecutionHistory(*Graph);
		auto ExecutionCreate = CreateEditorGraphNodeAndConnect(ExecutionHistory, Schema,
			"test.flow-target", "flow-in", FlowOut->ID, 700.0, 100.0);
		PF_CHECK(Tests, ExecutionCreate && ExecutionHistory.Graph().Connections.size() == 1 &&
			ExecutionHistory.Graph().Connections.front().OutputPin == FlowOut->ID);

		EditorGraphCommandHistory RejectionHistory(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(RejectionHistory, Schema, NumberOut->ID, MixedNumber->ID));
		PF_CHECK(Tests, RejectionHistory.Undo() && RejectionHistory.CanRedo());
		const auto BeforeRejectedCreate = RejectionHistory.Graph();
		const auto RejectedCreate = CreateEditorGraphNodeAndConnect(RejectionHistory, Schema,
			"test.mixed", "choice-first", NumberOut->ID, 900.0, 100.0);
		PF_CHECK(Tests, !RejectedCreate && RejectionHistory.Graph() == BeforeRejectedCreate && RejectionHistory.CanRedo());

		EditorGraphCommandHistory MissingSourceHistory(*Graph);
		PF_CHECK(Tests, DeleteEditorGraphNodes(MissingSourceHistory, Schema, { Original.Nodes[0].ID }));
		const auto AfterDelete = MissingSourceHistory.Graph();
		const auto MissingSource = CreateEditorGraphNodeAndConnect(MissingSourceHistory, Schema,
			"test.pair", "first", NumberOut->ID, 920.0, 140.0);
		PF_CHECK(Tests, !MissingSource && MissingSourceHistory.Graph() == AfterDelete);
	}

	void TestMixedTypeCyclePolicies(TestRunner& Tests)
	{
		const auto Schema = MakeMixedTypeCycleSchema();
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "test.a", 0.0) &&
			AppendNode(*Graph, Schema, "test.b", 200.0) && AppendNode(*Graph, Schema, "test.c", 400.0));
		if (!Graph) return;
		const auto* AVector = FindPin(*Graph, "test.a", "vector-out");
		const auto* AFloat = FindPin(*Graph, "test.a", "float-in");
		const auto* ABool = FindPin(*Graph, "test.a", "bool-in");
		const auto* BVector = FindPin(*Graph, "test.b", "vector-in");
		const auto* BFloat = FindPin(*Graph, "test.b", "float-out");
		const auto* BBool = FindPin(*Graph, "test.b", "bool-out");
		const auto* CBool = FindPin(*Graph, "test.c", "bool-in");
		const auto* CFloat = FindPin(*Graph, "test.c", "float-out");
		PF_CHECK(Tests, AVector && AFloat && ABool && BVector && BFloat && BBool && CBool && CFloat);
		if (!AVector || !AFloat || !ABool || !BVector || !BFloat || !BBool || !CBool || !CFloat) return;
		const auto IDs = std::array{ AVector->ID, AFloat->ID, ABool->ID, BVector->ID,
			BFloat->ID, BBool->ID, CBool->ID, CFloat->ID };
		EditorGraphCommandHistory History(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, IDs[0], IDs[3]).has_value());
		const auto TwoNodeCycle = CanConnectEditorGraphPins(History.Graph(), Schema, IDs[4], IDs[1]);
		PF_CHECK(Tests, !TwoNodeCycle.Allowed && TwoNodeCycle.Reason.find("cycle") != std::string::npos);
		const EditorGraph BeforeRejected = History.Graph();
		const bool HadUndo = History.CanUndo();
		PF_CHECK(Tests, !ConnectEditorGraphPins(History, Schema, IDs[4], IDs[1]));
		PF_CHECK(Tests, History.Graph() == BeforeRejected && History.CanUndo() == HadUndo);

		EditorGraphCommandHistory LongHistory(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(LongHistory, Schema, IDs[0], IDs[3]).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(LongHistory, Schema, IDs[5], IDs[6]).has_value());
		PF_CHECK(Tests, ValidateEditorGraph(LongHistory.Graph(), Schema).empty());
		PF_CHECK(Tests, !CanConnectEditorGraphPins(LongHistory.Graph(), Schema, IDs[7], IDs[1]).Allowed);
		const EditorGraph BeforeLongReject = LongHistory.Graph();
		PF_CHECK(Tests, !ConnectEditorGraphPins(LongHistory, Schema, IDs[7], IDs[1]));
		PF_CHECK(Tests, LongHistory.Graph() == BeforeLongReject);

		// Reconnecting the existing A->B edge removes it first. Without that omission,
		// the old path A->B->C would incorrectly reject the valid C->A replacement.
		EditorGraphCommandHistory ReconnectHistory(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(ReconnectHistory, Schema, IDs[0], IDs[3]).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(ReconnectHistory, Schema, IDs[5], IDs[6]).has_value());
		const auto ReplacedID = ReconnectHistory.Graph().Connections.front().ID;
		PF_CHECK(Tests, CanConnectEditorGraphPins(ReconnectHistory.Graph(), Schema, IDs[7], IDs[1], ReplacedID).Allowed);
		PF_CHECK(Tests, ReconnectEditorGraphConnection(ReconnectHistory, Schema, ReplacedID, IDs[7], IDs[1]).has_value());
		PF_CHECK(Tests, ValidateEditorGraph(ReconnectHistory.Graph(), Schema).empty());

		// Full-source validation must reject the otherwise well-formed mixed-type cycle.
		EditorGraphCommandHistory SerializedHistory(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(SerializedHistory, Schema, IDs[0], IDs[3]).has_value());
		auto Source = SerializeEditorGraph(SerializedHistory.Graph(), Schema);
		PF_CHECK(Tests, Source.has_value());
		if (Source)
		{
			const std::string ArrayEnd = "\n  ]";
			const auto Position = Source->rfind(ArrayEnd);
			PF_CHECK(Tests, Position != std::string::npos);
			if (Position != std::string::npos)
			{
				const auto CycleConnectionID = ID(700).ToString();
				Source->insert(Position, "    ,\n    {\"id\":\"" + CycleConnectionID + "\",\"output\":\"" +
					ID(701).ToString() + "\",\"input\":\"" + ID(702).ToString() + "\"}\n");
				// Replace the synthetic pin IDs with the actual B.Float -> A.Float endpoints.
				const std::string Synthetic = "\"output\":\"" + ID(701).ToString() + "\",\"input\":\"" + ID(702).ToString() + "\"";
				const auto SyntheticPosition = Source->find(Synthetic);
				PF_CHECK(Tests, SyntheticPosition != std::string::npos);
				if (SyntheticPosition != std::string::npos)
					Source->replace(SyntheticPosition, Synthetic.size(), "\"output\":\"" + IDs[4].ToString() +
						"\",\"input\":\"" + IDs[1].ToString() + "\"");
				PF_CHECK(Tests, !DeserializeEditorGraph(*Source, Schema));
			}
		}

		// A mixed-type acyclic chain remains valid, while execution cycles stay governed
		// by the gameplay fixture's independent Allow policy.
		PF_CHECK(Tests, ValidateEditorGraph(LongHistory.Graph(), Schema).empty());
		const auto Gameplay = MakeGameplayEventFixtureSchema();
		auto GameplayGraph = FixtureGraph(Gameplay);
		PF_CHECK(Tests, GameplayGraph && AppendNode(*GameplayGraph, Gameplay, "gameplay.flow.branch", 0.0) &&
			AppendNode(*GameplayGraph, Gameplay, "gameplay.flow.branch", 200.0));
		if (GameplayGraph)
		{
			const auto* BranchA = FindNode(*GameplayGraph, "gameplay.flow.branch");
			const auto* BranchB = FindNode(*GameplayGraph, "gameplay.flow.branch", 1);
			PF_CHECK(Tests, BranchA && BranchB);
			if (BranchA && BranchB)
			{
				const auto* OutA = FindPin(*BranchA, "true");
				const auto* InA = FindPin(*BranchA, "exec-in");
				const auto* OutB = FindPin(*BranchB, "true");
				const auto* InB = FindPin(*BranchB, "exec-in");
				PF_CHECK(Tests, OutA && InA && OutB && InB);
				if (OutA && InA && OutB && InB)
				{
					EditorGraphCommandHistory ExecutionHistory(*GameplayGraph);
					PF_CHECK(Tests, ConnectEditorGraphPins(ExecutionHistory, Gameplay, OutA->ID, InB->ID).has_value());
					PF_CHECK(Tests, ConnectEditorGraphPins(ExecutionHistory, Gameplay, OutB->ID, InA->ID).has_value());
					PF_CHECK(Tests, ValidateEditorGraph(ExecutionHistory.Graph(), Gameplay).empty());
				}
			}
		}
	}
}

namespace
{
	void TestIndexedTraversalAndEndpointInvariants(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
#if defined(NDEBUG)
		constexpr const char* BuildFlavor = "optimized";
#else
		constexpr const char* BuildFlavor = "debug";
#endif
		EditorGraphSchema ChainSchema{ "test.indexed-chain", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true, { { "test.step", "Step", {
				{ "input", "Input", EditorGraphPinDirection::Input, EditorGraphPinType::Float,
					EditorGraphPinCardinality::Single, 0.0 },
				{ "output", "Output", EditorGraphPinDirection::Output, EditorGraphPinType::Float,
					EditorGraphPinCardinality::Multiple, 0.0 } } } } };
		auto LongGraph = CreateEditorGraph(ChainSchema, ID(0xabc));
		PF_CHECK(Tests, LongGraph.has_value());
		if (!LongGraph) return;
		constexpr size_t NodeCount = 1000;
		LongGraph->Nodes.reserve(NodeCount);
		for (size_t Index = 0; Index < NodeCount; ++Index)
		{
			auto Node = CreateEditorGraphNode(ChainSchema, "test.step", static_cast<double>(Index * 40), 20.0);
			PF_CHECK(Tests, Node.has_value());
			if (!Node) return;
			LongGraph->Nodes.push_back(std::move(*Node));
		}
		LongGraph->Connections.reserve(NodeCount - 1);
		for (size_t Index = 0; Index + 1 < NodeCount; ++Index)
			LongGraph->Connections.push_back({ ID(0x100000 + Index), LongGraph->Nodes[Index].Pins[1].ID,
				LongGraph->Nodes[Index + 1].Pins[0].ID });
		EditorGraphWorkStats ValidationWork;
		PF_CHECK(Tests, ValidateEditorGraph(*LongGraph, ChainSchema, &ValidationWork).empty());
		PF_CHECK(Tests, ValidationWork.NodeVisits == NodeCount && ValidationWork.PinVisits == NodeCount * 2 &&
			ValidationWork.ConnectionVisits == NodeCount - 1 && ValidationWork.TraversedEdges <= (NodeCount - 1) * 2);
		EditorGraphWorkStats DecisionWork;
		const auto Cycle = CanConnectEditorGraphPins(*LongGraph, ChainSchema,
			LongGraph->Nodes.back().Pins[1].ID, LongGraph->Nodes.front().Pins[0].ID, std::nullopt, &DecisionWork);
		PF_CHECK(Tests, !Cycle.Allowed && DecisionWork.NodeVisits == NodeCount &&
			DecisionWork.PinVisits == NodeCount * 2 && DecisionWork.ConnectionVisits == NodeCount - 1 &&
			DecisionWork.TraversedEdges <= NodeCount - 1);
		EditorGraphWorkStats DiscoveryWork;
		const auto Actions = DiscoverEditorGraphNodeActions(*LongGraph, ChainSchema,
			LongGraph->Nodes.front().Pins[1].ID, &DiscoveryWork);
		PF_CHECK(Tests, !Actions.empty() && DiscoveryWork.NodeVisits == NodeCount &&
			DiscoveryWork.PinVisits == NodeCount * 2 && DiscoveryWork.ConnectionVisits == NodeCount - 1);
		EditorGraphSchema ExpandedDiscoverySchema = ChainSchema;
		for (size_t Index = 0; Index < 256; ++Index)
			ExpandedDiscoverySchema.Nodes.push_back({ "test.discovery.benchmark." + std::to_string(Index), "Expanded",
				{ { "input", "Input", EditorGraphPinDirection::Input, EditorGraphPinType::Float,
					EditorGraphPinCardinality::Single, 0.0 } } });
		const auto MeasureDiscovery = [&](const EditorGraphSchema& DiscoverySchema)
		{
			const auto Begin = std::chrono::steady_clock::now();
			EditorGraphWorkStats Work;
			for (size_t Iteration = 0; Iteration < 5; ++Iteration)
				(void)DiscoverEditorGraphNodeActions(*LongGraph, DiscoverySchema,
					LongGraph->Nodes.front().Pins[1].ID, &Work);
			const auto End = std::chrono::steady_clock::now();
			return std::pair{ std::chrono::duration<double, std::milli>(End - Begin).count() / 5.0,
				Work.RerouteTopologyBuilds };
		};
		const auto OrdinaryDiscovery = MeasureDiscovery(ChainSchema);
		const auto ExpandedDiscovery = MeasureDiscovery(ExpandedDiscoverySchema);
		std::cout << "G21 optimized discovery [" << BuildFlavor << "] linked_nodes=1000 catalog=ordinary elapsed_ms="
			<< OrdinaryDiscovery.first << " topology_builds=" << OrdinaryDiscovery.second << '\n';
		std::cout << "G21 optimized discovery [" << BuildFlavor << "] linked_nodes=1000 catalog=expanded-256 elapsed_ms="
			<< ExpandedDiscovery.first << " topology_builds=" << ExpandedDiscovery.second << '\n';
		EditorGraphSchema RouteSchema = ChainSchema;
		RouteSchema.RerouteTypes = { EditorGraphPinType::Float };
		const auto MeasureRoute = [&](size_t KnotCount)
		{
			std::vector<EditorGraphVector3> Knots(KnotCount);
			for (size_t Index = 0; Index < KnotCount; ++Index)
				Knots[Index] = { static_cast<double>(Index * 20), 80.0, 0.0 };
			const auto Begin = std::chrono::steady_clock::now();
			EditorGraphWorkStats Work;
			const auto Decision = CanCreateEditorGraphWireRoute(*LongGraph, RouteSchema,
				LongGraph->Nodes.back().Pins[1].ID, Knots, std::nullopt, nullptr, &Work);
			const auto End = std::chrono::steady_clock::now();
			PF_CHECK(Tests, Decision.Allowed);
			return std::pair{ std::chrono::duration<double, std::milli>(End - Begin).count(),
				Work.RoutePrefixStatesValidated };
		};
		const auto ShortRoute = MeasureRoute(2);
		const auto LongRoute = MeasureRoute(32);
		std::cout << "G21 route preview [" << BuildFlavor << "] linked_nodes=1000 knots=2 elapsed_ms="
			<< ShortRoute.first << " prefix_states=" << ShortRoute.second << '\n';
		std::cout << "G21 route preview [" << BuildFlavor << "] linked_nodes=1000 knots=32 elapsed_ms="
			<< LongRoute.first << " prefix_states=" << LongRoute.second << '\n';

		// Action discovery is graph-context-sensitive. The popup cache keys this result
		// by the history graph revision, which must change when a committed edge changes
		// a schema policy decision.
		EditorGraphSchema ContextActionSchema = ChainSchema;
		ContextActionSchema.Key = "test.context-actions";
		ContextActionSchema.ConnectionPolicy = [](const EditorGraph& Graph, const EditorGraphConnectionPolicyContext& Context)
			-> std::optional<std::string>
			{
				if (std::any_of(Graph.Connections.begin(), Graph.Connections.end(), [&Context](const auto& Connection)
					{ return !Context.RemovesConnection(Connection.ID) && Context.Output.PinID == Connection.OutputPin &&
						Context.Input.PinID == Connection.InputPin; })) return std::nullopt;
				return std::any_of(Graph.Connections.begin(), Graph.Connections.end(), [&Context](const auto& Connection)
					{ return !Context.RemovesConnection(Connection.ID); })
					? std::optional<std::string>("No additional links in this context") : std::nullopt;
			};
		auto ContextGraph = CreateEditorGraph(ContextActionSchema, ID(0xabd));
		PF_CHECK(Tests, ContextGraph && AppendNode(*ContextGraph, ContextActionSchema, "test.step", 0.0) &&
			AppendNode(*ContextGraph, ContextActionSchema, "test.step", 200.0));
		if (ContextGraph)
		{
			EditorGraphCommandHistory ContextHistory(*ContextGraph);
			const auto ContextSource = ContextHistory.Graph().Nodes.front().Pins[1].ID;
			PF_CHECK(Tests, !DiscoverEditorGraphNodeActions(ContextHistory.Graph(), ContextActionSchema, ContextSource).empty());
			const auto InitialRevision = ContextHistory.GraphRevision();
			PF_CHECK(Tests, ConnectEditorGraphPins(ContextHistory, ContextActionSchema, ContextSource,
				ContextHistory.Graph().Nodes.back().Pins[0].ID).has_value());
			PF_CHECK(Tests, ContextHistory.GraphRevision() != InitialRevision);
			PF_CHECK(Tests, DiscoverEditorGraphNodeActions(ContextHistory.Graph(), ContextActionSchema, ContextSource).empty());
		}

		for (const size_t Count : { size_t{ 100 }, size_t{ 500 }, size_t{ 1000 } })
		{
			EditorGraph Sample = *LongGraph;
			Sample.Nodes.resize(Count);
			Sample.Connections.resize(Count - 1);
			const auto ValidateStart = std::chrono::steady_clock::now();
			const auto Diagnostics = ValidateEditorGraph(Sample, ChainSchema);
			const auto ValidateEnd = std::chrono::steady_clock::now();
			const auto DecisionStart = std::chrono::steady_clock::now();
			const auto ClosingEdge = CanConnectEditorGraphPins(Sample, ChainSchema,
				Sample.Nodes.back().Pins[1].ID, Sample.Nodes.front().Pins[0].ID);
			const auto DecisionEnd = std::chrono::steady_clock::now();
			PF_CHECK(Tests, Diagnostics.empty() && !ClosingEdge.Allowed);
			const auto ValidateMs = std::chrono::duration<double, std::milli>(ValidateEnd - ValidateStart).count();
			const auto DecisionMs = std::chrono::duration<double, std::milli>(DecisionEnd - DecisionStart).count();
			std::cout << "Graph traversal benchmark [" << BuildFlavor << "] " << Count
				<< " nodes: validate=" << ValidateMs << " ms, cycle-decision=" << DecisionMs << " ms\n";
		}

		EditorGraphSchema SingleOutputSchema = ChainSchema;
		SingleOutputSchema.Key = "test.single-output";
		SingleOutputSchema.Nodes[0].Pins[1].Cardinality = EditorGraphPinCardinality::Single;
		auto SmallGraph = CreateEditorGraph(SingleOutputSchema, ID(0xdef));
		PF_CHECK(Tests, SmallGraph && AppendNode(*SmallGraph, SingleOutputSchema, "test.step", 0.0) &&
			AppendNode(*SmallGraph, SingleOutputSchema, "test.step", 200.0) &&
			AppendNode(*SmallGraph, SingleOutputSchema, "test.step", 400.0));
		if (!SmallGraph) return;
		EditorGraphCommandHistory History(*SmallGraph);
		const auto SourcePin = History.Graph().Nodes[0].Pins[1].ID;
		const auto InputA = History.Graph().Nodes[1].Pins[0].ID;
		const auto InputB = History.Graph().Nodes[2].Pins[0].ID;
		PF_CHECK(Tests, ConnectEditorGraphPins(History, SingleOutputSchema, SourcePin, InputA).has_value());
		PF_CHECK(Tests, !CanConnectEditorGraphPins(History.Graph(), SingleOutputSchema, SourcePin, InputA).Allowed);
		const auto BeforeDuplicateConnect = History.Graph();
		PF_CHECK(Tests, !ConnectEditorGraphPins(History, SingleOutputSchema, SourcePin, InputA));
		PF_CHECK(Tests, History.Graph() == BeforeDuplicateConnect);
		const EditorGraph BeforeRejected = History.Graph();
		PF_CHECK(Tests, !CanConnectEditorGraphPins(History.Graph(), SingleOutputSchema, SourcePin, InputB).Allowed);
		PF_CHECK(Tests, !ConnectEditorGraphPins(History, SingleOutputSchema, SourcePin, InputB));
		PF_CHECK(Tests, History.Graph() == BeforeRejected && ValidateEditorGraph(History.Graph(), SingleOutputSchema).empty());
		const auto ExistingID = History.Graph().Connections.front().ID;
		PF_CHECK(Tests, CanConnectEditorGraphPins(History.Graph(), SingleOutputSchema, SourcePin, InputA, ExistingID).Allowed);
		PF_CHECK(Tests, ReconnectEditorGraphConnection(History, SingleOutputSchema, ExistingID, SourcePin, InputA).has_value());
		EditorGraph DuplicatePair = History.Graph();
		DuplicatePair.Connections.push_back({ ID(0x777777), SourcePin, InputA });
		const auto DuplicateDiagnostics = ValidateEditorGraph(DuplicatePair, SingleOutputSchema);
		PF_CHECK(Tests, std::any_of(DuplicateDiagnostics.begin(), DuplicateDiagnostics.end(),
			[](const auto& Diagnostic) { return Diagnostic.Code == "connection.duplicate"; }));
		PF_CHECK(Tests, !SerializeEditorGraph(DuplicatePair, SingleOutputSchema));
		const auto ValidSource = SerializeEditorGraph(History.Graph(), SingleOutputSchema);
		PF_CHECK(Tests, ValidSource.has_value());
		if (ValidSource)
		{
			std::string DuplicateSource = *ValidSource;
			const auto& Existing = History.Graph().Connections.front();
			const std::string Record = "    ,\n    {\"id\":\"" + ID(0x777778).ToString() + "\",\"output\":\"" +
				Existing.OutputPin.ToString() + "\",\"input\":\"" + Existing.InputPin.ToString() + "\"}\n";
			const auto ConnectionsEnd = DuplicateSource.rfind("\n  ]");
			PF_CHECK(Tests, ConnectionsEnd != std::string::npos);
			if (ConnectionsEnd != std::string::npos)
			{
				DuplicateSource.insert(ConnectionsEnd, Record);
				PF_CHECK(Tests, !DeserializeEditorGraph(DuplicateSource, SingleOutputSchema));
			}
		}
	}

	void TestConnectionPolicyContextsAndCapacity(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const auto FloatPin = [](std::string Key, EditorGraphPinDirection Direction,
			EditorGraphPinCardinality Cardinality = EditorGraphPinCardinality::Single)
		{
			return EditorGraphPinDefinition{ std::move(Key), "Value", Direction, EditorGraphPinType::Float,
				Cardinality, 0.0 };
		};
		EditorGraphSchema ContextSchema{ "test.policy-context", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true, {
				{ "test.live-output", "Live output", { FloatPin("shared", EditorGraphPinDirection::Output,
					EditorGraphPinCardinality::Multiple) } },
				{ "test.live-input", "Live input", { FloatPin("shared", EditorGraphPinDirection::Input) } },
				{ "test.allowed-target", "Allowed target", { FloatPin("shared", EditorGraphPinDirection::Input) } },
				{ "test.denied-target", "Denied target", { FloatPin("shared", EditorGraphPinDirection::Input) } },
				{ "test.allowed-source", "Allowed source", { FloatPin("shared", EditorGraphPinDirection::Output,
					EditorGraphPinCardinality::Multiple) } },
				{ "test.denied-source", "Denied source", { FloatPin("shared", EditorGraphPinDirection::Output,
					EditorGraphPinCardinality::Multiple) } }
			} };
		ContextSchema.ConnectionPolicy = [](const EditorGraph&, const EditorGraphConnectionPolicyContext& Context)
			-> std::optional<std::string>
		{
			if ((Context.Output.IsProspective && Context.Output.OwnerNodeTypeKey != "test.allowed-source") ||
				Context.Output.OwnerNodeTypeKey == "test.denied-source")
				return "Only the registered source node is permitted";
			if ((Context.Input.IsProspective && Context.Input.OwnerNodeTypeKey != "test.allowed-target") ||
				Context.Input.OwnerNodeTypeKey == "test.denied-target")
				return "Only the registered target node is permitted";
			const auto IsOutOfRange = [](const EditorGraphPolicyEndpoint& Endpoint)
				{ return !Endpoint.IsProspective && std::holds_alternative<double>(Endpoint.CurrentValue) &&
					std::get<double>(Endpoint.CurrentValue) > 3.0; };
			if (IsOutOfRange(Context.Output) || IsOutOfRange(Context.Input))
				return "The live endpoint value exceeds the domain range";
			return std::nullopt;
		};
		auto ContextGraph = CreateEditorGraph(ContextSchema, ID(0xc01));
		PF_CHECK(Tests, ContextGraph && AppendNode(*ContextGraph, ContextSchema, "test.live-output", 0.0) &&
			AppendNode(*ContextGraph, ContextSchema, "test.live-input", 100.0) &&
			AppendNode(*ContextGraph, ContextSchema, "test.allowed-target", 200.0) &&
			AppendNode(*ContextGraph, ContextSchema, "test.denied-target", 300.0) &&
			AppendNode(*ContextGraph, ContextSchema, "test.allowed-source", 400.0) &&
			AppendNode(*ContextGraph, ContextSchema, "test.denied-source", 500.0));
		if (!ContextGraph) return;
		ContextGraph->Nodes[0].Pins[0].DefaultValue = 2.5;
		ContextGraph->Nodes[1].Pins[0].DefaultValue = 2.5;
		const auto LiveOutputID = ContextGraph->Nodes[0].Pins[0].ID;
		const auto LiveInputID = ContextGraph->Nodes[1].Pins[0].ID;
		const auto TargetActions = DiscoverEditorGraphNodeActions(*ContextGraph, ContextSchema, LiveOutputID);
		PF_CHECK(Tests, TargetActions.size() == 1 && TargetActions.front().NodeTypeKey == "test.allowed-target" &&
			TargetActions.front().ConnectPinKey == "shared");
		const auto SourceActions = DiscoverEditorGraphNodeActions(*ContextGraph, ContextSchema, LiveInputID);
		PF_CHECK(Tests, SourceActions.size() == 1 && SourceActions.front().NodeTypeKey == "test.allowed-source");
		EditorGraphCommandHistory ContextHistory(*ContextGraph);
		auto CreatedTarget = CreateEditorGraphNodeAndConnect(ContextHistory, ContextSchema,
			"test.allowed-target", "shared", LiveOutputID, 700.0, 50.0);
		PF_CHECK(Tests, CreatedTarget && ValidateEditorGraph(ContextHistory.Graph(), ContextSchema).empty());
		if (CreatedTarget) PF_CHECK(Tests, ContextHistory.Graph().Connections.back().OutputPin == LiveOutputID);
		const auto BeforeForbiddenCreation = ContextHistory.Graph();
		const auto BeforeForbiddenRevision = ContextHistory.GraphRevision();
		const bool BeforeForbiddenUndo = ContextHistory.CanUndo();
		PF_CHECK(Tests, !CreateEditorGraphNodeAndConnect(ContextHistory, ContextSchema,
			"test.denied-target", "shared", LiveOutputID, 800.0, 50.0));
		PF_CHECK(Tests, ContextHistory.Graph() == BeforeForbiddenCreation &&
			ContextHistory.GraphRevision() == BeforeForbiddenRevision && ContextHistory.CanUndo() == BeforeForbiddenUndo);
		const auto LiveSourceNodeID = ContextHistory.Graph().Nodes.front().ID;
		PF_CHECK(Tests, DeleteEditorGraphNodes(ContextHistory, ContextSchema, { LiveSourceNodeID }));
		PF_CHECK(Tests, DiscoverEditorGraphNodeActions(ContextHistory.Graph(), ContextSchema, LiveOutputID).empty());
		auto CreatedSource = CreateEditorGraphNodeAndConnect(ContextHistory, ContextSchema,
			"test.allowed-source", "shared", LiveInputID, 900.0, 50.0);
		PF_CHECK(Tests, CreatedSource && ValidateEditorGraph(ContextHistory.Graph(), ContextSchema).empty());
		if (CreatedSource)
		{
			const auto NewOutput = std::find_if(ContextHistory.Graph().Nodes.begin(), ContextHistory.Graph().Nodes.end(),
				[&CreatedSource](const auto& Node) { return Node.ID == CreatedSource->NodeID; });
			PF_CHECK(Tests, NewOutput != ContextHistory.Graph().Nodes.end() &&
				ContextHistory.Graph().Connections.back().InputPin == LiveInputID);
		}
		auto OutOfRangeGraph = *ContextGraph;
		OutOfRangeGraph.Nodes[0].Pins[0].DefaultValue = 4.0;
		PF_CHECK(Tests, DiscoverEditorGraphNodeActions(OutOfRangeGraph, ContextSchema, LiveOutputID).empty());
		EditorGraphCommandHistory OutOfRangeHistory(OutOfRangeGraph);
		const auto OutOfRangeBefore = OutOfRangeHistory.Graph();
		PF_CHECK(Tests, !CreateEditorGraphNodeAndConnect(OutOfRangeHistory, ContextSchema,
			"test.allowed-target", "shared", LiveOutputID, 600.0, 40.0));
		PF_CHECK(Tests, OutOfRangeHistory.Graph() == OutOfRangeBefore && !OutOfRangeHistory.CanUndo());

		EditorGraphSchema ReplacementSchema{ "test.policy-replacement", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true, {
				{ "test.source", "Source", { FloatPin("out", EditorGraphPinDirection::Output,
					EditorGraphPinCardinality::Multiple) } },
				{ "test.sink", "Sink", { FloatPin("a", EditorGraphPinDirection::Input),
					FloatPin("b", EditorGraphPinDirection::Input) } }
			} };
		ReplacementSchema.ConnectionPolicy = [](const EditorGraph& Graph, const EditorGraphConnectionPolicyContext& Context)
			-> std::optional<std::string>
		{
			if (!Context.Input.OwnerNodeID) return std::nullopt;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> ConnectedInputs;
			for (const auto& Connection : Graph.Connections)
			{
				if (Context.RemovesConnection(Connection.ID)) continue;
				for (const auto& Node : Graph.Nodes)
					for (const auto& Pin : Node.Pins)
						if (Pin.ID == Connection.InputPin && Node.ID == *Context.Input.OwnerNodeID)
							ConnectedInputs.insert(Pin.ID);
			}
			if (!Context.Input.IsProspective && Context.Input.PinID)
				ConnectedInputs.insert(*Context.Input.PinID);
			return ConnectedInputs.size() > 1
				? std::optional<std::string>("Only one input on a node may be connected") : std::nullopt;
		};
		auto ReplacementGraph = CreateEditorGraph(ReplacementSchema, ID(0xc02));
		PF_CHECK(Tests, ReplacementGraph && AppendNode(*ReplacementGraph, ReplacementSchema, "test.source", 0.0) &&
			AppendNode(*ReplacementGraph, ReplacementSchema, "test.source", 100.0) &&
			AppendNode(*ReplacementGraph, ReplacementSchema, "test.sink", 200.0) &&
			AppendNode(*ReplacementGraph, ReplacementSchema, "test.sink", 300.0));
		if (ReplacementGraph)
		{
			EditorGraphCommandHistory ReplacementHistory(*ReplacementGraph);
			const auto SourceA = ReplacementHistory.Graph().Nodes[0].Pins[0].ID;
			const auto SourceB = ReplacementHistory.Graph().Nodes[1].Pins[0].ID;
			const auto InputA = ReplacementHistory.Graph().Nodes[2].Pins[0].ID;
			const auto InputB = ReplacementHistory.Graph().Nodes[2].Pins[1].ID;
			const auto InputC = ReplacementHistory.Graph().Nodes[3].Pins[0].ID;
			const auto StalePreview = CanConnectEditorGraphPins(ReplacementHistory.Graph(), ReplacementSchema, SourceB, InputB);
			PF_CHECK(Tests, StalePreview.Allowed);
			PF_CHECK(Tests, ConnectEditorGraphPins(ReplacementHistory, ReplacementSchema, SourceA, InputA));
			const auto AfterFirstConnection = ReplacementHistory.Graph();
			const auto AfterFirstRevision = ReplacementHistory.GraphRevision();
			const bool AfterFirstUndo = ReplacementHistory.CanUndo();
			PF_CHECK(Tests, !ConnectEditorGraphPins(ReplacementHistory, ReplacementSchema, SourceB, InputB));
			PF_CHECK(Tests, ReplacementHistory.Graph() == AfterFirstConnection &&
				ReplacementHistory.GraphRevision() == AfterFirstRevision && ReplacementHistory.CanUndo() == AfterFirstUndo);
			const auto StableConnectionID = ReplacementHistory.Graph().Connections.front().ID;
			const auto ReconnectDecision = CanConnectEditorGraphPins(ReplacementHistory.Graph(), ReplacementSchema,
				SourceA, InputB, StableConnectionID);
			PF_CHECK(Tests, ReconnectDecision.Allowed && ReconnectDecision.RemovedConnectionIDs.size() == 1 &&
				ReconnectDecision.RemovedConnectionIDs.front() == StableConnectionID);
			PF_CHECK(Tests, ReconnectEditorGraphConnection(ReplacementHistory, ReplacementSchema,
				StableConnectionID, SourceA, InputB));
			PF_CHECK(Tests, ReplacementHistory.Graph().Connections.size() == 1 &&
				ReplacementHistory.Graph().Connections.front().ID == StableConnectionID &&
				ReplacementHistory.Graph().Connections.front().InputPin == InputB &&
				ValidateEditorGraph(ReplacementHistory.Graph(), ReplacementSchema).empty());
			const auto BeforeExtra = ReplacementHistory.Graph();
			PF_CHECK(Tests, !ConnectEditorGraphPins(ReplacementHistory, ReplacementSchema, SourceB, InputA));
			PF_CHECK(Tests, ReplacementHistory.Graph() == BeforeExtra);
			PF_CHECK(Tests, ReplacementHistory.Undo() && ReplacementHistory.Graph().Connections.front().InputPin == InputA);
			PF_CHECK(Tests, ReplacementHistory.Redo() && ReplacementHistory.Graph().Connections.front().InputPin == InputB &&
				ReplacementHistory.Graph().Connections.front().ID == StableConnectionID);

			const auto SourceCNode = CreateEditorGraphNode(ReplacementSchema, "test.source", 300.0, 0.0);
			PF_CHECK(Tests, SourceCNode.has_value());
			if (SourceCNode)
			{
				const auto SourceC = SourceCNode->Pins.front().ID;
				std::string AddError;
				PF_CHECK(Tests, ReplacementHistory.Execute(ReplacementSchema,
					[Node = *SourceCNode](EditorGraph& Graph) mutable -> std::expected<void, std::string>
					{ Graph.Nodes.push_back(std::move(Node)); return {}; }, AddError));
				PF_CHECK(Tests, ConnectEditorGraphPins(ReplacementHistory, ReplacementSchema, SourceB, InputC));
				const auto ReplacedID = std::find_if(ReplacementHistory.Graph().Connections.begin(),
					ReplacementHistory.Graph().Connections.end(), [InputC](const auto& Connection) { return Connection.InputPin == InputC; })->ID;
				const auto ReplacingDecision = CanConnectEditorGraphPins(ReplacementHistory.Graph(), ReplacementSchema,
					SourceC, InputC);
				PF_CHECK(Tests, ReplacingDecision.Allowed && ReplacingDecision.RemovedConnectionIDs.size() == 1 &&
					ReplacingDecision.RemovedConnectionIDs.front() == ReplacedID);
				PF_CHECK(Tests, ConnectEditorGraphPins(ReplacementHistory, ReplacementSchema, SourceC, InputC));
				PF_CHECK(Tests, ReplacementHistory.Graph().Connections.size() == 2 &&
					std::none_of(ReplacementHistory.Graph().Connections.begin(), ReplacementHistory.Graph().Connections.end(),
						[ReplacedID](const auto& Connection) { return Connection.ID == ReplacedID; }));
			}
			ReplacementSchema.ReplaceOccupiedInput = false;
			PF_CHECK(Tests, !CanConnectEditorGraphPins(ReplacementHistory.Graph(), ReplacementSchema, SourceA, InputC).Allowed);
		}

		EditorGraphSchema ConnectionLimitSchema{ "test.connection-capacity", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true, {
				{ "test.many-outputs", "Many outputs", [&FloatPin]()
					{ std::vector<EditorGraphPinDefinition> Pins; for (size_t I = 0; I < 64; ++I)
						Pins.push_back(FloatPin("out-" + std::to_string(I), EditorGraphPinDirection::Output,
							EditorGraphPinCardinality::Multiple)); return Pins; }() },
				{ "test.many-inputs", "Many inputs", [&FloatPin]()
					{ std::vector<EditorGraphPinDefinition> Pins; for (size_t I = 0; I < 64; ++I)
						Pins.push_back(FloatPin("in-" + std::to_string(I), EditorGraphPinDirection::Input,
							EditorGraphPinCardinality::Single)); return Pins; }() }
			} };
		auto AtConnectionLimit = CreateEditorGraph(ConnectionLimitSchema, ID(0xc04));
		PF_CHECK(Tests, AtConnectionLimit.has_value());
		if (AtConnectionLimit)
		{
			for (size_t NodeIndex = 0; NodeIndex < 256; ++NodeIndex)
			{
				auto Node = CreateEditorGraphNode(ConnectionLimitSchema,
					NodeIndex < 128 ? "test.many-outputs" : "test.many-inputs", static_cast<double>(NodeIndex), 0.0);
				PF_CHECK(Tests, Node.has_value());
				if (Node) AtConnectionLimit->Nodes.push_back(std::move(*Node));
			}
			if (AtConnectionLimit->Nodes.size() == 256)
			{
				for (size_t OutputNode = 0; OutputNode < 128; ++OutputNode)
					for (size_t PinIndex = 0; PinIndex < 64; ++PinIndex)
						AtConnectionLimit->Connections.push_back({ ID(0xd00000 + OutputNode * 64 + PinIndex),
							AtConnectionLimit->Nodes[OutputNode].Pins[PinIndex].ID,
							AtConnectionLimit->Nodes[128 + OutputNode].Pins[PinIndex].ID });
				PF_CHECK(Tests, AtConnectionLimit->Connections.size() == EditorGraphMaximumConnections &&
					ValidateEditorGraph(*AtConnectionLimit, ConnectionLimitSchema).empty());
				const PulseForge::UUID ExistingInput = AtConnectionLimit->Nodes[128].Pins[0].ID;
				const PulseForge::UUID ExistingOutput = AtConnectionLimit->Nodes[0].Pins[0].ID;
				PF_CHECK(Tests, DiscoverEditorGraphNodeActions(*AtConnectionLimit, ConnectionLimitSchema, ExistingOutput).empty());
				auto OneBelow = *AtConnectionLimit;
				OneBelow.Connections.pop_back();
				PF_CHECK(Tests, !DiscoverEditorGraphNodeActions(OneBelow, ConnectionLimitSchema, ExistingOutput).empty());
				const auto InputOriginActions = DiscoverEditorGraphNodeActions(*AtConnectionLimit,
					ConnectionLimitSchema, ExistingInput);
				PF_CHECK(Tests, !InputOriginActions.empty());
				EditorGraphCommandHistory CapacityHistory(*AtConnectionLimit);
				const auto ExistingConnectionID = CapacityHistory.Graph().Connections.front().ID;
				auto Replacement = CreateEditorGraphNodeAndConnect(CapacityHistory, ConnectionLimitSchema,
					"test.many-outputs", "out-0", ExistingInput, 3000.0, 0.0);
				PF_CHECK(Tests, Replacement && CapacityHistory.Graph().Connections.size() == EditorGraphMaximumConnections &&
					std::none_of(CapacityHistory.Graph().Connections.begin(), CapacityHistory.Graph().Connections.end(),
						[ExistingConnectionID](const auto& Connection) { return Connection.ID == ExistingConnectionID; }));
			}
		}

		EditorGraphSchema NodeLimitSchema{ "test.node-capacity", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true, { { "test.empty", "Empty", {} } } };
		auto AtNodeLimit = CreateEditorGraph(NodeLimitSchema, ID(0xc03));
		PF_CHECK(Tests, AtNodeLimit.has_value());
		if (AtNodeLimit)
		{
			AtNodeLimit->Nodes.reserve(EditorGraphMaximumNodes);
			for (size_t Index = 0; Index < EditorGraphMaximumNodes; ++Index)
			{
				auto Node = CreateEditorGraphNode(NodeLimitSchema, "test.empty", static_cast<double>(Index), 0.0);
				if (!Node) { PF_CHECK(Tests, false); return; }
				AtNodeLimit->Nodes.push_back(std::move(*Node));
			}
			PF_CHECK(Tests, DiscoverEditorGraphNodeActions(*AtNodeLimit, NodeLimitSchema).empty());
			EditorGraphCommandHistory FullNodeHistory(*AtNodeLimit);
			PF_CHECK(Tests, !AddEditorGraphNode(FullNodeHistory, NodeLimitSchema, "test.empty", 0.0, 0.0) &&
				FullNodeHistory.Graph().Nodes.size() == EditorGraphMaximumNodes && !FullNodeHistory.CanUndo());
			AtNodeLimit->Nodes.pop_back();
			PF_CHECK(Tests, DiscoverEditorGraphNodeActions(*AtNodeLimit, NodeLimitSchema).size() == 1);
		}
	}
}

namespace
{
	void TestBatchPinConnectionMove(TestRunner& Tests)
	{
		const auto Schema = MakeMaterialExpressionFixtureSchema();
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "material.scalar", 0.0) &&
			AppendNode(*Graph, Schema, "material.scalar", 160.0) &&
			AppendNode(*Graph, Schema, "material.multiply", 320.0) &&
			AppendNode(*Graph, Schema, "material.multiply", 480.0));
		if (!Graph) return;
		const auto SourceA = FindPin(Graph->Nodes[0], "value")->ID;
		const auto SourceB = FindPin(Graph->Nodes[1], "value")->ID;
		const auto InputA = FindPin(Graph->Nodes[2], "a")->ID;
		const auto InputB = FindPin(Graph->Nodes[3], "a")->ID;
		EditorGraphCommandHistory History(*Graph);
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, SourceA, InputA));
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, SourceA, InputB));
		const EditorGraph Before = History.Graph();
		std::vector<EditorGraphConnection> Captured;
		for (const auto& Connection : Before.Connections)
			if (Connection.OutputPin == SourceA) Captured.push_back(Connection);
		const auto Preview = CanMoveEditorGraphPinConnections(Before, Schema, SourceA, SourceB, Captured);
		PF_CHECK(Tests, Preview.Allowed && !Preview.NoOp && Preview.MovedConnectionIDs.size() == 2);
		EditorGraphCommandHistory OneWireHistory(Before);
		const std::array OneWire{ Captured.front() };
		const auto OneWirePreview = CanMoveEditorGraphPinConnections(Before, Schema, SourceA, SourceB,
			OneWire, nullptr, false);
		PF_CHECK(Tests, OneWirePreview.Allowed && OneWirePreview.MovedConnectionIDs.size() == 1);
		PF_CHECK(Tests, MoveEditorGraphPinConnections(OneWireHistory, Schema, SourceA, SourceB, OneWire, false));
		PF_CHECK(Tests, std::any_of(OneWireHistory.Graph().Connections.begin(), OneWireHistory.Graph().Connections.end(),
			[&Captured](const auto& Link) { return Link.ID == Captured[0].ID && Link.OutputPin != Captured[0].OutputPin; }) &&
			std::any_of(OneWireHistory.Graph().Connections.begin(), OneWireHistory.Graph().Connections.end(),
			[&Captured](const auto& Link) { return Link.ID == Captured[1].ID && Link.OutputPin == Captured[1].OutputPin; }));
		PF_CHECK(Tests, OneWireHistory.Undo() && OneWireHistory.Graph() == Before);
		PF_CHECK(Tests, MoveEditorGraphPinConnections(History, Schema, SourceA, SourceB, Captured));
		const EditorGraph Moved = History.Graph();
		PF_CHECK(Tests, Moved.Connections.size() == Before.Connections.size());
		for (const auto& Original : Captured)
		{
			const auto Link = std::find_if(Moved.Connections.begin(), Moved.Connections.end(),
				[&Original](const auto& Candidate) { return Candidate.ID == Original.ID; });
			PF_CHECK(Tests, Link != Moved.Connections.end() && Link->OutputPin == SourceB);
		}
		PF_CHECK(Tests, History.Undo() && History.Graph() == Before);
		PF_CHECK(Tests, History.Redo() && History.Graph() == Moved);
		PF_CHECK(Tests, History.Undo());

		auto Restricted = Schema;
		Restricted.ConnectionPolicy = [](const EditorGraph&, const EditorGraphConnectionPolicyContext& Context)
			-> std::optional<std::string>
		{
			if (Context.Purpose == EditorGraphConnectionPolicyPurpose::ProposedOperation &&
				Context.ProposedConnections.size() == 2 && Context.Input.PinKey == "a" &&
				Context.Input.OwnerNodeID == Context.ProposedConnections.back().Input.OwnerNodeID)
				return "The complete moved group is forbidden";
			return std::nullopt;
		};
		const auto Revision = History.GraphRevision();
		const auto Rejected = CanMoveEditorGraphPinConnections(History.Graph(), Restricted, SourceA, SourceB, Captured);
		PF_CHECK(Tests, !Rejected.Allowed && Rejected.Reason == "The complete moved group is forbidden");
		PF_CHECK(Tests, !MoveEditorGraphPinConnections(History, Restricted, SourceA, SourceB, Captured));
		PF_CHECK(Tests, History.Graph() == Before && History.GraphRevision() == Revision && History.CanRedo());

		auto Stale = Captured;
		Stale.front().InputPin = InputB;
		const auto StaleDecision = CanMoveEditorGraphPinConnections(Before, Schema, SourceA, SourceB, Stale);
		PF_CHECK(Tests, !StaleDecision.Allowed && StaleDecision.Reason.find("connection set changed") != std::string::npos);
		auto ChangedSourceGraph = Before;
		ChangedSourceGraph.Connections.push_back({ ID(0xc302), SourceA, InputB });
		const auto ChangedSource = CanMoveEditorGraphPinConnections(ChangedSourceGraph, Schema,
			SourceA, SourceB, Captured);
		PF_CHECK(Tests, !ChangedSource.Allowed && ChangedSource.Reason.find("connection set changed") != std::string::npos);

		EditorGraphCommandHistory InputHistory(Before);
		const auto ExistingID = Before.Connections[0].ID;
		const auto DisplacedID = Before.Connections[1].ID;
		const auto InputCaptured = std::span<const EditorGraphConnection>(&Before.Connections[0], 1);
		const auto InputDecision = CanMoveEditorGraphPinConnections(Before, Schema, InputA, InputB, InputCaptured);
		PF_CHECK(Tests, InputDecision.Allowed && InputDecision.DisplacedConnectionIDs.size() == 1 &&
			InputDecision.DisplacedConnectionIDs.front() == DisplacedID);
		PF_CHECK(Tests, MoveEditorGraphPinConnections(InputHistory, Schema, InputA, InputB, InputCaptured));
		PF_CHECK(Tests, InputHistory.Graph().Connections.size() == 1 &&
			InputHistory.Graph().Connections.front().ID == ExistingID &&
			InputHistory.Graph().Connections.front().InputPin == InputB);
		const auto AfterInputMove = InputHistory.Graph();
		PF_CHECK(Tests, InputHistory.Undo() && InputHistory.Graph() == Before);
		PF_CHECK(Tests, InputHistory.Redo() && InputHistory.Graph() == AfterInputMove);
		auto NoReplace = Schema;
		NoReplace.ReplaceOccupiedInput = false;
		PF_CHECK(Tests, !CanMoveEditorGraphPinConnections(Before, NoReplace, InputA, InputB, InputCaptured).Allowed);

		EditorGraphSchema CapacitySchema{ "test.pin-move-output-capacity", 1, EditorGraphCyclePolicy::Reject,
			EditorGraphCyclePolicy::Allow, true, {
				{ "test.fanout", "Fan-out", { { "out", "Out", EditorGraphPinDirection::Output,
					EditorGraphPinType::Float, EditorGraphPinCardinality::Multiple, 0.0 } } },
				{ "test.single-output", "Single output", { { "out", "Out", EditorGraphPinDirection::Output,
					EditorGraphPinType::Float, EditorGraphPinCardinality::Single, 0.0 } } },
				{ "test.multi-input", "Multi input", { { "in", "In", EditorGraphPinDirection::Input,
					EditorGraphPinType::Float, EditorGraphPinCardinality::Multiple, 0.0 } } }
			} };
		auto CapacityGraph = FixtureGraph(CapacitySchema);
		PF_CHECK(Tests, CapacityGraph && AppendNode(*CapacityGraph, CapacitySchema, "test.fanout", 0.0) &&
			AppendNode(*CapacityGraph, CapacitySchema, "test.single-output", 100.0) &&
			AppendNode(*CapacityGraph, CapacitySchema, "test.multi-input", 200.0) &&
			AppendNode(*CapacityGraph, CapacitySchema, "test.multi-input", 300.0));
		if (CapacityGraph)
		{
			const auto Fanout = CapacityGraph->Nodes[0].Pins[0].ID;
			const auto Single = CapacityGraph->Nodes[1].Pins[0].ID;
			const auto InputOne = CapacityGraph->Nodes[2].Pins[0].ID;
			const auto InputTwo = CapacityGraph->Nodes[3].Pins[0].ID;
			EditorGraphCommandHistory CapacityHistory(*CapacityGraph);
			PF_CHECK(Tests, ConnectEditorGraphPins(CapacityHistory, CapacitySchema, Fanout, InputOne));
			PF_CHECK(Tests, ConnectEditorGraphPins(CapacityHistory, CapacitySchema, Fanout, InputTwo));
			std::vector<EditorGraphConnection> CapacityCaptured(CapacityHistory.Graph().Connections.begin(),
				CapacityHistory.Graph().Connections.end());
			const auto Capacity = CanMoveEditorGraphPinConnections(CapacityHistory.Graph(), CapacitySchema,
				Fanout, Single, CapacityCaptured);
			PF_CHECK(Tests, !Capacity.Allowed && Capacity.Reason.find("single-cardinality output") != std::string::npos);
		}
		if (CapacityGraph)
		{
			auto DuplicateSchema = CapacitySchema;
			for (auto& Definition : DuplicateSchema.Nodes)
				if (Definition.TypeKey == "test.multi-input")
					Definition.Pins.front().Cardinality = EditorGraphPinCardinality::Multiple;
			auto DuplicateGraph = CreateEditorGraph(DuplicateSchema, ID(0xc303));
			PF_CHECK(Tests, DuplicateGraph && AppendNode(*DuplicateGraph, DuplicateSchema, "test.fanout", 0.0) &&
				AppendNode(*DuplicateGraph, DuplicateSchema, "test.fanout", 100.0) &&
				AppendNode(*DuplicateGraph, DuplicateSchema, "test.multi-input", 200.0) &&
				AppendNode(*DuplicateGraph, DuplicateSchema, "test.multi-input", 300.0));
			if (DuplicateGraph)
			{
				const auto OutputA = DuplicateGraph->Nodes[0].Pins[0].ID;
				const auto OutputB = DuplicateGraph->Nodes[1].Pins[0].ID;
				const auto TargetA = DuplicateGraph->Nodes[2].Pins[0].ID;
				const auto TargetB = DuplicateGraph->Nodes[3].Pins[0].ID;
				EditorGraphCommandHistory DuplicateHistory(*DuplicateGraph);
				PF_CHECK(Tests, ConnectEditorGraphPins(DuplicateHistory, DuplicateSchema, OutputA, TargetA));
				PF_CHECK(Tests, ConnectEditorGraphPins(DuplicateHistory, DuplicateSchema, OutputA, TargetB));
				PF_CHECK(Tests, ConnectEditorGraphPins(DuplicateHistory, DuplicateSchema, OutputB, TargetA));
				std::vector<EditorGraphConnection> DuplicateCaptured;
				for (const auto& Connection : DuplicateHistory.Graph().Connections)
					if (Connection.OutputPin == OutputA) DuplicateCaptured.push_back(Connection);
				const auto Duplicate = CanMoveEditorGraphPinConnections(DuplicateHistory.Graph(), DuplicateSchema,
					OutputA, OutputB, DuplicateCaptured);
				PF_CHECK(Tests, !Duplicate.Allowed && Duplicate.Reason.find("Duplicate connections") != std::string::npos);
			}
		}
	}
}

void RunEditorGraphModelTests(TestRunner& Tests)
{
	{
		using namespace PulseForgeEditor;
		const auto SameConnections = [](const auto& Left, const auto& Right)
		{
			if (Left.size() != Right.size()) return false;
			return std::all_of(Left.begin(), Left.end(), [&Right](const auto& Link)
			{
				return std::any_of(Right.begin(), Right.end(), [&Link](const auto& Candidate)
					{ return Link.ID == Candidate.ID && Link.OutputPin == Candidate.OutputPin &&
						Link.InputPin == Candidate.InputPin; });
			});
		};
		auto Schema = MakeMaterialExpressionFixtureSchema();
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "material.scalar", 0.0) &&
			AppendNode(*Graph, Schema, "material.multiply", 240.0) &&
			AppendNode(*Graph, Schema, "material.multiply", 480.0) &&
			AppendNode(*Graph, Schema, "material.color", 720.0) &&
			AppendNode(*Graph, Schema, "material.output", 960.0));
		if (Graph)
		{
			const auto Source = FindPin(*Graph, "material.scalar", "value")->ID;
			const auto InputA = FindPin(*Graph, "material.multiply", "a")->ID;
			const auto InputB = FindPin(*Graph, "material.multiply", "a", 1)->ID;
			const auto VectorSource = FindPin(*Graph, "material.color", "color")->ID;
			const auto VectorTarget = FindPin(*Graph, "material.output", "surface")->ID;
			const std::vector<EditorGraphVector3> NoKnots;
			PF_CHECK(Tests, !CanCreateEditorGraphWireRoute(*Graph, Schema, Source, NoKnots, InputA).Allowed);

			EditorGraphCommandHistory OutputHistory(*Graph);
			const std::array<EditorGraphVector3, 2> OutputKnots{ EditorGraphVector3{ 330.0, 80.0, 0.0 },
				EditorGraphVector3{ 400.0, 140.0, 0.0 } };
			const auto OutputBefore = OutputHistory.Graph();
			const auto OutputDecision = CanCreateEditorGraphWireRoute(OutputBefore, Schema, Source, OutputKnots, InputA);
			const auto OutputCreated = CreateEditorGraphWireRoute(OutputHistory, Schema, Source, OutputKnots, InputA);
			PF_CHECK(Tests, OutputDecision.Allowed && OutputCreated && OutputCreated->RerouteNodeIDs.size() == 2 &&
				OutputHistory.Graph().Connections.size() == OutputBefore.Connections.size() + 3 &&
				ValidateEditorGraph(OutputHistory.Graph(), Schema).empty());
			if (OutputCreated)
			{
				const auto After = OutputHistory.Graph();
				const auto ExpectedNodeIDs = OutputCreated->RerouteNodeIDs;
				const auto ExpectedConnectionIDs = OutputCreated->ConnectionIDs;
				PF_CHECK(Tests, OutputHistory.Undo() &&
					OutputHistory.Graph().Nodes.size() == OutputBefore.Nodes.size() + 2 &&
					OutputHistory.Graph().Connections.size() == OutputBefore.Connections.size() + 2);
				PF_CHECK(Tests, OutputHistory.Undo() &&
					OutputHistory.Graph().Nodes.size() == OutputBefore.Nodes.size() + 1 &&
					OutputHistory.Graph().Connections.size() == OutputBefore.Connections.size() + 1);
				PF_CHECK(Tests, OutputHistory.Undo() && OutputHistory.Graph() == OutputBefore);
				PF_CHECK(Tests, OutputHistory.Redo() && OutputHistory.Redo() && OutputHistory.Redo() &&
					OutputHistory.Graph() == After && OutputHistory.Undo() && OutputHistory.Undo() &&
					OutputHistory.Graph().Nodes.size() == OutputBefore.Nodes.size() + 1 &&
					OutputHistory.Redo() && OutputHistory.Graph().Nodes.size() == OutputBefore.Nodes.size() + 2 &&
					OutputHistory.Undo() && OutputHistory.Undo() && OutputHistory.Graph() == OutputBefore &&
					OutputHistory.CanRedo());
				PF_CHECK(Tests, OutputHistory.Redo() && OutputHistory.Redo() && OutputHistory.Redo() &&
					OutputHistory.Graph() == After);
				PF_CHECK(Tests, SameConnections(OutputHistory.Graph().Connections, After.Connections));
				PF_CHECK(Tests, std::all_of(ExpectedNodeIDs.begin(), ExpectedNodeIDs.end(), [&](const auto NodeID)
					{ return std::any_of(OutputHistory.Graph().Nodes.begin(), OutputHistory.Graph().Nodes.end(),
						[NodeID](const auto& Node) { return Node.ID == NodeID; }); }) &&
					std::all_of(ExpectedConnectionIDs.begin(), ExpectedConnectionIDs.end(), [&](const auto ConnectionID)
					{ return std::any_of(OutputHistory.Graph().Connections.begin(), OutputHistory.Graph().Connections.end(),
						[ConnectionID](const auto& Link) { return Link.ID == ConnectionID; }); }));
			}

			const auto CheckCompletedStepRoute = [&](size_t KnotCount, bool ShouldBeAllowed)
			{
				std::vector<EditorGraphVector3> Knots(KnotCount, EditorGraphVector3{ 300.0, 260.0, 0.0 });
				EditorGraphCommandHistory History(*Graph);
				const auto Before = History.Graph();
				const auto Revision = History.GraphRevision();
				const auto Preview = CanCreateEditorGraphWireRoute(History.Graph(), Schema, Source, Knots, InputA,
					nullptr, nullptr, nullptr, &History);
				if (!ShouldBeAllowed)
				{
					const bool Dirty = History.IsDirty();
					const bool CanUndo = History.CanUndo();
					const bool CanRedo = History.CanRedo();
					const auto Commit = CreateEditorGraphWireRoute(History, Schema, Source, Knots, InputA);
					PF_CHECK(Tests, !Preview.Allowed && !Commit && History.Graph() == Before &&
						History.GraphRevision() == Revision && History.IsDirty() == Dirty &&
						History.CanUndo() == CanUndo && History.CanRedo() == CanRedo);
					return;
				}
				const auto Commit = CreateEditorGraphWireRoute(History, Schema, Source, Knots, InputA);
				PF_CHECK(Tests, Preview.Allowed && Commit && History.Graph().Nodes.size() == Before.Nodes.size() + KnotCount &&
					History.Graph().Connections.size() == Before.Connections.size() + KnotCount + 1);
				if (!Commit) return;
				const auto After = History.Graph();
				for (size_t Step = 0; Step < KnotCount + 1; ++Step) PF_CHECK(Tests, History.Undo());
				PF_CHECK(Tests, History.Graph() == Before && !History.CanUndo());
				for (size_t Step = 0; Step < KnotCount + 1; ++Step) PF_CHECK(Tests, History.Redo());
				PF_CHECK(Tests, History.Graph() == After &&
					std::all_of(Commit->RerouteNodeIDs.begin(), Commit->RerouteNodeIDs.end(), [&](const auto NodeID)
					{ return std::any_of(After.Nodes.begin(), After.Nodes.end(),
						[NodeID](const auto& Node) { return Node.ID == NodeID; }); }) &&
					std::all_of(Commit->ConnectionIDs.begin(), Commit->ConnectionIDs.end(), [&](const auto ConnectionID)
					{ return std::any_of(After.Connections.begin(), After.Connections.end(),
						[ConnectionID](const auto& Link) { return Link.ID == ConnectionID; }); }));
			};
			CheckCompletedStepRoute(64, true);
			CheckCompletedStepRoute(127, true);
			std::vector<EditorGraphVector3> OverStepRoute(128, EditorGraphVector3{ 300.0, 260.0, 0.0 });
			CheckCompletedStepRoute(OverStepRoute.size(), false);

			EditorGraphSchema PrefixPolicySchema = Schema;
			PrefixPolicySchema.ReroutePolicyAware = true;
			PrefixPolicySchema.GraphValidationPolicy = [](const EditorGraph& Candidate)
				-> std::optional<EditorGraphDiagnostic>
			{
				const size_t RerouteCount = static_cast<size_t>(std::count_if(Candidate.Nodes.begin(), Candidate.Nodes.end(),
					[](const auto& Node) { return IsEditorGraphRerouteType(Node.TypeKey); }));
				if (RerouteCount == 1) return EditorGraphDiagnostic{ "test.route_prefix", "One-knot prefixes are not restorable", {} };
				return std::nullopt;
			};
			const std::array<EditorGraphVector3, 2> PrefixKnots{ EditorGraphVector3{ 350.0, 280.0, 0.0 },
				EditorGraphVector3{ 410.0, 320.0, 0.0 } };
			const auto PrefixPreview = CanCreateEditorGraphWireRoute(*Graph, PrefixPolicySchema, Source, PrefixKnots, InputA);
			EditorGraphCommandHistory PrefixHistory(*Graph);
			const auto PrefixBefore = PrefixHistory.Graph();
			const auto PrefixRevision = PrefixHistory.GraphRevision();
			const auto PrefixCommit = CreateEditorGraphWireRoute(PrefixHistory, PrefixPolicySchema, Source, PrefixKnots, InputA);
			PF_CHECK(Tests, !PrefixPreview.Allowed && !PrefixCommit && PrefixHistory.Graph() == PrefixBefore &&
				PrefixHistory.GraphRevision() == PrefixRevision && !PrefixHistory.IsDirty() && !PrefixHistory.CanUndo());

			EditorGraphCommandHistory OpenHistory(*Graph);
			const std::array<EditorGraphVector3, 2> OpenKnots{ EditorGraphVector3{ 330.0, -40.0, 0.0 },
				EditorGraphVector3{ 390.0, 20.0, 0.0 } };
			const auto OpenCreated = CreateEditorGraphWireRoute(OpenHistory, Schema, Source, OpenKnots);
			PF_CHECK(Tests, OpenCreated && OpenCreated->OpenEnded && OpenCreated->ConnectionIDs.size() == 2 &&
				ValidateEditorGraph(OpenHistory.Graph(), Schema).empty());
			if (OpenCreated)
			{
				const auto OpenAfter = OpenHistory.Graph();
				PF_CHECK(Tests, OpenHistory.Undo() && OpenHistory.Graph().Nodes.size() == Graph->Nodes.size() + 1 &&
					OpenHistory.Graph().Connections.size() == Graph->Connections.size() + 1);
				PF_CHECK(Tests, OpenHistory.Undo() && OpenHistory.Graph() == *Graph);
				PF_CHECK(Tests, OpenHistory.Redo() && OpenHistory.Redo() && OpenHistory.Graph() == OpenAfter);
				const auto Serialized = SerializeEditorGraph(OpenHistory.Graph(), Schema);
				const auto Reopened = Serialized ? DeserializeEditorGraph(*Serialized, Schema) :
					std::expected<EditorGraph, std::string>(std::unexpected("route serialization failed"));
				PF_CHECK(Tests, Reopened && SameConnections(Reopened->Connections, OpenHistory.Graph().Connections));
				if (Reopened)
				{
					const auto Knot = std::find_if(Reopened->Nodes.begin(), Reopened->Nodes.end(),
						[&](const auto& Node) { return Node.ID == OpenCreated->RerouteNodeIDs.back(); });
					PF_CHECK(Tests, Knot != Reopened->Nodes.end());
					if (Knot != Reopened->Nodes.end())
					{
						const auto* Output = FindPin(*Knot, "out");
						EditorGraphCommandHistory LaterCompletion(*Reopened);
						PF_CHECK(Tests, Output && ConnectEditorGraphPins(LaterCompletion, Schema, Output->ID, InputB));
					}
				}
			}

			EditorGraphCommandHistory InputHistory(*Graph);
			const std::array<EditorGraphVector3, 2> InputKnots{ EditorGraphVector3{ 430.0, 80.0, 0.0 },
				EditorGraphVector3{ 360.0, 140.0, 0.0 } };
			const auto InputPreview = CanCreateEditorGraphWireRoute(*Graph, Schema, InputB, InputKnots, Source);
			PF_CHECK(Tests, InputPreview.Allowed && !InputPreview.OpenEnded);
			const auto InputCreated = CreateEditorGraphWireRoute(InputHistory, Schema, InputB, InputKnots, Source);
			PF_CHECK(Tests, InputCreated && InputCreated->ConnectionIDs.size() == 3 &&
				ValidateEditorGraph(InputHistory.Graph(), Schema).empty());
			if (InputCreated)
			{
				const auto InputAfter = InputHistory.Graph();
				PF_CHECK(Tests, InputHistory.Undo() && InputHistory.Graph().Nodes.size() == Graph->Nodes.size() + 2 &&
					InputHistory.Graph().Connections.size() == Graph->Connections.size() + 2);
				PF_CHECK(Tests, InputHistory.Undo() && InputHistory.Graph().Nodes.size() == Graph->Nodes.size() + 1 &&
					InputHistory.Graph().Connections.size() == Graph->Connections.size() + 1);
				PF_CHECK(Tests, InputHistory.Undo() && InputHistory.Graph() == *Graph);
				PF_CHECK(Tests, InputHistory.Redo() && InputHistory.Redo() && InputHistory.Redo() &&
					InputHistory.Graph() == InputAfter);
				const auto FirstKnot = std::find_if(InputHistory.Graph().Nodes.begin(), InputHistory.Graph().Nodes.end(),
					[&](const auto& Node) { return Node.ID == InputCreated->RerouteNodeIDs.front(); });
				const auto LastKnot = std::find_if(InputHistory.Graph().Nodes.begin(), InputHistory.Graph().Nodes.end(),
					[&](const auto& Node) { return Node.ID == InputCreated->RerouteNodeIDs.back(); });
				const auto* FirstInput = FirstKnot == InputHistory.Graph().Nodes.end() ? nullptr : FindPin(*FirstKnot, "in");
				const auto* LastOutput = LastKnot == InputHistory.Graph().Nodes.end() ? nullptr : FindPin(*LastKnot, "out");
				PF_CHECK(Tests, FirstInput && LastOutput &&
					std::any_of(InputHistory.Graph().Connections.begin(), InputHistory.Graph().Connections.end(),
						[&](const auto& Link) { return Link.OutputPin == LastOutput->ID && Link.InputPin == FirstInput->ID; }) &&
					std::any_of(InputHistory.Graph().Connections.begin(), InputHistory.Graph().Connections.end(),
						[&](const auto& Link) { return Link.OutputPin == FirstInput->ID || Link.InputPin == InputB; }));
			}

			EditorGraphCommandHistory VectorHistory(*Graph);
			const std::array<EditorGraphVector3, 1> OneKnot{ EditorGraphVector3{ 840.0, 120.0, 0.0 } };
			const auto VectorRoute = CreateEditorGraphWireRoute(VectorHistory, Schema, VectorSource, OneKnot, VectorTarget);
			PF_CHECK(Tests, VectorRoute && ValidateEditorGraph(VectorHistory.Graph(), Schema).empty());
		}
		{
			const auto ExecutionSchema = MakeGameplayEventFixtureSchema();
			auto ExecutionGraph = FixtureGraph(ExecutionSchema);
			PF_CHECK(Tests, ExecutionGraph && AppendNode(*ExecutionGraph, ExecutionSchema, "gameplay.event.begin", 0.0) &&
				AppendNode(*ExecutionGraph, ExecutionSchema, "gameplay.flow.branch", 240.0));
			if (ExecutionGraph)
			{
				const auto* Event = FindPin(*ExecutionGraph, "gameplay.event.begin", "event");
				const auto* Input = FindPin(*ExecutionGraph, "gameplay.flow.branch", "exec-in");
				const std::array<EditorGraphVector3, 1> Knot{ EditorGraphVector3{ 140.0, 70.0, 0.0 } };
				EditorGraphCommandHistory History(*ExecutionGraph);
				const auto Created = Event && Input ? CreateEditorGraphWireRoute(History, ExecutionSchema,
					Event->ID, Knot, Input->ID) : std::unexpected(std::string("execution route endpoints unavailable"));
				PF_CHECK(Tests, Created && ValidateEditorGraph(History.Graph(), ExecutionSchema).empty());
			}
		}
	}
	TestBatchPinConnectionMove(Tests);
	{
		using namespace PulseForgeEditor;
		auto Schema = MakeMaterialExpressionFixtureSchema();
		auto Graph = FixtureGraph(Schema);
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "material.scalar", 0.0) &&
			AppendNode(*Graph, Schema, "material.multiply", 240.0) && AppendNode(*Graph, Schema, "material.multiply", 480.0));
		if (Graph)
		{
			const auto Output = FindPin(Graph->Nodes[0], "value")->ID;
			const auto InputA = FindPin(Graph->Nodes[1], "a")->ID;
			const auto InputB = FindPin(Graph->Nodes[2], "a")->ID;
			EditorGraphCommandHistory History(*Graph);
			PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, Output, InputA));
			PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, Output, InputB));
			const auto Before = History.Graph();
			const auto SelectedID = Before.Connections.front().ID;
			const auto SiblingID = Before.Connections.back().ID;
			const auto Inserted = InsertEditorGraphReroute(History, Schema, SelectedID, 360.0, 80.0);
			PF_CHECK(Tests, Inserted.has_value() && History.Graph().Nodes.size() == Before.Nodes.size() + 1 &&
				History.Graph().Connections.size() == Before.Connections.size() + 1);
			if (Inserted)
			{
				const auto Node = std::find_if(History.Graph().Nodes.begin(), History.Graph().Nodes.end(),
					[&Inserted](const auto& Candidate) { return Candidate.ID == *Inserted; });
				PF_CHECK(Tests, Node != History.Graph().Nodes.end() && IsEditorGraphRerouteType(Node->TypeKey));
				const auto OriginalSegment = std::find_if(History.Graph().Connections.begin(), History.Graph().Connections.end(),
					[SelectedID](const auto& Candidate) { return Candidate.ID == SelectedID; });
				const auto* RerouteInput = Node == History.Graph().Nodes.end() ? nullptr : FindPin(*Node, "in");
				const auto UpstreamSegment = std::find_if(History.Graph().Connections.begin(), History.Graph().Connections.end(),
				[RerouteInput](const auto& Candidate)
					{ return RerouteInput && Candidate.InputPin == RerouteInput->ID; });
				PF_CHECK(Tests, OriginalSegment != History.Graph().Connections.end() && UpstreamSegment != History.Graph().Connections.end() &&
					OriginalSegment->InputPin == InputA && OriginalSegment->OutputPin != Output &&
					UpstreamSegment->InputPin == RerouteInput->ID && UpstreamSegment->OutputPin == Output &&
					UpstreamSegment->ID != SelectedID);
				PF_CHECK(Tests, std::any_of(History.Graph().Connections.begin(), History.Graph().Connections.end(),
					[SiblingID, Output, InputB](const auto& Link)
					{ return Link.ID == SiblingID && Link.OutputPin == Output && Link.InputPin == InputB; }));
				if (Node != History.Graph().Nodes.end())
				{
					const auto* RerouteOutput = FindPin(*Node, "out");
					const auto* AdditionalInput = FindPin(History.Graph().Nodes[1], "b");
					EditorGraphCommandHistory BranchHistory(History.Graph());
					PF_CHECK(Tests, RerouteOutput && AdditionalInput && ConnectEditorGraphPins(
						BranchHistory, Schema, RerouteOutput->ID, AdditionalInput->ID));
					PF_CHECK(Tests, BranchHistory.Graph().Connections.size() == History.Graph().Connections.size() + 1 &&
						ValidateEditorGraph(BranchHistory.Graph(), Schema).empty());
				}
				const auto After = History.Graph();
				PF_CHECK(Tests, History.Undo() && History.Graph() == Before);
				PF_CHECK(Tests, History.Redo() && History.Graph() == After);
				PF_CHECK(Tests, ValidateEditorGraph(History.Graph(), Schema).empty());
			}
			auto Custom = Schema;
			Custom.ConnectionPolicy = [](const EditorGraph&, const EditorGraphConnectionPolicyContext&)
				-> std::optional<std::string> { return std::nullopt; };
			PF_CHECK(Tests, !InsertEditorGraphReroute(History, Custom, SelectedID, 360.0, 80.0));
		}
		{
			const auto Schema = MakeGameplayEventFixtureSchema();
			auto Graph = FixtureGraph(Schema);
			PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "gameplay.event.begin", 0.0) &&
				AppendNode(*Graph, Schema, "gameplay.flow.branch", 240.0));
			if (Graph)
			{
				const auto Output = FindPin(Graph->Nodes[0], "event")->ID;
				const auto Input = FindPin(Graph->Nodes[1], "exec-in")->ID;
				EditorGraphCommandHistory History(*Graph);
				PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, Output, Input));
				const auto Wire = History.Graph().Connections.front().ID;
				const auto Reroute = InsertEditorGraphReroute(History, Schema, Wire, 120.0, 40.0);
				PF_CHECK(Tests, Reroute && ValidateEditorGraph(History.Graph(), Schema).empty());
				const auto Serialized = SerializeEditorGraph(History.Graph(), Schema);
				const auto Reopened = Serialized ? DeserializeEditorGraph(*Serialized, Schema)
					: std::expected<EditorGraph, std::string>(std::unexpected("reroute serialization failed"));
				const auto ConnectionsMatchByIdentity = [](const auto& Left, const auto& Right)
				{
					if (Left.size() != Right.size()) return false;
					return std::all_of(Left.begin(), Left.end(), [&Right](const auto& Link)
					{
						return std::any_of(Right.begin(), Right.end(), [&Link](const auto& Candidate)
							{ return Candidate.ID == Link.ID && Candidate.OutputPin == Link.OutputPin &&
								Candidate.InputPin == Link.InputPin; });
					});
				};
				PF_CHECK(Tests, Reopened && Reopened->Nodes.size() == History.Graph().Nodes.size() &&
					ConnectionsMatchByIdentity(Reopened->Connections, History.Graph().Connections));
			}
		}
		{
			auto Schema = MakeMaterialExpressionFixtureSchema();
			auto Graph = FixtureGraph(Schema);
			PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "material.scalar", 0.0) &&
				AppendNode(*Graph, Schema, "material.multiply", 220.0) &&
				AppendNode(*Graph, Schema, "material.multiply", 460.0) &&
				AppendNode(*Graph, Schema, "material.scalar", 700.0));
			if (Graph)
			{
				const auto Producer = FindPin(Graph->Nodes[0], "value")->ID;
				const auto FirstConsumer = FindPin(Graph->Nodes[1], "a")->ID;
				const auto SecondConsumer = FindPin(Graph->Nodes[1], "b")->ID;
				const auto ThirdConsumer = FindPin(Graph->Nodes[2], "a")->ID;
				const auto SingleProducer = FindPin(Graph->Nodes[3], "value")->ID;
				EditorGraphCommandHistory History(*Graph);
				PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, Producer, FirstConsumer));
				const auto OriginalConnection = History.Graph().Connections.front().ID;
				const auto RerouteID = InsertEditorGraphReroute(History, Schema, OriginalConnection, 340.0, 80.0);
				const auto RerouteNode = RerouteID ? std::find_if(History.Graph().Nodes.begin(), History.Graph().Nodes.end(),
					[&](const auto& Candidate) { return Candidate.ID == *RerouteID; }) : History.Graph().Nodes.end();
				const auto* RerouteInput = RerouteNode == History.Graph().Nodes.end() ? nullptr : FindPin(*RerouteNode, "in");
				const auto* RerouteOutput = RerouteNode == History.Graph().Nodes.end() ? nullptr : FindPin(*RerouteNode, "out");
				PF_CHECK(Tests, RerouteInput && RerouteOutput && ConnectEditorGraphPins(History, Schema,
					RerouteOutput->ID, SecondConsumer) && ConnectEditorGraphPins(History, Schema,
					RerouteOutput->ID, ThirdConsumer) && ValidateEditorGraph(History.Graph(), Schema).empty());
				if (RerouteInput && RerouteOutput)
				{
					auto RestrictedSchema = Schema;
					const auto ScalarDefinition = std::find_if(RestrictedSchema.Nodes.begin(), RestrictedSchema.Nodes.end(),
						[](const auto& Definition) { return Definition.TypeKey == "material.scalar"; });
					if (ScalarDefinition != RestrictedSchema.Nodes.end())
						if (const auto OutputDefinition = std::find_if(ScalarDefinition->Pins.begin(), ScalarDefinition->Pins.end(),
							[](const auto& Pin) { return Pin.Key == "value"; }); OutputDefinition != ScalarDefinition->Pins.end())
							OutputDefinition->Cardinality = EditorGraphPinCardinality::Single;
					auto RestrictedGraph = History.Graph();
					if (auto PinIt = std::find_if(RestrictedGraph.Nodes[3].Pins.begin(), RestrictedGraph.Nodes[3].Pins.end(),
						[](const auto& Pin) { return Pin.Key == "value"; }); PinIt != RestrictedGraph.Nodes[3].Pins.end())
						PinIt->Cardinality = EditorGraphPinCardinality::Single;
					EditorGraphCommandHistory RestrictedHistory(std::move(RestrictedGraph));
					const auto UpstreamConnection = std::find_if(RestrictedHistory.Graph().Connections.begin(),
						RestrictedHistory.Graph().Connections.end(), [RerouteInput](const auto& Link)
							{ return Link.InputPin == RerouteInput->ID; });
					const auto UpstreamID = UpstreamConnection == RestrictedHistory.Graph().Connections.end()
						? PulseForge::UUID{} : UpstreamConnection->ID;
					const auto BeforeRejectedReconnect = RestrictedHistory.Graph();
					const auto Decision = CanConnectEditorGraphPins(RestrictedHistory.Graph(), RestrictedSchema,
						SingleProducer, RerouteInput->ID, UpstreamID);
					PF_CHECK(Tests, !Decision.Allowed && Decision.Reason.find("single-cardinality output") != std::string::npos);
					const auto Reconnected = ReconnectEditorGraphConnection(RestrictedHistory, RestrictedSchema, UpstreamID,
						SingleProducer, RerouteInput->ID);
					PF_CHECK(Tests, !Reconnected && RestrictedHistory.Graph() == BeforeRejectedReconnect);

					auto InvalidStored = RestrictedHistory.Graph();
					auto Upstream = std::find_if(InvalidStored.Connections.begin(), InvalidStored.Connections.end(),
						[RerouteInput](const auto& Link) { return Link.InputPin == RerouteInput->ID; });
					if (Upstream != InvalidStored.Connections.end()) Upstream->OutputPin = SingleProducer;
					const auto Diagnostics = ValidateEditorGraph(InvalidStored, RestrictedSchema);
					PF_CHECK(Tests, std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const auto& Diagnostic)
						{ return Diagnostic.Code == "connection.reroute_cardinality"; }));
				}
			}
		}
		{
			auto RestrictedSchema = MakeMaterialExpressionFixtureSchema();
			const auto ScalarDefinition = std::find_if(RestrictedSchema.Nodes.begin(), RestrictedSchema.Nodes.end(),
				[](const auto& Definition) { return Definition.TypeKey == "material.scalar"; });
			if (ScalarDefinition != RestrictedSchema.Nodes.end())
				if (const auto OutputDefinition = std::find_if(ScalarDefinition->Pins.begin(), ScalarDefinition->Pins.end(),
					[](const auto& Pin) { return Pin.Key == "value"; }); OutputDefinition != ScalarDefinition->Pins.end())
					OutputDefinition->Cardinality = EditorGraphPinCardinality::Single;
			auto BranchGraph = FixtureGraph(RestrictedSchema);
			PF_CHECK(Tests, BranchGraph && AppendNode(*BranchGraph, RestrictedSchema, "material.scalar", 0.0) &&
				AppendNode(*BranchGraph, RestrictedSchema, "pulseforge.reroute.float", 160.0) &&
				AppendNode(*BranchGraph, RestrictedSchema, "material.multiply", 320.0) &&
				AppendNode(*BranchGraph, RestrictedSchema, "material.multiply", 520.0));
			if (BranchGraph)
			{
				const auto Source = FindPin(BranchGraph->Nodes[0], "value")->ID;
				const auto RouteInput = FindPin(BranchGraph->Nodes[1], "in")->ID;
				const auto RouteOutput = FindPin(BranchGraph->Nodes[1], "out")->ID;
				const auto ConsumerA = FindPin(BranchGraph->Nodes[2], "a")->ID;
				const auto ConsumerB = FindPin(BranchGraph->Nodes[3], "a")->ID;
				EditorGraphCommandHistory BranchHistory(*BranchGraph);
				PF_CHECK(Tests, ConnectEditorGraphPins(BranchHistory, RestrictedSchema, Source, RouteInput) &&
					ConnectEditorGraphPins(BranchHistory, RestrictedSchema, RouteOutput, ConsumerA));
				const auto BranchPreview = CanConnectEditorGraphPins(BranchHistory.Graph(), RestrictedSchema,
					RouteOutput, ConsumerB);
				PF_CHECK(Tests, !BranchPreview.Allowed &&
					BranchPreview.Reason.find("single-cardinality output") != std::string::npos);
				const auto DiscoveredFromRoute = DiscoverEditorGraphNodeActions(BranchHistory.Graph(),
					RestrictedSchema, RouteOutput);
				PF_CHECK(Tests, std::none_of(DiscoveredFromRoute.begin(), DiscoveredFromRoute.end(),
					[](const auto& Action) { return Action.NodeTypeKey == "material.multiply"; }));
				auto SingleSplitGraph = FixtureGraph(RestrictedSchema);
				PF_CHECK(Tests, SingleSplitGraph && AppendNode(*SingleSplitGraph, RestrictedSchema,
					"material.scalar", 0.0) && AppendNode(*SingleSplitGraph, RestrictedSchema,
					"material.multiply", 220.0));
				if (SingleSplitGraph)
				{
					const auto ProducerOutput = FindPin(SingleSplitGraph->Nodes[0], "value")->ID;
					const auto ConsumerInput = FindPin(SingleSplitGraph->Nodes[1], "a")->ID;
					EditorGraphCommandHistory SingleHistory(*SingleSplitGraph);
					PF_CHECK(Tests, ConnectEditorGraphPins(SingleHistory, RestrictedSchema,
						ProducerOutput, ConsumerInput).has_value());
					const auto OriginalID = SingleHistory.Graph().Connections.front().ID;
					const auto Split = InsertEditorGraphReroute(SingleHistory, RestrictedSchema, OriginalID, 100.0, 60.0);
					PF_CHECK(Tests, Split && ValidateEditorGraph(SingleHistory.Graph(), RestrictedSchema).empty() &&
						SingleHistory.Graph().Connections.size() == 2);
					PF_CHECK(Tests, !CanConnectEditorGraphPins(SingleHistory.Graph(), RestrictedSchema,
						ProducerOutput, FindPin(SingleHistory.Graph().Nodes[1], "b")->ID).Allowed);
				}
				auto InvalidBranch = BranchHistory.Graph();
				InvalidBranch.Connections.push_back({ ID(0xf055), RouteOutput, ConsumerB });
				const auto Diagnostics = ValidateEditorGraph(InvalidBranch, RestrictedSchema);
				PF_CHECK(Tests, std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const auto& Diagnostic)
					{ return Diagnostic.Code == "connection.reroute_cardinality"; }));
			}
		}
		{
			const auto Schema = MakeGameplayEventFixtureSchema();
			auto Graph = FixtureGraph(Schema);
			PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "pulseforge.reroute.execution", 0.0) &&
				AppendNode(*Graph, Schema, "pulseforge.reroute.execution", 120.0));
			if (Graph)
			{
				const auto FirstInput = FindPin(Graph->Nodes[0], "in")->ID;
				const auto FirstOutput = FindPin(Graph->Nodes[0], "out")->ID;
				const auto SecondInput = FindPin(Graph->Nodes[1], "in")->ID;
				const auto SecondOutput = FindPin(Graph->Nodes[1], "out")->ID;
				Graph->Connections = { { ID(0xf101), FirstOutput, SecondInput },
					{ ID(0xf102), SecondOutput, FirstInput } };
				const auto Diagnostics = ValidateEditorGraph(*Graph, Schema);
				PF_CHECK(Tests, std::any_of(Diagnostics.begin(), Diagnostics.end(), [](const auto& Diagnostic)
					{ return Diagnostic.Code == "connection.reroute_cycle"; }));
				const auto CyclePreview = CanConnectEditorGraphPins(*Graph, Schema, SecondOutput, FirstInput,
					ID(0xf102));
				PF_CHECK(Tests, !CyclePreview.Allowed && CyclePreview.Reason.find("reroute-only cycle") != std::string::npos);
			}
		}
	}
	{
		using namespace PulseForgeEditor;
		const auto Schema = MakeGameplayEventFixtureSchema();
		auto Graph = CreateEditorGraph(Schema, ID(0x702));
		PF_CHECK(Tests, Graph && AppendNode(*Graph, Schema, "gameplay.action.log", 0.0));
		if (!Graph) return;
		auto* Message = FindPin(*Graph, "gameplay.action.log", "message");
		PF_CHECK(Tests, Message != nullptr);
		if (!Message) return;
		EditorGraphCommandHistory History(*Graph);
		const auto BeforeInvalidEdit = History.Graph();
		const uint64_t BeforeInvalidRevision = History.GraphRevision();
		const std::string MalformedUtf8(1, static_cast<char>(0xFF));
		const auto RejectedValue = SetEditorGraphPinDefaultValue(History, Schema, Message->ID, MalformedUtf8);
		PF_CHECK(Tests, !RejectedValue && History.Graph() == BeforeInvalidEdit &&
			History.GraphRevision() == BeforeInvalidRevision && !History.CanUndo());
		auto InvalidInMemoryGraph = History.Graph();
		if (auto Pin = std::find_if(InvalidInMemoryGraph.Nodes.front().Pins.begin(),
			InvalidInMemoryGraph.Nodes.front().Pins.end(), [](const auto& Value) { return Value.Key == "message"; });
			Pin != InvalidInMemoryGraph.Nodes.front().Pins.end()) Pin->DefaultValue = MalformedUtf8;
		PF_CHECK(Tests, !ValidateEditorGraph(InvalidInMemoryGraph, Schema).empty() &&
			!SerializeEditorGraph(InvalidInMemoryGraph, Schema));
		auto InvalidClipboard = CopyEditorGraphSubgraph(History.Graph(), { History.Graph().Nodes.front().ID });
		if (auto Pin = std::find_if(InvalidClipboard.Nodes.front().Pins.begin(),
			InvalidClipboard.Nodes.front().Pins.end(), [](const auto& Value) { return Value.Key == "message"; });
			Pin != InvalidClipboard.Nodes.front().Pins.end()) Pin->DefaultValue = MalformedUtf8;
		PF_CHECK(Tests, !SerializeEditorGraphClipboard(InvalidClipboard));

		const std::string EscapedMultibyte = "quote: \\\"; slash: \\\\; text: " +
			std::string("caf\xC3\xA9 \xE2\x98\x83");
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(History, Schema, Message->ID, EscapedMultibyte).has_value());
		const auto EncodedClipboard = SerializeEditorGraphClipboard(
			CopyEditorGraphSubgraph(History.Graph(), { History.Graph().Nodes.front().ID }));
		PF_CHECK(Tests, EncodedClipboard.has_value());
		const auto Encoded = SerializeEditorGraph(History.Graph(), Schema);
		const auto Reopened = Encoded ? DeserializeEditorGraph(*Encoded, Schema) :
			std::expected<EditorGraph, std::string>(std::unexpected("source encode failed"));
		const auto* ReopenedMessage = Reopened ? FindPin(Reopened->Nodes.front(), "message") : nullptr;
		PF_CHECK(Tests, ReopenedMessage && std::get<std::string>(ReopenedMessage->DefaultValue) == EscapedMultibyte);

		EditorGraphCommandHistory LargeHistory(*Graph);
		std::string AggregateString(4096, 'x');
		std::string LargeError;
		const bool LargeEdit = LargeHistory.Execute(Schema, [&](EditorGraph& Candidate) -> std::expected<void, std::string>
		{
			Candidate.Nodes.clear();
			Candidate.Nodes.reserve(500);
			for (size_t Index = 0; Index < 500; ++Index)
			{
				auto Node = CreateEditorGraphNode(Schema, "gameplay.action.log", static_cast<double>(Index) * 20.0, 0.0);
				if (!Node) return std::unexpected(Node.error());
				auto Pin = std::find_if(Node->Pins.begin(), Node->Pins.end(), [](const auto& Value)
					{ return Value.Key == "message"; });
				if (Pin == Node->Pins.end()) return std::unexpected("log message pin is missing");
				Pin->DefaultValue = AggregateString;
				Candidate.Nodes.push_back(std::move(*Node));
			}
			return {};
		}, LargeError);
		PF_CHECK(Tests, LargeEdit && LargeError.empty() && LargeHistory.Graph().Nodes.size() == 500);
		const auto LargeSource = SerializeEditorGraph(LargeHistory.Graph(), Schema);
		PF_CHECK(Tests, LargeSource && LargeSource->size() > 2 * 1024 * 1024 &&
			LargeSource->size() <= EditorGraphMaximumSourceBytes);
		const auto LargeReopened = LargeSource ? DeserializeEditorGraph(*LargeSource, Schema) :
			std::expected<EditorGraph, std::string>(std::unexpected("aggregate source encode failed"));
		PF_CHECK(Tests, LargeReopened && LargeReopened->Nodes.size() == 500);

		EditorGraphCommandHistory OversizedHistory(*Graph);
		const auto OversizedBefore = OversizedHistory.Graph();
		const uint64_t OversizedRevision = OversizedHistory.GraphRevision();
		const std::string ControlText(4096, '\x01');
		std::string OversizedError;
		const bool OversizedEdit = OversizedHistory.Execute(Schema,
			[&](EditorGraph& Candidate) -> std::expected<void, std::string>
			{
				Candidate.Nodes.clear();
				Candidate.Nodes.reserve(700);
				for (size_t Index = 0; Index < 700; ++Index)
				{
					auto Node = CreateEditorGraphNode(Schema, "gameplay.action.log", static_cast<double>(Index) * 20.0, 0.0);
					if (!Node) return std::unexpected(Node.error());
					auto Pin = std::find_if(Node->Pins.begin(), Node->Pins.end(), [](const auto& Value)
						{ return Value.Key == "message"; });
					if (Pin == Node->Pins.end()) return std::unexpected("log message pin is missing");
					Pin->DefaultValue = ControlText;
					Candidate.Nodes.push_back(std::move(*Node));
				}
				return {};
			}, OversizedError);
		PF_CHECK(Tests, !OversizedEdit && OversizedError.find("16 MiB") != std::string::npos &&
			OversizedHistory.Graph() == OversizedBefore &&
			OversizedHistory.GraphRevision() == OversizedRevision && !OversizedHistory.CanUndo());
		auto OversizedGraph = OversizedBefore;
		OversizedGraph.Nodes.clear();
		for (size_t Index = 0; Index < 700; ++Index)
		{
			auto Node = CreateEditorGraphNode(Schema, "gameplay.action.log", static_cast<double>(Index) * 20.0, 0.0);
			if (!Node) break;
			std::find_if(Node->Pins.begin(), Node->Pins.end(), [](const auto& Value)
				{ return Value.Key == "message"; })->DefaultValue = ControlText;
			OversizedGraph.Nodes.push_back(std::move(*Node));
		}
		const auto OversizedSource = SerializeEditorGraph(OversizedGraph, Schema);
		PF_CHECK(Tests, !OversizedSource && OversizedSource.error().find("16 MiB") != std::string::npos);

		EditorGraphCommandHistory BudgetHistory(*Graph);
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(BudgetHistory, Schema, Message->ID, std::string("before budget")).has_value());
		PF_CHECK(Tests, BudgetHistory.Undo() && BudgetHistory.CanRedo());
		const auto BeforeBudgetRejection = BudgetHistory.Graph();
		const uint64_t BeforeBudgetRevision = BudgetHistory.GraphRevision();
		std::vector<EditorGraph> TooManyRouteSteps;
		TooManyRouteSteps.reserve(129);
		for (size_t Index = 0; Index < 129; ++Index)
		{
			auto Candidate = BeforeBudgetRejection;
			Candidate.Nodes.front().X += static_cast<double>(Index + 1);
			TooManyRouteSteps.push_back(std::move(Candidate));
		}
		std::string BudgetError;
		PF_CHECK(Tests, !BudgetHistory.ExecuteSequence(Schema, TooManyRouteSteps, BudgetError) &&
			BudgetError.find("Undo steps") != std::string::npos && BudgetHistory.Graph() == BeforeBudgetRejection &&
			BudgetHistory.GraphRevision() == BeforeBudgetRevision && BudgetHistory.CanRedo());
	}
	{
		using namespace PulseForgeEditor;
		const auto Schema = MakeGameplayEventFixtureSchema();
		const auto MakeBoundaryGraph = [&Schema](size_t LogNodeCount, size_t PayloadSize) -> std::optional<EditorGraph>
		{
			auto Graph = CreateEditorGraph(Schema, ID(0x703));
			if (!Graph || !AppendNode(*Graph, Schema, "gameplay.event.begin", 0.0)) return std::nullopt;
			const std::string Payload(PayloadSize, 'x');
			for (size_t Index = 0; Index < LogNodeCount; ++Index)
			{
				if (!AppendNode(*Graph, Schema, "gameplay.action.log", static_cast<double>(Index + 1) * 20.0))
					return std::nullopt;
				auto* LogNode = FindNode(*Graph, "gameplay.action.log", Index);
				auto MessagePin = LogNode ? std::find_if(LogNode->Pins.begin(), LogNode->Pins.end(),
					[](const auto& Pin) { return Pin.Key == "message"; }) : std::vector<EditorGraphPin>::iterator{};
				if (!LogNode || MessagePin == LogNode->Pins.end()) return std::nullopt;
				MessagePin->DefaultValue = Payload;
			}
			return std::move(*Graph);
		};
		const auto RejectedGraph = MakeBoundaryGraph(420, 4'096);
		PF_CHECK(Tests, RejectedGraph.has_value());
		if (RejectedGraph)
		{
			EditorGraphCommandHistory RejectedHistory(*RejectedGraph);
			const auto MessageID = FindPin(RejectedHistory.Graph().Nodes[1], "message")->ID;
			const auto RedoEdit = SetEditorGraphPinDefaultValue(RejectedHistory, Schema, MessageID, std::string("redo marker"));
			PF_CHECK(Tests, RedoEdit.has_value());
			PF_CHECK(Tests, RedoEdit && RejectedHistory.Undo() && RejectedHistory.CanRedo());
			const auto Before = RejectedHistory.Graph();
			const auto Revision = RejectedHistory.GraphRevision();
			const bool Dirty = RejectedHistory.IsDirty();
			const bool CanUndoBefore = RejectedHistory.CanUndo();
			const bool CanRedoBefore = RejectedHistory.CanRedo();
			const auto* EventPin = FindPin(RejectedHistory.Graph().Nodes.front(), "event");
			std::array<EditorGraphVector3, 32> Knots{};
			for (size_t Index = 0; Index < Knots.size(); ++Index)
				Knots[Index] = { static_cast<double>(Index * 24), 80.0, 0.0 };
			EditorGraphWorkStats PreviewWork;
			const auto PreviewStart = std::chrono::steady_clock::now();
			const auto Preview = EventPin ? CanCreateEditorGraphWireRoute(RejectedHistory.Graph(), Schema,
				EventPin->ID, Knots, std::nullopt, nullptr, &PreviewWork, nullptr, &RejectedHistory) :
				EditorGraphWireRouteDecision{};
			const double PreviewMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - PreviewStart).count();
			EditorGraphWorkStats CommitWork;
			const auto CommitStart = std::chrono::steady_clock::now();
			const auto Rejected = EventPin ? CreateEditorGraphWireRoute(RejectedHistory, Schema, EventPin->ID, Knots,
				std::nullopt, &CommitWork) :
				std::expected<EditorGraphWireRouteDecision, std::string>(std::unexpected("event output unavailable"));
			const double CommitMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - CommitStart).count();
			std::cout << "G21 oversized route nodes=420 knots=32 preview_ms=" <<
				PreviewMilliseconds << " preview_states=" << PreviewWork.RoutePrefixStatesValidated <<
				" commit_ms=" << CommitMilliseconds << " commit_states=" <<
				CommitWork.RoutePrefixStatesValidated << '\n';
			PF_CHECK(Tests, !Rejected && RejectedHistory.Graph() == Before && RejectedHistory.GraphRevision() == Revision &&
				RejectedHistory.IsDirty() == Dirty && RejectedHistory.CanUndo() == CanUndoBefore &&
				RejectedHistory.CanRedo() == CanRedoBefore && !Preview.Allowed &&
				PreviewWork.RoutePrefixStatesValidated > 0 && PreviewWork.RoutePrefixStatesValidated < Knots.size() &&
				CommitWork.RoutePrefixStatesValidated > 0 && CommitWork.RoutePrefixStatesValidated < Knots.size());
			PF_CHECK(Tests, RejectedHistory.Redo() && std::get<std::string>(
				FindPin(RejectedHistory.Graph().Nodes[1], "message")->DefaultValue) == "redo marker");
		}
		const auto AdmissibleGraph = MakeBoundaryGraph(230, 4'096);
		PF_CHECK(Tests, AdmissibleGraph.has_value());
		if (AdmissibleGraph)
		{
			EditorGraphCommandHistory RouteHistory(*AdmissibleGraph);
			const auto* EventPin = FindPin(AdmissibleGraph->Nodes.front(), "event");
			std::array<EditorGraphVector3, 32> Knots{};
			for (size_t Index = 0; Index < Knots.size(); ++Index)
				Knots[Index] = { static_cast<double>(Index * 24), 80.0, 0.0 };
			const auto Created = EventPin ? CreateEditorGraphWireRoute(RouteHistory, Schema, EventPin->ID, Knots) :
				std::expected<EditorGraphWireRouteDecision, std::string>(std::unexpected("event output unavailable"));
			PF_CHECK(Tests, Created && Created->RerouteNodeIDs.size() == 32 && Created->ConnectionIDs.size() == 32);
			if (Created)
			{
				const auto FinalGraph = RouteHistory.Graph();
				PF_CHECK(Tests, RouteHistory.Undo() && RouteHistory.Undo() && RouteHistory.Redo());
				size_t RemainingUndoSteps = 0;
				while (RouteHistory.Undo()) ++RemainingUndoSteps;
				PF_CHECK(Tests, RemainingUndoSteps == 31 && RouteHistory.Graph() == *AdmissibleGraph);
				while (RouteHistory.Redo()) {}
				PF_CHECK(Tests, RouteHistory.Graph() == FinalGraph &&
					std::all_of(Created->RerouteNodeIDs.begin(), Created->RerouteNodeIDs.end(), [&](const auto NodeID)
					{ return std::any_of(RouteHistory.Graph().Nodes.begin(), RouteHistory.Graph().Nodes.end(),
						[NodeID](const auto& Node) { return Node.ID == NodeID; }); }) &&
					std::all_of(Created->ConnectionIDs.begin(), Created->ConnectionIDs.end(), [&](const auto ConnectionID)
					{ return std::any_of(RouteHistory.Graph().Connections.begin(), RouteHistory.Graph().Connections.end(),
						[ConnectionID](const auto& Link) { return Link.ID == ConnectionID; }); }));
			}
		}
	}
	TestMaterialSchemaAndSerialization(Tests);
	TestGameplaySchemaAndTransactions(Tests);
	TestPropertyEditCoordinator(Tests);
	TestReusableGraphLayoutAndPinCommands(Tests);
	TestNodeActionDiscoverySearchAndAtomicAutowire(Tests);
	TestMixedTypeCyclePolicies(Tests);
	TestIndexedTraversalAndEndpointInvariants(Tests);
	TestConnectionPolicyContextsAndCapacity(Tests);
	{
		using namespace PulseForgeEditor;
		EditorGraphPinDefinition Definition{ "socket", "Socket", EditorGraphPinDirection::Input,
			EditorGraphPinType::Float, EditorGraphPinCardinality::Single, 2.5 };
		EditorGraphPin Pin{ PulseForge::UUID{}, Definition.Key, Definition.Direction, Definition.Type,
			Definition.Cardinality, Definition.DefaultValue };
		PF_CHECK(Tests, ShouldShowEditorGraphPinDefault(Pin, Definition, 0));
		PF_CHECK(Tests, !ShouldShowEditorGraphPinDefault(Pin, Definition, 1));
		PF_CHECK(Tests, CanHideEditorGraphPin(Definition));
		PF_CHECK(Tests, !CanAutoHideEditorGraphPin(Pin, Definition, 0, false));
		PF_CHECK(Tests, CanAutoHideEditorGraphPin(Pin, Definition, 0, true));
		Pin.DefaultValue = 3.0;
		PF_CHECK(Tests, !CanAutoHideEditorGraphPin(Pin, Definition, 0, true));
		Definition.DefaultControl = EditorGraphPinDefaultControl::Always;
		PF_CHECK(Tests, ShouldShowEditorGraphPinDefault(Pin, Definition, 1));
		Definition.Required = true;
		PF_CHECK(Tests, !CanHideEditorGraphPin(Definition));
	}
	{
		using namespace PulseForgeEditor;
		const auto Schema = MakeMaterialExpressionFixtureSchema();
		auto Graph = CreateEditorGraph(Schema, ID(0x701));
		PF_CHECK(Tests, Graph.has_value());
		if (Graph)
		{
			PF_CHECK(Tests, AppendNode(*Graph, Schema, "material.scalar", 0.0));
			PF_CHECK(Tests, AppendNode(*Graph, Schema, "material.multiply", 240.0));
			EditorGraphCommandHistory History(*Graph);
			const auto* Scalar = FindNode(*Graph, "material.scalar");
			const auto* Multiply = FindNode(*Graph, "material.multiply");
			const auto* Output = Scalar ? FindPin(*Scalar, "value") : nullptr;
			const auto* Input = Multiply ? FindPin(*Multiply, "a") : nullptr;
			PF_CHECK(Tests, Scalar && Multiply && Output && Input);
			if (Scalar && Multiply && Output && Input)
			{
				PF_CHECK(Tests, !IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
				PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, Output->ID, Input->ID).has_value());
				PF_CHECK(Tests, IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
				PF_CHECK(Tests, History.Undo() && !IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
				PF_CHECK(Tests, History.Redo() && IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
				PF_CHECK(Tests, DisconnectEditorGraphPin(History, Schema, Input->ID).has_value());
				PF_CHECK(Tests, !IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
				PF_CHECK(Tests, History.Undo() && IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
				PF_CHECK(Tests, DeleteEditorGraphNodes(History, Schema, { Scalar->ID }).has_value());
				PF_CHECK(Tests, !IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
				PF_CHECK(Tests, History.Undo() && IsEditorGraphNodeConnected(History.Graph(), Multiply->ID));
			}
		}
	}
}
