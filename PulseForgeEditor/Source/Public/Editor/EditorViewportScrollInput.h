#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

struct GLFWwindow;
struct ImGuiContext;

namespace PulseForgeEditor::Detail
{
	class ViewportScrollEventAccumulator final
	{
	public:
		void Record(uint64_t Registration, float Delta);
		[[nodiscard]] float Consume(uint64_t Registration) noexcept;
		void Forget(uint64_t Registration) noexcept;
		void DiscardAll() noexcept { m_Pending.clear(); }

	private:
		std::unordered_map<uint64_t, float> m_Pending;
	};

	class EditorViewportScrollInput final
	{
	public:
		EditorViewportScrollInput();
		~EditorViewportScrollInput();
		EditorViewportScrollInput(const EditorViewportScrollInput&) = delete;
		EditorViewportScrollInput& operator=(const EditorViewportScrollInput&) = delete;

		[[nodiscard]] bool Install(ImGuiContext* Context, GLFWwindow* MainWindow);
		[[nodiscard]] float Consume(uint32_t ViewportID, GLFWwindow* Window) noexcept;
		[[nodiscard]] uint64_t GetInputWindowID(uint32_t ViewportID, GLFWwindow* Window) const noexcept;
		void DiscardPending() noexcept;
		void Shutdown() noexcept;

	private:
		struct State;
		std::unique_ptr<State> m_State;
	};
} // namespace PulseForgeEditor::Detail
