#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	inline constexpr uint32_t CurrentDefaultEditorLayoutVersion = 4;

	enum class EditorPanel : uint8_t
	{
		SceneViewport,
		Hierarchy,
		Inspector,
		ContentBrowser,
		Console,
		Scene,
		MaterialDetails,
		Count
	};

	enum class EditorDefaultDockRegion : uint8_t
	{
		Left,
		Center,
		Right,
		Bottom
	};

	[[nodiscard]] constexpr EditorDefaultDockRegion GetDefaultDockRegion(EditorPanel Panel) noexcept
	{
		switch (Panel)
		{
			case EditorPanel::Hierarchy: return EditorDefaultDockRegion::Left;
			case EditorPanel::SceneViewport: return EditorDefaultDockRegion::Center;
			case EditorPanel::Inspector:
			case EditorPanel::MaterialDetails: return EditorDefaultDockRegion::Right;
			case EditorPanel::ContentBrowser:
			case EditorPanel::Console:
			case EditorPanel::Scene: return EditorDefaultDockRegion::Bottom;
			case EditorPanel::Count: return EditorDefaultDockRegion::Center;
		}
		return EditorDefaultDockRegion::Center;
	}

	struct EditorPanelDescriptor
	{
		EditorPanel Panel;
		std::string_view SettingsKey;
		std::string_view WindowName;
		bool IsUtilityPanel;
		bool DefaultVisible;
	};

	inline constexpr std::array<EditorPanelDescriptor, static_cast<size_t>(EditorPanel::Count)> EditorPanelDescriptors = {{
		{ EditorPanel::SceneViewport, "SceneViewport", "Scene Viewport", true, true },
		{ EditorPanel::Hierarchy, "Hierarchy", "Hierarchy", true, true },
		{ EditorPanel::Inspector, "Inspector", "Inspector", true, true },
		{ EditorPanel::ContentBrowser, "ContentBrowser", "Content Browser", true, true },
		{ EditorPanel::Console, "Console", "Console", true, true },
		{ EditorPanel::Scene, "Scene", "Scene", true, false },
		// Keep the settings key and ImGui window name stable for existing workspaces.
		{ EditorPanel::MaterialDetails, "MaterialEditor", "Material Editor", true, false }
	}};

	class EditorPanelVisibility final
	{
	public:
		constexpr EditorPanelVisibility() noexcept
		{
			ResetToDefaults();
		}

		[[nodiscard]] constexpr bool IsVisible(EditorPanel Panel) const noexcept
		{
			const size_t Index = static_cast<size_t>(Panel);
			return Index < m_Visibility.size() && m_Visibility[Index];
		}

		[[nodiscard]] constexpr bool* GetVisibility(EditorPanel Panel) noexcept
		{
			const size_t Index = static_cast<size_t>(Panel);
			return Index < m_Visibility.size() ? &m_Visibility[Index] : nullptr;
		}

		[[nodiscard]] constexpr bool SetVisible(EditorPanel Panel, bool Visible) noexcept
		{
			bool* Current = GetVisibility(Panel);
			if (!Current || *Current == Visible)
				return false;
			*Current = Visible;
			return true;
		}

		constexpr void ResetToDefaults() noexcept
		{
			for (const EditorPanelDescriptor& Descriptor : EditorPanelDescriptors)
				m_Visibility[static_cast<size_t>(Descriptor.Panel)] = Descriptor.DefaultVisible;
		}

	private:
		std::array<bool, static_cast<size_t>(EditorPanel::Count)> m_Visibility{};
	};

	// Owns editor-only ImGui docking and user workspace persistence. All methods that
	// interact with ImGui require this editor's context to be current.
	class EditorLayout final
	{
	public:
		[[nodiscard]] std::expected<void, std::string> Initialize();
		[[nodiscard]] bool SubmitDockspace();

		[[nodiscard]] bool IsPanelVisible(EditorPanel Panel) const noexcept;
		[[nodiscard]] bool* GetPanelVisibility(EditorPanel Panel) noexcept;
		[[nodiscard]] bool SetPanelVisible(EditorPanel Panel, bool Visible) noexcept;
		[[nodiscard]] uint32_t GetDocumentOpeningPreference() const noexcept { return m_DocumentOpeningPreference; }
		[[nodiscard]] std::filesystem::path GetDocumentWorkspacePath(std::string_view ProjectID) const;
		[[nodiscard]] bool SetDocumentOpeningPreference(uint32_t Preference) noexcept;
		[[nodiscard]] bool DockPanelBesideIfUnplaced(EditorPanel Panel, EditorPanel ReferencePanel);
		[[nodiscard]] bool DockPanelBeside(EditorPanel Panel, EditorPanel ReferencePanel);
		[[nodiscard]] bool ActivatePanel(EditorPanel Panel);
		void MarkSettingsDirty() noexcept;
		void RequestReset() noexcept;

		[[nodiscard]] std::expected<void, std::string> SaveIfRequested();
		[[nodiscard]] std::expected<void, std::string> SaveNow();

	private:
		[[nodiscard]] bool HasSavedWorkspace(uint32_t DockspaceID) const;
		void BuildDefaultDockspace(uint32_t DockspaceID);
		[[nodiscard]] std::expected<void, std::string> SaveSettings();
		void LoadPanelState(std::string_view IniContents);

		EditorPanelVisibility m_Panels;
		std::filesystem::path m_SettingsPath;
		uint32_t m_LoadedDefaultLayoutVersion = CurrentDefaultEditorLayoutVersion;
		uint32_t m_DocumentOpeningPreference = 0;
		bool m_Initialized = false;
		bool m_LayoutInitialized = false;
		bool m_HasSavedPanelState = false;
		bool m_ResetRequested = false;
		bool m_MigrateLegacySettings = false;
	};
}
