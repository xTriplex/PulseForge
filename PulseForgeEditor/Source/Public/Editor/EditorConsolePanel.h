#pragma once

#include <memory>

namespace PulseForgeEditor
{
	class EditorStyle;

	class EditorConsolePanel final
	{
	public:
		EditorConsolePanel();
		~EditorConsolePanel();
		EditorConsolePanel(const EditorConsolePanel&) = delete;
		EditorConsolePanel& operator=(const EditorConsolePanel&) = delete;

		void Attach();
		void Detach() noexcept;
		void CapturePendingLogs();
		void DrawContents(EditorStyle& Style);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
} // namespace PulseForgeEditor
