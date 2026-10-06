#include "Core/PulseForgePCH.h"
#include "Scene/Components/RigidbodyComponent.h"

#include <cmath>

namespace PulseForge
{
	std::expected<void, RigidbodyError> RigidbodyComponent::Validate() const
	{
		if (MotionType != RigidbodyMotionType::Static && MotionType != RigidbodyMotionType::Dynamic)
		{
			return std::unexpected(RigidbodyError{
				RigidbodyErrorCode::InvalidMotionType,
				"Rigidbody motion type is not supported" });
		}
		if (!std::isfinite(Mass) || Mass <= 0.0f)
		{
			return std::unexpected(RigidbodyError{
				RigidbodyErrorCode::InvalidMass,
				"Rigidbody mass must be finite and greater than zero" });
		}
		if (!std::isfinite(Friction) || Friction < 0.0f || Friction > 1.0f)
		{
			return std::unexpected(RigidbodyError{
				RigidbodyErrorCode::InvalidFriction,
				"Rigidbody friction must be finite and between zero and one" });
		}
		if (!std::isfinite(Restitution) || Restitution < 0.0f || Restitution > 1.0f)
		{
			return std::unexpected(RigidbodyError{
				RigidbodyErrorCode::InvalidRestitution,
				"Rigidbody restitution must be finite and between zero and one" });
		}
		return {};
	}
}
