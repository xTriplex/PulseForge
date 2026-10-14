#include "EditorViewportCamera.h"

#include "Editor/ViewportMath.h"
#include "Scene/Components/CameraComponent.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace PulseForgeEditor
{
	void EditorViewportCamera::Reset() noexcept
	{
		m_Position = { 0.0f, 0.0f, 3.1f };
		m_YawDegrees = -90.0f;
		m_PitchDegrees = 0.0f;
		m_MoveSpeed = 5.0f;
		m_HasFocusPivot = false;
	}

	void EditorViewportCamera::Rotate(float DeltaX, float DeltaY) noexcept
	{
		constexpr float Sensitivity = 0.1f;
		m_YawDegrees = std::remainder(m_YawDegrees + DeltaX * Sensitivity, 360.0f);
		m_PitchDegrees = std::clamp(m_PitchDegrees - DeltaY * Sensitivity, -89.0f, 89.0f);
		m_HasFocusPivot = false;
	}

	void EditorViewportCamera::Focus(const glm::vec3& Pivot, float Radius, float AspectRatio) noexcept
	{
		if (!Detail::IsFinite(Pivot) || !std::isfinite(Radius) || Radius < 0.0f ||
			!std::isfinite(AspectRatio) || AspectRatio <= 0.0f)
			return;
		constexpr float VerticalFov = glm::radians(45.0f);
		const float HorizontalFov = 2.0f * std::atan(std::tan(VerticalFov * 0.5f) * AspectRatio);
		const float LimitingFov = (std::min)(VerticalFov, HorizontalFov);
		const float Distance = std::clamp(Radius / std::sin(LimitingFov * 0.5f) * 1.2f, 0.2f, 100000.0f);
		if (!std::isfinite(Distance))
			return;
		m_FocusPivot = Pivot;
		m_HasFocusPivot = true;
		m_Position = Pivot - GetForward() * Distance;
	}

	void EditorViewportCamera::Orbit(float DeltaX, float DeltaY) noexcept
	{
		if (!m_HasFocusPivot)
			return;
		const float Distance = glm::length(m_Position - m_FocusPivot);
		constexpr float Sensitivity = 0.1f;
		m_YawDegrees = std::remainder(m_YawDegrees + DeltaX * Sensitivity, 360.0f);
		m_PitchDegrees = std::clamp(m_PitchDegrees - DeltaY * Sensitivity, -89.0f, 89.0f);
		m_Position = m_FocusPivot - GetForward() * Distance;
	}

	void EditorViewportCamera::Pan(float DeltaX, float DeltaY, float ViewportHeight) noexcept
	{
		if (!m_HasFocusPivot || !std::isfinite(ViewportHeight) || ViewportHeight < 1.0f)
			return;
		const float Distance = glm::length(m_Position - m_FocusPivot);
		const float UnitsPerPixel = 2.0f * Distance * std::tan(glm::radians(45.0f) * 0.5f) / ViewportHeight;
		const glm::vec3 Right = glm::normalize(glm::cross(GetForward(), glm::vec3(0.0f, 1.0f, 0.0f)));
		const glm::vec3 Up = glm::normalize(glm::cross(Right, GetForward()));
		const glm::vec3 Offset = (-Right * DeltaX + Up * DeltaY) * UnitsPerPixel;
		m_Position += Offset;
		m_FocusPivot += Offset;
	}

	void EditorViewportCamera::Dolly(float Delta) noexcept
	{
		if (!std::isfinite(Delta) || std::abs(Delta) < 1.0e-6f)
			return;
		const std::optional<glm::vec3> ExistingPivot = m_HasFocusPivot
			? std::optional<glm::vec3>{ m_FocusPivot }
			: std::nullopt;
		const auto DollyResult = CalculateViewportCameraDolly(m_Position, GetForward(), ExistingPivot, m_MoveSpeed, Delta);
		if (!DollyResult)
			return;
		m_Position = DollyResult->Position;
		m_FocusPivot = DollyResult->FocusPivot;
		m_HasFocusPivot = true;
	}

	void EditorViewportCamera::AdjustMoveSpeed(float WheelDelta) noexcept
	{
		if (!std::isfinite(WheelDelta))
			return;
		m_MoveSpeed = std::clamp(m_MoveSpeed * std::pow(1.2f, WheelDelta), 0.2f, 80.0f);
	}

	void EditorViewportCamera::Move(const ViewportCameraMoveKeys& Keys, double DeltaSeconds) noexcept
	{
		const glm::vec3 Forward = GetForward();
		const glm::vec3 Right = glm::normalize(glm::cross(Forward, glm::vec3(0.0f, 1.0f, 0.0f)));
		const glm::vec3 Offset = CalculateViewportCameraMoveOffset(Forward, Right, Keys, m_MoveSpeed, DeltaSeconds);
		if (glm::dot(Offset, Offset) < 1.0e-12f)
			return;
		m_Position += Offset;
		if (m_HasFocusPivot)
			m_FocusPivot += Offset;
	}

	std::expected<glm::mat4, std::string> EditorViewportCamera::GetViewProjection(float AspectRatio) const
	{
		PulseForge::CameraComponent Camera;
		auto Projection = Camera.GetProjectionMatrix(AspectRatio);
		if (!Projection)
			return std::unexpected(Projection.error().Message);
		return *Projection * GetView();
	}

	glm::mat4 EditorViewportCamera::GetView() const
	{
		return glm::lookAtRH(m_Position, m_Position + GetForward(), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	std::expected<glm::mat4, std::string> EditorViewportCamera::GetProjection(float AspectRatio) const
	{
		PulseForge::CameraComponent Camera;
		auto Projection = Camera.GetProjectionMatrix(AspectRatio);
		if (!Projection)
			return std::unexpected(Projection.error().Message);
		return *Projection;
	}

	glm::vec3 EditorViewportCamera::GetPosition() const noexcept
	{
		return m_Position;
	}

	glm::vec3 EditorViewportCamera::GetForward() const noexcept
	{
		const float Yaw = glm::radians(m_YawDegrees);
		const float Pitch = glm::radians(m_PitchDegrees);
		return glm::normalize(glm::vec3(
			std::cos(Yaw) * std::cos(Pitch),
			std::sin(Pitch),
			std::sin(Yaw) * std::cos(Pitch)));
	}
}
