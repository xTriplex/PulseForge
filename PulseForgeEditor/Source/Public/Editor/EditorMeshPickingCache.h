#pragma once

#include "Assets/AssetID.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace PulseForge
{
	class Project;
}

namespace PulseForgeEditor
{
	struct EditorMeshPickingGeometry
	{
		std::vector<glm::vec3> Positions;
		std::vector<uint32_t> Indices;
	};

	class EditorMeshPickingCache final
	{
	public:
		[[nodiscard]] std::expected<const EditorMeshPickingGeometry*, std::string> Get(
			const PulseForge::Project& Project, const PulseForge::AssetID& Asset);
		void Invalidate(const PulseForge::AssetID& Asset) noexcept;
		void Clear() noexcept;
		[[nodiscard]] size_t GetGeometryCount() const noexcept { return m_Geometries.size(); }
		[[nodiscard]] size_t GetFailureCount() const noexcept { return m_Failures.size(); }

	private:
		std::optional<PulseForge::UUID> m_ProjectID;
		std::unordered_map<PulseForge::AssetID, EditorMeshPickingGeometry, PulseForge::UUIDHash> m_Geometries;
		std::unordered_map<PulseForge::AssetID, std::string, PulseForge::UUIDHash> m_Failures;
	};
} // namespace PulseForgeEditor
