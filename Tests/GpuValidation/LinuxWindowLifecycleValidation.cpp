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
		WaitingForBaseline,
		WaitingForBaselineFrame,
		WaitingForResize,
		WaitingForResizedFrame,
		WaitingForMinimize,
		WaitingForRestoreObservation,
		WaitingForRestoredFrame,
		Complete,
		Failed
	};

	class LinuxWindowLifecycleLayer final : public PulseForge::Layer
	{
	public:
		LinuxWindowLifecycleLayer() : Layer("Linux Vulkan window resize/minimize/restore validation") {}

		void OnAttach() override
		{
			auto& Window = PulseForge::Application::Get().GetWindow();
			m_NativeWindow = static_cast<GLFWwindow*>(Window.GetNativeWindow());
			if (!m_NativeWindow)
			{
				Fail("The Linux lifecycle fixture did not receive a GLFW window");
				return;
			}

			const int Platform = glfwGetPlatform();
			if (Platform != GLFW_PLATFORM_X11 && Platform != GLFW_PLATFORM_WAYLAND)
			{
				Fail("The Linux lifecycle fixture requires GLFW X11 or Wayland; active GLFW platform is " + std::to_string(Platform));
				return;
			}

			m_IsWayland = Platform == GLFW_PLATFORM_WAYLAND;
			PF_CORE_INFO("Linux window lifecycle validation selected GLFW {0}", m_IsWayland ? "Wayland" : "X11");
			m_InjectIconifiedBaselineForTesting = IsEnabled("PULSEFORGE_TEST_GLFW_LIFECYCLE_FORCE_ICONIFIED_BASELINE");
			m_SuppressIconifyForTesting = IsEnabled("PULSEFORGE_TEST_GLFW_LIFECYCLE_SUPPRESS_ICONIFY");
			m_SuppressRestoreForTesting = IsEnabled("PULSEFORGE_TEST_GLFW_LIFECYCLE_SUPPRESS_RESTORE");
			m_InitialExtent = Window.GetFramebufferSize();
			if (m_InitialExtent.first == 0 || m_InitialExtent.second == 0)
			{
				Fail("The Vulkan window started with a zero framebuffer extent");
				return;
			}

			SetPhase(WindowLifecyclePhase::WaitingForBaseline);
		}

		void OnUpdate(PulseForge::Timestep) override
		{
			auto& App = PulseForge::Application::Get();
			const auto Extent = App.GetWindow().GetFramebufferSize();
			const bool HasExtent = Extent.first > 0 && Extent.second > 0;
			const bool TimedOut = Clock::now() - m_PhaseStartedAt > PhaseTimeout;

			switch (m_Phase)
			{
			case WindowLifecyclePhase::WaitingForBaseline:
			{
				const bool IsIconified =
					glfwGetWindowAttrib(m_NativeWindow, GLFW_ICONIFIED) != GLFW_FALSE || m_InjectIconifiedBaselineForTesting;
				if (IsIconified)
				{
					Fail(m_InjectIconifiedBaselineForTesting
							 ? "The injected negative baseline reports GLFW_ICONIFIED=true; expected GLFW_ICONIFIED=false before rendering"
							 : "The Linux window was already iconified before the baseline frame; expected GLFW_ICONIFIED=false");
				}
				else if (!HasExtent) Fail("The Linux window lost its usable framebuffer before the baseline frame");
				else
				{
					m_RenderCountAtBaseline = m_RenderCount;
					SetPhase(WindowLifecyclePhase::WaitingForBaselineFrame);
				}
				break;
			}

			case WindowLifecyclePhase::WaitingForBaselineFrame:
				if (m_RenderCount > m_RenderCountAtBaseline)
				{
					if (!HasExtent || glfwGetWindowAttrib(m_NativeWindow, GLFW_ICONIFIED) != GLFW_FALSE)
					{
						Fail("The baseline OnRender callback did not occur with an uniconified window and usable framebuffer");
						break;
					}
					glfwSetWindowSize(m_NativeWindow, 1024, 640);
					SetPhase(WindowLifecyclePhase::WaitingForResize);
				}
				else if (TimedOut) Fail("No OnRender callback followed the verified uniconified window with usable framebuffer");
				break;

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
					if (m_IsWayland)
					{
						m_WasSkipped = true;
						PF_CORE_WARN("Linux minimize validation skipped: GLFW's Wayland backend cannot observe minimized state; baseline, "
									 "resize, and "
									 "orderly shutdown were exercised");
						m_Phase = WindowLifecyclePhase::Complete;
						App.RequestClose();
						break;
					}

					if (m_SuppressIconifyForTesting)
						PF_CORE_WARN("Negative lifecycle test suppresses glfwIconifyWindow; the bounded X11 minimize phase must fail");
					else glfwIconifyWindow(m_NativeWindow);
					SetPhase(WindowLifecyclePhase::WaitingForMinimize);
				}
				else if (TimedOut) Fail("No OnRender callback followed the resized Vulkan framebuffer");
				break;

			case WindowLifecyclePhase::WaitingForMinimize:
			{
				const bool IsIconified = glfwGetWindowAttrib(m_NativeWindow, GLFW_ICONIFIED) == GLFW_TRUE;
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
					if (m_SuppressRestoreForTesting)
						PF_CORE_WARN(
							"Negative lifecycle test suppresses glfwRestoreWindow; the bounded restore-observation phase must fail");
					else glfwRestoreWindow(m_NativeWindow);
					SetPhase(WindowLifecyclePhase::WaitingForRestoreObservation);
				}
				else if (TimedOut)
				{
					FailMinimizeTimeout(Extent, IsIconified);
				}
				break;
			}

			case WindowLifecyclePhase::WaitingForRestoreObservation:
				if (HasExtent && glfwGetWindowAttrib(m_NativeWindow, GLFW_ICONIFIED) == GLFW_FALSE)
				{
					m_RenderCountAtRestoreObservation = m_RenderCount;
					PF_CORE_INFO("Linux window restore observed at framebuffer {0}x{1}; waiting for a later render callback", Extent.first,
						Extent.second);
					SetPhase(WindowLifecyclePhase::WaitingForRestoredFrame);
				}
				else if (TimedOut) Fail("The Linux window did not report a usable framebuffer with GLFW_ICONIFIED=false after restore");
				break;

			case WindowLifecyclePhase::WaitingForRestoredFrame:
				if (m_SawPostRestoreRender && m_RenderCount > m_RenderCountAtRestoreObservation)
				{
					PF_CORE_INFO("Active main-frame OnRender callback observed after restore; requesting orderly renderer shutdown. "
								 "This callback does not prove image presentation or pixel contents.");
					m_Phase = WindowLifecyclePhase::Complete;
					App.RequestClose();
				}
				else if (TimedOut) Fail("The Vulkan window did not invoke OnRender after positive restore observation");
				break;

			case WindowLifecyclePhase::Complete:
			case WindowLifecyclePhase::Failed:
				break;
			}

			if (m_Phase == WindowLifecyclePhase::Failed) App.RequestClose();
		}

		void OnRender() override
		{
			auto& App = PulseForge::Application::Get();
			if (!m_NativeWindow)
			{
				App.RequestClose();
				return;
			}
			const auto Extent = App.GetWindow().GetFramebufferSize();
			const bool HasExtent = Extent.first > 0 && Extent.second > 0;
			const bool IsIconified = glfwGetWindowAttrib(m_NativeWindow, GLFW_ICONIFIED) != GLFW_FALSE;
			if (m_Phase == WindowLifecyclePhase::WaitingForMinimize && !HasExtent)
			{
				Fail("The application invoked OnRender while its minimized framebuffer extent was zero");
				App.RequestClose();
				return;
			}
			if (m_Phase == WindowLifecyclePhase::WaitingForBaselineFrame && (!HasExtent || IsIconified || !App.IsMainFrameActive()))
			{
				Fail("The baseline OnRender callback requires an active main frame, GLFW_ICONIFIED=false, and a usable framebuffer");
				App.RequestClose();
				return;
			}
			if (m_Phase == WindowLifecyclePhase::WaitingForRestoredFrame)
			{
				if (!HasExtent || IsIconified || !App.IsMainFrameActive())
				{
					Fail(
						"The post-restore OnRender callback requires an active main frame, GLFW_ICONIFIED=false, and a usable framebuffer");
					App.RequestClose();
					return;
				}
				m_SawPostRestoreRender = true;
			}

			++m_RenderCount;
		}

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return m_Phase == WindowLifecyclePhase::Complete && !m_WasSkipped && m_SawMinimize && m_SawPostRestoreRender &&
				   m_RenderCount > m_RenderCountAtRestoreObservation;
		}

		[[nodiscard]] bool WasSkipped() const noexcept { return m_WasSkipped; }

	private:
		static bool IsEnabled(const char* Name)
		{
			const char* Value = std::getenv(Name);
			return Value && std::string(Value) == "1";
		}

		void FailMinimizeTimeout(std::pair<unsigned int, unsigned int> Extent, bool IsIconified)
		{
			const std::string State = "GLFW_ICONIFIED=" + std::string(IsIconified ? "true" : "false") +
									  ", framebuffer=" + std::to_string(Extent.first) + "x" + std::to_string(Extent.second);
			if (glfwGetPlatform() == GLFW_PLATFORM_X11)
				Fail(
					"X11 minimize was not observed within the phase timeout (" + State + "); expected iconification or a zero framebuffer");
			else Fail("Minimize was not observed within the phase timeout on a non-Wayland GLFW backend (" + State + ")");
		}

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

		GLFWwindow* m_NativeWindow = nullptr;
		WindowLifecyclePhase m_Phase = WindowLifecyclePhase::WaitingForBaseline;
		Clock::time_point m_PhaseStartedAt = Clock::now();
		std::pair<unsigned int, unsigned int> m_InitialExtent{};
		uint64_t m_RenderCount = 0;
		uint64_t m_RenderCountAtBaseline = 0;
		uint64_t m_RenderCountAtResize = 0;
		uint64_t m_RenderCountAtRestoreObservation = 0;
		Clock::time_point m_MinimizedAt{};
		bool m_SawMinimize = false;
		bool m_SawPostRestoreRender = false;
		bool m_WasSkipped = false;
		bool m_IsWayland = false;
		bool m_InjectIconifiedBaselineForTesting = false;
		bool m_SuppressIconifyForTesting = false;
		bool m_SuppressRestoreForTesting = false;
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
