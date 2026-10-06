#include "Core/PulseForgePCH.h"
#include "Scene/Components/CameraComponent.h"

#include <cmath>
#include <limits>

#include <glm/ext/matrix_clip_space.hpp>

namespace PulseForge
{
	std::expected<void, CameraError> CameraComponent::Validate() const
	{
		constexpr float Pi = 3.14159265358979323846f;
		if (!std::isfinite(VerticalFieldOfViewRadians) ||
			VerticalFieldOfViewRadians <= 0.0f || VerticalFieldOfViewRadians >= Pi)
		{
			return std::unexpected(CameraError{
				CameraErrorCode::InvalidFieldOfView,
				"Camera vertical field of view must be finite and between zero and pi radians" });
		}

		if (!std::isfinite(NearClipPlane) || !std::isfinite(FarClipPlane) ||
			NearClipPlane <= 0.0f || FarClipPlane <= NearClipPlane)
		{
			return std::unexpected(CameraError{
				CameraErrorCode::InvalidClipPlanes,
				"Camera clip planes must be finite, with near greater than zero and far greater than near" });
		}
		return {};
	}

	std::expected<glm::mat4, CameraError> CameraComponent::GetProjectionMatrix(float AspectRatio) const
	{
		if (auto ValidationResult = Validate(); !ValidationResult)
			return std::unexpected(ValidationResult.error());
		if (!std::isfinite(AspectRatio) || AspectRatio <= std::numeric_limits<float>::epsilon())
		{
			return std::unexpected(CameraError{
				CameraErrorCode::InvalidAspectRatio,
				"Camera aspect ratio must be finite and greater than zero" });
		}

		glm::mat4 Projection = glm::perspectiveRH_ZO(
			VerticalFieldOfViewRadians,
			AspectRatio,
			NearClipPlane,
			FarClipPlane);
		// Keep camera-up oriented toward the top of PulseForge's framebuffer coordinates.
		Projection[1][1] *= -1.0f;
		for (int Column = 0; Column < 4; ++Column)
		{
			for (int Row = 0; Row < 4; ++Row)
			{
				if (!std::isfinite(Projection[Column][Row]))
				{
					return std::unexpected(CameraError{
						CameraErrorCode::NonFiniteProjection,
						"Camera projection parameters produce a non-finite matrix" });
				}
			}
		}
		return Projection;
	}
}
