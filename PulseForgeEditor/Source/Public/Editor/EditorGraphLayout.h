#pragma once

#include "Editor/EditorGraphModel.h"

#include <vector>

namespace PulseForgeEditor
{
	struct EditorGraphLayoutNode
	{
		PulseForge::UUID ID;
		EditorGraphVector3 Position;
	};

	struct EditorGraphPinAnchor
	{
		PulseForge::UUID NodeID;
		PulseForge::UUID PinID;
		double OffsetY = 0.0;
	};

	// Calculates a deterministic best-effort vertical placement for selected nodes.
	// Connected selected pin rows are aligned along a stable spanning forest. When
	// there are no selected internal connections, nodes are aligned to the topmost
	// selected node. X coordinates and graph topology are never changed.
	[[nodiscard]] std::vector<std::pair<PulseForge::UUID, EditorGraphVector3>>
		CalculateEditorGraphStraightenedLayout(
			const std::vector<EditorGraphLayoutNode>& Nodes,
			const std::vector<EditorGraphPinAnchor>& PinAnchors,
			const std::vector<EditorGraphConnection>& Connections,
			const std::vector<PulseForge::UUID>& SelectedNodeIDs);
}
