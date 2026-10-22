#include "Editor/EditorViewportMouseCapture.h"

#include <cfloat>
#include <cmath>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <imgui.h>

namespace PulseForgeEditor::Detail
{
	namespace
	{
		GLFWwindow* ResolveWindow(ImGuiContext* Context, uint32_t ViewportID, GLFWwindow* ExpectedWindow) noexcept
		{
			if (!Context || ViewportID == 0 || !ExpectedWindow) return nullptr;
			ImGui::SetCurrentContext(Context);
			ImGuiViewport* Viewport = ImGui::FindViewportByID(ViewportID);
			return Viewport && Viewport->PlatformHandle == ExpectedWindow ? ExpectedWindow : nullptr;
		}
	}

	bool EditorViewportMouseCapture::Begin(
		GLFWwindow* Window, uint32_t ViewportID, ImGuiContext* Context, bool RightButtonPressed, bool CursorOverImage,
		bool GizmoDragging, bool PlayMode)
	{
		if (!Window || ViewportID == 0 || !Context || !ResolveWindow(Context, ViewportID, Window)) return false;

		ImGui::SetCurrentContext(Context);
		ImGuiIO& IO = ImGui::GetIO();
		if (IO.KeyAlt) return false;
		const bool WindowFocused = glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE;
		if (!m_State.Begin(RightButtonPressed, CursorOverImage, WindowFocused, GizmoDragging, PlayMode)) return false;

		m_PreviousCursorMode = glfwGetInputMode(Window, GLFW_CURSOR);
		glfwGetCursorPos(Window, &m_CursorRestoreX, &m_CursorRestoreY);
		m_CursorRestoreValid = std::isfinite(m_CursorRestoreX) && std::isfinite(m_CursorRestoreY);
		m_PreviousImGuiNoMouse = (IO.ConfigFlags & ImGuiConfigFlags_NoMouse) != 0;
		m_RawMouseMotionChanged = glfwRawMouseMotionSupported() == GLFW_TRUE;
		m_PreviousRawMouseMotion = m_RawMouseMotionChanged && glfwGetInputMode(Window, GLFW_RAW_MOUSE_MOTION) == GLFW_TRUE;

		IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		glfwSetInputMode(Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		if (m_RawMouseMotionChanged) glfwSetInputMode(Window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
		m_OwnerWindow = Window;
		m_OwnerViewportID = ViewportID;
		return true;
	}

	void EditorViewportMouseCapture::End(ImGuiContext* Context, bool RestoreCursorPosition) noexcept
	{
		if (!m_State.OwnsMouse()) return;
		m_State.End();
		GLFWwindow* Window = ResolveWindow(Context, m_OwnerViewportID, m_OwnerWindow);

		if (Window)
		{
			if (m_RawMouseMotionChanged) glfwSetInputMode(Window, GLFW_RAW_MOUSE_MOTION, m_PreviousRawMouseMotion ? GLFW_TRUE : GLFW_FALSE);
			if (m_PreviousCursorMode) glfwSetInputMode(Window, GLFW_CURSOR, *m_PreviousCursorMode);
			if (RestoreCursorPosition && m_CursorRestoreValid && glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE)
				glfwSetCursorPos(Window, m_CursorRestoreX, m_CursorRestoreY);
		}

		m_PreviousCursorMode.reset();
		m_RawMouseMotionChanged = false;
		m_CursorRestoreValid = false;
		m_OwnerWindow = nullptr;
		m_OwnerViewportID = 0;
		if (!Context) return;

		ImGui::SetCurrentContext(Context);
		ImGuiIO& IO = ImGui::GetIO();
		if (m_PreviousImGuiNoMouse) IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		else IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	}

	bool EditorViewportMouseCapture::OwnsViewport(uint32_t ViewportID, GLFWwindow* Window) const noexcept
	{
		return m_State.OwnsMouse() && m_OwnerViewportID == ViewportID && m_OwnerWindow == Window;
	}

	std::optional<std::pair<float, float>> EditorViewportMouseCapture::UpdateCursorPosition(ImGuiContext* Context) noexcept
	{
		GLFWwindow* Window = ResolveWindow(Context, m_OwnerViewportID, m_OwnerWindow);
		if (!m_State.OwnsMouse() || !Window) return std::nullopt;
		double X = 0.0;
		double Y = 0.0;
		glfwGetCursorPos(Window, &X, &Y);
		return m_State.UpdateCursorPosition(static_cast<float>(X), static_cast<float>(Y));
	}
} // namespace PulseForgeEditor::Detail
