#include "Editor/EditorStyle.h"

#include "Core/Log.h"

#include <imgui.h>
#include <imgui_freetype.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	constexpr size_t LabelBufferSize = 512;

	std::expected<std::filesystem::path, std::string> GetExecutableDirectory()
	{
		constexpr DWORD MaximumPathLength = 32768;
		std::vector<wchar_t> Buffer(512);
		while (Buffer.size() <= MaximumPathLength)
		{
			SetLastError(ERROR_SUCCESS);
			const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
			if (Length == 0)
				return std::unexpected("Windows could not locate the PulseForgeEditor executable");
			if (Length < Buffer.size() - 1)
				return std::filesystem::path(std::wstring_view(Buffer.data(), Length)).parent_path();
			Buffer.resize(std::min(Buffer.size() * 2, static_cast<size_t>(MaximumPathLength)));
		}
		return std::unexpected("The PulseForgeEditor executable path exceeds the Windows path limit");
	}

	std::string PathToUtf8(const std::filesystem::path& Path)
	{
		const std::u8string Encoded = Path.u8string();
		return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
	}

	std::array<ImWchar, PulseForgeEditor::EditorIconGlyphRanges.size()> BuildImGuiIconRanges()
	{
		std::array<ImWchar, PulseForgeEditor::EditorIconGlyphRanges.size()> Ranges{};
		for (size_t Index = 0; Index < Ranges.size(); ++Index)
			Ranges[Index] = static_cast<ImWchar>(PulseForgeEditor::EditorIconGlyphRanges[Index]);
		return Ranges;
	}

	std::array<ImWchar, PulseForgeEditor::EditorTextGlyphRanges.size()> BuildImGuiTextRanges()
	{
		std::array<ImWchar, PulseForgeEditor::EditorTextGlyphRanges.size()> Ranges{};
		for (size_t Index = 0; Index < Ranges.size(); ++Index)
			Ranges[Index] = static_cast<ImWchar>(PulseForgeEditor::EditorTextGlyphRanges[Index]);
		return Ranges;
	}

	void AppendCodepoint(std::array<char, LabelBufferSize>& Buffer, size_t& Offset, uint32_t Codepoint)
	{
		if (Codepoint <= 0x7f)
			Buffer[Offset++] = static_cast<char>(Codepoint);
		else if (Codepoint <= 0x7ff)
		{
			Buffer[Offset++] = static_cast<char>(0xc0 | (Codepoint >> 6));
			Buffer[Offset++] = static_cast<char>(0x80 | (Codepoint & 0x3f));
		}
		else if (Codepoint <= 0xffff)
		{
			Buffer[Offset++] = static_cast<char>(0xe0 | (Codepoint >> 12));
			Buffer[Offset++] = static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3f));
			Buffer[Offset++] = static_cast<char>(0x80 | (Codepoint & 0x3f));
		}
		else
		{
			Buffer[Offset++] = static_cast<char>(0xf0 | (Codepoint >> 18));
			Buffer[Offset++] = static_cast<char>(0x80 | ((Codepoint >> 12) & 0x3f));
			Buffer[Offset++] = static_cast<char>(0x80 | ((Codepoint >> 6) & 0x3f));
			Buffer[Offset++] = static_cast<char>(0x80 | (Codepoint & 0x3f));
		}
	}

	std::array<char, LabelBufferSize> MakeLabel(
		bool HasIcons,
		PulseForgeEditor::EditorIcon Icon,
		std::string_view Label,
		std::string_view StableID = {})
	{
		std::array<char, LabelBufferSize> Buffer{};
		size_t Offset = 0;
		if (HasIcons)
		{
			if (const PulseForgeEditor::EditorIconDescriptor* Descriptor = PulseForgeEditor::FindEditorIcon(Icon))
			{
				AppendCodepoint(Buffer, Offset, Descriptor->Codepoint);
				Buffer[Offset++] = ' ';
			}
		}

		const size_t MaximumIDLength = Buffer.size() - Offset - 4;
		const size_t IDLength = (std::min)(StableID.size(), MaximumIDLength);
		const size_t SuffixLength = StableID.empty() ? 0 : IDLength + 3;
		const size_t AvailableLabelLength = Buffer.size() - Offset - SuffixLength - 1;
		const size_t CopyLength = PulseForgeEditor::Utf8SafePrefixLength(Label, AvailableLabelLength);
		if (CopyLength > 0)
			std::memcpy(Buffer.data() + Offset, Label.data(), CopyLength);
		Offset += CopyLength;
		if (!StableID.empty())
		{
			Buffer[Offset++] = '#';
			Buffer[Offset++] = '#';
			Buffer[Offset++] = '#';
			std::memcpy(Buffer.data() + Offset, StableID.data(), IDLength);
			Offset += IDLength;
		}
		Buffer[Offset] = '\0';
		return Buffer;
	}

	ImFont* LoadFont(
		ImFontAtlas& Atlas,
		const std::filesystem::path& Path,
		float PixelSize,
		const ImWchar* GlyphRanges)
	{
		std::error_code Error;
		if (!std::filesystem::is_regular_file(Path, Error) || Error)
		{
			PF_WARN("Bundled editor font is unavailable: {}", PathToUtf8(Path));
			return nullptr;
		}

		ImFontConfig Config;
		Config.Flags |= ImFontFlags_NoLoadError;
		const std::string Utf8Path = PathToUtf8(Path);
		ImFont* Font = Atlas.AddFontFromFileTTF(Utf8Path.c_str(), PixelSize, &Config, GlyphRanges);
		if (!Font)
			PF_WARN("Could not load bundled editor font: {}", Utf8Path);
		return Font;
	}

	ImVec4 ToImVec4(PulseForgeEditor::EditorColorValue Color)
	{
		return { Color.Red, Color.Green, Color.Blue, Color.Alpha };
	}

	bool LoadIconFont(ImFontAtlas& Atlas, const std::filesystem::path& Path, ImFont* Target)
	{
		ImFontConfig Config;
		Config.Flags |= ImFontFlags_NoLoadError;
		Config.MergeMode = true;
		Config.DstFont = Target;
		Config.PixelSnapH = true;
		static const std::array<ImWchar, PulseForgeEditor::EditorIconGlyphRanges.size()> IconRanges =
			BuildImGuiIconRanges();
		const std::string Utf8Path = PathToUtf8(Path);
		return Atlas.AddFontFromFileTTF(Utf8Path.c_str(), PulseForgeEditor::EditorIconFontPixelSize,
			&Config, IconRanges.data()) != nullptr;
	}
}

namespace PulseForgeEditor
{
	EditorStyle::ScopedFont::ScopedFont(ImFont* Font)
	{
		if (Font)
		{
			ImGui::PushFont(Font);
			m_Pushed = true;
		}
	}

	EditorStyle::ScopedFont::~ScopedFont()
	{
		if (m_Pushed)
			ImGui::PopFont();
	}

	std::expected<void, std::string> EditorStyle::Initialize()
	{
		ImGuiContext* Context = ImGui::GetCurrentContext();
		if (!Context)
			return std::unexpected("An ImGui context must be current before editor fonts are registered");

		ImGuiIO& IO = ImGui::GetIO();
		IO.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
		static_assert(IsValidEditorTextGlyphRanges(), "Editor text glyph ranges must be sorted, disjoint, and terminated");
		static_assert(EditorTextGlyphRanges.back() <= (std::numeric_limits<ImWchar>::max)(),
			"Editor glyph range exceeds Dear ImGui's configured codepoint type");
		static const std::array<ImWchar, EditorTextGlyphRanges.size()> TextRanges = BuildImGuiTextRanges();

		std::filesystem::path AssetDirectory;
		if (auto ExecutableDirectory = GetExecutableDirectory(); ExecutableDirectory)
			AssetDirectory = *ExecutableDirectory / "EditorAssets";
		else
			PF_WARN("{}; using Dear ImGui's built-in editor font", ExecutableDirectory.error());

		const auto FontPath = [&AssetDirectory](EditorFontRole Role) -> std::filesystem::path
		{
			for (const EditorFontDescriptor& Descriptor : EditorFontDescriptors)
				if (Descriptor.Role == Role)
					return AssetDirectory / std::filesystem::path(Descriptor.RelativePath);
			return {};
		};

		const auto FontSize = [](EditorFontRole Role) -> float
		{
			for (const EditorFontDescriptor& Descriptor : EditorFontDescriptors)
				if (Descriptor.Role == Role)
					return Descriptor.PixelSize;
			return 0.0f;
		};

		m_InterfaceFont = AssetDirectory.empty()
			? nullptr
			: LoadFont(*IO.Fonts, FontPath(EditorFontRole::Interface), FontSize(EditorFontRole::Interface),
				TextRanges.data());
		if (!m_InterfaceFont)
		{
			ImFontConfig FallbackConfig;
			FallbackConfig.SizePixels = FontSize(EditorFontRole::Interface);
			FallbackConfig.GlyphRanges = TextRanges.data();
			m_InterfaceFont = IO.Fonts->AddFontDefault(&FallbackConfig);
		}
		if (!m_InterfaceFont)
			return std::unexpected("Could not create either the bundled or built-in Dear ImGui interface font");

		m_EmphasisFont = AssetDirectory.empty()
			? nullptr
			: LoadFont(*IO.Fonts, FontPath(EditorFontRole::Emphasis), FontSize(EditorFontRole::Emphasis),
				TextRanges.data());
		if (!m_EmphasisFont)
			m_EmphasisFont = m_InterfaceFont;

		m_MonospaceFont = AssetDirectory.empty()
			? nullptr
			: LoadFont(*IO.Fonts, FontPath(EditorFontRole::Monospace), FontSize(EditorFontRole::Monospace),
				TextRanges.data());
		if (!m_MonospaceFont)
			m_MonospaceFont = m_InterfaceFont;

		const std::filesystem::path IconPath = AssetDirectory / "Icons" / "Font Awesome 7 Free-Solid-900.otf";
		std::error_code IconPathError;
		if (std::filesystem::is_regular_file(IconPath, IconPathError) && !IconPathError)
		{
			const bool InterfaceIconsLoaded = LoadIconFont(*IO.Fonts, IconPath, m_InterfaceFont);
			const bool EmphasisIconsLoaded = m_EmphasisFont == m_InterfaceFont ||
				LoadIconFont(*IO.Fonts, IconPath, m_EmphasisFont);
			m_HasIcons = InterfaceIconsLoaded && EmphasisIconsLoaded;
		}
		if (!m_HasIcons)
			PF_WARN("Font Awesome editor icons could not be loaded from {}. Text-only action labels will be used.",
				PathToUtf8(IconPath));

		IO.FontDefault = m_InterfaceFont;
#ifdef PF_EDITOR_RENDERER_OPENGL
		constexpr bool RequiresStaticAtlasValidation = false;
		PF_INFO("Registered editor fonts for Dear ImGui's dynamic OpenGL texture atlas");
#else
		constexpr bool RequiresStaticAtlasValidation = true;
		unsigned char* AtlasPixels = nullptr;
		int AtlasWidth = 0;
		int AtlasHeight = 0;
		int AtlasBytesPerPixel = 0;
		IO.Fonts->GetTexDataAsRGBA32(&AtlasPixels, &AtlasWidth, &AtlasHeight, &AtlasBytesPerPixel);
		if (!AtlasPixels || AtlasWidth <= 0 || AtlasHeight <= 0 || AtlasBytesPerPixel != 4)
			return std::unexpected("Dear ImGui could not build a valid static editor font atlas");

		PF_INFO("Built editor font atlas at {}x{} (RGBA)", AtlasWidth, AtlasHeight);
#endif
		constexpr std::array<uint32_t, 15> RequiredTextGlyphs = {
			0x011e, 0x011f, 0x0130, 0x0131, 0x015e, 0x015f, 0x010c, 0x0141,
			0x017d, 0x0150, 0x00c5, 0x00e9, 0x00df, 0x20ac, 0x017e
		};
		const auto ValidateFontGlyphs = [](ImFont* Font, float Size, std::string_view Role,
			const auto& Codepoints)
		{
			if (!Font)
				return;
			ImFontBaked* Baked = nullptr;
			if constexpr (RequiresStaticAtlasValidation)
			{
				Baked = Font->GetFontBaked(Size);
				if (!Baked)
				{
					PF_WARN("Editor {} font did not produce baked glyph data", Role);
					return;
				}
			}
			for (const uint32_t Codepoint : Codepoints)
			{
				const ImWchar Glyph = static_cast<ImWchar>(Codepoint);
				if (!Font->IsGlyphInFont(Glyph) ||
					(RequiresStaticAtlasValidation && !Baked->IsGlyphLoaded(Glyph)))
				{
					if constexpr (RequiresStaticAtlasValidation)
						PF_WARN("Editor {} font atlas is missing required U+{:04X}", Role, Codepoint);
					else
						PF_WARN("Editor {} font source is missing required U+{:04X}", Role, Codepoint);
				}
			}
		};
		ValidateFontGlyphs(m_InterfaceFont, FontSize(EditorFontRole::Interface), "interface", RequiredTextGlyphs);
		ValidateFontGlyphs(m_EmphasisFont, FontSize(EditorFontRole::Emphasis), "emphasis", RequiredTextGlyphs);
		ValidateFontGlyphs(m_MonospaceFont, FontSize(EditorFontRole::Monospace), "monospace", RequiredTextGlyphs);
		if (m_HasIcons)
		{
			const auto ValidateIconGlyphs = [](ImFont* Font, float Size)
			{
				if (!Font)
					return false;
				ImFontBaked* Baked = nullptr;
				if constexpr (RequiresStaticAtlasValidation)
					Baked = Font->GetFontBaked(Size);
				if constexpr (RequiresStaticAtlasValidation)
					if (!Baked)
						return false;
				return std::all_of(EditorIconDescriptors.begin(), EditorIconDescriptors.end(),
					[Font, Baked](const EditorIconDescriptor& Descriptor)
					{
						const ImWchar Codepoint = static_cast<ImWchar>(Descriptor.Codepoint);
						return Font->IsGlyphInFont(Codepoint) &&
							(!RequiresStaticAtlasValidation || Baked->IsGlyphLoaded(Codepoint));
					});
			};
			if (!ValidateIconGlyphs(m_InterfaceFont, FontSize(EditorFontRole::Interface)) ||
				!ValidateIconGlyphs(m_EmphasisFont, FontSize(EditorFontRole::Emphasis)))
			{
				m_HasIcons = false;
				PF_WARN("Editor icon glyph coverage is incomplete; action labels will use text fallbacks");
			}
		}
		return {};
	}

	void EditorStyle::ApplyTheme(PulseForge::OutputColorEncoding Encoding)
	{
		if (m_ThemeApplied && m_OutputColorEncoding == Encoding)
			return;

		m_OutputColorEncoding = Encoding;
		m_ThemeApplied = true;
		ImGuiStyle& Style = ImGui::GetStyle();
		Style.WindowPadding = { 9.0f, 8.0f };
		Style.FramePadding = { 7.0f, 5.0f };
		Style.ItemSpacing = { 8.0f, 6.0f };
		Style.ItemInnerSpacing = { 6.0f, 4.0f };
		Style.IndentSpacing = 15.0f;
		Style.ScrollbarSize = 12.0f;
		Style.GrabMinSize = 10.0f;
		Style.WindowBorderSize = 1.0f;
		Style.ChildBorderSize = 1.0f;
		Style.PopupBorderSize = 1.0f;
		Style.FrameBorderSize = 0.0f;
		Style.TabBorderSize = 0.0f;
		Style.DockingSeparatorSize = 1.5f;
		Style.WindowRounding = 0.0f;
		Style.ChildRounding = 2.0f;
		Style.FrameRounding = 3.0f;
		Style.PopupRounding = 3.0f;
		Style.ScrollbarRounding = 5.0f;
		Style.GrabRounding = 3.0f;
		Style.TabRounding = 3.0f;

		Style.Colors[ImGuiCol_Text] = GetColor(EditorColorToken::Text);
		Style.Colors[ImGuiCol_TextDisabled] = GetColor(EditorColorToken::TextMuted);
		Style.Colors[ImGuiCol_WindowBg] = GetColor(EditorColorToken::Background);
		Style.Colors[ImGuiCol_ChildBg] = GetColor(EditorColorToken::Panel);
		Style.Colors[ImGuiCol_PopupBg] = GetColor(EditorColorToken::PanelRaised);
		Style.Colors[ImGuiCol_Border] = GetColor(EditorColorToken::Border);
		Style.Colors[ImGuiCol_BorderShadow] = GetColor(EditorColorToken::Transparent);
		Style.Colors[ImGuiCol_FrameBg] = GetColor(EditorColorToken::Input);
		Style.Colors[ImGuiCol_FrameBgHovered] = GetColor(EditorColorToken::PanelRaised);
		Style.Colors[ImGuiCol_FrameBgActive] = GetColor(EditorColorToken::Selection);
		Style.Colors[ImGuiCol_TitleBg] = GetColor(EditorColorToken::Panel);
		Style.Colors[ImGuiCol_TitleBgActive] = GetColor(EditorColorToken::PanelRaised);
		Style.Colors[ImGuiCol_TitleBgCollapsed] = GetColor(EditorColorToken::Panel);
		Style.Colors[ImGuiCol_MenuBarBg] = GetColor(EditorColorToken::MenuBar);
		Style.Colors[ImGuiCol_ScrollbarBg] = GetColor(EditorColorToken::Background);
		Style.Colors[ImGuiCol_ScrollbarGrab] = GetColor(EditorColorToken::BorderStrong);
		Style.Colors[ImGuiCol_ScrollbarGrabHovered] = GetColor(EditorColorToken::AccentHovered);
		Style.Colors[ImGuiCol_ScrollbarGrabActive] = GetColor(EditorColorToken::AccentActive);
		Style.Colors[ImGuiCol_CheckMark] = GetColor(EditorColorToken::AccentHovered);
		Style.Colors[ImGuiCol_SliderGrab] = GetColor(EditorColorToken::Accent);
		Style.Colors[ImGuiCol_SliderGrabActive] = GetColor(EditorColorToken::AccentHovered);
		Style.Colors[ImGuiCol_Button] = GetColor(EditorColorToken::PanelRaised);
		Style.Colors[ImGuiCol_ButtonHovered] = GetColor(EditorColorToken::BorderStrong);
		Style.Colors[ImGuiCol_ButtonActive] = GetColor(EditorColorToken::AccentActive);
		Style.Colors[ImGuiCol_Header] = GetColor(EditorColorToken::Selection);
		Style.Colors[ImGuiCol_HeaderHovered] = GetColor(EditorColorToken::AccentActive);
		Style.Colors[ImGuiCol_HeaderActive] = GetColor(EditorColorToken::Accent);
		Style.Colors[ImGuiCol_Separator] = GetColor(EditorColorToken::Border);
		Style.Colors[ImGuiCol_SeparatorHovered] = GetColor(EditorColorToken::AccentHovered);
		Style.Colors[ImGuiCol_SeparatorActive] = GetColor(EditorColorToken::Accent);
		ImVec4 AccentTint = GetColor(EditorColorToken::Accent);
		AccentTint.w = 0.22f;
		Style.Colors[ImGuiCol_ResizeGrip] = AccentTint;
		AccentTint = GetColor(EditorColorToken::AccentHovered);
		AccentTint.w = 0.65f;
		Style.Colors[ImGuiCol_ResizeGripHovered] = AccentTint;
		Style.Colors[ImGuiCol_ResizeGripActive] = GetColor(EditorColorToken::Accent);
		Style.Colors[ImGuiCol_InputTextCursor] = GetColor(EditorColorToken::AccentHovered);
		Style.Colors[ImGuiCol_TabHovered] = GetColor(EditorColorToken::AccentActive);
		Style.Colors[ImGuiCol_Tab] = GetColor(EditorColorToken::Panel);
		Style.Colors[ImGuiCol_TabSelected] = GetColor(EditorColorToken::PanelRaised);
		Style.Colors[ImGuiCol_TabSelectedOverline] = GetColor(EditorColorToken::Accent);
		Style.Colors[ImGuiCol_TabDimmed] = GetColor(EditorColorToken::Background);
		Style.Colors[ImGuiCol_TabDimmedSelected] = GetColor(EditorColorToken::Panel);
		Style.Colors[ImGuiCol_TabDimmedSelectedOverline] = GetColor(EditorColorToken::AccentActive);
		AccentTint = GetColor(EditorColorToken::Accent);
		AccentTint.w = 0.70f;
		Style.Colors[ImGuiCol_DockingPreview] = AccentTint;
		Style.Colors[ImGuiCol_DockingEmptyBg] = GetColor(EditorColorToken::Background);
		Style.Colors[ImGuiCol_TableHeaderBg] = GetColor(EditorColorToken::PanelRaised);
		Style.Colors[ImGuiCol_TableBorderStrong] = GetColor(EditorColorToken::BorderStrong);
		Style.Colors[ImGuiCol_TableBorderLight] = GetColor(EditorColorToken::Border);
		Style.Colors[ImGuiCol_TableRowBg] = GetColor(EditorColorToken::Panel);
		Style.Colors[ImGuiCol_TableRowBgAlt] = GetColor(EditorColorToken::PanelAlternate);
		Style.Colors[ImGuiCol_TextLink] = GetColor(EditorColorToken::AccentHovered);
		AccentTint = GetColor(EditorColorToken::AccentActive);
		AccentTint.w = 0.45f;
		Style.Colors[ImGuiCol_TextSelectedBg] = AccentTint;
		Style.Colors[ImGuiCol_TreeLines] = GetColor(EditorColorToken::BorderStrong);
		Style.Colors[ImGuiCol_DragDropTarget] = GetColor(EditorColorToken::AccentHovered);
		AccentTint = GetColor(EditorColorToken::Accent);
		AccentTint.w = 0.22f;
		Style.Colors[ImGuiCol_DragDropTargetBg] = AccentTint;
		Style.Colors[ImGuiCol_UnsavedMarker] = GetColor(EditorColorToken::Warning);
		Style.Colors[ImGuiCol_NavCursor] = GetColor(EditorColorToken::AccentHovered);
	}

	ImVec4 EditorStyle::GetColor(EditorColorToken Token) const noexcept
	{
		return ToImVec4(ConvertEditorColorForOutput(GetEditorColorValue(Token), m_OutputColorEncoding));
	}

	void EditorStyle::Reset() noexcept
	{
		m_InterfaceFont = nullptr;
		m_EmphasisFont = nullptr;
		m_MonospaceFont = nullptr;
		m_HasIcons = false;
		m_OutputColorEncoding = PulseForge::OutputColorEncoding::UnormAttachment;
		m_ThemeApplied = false;
	}

	bool EditorStyle::Button(EditorIcon Icon, std::string_view Label) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		return ImGui::Button(Text.data());
	}

	bool EditorStyle::FullWidthButton(EditorIcon Icon, std::string_view Label) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		return ImGui::Button(Text.data(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
	}

	bool EditorStyle::AccentButton(EditorIcon Icon, std::string_view Label) const
	{
		ImGui::PushStyleColor(ImGuiCol_Button, GetColor(EditorColorToken::AccentActive));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetColor(EditorColorToken::Accent));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetColor(EditorColorToken::AccentHovered));
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		const bool Pressed = ImGui::Button(Text.data());
		ImGui::PopStyleColor(3);
		return Pressed;
	}

	bool EditorStyle::DangerButton(EditorIcon Icon, std::string_view Label) const
	{
		ImVec4 Hovered = GetColor(EditorColorToken::Error);
		Hovered.w = 0.55f;
		ImVec4 Active = GetColor(EditorColorToken::Error);
		Active.w = 0.82f;
		ImGui::PushStyleColor(ImGuiCol_Button, GetColor(EditorColorToken::PanelRaised));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hovered);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, Active);
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		const bool Pressed = ImGui::Button(Text.data());
		ImGui::PopStyleColor(3);
		return Pressed;
	}

	bool EditorStyle::SmallButton(EditorIcon Icon, std::string_view Label) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		return ImGui::SmallButton(Text.data());
	}

	bool EditorStyle::ToolButton(EditorIcon Icon, std::string_view Label, bool Active) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		if (Active)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, GetColor(EditorColorToken::Selection));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetColor(EditorColorToken::AccentActive));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetColor(EditorColorToken::Accent));
			bool Pressed = false;
			{
				const ScopedFont FontScope(m_EmphasisFont);
				Pressed = ImGui::Button(Text.data());
			}
			ImGui::PopStyleColor(3);
			return Pressed;
		}
		return ImGui::Button(Text.data());
	}

	bool EditorStyle::ToolIconButton(
		EditorIcon Icon,
		std::string_view FallbackLabel,
		std::string_view StableID,
		std::string_view Tooltip,
		bool Active,
		bool Enabled,
		float ButtonSize) const
	{
		const bool UseIcon = m_HasIcons && FindEditorIcon(Icon);
		const auto Label = MakeLabel(UseIcon, Icon, UseIcon ? std::string_view{} : FallbackLabel, StableID);
		if (Active)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, GetColor(EditorColorToken::Selection));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetColor(EditorColorToken::AccentActive));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, GetColor(EditorColorToken::Accent));
		}
		ImGui::BeginDisabled(!Enabled);
		bool Pressed = false;
		{
			const ScopedFont FontScope(Active ? m_EmphasisFont : m_InterfaceFont);
			Pressed = ImGui::Button(Label.data(), UseIcon
				? ImVec2(ButtonSize, ButtonSize)
				: ImVec2(0.0f, ButtonSize));
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered() && !Tooltip.empty())
			ImGui::SetTooltip("%.*s", static_cast<int>(Tooltip.size()), Tooltip.data());
		if (Active)
			ImGui::PopStyleColor(3);
		return Pressed;
	}

	bool EditorStyle::AssetTile(
		EditorIcon Icon,
		std::string_view Label,
		bool Selected,
		const ImVec2& Size,
		int Flags) const
	{
		const bool Pressed = ImGui::Selectable("##AssetTile", Selected,
			static_cast<ImGuiSelectableFlags>(Flags), Size);
		const ImVec2 Minimum = ImGui::GetItemRectMin();
		const ImVec2 Maximum = ImGui::GetItemRectMax();
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(Minimum, Maximum, true);

		const EditorIconDescriptor* Descriptor = m_HasIcons ? FindEditorIcon(Icon) : nullptr;
		if (Descriptor)
		{
			const auto Glyph = MakeLabel(true, Icon, {});
			size_t GlyphBytes = 1;
			const unsigned char FirstByte = static_cast<unsigned char>(Glyph[0]);
			if ((FirstByte & 0xe0u) == 0xc0u)
				GlyphBytes = 2;
			else if ((FirstByte & 0xf0u) == 0xe0u)
				GlyphBytes = 3;
			else if ((FirstByte & 0xf8u) == 0xf0u)
				GlyphBytes = 4;
			const float GlyphFontSize = ImGui::GetFontSize() + 10.0f;
			const ImVec2 GlyphSize = ImGui::GetFont()->CalcTextSizeA(
				GlyphFontSize, (std::numeric_limits<float>::max)(), 0.0f, Glyph.data(), Glyph.data() + GlyphBytes);
			const ImVec2 GlyphPosition(
				Minimum.x + (Size.x - GlyphSize.x) * 0.5f,
				Minimum.y + 8.0f);
			DrawList->AddText(ImGui::GetFont(), GlyphFontSize, GlyphPosition,
				ImGui::GetColorU32(GetColor(EditorColorToken::AccentHovered)),
				Glyph.data(), Glyph.data() + GlyphBytes);
		}

		const float NameTop = Minimum.y + (Descriptor ? 39.0f : 11.0f);
		const float NameWidth = (std::max)(Size.x - 12.0f, 1.0f);
		const ImVec2 NameSize = ImGui::GetFont()->CalcTextSizeA(
			ImGui::GetFontSize(), NameWidth, 0.0f, Label.data(), Label.data() + Label.size());
		const float NameX = Minimum.x + (Size.x - NameSize.x) * 0.5f;
		DrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(NameX, NameTop),
			ImGui::GetColorU32(GetColor(EditorColorToken::Text)), Label.data(), Label.data() + Label.size(), NameWidth);
		DrawList->PopClipRect();

		const ImVec4 BorderColor = GetColor(Selected ? EditorColorToken::Accent : EditorColorToken::Border);
		DrawList->AddRect(Minimum, Maximum, ImGui::GetColorU32(BorderColor), 2.0f);
		return Pressed;
	}

	bool EditorStyle::TreeNode(
		EditorIcon Icon,
		std::string_view Label,
		std::string_view StableID,
		int Flags) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label, StableID);
		return ImGui::TreeNodeEx(Text.data(), static_cast<ImGuiTreeNodeFlags>(Flags));
	}

	bool EditorStyle::SectionHeader(EditorIcon Icon, std::string_view Label, bool DefaultOpen) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label, Label);
		const ImGuiTreeNodeFlags Flags = DefaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
		ImGui::PushStyleColor(ImGuiCol_Header, GetColor(EditorColorToken::PanelRaised));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, GetColor(EditorColorToken::BorderStrong));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, GetColor(EditorColorToken::Selection));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 6.0f));
		const bool Expanded = ImGui::CollapsingHeader(Text.data(), Flags);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		return Expanded;
	}

	bool EditorStyle::IconButton(
		EditorIcon Icon,
		std::string_view FallbackLabel,
		std::string_view StableID,
		std::string_view Tooltip,
		bool Destructive,
		float ButtonWidth,
		float ButtonHeight) const
	{
		const bool UseIcon = m_HasIcons && FindEditorIcon(Icon);
		const auto Text = MakeLabel(UseIcon, Icon, UseIcon ? std::string_view{} : FallbackLabel, StableID);
		if (Destructive)
		{
			ImVec4 Hovered = GetColor(EditorColorToken::Error);
			Hovered.w = 0.40f;
			ImVec4 Active = GetColor(EditorColorToken::Error);
			Active.w = 0.72f;
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hovered);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, Active);
		}
		const bool Pressed = UseIcon
			? ImGui::Button(Text.data(), ImVec2(ButtonWidth, ButtonHeight))
			: ImGui::Button(Text.data());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%.*s", static_cast<int>(Tooltip.size()), Tooltip.data());
		if (Destructive)
			ImGui::PopStyleColor(2);
		return Pressed;
	}

	bool EditorStyle::Selectable(EditorIcon Icon, std::string_view Label, bool Selected, int Flags) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		return ImGui::Selectable(Text.data(), Selected, static_cast<ImGuiSelectableFlags>(Flags));
	}

	bool EditorStyle::MenuItem(
		EditorIcon Icon,
		std::string_view Label,
		const char* Shortcut,
		bool Selected,
		bool Enabled) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		return ImGui::MenuItem(Text.data(), Shortcut, Selected, Enabled);
	}

	bool EditorStyle::BeginToolbar(const char* ID, float Height) const
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, GetColor(EditorColorToken::PanelRaised));
		ImGui::PushStyleColor(ImGuiCol_Border, GetColor(EditorColorToken::Border));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.0f);
		return ImGui::BeginChild(ID, ImVec2(0.0f, Height), ImGuiChildFlags_Borders,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
	}

	void EditorStyle::EndToolbar() const
	{
		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
	}

	bool EditorStyle::BeginPropertyTable(const char* ID) const
	{
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, GetColor(EditorColorToken::Border));
		const ImGuiTableFlags Flags = ImGuiTableFlags_SizingStretchProp |
			ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
		if (!ImGui::BeginTable(ID, 2, Flags))
		{
			ImGui::PopStyleColor();
			return false;
		}

		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const float LabelWidth = std::clamp(AvailableWidth * 0.34f, 66.0f, 104.0f);
		ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, LabelWidth);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		return true;
	}

	bool EditorStyle::BeginPropertyRow(std::string_view Label) const
	{
		ImGui::TableNextRow();
		if (!ImGui::TableSetColumnIndex(0))
			return false;
		TextMuted(Label);
		return ImGui::TableSetColumnIndex(1);
	}

	bool EditorStyle::Vector3PropertyRow(
		std::string_view Label,
		float Values[3],
		float Speed,
		const char* Format) const
	{
		if (!BeginPropertyRow(Label))
			return false;

		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		if (AvailableWidth < 190.0f)
			return ImGui::DragFloat3("##Vector3", Values, Speed, 0.0f, 0.0f, Format);

		bool Changed = false;
		ImGui::PushID(Label.data(), Label.data() + Label.size());
		if (ImGui::BeginTable("##Vector3Axes", 6,
			ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 12.0f);
			ImGui::TableSetupColumn("XValue", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 12.0f);
			ImGui::TableSetupColumn("YValue", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthFixed, 12.0f);
			ImGui::TableSetupColumn("ZValue", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableNextRow();
			constexpr std::array<EditorColorToken, 3> AxisColors = {
				EditorColorToken::GizmoAxisX,
				EditorColorToken::GizmoAxisY,
				EditorColorToken::GizmoAxisZ
			};
			constexpr std::array<const char*, 3> AxisNames = { "X", "Y", "Z" };
			for (int Axis = 0; Axis < 3; ++Axis)
			{
				ImGui::TableSetColumnIndex(Axis * 2);
				ImGui::TextColored(GetColor(AxisColors[static_cast<size_t>(Axis)]), "%s", AxisNames[Axis]);
				ImGui::TableSetColumnIndex(Axis * 2 + 1);
				ImGui::PushID(Axis);
				Changed |= ImGui::DragFloat("##AxisValue", &Values[Axis], Speed, 0.0f, 0.0f, Format);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::PopID();
		return Changed;
	}

	void EditorStyle::EndPropertyTable() const
	{
		ImGui::EndTable();
		ImGui::PopStyleColor();
	}

	void EditorStyle::BeginComponentBody(const char* ID) const
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, GetColor(EditorColorToken::Panel));
		ImGui::PushStyleColor(ImGuiCol_Border, GetColor(EditorColorToken::Border));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.0f);
		(void)ImGui::BeginChild(ID, ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
	}

	void EditorStyle::EndComponentBody() const
	{
		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
		ImGui::Spacing();
	}

	void EditorStyle::IconText(EditorIcon Icon, std::string_view Text) const
	{
		const auto Label = MakeLabel(m_HasIcons, Icon, Text);
		ImGui::TextUnformatted(Label.data());
	}

	void EditorStyle::EmptyState(EditorIcon Icon, std::string_view Text) const
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, GetColor(EditorColorToken::TextMuted));
		IconText(Icon, Text);
		ImGui::PopStyleColor();
	}

	void EditorStyle::TextMuted(std::string_view Text) const
	{
		if (Text.empty())
			return;
		ImGui::PushStyleColor(ImGuiCol_Text, GetColor(EditorColorToken::TextMuted));
		ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
		ImGui::PopStyleColor();
	}

	EditorStyle::ScopedFont EditorStyle::PushEmphasisFont() const
	{
		return ScopedFont(m_EmphasisFont);
	}

	EditorStyle::ScopedFont EditorStyle::PushMonospaceFont() const
	{
		return ScopedFont(m_MonospaceFont);
	}
}
