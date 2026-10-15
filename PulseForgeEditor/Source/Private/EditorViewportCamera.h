#pragma once

#include <expected>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace PulseForgeEditor
{
	struct ViewportCameraMoveKeys;

	class EditorViewportCamera final
	{
	public:
		void Reset() noexcept;
		void Rotate(float DeltaX, float DeltaY) noexcept;
		void Focus(const glm::vec3& Pivot, float Radius, float AspectRatio) noexcept;
		void Orbit(float DeltaX, float DeltaY) noexcept;
		void Pan(float DeltaX, float DeltaY, float ViewportHeight) noexcept;
		void Dolly(float Delta) noexcept;
		void AdjustMoveSpeed(float WheelDelta) noexcept;
		void Move(const ViewportCameraMoveKeys& Keys, double DeltaSeconds) noexcept;

		[[nodiscard]] std::expected<glm::mat4, std::string> GetViewProjection(float AspectRatio) const;
		[[nodiscard]] glm::mat4 GetView() const;
		[[nodiscard]] std::expected<glm::mat4, std::string> GetProjection(float AspectRatio) const;
		[[nodiscard]] glm::vec3 GetPosition() const noexcept;
		[[nodiscard]] glm::vec3 GetForward() const noexcept;

	private:
		glm::vec3 m_Position{ 0.0f, 0.0f, 3.1f };
		glm::vec3 m_FocusPivot{ 0.0f };
		float m_YawDegrees = -90.0f;
		float m_PitchDegrees = 0.0f;
		float m_MoveSpeed = 5.0f;
		bool m_HasFocusPivot = false;
	};
} // namespace PulseForgeEditor
