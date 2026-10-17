#include "Editor/ViewportGizmoGeometry.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace PulseForgeEditor
{
	namespace
	{
		constexpr size_t RotationRingSegmentCount = 48;
		constexpr size_t RotationRingPointCount = RotationRingSegmentCount + 1;

		[[nodiscard]] float DistanceSquaredToSegment(const glm::vec2& Point, const glm::vec2& Start, const glm::vec2& End)
		{
			const glm::vec2 Segment = End - Start;
			const float LengthSquared = glm::dot(Segment, Segment);
			const float Parameter =
				LengthSquared > 1.0e-6f ? std::clamp(glm::dot(Point - Start, Segment) / LengthSquared, 0.0f, 1.0f) : 0.0f;
			const glm::vec2 Difference = Point - (Start + Segment * Parameter);
			return glm::dot(Difference, Difference);
		}
	} // namespace

	std::optional<ViewportGizmoGeometry> BuildViewportGizmoGeometry(const ViewportGizmoGeometryInput& Input)
	{
		if (!Detail::IsFinite(Input.ViewProjection) || !Detail::IsFinite(Input.CameraPosition) || !Detail::IsFinite(Input.CameraForward) ||
			!Detail::IsFinite(Input.ParentWorldTransform) || !Detail::IsFinite(Input.LocalRotation) ||
			!Detail::IsFinite(Input.EntityWorldPosition))
			return std::nullopt;

		ViewportGizmoGeometry Geometry;
		Geometry.PivotWorld = Input.EntityWorldPosition;
		if (Input.HasMultipleSelectedRoots)
		{
			glm::vec3 Minimum((std::numeric_limits<float>::max)());
			glm::vec3 Maximum(-(std::numeric_limits<float>::max)());
			bool HasPosition = false;
			for (const glm::vec3& Position : Input.SelectedRootWorldPositions)
			{
				if (!Detail::IsFinite(Position)) continue;
				for (glm::length_t Axis = 0; Axis < 3; ++Axis)
				{
					Minimum[Axis] = (std::min)(Minimum[Axis], Position[Axis]);
					Maximum[Axis] = (std::max)(Maximum[Axis], Position[Axis]);
				}
				HasPosition = true;
			}
			if (HasPosition) Geometry.PivotWorld = (Minimum + Maximum) * 0.5f;
		}

		const auto PivotScreen = ProjectWorldToViewportImage(Geometry.PivotWorld, Input.ViewProjection, Input.ImageRect);
		if (!PivotScreen) return std::nullopt;
		Geometry.PivotScreen = *PivotScreen;

		const float CameraDistance = glm::length(Input.CameraPosition - Geometry.PivotWorld);
		const float ImageHeight = Input.ImageRect.Maximum.y - Input.ImageRect.Minimum.y;
		if (!std::isfinite(CameraDistance) || CameraDistance < 1.0e-4f || !std::isfinite(ImageHeight) || ImageHeight <= 1.0f)
			return std::nullopt;

		Geometry.HandleLengthWorld = CameraDistance * 2.0f * std::tan(glm::radians(45.0f) * 0.5f) * 82.0f / ImageHeight;
		if (!std::isfinite(Geometry.HandleLengthWorld) || Geometry.HandleLengthWorld <= 1.0e-6f) return std::nullopt;

		for (uint32_t AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			const auto Axis = GetGizmoAxis(Input.ParentWorldTransform, Input.LocalRotation, AxisIndex, Input.LocalSpace);
			if (!Axis) continue;

			const auto Endpoint = ProjectWorldToViewportImage(
				Geometry.PivotWorld + Axis->Direction * Geometry.HandleLengthWorld, Input.ViewProjection, Input.ImageRect);
			if (!Endpoint) continue;
			Geometry.Axes[AxisIndex] = { Axis->Direction, Axis->WorldUnitsPerLocalUnit, *Endpoint, true };

			glm::vec3 RingU = glm::cross(Axis->Direction, Input.CameraForward);
			if (glm::length(RingU) < 1.0e-4f)
			{
				const glm::vec3 Fallback = std::abs(Axis->Direction.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
				RingU = glm::cross(Axis->Direction, Fallback);
			}
			const float RingULength = glm::length(RingU);
			if (!std::isfinite(RingULength) || RingULength <= 1.0e-5f) continue;

			RingU /= RingULength;
			const glm::vec3 RingVUnnormalized = glm::cross(Axis->Direction, RingU);
			const float RingVLength = glm::length(RingVUnnormalized);
			if (!std::isfinite(RingVLength) || RingVLength <= 1.0e-5f) continue;
			const glm::vec3 RingV = RingVUnnormalized / RingVLength;
			bool RingValid = true;
			for (size_t PointIndex = 0; PointIndex < RotationRingPointCount; ++PointIndex)
			{
				const float Angle = glm::two_pi<float>() * static_cast<float>(PointIndex) / static_cast<float>(RotationRingSegmentCount);
				const glm::vec3 Point =
					Geometry.PivotWorld + Geometry.HandleLengthWorld * 0.82f * (RingU * std::cos(Angle) + RingV * std::sin(Angle));
				const auto ScreenPoint = ProjectWorldToViewportImage(Point, Input.ViewProjection, Input.ImageRect);
				if (!ScreenPoint)
				{
					RingValid = false;
					break;
				}
				Geometry.RotationRings[AxisIndex][PointIndex] = *ScreenPoint;
			}
			Geometry.RotationRingValid[AxisIndex] = RingValid;
		}

		Geometry.IsValid = true;
		return Geometry;
	}

	std::optional<ViewportGizmoHandle> FindViewportGizmoHandle(
		const ViewportGizmoGeometry& Geometry, TransformGizmoOperation Operation, const glm::vec2& MousePosition)
	{
		if (!Geometry.IsValid || !Detail::IsFinite(MousePosition)) return std::nullopt;

		constexpr float HitRadiusSquared = 8.0f * 8.0f;
		float ClosestDistance = HitRadiusSquared;
		std::optional<ViewportGizmoHandle> Closest;
		if (Operation == TransformGizmoOperation::Rotate)
		{
			for (uint32_t Axis = 0; Axis < 3; ++Axis)
			{
				if (!Geometry.RotationRingValid[Axis]) continue;
				const auto& Ring = Geometry.RotationRings[Axis];
				for (size_t Point = 0; Point + 1 < Ring.size(); ++Point)
				{
					const float Distance = DistanceSquaredToSegment(MousePosition, Ring[Point], Ring[Point + 1]);
					if (Distance < ClosestDistance)
					{
						ClosestDistance = Distance;
						Closest = ViewportGizmoHandle{ Operation, Axis };
					}
				}
			}
			return Closest;
		}

		for (uint32_t Axis = 0; Axis < 3; ++Axis)
		{
			if (!Geometry.Axes[Axis].IsValid) continue;
			const glm::vec2 ScreenAxis = Geometry.Axes[Axis].Endpoint - Geometry.PivotScreen;
			if (glm::dot(ScreenAxis, ScreenAxis) < 14.0f * 14.0f) continue;
			const glm::vec2 Start = Geometry.PivotScreen + ScreenAxis * 0.12f;
			const float Distance = DistanceSquaredToSegment(MousePosition, Start, Geometry.Axes[Axis].Endpoint);
			if (Distance < ClosestDistance)
			{
				ClosestDistance = Distance;
				Closest = ViewportGizmoHandle{ Operation, Axis };
			}
		}
		return Closest;
	}
} // namespace PulseForgeEditor
