#include "TestSupport.h"

#include "Editor/EditorGraphModel.h"
#include "Editor/EditorGraphLayout.h"

#include <algorithm>
#include <array>
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
		auto Duplicated = DuplicateEditorGraphSubgraph(History, Schema, { BranchAID, BranchBID });
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
				PF_CHECK(Tests, std::abs((DuplicateMaximumX - DuplicateMinimumX) -
					(SourceMaximumX - SourceMinimumX)) < 0.001 &&
					std::abs(DuplicateMinimumX - std::floor(SourceMinimumX + 32.0)) < 0.001);
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

void RunEditorGraphModelTests(TestRunner& Tests)
{
	TestMaterialSchemaAndSerialization(Tests);
	TestGameplaySchemaAndTransactions(Tests);
	TestPropertyEditCoordinator(Tests);
	TestReusableGraphLayoutAndPinCommands(Tests);
	TestNodeActionDiscoverySearchAndAtomicAutowire(Tests);
	TestMixedTypeCyclePolicies(Tests);
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
