#pragma once

#include "Editor/EditorDocumentManager.h"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorDocumentWorkspaceState
	{
		uint32_t Version = 5;
		bool LegacyMaterialDetailsMigration = false;
		PulseForge::UUID Project;
		std::optional<PulseForge::UUID> MaterialDetailsAsset;
		std::vector<EditorDocumentHost> Hosts;
		std::vector<std::pair<EditorDocumentKind, EditorDocumentPlacement>> RememberedPlacements;
		struct GraphPinVisibility final
		{
			EditorDocumentIdentity Document;
			PulseForge::UUID GraphID;
			PulseForge::UUID NodeID;
			PulseForge::UUID PinID;
			friend bool operator==(const GraphPinVisibility&, const GraphPinVisibility&) = default;
		};
		std::vector<GraphPinVisibility> HiddenGraphPins;
		std::string FocusedHostID = std::string(EditorDocumentManager::MainHostID);
	};

	class EditorDocumentWorkspaceCodec final
	{
	public:
		[[nodiscard]] static std::expected<std::string, std::string> Serialize(
			const EditorDocumentWorkspaceState& State);
		[[nodiscard]] static std::expected<EditorDocumentWorkspaceState, std::string> Deserialize(
			std::string_view Contents,
			const PulseForge::UUID& ExpectedProject);
	};

	[[nodiscard]] std::vector<PulseForge::AssetID> GetMaterialDetailsMigrationCandidates(
		const EditorDocumentWorkspaceState& State);
	[[nodiscard]] std::vector<PulseForge::AssetID> ConsumeLegacyMaterialDetailsEntries(
		EditorDocumentWorkspaceState& State);
}
