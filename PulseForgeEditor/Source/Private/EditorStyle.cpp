#include "Editor/EditorStyle.h"

#include "Core/Log.h"

#include <imgui.h>
#include <imgui_freetype.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
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
		bool HasIcons, PulseForgeEditor::EditorIcon Icon, std::string_view Label)
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

		const size_t CopyLength = std::min(Label.size(), Buffer.size() - Offset - 1);
		if (CopyLength > 0)
			std::memcpy(Buffer.data() + Offset, Label.data(), CopyLength);
		Buffer[Offset + CopyLength] = '\0';
		return Buffer;
	}

	ImFont* LoadFont(ImFontAtlas& Atlas, const std::filesystem::path& Path, float PixelSize)
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
		ImFont* Font = Atlas.AddFontFromFileTTF(Utf8Path.c_str(), PixelSize, &Config);
		if (!Font)
			PF_WARN("Could not load bundled editor font: {}", Utf8Path);
		return Font;
	}
}

namespace PulseForgeEditor
{
	std::expected<void, std::string> EditorStyle::Initialize()
	{
		ImGuiContext* Context = ImGui::GetCurrentContext();
		if (!Context)
			return std::unexpected("An ImGui context must be current before editor fonts are registered");

		ImGuiIO& IO = ImGui::GetIO();
		IO.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());

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
			: LoadFont(*IO.Fonts, FontPath(EditorFontRole::Interface), FontSize(EditorFontRole::Interface));
		if (!m_InterfaceFont)
		{
			ImFontConfig FallbackConfig;
			FallbackConfig.SizePixels = FontSize(EditorFontRole::Interface);
			m_InterfaceFont = IO.Fonts->AddFontDefault(&FallbackConfig);
		}
		if (!m_InterfaceFont)
			return std::unexpected("Could not create either the bundled or built-in Dear ImGui interface font");

		m_EmphasisFont = AssetDirectory.empty()
			? nullptr
			: LoadFont(*IO.Fonts, FontPath(EditorFontRole::Emphasis), FontSize(EditorFontRole::Emphasis));
		if (!m_EmphasisFont)
			m_EmphasisFont = m_InterfaceFont;

		m_MonospaceFont = AssetDirectory.empty()
			? nullptr
			: LoadFont(*IO.Fonts, FontPath(EditorFontRole::Monospace), FontSize(EditorFontRole::Monospace));
		if (!m_MonospaceFont)
			m_MonospaceFont = m_InterfaceFont;

		const std::filesystem::path IconPath = AssetDirectory / "Icons" / "Font Awesome 7 Free-Solid-900.otf";
		std::error_code IconPathError;
		if (std::filesystem::is_regular_file(IconPath, IconPathError) && !IconPathError)
		{
			ImFontConfig IconConfig;
			IconConfig.Flags |= ImFontFlags_NoLoadError;
			IconConfig.MergeMode = true;
			IconConfig.DstFont = m_InterfaceFont;
			IconConfig.PixelSnapH = true;
			static const std::array<ImWchar, EditorIconGlyphRanges.size()> IconRanges = BuildImGuiIconRanges();
			const std::string Utf8IconPath = PathToUtf8(IconPath);
			m_HasIcons = IO.Fonts->AddFontFromFileTTF(
				Utf8IconPath.c_str(), EditorIconFontPixelSize, &IconConfig, IconRanges.data()) != nullptr;
		}
		if (!m_HasIcons)
			PF_WARN("Font Awesome editor icons could not be loaded from {}. Text-only action labels will be used.",
				PathToUtf8(IconPath));

		IO.FontDefault = m_InterfaceFont;
		return {};
	}

	void EditorStyle::Reset() noexcept
	{
		m_InterfaceFont = nullptr;
		m_EmphasisFont = nullptr;
		m_MonospaceFont = nullptr;
		m_HasIcons = false;
	}

	bool EditorStyle::Button(EditorIcon Icon, std::string_view Label) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		return ImGui::Button(Text.data());
	}

	bool EditorStyle::SmallButton(EditorIcon Icon, std::string_view Label) const
	{
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		return ImGui::SmallButton(Text.data());
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

	bool EditorStyle::RadioButton(EditorIcon Icon, std::string_view Label, bool Active) const
	{
		if (Active && m_EmphasisFont)
			ImGui::PushFont(m_EmphasisFont);
		const auto Text = MakeLabel(m_HasIcons, Icon, Label);
		const bool Changed = ImGui::RadioButton(Text.data(), Active);
		if (Active && m_EmphasisFont)
			ImGui::PopFont();
		return Changed;
	}

	void EditorStyle::PushMonospaceFont() const
	{
		if (m_MonospaceFont)
			ImGui::PushFont(m_MonospaceFont);
	}

	void EditorStyle::PopFont() const
	{
		if (m_MonospaceFont)
			ImGui::PopFont();
	}
}
