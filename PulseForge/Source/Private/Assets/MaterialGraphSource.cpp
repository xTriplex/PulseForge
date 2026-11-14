#include "Assets/MaterialGraphSource.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::json;
		constexpr size_t MaximumGraphBytes = 1024 * 1024;
		constexpr size_t MaximumGraphNodes = 1024;
		constexpr size_t MaximumGraphConnections = 4096;

		std::string_view KindName(MaterialGraphNodeKind Kind)
		{
			switch (Kind)
			{
			case MaterialGraphNodeKind::FloatConstant: return "floatConstant";
			case MaterialGraphNodeKind::ColorConstant: return "colorConstant";
			case MaterialGraphNodeKind::FloatParameter: return "floatParameter";
			case MaterialGraphNodeKind::Add: return "add";
			case MaterialGraphNodeKind::Multiply: return "multiply";
			case MaterialGraphNodeKind::Reroute: return "reroute";
			case MaterialGraphNodeKind::UV0: return "uv0";
			case MaterialGraphNodeKind::BaseColorTextureSample: return "baseColorTextureSample";
			case MaterialGraphNodeKind::PbrOutput: return "pbrOutput";
			}
			return {};
		}

		std::optional<MaterialGraphNodeKind> ParseKind(std::string_view Name)
		{
			for (const auto Kind : { MaterialGraphNodeKind::FloatConstant, MaterialGraphNodeKind::ColorConstant,
				MaterialGraphNodeKind::FloatParameter,
				MaterialGraphNodeKind::Add, MaterialGraphNodeKind::Multiply, MaterialGraphNodeKind::Reroute,
				MaterialGraphNodeKind::UV0, MaterialGraphNodeKind::BaseColorTextureSample,
				MaterialGraphNodeKind::PbrOutput })
				if (KindName(Kind) == Name) return Kind;
			return std::nullopt;
		}

		bool IsValidParameterName(std::string_view Name)
		{
			if (Name.empty() || Name.size() > 64) return false;
			const auto IsStart = [](char C) { return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_'; };
			const auto IsContinue = [&](char C) { return IsStart(C) || (C >= '0' && C <= '9'); };
			if (!IsStart(Name.front())) return false;
			return std::all_of(Name.begin() + 1, Name.end(), IsContinue);
		}
	}

	UUID GetMaterialGraphPinID(UUID Node, bool Output, uint8_t Slot) noexcept
	{
		if (Node.IsNil()) return {};
		const uint64_t PinKind = (static_cast<uint64_t>(Output) << 63) | Slot;
		const auto Mix = [](uint64_t Value)
		{
			Value ^= Value >> 30;
			Value *= 0xbf58476d1ce4e5b9ull;
			Value ^= Value >> 27;
			Value *= 0x94d049bb133111ebull;
			return Value ^ (Value >> 31);
		};
		uint64_t High = Mix(Node.GetHigh() ^ std::rotl(Node.GetLow(), 23) ^ 0x5046474d41545049ull ^ PinKind);
		uint64_t Low = Mix(Node.GetLow() ^ std::rotl(Node.GetHigh(), 41) ^ 0x4e47524150485049ull ^
			(PinKind * 0x9e3779b97f4a7c15ull));
		if (High == 0 && Low == 0) Low = 1;
		return UUID(High, Low);
	}

	MaterialGraphValidation ValidateMaterialGraphSource(const MaterialGraphSource& Source)
	{
		MaterialGraphValidation Result;
		const auto Add = [&](UUID Node, std::string Message, UUID Pin = UUID{})
			{ Result.Diagnostics.push_back({ Node, Pin, std::move(Message) }); };
		if (Source.Version != MaterialGraphSource::CurrentVersion) Add({}, "Unsupported material graph source version");
		if (Source.GraphID.IsNil()) Add({}, "Material graph identity must not be nil");
		if (Source.Nodes.empty() || Source.Nodes.size() > MaximumGraphNodes) Add({}, "Material graph node count is outside the supported range");
		if (Source.Connections.size() > MaximumGraphConnections) Add({}, "Material graph connection count exceeds the supported limit");

		std::unordered_map<UUID, const MaterialGraphNode*, UUIDHash> Nodes;
		std::unordered_map<UUID, std::vector<UUID>, UUIDHash> Inputs;
		std::unordered_map<UUID, std::array<UUID, 3>, UUIDHash> InputSources;
		std::unordered_set<UUID, UUIDHash> ConnectionIDs;
		std::unordered_map<std::string, UUID> ParameterNames;
		size_t OutputCount = 0;
		for (const MaterialGraphNode& Node : Source.Nodes)
		{
			if (KindName(Node.Kind).empty()) Add(Node.ID, "Node kind is not supported");
			if (Node.ID.IsNil() || !Nodes.emplace(Node.ID, &Node).second) Add(Node.ID, "Node identity is nil or duplicated");
			if (!std::isfinite(Node.X) || !std::isfinite(Node.Y) || std::abs(Node.X) > 1'000'000.0 ||
				std::abs(Node.Y) > 1'000'000.0)
				Add(Node.ID, "Node presentation coordinates must be finite and within the supported range");
			if (Node.Kind == MaterialGraphNodeKind::PbrOutput) ++OutputCount;
			if ((Node.Kind == MaterialGraphNodeKind::FloatConstant || Node.Kind == MaterialGraphNodeKind::FloatParameter) &&
				!std::isfinite(Node.Value)) Add(Node.ID, "Float node value must be finite");
			if (Node.Kind == MaterialGraphNodeKind::ColorConstant &&
				!std::ranges::all_of(Node.Color, [](float Component) { return std::isfinite(Component); }))
				Add(Node.ID, "Color node components must be finite");
			if (Node.Kind == MaterialGraphNodeKind::FloatParameter && !IsValidParameterName(Node.ParameterName))
				Add(Node.ID, "Parameter name must be a valid identifier of at most 64 characters");
			if (Node.Kind == MaterialGraphNodeKind::FloatParameter && IsValidParameterName(Node.ParameterName) &&
				!ParameterNames.emplace(Node.ParameterName, Node.ID).second)
				Add(Node.ID, "Material parameter names must be unique");
		}
		if (OutputCount != 1) Add({}, "Material graph must contain exactly one PBR output node");

		for (const MaterialGraphConnection& Connection : Source.Connections)
		{
			if (Connection.ID.IsNil() || !ConnectionIDs.insert(Connection.ID).second)
				Add(Connection.InputNode, "Connection identity is nil or duplicated");
			const auto From = Nodes.find(Connection.OutputNode);
			const auto To = Nodes.find(Connection.InputNode);
			if (From == Nodes.end() || To == Nodes.end())
			{
				Add(Connection.InputNode, "Connection references a missing node");
				continue;
			}
			if (From->second->Kind == MaterialGraphNodeKind::PbrOutput ||
				To->second->Kind == MaterialGraphNodeKind::FloatConstant ||
				To->second->Kind == MaterialGraphNodeKind::FloatParameter ||
				To->second->Kind == MaterialGraphNodeKind::UV0)
			{
				Add(Connection.InputNode, "Connection direction is incompatible with node kinds");
				continue;
			}
			const uint8_t SlotCount = To->second->Kind == MaterialGraphNodeKind::Add ||
				To->second->Kind == MaterialGraphNodeKind::Multiply ? 2 :
				To->second->Kind == MaterialGraphNodeKind::PbrOutput ? 3 : 1;
			if (Connection.InputSlot >= SlotCount)
			{
				Add(Connection.InputNode, "Connection input slot is not defined by this node",
					GetMaterialGraphPinID(Connection.InputNode, false, Connection.InputSlot));
				continue;
			}
			auto& Slots = Inputs[Connection.InputNode];
			if (std::find_if(Source.Connections.begin(), Source.Connections.end(), [&](const auto& Existing)
				{ return Existing.ID != Connection.ID && Existing.InputNode == Connection.InputNode && Existing.InputSlot == Connection.InputSlot; })
				!= Source.Connections.end())
				Add(Connection.InputNode, "An input slot may have only one connection",
					GetMaterialGraphPinID(Connection.InputNode, false, Connection.InputSlot));
			Slots.push_back(Connection.OutputNode);
			InputSources[Connection.InputNode][Connection.InputSlot] = Connection.OutputNode;
		}

		for (const MaterialGraphNode& Node : Source.Nodes)
		{
			if (Node.Kind != MaterialGraphNodeKind::Add && Node.Kind != MaterialGraphNodeKind::Multiply &&
				Node.Kind != MaterialGraphNodeKind::Reroute && Node.Kind != MaterialGraphNodeKind::BaseColorTextureSample &&
				Node.Kind != MaterialGraphNodeKind::PbrOutput) continue;
			const uint8_t Required = Node.Kind == MaterialGraphNodeKind::Add || Node.Kind == MaterialGraphNodeKind::Multiply ? 2 : 1;
			for (uint8_t Slot = 0; Slot < Required; ++Slot)
				if (std::none_of(Source.Connections.begin(), Source.Connections.end(), [&](const auto& Connection)
					{ return Connection.InputNode == Node.ID && Connection.InputSlot == Slot; }))
					Add(Node.ID, "Required material graph input is unconnected",
						GetMaterialGraphPinID(Node.ID, false, Slot));
		}

		std::unordered_map<UUID, uint8_t, UUIDHash> Marks;
		const auto Visit = [&](auto&& Self, UUID ID) -> bool
		{
			auto& Mark = Marks[ID];
			if (Mark == 1) return false;
			if (Mark == 2) return true;
			Mark = 1;
			for (const auto& Connection : Source.Connections)
				if (Connection.InputNode == ID && Nodes.contains(Connection.OutputNode) && !Self(Self, Connection.OutputNode)) return false;
			Mark = 2;
			return true;
		};
		for (const auto& Node : Source.Nodes)
			if (!Visit(Visit, Node.ID)) { Add(Node.ID, "Material graph data cycles are unsupported"); break; }

		enum class ValueType : uint8_t { Invalid, Scalar, Color, UV };
		std::unordered_map<UUID, ValueType, UUIDHash> ResolvedTypes;
		std::unordered_set<UUID, UUIDHash> ResolvingTypes;
		const auto ResolveType = [&](auto&& Self, UUID NodeID) -> ValueType
		{
			if (const auto Cached = ResolvedTypes.find(NodeID); Cached != ResolvedTypes.end()) return Cached->second;
			const auto Node = Nodes.find(NodeID);
			if (Node == Nodes.end() || !ResolvingTypes.insert(NodeID).second) return ValueType::Invalid;
			ValueType Type = ValueType::Invalid;
			switch (Node->second->Kind)
			{
			case MaterialGraphNodeKind::FloatConstant:
			case MaterialGraphNodeKind::FloatParameter:
			case MaterialGraphNodeKind::Add:
			case MaterialGraphNodeKind::Multiply: Type = ValueType::Scalar; break;
			case MaterialGraphNodeKind::ColorConstant:
			case MaterialGraphNodeKind::BaseColorTextureSample: Type = ValueType::Color; break;
			case MaterialGraphNodeKind::UV0: Type = ValueType::UV; break;
			case MaterialGraphNodeKind::Reroute:
				if (InputSources.contains(NodeID) && !InputSources.at(NodeID)[0].IsNil())
					Type = Self(Self, InputSources.at(NodeID)[0]);
				break;
			case MaterialGraphNodeKind::PbrOutput: break;
			}
			ResolvingTypes.erase(NodeID);
			ResolvedTypes.emplace(NodeID, Type);
			return Type;
		};
		for (const MaterialGraphConnection& Connection : Source.Connections)
		{
			const auto Input = Nodes.find(Connection.InputNode);
			if (Input == Nodes.end()) continue;
			const ValueType OutputType = ResolveType(ResolveType, Connection.OutputNode);
			const MaterialGraphNodeKind InputKind = Input->second->Kind;
			if ((InputKind == MaterialGraphNodeKind::Add || InputKind == MaterialGraphNodeKind::Multiply) &&
				OutputType != ValueType::Scalar)
				Add(Connection.InputNode, "Scalar arithmetic inputs accept only scalar values",
					GetMaterialGraphPinID(Connection.InputNode, false, Connection.InputSlot));
			else if (InputKind == MaterialGraphNodeKind::BaseColorTextureSample && OutputType != ValueType::UV)
				Add(Connection.InputNode, "Base-color texture sampling requires UV0 coordinates",
					GetMaterialGraphPinID(Connection.InputNode, false, Connection.InputSlot));
			else if (InputKind == MaterialGraphNodeKind::PbrOutput &&
				((Connection.InputSlot == 0 && OutputType != ValueType::Scalar && OutputType != ValueType::Color) ||
				 (Connection.InputSlot > 0 && OutputType != ValueType::Scalar)))
				Add(Connection.InputNode, Connection.InputSlot == 0
					? "PBR base color accepts scalar or color values, not UV coordinates"
					: "PBR metallic and roughness inputs accept only scalar values",
					GetMaterialGraphPinID(Connection.InputNode, false, Connection.InputSlot));
		}
		return Result;
	}

	std::expected<MaterialGraphIR, MaterialGraphValidation> LowerMaterialGraphToIR(const MaterialGraphSource& Source)
	{
		MaterialGraphValidation Validation = ValidateMaterialGraphSource(Source);
		if (!Validation.IsValid()) return std::unexpected(std::move(Validation));

		std::unordered_map<UUID, const MaterialGraphNode*, UUIDHash> Nodes;
		std::unordered_map<UUID, std::array<const MaterialGraphConnection*, 3>, UUIDHash> Inputs;
		for (const auto& Node : Source.Nodes) Nodes.emplace(Node.ID, &Node);
		for (const auto& Connection : Source.Connections) Inputs[Connection.InputNode][Connection.InputSlot] = &Connection;
		const auto Output = std::find_if(Source.Nodes.begin(), Source.Nodes.end(), [](const auto& Node)
			{ return Node.Kind == MaterialGraphNodeKind::PbrOutput; });

		MaterialGraphIR IR;
		IR.SourceGraph = Source.GraphID;
		IR.OutputNode = Output->ID;
		IR.Expressions.reserve(Source.Nodes.size());
		std::unordered_map<UUID, uint32_t, UUIDHash> Lowered;
		const auto Lower = [&](auto&& Self, UUID NodeID) -> uint32_t
		{
			if (const auto Found = Lowered.find(NodeID); Found != Lowered.end()) return Found->second;
			const MaterialGraphNode& Node = *Nodes.at(NodeID);
			if (Node.Kind == MaterialGraphNodeKind::Reroute)
				return Self(Self, Inputs.at(Node.ID)[0]->OutputNode);
			MaterialGraphExpression Expression;
			Expression.SourceNode = Node.ID;
			Expression.Value = Node.Value;
			Expression.Color = Node.Color;
			Expression.ParameterName = Node.ParameterName;
			switch (Node.Kind)
			{
			case MaterialGraphNodeKind::FloatConstant: Expression.Op = MaterialGraphExpressionOp::FloatConstant; break;
			case MaterialGraphNodeKind::ColorConstant: Expression.Op = MaterialGraphExpressionOp::ColorConstant; break;
			case MaterialGraphNodeKind::FloatParameter: Expression.Op = MaterialGraphExpressionOp::FloatParameter; break;
			case MaterialGraphNodeKind::UV0: Expression.Op = MaterialGraphExpressionOp::UV0; break;
			case MaterialGraphNodeKind::BaseColorTextureSample:
				Expression.Op = MaterialGraphExpressionOp::BaseColorTextureSample;
				Expression.Left = Self(Self, Inputs.at(Node.ID)[0]->OutputNode);
				break;
			case MaterialGraphNodeKind::Add:
			case MaterialGraphNodeKind::Multiply:
			{
				Expression.Op = Node.Kind == MaterialGraphNodeKind::Add
					? MaterialGraphExpressionOp::Add : MaterialGraphExpressionOp::Multiply;
				const auto& ConnectedInputs = Inputs.at(Node.ID);
				Expression.Left = Self(Self, ConnectedInputs[0]->OutputNode);
				Expression.Right = Self(Self, ConnectedInputs[1]->OutputNode);
				break;
			}
			case MaterialGraphNodeKind::Reroute:
				break;
			case MaterialGraphNodeKind::PbrOutput:
				break;
			}
			const uint32_t Index = static_cast<uint32_t>(IR.Expressions.size());
			IR.Expressions.push_back(std::move(Expression));
			Lowered.emplace(NodeID, Index);
			return Index;
		};
		IR.OutputExpression = Lower(Lower, Inputs.at(Output->ID)[0]->OutputNode);
		if (Inputs.at(Output->ID)[1])
			IR.MetallicExpression = Lower(Lower, Inputs.at(Output->ID)[1]->OutputNode);
		if (Inputs.at(Output->ID)[2])
			IR.RoughnessExpression = Lower(Lower, Inputs.at(Output->ID)[2]->OutputNode);
		return IR;
	}

	std::expected<std::string, std::string> SerializeMaterialGraphSource(const MaterialGraphSource& Source)
	{
		const auto Validation = ValidateMaterialGraphSource(Source);
		if (!Validation.IsValid()) return std::unexpected(Validation.Diagnostics.front().Message);
		Json Document = {
			{ "schema", "pulseforge.material-graph" }, { "version", Source.Version }, { "graph", Source.GraphID.ToString() },
			{ "nodes", Json::array() }, { "connections", Json::array() }
		};
		auto Nodes = Source.Nodes;
		std::sort(Nodes.begin(), Nodes.end(), [](const auto& A, const auto& B) { return A.ID < B.ID; });
		for (const auto& Node : Nodes)
			Document["nodes"].push_back({ { "id", Node.ID.ToString() }, { "kind", KindName(Node.Kind) },
				{ "value", Node.Value }, { "color", Node.Color }, { "parameter", Node.ParameterName },
				{ "x", Node.X }, { "y", Node.Y } });
		auto Connections = Source.Connections;
		std::sort(Connections.begin(), Connections.end(), [](const auto& A, const auto& B) { return A.ID < B.ID; });
		for (const auto& Connection : Connections)
			Document["connections"].push_back({ { "id", Connection.ID.ToString() },
				{ "output", Connection.OutputNode.ToString() }, { "input", Connection.InputNode.ToString() },
				{ "slot", Connection.InputSlot } });
		const std::string Bytes = Document.dump(2) + "\n";
		if (Bytes.size() > MaximumGraphBytes) return std::unexpected("Serialized material graph exceeds the size limit");
		return Bytes;
	}

	std::expected<MaterialGraphSource, std::string> DeserializeMaterialGraphSource(std::string_view Bytes)
	{
		if (Bytes.size() > MaximumGraphBytes) return std::unexpected("Material graph source exceeds the size limit");
		Json Document;
		try { Document = Json::parse(Bytes); }
		catch (const Json::exception& Error) { return std::unexpected(Error.what()); }
		if (!Document.is_object() || Document.value("schema", std::string{}) != "pulseforge.material-graph")
			return std::unexpected("Material graph source schema is invalid");
		if (!Document.contains("nodes") || !Document["nodes"].is_array() || Document["nodes"].size() > MaximumGraphNodes ||
			!Document.contains("connections") || !Document["connections"].is_array() || Document["connections"].size() > MaximumGraphConnections)
			return std::unexpected("Material graph source node or connection list is invalid");
		try
		{
			MaterialGraphSource Source;
			const uint32_t SourceVersion = Document.at("version").get<uint32_t>();
			if (SourceVersion < 1 || SourceVersion > MaterialGraphSource::CurrentVersion)
				return std::unexpected("Material graph source version is unsupported");
			Source.Version = MaterialGraphSource::CurrentVersion;
			const auto GraphID = UUID::Parse(Document.at("graph").get<std::string>());
			if (!GraphID) return std::unexpected(GraphID.error().Message);
			Source.GraphID = *GraphID;
			for (const auto& Item : Document["nodes"])
			{
				const auto ID = UUID::Parse(Item.at("id").get<std::string>());
				const auto Kind = ParseKind(Item.at("kind").get<std::string>());
				if (!ID || !Kind) return std::unexpected("Material graph node identity or kind is invalid");
				MaterialGraphNode Node{ *ID, *Kind, Item.value("value", 0.0f), Item.value("parameter", std::string{}),
					Item.value("x", 0.0), Item.value("y", 0.0) };
				if (Item.contains("color")) Node.Color = Item.at("color").get<std::array<float, 3>>();
				Source.Nodes.push_back(std::move(Node));
			}
			for (const auto& Item : Document["connections"])
			{
				const auto ID = UUID::Parse(Item.at("id").get<std::string>());
				const auto Output = UUID::Parse(Item.at("output").get<std::string>());
				const auto Input = UUID::Parse(Item.at("input").get<std::string>());
				if (!ID || !Output || !Input) return std::unexpected("Material graph connection identity is invalid");
				Source.Connections.push_back({ *ID, *Output, *Input, Item.at("slot").get<uint8_t>() });
			}
			const auto Validation = ValidateMaterialGraphSource(Source);
			if (!Validation.IsValid()) return std::unexpected(Validation.Diagnostics.front().Message);
			return Source;
		}
		catch (const Json::exception& Error) { return std::unexpected(Error.what()); }
	}
}
