#pragma once

#include "Scene/UUID.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorGraphVector3
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		friend bool operator==(const EditorGraphVector3&, const EditorGraphVector3&) = default;
	};

	using EditorGraphValue = std::variant<std::monostate, bool, double, EditorGraphVector3, std::string>;

	enum class EditorGraphPinDirection : uint8_t
	{
		Input,
		Output
	};

	enum class EditorGraphPinType : uint8_t
	{
		Float,
		Vector3,
		Boolean,
		String,
		Execution
	};

	enum class EditorGraphPinCardinality : uint8_t
	{
		Single,
		Multiple
	};

	enum class EditorGraphCyclePolicy : uint8_t
	{
		Reject,
		Allow
	};

	struct EditorGraphPinDefinition
	{
		std::string Key;
		std::string Label;
		EditorGraphPinDirection Direction = EditorGraphPinDirection::Input;
		EditorGraphPinType Type = EditorGraphPinType::Float;
		EditorGraphPinCardinality Cardinality = EditorGraphPinCardinality::Single;
		EditorGraphValue DefaultValue;
	};

	struct EditorGraphNodeDefinition
	{
		std::string TypeKey;
		std::string Label;
		std::vector<EditorGraphPinDefinition> Pins;
	};

	struct EditorGraphSchema
	{
		std::string Key;
		uint32_t Version = 1;
		EditorGraphCyclePolicy DataCyclePolicy = EditorGraphCyclePolicy::Reject;
		EditorGraphCyclePolicy ExecutionCyclePolicy = EditorGraphCyclePolicy::Reject;
		bool ReplaceOccupiedInput = true;
		std::vector<EditorGraphNodeDefinition> Nodes;
	};

	struct EditorGraphPin
	{
		PulseForge::UUID ID;
		std::string Key;
		EditorGraphPinDirection Direction = EditorGraphPinDirection::Input;
		EditorGraphPinType Type = EditorGraphPinType::Float;
		EditorGraphPinCardinality Cardinality = EditorGraphPinCardinality::Single;
		EditorGraphValue DefaultValue;
		friend bool operator==(const EditorGraphPin&, const EditorGraphPin&) = default;
	};

	struct EditorGraphNode
	{
		PulseForge::UUID ID;
		std::string TypeKey;
		double X = 0.0;
		double Y = 0.0;
		std::vector<EditorGraphPin> Pins;
		friend bool operator==(const EditorGraphNode&, const EditorGraphNode&) = default;
	};

	struct EditorGraphConnection
	{
		PulseForge::UUID ID;
		PulseForge::UUID OutputPin;
		PulseForge::UUID InputPin;
		friend bool operator==(const EditorGraphConnection&, const EditorGraphConnection&) = default;
	};

	struct EditorGraph
	{
		PulseForge::UUID ID;
		std::string SchemaKey;
		uint32_t SchemaVersion = 1;
		std::vector<EditorGraphNode> Nodes;
		std::vector<EditorGraphConnection> Connections;
		friend bool operator==(const EditorGraph&, const EditorGraph&) = default;
	};

	struct EditorGraphDiagnostic
	{
		std::string Code;
		std::string Message;
		PulseForge::UUID Element;
	};

	struct EditorGraphConnectionDecision
	{
		bool Allowed = false;
		std::string Reason;
		PulseForge::UUID OutputPin;
		PulseForge::UUID InputPin;
	};

	struct EditorGraphNodeCreationAction
	{
		std::string NodeTypeKey;
		std::string NodeLabel;
		std::optional<std::string> ConnectPinKey;
		std::string ConnectPinLabel;
		friend bool operator==(const EditorGraphNodeCreationAction&, const EditorGraphNodeCreationAction&) = default;
	};

	struct EditorGraphNodeCreationResult
	{
		PulseForge::UUID NodeID;
		std::optional<PulseForge::UUID> ConnectedPinID;
		std::optional<PulseForge::UUID> ConnectionID;
	};

	struct EditorGraphClipboard
	{
		std::string SchemaKey;
		uint32_t SchemaVersion = 1;
		std::vector<EditorGraphNode> Nodes;
		std::vector<EditorGraphConnection> Connections;
	};

	class EditorGraphCommandHistory final
	{
	public:
		static constexpr size_t MaximumSnapshots = 128;
		static constexpr size_t MaximumHistoryBytes = 32 * 1024 * 1024;

		explicit EditorGraphCommandHistory(EditorGraph InitialGraph);

		[[nodiscard]] const EditorGraph& Graph() const noexcept { return m_Graph; }
		[[nodiscard]] bool CanUndo() const noexcept { return !m_Undo.empty() && !m_Transaction; }
		[[nodiscard]] bool CanRedo() const noexcept { return !m_Redo.empty() && !m_Transaction; }
		[[nodiscard]] bool HasOpenTransaction() const noexcept { return m_Transaction.has_value(); }
		[[nodiscard]] bool IsDirty() const noexcept { return m_Graph != m_SavedGraph; }
		[[nodiscard]] bool BeginTransaction();
		[[nodiscard]] bool CommitTransaction(const EditorGraphSchema& Schema, std::string& Error);
		void CancelTransaction();
		void Reset(EditorGraph SavedGraph);
		[[nodiscard]] bool Undo();
		[[nodiscard]] bool Redo();
		void MarkSaved();
		[[nodiscard]] bool Replace(EditorGraph Candidate, const EditorGraphSchema& Schema, std::string& Error);
		[[nodiscard]] bool Execute(const EditorGraphSchema& Schema,
			const std::function<std::expected<void, std::string>(EditorGraph&)>& Mutation, std::string& Error);

	private:
		void PushUndo(EditorGraph Snapshot);
		void TrimHistory();

		EditorGraph m_Graph;
		EditorGraph m_SavedGraph;
		std::vector<EditorGraph> m_Undo;
		std::vector<EditorGraph> m_Redo;
		std::optional<EditorGraph> m_Transaction;
	};

	struct EditorGraphPropertyEditEvent
	{
		PulseForge::UUID PinID;
		EditorGraphValue Value;
		bool Activated = false;
		bool Changed = false;
		bool Deactivated = false;
	};

	struct EditorGraphPropertyEditResult
	{
		bool GraphChanged = false;
		std::string Error;
		bool PropertyInteractionActive = false;
		bool TransactionOpen = false;
	};

	class EditorGraphPropertyEditCoordinator final
	{
	public:
		[[nodiscard]] EditorGraphPropertyEditResult ProcessFrame(EditorGraphCommandHistory& History,
			const EditorGraphSchema& Schema, const std::vector<EditorGraphPropertyEditEvent>& Events, bool Cancel);
		[[nodiscard]] EditorGraphPropertyEditResult Finish(EditorGraphCommandHistory& History,
			const EditorGraphSchema& Schema);
		[[nodiscard]] EditorGraphPropertyEditResult Cancel(EditorGraphCommandHistory& History);
		[[nodiscard]] bool HasActiveEdit() const noexcept { return m_ActivePin.has_value(); }
		void Reset() noexcept { m_ActivePin.reset(); }

	private:
		[[nodiscard]] std::string FinishActive(EditorGraphCommandHistory& History, const EditorGraphSchema& Schema);
		[[nodiscard]] bool Begin(EditorGraphCommandHistory& History, PulseForge::UUID PinID, std::string& Error);
		std::optional<PulseForge::UUID> m_ActivePin;
	};

	[[nodiscard]] EditorGraphSchema MakeMaterialExpressionFixtureSchema();
	[[nodiscard]] EditorGraphSchema MakeGameplayEventFixtureSchema();
	[[nodiscard]] std::expected<EditorGraph, std::string> CreateEditorGraph(
		const EditorGraphSchema& Schema, PulseForge::UUID GraphID);
	[[nodiscard]] std::expected<EditorGraphNode, std::string> CreateEditorGraphNode(
		const EditorGraphSchema& Schema, std::string_view TypeKey, double X, double Y);
	[[nodiscard]] std::vector<EditorGraphDiagnostic> ValidateEditorGraph(
		const EditorGraph& Graph, const EditorGraphSchema& Schema);
	[[nodiscard]] EditorGraphConnectionDecision CanConnectEditorGraphPins(
		const EditorGraph& Graph, const EditorGraphSchema& Schema,
		PulseForge::UUID FirstPin, PulseForge::UUID SecondPin,
		std::optional<PulseForge::UUID> ReplacingConnection = std::nullopt);
	[[nodiscard]] std::vector<EditorGraphNodeCreationAction> DiscoverEditorGraphNodeActions(
		const EditorGraph& Graph, const EditorGraphSchema& Schema,
		std::optional<PulseForge::UUID> SourcePin = std::nullopt);
	[[nodiscard]] std::vector<EditorGraphNodeCreationAction> SearchEditorGraphNodeActions(
		const std::vector<EditorGraphNodeCreationAction>& Actions, std::string_view Query);
	[[nodiscard]] std::expected<std::string, std::string> SerializeEditorGraph(const EditorGraph& Graph,
		const EditorGraphSchema& Schema);
	[[nodiscard]] std::expected<EditorGraph, std::string> DeserializeEditorGraph(
		std::string_view Source, const EditorGraphSchema& Schema);
	[[nodiscard]] EditorGraphClipboard CopyEditorGraphSubgraph(
		const EditorGraph& Graph, const std::vector<PulseForge::UUID>& NodeIDs);
	[[nodiscard]] std::expected<std::string, std::string> SerializeEditorGraphClipboard(
		const EditorGraphClipboard& Clipboard);
	[[nodiscard]] std::expected<void, std::string> PasteEditorGraphClipboard(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view Source, double OffsetX, double OffsetY);
	[[nodiscard]] std::expected<void, std::string> AddEditorGraphNode(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view TypeKey, double X, double Y);
	[[nodiscard]] std::expected<EditorGraphNodeCreationResult, std::string> CreateEditorGraphNodeAndConnect(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view TypeKey, std::string_view NewNodePinKey,
		PulseForge::UUID SourcePinID, double X, double Y);
	[[nodiscard]] std::expected<void, std::string> DeleteEditorGraphNodes(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		const std::vector<PulseForge::UUID>& NodeIDs);
	[[nodiscard]] std::expected<void, std::string> ConnectEditorGraphPins(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID FirstPin, PulseForge::UUID SecondPin);
	[[nodiscard]] std::expected<void, std::string> DisconnectEditorGraphConnection(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID ConnectionID);
	[[nodiscard]] std::expected<void, std::string> DisconnectEditorGraphPin(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID PinID);
	[[nodiscard]] std::expected<void, std::string> ReconnectEditorGraphConnection(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID ConnectionID, PulseForge::UUID FirstPin, PulseForge::UUID SecondPin);
	[[nodiscard]] std::expected<void, std::string> MoveEditorGraphNodes(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		const std::vector<std::pair<PulseForge::UUID, EditorGraphVector3>>& Positions);
	[[nodiscard]] std::expected<void, std::string> SetEditorGraphPinDefaultValue(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID PinID, EditorGraphValue Value);
}
