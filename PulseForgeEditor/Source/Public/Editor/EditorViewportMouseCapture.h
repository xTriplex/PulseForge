#pragma once

#include "Editor/ViewportMouseCapture.h"

#include <cstdint>
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
			GLFWwindow* Window, uint32_t ViewportID, ImGuiContext* Context, bool RightButtonPressed, bool CursorOverImage,
			bool GizmoDragging, bool PlayMode);
		void End(ImGuiContext* Context, bool RestoreCursorPosition = true) noexcept;
		[[nodiscard]] bool OwnsViewport(uint32_t ViewportID, GLFWwindow* Window) const noexcept;
		[[nodiscard]] std::optional<std::pair<float, float>> UpdateCursorPosition(ImGuiContext* Context) noexcept;

		[[nodiscard]] bool OwnsMouse() const noexcept { return m_State.OwnsMouse(); }
		[[nodiscard]] uint32_t GetOwnerViewportID() const noexcept { return m_OwnerViewportID; }

	private:
		ViewportMouseCaptureState m_State;
		GLFWwindow* m_OwnerWindow = nullptr;
		uint32_t m_OwnerViewportID = 0;
		std::optional<int> m_PreviousCursorMode;
		bool m_PreviousImGuiNoMouse = false;
		bool m_PreviousRawMouseMotion = false;
		bool m_RawMouseMotionChanged = false;
		bool m_CursorRestoreValid = false;
		double m_CursorRestoreX = 0.0;
		double m_CursorRestoreY = 0.0;
	};
} // namespace PulseForgeEditor::Detail
