#include "Core/PulseForgePCH.h"
#include "Renderer/DirectionalShadowMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace PulseForge
{
	namespace
	{
		bool IsFinite(const glm::mat4& Matrix)
		{
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row]))
						return false;
			return true;
		}
	}

	std::expected<DirectionalShadowCascadeSet, DirectionalShadowMathError> BuildDirectionalShadowCascades(
		const glm::mat4& View,
		const glm::mat4& Projection,
		float NearClipPlane,
		float FarClipPlane,
		float ShadowDistance,
		const glm::vec3& LightRayDirection,
		uint32_t Resolution)
	{
		if (!IsFinite(View) || !IsFinite(Projection) || !std::isfinite(NearClipPlane) ||
			!std::isfinite(FarClipPlane) || !std::isfinite(ShadowDistance) ||
			NearClipPlane <= 0.0f || FarClipPlane <= NearClipPlane || ShadowDistance <= NearClipPlane || Resolution == 0)
			return std::unexpected(DirectionalShadowMathError{ "Cascade inputs must contain finite matrices, valid clip ranges, distance, and resolution" });

		const float DirectionLength = glm::length(LightRayDirection);
		if (!std::isfinite(DirectionLength) || DirectionLength < 1.0e-6f)
			return std::unexpected(DirectionalShadowMathError{ "Directional-light ray direction must be finite and non-zero" });

		DirectionalShadowCascadeSet Result;
		Result.ShadowDistance = std::min(FarClipPlane, ShadowDistance);
		if (Result.ShadowDistance <= NearClipPlane)
			return std::unexpected(DirectionalShadowMathError{ "Shadow distance must extend beyond the camera near plane" });

		const glm::mat4 InverseProjection = glm::inverse(Projection);
		const glm::mat4 InverseView = glm::inverse(View);
		if (!IsFinite(InverseProjection) || !IsFinite(InverseView))
			return std::unexpected(DirectionalShadowMathError{ "Camera view and projection must be invertible" });

		constexpr float SplitLambda = 0.65f;
		constexpr float CasterDepthMargin = 50.0f;
		const glm::vec3 Ray = LightRayDirection / DirectionLength;
		float PreviousSplit = NearClipPlane;
		for (uint32_t CascadeIndex = 0; CascadeIndex < Result.Cascades.size(); ++CascadeIndex)
		{
			const float Fraction = static_cast<float>(CascadeIndex + 1) / static_cast<float>(Result.Cascades.size());
			const float LogSplit = NearClipPlane * std::pow(Result.ShadowDistance / NearClipPlane, Fraction);
			const float UniformSplit = NearClipPlane + (Result.ShadowDistance - NearClipPlane) * Fraction;
			const float CurrentSplit = SplitLambda * LogSplit + (1.0f - SplitLambda) * UniformSplit;
			Result.Cascades[CascadeIndex].NearDistance = PreviousSplit;
			Result.Cascades[CascadeIndex].FarDistance = CurrentSplit;

			std::array<glm::vec3, 8> Corners;
			size_t CornerIndex = 0;
			for (const float Distance : { PreviousSplit, CurrentSplit })
			{
				for (const float Y : { -1.0f, 1.0f })
				{
					for (const float X : { -1.0f, 1.0f })
					{
						glm::vec4 ViewCorner = InverseProjection * glm::vec4(X, Y, 1.0f, 1.0f);
						if (!std::isfinite(ViewCorner.w) || std::abs(ViewCorner.w) < 1.0e-6f)
							return std::unexpected(DirectionalShadowMathError{ "Projection did not produce finite perspective-frustum corners" });
						ViewCorner /= ViewCorner.w;
						if (std::abs(ViewCorner.z) < 1.0e-6f)
							return std::unexpected(DirectionalShadowMathError{ "Projection frustum ray is parallel to the view plane" });
						const glm::vec3 ViewPosition = glm::vec3(ViewCorner) * (Distance / -ViewCorner.z);
						const glm::vec4 WorldCorner = InverseView * glm::vec4(ViewPosition, 1.0f);
						if (!std::isfinite(WorldCorner.w) || std::abs(WorldCorner.w) < 1.0e-6f)
							return std::unexpected(DirectionalShadowMathError{ "Camera transform produced a non-finite world-space frustum corner" });
						Corners[CornerIndex++] = glm::vec3(WorldCorner) / WorldCorner.w;
					}
				}
			}

			glm::vec3 Center(0.0f);
			for (const glm::vec3& Corner : Corners)
				Center += Corner;
			Center /= static_cast<float>(Corners.size());
			glm::vec3 Up(0.0f, 1.0f, 0.0f);
			if (std::abs(glm::dot(Ray, Up)) > 0.98f)
				Up = glm::vec3(0.0f, 0.0f, 1.0f);
			const glm::mat4 LightView = glm::lookAtRH(Center - Ray * (CasterDepthMargin + 1.0f), Center, Up);

			glm::vec3 Minimum(std::numeric_limits<float>::max());
			glm::vec3 Maximum(std::numeric_limits<float>::lowest());
			for (const glm::vec3& Corner : Corners)
			{
				const glm::vec3 LightCorner = glm::vec3(LightView * glm::vec4(Corner, 1.0f));
				Minimum = glm::min(Minimum, LightCorner);
				Maximum = glm::max(Maximum, LightCorner);
			}
			const glm::vec2 LightCenter((Minimum.x + Maximum.x) * 0.5f, (Minimum.y + Maximum.y) * 0.5f);
			float Radius = 0.0f;
			for (const glm::vec3& Corner : Corners)
			{
				const glm::vec3 LightCorner = glm::vec3(LightView * glm::vec4(Corner, 1.0f));
				Radius = std::max(Radius, glm::length(glm::vec2(LightCorner) - LightCenter));
			}
			Radius = std::max(std::ceil(Radius * 16.0f) / 16.0f, 0.5f);
			// Expand XY slightly so nearby off-frustum casters can still contribute into the slice.
			Radius += std::max(Radius * 0.1f, 2.0f);
			const float TexelSize = (Radius * 2.0f) / static_cast<float>(Resolution);
			const glm::vec2 SnappedCenter = glm::round(LightCenter / TexelSize) * TexelSize;
			glm::mat4 StableLightView = LightView;
			StableLightView[3][0] += SnappedCenter.x - LightCenter.x;
			StableLightView[3][1] += SnappedCenter.y - LightCenter.y;

			const float MinimumDistance = -Maximum.z;
			const float MaximumDistance = -Minimum.z;
			const float NearDistance = std::max(0.01f, MinimumDistance - CasterDepthMargin);
			const float FarDistance = std::max(NearDistance + 0.1f, MaximumDistance + CasterDepthMargin);
			const glm::mat4 LightProjection = glm::orthoRH_ZO(
				-Radius, Radius, -Radius, Radius, NearDistance, FarDistance);
			Result.Cascades[CascadeIndex].ViewProjection = LightProjection * StableLightView;
			if (!IsFinite(Result.Cascades[CascadeIndex].ViewProjection))
				return std::unexpected(DirectionalShadowMathError{ "Cascade projection contains non-finite values" });
			PreviousSplit = CurrentSplit;
		}
		return Result;
	}
}
