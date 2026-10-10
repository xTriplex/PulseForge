#include "Core/PulseForgePCH.h"
#include "Renderer/PointLightShadowMath.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace PulseForge
{
	namespace
	{
		bool IsFinite(const glm::vec3& Value) noexcept
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool IsFinite(const glm::mat4& Matrix) noexcept
		{
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row]))
						return false;
			return true;
		}
	}

	std::expected<PointShadowProjectionSet, PointShadowMathError> BuildPointShadowProjections(
		const glm::vec3& Position,
		float Range)
	{
		if (!IsFinite(Position) || !std::isfinite(Range) || Range <= 0.0f)
			return std::unexpected(PointShadowMathError{ "Point-shadow position and range must be finite and range must be positive" });

		PointShadowProjectionSet Result;
		Result.NearPlane = (std::min)(PointLightShadowPreferredNearPlane, Range * 0.01f);
		Result.FarPlane = Range;
		if (!std::isfinite(Result.NearPlane) || Result.NearPlane <= 0.0f || Result.NearPlane >= Result.FarPlane)
			return std::unexpected(PointShadowMathError{ "Point-shadow near/far planes are not valid for the light range" });

		constexpr std::array<glm::vec3, 6> Directions = {
			glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)
		};
		// These up vectors match Vulkan cube image coordinates; the Y faces avoid a parallel world-up basis.
		constexpr std::array<glm::vec3, 6> Ups = {
			glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f),
			glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)
		};
		const glm::mat4 Projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, Result.NearPlane, Result.FarPlane);
		for (size_t Face = 0; Face < Result.Faces.size(); ++Face)
		{
			PointShadowFaceProjection& Output = Result.Faces[Face];
			Output.Direction = Directions[Face];
			Output.Up = Ups[Face];
			Output.View = glm::lookAtRH(Position, Position + Output.Direction, Output.Up);
			Output.Projection = Projection;
			Output.ViewProjection = Projection * Output.View;
			if (!IsFinite(Output.View) || !IsFinite(Output.Projection) || !IsFinite(Output.ViewProjection))
				return std::unexpected(PointShadowMathError{ "Point-shadow face view-projection contains non-finite values" });
		}
		return Result;
	}

	PointShadowCubeFace SelectPointShadowCubeFace(const glm::vec3& Direction) noexcept
	{
		const glm::vec3 Absolute = glm::abs(Direction);
		if (Absolute.x >= Absolute.y && Absolute.x >= Absolute.z)
			return Direction.x >= 0.0f ? PointShadowCubeFace::PositiveX : PointShadowCubeFace::NegativeX;
		if (Absolute.y >= Absolute.z)
			return Direction.y >= 0.0f ? PointShadowCubeFace::PositiveY : PointShadowCubeFace::NegativeY;
		return Direction.z >= 0.0f ? PointShadowCubeFace::PositiveZ : PointShadowCubeFace::NegativeZ;
	}

	float EncodePointShadowRadialDepth(float Distance, float Range) noexcept
	{
		if (!std::isfinite(Distance) || !std::isfinite(Range) || Distance < 0.0f || Range <= 0.0f)
			return 1.0f;
		return glm::clamp(Distance / Range, 0.0f, 1.0f);
	}

	bool IsPointShadowOccluded(float ReceiverDepth, float StoredDepth, float Bias) noexcept
	{
		if (!std::isfinite(ReceiverDepth) || !std::isfinite(StoredDepth) || !std::isfinite(Bias) || Bias < 0.0f)
			return false;
		return ReceiverDepth - Bias > StoredDepth;
	}
}
