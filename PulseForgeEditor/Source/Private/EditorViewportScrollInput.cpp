#include "Editor/EditorViewportScrollInput.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <unordered_map>

namespace PulseForgeEditor::Detail
{
	struct EditorViewportScrollInput::State
	{
		struct WindowRoute
		{
			uint32_t ViewportID = 0;
			uint64_t Registration = 0;
			GLFWscrollfun BackendCallback = nullptr;
		};

		ImGuiContext* Context = nullptr;
		ImGuiPlatformIO* PlatformIO = nullptr;
		void (*PreviousCreateWindow)(ImGuiViewport*) = nullptr;
		void (*PreviousDestroyWindow)(ImGuiViewport*) = nullptr;
		std::unordered_map<GLFWwindow*, WindowRoute> Routes;
		ViewportScrollEventAccumulator PendingScroll;
		uint64_t NextRegistration = 1;
		bool Installed = false;

		static std::unordered_map<ImGuiContext*, State*> ContextOwners;
		static std::unordered_map<GLFWwindow*, State*> WindowOwners;
		static void CreateWindow(ImGuiViewport* Viewport);
		static void DestroyWindow(ImGuiViewport* Viewport);
		static void Scroll(GLFWwindow* Window, double XOffset, double YOffset);

		void Register(ImGuiViewport* Viewport, GLFWwindow* Window)
		{
			if (!Viewport || !Window) return;
			const uint64_t Registration = NextRegistration++;
			const GLFWscrollfun BackendCallback = glfwSetScrollCallback(Window, &Scroll);
			try
			{
				Routes.insert_or_assign(Window, WindowRoute{ Viewport->ID, Registration, BackendCallback });
				WindowOwners.insert_or_assign(Window, this);
			}
			catch (...)
			{
				Routes.erase(Window);
				glfwSetScrollCallback(Window, BackendCallback);
				throw;
			}
		}

		void Unregister(GLFWwindow* Window)
		{
			const auto Route = Routes.find(Window);
			if (Route == Routes.end()) return;
			PendingScroll.Forget(Route->second.Registration);
			Routes.erase(Route);
			WindowOwners.erase(Window);
		}
	};

	std::unordered_map<ImGuiContext*, EditorViewportScrollInput::State*> EditorViewportScrollInput::State::ContextOwners;
	std::unordered_map<GLFWwindow*, EditorViewportScrollInput::State*> EditorViewportScrollInput::State::WindowOwners;

	void EditorViewportScrollInput::State::CreateWindow(ImGuiViewport* Viewport)
	{
		const auto Context = ContextOwners.find(ImGui::GetCurrentContext());
		State* Owner = Context != ContextOwners.end() ? Context->second : nullptr;
		if (!Owner || !Owner->PreviousCreateWindow) return;
		Owner->PreviousCreateWindow(Viewport);
		try
		{
			Owner->Register(Viewport, Viewport ? static_cast<GLFWwindow*>(Viewport->PlatformHandle) : nullptr);
		}
		catch (...)
		{
			// The platform callback must not propagate an allocation failure into ImGui.
		}
	}

	void EditorViewportScrollInput::State::DestroyWindow(ImGuiViewport* Viewport)
	{
		const auto Context = ContextOwners.find(ImGui::GetCurrentContext());
		State* Owner = Context != ContextOwners.end() ? Context->second : nullptr;
		if (!Owner || !Owner->PreviousDestroyWindow) return;
		if (Viewport) Owner->Unregister(static_cast<GLFWwindow*>(Viewport->PlatformHandle));
		Owner->PreviousDestroyWindow(Viewport);
	}

	void EditorViewportScrollInput::State::Scroll(GLFWwindow* Window, double XOffset, double YOffset)
	{
		const auto WindowRoute = WindowOwners.find(Window);
		State* Owner = WindowRoute != WindowOwners.end() ? WindowRoute->second : nullptr;
		if (!Owner) return;
		const auto Route = Owner->Routes.find(Window);
		if (Route == Owner->Routes.end()) return;
		try
		{
			Owner->PendingScroll.Record(Route->second.Registration, static_cast<float>(YOffset));
		}
		catch (...)
		{
			// Never allow allocation failure to cross GLFW's C callback boundary. ImGui
			// still receives the event through the backend callback below.
		}
		if (Route->second.BackendCallback) Route->second.BackendCallback(Window, XOffset, YOffset);
	}

	void ViewportScrollEventAccumulator::Record(uint64_t Registration, float Delta)
	{
		if (Registration != 0) m_Pending[Registration] += Delta;
	}

	float ViewportScrollEventAccumulator::Consume(uint64_t Registration) noexcept
	{
		const auto Event = m_Pending.find(Registration);
		const float Result = Event != m_Pending.end() ? Event->second : 0.0f;
		m_Pending.clear();
		return Result;
	}

	void ViewportScrollEventAccumulator::Forget(uint64_t Registration) noexcept
	{
		m_Pending.erase(Registration);
	}

	EditorViewportScrollInput::EditorViewportScrollInput()
		: m_State(std::make_unique<State>())
	{
	}

	EditorViewportScrollInput::~EditorViewportScrollInput()
	{
		Shutdown();
	}

	bool EditorViewportScrollInput::Install(ImGuiContext* Context, GLFWwindow* MainWindow)
	{
		if (m_State->Installed || !Context || !MainWindow || State::ContextOwners.contains(Context)) return false;
		ImGui::SetCurrentContext(Context);
		m_State->Context = Context;
		m_State->PlatformIO = &ImGui::GetPlatformIO();
		m_State->PreviousCreateWindow = m_State->PlatformIO->Platform_CreateWindow;
		m_State->PreviousDestroyWindow = m_State->PlatformIO->Platform_DestroyWindow;
		if (!m_State->PreviousCreateWindow || !m_State->PreviousDestroyWindow) return false;

		try
		{
			State::ContextOwners.insert_or_assign(Context, m_State.get());
			m_State->PlatformIO->Platform_CreateWindow = &State::CreateWindow;
			m_State->PlatformIO->Platform_DestroyWindow = &State::DestroyWindow;
			m_State->Register(ImGui::GetMainViewport(), MainWindow);
		}
		catch (...)
		{
			if (m_State->PlatformIO->Platform_CreateWindow == &State::CreateWindow)
				m_State->PlatformIO->Platform_CreateWindow = m_State->PreviousCreateWindow;
			if (m_State->PlatformIO->Platform_DestroyWindow == &State::DestroyWindow)
				m_State->PlatformIO->Platform_DestroyWindow = m_State->PreviousDestroyWindow;
			if (const auto RegisteredContext = State::ContextOwners.find(Context);
				RegisteredContext != State::ContextOwners.end() && RegisteredContext->second == m_State.get())
				State::ContextOwners.erase(RegisteredContext);
			return false;
		}
		m_State->Installed = true;
		return true;
	}

	float EditorViewportScrollInput::Consume(uint32_t ViewportID, GLFWwindow* Window) noexcept
	{
		if (!m_State->Installed) return 0.0f;
		const auto Route = m_State->Routes.find(Window);
		if (Route == m_State->Routes.end() || Route->second.ViewportID != ViewportID)
		{
			m_State->PendingScroll.DiscardAll();
			return 0.0f;
		}
		return m_State->PendingScroll.Consume(Route->second.Registration);
	}

	void EditorViewportScrollInput::DiscardPending() noexcept
	{
		if (m_State) m_State->PendingScroll.DiscardAll();
	}

	void EditorViewportScrollInput::Shutdown() noexcept
	{
		if (!m_State || !m_State->Installed) return;
		if (m_State->PlatformIO)
		{
			if (m_State->PlatformIO->Platform_CreateWindow == &State::CreateWindow)
				m_State->PlatformIO->Platform_CreateWindow = m_State->PreviousCreateWindow;
			if (m_State->PlatformIO->Platform_DestroyWindow == &State::DestroyWindow)
				m_State->PlatformIO->Platform_DestroyWindow = m_State->PreviousDestroyWindow;
		}
		for (const auto& [Window, Route] : m_State->Routes)
		{
			const GLFWscrollfun Current = glfwSetScrollCallback(Window, nullptr);
			glfwSetScrollCallback(Window, Current == &State::Scroll ? Route.BackendCallback : Current);
			State::WindowOwners.erase(Window);
		}
		m_State->Routes.clear();
		m_State->PendingScroll.DiscardAll();
		if (const auto Context = State::ContextOwners.find(m_State->Context);
			Context != State::ContextOwners.end() && Context->second == m_State.get())
			State::ContextOwners.erase(Context);
		m_State->Installed = false;
	}
} // namespace PulseForgeEditor::Detail
