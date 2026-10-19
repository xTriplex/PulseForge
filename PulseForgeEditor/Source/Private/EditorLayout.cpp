#include "Editor/EditorLayout.h"
#include "Editor/EditorMaterialEditorPolicy.h"
#include "Editor/EditorLayoutSettingsPolicy.h"
#include "Editor/EditorPlatformFileSystem.h"
#include "Core/PlatformServices.h"

#include <imgui_internal.h>

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace
{
	constexpr std::string_view WorkspaceSection = "[PulseForgeWorkspace][State]";
	constexpr std::string_view WorkspaceSettingsFormatKey = "StateFormat";
	constexpr uint32_t WorkspaceSettingsFormat = 1;
	constexpr uintmax_t MaximumWorkspaceFileSize = 16 * 1024 * 1024;

	std::expected<std::filesystem::path, std::string> GetUserWorkspacePath()
	{
		auto Override = PulseForgeEditor::EditorPlatformFileSystem::GetSettingsDirectoryOverride();
		if (!Override) return std::unexpected(std::move(Override.error()));
		if (*Override)
		{
			std::filesystem::path Directory = std::move(**Override);
			if (!Directory.is_absolute()) return std::unexpected("The editor settings directory override must be an absolute path");
			std::error_code Error;
			std::filesystem::create_directories(Directory, Error);
			if (Error)
				return std::unexpected("Could not create the requested editor workspace directory: " + Error.message());
			return Directory / "imgui.ini";
		}

		auto UserDataDirectory = PulseForge::Platform::GetUserDataDirectory();
		if (!UserDataDirectory) return std::unexpected(UserDataDirectory.error().Message);

		std::filesystem::path Directory = std::move(*UserDataDirectory);
		if (!Directory.is_absolute()) return std::unexpected("The platform user data directory must be an absolute path");
		Directory /= "PulseForge";
		Directory /= "Editor";

		std::error_code Error;
		std::filesystem::create_directories(Directory, Error);
		if (Error)
			return std::unexpected("Could not create the PulseForge editor settings directory: " + Error.message());

		return Directory / "imgui.ini";
	}

	struct WorkspaceReadError
	{
		std::string Message;
	};

	std::expected<std::string, WorkspaceReadError> ReadFile(const std::filesystem::path& Path)
	{
		std::error_code Error;
		const uintmax_t FileSize = std::filesystem::file_size(Path, Error);
		if (Error)
			return std::unexpected(WorkspaceReadError{ "Could not inspect editor workspace settings: " + Error.message() });
		if (FileSize > MaximumWorkspaceFileSize)
			return std::unexpected(WorkspaceReadError{ "Editor workspace settings exceed the 16 MiB safety limit" });

		std::ifstream Input(Path, std::ios::binary);
		if (!Input)
			return std::unexpected(WorkspaceReadError{ "Could not open editor workspace settings for reading" });

		std::string Contents(static_cast<size_t>(FileSize), '\0');
		if (!Contents.empty())
			Input.read(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		if (!Input && !Input.eof())
			return std::unexpected(WorkspaceReadError{ "Could not read editor workspace settings" });
		return Contents;
	}

	bool ParseUnsigned(std::string_view Text, uint32_t& Value)
	{
		const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
		return Error == std::errc{} && End == Text.data() + Text.size();
	}

	bool ParseBoolean(std::string_view Text, bool& Value)
	{
		uint32_t Parsed = 0;
		if (!ParseUnsigned(Text, Parsed) || Parsed > 1)
			return false;
		Value = Parsed != 0;
		return true;
	}

	ImGuiID GetDockspaceID(const ImGuiViewport* Viewport)
	{
		// Keep the ID used by DockSpaceOverViewport(0, ...) so existing ImGui layouts
		// remain attached when the editor gains programmatic first-run docking.
		char HostWindowName[32]{};
		ImFormatString(HostWindowName, IM_ARRAYSIZE(HostWindowName), "WindowOverViewport_%08X", Viewport->ID);
		return ImHashStr("DockSpace", 0, ImHashStr(HostWindowName));
	}

	void AppendPanelState(
		std::string& Contents,
		const PulseForgeEditor::EditorPanelVisibility& Panels,
		uint32_t DocumentOpeningPreference)
	{
		if (!Contents.empty() && Contents.back() != '\n')
			Contents.push_back('\n');
		Contents.append(WorkspaceSection);
		Contents.push_back('\n');
		Contents.append(WorkspaceSettingsFormatKey);
		Contents.push_back('=');
		Contents.append(std::to_string(WorkspaceSettingsFormat));
		Contents.append("\nDefaultLayoutVersion=");
		Contents.append(std::to_string(PulseForgeEditor::CurrentDefaultEditorLayoutVersion));
		Contents.append("\nDocumentOpeningPreference=");
		Contents.append(std::to_string(DocumentOpeningPreference));
		Contents.push_back('\n');
		for (const PulseForgeEditor::EditorPanelDescriptor& Descriptor : PulseForgeEditor::EditorPanelDescriptors)
		{
			Contents.append(Descriptor.SettingsKey);
			Contents.push_back('=');
			Contents.push_back(Panels.IsVisible(Descriptor.Panel) ? '1' : '0');
			Contents.push_back('\n');
		}
	}

	std::expected<void, std::string> WriteAtomically(const std::filesystem::path& Path, std::string_view Contents)
	{
		std::filesystem::path TemporaryPath = Path;
		TemporaryPath += ".tmp";

		{
			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output)
				return std::unexpected("Could not create a temporary editor workspace settings file");

			Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			Output.flush();
			if (!Output)
			{
				Output.close();
				std::error_code RemoveError;
				std::filesystem::remove(TemporaryPath, RemoveError);
				return std::unexpected("Could not write editor workspace settings");
			}
		}

		if (auto Replaced = PulseForgeEditor::EditorPlatformFileSystem::ReplaceFileAtomically(TemporaryPath, Path); !Replaced)
		{
			std::error_code RemoveError;
			std::filesystem::remove(TemporaryPath, RemoveError);
			return std::unexpected("Could not replace editor workspace settings: " + Replaced.error());
		}
		return {};
	}
}

namespace PulseForgeEditor
{
	std::expected<void, std::string> EditorLayout::Initialize()
	{
		if (m_Initialized)
			return {};
		if (!ImGui::GetCurrentContext())
			return std::unexpected("An ImGui context must be current before initializing the editor layout");

		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;

		auto SettingsPath = GetUserWorkspacePath();
		if (!SettingsPath)
		{
			m_Initialized = true;
			return std::unexpected(std::move(SettingsPath.error()));
		}
		std::filesystem::path LegacyPath;
		std::error_code CurrentPathError;
		const std::filesystem::path CurrentDirectory = std::filesystem::current_path(CurrentPathError);
		if (!CurrentPathError) LegacyPath = CurrentDirectory / "imgui.ini";

		std::filesystem::path SourcePath;
		m_SettingsPath = *SettingsPath;
		std::error_code ExistsError;
		if (std::filesystem::exists(*SettingsPath, ExistsError) && !ExistsError) SourcePath = *SettingsPath;
		else if (!LegacyPath.empty() && LegacyPath != *SettingsPath && std::filesystem::exists(LegacyPath, ExistsError) && !ExistsError)
		{
			SourcePath = LegacyPath;
			m_MigrateLegacySettings = true;
		}

		m_Initialized = true;

		if (!SourcePath.empty())
		{
			auto Loaded = ReadFile(SourcePath);
			if (!Loaded)
			{
				m_SettingsPath.clear();
				return std::unexpected(Loaded.error().Message);
			}
			LoadPanelState(*Loaded);
			if (!Loaded->empty())
				ImGui::LoadIniSettingsFromMemory(Loaded->data(), Loaded->size());
			if (m_MigrateLegacySettings)
				ImGui::MarkIniSettingsDirty();
		}

		return {};
	}

	bool EditorLayout::SubmitDockspace(bool KeepAliveOnly)
	{
		if (!m_Initialized || !ImGui::GetCurrentContext())
			return false;

		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const ImGuiID DockspaceID = GetDockspaceID(Viewport);
		bool ResetApplied = false;

		if (m_ResetRequested)
		{
			ImGui::ClearIniSettings();
			m_Panels.ResetToDefaults();
			m_LayoutInitialized = false;
			m_ResetRequested = false;
			ResetApplied = true;
		}

		if (!m_LayoutInitialized)
		{
			if (ShouldBuildDefaultDockspace(HasRestorableDockspace(DockspaceID)))
			{
				BuildDefaultDockspace(DockspaceID);
				ImGui::MarkIniSettingsDirty();
			}
			else if (m_LoadedDefaultLayoutVersion != CurrentDefaultEditorLayoutVersion)
				ImGui::MarkIniSettingsDirty();
			m_LayoutInitialized = true;
		}

		ImGui::DockSpaceOverViewport(DockspaceID, Viewport,
			KeepAliveOnly ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_None);
		return ResetApplied;
	}

	bool EditorLayout::HasRestorableDockspace(uint32_t DockspaceID) const
	{
		// Panel visibility/preference metadata alone is not a restorable dock layout. A
		// truncated ini file can retain the state section while losing all Docking data.
		// In that case rebuild only the default dockspace and keep the parsed panel settings.
		return ImGui::DockBuilderGetNode(DockspaceID) != nullptr;
	}

	void EditorLayout::BuildDefaultDockspace(uint32_t DockspaceID)
	{
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		ImVec2 Size = Viewport->WorkSize;
		if (!std::isfinite(Size.x) || !std::isfinite(Size.y) || Size.x <= 1.0f || Size.y <= 1.0f)
			Size = ImGui::GetIO().DisplaySize;
		Size.x = std::max(Size.x, 1.0f);
		Size.y = std::max(Size.y, 1.0f);

		ImGui::DockBuilderRemoveNode(DockspaceID);
		ImGui::DockBuilderAddNode(DockspaceID, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodePos(DockspaceID, Viewport->WorkPos);
		ImGui::DockBuilderSetNodeSize(DockspaceID, Size);

		ImGuiID Inspector = 0;
		ImGuiID Workspace = 0;
		ImGui::DockBuilderSplitNode(DockspaceID, ImGuiDir_Right, 0.25f, &Inspector, &Workspace);

		ImGuiID Bottom = 0;
		ImGuiID Main = 0;
		ImGui::DockBuilderSplitNode(Workspace, ImGuiDir_Down, 0.30f, &Bottom, &Main);

		ImGuiID Hierarchy = 0;
		ImGuiID ViewportNode = 0;
		ImGui::DockBuilderSplitNode(Main, ImGuiDir_Left, 0.23f, &Hierarchy, &ViewportNode);

		const std::array<ImGuiID, 4> RegionNodes = { Hierarchy, ViewportNode, Inspector, Bottom };
		const auto DockPanel = [&RegionNodes](EditorPanel Panel, const char* WindowName)
		{
			const size_t Region = static_cast<size_t>(GetDefaultDockRegion(Panel));
			ImGui::DockBuilderDockWindow(WindowName, RegionNodes[Region]);
		};

		DockPanel(EditorPanel::Hierarchy, "Hierarchy");
		DockPanel(EditorPanel::Inspector, "Inspector");
		DockPanel(EditorPanel::SceneViewport, "Scene Viewport");
		DockPanel(EditorPanel::Scene, "Scene");
		DockPanel(EditorPanel::Console, "Console");
		DockPanel(EditorPanel::ContentBrowser, "Content Browser");
		DockPanel(EditorPanel::MaterialDetails, "Material Editor");
		ImGui::DockBuilderFinish(DockspaceID);
	}

	bool EditorLayout::IsPanelVisible(EditorPanel Panel) const noexcept
	{
		return m_Panels.IsVisible(Panel);
	}

	bool* EditorLayout::GetPanelVisibility(EditorPanel Panel) noexcept
	{
		return m_Panels.GetVisibility(Panel);
	}

	bool EditorLayout::SetPanelVisible(EditorPanel Panel, bool Visible) noexcept
	{
		if (!m_Panels.SetVisible(Panel, Visible))
			return false;
		MarkSettingsDirty();
		return true;
	}

	bool EditorLayout::SetDocumentOpeningPreference(uint32_t Preference) noexcept
	{
		if (Preference > 2 || m_DocumentOpeningPreference == Preference)
			return false;
		m_DocumentOpeningPreference = Preference;
		MarkSettingsDirty();
		return true;
	}

	std::filesystem::path EditorLayout::GetDocumentWorkspacePath(std::string_view ProjectID) const
	{
		if (m_SettingsPath.empty() || ProjectID.empty())
			return {};
		return m_SettingsPath.parent_path() / ("documents-" + std::string(ProjectID) + ".json");
	}

	bool EditorLayout::DockPanelBesideIfUnplaced(EditorPanel Panel, EditorPanel ReferencePanel)
	{
		if (!m_Initialized || !m_LayoutInitialized || !ImGui::GetCurrentContext() || Panel == ReferencePanel)
			return false;

		const size_t PanelIndex = static_cast<size_t>(Panel);
		const size_t ReferenceIndex = static_cast<size_t>(ReferencePanel);
		if (PanelIndex >= EditorPanelDescriptors.size() || ReferenceIndex >= EditorPanelDescriptors.size())
			return false;

		const std::string PanelName(EditorPanelDescriptors[PanelIndex].WindowName);
		const std::string ReferenceName(EditorPanelDescriptors[ReferenceIndex].WindowName);
		const bool HasPanelWindowSettings =
			ImGui::FindWindowSettingsByID(ImHashStr(PanelName.c_str())) != nullptr;
		const ImGuiWindowSettings* ReferenceSettings =
			ImGui::FindWindowSettingsByID(ImHashStr(ReferenceName.c_str()));
		ImGuiDockNode* ReferenceNode = ReferenceSettings && ReferenceSettings->DockId != 0
			? ImGui::DockBuilderGetNode(ReferenceSettings->DockId)
			: nullptr;
		if (!ReferenceSettings || !ReferenceNode || !ShouldApplyFirstOpenPanelDocking(HasPanelWindowSettings, true)) return false;

		ImGui::DockBuilderDockWindow(PanelName.c_str(), ReferenceSettings->DockId);
		const ImGuiDockNode* RootNode = ImGui::DockNodeGetRootNode(ReferenceNode);
		ImGui::DockBuilderFinish(RootNode->ID);
		MarkSettingsDirty();
		return true;
	}

	bool EditorLayout::DockPanelBeside(EditorPanel Panel, EditorPanel ReferencePanel)
	{
		if (!m_Initialized || !m_LayoutInitialized || !ImGui::GetCurrentContext() || Panel == ReferencePanel)
			return false;
		const size_t PanelIndex = static_cast<size_t>(Panel);
		const size_t ReferenceIndex = static_cast<size_t>(ReferencePanel);
		if (PanelIndex >= EditorPanelDescriptors.size() || ReferenceIndex >= EditorPanelDescriptors.size())
			return false;
		const std::string PanelName(EditorPanelDescriptors[PanelIndex].WindowName);
		const std::string ReferenceName(EditorPanelDescriptors[ReferenceIndex].WindowName);
		const ImGuiWindowSettings* ReferenceSettings =
			ImGui::FindWindowSettingsByID(ImHashStr(ReferenceName.c_str()));
		if (!ReferenceSettings || ReferenceSettings->DockId == 0 ||
			!ImGui::DockBuilderGetNode(ReferenceSettings->DockId))
			return false;
		ImGui::DockBuilderDockWindow(PanelName.c_str(), ReferenceSettings->DockId);
		ImGui::DockBuilderFinish(ImGui::DockNodeGetRootNode(
			ImGui::DockBuilderGetNode(ReferenceSettings->DockId))->ID);
		MarkSettingsDirty();
		return true;
	}

	bool EditorLayout::ActivatePanel(EditorPanel Panel)
	{
		const size_t PanelIndex = static_cast<size_t>(Panel);
		if (!m_Initialized || !ImGui::GetCurrentContext() || PanelIndex >= EditorPanelDescriptors.size())
			return false;
		const std::string WindowName(EditorPanelDescriptors[PanelIndex].WindowName);
		ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
		if (!Window)
			return false;
		if (ImGuiDockNode* Node = Window->DockNode; Node && Node->TabBar)
		{
			Node->SelectedTabId = Window->TabId;
			Node->TabBar->NextSelectedTabId = Window->TabId;
		}
		ImGui::FocusWindow(Window);
		MarkSettingsDirty();
		return true;
	}

	void EditorLayout::MarkSettingsDirty() noexcept
	{
		if (m_Initialized && ImGui::GetCurrentContext())
			ImGui::MarkIniSettingsDirty();
	}

	void EditorLayout::RequestReset() noexcept
	{
		m_ResetRequested = true;
	}

	std::expected<void, std::string> EditorLayout::SaveIfRequested()
	{
		if (!m_Initialized || !ImGui::GetCurrentContext() || !ImGui::GetIO().WantSaveIniSettings)
			return {};

		auto Result = SaveSettings();
		ImGui::GetIO().WantSaveIniSettings = false;
		return Result;
	}

	std::expected<void, std::string> EditorLayout::SaveNow()
	{
		if (!m_Initialized || !ImGui::GetCurrentContext())
			return {};
		return SaveSettings();
	}

	std::expected<void, std::string> EditorLayout::SaveSettings()
	{
		if (m_SettingsPath.empty())
			return std::unexpected("The editor workspace settings path is unavailable");

		size_t IniSize = 0;
		const char* IniData = ImGui::SaveIniSettingsToMemory(&IniSize);
		(void)IniSize;
		std::string Contents(IniData ? IniData : "");
		AppendPanelState(Contents, m_Panels, m_DocumentOpeningPreference);
		return WriteAtomically(m_SettingsPath, Contents);
	}

	void EditorLayout::LoadPanelState(std::string_view IniContents)
	{
		m_Panels.ResetToDefaults();
		bool InWorkspaceSection = false;
		bool FormatWasValid = false;
		bool HasRecognizedState = false;
		std::optional<uint32_t> LoadedDocumentOpeningPreference;

		std::istringstream Input{ std::string(IniContents) };
		std::string Line;
		while (std::getline(Input, Line))
		{
			if (!Line.empty() && Line.back() == '\r')
				Line.pop_back();
			if (!Line.empty() && Line.front() == '[')
			{
				InWorkspaceSection = Line == WorkspaceSection;
				if (InWorkspaceSection)
					HasRecognizedState = true;
				continue;
			}
			if (!InWorkspaceSection)
				continue;

			const size_t Equals = Line.find('=');
			if (Equals == std::string::npos)
				continue;
			const std::string_view Key(Line.data(), Equals);
			const std::string_view Value(Line.data() + Equals + 1, Line.size() - Equals - 1);
			if (Key == WorkspaceSettingsFormatKey)
			{
				uint32_t ParsedFormat = 0;
				FormatWasValid = ParseUnsigned(Value, ParsedFormat) && ParsedFormat == WorkspaceSettingsFormat;
				continue;
			}
			if (Key == "DefaultLayoutVersion")
			{
				(void)ParseUnsigned(Value, m_LoadedDefaultLayoutVersion);
				continue;
			}
			if (Key == "DocumentOpeningPreference")
			{
				LoadedDocumentOpeningPreference = ParseDocumentOpeningPreference(Value);
				continue;
			}
			for (const EditorPanelDescriptor& Descriptor : EditorPanelDescriptors)
			{
				if (Key != Descriptor.SettingsKey)
					continue;
				bool Visible = Descriptor.DefaultVisible;
				if (ParseBoolean(Value, Visible))
					(void)m_Panels.SetVisible(Descriptor.Panel, Visible);
				break;
			}
		}

		if (HasRecognizedState && !FormatWasValid)
			m_Panels.ResetToDefaults();
		m_DocumentOpeningPreference = ResolveDocumentOpeningPreference(
			HasRecognizedState, FormatWasValid, LoadedDocumentOpeningPreference);
	}
}
