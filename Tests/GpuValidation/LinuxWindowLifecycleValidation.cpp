#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace
{
	using Clock = std::chrono::steady_clock;
	constexpr auto MinimizedHoldDuration = std::chrono::milliseconds(500);
	constexpr auto PhaseTimeout = std::chrono::seconds(8);

	enum class WindowLifecyclePhase
	{
		WaitingForResize,
		WaitingForResizedFrame,
		WaitingForMinimize,
		WaitingForRestore,
		Complete,
		Failed
	};

	class LinuxWindowLifecycleLayer final : public PulseForge::Layer
	{
	public:
		LinuxWindowLifecycleLayer() : Layer("Linux Vulkan window resize/minimize/restore validation") {}

		void OnAttach() override
		{
			m_InitialExtent = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
			if (m_InitialExtent.first == 0 || m_InitialExtent.second == 0)
			{
				Fail("The Vulkan window started with a zero framebuffer extent");
				return;
			}

			glfwSetWindowSize(static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow()), 1024, 640);
			SetPhase(WindowLifecyclePhase::WaitingForResize);
		}

		void OnUpdate(PulseForge::Timestep) override
		{
			auto& App = PulseForge::Application::Get();
			const auto Extent = App.GetWindow().GetFramebufferSize();
			const bool HasExtent = Extent.first > 0 && Extent.second > 0;
			const bool TimedOut = Clock::now() - m_PhaseStartedAt > PhaseTimeout;

			switch (m_Phase)
			{
			case WindowLifecyclePhase::WaitingForResize:
				if (HasExtent && Extent != m_InitialExtent)
				{
					PF_CORE_INFO("Linux window resized to framebuffer {0}x{1}", Extent.first, Extent.second);
					m_RenderCountAtResize = m_RenderCount;
					SetPhase(WindowLifecyclePhase::WaitingForResizedFrame);
				}
				else if (TimedOut) Fail("The GLFW window did not report a resized framebuffer extent");
				break;

			case WindowLifecyclePhase::WaitingForResizedFrame:
				if (m_RenderCount > m_RenderCountAtResize)
				{
					if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
					{
						m_WasSkipped = true;
						PF_CORE_WARN("Linux minimize validation skipped: GLFW's Wayland backend cannot observe minimized state; resize and "
									 "shutdown were exercised");
						m_Phase = WindowLifecyclePhase::Complete;
						App.RequestClose();
						break;
					}

					m_RenderCountAtMinimize = m_RenderCount;
					glfwIconifyWindow(static_cast<GLFWwindow*>(App.GetWindow().GetNativeWindow()));
					SetPhase(WindowLifecyclePhase::WaitingForMinimize);
				}
				else if (TimedOut) Fail("The resized Vulkan swapchain did not render a frame");
				break;

			case WindowLifecyclePhase::WaitingForMinimize:
			{
				const bool IsIconified =
					glfwGetWindowAttrib(static_cast<GLFWwindow*>(App.GetWindow().GetNativeWindow()), GLFW_ICONIFIED) == GLFW_TRUE;
				const bool IsMinimized = !HasExtent || IsIconified;
				if (IsMinimized && !m_SawMinimize)
				{
					m_SawMinimize = true;
					m_MinimizedAt = Clock::now();
					PF_CORE_INFO("Linux minimize observed at framebuffer {0}x{1}", Extent.first, Extent.second);
				}
				else if (m_SawMinimize && !IsMinimized)
					Fail("The Linux window left minimized state before the fixture requested restoration");
				else if (m_SawMinimize && Clock::now() - m_MinimizedAt >= MinimizedHoldDuration)
				{
					m_RenderCountBeforeRestore = m_RenderCount;
					glfwRestoreWindow(static_cast<GLFWwindow*>(App.GetWindow().GetNativeWindow()));
					SetPhase(WindowLifecyclePhase::WaitingForRestore);
				}
				else if (TimedOut)
				{
					m_WasSkipped = true;
					PF_CORE_WARN("Linux minimize validation skipped: the active compositor did not expose minimized state or a zero "
								 "framebuffer extent");
					m_Phase = WindowLifecyclePhase::Complete;
					App.RequestClose();
				}
				break;
			}

			case WindowLifecyclePhase::WaitingForRestore:
				if (HasExtent && m_RenderCount > m_RenderCountBeforeRestore)
				{
					PF_CORE_INFO(
						"Linux window restored at framebuffer {0}x{1}; requesting orderly renderer shutdown", Extent.first, Extent.second);
					m_Phase = WindowLifecyclePhase::Complete;
					App.RequestClose();
				}
				else if (TimedOut) Fail("The Vulkan window did not render again after restore");
				break;

			case WindowLifecyclePhase::Complete:
			case WindowLifecyclePhase::Failed:
				break;
			}

			if (m_Phase == WindowLifecyclePhase::Failed) App.RequestClose();
		}

		void OnRender() override
		{
			const auto Extent = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
			if (m_Phase == WindowLifecyclePhase::WaitingForMinimize && (Extent.first == 0 || Extent.second == 0))
			{
				Fail("The application rendered a main frame while its minimized framebuffer extent was zero");
				PulseForge::Application::Get().RequestClose();
				return;
			}

			++m_RenderCount;
		}

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return m_Phase == WindowLifecyclePhase::Complete && !m_WasSkipped && m_SawMinimize && m_RenderCount > m_RenderCountAtMinimize;
		}

		[[nodiscard]] bool WasSkipped() const noexcept { return m_WasSkipped; }

	private:
		void SetPhase(WindowLifecyclePhase Phase)
		{
			m_Phase = Phase;
			m_PhaseStartedAt = Clock::now();
		}

		void Fail(std::string Message)
		{
			m_Failure = std::move(Message);
			m_Phase = WindowLifecyclePhase::Failed;
			PF_CORE_ERROR("{0}", m_Failure);
		}

		WindowLifecyclePhase m_Phase = WindowLifecyclePhase::WaitingForResize;
		Clock::time_point m_PhaseStartedAt = Clock::now();
		std::pair<unsigned int, unsigned int> m_InitialExtent{};
		uint64_t m_RenderCount = 0;
		uint64_t m_RenderCountAtResize = 0;
		uint64_t m_RenderCountAtMinimize = 0;
		uint64_t m_RenderCountBeforeRestore = 0;
		Clock::time_point m_MinimizedAt{};
		bool m_SawMinimize = false;
		bool m_WasSkipped = false;
		std::string m_Failure;
	};

	class LinuxWindowLifecycleApplication final : public PulseForge::Application
	{
	public:
		LinuxWindowLifecycleApplication() : Application(PulseForge::RendererAPI::Vulkan)
		{
			auto Layer = std::make_unique<LinuxWindowLifecycleLayer>();
			m_Layer = Layer.get();
			PushLayer(std::move(Layer));
		}

		int RunValidation()
		{
			Run();
			if (m_Layer && m_Layer->WasSkipped()) return 77;
			return m_Layer && m_Layer->Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
		}

	private:
		LinuxWindowLifecycleLayer* m_Layer = nullptr;
	};
} // namespace

int main()
{
	PulseForge::Log::Init();
	try
	{
		if (std::getenv("PULSEFORGE_TEST_GLFW_X11")) glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
		LinuxWindowLifecycleApplication App;
		return App.RunValidation();
	}
	catch (const std::exception& Exception)
	{
		PF_CORE_CRITICAL("Linux window lifecycle validation failed: {0}", Exception.what());
		return EXIT_FAILURE;
	}
}
