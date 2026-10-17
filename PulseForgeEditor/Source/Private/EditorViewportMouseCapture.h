#pragma once

#include "Editor/ViewportMouseCapture.h"

#include <optional>
#include <utility>

struct GLFWwindow;
struct ImGuiContext;

namespace PulseForgeEditor::Detail
{
	class EditorViewportMouseCapture final
	{
	public:
		[[nodiscard]] bool Begin(
			GLFWwindow* Window, ImGuiContext* Context, bool RightButtonPressed, bool CursorOverImage, bool GizmoDragging, bool PlayMode);
		void End(GLFWwindow* Window, ImGuiContext* Context, bool RestoreCursorPosition = true) noexcept;

		[[nodiscard]] bool OwnsMouse() const noexcept { return m_State.OwnsMouse(); }
		[[nodiscard]] std::optional<std::pair<float, float>> UpdateCursorPosition(float X, float Y) noexcept
		{
			return m_State.UpdateCursorPosition(X, Y);
		}

	private:
		ViewportMouseCaptureState m_State;
		std::optional<int> m_PreviousCursorMode;
		bool m_PreviousImGuiNoMouse = false;
		bool m_PreviousRawMouseMotion = false;
		bool m_RawMouseMotionChanged = false;
		bool m_CursorRestoreValid = false;
		double m_CursorRestoreX = 0.0;
		double m_CursorRestoreY = 0.0;
	};
} // namespace PulseForgeEditor::Detail
