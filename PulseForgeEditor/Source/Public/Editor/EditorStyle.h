#pragma once

#include "Editor/EditorIcons.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

struct ImFont;
struct ImVec4;

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

	// This is the explicit static-atlas policy used by all bundled text fonts. Keep it broad enough for
	// common European project/entity names and diagnostics without loading CJK blocks into the legacy atlas.
	inline constexpr std::array<uint32_t, 23> EditorTextGlyphRanges = {
		0x0020, 0x007e, // Basic Latin
		0x00a0, 0x00ff, // Latin-1 Supplement
		0x0100, 0x017f, // Latin Extended-A
		0x0180, 0x024f, // Latin Extended-B
		0x0250, 0x02af, // IPA Extensions
		0x0300, 0x036f, // Combining Diacritical Marks
		0x0370, 0x03ff, // Greek and Coptic
		0x0400, 0x052f, // Cyrillic and Cyrillic Supplement
		0x1e00, 0x1eff, // Latin Extended Additional
		0x2000, 0x206f, // General Punctuation
		0x20a0, 0x20cf, // Currency Symbols
		0
	};

	struct EditorGlyphRange
	{
		uint32_t First;
		uint32_t Last;
	};

	[[nodiscard]] constexpr bool IsValidEditorTextGlyphRanges() noexcept
	{
		if (EditorTextGlyphRanges.back() != 0 || EditorTextGlyphRanges.size() % 2 == 0)
			return false;

		uint32_t PreviousLast = 0;
		for (size_t Index = 0; Index + 1 < EditorTextGlyphRanges.size() - 1; Index += 2)
		{
			const uint32_t First = EditorTextGlyphRanges[Index];
			const uint32_t Last = EditorTextGlyphRanges[Index + 1];
			if (First == 0 || First > Last || (Index != 0 && First <= PreviousLast))
				return false;
			PreviousLast = Last;
		}
		return true;
	}

	[[nodiscard]] constexpr bool EditorTextGlyphRangeContains(uint32_t Codepoint) noexcept
	{
		for (size_t Index = 0; Index + 1 < EditorTextGlyphRanges.size() - 1; Index += 2)
			if (Codepoint >= EditorTextGlyphRanges[Index] && Codepoint <= EditorTextGlyphRanges[Index + 1])
				return true;
		return false;
	}

	enum class EditorColorToken : uint8_t
	{
		Background,
		Panel,
		PanelRaised,
		MenuBar,
		Input,
		Border,
		BorderStrong,
		Text,
		TextMuted,
		Accent,
		AccentHovered,
		AccentActive,
		Selection,
		Warning,
		Error,
		Success,
		Count
	};

	struct EditorColorValue
	{
		float Red;
		float Green;
		float Blue;
		float Alpha;
	};

	inline constexpr std::array<EditorColorValue, static_cast<size_t>(EditorColorToken::Count)> EditorPalette = {{
		{ 0.075f, 0.086f, 0.102f, 1.0f }, // Background
		{ 0.106f, 0.122f, 0.145f, 1.0f }, // Panel
		{ 0.137f, 0.157f, 0.184f, 1.0f }, // PanelRaised
		{ 0.090f, 0.106f, 0.125f, 1.0f }, // MenuBar
		{ 0.071f, 0.086f, 0.106f, 1.0f }, // Input
		{ 0.205f, 0.235f, 0.275f, 1.0f }, // Border
		{ 0.285f, 0.325f, 0.375f, 1.0f }, // BorderStrong
		{ 0.875f, 0.898f, 0.925f, 1.0f }, // Text
		{ 0.590f, 0.635f, 0.690f, 1.0f }, // TextMuted
		{ 0.310f, 0.555f, 0.675f, 1.0f }, // Accent
		{ 0.380f, 0.635f, 0.750f, 1.0f }, // AccentHovered
		{ 0.245f, 0.455f, 0.565f, 1.0f }, // AccentActive
		{ 0.155f, 0.245f, 0.310f, 1.0f }, // Selection
		{ 0.820f, 0.650f, 0.350f, 1.0f }, // Warning
		{ 0.825f, 0.390f, 0.370f, 1.0f }, // Error
		{ 0.415f, 0.680f, 0.525f, 1.0f }  // Success
	}};

	[[nodiscard]] constexpr EditorColorValue GetEditorColorValue(EditorColorToken Token) noexcept
	{
		const std::size_t Index = static_cast<std::size_t>(Token);
		return Index < EditorPalette.size() ? EditorPalette[Index] : EditorPalette[0];
	}

	[[nodiscard]] constexpr bool EditorFontRoleHasIcons(EditorFontRole Role) noexcept
	{
		return Role == EditorFontRole::Interface || Role == EditorFontRole::Emphasis;
	}

	// Returns the longest byte prefix that ends at a UTF-8 code-point boundary. Label inputs are expected to be valid UTF-8.
	[[nodiscard]] constexpr std::size_t Utf8SafePrefixLength(
		std::string_view Text,
		std::size_t MaximumBytes) noexcept
	{
		if (Text.size() <= MaximumBytes)
			return Text.size();

		std::size_t Length = MaximumBytes;
		while (Length > 0 && (static_cast<uint8_t>(Text[Length]) & 0xc0u) == 0x80u)
			--Length;
		return Length;
	}

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
		void ApplyTheme() const;
		void Reset() noexcept;
		[[nodiscard]] ImVec4 GetColor(EditorColorToken Token) const noexcept;

		[[nodiscard]] ImFont* GetInterfaceFont() const noexcept { return m_InterfaceFont; }
		[[nodiscard]] ImFont* GetEmphasisFont() const noexcept { return m_EmphasisFont; }
		[[nodiscard]] ImFont* GetMonospaceFont() const noexcept { return m_MonospaceFont; }
		[[nodiscard]] bool HasIcons() const noexcept { return m_HasIcons; }

		[[nodiscard]] bool Button(EditorIcon Icon, std::string_view Label) const;
		[[nodiscard]] bool AccentButton(EditorIcon Icon, std::string_view Label) const;
		[[nodiscard]] bool SmallButton(EditorIcon Icon, std::string_view Label) const;
		[[nodiscard]] bool ToolButton(EditorIcon Icon, std::string_view Label, bool Active) const;
		[[nodiscard]] bool IconButton(
			EditorIcon Icon,
			std::string_view FallbackLabel,
			std::string_view StableID,
			std::string_view Tooltip) const;
		[[nodiscard]] bool Selectable(
			EditorIcon Icon,
			std::string_view Label,
			bool Selected,
			int Flags = 0) const;
		[[nodiscard]] bool MenuItem(
			EditorIcon Icon,
			std::string_view Label,
			const char* Shortcut = nullptr,
			bool Selected = false,
			bool Enabled = true) const;
		void TextMuted(std::string_view Text) const;
		void PushEmphasisFont() const;
		void PushMonospaceFont() const;
		void PopFont() const;

	private:
		ImFont* m_InterfaceFont = nullptr;
		ImFont* m_EmphasisFont = nullptr;
		ImFont* m_MonospaceFont = nullptr;
		bool m_HasIcons = false;
	};
}
