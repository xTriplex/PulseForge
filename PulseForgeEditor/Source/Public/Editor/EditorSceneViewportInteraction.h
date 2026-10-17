#pragma once

#include "Editor/EditorMeshPickingGeometry.h"
#include "Editor/ViewportMath.h"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge
{
	class Scene;
}

namespace PulseForgeEditor
{
	class EditorSceneWorkspaceState;

	struct EditorViewportSelectionFocus
	{
		glm::vec3 Center{ 0.0f };
		float Radius = 0.35f;
	};

	struct EditorViewportSelectionFocusResult
	{
		std::optional<EditorViewportSelectionFocus> Focus;
		std::vector<std::string> Warnings;
	};

	class EditorSceneViewportInteraction final
	{
	public:
		[[nodiscard]] std::expected<std::vector<std::string>, std::string> PickEntityAt(PulseForge::Scene* SceneValue,
			EditorSceneWorkspaceState& Workspace, bool RuntimeActive, bool ViewProjectionValid, const ViewportImageRect& ImageRect,
			const glm::mat4& ViewProjection, const glm::vec3& CameraPosition, const glm::vec2& MousePosition, bool Control, bool Shift,
			const EditorMeshGeometryResolver& ResolveMesh) const;

		[[nodiscard]] std::expected<EditorViewportSelectionFocusResult, std::string> FindSelectionFocus(const PulseForge::Scene& SceneValue,
			const EditorSceneWorkspaceState& Workspace, const EditorMeshGeometryResolver& ResolveMesh) const;
	};
} // namespace PulseForgeEditor
