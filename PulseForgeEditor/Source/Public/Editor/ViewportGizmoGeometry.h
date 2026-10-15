#pragma once

#include "Editor/ViewportMath.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace PulseForgeEditor
{
	struct ViewportGizmoAxisScreen
	{
		glm::vec3 WorldDirection{ 0.0f };
		float WorldUnitsPerLocalUnit = 0.0f;
		glm::vec2 Endpoint{ 0.0f };
		bool IsValid = false;
	};

	struct ViewportGizmoGeometry
	{
		glm::vec3 PivotWorld{ 0.0f };
		glm::vec2 PivotScreen{ 0.0f };
		float HandleLengthWorld = 0.0f;
		std::array<ViewportGizmoAxisScreen, 3> Axes{};
		std::array<std::array<glm::vec2, 49>, 3> RotationRings{};
		std::array<bool, 3> RotationRingValid{};
		bool IsValid = false;
	};

	struct ViewportGizmoHandle
	{
		TransformGizmoOperation Operation = TransformGizmoOperation::Translate;
		uint32_t Axis = 0;
	};

	struct ViewportGizmoGeometryInput
	{
		glm::mat4 ViewProjection{ 1.0f };
		glm::vec3 CameraPosition{ 0.0f };
		glm::vec3 CameraForward{ 0.0f, 0.0f, -1.0f };
		glm::mat4 ParentWorldTransform{ 1.0f };
		glm::quat LocalRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 EntityWorldPosition{ 0.0f };
		std::span<const glm::vec3> SelectedRootWorldPositions;
		ViewportImageRect ImageRect{};
		bool LocalSpace = true;
		bool HasMultipleSelectedRoots = false;
	};

	[[nodiscard]] std::optional<ViewportGizmoGeometry> BuildViewportGizmoGeometry(const ViewportGizmoGeometryInput& Input);

	[[nodiscard]] std::optional<ViewportGizmoHandle> FindViewportGizmoHandle(
		const ViewportGizmoGeometry& Geometry, TransformGizmoOperation Operation, const glm::vec2& MousePosition);
} // namespace PulseForgeEditor
