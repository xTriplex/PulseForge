#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Events/Event.h"
#include "Window/Window.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <stdexcept>

namespace
{
	class EditorLayer final : public PulseForge::Layer
	{
	public:
		EditorLayer()
			: Layer("Editor")
		{
		}

		~EditorLayer() override
		{
			ShutdownImGui();
		}

		void OnAttach() override
		{
			m_Context = ImGui::CreateContext();
			if (!m_Context)
				throw std::runtime_error("Could not create the editor ImGui context");

			ImGui::SetCurrentContext(m_Context);
			ImGuiIO& IO = ImGui::GetIO();
			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable |
				ImGuiConfigFlags_ViewportsEnable;
			ImGui::StyleColorsDark();

			ImGuiStyle& Style = ImGui::GetStyle();
			Style.WindowRounding = 0.0f;
			Style.Colors[ImGuiCol_WindowBg].w = 1.0f;

			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			if (!ImGui_ImplGlfw_InitForOpenGL(Window, true))
			{
				ShutdownImGui();
				throw std::runtime_error("Could not initialize the editor GLFW ImGui backend");
			}
			m_GlfwBackendActive = true;

			if (!ImGui_ImplOpenGL3_Init("#version 410"))
			{
				ShutdownImGui();
				throw std::runtime_error("Could not initialize the editor OpenGL ImGui backend");
			}
			m_OpenGLBackendActive = true;
		}

		void OnDetach() override
		{
			ShutdownImGui();
		}

		void OnRender() override
		{
			ImGui::SetCurrentContext(m_Context);
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGuiIO& IO = ImGui::GetIO();
			const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
			IO.DisplaySize = ImVec2(static_cast<float>(Width), static_cast<float>(Height));
			ImGui::NewFrame();

			ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
			DrawMainMenu();
			DrawWorkspacePanels();

			ImGui::Render();
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

			if (IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				GLFWwindow* MainContext = glfwGetCurrentContext();
				ImGui::UpdatePlatformWindows();
				ImGui::RenderPlatformWindowsDefault();
				glfwMakeContextCurrent(MainContext);
			}
		}

		void OnEvent(PulseForge::Event& Event) override
		{
			ImGui::SetCurrentContext(m_Context);
			const ImGuiIO& IO = ImGui::GetIO();
			Event.bHandled |= Event.IsInCategory(PulseForge::EventCategoryMouse) && IO.WantCaptureMouse;
			Event.bHandled |= Event.IsInCategory(PulseForge::EventCategoryKeyboard) && IO.WantCaptureKeyboard;
		}

	private:
		static void DrawMainMenu()
		{
			if (!ImGui::BeginMainMenuBar())
				return;

			ImGui::TextUnformatted("PulseForge");
			if (ImGui::BeginMenu("File"))
			{
				ImGui::MenuItem("New Project", nullptr, false, false);
				ImGui::MenuItem("Open Project", nullptr, false, false);
				ImGui::Separator();
				ImGui::MenuItem("Exit", nullptr, false, false);
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		static void DrawWorkspacePanels()
		{
			if (ImGui::Begin("Scene"))
				ImGui::TextUnformatted("Open a project and scene to begin.");
			ImGui::End();

			if (ImGui::Begin("Hierarchy"))
				ImGui::TextUnformatted("No scene is open.");
			ImGui::End();

			if (ImGui::Begin("Inspector"))
				ImGui::TextUnformatted("Select an entity to inspect its components.");
			ImGui::End();

			if (ImGui::Begin("Content Browser"))
				ImGui::TextUnformatted("Open a project to browse managed assets.");
			ImGui::End();

			if (ImGui::Begin("Console"))
				ImGui::TextUnformatted("Engine diagnostics will appear here when connected.");
			ImGui::End();
		}

		void ShutdownImGui() noexcept
		{
			if (!m_Context)
				return;

			ImGui::SetCurrentContext(m_Context);
			if (m_OpenGLBackendActive)
				ImGui_ImplOpenGL3_Shutdown();
			if (m_GlfwBackendActive)
				ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext(m_Context);
			m_Context = nullptr;
			m_OpenGLBackendActive = false;
			m_GlfwBackendActive = false;
		}

		ImGuiContext* m_Context = nullptr;
		bool m_GlfwBackendActive = false;
		bool m_OpenGLBackendActive = false;
	};

	class PulseForgeEditorApplication final : public PulseForge::Application
	{
	public:
		PulseForgeEditorApplication()
			: Application(PulseForge::RendererAPI::OpenGL)
		{
			PushLayer(std::make_unique<EditorLayer>());
			PF_INFO("PulseForge editor shell started; scene and project operations are not connected yet");
		}
	};
}

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication()
{
	return std::make_unique<PulseForgeEditorApplication>();
}
