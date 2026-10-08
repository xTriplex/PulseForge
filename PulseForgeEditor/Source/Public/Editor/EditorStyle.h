#pragma once

#include "Editor/EditorIcons.h"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

struct ImFont;

namespace PulseForgeEditor
{
	enum class EditorFontRole : uint8_t
	{
		Interface,
		Emphasis,
		Monospace
	};

	struct EditorFontDescriptor
	{
		EditorFontRole Role;
		std::string_view RelativePath;
		float PixelSize;
	};

	inline constexpr float EditorIconFontPixelSize = 15.0f;
	inline constexpr std::array<EditorFontDescriptor, 3> EditorFontDescriptors = {{
		{ EditorFontRole::Interface, "Fonts/Inter-Regular.ttf", 15.0f },
		{ EditorFontRole::Emphasis, "Fonts/Inter-SemiBold.ttf", 15.0f },
		{ EditorFontRole::Monospace, "Fonts/JetBrainsMono-Regular.ttf", 14.0f }
	}};

	// Loads editor fonts into the current ImGui atlas. The atlas owns the font data;
	// this object only keeps non-owning font pointers for editor UI helpers.
	class EditorStyle final
	{
	public:
		[[nodiscard]] std::expected<void, std::string> Initialize();
		void Reset() noexcept;

		[[nodiscard]] ImFont* GetInterfaceFont() const noexcept { return m_InterfaceFont; }
		[[nodiscard]] ImFont* GetEmphasisFont() const noexcept { return m_EmphasisFont; }
		[[nodiscard]] ImFont* GetMonospaceFont() const noexcept { return m_MonospaceFont; }
		[[nodiscard]] bool HasIcons() const noexcept { return m_HasIcons; }

		[[nodiscard]] bool Button(EditorIcon Icon, std::string_view Label) const;
		[[nodiscard]] bool SmallButton(EditorIcon Icon, std::string_view Label) const;
		[[nodiscard]] bool MenuItem(
			EditorIcon Icon,
			std::string_view Label,
			const char* Shortcut = nullptr,
			bool Selected = false,
			bool Enabled = true) const;
		[[nodiscard]] bool RadioButton(EditorIcon Icon, std::string_view Label, bool Active) const;

		void PushMonospaceFont() const;
		void PopFont() const;

	private:
		ImFont* m_InterfaceFont = nullptr;
		ImFont* m_EmphasisFont = nullptr;
		ImFont* m_MonospaceFont = nullptr;
		bool m_HasIcons = false;
	};
}
