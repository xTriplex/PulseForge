#include "Editor/EditorMeshPickingCache.h"

#include "Assets/GltfMeshImporter.h"
#include "Assets/Project.h"
#include "Editor/ViewportMath.h"

#include <utility>

namespace PulseForgeEditor
{
	std::expected<const EditorMeshPickingGeometry*, std::string> EditorMeshPickingCache::Get(
		const PulseForge::Project& Project, const PulseForge::AssetID& Asset)
	{
		const PulseForge::UUID& ProjectID = Project.GetDescription().ID;
		if (!m_ProjectID || *m_ProjectID != ProjectID)
		{
			Clear();
			m_ProjectID = ProjectID;
		}
		if (const auto Existing = m_Geometries.find(Asset); Existing != m_Geometries.end()) return &Existing->second;
		if (const auto ExistingError = m_Failures.find(Asset); ExistingError != m_Failures.end())
			return std::unexpected(ExistingError->second);

		auto Imported = PulseForge::GltfMeshImporter::ImportStaticPrimitive(Asset, Project.GetRootPath(), Project.GetAssetRegistry());
		if (!Imported)
		{
			const std::string Message =
				"Mesh " + Asset.ToString() + " could not be loaded for viewport picking: " + Imported.error().Message;
			m_Failures.emplace(Asset, Message);
			return std::unexpected(Message);
		}

		EditorMeshPickingGeometry Geometry;
		Geometry.Positions.reserve(Imported->Vertices.size());
		for (const PulseForge::GltfMeshVertex& Vertex : Imported->Vertices)
		{
			const glm::vec3 Position{ Vertex.Position[0], Vertex.Position[1], Vertex.Position[2] };
			if (!Detail::IsFinite(Position))
			{
				const std::string Message = "Mesh " + Asset.ToString() + " contains a non-finite vertex position for viewport picking";
				m_Failures.emplace(Asset, Message);
				return std::unexpected(Message);
			}
			Geometry.Positions.push_back(Position);
		}
		Geometry.Indices = std::move(Imported->Indices);
		auto [Inserted, WasInserted] = m_Geometries.emplace(Asset, std::move(Geometry));
		(void)WasInserted;
		return &Inserted->second;
	}

	void EditorMeshPickingCache::Invalidate(const PulseForge::AssetID& Asset) noexcept
	{
		m_Geometries.erase(Asset);
		m_Failures.erase(Asset);
	}

	void EditorMeshPickingCache::Clear() noexcept
	{
		m_ProjectID.reset();
		m_Geometries.clear();
		m_Failures.clear();
	}
} // namespace PulseForgeEditor
