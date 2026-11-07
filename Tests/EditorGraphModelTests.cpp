#include "TestSupport.h"

#include "Editor/EditorGraphModel.h"

#include <algorithm>
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
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, Begin->ID, BranchAInput->ID).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, BranchAOut->ID, BranchBInput->ID).has_value());
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, BranchBOut->ID, BranchAInput->ID).has_value());	// Execution cycles are deliberately allowed in this fixture.
		PF_CHECK(Tests, ConnectEditorGraphPins(History, Schema, BranchBOut->ID, LogInput->ID).has_value());
		const auto* TextPin = FindPin(*LogNode, "message");
		PF_CHECK(Tests, TextPin != nullptr);
		if (!TextPin) return;
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(History, Schema, TextPin->ID, std::string("authoring fixture" )).has_value());
		PF_CHECK(Tests, !SetEditorGraphPinDefaultValue(History, Schema, Condition->ID, 2.0));
		const EditorGraph BeforeMove = History.Graph();
		PF_CHECK(Tests, History.BeginTransaction());
		PF_CHECK(Tests, MoveEditorGraphNodes(History, Schema, {
			{ BranchA->ID, EditorGraphVector3{ 235.0, 90.0, 0.0 } },
			{ BranchB->ID, EditorGraphVector3{ 465.0, 160.0, 0.0 } } }).has_value());
		PF_CHECK(Tests, MoveEditorGraphNodes(History, Schema, {
			{ BranchA->ID, EditorGraphVector3{ 250.0, 100.0, 0.0 } },
			{ BranchB->ID, EditorGraphVector3{ 480.0, 180.0, 0.0 } } }).has_value());
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
			{ BranchA->ID, EditorGraphVector3{ 999.0, 999.0, 0.0 } } }).has_value());
		History.CancelTransaction();
		PF_CHECK(Tests, History.Graph() == BeforeMove && History.CanRedo());
		PF_CHECK(Tests, History.Redo() && History.Graph() == AfterMove);
		PF_CHECK(Tests, History.Undo() && History.Graph() == BeforeMove);
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(History, Schema, Condition->ID, true).has_value());
		PF_CHECK(Tests, !History.CanRedo());
		PF_CHECK(Tests, SetEditorGraphPinDefaultValue(History, Schema, Condition->ID, false).has_value());

		const auto Clipboard = CopyEditorGraphSubgraph(History.Graph(), { BranchA->ID, BranchB->ID });
		PF_CHECK(Tests, Clipboard.Nodes.size() == 2 && Clipboard.Connections.size() == 2);
		const auto ClipboardText = SerializeEditorGraphClipboard(Clipboard);
		PF_CHECK(Tests, ClipboardText.has_value());
		if (!ClipboardText) return;
		const auto BeforePaste = History.Graph();
		PF_CHECK(Tests, PasteEditorGraphClipboard(History, Schema, *ClipboardText, 40.0, 60.0).has_value());
		PF_CHECK(Tests, History.Graph().Nodes.size() == BeforePaste.Nodes.size() + 2);
		PF_CHECK(Tests, History.Graph().Connections.size() == BeforePaste.Connections.size() + 2);
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
		PF_CHECK(Tests, PasteEditorGraphClipboard(History, Schema, *ClipboardText, 80.0, 100.0).has_value());
		PF_CHECK(Tests, History.Graph().Nodes.size() == PastedGraph.Nodes.size() + 2);
		PF_CHECK(Tests, !PasteEditorGraphClipboard(History, MakeMaterialExpressionFixtureSchema(), *ClipboardText, 0.0, 0.0));

		const auto BeforeDelete = History.Graph();
		PF_CHECK(Tests, DeleteEditorGraphNodes(History, Schema, { BranchA->ID }).has_value());
		PF_CHECK(Tests, History.Graph().Connections.size() < BeforeDelete.Connections.size());
		PF_CHECK(Tests, History.Undo());
		PF_CHECK(Tests, History.Graph() == BeforeDelete);
		const auto Current = History.Graph();
		PF_CHECK(Tests, !PasteEditorGraphClipboard(History, Schema, "{\"format\":\"PulseForgeGraphClipboard\",\"version\":4294967297}", 0.0, 0.0));
		PF_CHECK(Tests, History.Graph() == Current);
	}
}

void RunEditorGraphModelTests(TestRunner& Tests)
{
	TestMaterialSchemaAndSerialization(Tests);
	TestGameplaySchemaAndTransactions(Tests);
}
