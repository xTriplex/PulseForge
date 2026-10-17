#pragma once

#include <cmath>
#include <optional>
#include <utility>

namespace PulseForgeEditor::Detail
{
	class ViewportMouseCaptureState final
	{
	public:
		[[nodiscard]] constexpr bool Begin(
			bool RightButtonPressed, bool CursorOverImage, bool WindowFocused, bool GizmoDragging, bool PlayMode) noexcept
		{
			if (m_Active || !RightButtonPressed || !CursorOverImage || !WindowFocused || GizmoDragging || PlayMode) return false;
			m_Active = true;
			m_IgnoreNextCursorPosition = true;
			return true;
		}

		constexpr void End() noexcept
		{
			m_Active = false;
			m_IgnoreNextCursorPosition = true;
		}

		[[nodiscard]] constexpr bool OwnsMouse() const noexcept { return m_Active; }

		[[nodiscard]] std::optional<std::pair<float, float>> UpdateCursorPosition(float X, float Y) noexcept
		{
			if (!m_Active) return std::nullopt;
			if (!std::isfinite(X) || !std::isfinite(Y))
			{
				m_IgnoreNextCursorPosition = true;
				return std::nullopt;
			}

			if (m_IgnoreNextCursorPosition)
			{
				m_LastCursorX = X;
				m_LastCursorY = Y;
				m_IgnoreNextCursorPosition = false;
				return std::nullopt;
			}

			const std::pair<float, float> Delta{ X - m_LastCursorX, Y - m_LastCursorY };
			m_LastCursorX = X;
			m_LastCursorY = Y;
			if (!std::isfinite(Delta.first) || !std::isfinite(Delta.second))
			{
				m_IgnoreNextCursorPosition = true;
				return std::nullopt;
			}
			return Delta;
		}

	private:
		bool m_Active = false;
		bool m_IgnoreNextCursorPosition = true;
		float m_LastCursorX = 0.0f;
		float m_LastCursorY = 0.0f;
	};
} // namespace PulseForgeEditor::Detail
