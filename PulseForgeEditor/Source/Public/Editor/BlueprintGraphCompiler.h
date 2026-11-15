#pragma once

#include "Editor/EditorGraphModel.h"
#include "Scripting/BlueprintAsset.h"

#include <expected>
#include <string>

namespace PulseForgeEditor
{
	enum class BlueprintGraphDiagnosticCode : uint8_t
	{
		InvalidGraph,
		MissingEvent,
		UnsupportedNode,
		InvalidProperty,
		InvalidExpression,
		LimitExceeded,
		InvalidProgram
	};

	struct BlueprintGraphDiagnostic
	{
		BlueprintGraphDiagnosticCode Code = BlueprintGraphDiagnosticCode::InvalidGraph;
		PulseForge::UUID Node;
		PulseForge::UUID Pin;
		std::string Message;
	};

	[[nodiscard]] EditorGraphSchema MakeBlueprintAuthoringSchema();
	[[nodiscard]] std::expected<PulseForge::VerifiedBlueprintAsset, BlueprintGraphDiagnostic>
		CompileBlueprintGraph(const EditorGraph& Graph, const EditorGraphSchema& Schema);
}
