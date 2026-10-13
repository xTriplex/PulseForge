#pragma once

#include "Editor/EditorDocumentManager.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorDocumentWorkspaceState
	{
		uint32_t Version = 1;
		PulseForge::UUID Project;
		std::vector<EditorDocumentHost> Hosts;
		std::vector<std::pair<EditorDocumentKind, std::string>> RememberedHosts;
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
}
