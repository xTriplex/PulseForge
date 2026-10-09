#pragma once

namespace PulseForgeEditor::Detail
{
	class ViewportMouseCaptureState final
	{
	public:
		[[nodiscard]] constexpr bool Begin(bool RightButtonPressed, bool CursorOverImage, bool WindowFocused,
			bool GizmoDragging, bool PlayMode) noexcept
		{
			if (m_Active || !RightButtonPressed || !CursorOverImage || !WindowFocused || GizmoDragging || PlayMode)
				return false;
			m_Active = true;
			return true;
		}

		constexpr void End() noexcept { m_Active = false; }
		[[nodiscard]] constexpr bool OwnsMouse() const noexcept { return m_Active; }

	private:
		bool m_Active = false;
	};
}
