#include "Core/PulseForgePCH.h"
#include "Renderer/SpotlightShadowMath.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace PulseForge
{
	namespace
	{
		bool IsFinite(const glm::mat4& Matrix) noexcept
		{
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row]))
						return false;
			return true;
		}

		bool IsFinite(const glm::vec3& Value) noexcept
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}
	}

	std::expected<SpotlightShadowProjection, SpotlightShadowMathError> BuildSpotlightShadowProjection(
		const glm::vec3& Position,
		const glm::vec3& Direction,
		float Range,
		float OuterConeHalfAngleDegrees,
		float AspectRatio)
	{
		if (!IsFinite(Position) || !IsFinite(Direction) || !std::isfinite(Range) ||
			!std::isfinite(OuterConeHalfAngleDegrees) || !std::isfinite(AspectRatio))
			return std::unexpected(SpotlightShadowMathError{ "Spotlight shadow projection inputs must be finite" });
		const float DirectionLength = glm::length(Direction);
		if (DirectionLength <= 1.0e-6f || Range <= MinSpotlightShadowNearPlane || AspectRatio <= 0.0f)
			return std::unexpected(SpotlightShadowMathError{ "Spotlight shadow direction, range, and aspect ratio must be valid" });
		if (OuterConeHalfAngleDegrees <= 0.0f ||
			OuterConeHalfAngleDegrees > MaxSpotlightShadowOuterHalfAngleDegrees)
			return std::unexpected(SpotlightShadowMathError{
				"Spotlight shadow cone half-angle must be greater than zero and no wider than 80 degrees" });

		const glm::vec3 Forward = Direction / DirectionLength;
		glm::vec3 Up(0.0f, 1.0f, 0.0f);
		if (std::abs(glm::dot(Forward, Up)) > 0.98f)
			Up = glm::vec3(1.0f, 0.0f, 0.0f);

		SpotlightShadowProjection Result;
		Result.View = glm::lookAtRH(Position, Position + Forward, Up);
		Result.Projection = glm::perspectiveRH_ZO(
			glm::radians(OuterConeHalfAngleDegrees * 2.0f), AspectRatio, MinSpotlightShadowNearPlane, Range);
		Result.ViewProjection = Result.Projection * Result.View;
		if (!IsFinite(Result.View) || !IsFinite(Result.Projection) || !IsFinite(Result.ViewProjection))
			return std::unexpected(SpotlightShadowMathError{ "Spotlight shadow view-projection contains non-finite values" });
		return Result;
	}

	bool ProjectSpotlightShadowCoordinate(
		const glm::mat4& ViewProjection,
		const glm::vec3& WorldPosition,
		glm::vec3& UvDepth) noexcept
	{
		if (!IsFinite(ViewProjection) || !IsFinite(WorldPosition))
			return false;
		const glm::vec4 Clip = ViewProjection * glm::vec4(WorldPosition, 1.0f);
		if (!std::isfinite(Clip.w) || Clip.w <= 1.0e-6f)
			return false;
		const glm::vec3 Ndc = glm::vec3(Clip) / Clip.w;
		if (!IsFinite(Ndc) || Ndc.x < -1.0f || Ndc.x > 1.0f || Ndc.y < -1.0f || Ndc.y > 1.0f ||
			Ndc.z < 0.0f || Ndc.z > 1.0f)
			return false;
		UvDepth = glm::vec3(Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f, Ndc.z);
		return IsFinite(UvDepth);
	}
}
