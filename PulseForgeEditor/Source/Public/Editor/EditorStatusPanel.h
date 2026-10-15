#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	class EditorStyle;

	enum class EditorStatusSeverity : uint8_t
	{
		Information,
		Warning,
		Error
	};

	enum class EditorStatusMessagePresentation : uint8_t
	{
		SingleLine,
		Wrapped
	};

	class EditorStatusPanel final
	{
	public:
		void SetStatus(std::string Message);
		void SetWarning(std::string Message);
		void SetError(std::string Message);

		[[nodiscard]] const std::string& Message() const noexcept { return m_Message; }
		[[nodiscard]] EditorStatusSeverity Severity() const noexcept { return m_Severity; }

		void DrawMessage(EditorStyle& Style, EditorStatusMessagePresentation Presentation) const;
		void DrawStatusBarContents(
			EditorStyle& Style,
			bool HasScene,
			bool SceneDirty,
			bool RuntimeActive,
			std::string_view RendererName) const;

	private:
		std::string m_Message = "Create or open a project to begin.";
		EditorStatusSeverity m_Severity = EditorStatusSeverity::Information;
	};
}
