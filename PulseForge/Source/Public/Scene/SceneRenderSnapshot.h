#pragma once

#include "Core/Core.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace PulseForge
{
	// Returns the inverse-transpose of the model's linear transform, or null for non-finite/singular transforms.
	[[nodiscard]] PULSEFORGE_API std::optional<glm::mat4> BuildNormalTransform(const glm::mat4& Model) noexcept;

	struct SceneMeshInstance
	{
		UUID Entity;
		AssetID MeshAsset;
		std::optional<AssetID> MaterialAsset;
		glm::mat4 WorldTransform{ 1.0f };
	};

	struct SceneDirectionalLight
	{
		UUID Entity;
		// Direction in which light rays travel; surface-to-light direction is its negation in the BRDF.
		glm::vec3 RayDirection{ 0.0f, 0.0f, -1.0f };
		glm::vec3 Color{ 1.0f };
		float Intensity = 1.0f;
	};

	struct SceneEnvironmentLight
	{
		UUID Entity;
		AssetID HdrImage;
		float Intensity = 1.0f;
		// Rotation composition is parent-world * local; shaders rotate world sample vectors by the inverse.
		glm::quat WorldRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	};

	struct SceneRenderSnapshot
	{
		// Empty when the view is transient editor state rather than an authored scene camera.
		std::optional<UUID> CameraEntity;
		glm::mat4 ViewProjection{ 1.0f };
		glm::vec3 CameraWorldPosition{ 0.0f };
		std::optional<SceneDirectionalLight> DirectionalLight;
		std::optional<SceneEnvironmentLight> EnvironmentLight;
		// Scene::GetEntities returns UUID-sorted entities, so snapshot order is deterministic.
		std::vector<SceneMeshInstance> Meshes;
	};

	enum class SceneRenderSnapshotErrorCode : uint8_t
	{
		InvalidCameraEntity,
		MissingCameraComponent,
		InvalidCamera,
		InvalidCameraTransform,
		InvalidViewProjection,
		InvalidMeshTransform,
		InvalidDirectionalLightTransform,
		MultipleDirectionalLights,
		InvalidEnvironmentTransform,
		MultipleEnvironmentLights,
		MissingPrimaryCamera,
		MultiplePrimaryCameras,
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
		// Selects the scene's sole primary camera; no backend work is performed.
		[[nodiscard]] static std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> Build(
			const Scene& Source,
			float AspectRatio);
		// Builds scene geometry using a caller-owned transient view without requiring an authored camera entity.
		[[nodiscard]] static std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> BuildForView(
			const Scene& Source,
			const glm::mat4& ViewProjection,
			const glm::vec3& CameraWorldPosition);
	};
}
