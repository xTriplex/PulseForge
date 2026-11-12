#pragma once

#include "Scene/UUID.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <variant>
#include <vector>

namespace PulseForgeEditor
{
	inline constexpr size_t EditorGraphMaximumNodes = 2048;
	inline constexpr size_t EditorGraphMaximumConnections = 8192;
	inline constexpr size_t EditorGraphMaximumSourceBytes = 16 * 1024 * 1024;

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

	enum class EditorGraphPinDefaultControl : uint8_t
	{
		Never,
		WhenDisconnected,
		Always
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
		EditorGraphPinDefaultControl DefaultControl = EditorGraphPinDefaultControl::WhenDisconnected;
		bool Hideable = true;
		bool Required = false;
	};

	struct EditorGraphNodeDefinition
	{
		std::string TypeKey;
		std::string Label;
		std::vector<EditorGraphPinDefinition> Pins;
	};
	struct EditorGraph;
	struct EditorGraphPin;
	struct EditorGraphDiagnostic;
	struct EditorGraphConnectionPolicyContext;

	struct EditorGraphSchema
	{
		std::string Key;
		uint32_t Version = 1;
		EditorGraphCyclePolicy DataCyclePolicy = EditorGraphCyclePolicy::Reject;
		EditorGraphCyclePolicy ExecutionCyclePolicy = EditorGraphCyclePolicy::Reject;
		bool ReplaceOccupiedInput = true;
		std::vector<EditorGraphNodeDefinition> Nodes;
		// Policies are read-only and deterministic for the lifetime of a registered schema; mutable
		// external policy state is unsupported. The context distinguishes live from prospective pins
		// and lists edges removed before the candidate pair is considered.
		std::function<std::optional<std::string>(const EditorGraph&, const EditorGraphConnectionPolicyContext&)>
			ConnectionPolicy;
		std::function<std::optional<EditorGraphDiagnostic>(const EditorGraph&)> GraphValidationPolicy;
		// Explicitly opted-in pin types that may be split by the authoring reroute command.
		// Schemas with custom policies must leave this empty until their policy declares routing transparency.
		std::vector<EditorGraphPinType> RerouteTypes;
		bool ReroutePolicyAware = false;
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
	struct EditorGraphWorkStats
	{
		size_t NodeVisits = 0;
		size_t PinVisits = 0;
		size_t ConnectionVisits = 0;
		size_t TraversedEdges = 0;
		size_t RerouteTopologyBuilds = 0;
		size_t RoutePrefixStatesValidated = 0;
	};

	class EditorGraphSchemaRegistry final
	{
	public:
		[[nodiscard]] std::expected<void, std::string> Register(EditorGraphSchema Schema);
		[[nodiscard]] std::expected<EditorGraphSchema, std::string> Resolve(std::string_view Key, uint32_t Version) const;
		[[nodiscard]] bool Contains(std::string_view Key) const;
	private:
		std::unordered_map<std::string, EditorGraphSchema> m_Schemas;
	};

	[[nodiscard]] bool ShouldShowEditorGraphPinDefault(const EditorGraphPin& Pin,
		const EditorGraphPinDefinition& Definition, size_t IncidentConnectionCount) noexcept;
	[[nodiscard]] bool CanHideEditorGraphPin(const EditorGraphPinDefinition& Definition) noexcept;
	[[nodiscard]] bool CanAutoHideEditorGraphPin(const EditorGraphPin& Pin,
		const EditorGraphPinDefinition& Definition, size_t IncidentConnectionCount, bool NodeConnected) noexcept;
	[[nodiscard]] bool IsEditorGraphNodeConnected(const EditorGraph& Graph, PulseForge::UUID NodeID);
	// Reroutes are authoring topology only: consumers follow their input upstream and output downstream;
	// node position and selection have no evaluation meaning.
	[[nodiscard]] bool IsEditorGraphRerouteType(std::string_view TypeKey);
	[[nodiscard]] const EditorGraphNodeDefinition* FindEditorGraphNodeDefinition(
		const EditorGraphSchema& Schema, std::string_view TypeKey);

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

	enum class EditorGraphConnectionPolicyPurpose : uint8_t
	{
		ProposedOperation,
		StoredConnectionValidation,
		NodeActionDiscovery
	};

	struct EditorGraphPolicyEndpoint
	{
		bool IsProspective = false;
		std::optional<PulseForge::UUID> PinID;
		std::optional<PulseForge::UUID> OwnerNodeID;
		std::string OwnerNodeTypeKey;
		std::string PinKey;
		EditorGraphPinDirection Direction = EditorGraphPinDirection::Input;
		EditorGraphPinType Type = EditorGraphPinType::Float;
		EditorGraphPinCardinality Cardinality = EditorGraphPinCardinality::Single;
		EditorGraphValue CurrentValue;
	};

	struct EditorGraphConnectionPolicyContext
	{
		EditorGraphConnectionPolicyPurpose Purpose = EditorGraphConnectionPolicyPurpose::ProposedOperation;
		EditorGraphPolicyEndpoint Output;
		EditorGraphPolicyEndpoint Input;
		// The callback sees the original immutable graph, excludes these IDs, then treats Output and Input
		// as the proposed pair. StoredConnectionValidation removes the stored edge being checked before
		// supplying that same pair, so domain rules evaluate the effective result without counting it twice.
		std::span<const PulseForge::UUID> RemovedConnectionIDs;
		// Batch proposals are evaluated against the original graph after RemovedConnectionIDs
		// are removed and all of these stable-identity endpoint pairs are applied.
		struct ProposedConnection
		{
			PulseForge::UUID ConnectionID;
			EditorGraphPolicyEndpoint Output;
			EditorGraphPolicyEndpoint Input;
		};
		std::span<const ProposedConnection> ProposedConnections;
		std::optional<PulseForge::UUID> ProposedConnectionID;
		bool ProposedConnectionIsStored = false;

		[[nodiscard]] bool RemovesConnection(PulseForge::UUID ConnectionID) const noexcept
		{
			return std::find(RemovedConnectionIDs.begin(), RemovedConnectionIDs.end(), ConnectionID) != RemovedConnectionIDs.end();
		}
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
		std::vector<PulseForge::UUID> RemovedConnectionIDs;
	};

	struct EditorGraphPinMoveDecision
	{
		bool Allowed = false;
		bool NoOp = false;
		std::string Reason;
		std::vector<PulseForge::UUID> MovedConnectionIDs;
		std::vector<PulseForge::UUID> DisplacedConnectionIDs;
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

	struct EditorGraphWireRouteDecision
	{
		bool Allowed = false;
		bool OpenEnded = false;
		std::string Reason;
		std::vector<PulseForge::UUID> RerouteNodeIDs;
		std::vector<PulseForge::UUID> ConnectionIDs;
		std::vector<PulseForge::UUID> DisplacedConnectionIDs;
	};

	struct EditorGraphWireRouteAdmission
	{
		size_t Steps = 0;
		size_t TotalStateBytes = 0;
		size_t SmallestStateBytes = 0;
		size_t PlannedSnapshotBytes = 0;
		bool Initialized = false;
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
		[[nodiscard]] uint64_t GraphRevision() const noexcept { return m_GraphRevision; }
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
		[[nodiscard]] bool ExecuteSequence(const EditorGraphSchema& Schema,
			std::span<const EditorGraph> Candidates, std::string& Error);
		[[nodiscard]] bool ExecuteSequence(const EditorGraphSchema& Schema,
			std::vector<EditorGraph>&& Candidates, std::string& Error);
		[[nodiscard]] bool CanAdmitRoute(size_t PlannedSteps, size_t RouteSnapshotBytes,
			size_t CandidatePlanBytes, size_t PlanningGraphBytes, std::string& Error) const;

	private:
		void PushUndo(EditorGraph Snapshot);
		void TrimHistory();
		void BumpGraphRevision() noexcept { if (++m_GraphRevision == 0) ++m_GraphRevision; }

		EditorGraph m_Graph;
		EditorGraph m_SavedGraph;
		std::vector<EditorGraph> m_Undo;
		std::vector<EditorGraph> m_Redo;
		std::optional<EditorGraph> m_Transaction;
		uint64_t m_GraphRevision = 1;
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
		[[nodiscard]] std::optional<PulseForge::UUID> ActivePin() const noexcept { return m_ActivePin; }
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
		const EditorGraph& Graph, const EditorGraphSchema& Schema, EditorGraphWorkStats* Stats = nullptr);
	[[nodiscard]] EditorGraphConnectionDecision CanConnectEditorGraphPins(
		const EditorGraph& Graph, const EditorGraphSchema& Schema,
		PulseForge::UUID FirstPin, PulseForge::UUID SecondPin,
		std::optional<PulseForge::UUID> ReplacingConnection = std::nullopt, EditorGraphWorkStats* Stats = nullptr);
	[[nodiscard]] std::vector<EditorGraphNodeCreationAction> DiscoverEditorGraphNodeActions(
		const EditorGraph& Graph, const EditorGraphSchema& Schema,
		std::optional<PulseForge::UUID> SourcePin = std::nullopt, EditorGraphWorkStats* Stats = nullptr);
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
	[[nodiscard]] std::expected<std::vector<PulseForge::UUID>, std::string> PasteEditorGraphClipboard(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		std::string_view Source, double TargetGroupOriginX, double TargetGroupOriginY);
	[[nodiscard]] std::expected<std::vector<PulseForge::UUID>, std::string> DuplicateEditorGraphSubgraph(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		const std::vector<PulseForge::UUID>& NodeIDs, double TargetGroupOriginX, double TargetGroupOriginY);
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
	[[nodiscard]] EditorGraphPinMoveDecision CanMoveEditorGraphPinConnections(
		const EditorGraph& Graph, const EditorGraphSchema& Schema, PulseForge::UUID SourcePin,
		PulseForge::UUID TargetPin, std::span<const EditorGraphConnection> CapturedConnections,
		EditorGraphWorkStats* Stats = nullptr, bool RequireCompleteSourceSet = true);
	[[nodiscard]] std::expected<void, std::string> MoveEditorGraphPinConnections(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema, PulseForge::UUID SourcePin,
		PulseForge::UUID TargetPin, std::span<const EditorGraphConnection> CapturedConnections,
		bool RequireCompleteSourceSet = true);
	[[nodiscard]] std::expected<PulseForge::UUID, std::string> InsertEditorGraphReroute(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID ConnectionID, double X, double Y);
	[[nodiscard]] EditorGraphWireRouteDecision CanCreateEditorGraphWireRoute(
		const EditorGraph& Graph, const EditorGraphSchema& Schema, PulseForge::UUID StartPin,
		std::span<const EditorGraphVector3> KnotPositions, std::optional<PulseForge::UUID> EndPin = std::nullopt,
		EditorGraph* PlannedGraph = nullptr, EditorGraphWorkStats* Stats = nullptr,
		EditorGraphWireRouteAdmission* Admission = nullptr,
		const EditorGraphCommandHistory* History = nullptr);
	[[nodiscard]] std::expected<EditorGraphWireRouteDecision, std::string> CreateEditorGraphWireRoute(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema, PulseForge::UUID StartPin,
		std::span<const EditorGraphVector3> KnotPositions, std::optional<PulseForge::UUID> EndPin = std::nullopt,
		EditorGraphWorkStats* Stats = nullptr);
	[[nodiscard]] std::expected<void, std::string> MoveEditorGraphNodes(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		const std::vector<std::pair<PulseForge::UUID, EditorGraphVector3>>& Positions);
	[[nodiscard]] std::expected<void, std::string> SetEditorGraphPinDefaultValue(
		EditorGraphCommandHistory& History, const EditorGraphSchema& Schema,
		PulseForge::UUID PinID, EditorGraphValue Value);
}
