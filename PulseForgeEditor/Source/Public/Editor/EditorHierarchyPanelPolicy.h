#pragma once

#include "Editor/EditorWorkspaceView.h"
#include "Scene/Scene.h"

#include <string_view>
#include <unordered_set>
#include <vector>

namespace PulseForgeEditor
{
	[[nodiscard]] inline std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> GetVisibleHierarchyEntities(
		const PulseForge::Scene& SceneValue, std::string_view Search)
	{
		std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> VisibleEntities;
		if (Search.empty()) return VisibleEntities;

		const std::vector<PulseForge::Entity> Entities = SceneValue.GetEntities();
		VisibleEntities.reserve(Entities.size());
		for (const PulseForge::Entity& Candidate : Entities)
		{
			const auto CandidateTag = Candidate.GetTag();
			if (!CandidateTag || !ContainsCaseInsensitive(CandidateTag->Name, Search)) continue;

			PulseForge::Entity Ancestor = Candidate;
			while (Ancestor)
			{
				if (!VisibleEntities.insert(Ancestor.GetUUID()).second) break;
				const auto Parent = Ancestor.GetParent();
				if (!Parent || !Parent->has_value()) break;
				Ancestor = **Parent;
			}
		}
		return VisibleEntities;
	}
} // namespace PulseForgeEditor
