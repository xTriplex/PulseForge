#pragma once

#include "Core/Core.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace PulseForge
{
	struct SceneMeshInstance
	{
		UUID Entity;
		AssetID MeshAsset;
		std::optional<AssetID> MaterialAsset;
		glm::mat4 WorldTransform{ 1.0f };
	};

	struct SceneRenderSnapshot
	{
		UUID CameraEntity;
		glm::mat4 ViewProjection{ 1.0f };
		// Scene::GetEntities returns UUID-sorted entities, so snapshot order is deterministic.
		std::vector<SceneMeshInstance> Meshes;
	};

	enum class SceneRenderSnapshotErrorCode : uint8_t
	{
		InvalidCameraEntity,
		MissingCameraComponent,
		InvalidCamera,
		InvalidCameraTransform,
		InvalidMeshTransform,
		SceneOperationFailed
	};

	struct SceneRenderSnapshotError
	{
		SceneRenderSnapshotErrorCode Code;
		UUID Entity;
		std::string Message;
	};

	class PULSEFORGE_API SceneRenderSnapshotBuilder final
	{
	public:
		// The caller chooses the active camera explicitly; this operation performs no backend work.
		[[nodiscard]] static std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> Build(
			const Scene& Source,
			UUID CameraEntity,
			float AspectRatio);
	};
}
