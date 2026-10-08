#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace PulseForgeEditor
{
	enum class EditorIcon : uint8_t
	{
		NewDocument,
		Search,
		Scene,
		Settings,
		Refresh,
		Camera,
		AudioListener,
		AudioSource,
		Rename,
		Translate,
		Play,
		Stop,
		Info,
		Add,
		Remove,
		Warning,
		Entity,
		Folder,
		FolderOpen,
		Scale,
		Save,
		AddComponent,
		Terminal,
		File,
		Cube,
		Script,
		Delete,
		Duplicate,
		Rotate,
		Prefab,
		Clear,
		Close,
		Import,
		Count
	};

	struct EditorIconDescriptor
	{
		EditorIcon Icon;
		uint16_t Codepoint;
		std::string_view Name;
	};

	// Sorted by glyph codepoint to keep the generated sparse atlas ranges deterministic.
	inline constexpr std::array<EditorIconDescriptor, static_cast<std::size_t>(EditorIcon::Count)> EditorIconDescriptors = {{
		{ EditorIcon::NewDocument, 0xe494, "file-circle-plus" },
		{ EditorIcon::Search, 0xf002, "magnifying-glass" },
		{ EditorIcon::Scene, 0xf008, "film" },
		{ EditorIcon::Settings, 0xf013, "gear" },
		{ EditorIcon::Refresh, 0xf021, "arrows-rotate" },
		{ EditorIcon::AudioListener, 0xf025, "headphones" },
		{ EditorIcon::AudioSource, 0xf028, "volume-high" },
		{ EditorIcon::Camera, 0xf030, "camera" },
		{ EditorIcon::Rename, 0xf044, "pen-to-square" },
		{ EditorIcon::Translate, 0xf047, "arrows-up-down-left-right" },
		{ EditorIcon::Play, 0xf04b, "play" },
		{ EditorIcon::Stop, 0xf04d, "stop" },
		{ EditorIcon::Add, 0xf055, "circle-plus" },
		{ EditorIcon::Info, 0xf05a, "circle-info" },
		{ EditorIcon::Remove, 0xf068, "minus" },
		{ EditorIcon::Warning, 0xf071, "triangle-exclamation" },
		{ EditorIcon::Folder, 0xf07b, "folder" },
		{ EditorIcon::FolderOpen, 0xf07c, "folder-open" },
		{ EditorIcon::Scale, 0xf0b2, "up-down-left-right" },
		{ EditorIcon::Save, 0xf0c7, "floppy-disk" },
		{ EditorIcon::Entity, 0xf111, "circle" },
		{ EditorIcon::Terminal, 0xf120, "terminal" },
		{ EditorIcon::AddComponent, 0xf12e, "puzzle-piece" },
		{ EditorIcon::File, 0xf15c, "file-lines" },
		{ EditorIcon::Cube, 0xf1b2, "cube" },
		{ EditorIcon::Script, 0xf1c9, "file-code" },
		{ EditorIcon::Delete, 0xf1f8, "trash" },
		{ EditorIcon::Duplicate, 0xf24d, "clone" },
		{ EditorIcon::Rotate, 0xf2f1, "rotate" },
		{ EditorIcon::Prefab, 0xf466, "box" },
		{ EditorIcon::Clear, 0xf51a, "broom" },
		{ EditorIcon::Close, 0xf52b, "door-open" },
		{ EditorIcon::Import, 0xf56f, "file-import" }
	}};

	[[nodiscard]] constexpr const EditorIconDescriptor* FindEditorIcon(EditorIcon Icon) noexcept
	{
		for (const EditorIconDescriptor& Descriptor : EditorIconDescriptors)
			if (Descriptor.Icon == Icon)
				return &Descriptor;
		return nullptr;
	}

	inline constexpr std::array<uint32_t, EditorIconDescriptors.size() * 2 + 1> EditorIconGlyphRanges = []
	{
		std::array<uint32_t, EditorIconDescriptors.size() * 2 + 1> Ranges{};
		for (std::size_t Index = 0; Index < EditorIconDescriptors.size(); ++Index)
		{
			Ranges[Index * 2] = EditorIconDescriptors[Index].Codepoint;
			Ranges[Index * 2 + 1] = EditorIconDescriptors[Index].Codepoint;
		}
		return Ranges;
	}();
}
