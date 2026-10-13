#include "Editor/EditorDocumentWorkspace.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace
{
	using Json = nlohmann::ordered_json;
	using namespace PulseForgeEditor;

	constexpr size_t MaximumWorkspaceBytes = 2 * 1024 * 1024;
	constexpr size_t MaximumWorkspaceHosts = 64;
	constexpr size_t MaximumWorkspaceDocuments = 512;

	bool IsValidKind(int64_t Value)
	{
		return Value >= static_cast<int64_t>(EditorDocumentKind::Level) &&
			Value <= static_cast<int64_t>(EditorDocumentKind::Texture);
	}

	bool IsValidHostID(std::string_view ID, EditorDocumentHostRole Role)
	{
		if (ID == EditorDocumentManager::MainHostID)
			return Role == EditorDocumentHostRole::Main;
		if (Role != EditorDocumentHostRole::Native || !ID.starts_with("native:") || ID.size() > 256)
			return false;
		for (const char Character : ID)
			if (!(Character >= 'a' && Character <= 'z') && !(Character >= 'A' && Character <= 'Z') &&
				!(Character >= '0' && Character <= '9') && Character != ':' && Character != '-' &&
				Character != '_')
				return false;
		return true;
	}

	bool IsValidBounds(const EditorDocumentHost::WindowBounds& Bounds)
	{
		if (!Bounds.HasValue)
			return true;
		return std::isfinite(Bounds.X) && std::isfinite(Bounds.Y) &&
			std::isfinite(Bounds.Width) && std::isfinite(Bounds.Height) &&
			std::isfinite(Bounds.DpiScale) && std::abs(Bounds.X) <= 100000.0f &&
			std::abs(Bounds.Y) <= 100000.0f && Bounds.Width >= 160.0f && Bounds.Width <= 16384.0f &&
			Bounds.Height >= 120.0f && Bounds.Height <= 16384.0f &&
			Bounds.DpiScale >= 0.5f && Bounds.DpiScale <= 8.0f && Bounds.MonitorName.size() <= 256;
	}

	std::expected<void, std::string> ValidateWorkspace(const EditorDocumentWorkspaceState& State)
	{
		if (State.Version != 1 || State.Project.IsNil())
			return std::unexpected("Document workspace version or project identity is invalid");
		if (State.Hosts.empty() || State.Hosts.size() > MaximumWorkspaceHosts)
			return std::unexpected("Document workspace host count is invalid");

		std::unordered_set<std::string> HostIDs;
		std::unordered_set<EditorDocumentIdentity, EditorDocumentIdentityHash> Documents;
		const EditorDocumentHost* MainHost = nullptr;
		bool HasLevelEditor = false;
		size_t DocumentCount = 0;
		for (const EditorDocumentHost& Host : State.Hosts)
		{
			if (!IsValidHostID(Host.ID, Host.Role) || !HostIDs.insert(Host.ID).second ||
				static_cast<uint8_t>(Host.PlacementKind) > static_cast<uint8_t>(EditorDocumentKind::Texture) ||
				!IsValidBounds(Host.Bounds))
				return std::unexpected("Document workspace contains an invalid or duplicate host");
			if (Host.Role == EditorDocumentHostRole::Main)
			{
				if (MainHost || Host.PlacementKind != EditorDocumentKind::Level)
					return std::unexpected("Document workspace must contain one valid main document host");
				MainHost = &Host;
			}
			else if (Host.Documents.empty())
				return std::unexpected("Empty native document hosts are not persisted");

			DocumentCount += Host.Documents.size();
			if (DocumentCount > MaximumWorkspaceDocuments)
				return std::unexpected("Document workspace contains too many documents");
			for (const EditorDocumentIdentity& Identity : Host.Documents)
			{
				if (Identity.Project != State.Project ||
					static_cast<uint8_t>(Identity.Kind) > static_cast<uint8_t>(EditorDocumentKind::Texture) ||
					(Identity.Kind == EditorDocumentKind::Level &&
						(Host.Role != EditorDocumentHostRole::Main || !Identity.Asset.IsNil())) ||
					(Identity.Kind != EditorDocumentKind::Level && Identity.Asset.IsNil()) ||
					!Documents.insert(Identity).second)
					return std::unexpected("Document workspace contains an illegal or duplicate document identity");
				if (Identity.Kind == EditorDocumentKind::Level)
					HasLevelEditor = true;
			}
			if (Host.ActiveDocument && std::find(Host.Documents.begin(), Host.Documents.end(), *Host.ActiveDocument) ==
				Host.Documents.end())
				return std::unexpected("Document workspace active document is not a member of its host");
		}
		if (!MainHost || !HasLevelEditor || !HostIDs.contains(State.FocusedHostID))
			return std::unexpected("Document workspace has no main host or references an unknown focused host");

		std::unordered_set<uint8_t> RememberedKinds;
		for (const auto& [Kind, HostID] : State.RememberedHosts)
		{
			if (Kind == EditorDocumentKind::Level || !RememberedKinds.insert(static_cast<uint8_t>(Kind)).second)
				return std::unexpected("Document workspace contains an invalid remembered placement");
			const auto Host = std::find_if(State.Hosts.begin(), State.Hosts.end(), [&HostID](const auto& Candidate)
			{
				return Candidate.ID == HostID && Candidate.Role == EditorDocumentHostRole::Native;
			});
			if (Host == State.Hosts.end() || Host->PlacementKind != Kind)
				return std::unexpected("Document workspace remembered placement references an incompatible host");
		}
		return {};
	}

	Json SerializeIdentity(const EditorDocumentIdentity& Identity)
	{
		return Json{ { "asset", Identity.Asset.ToString() }, { "kind", static_cast<uint8_t>(Identity.Kind) } };
	}

	std::expected<EditorDocumentIdentity, std::string> DeserializeIdentity(
		const Json& Value,
		const PulseForge::UUID& Project)
	{
		if (!Value.is_object() || !Value.contains("asset") || !Value["asset"].is_string() ||
			!Value.contains("kind") || !Value["kind"].is_number_integer())
			return std::unexpected("A persisted document identity is malformed");
		const int64_t KindValue = Value["kind"].get<int64_t>();
		if (!IsValidKind(KindValue))
			return std::unexpected("A persisted document kind is not supported");
		const std::string AssetText = Value["asset"].get<std::string>();
		const auto Asset = PulseForge::UUID::Parse(AssetText);
		if (!Asset)
			return std::unexpected("A persisted asset identity is malformed");
		return EditorDocumentIdentity{ Project, *Asset, static_cast<EditorDocumentKind>(KindValue) };
	}
}

namespace PulseForgeEditor
{
	std::expected<std::string, std::string> EditorDocumentWorkspaceCodec::Serialize(
		const EditorDocumentWorkspaceState& State)
	{
		if (auto Valid = ValidateWorkspace(State); !Valid)
			return std::unexpected(Valid.error());

		Json Root;
		Root["version"] = State.Version;
		Root["project"] = State.Project.ToString();
		Root["focusedHost"] = State.FocusedHostID;
		Root["hosts"] = Json::array();
		for (const EditorDocumentHost& Host : State.Hosts)
		{
			Json HostJSON;
			HostJSON["id"] = Host.ID;
			HostJSON["role"] = static_cast<uint8_t>(Host.Role);
			HostJSON["placementKind"] = static_cast<uint8_t>(Host.PlacementKind);
			HostJSON["documents"] = Json::array();
			for (const EditorDocumentIdentity& Identity : Host.Documents)
				HostJSON["documents"].push_back(SerializeIdentity(Identity));
			HostJSON["active"] = Host.ActiveDocument ? SerializeIdentity(*Host.ActiveDocument) : Json(nullptr);
			HostJSON["bounds"] = {
				{ "valid", Host.Bounds.HasValue }, { "x", Host.Bounds.X }, { "y", Host.Bounds.Y },
				{ "width", Host.Bounds.Width }, { "height", Host.Bounds.Height },
				{ "dpiScale", Host.Bounds.DpiScale }, { "monitor", Host.Bounds.MonitorName },
				{ "maximized", Host.Bounds.Maximized }
			};
			Root["hosts"].push_back(std::move(HostJSON));
		}
		Root["rememberedHosts"] = Json::array();
		for (const auto& [Kind, HostID] : State.RememberedHosts)
			Root["rememberedHosts"].push_back({ { "kind", static_cast<uint8_t>(Kind) }, { "host", HostID } });

		try
		{
			std::string Contents = Root.dump(2);
			if (Contents.size() > MaximumWorkspaceBytes)
				return std::unexpected("Serialized document workspace exceeds the safety limit");
			return Contents;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not serialize document workspace: ") + Exception.what());
		}
	}

	std::expected<EditorDocumentWorkspaceState, std::string> EditorDocumentWorkspaceCodec::Deserialize(
		std::string_view Contents,
		const PulseForge::UUID& ExpectedProject)
	{
		if (Contents.empty() || Contents.size() > MaximumWorkspaceBytes || ExpectedProject.IsNil())
			return std::unexpected("Document workspace size or expected project identity is invalid");
		try
		{
			const Json Root = Json::parse(Contents);
			if (!Root.is_object() || !Root.contains("version") || !Root["version"].is_number_unsigned() ||
				!Root.contains("project") || !Root["project"].is_string() ||
				!Root.contains("focusedHost") || !Root["focusedHost"].is_string() ||
				!Root.contains("hosts") || !Root["hosts"].is_array() ||
				!Root.contains("rememberedHosts") || !Root["rememberedHosts"].is_array())
				return std::unexpected("Document workspace root fields are malformed");

			const auto Project = PulseForge::UUID::Parse(Root["project"].get<std::string>());
			if (!Project || *Project != ExpectedProject)
				return std::unexpected("Document workspace belongs to a different or invalid project");
			EditorDocumentWorkspaceState State;
			State.Version = Root["version"].get<uint32_t>();
			State.Project = *Project;
			State.FocusedHostID = Root["focusedHost"].get<std::string>();
			if (Root["hosts"].size() > MaximumWorkspaceHosts)
				return std::unexpected("Document workspace host count exceeds the safety limit");
			for (const Json& HostJSON : Root["hosts"])
			{
				if (!HostJSON.is_object() || !HostJSON.contains("id") || !HostJSON["id"].is_string() ||
					!HostJSON.contains("role") || !HostJSON["role"].is_number_integer() ||
					!HostJSON.contains("placementKind") || !HostJSON["placementKind"].is_number_integer() ||
					!HostJSON.contains("documents") || !HostJSON["documents"].is_array() ||
					!HostJSON.contains("active") || !HostJSON.contains("bounds") || !HostJSON["bounds"].is_object())
					return std::unexpected("A persisted document host is malformed");
				const int64_t RoleValue = HostJSON["role"].get<int64_t>();
				const int64_t KindValue = HostJSON["placementKind"].get<int64_t>();
				if (RoleValue < 0 || RoleValue > static_cast<int64_t>(EditorDocumentHostRole::Native) ||
					!IsValidKind(KindValue))
					return std::unexpected("A persisted document host role or editor kind is invalid");

				EditorDocumentHost Host;
				Host.ID = HostJSON["id"].get<std::string>();
				Host.Role = static_cast<EditorDocumentHostRole>(RoleValue);
				Host.PlacementKind = static_cast<EditorDocumentKind>(KindValue);
				if (HostJSON["documents"].size() > MaximumWorkspaceDocuments)
					return std::unexpected("A persisted document host has too many tabs");
				for (const Json& DocumentJSON : HostJSON["documents"])
				{
					auto Identity = DeserializeIdentity(DocumentJSON, State.Project);
					if (!Identity)
						return std::unexpected(Identity.error());
					Host.Documents.push_back(*Identity);
				}
				if (!HostJSON["active"].is_null())
				{
					auto Active = DeserializeIdentity(HostJSON["active"], State.Project);
					if (!Active)
						return std::unexpected(Active.error());
					Host.ActiveDocument = *Active;
				}

				const Json& BoundsJSON = HostJSON["bounds"];
				if (!BoundsJSON.contains("valid") || !BoundsJSON["valid"].is_boolean() ||
					!BoundsJSON.contains("maximized") || !BoundsJSON["maximized"].is_boolean())
					return std::unexpected("A persisted native window bounds record is malformed");
				Host.Bounds.HasValue = BoundsJSON["valid"].get<bool>();
				Host.Bounds.Maximized = BoundsJSON["maximized"].get<bool>();
				if (!BoundsJSON.contains("x") || !BoundsJSON["x"].is_number() ||
					!BoundsJSON.contains("y") || !BoundsJSON["y"].is_number() ||
					!BoundsJSON.contains("width") || !BoundsJSON["width"].is_number() ||
					!BoundsJSON.contains("height") || !BoundsJSON["height"].is_number() ||
					!BoundsJSON.contains("dpiScale") || !BoundsJSON["dpiScale"].is_number() ||
					!BoundsJSON.contains("monitor") || !BoundsJSON["monitor"].is_string())
					return std::unexpected("A persisted native window bounds record is incomplete");
				Host.Bounds.X = BoundsJSON["x"].get<float>();
				Host.Bounds.Y = BoundsJSON["y"].get<float>();
				Host.Bounds.Width = BoundsJSON["width"].get<float>();
				Host.Bounds.Height = BoundsJSON["height"].get<float>();
				Host.Bounds.DpiScale = BoundsJSON["dpiScale"].get<float>();
				Host.Bounds.MonitorName = BoundsJSON["monitor"].get<std::string>();
				State.Hosts.push_back(std::move(Host));
			}

			if (Root["rememberedHosts"].size() > MaximumWorkspaceHosts)
				return std::unexpected("Remembered document host count exceeds the safety limit");
			for (const Json& RememberedJSON : Root["rememberedHosts"])
			{
				if (!RememberedJSON.is_object() || !RememberedJSON.contains("kind") ||
					!RememberedJSON["kind"].is_number_integer() || !RememberedJSON.contains("host") ||
					!RememberedJSON["host"].is_string())
					return std::unexpected("A remembered document placement is malformed");
				const int64_t KindValue = RememberedJSON["kind"].get<int64_t>();
				if (!IsValidKind(KindValue))
					return std::unexpected("A remembered document editor kind is invalid");
				State.RememberedHosts.emplace_back(
					static_cast<EditorDocumentKind>(KindValue), RememberedJSON["host"].get<std::string>());
			}

			auto Valid = ValidateWorkspace(State);
			if (!Valid)
				return std::unexpected(Valid.error());
			return State;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not parse document workspace: ") + Exception.what());
		}
	}
}
