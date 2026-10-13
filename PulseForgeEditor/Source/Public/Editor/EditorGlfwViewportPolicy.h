#pragma once

namespace PulseForgeEditor
{
	struct EditorGlfwViewportHandles
	{
		void* GlfwWindow = nullptr;
		void* NativeWindow = nullptr;
	};

	[[nodiscard]] constexpr void* GetGlfwContextWindow(const EditorGlfwViewportHandles& Handles) noexcept
	{
		return Handles.GlfwWindow;
	}
}
