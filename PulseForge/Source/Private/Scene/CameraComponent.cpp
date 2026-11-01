#include "Scene/Components/CameraComponent.h"

#include <cmath>
#include <limits>

#include <glm/ext/matrix_clip_space.hpp>

namespace PulseForge
{
	std::expected<void, CameraError> CameraComponent::Validate() const
	{
		constexpr float Pi = 3.14159265358979323846f;
		if (ProjectionType == CameraProjectionType::Perspective && (!std::isfinite(VerticalFieldOfViewRadians) ||
			VerticalFieldOfViewRadians <= 0.0f || VerticalFieldOfViewRadians >= Pi))
		{
			return std::unexpected(CameraError{
				CameraErrorCode::InvalidFieldOfView,
				"Camera vertical field of view must be finite and between zero and pi radians" });
		}
		if (ProjectionType == CameraProjectionType::Orthographic &&
			(!std::isfinite(OrthographicHeight) || OrthographicHeight <= std::numeric_limits<float>::epsilon()))
			return std::unexpected(CameraError{ CameraErrorCode::InvalidOrthographicHeight,
				"Orthographic camera height must be finite and greater than zero" });
		if (ProjectionType != CameraProjectionType::Perspective && ProjectionType != CameraProjectionType::Orthographic)
			return std::unexpected(CameraError{ CameraErrorCode::InvalidFieldOfView, "Camera projection type is unsupported" });

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

		glm::mat4 Projection;
		if (ProjectionType == CameraProjectionType::Orthographic)
		{
			const float HalfHeight = OrthographicHeight * 0.5f;
			const float HalfWidth = HalfHeight * AspectRatio;
			Projection = glm::orthoRH_ZO(-HalfWidth, HalfWidth, -HalfHeight, HalfHeight, NearClipPlane, FarClipPlane);
		}
		else
			Projection = glm::perspectiveRH_ZO(VerticalFieldOfViewRadians, AspectRatio, NearClipPlane, FarClipPlane);
		// NVRHI maps clip-space +Y to the framebuffer top using a negative-height Vulkan viewport.
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
