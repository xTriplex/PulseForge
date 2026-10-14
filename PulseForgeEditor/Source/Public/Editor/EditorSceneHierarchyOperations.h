#pragma once

#include "Scene/Scene.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	class EditorSceneHierarchyOperations final
	{
	public:
		[[nodiscard]] static std::expected<std::vector<PulseForge::Entity>, std::string> GetTopLevelSelection(
			const PulseForge::Scene& SceneValue,
			std::span<const PulseForge::UUID> SelectedIdentifiers);
		[[nodiscard]] static std::expected<std::vector<PulseForge::Entity>, std::string> CollectSubtrees(
			std::span<const PulseForge::Entity> Roots);
		[[nodiscard]] static std::string MakeUniqueName(
			const PulseForge::Scene& SceneValue,
			std::string BaseName,
			std::optional<PulseForge::UUID> IgnoredEntity = std::nullopt);
		[[nodiscard]] static std::expected<std::vector<PulseForge::UUID>, PulseForge::SceneError> DuplicateSubtrees(
			PulseForge::Scene& SceneValue,
			std::span<const PulseForge::Entity> Roots);
	};
}
