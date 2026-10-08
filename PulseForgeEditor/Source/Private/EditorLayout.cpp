#include "Editor/EditorLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Objbase.h>
#include <ShlObj.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
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

	std::expected<std::filesystem::path, std::string> GetUserWorkspacePath(bool& IsOverridden)
	{
		if (const wchar_t* Override = _wgetenv(L"PULSEFORGE_EDITOR_SETTINGS_DIR"); Override && Override[0] != L'\0')
		{
			IsOverridden = true;
			std::filesystem::path Directory(Override);
			std::error_code Error;
			std::filesystem::create_directories(Directory, Error);
			if (Error)
				return std::unexpected("Could not create the requested editor workspace directory: " + Error.message());
			return Directory / L"imgui.ini";
		}

		PWSTR KnownFolder = nullptr;
		const HRESULT Result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &KnownFolder);
		if (FAILED(Result) || !KnownFolder)
		{
			if (KnownFolder)
				CoTaskMemFree(KnownFolder);
			return std::unexpected("Windows could not resolve the user's Local AppData directory");
		}

		std::filesystem::path Directory(KnownFolder);
		CoTaskMemFree(KnownFolder);
		Directory /= L"PulseForge";
		Directory /= L"Editor";

		std::error_code Error;
		std::filesystem::create_directories(Directory, Error);
		if (Error)
			return std::unexpected("Could not create the PulseForge editor settings directory: " + Error.message());

		return Directory / L"imgui.ini";
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

	void AppendPanelState(std::string& Contents, const PulseForgeEditor::EditorPanelVisibility& Panels)
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
		TemporaryPath += L".tmp";

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

		if (!MoveFileExW(TemporaryPath.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD ErrorCode = GetLastError();
			std::error_code RemoveError;
			std::filesystem::remove(TemporaryPath, RemoveError);
			return std::unexpected("Could not replace editor workspace settings (Windows error " +
				std::to_string(ErrorCode) + ")");
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

		bool IsSettingsPathOverridden = false;
		auto SettingsPath = GetUserWorkspacePath(IsSettingsPathOverridden);
		std::filesystem::path LegacyPath;
		std::error_code CurrentPathError;
		const std::filesystem::path CurrentDirectory = std::filesystem::current_path(CurrentPathError);
		if (!IsSettingsPathOverridden && !CurrentPathError)
			LegacyPath = CurrentDirectory / L"imgui.ini";

		std::filesystem::path SourcePath;
		if (SettingsPath)
		{
			m_SettingsPath = *SettingsPath;
			std::error_code ExistsError;
			if (std::filesystem::exists(*SettingsPath, ExistsError) && !ExistsError)
				SourcePath = *SettingsPath;
			else if (!LegacyPath.empty() && LegacyPath != *SettingsPath && std::filesystem::exists(LegacyPath, ExistsError) && !ExistsError)
			{
				SourcePath = LegacyPath;
				m_MigrateLegacySettings = true;
			}
		}
		else if (!LegacyPath.empty())
		{
			m_SettingsPath = LegacyPath;
			std::error_code ExistsError;
			if (std::filesystem::exists(LegacyPath, ExistsError) && !ExistsError)
				SourcePath = LegacyPath;
		}
		else
		{
			m_Initialized = true;
			if (IsSettingsPathOverridden)
				return std::unexpected(SettingsPath.error());
			return std::unexpected(SettingsPath.error() + "; the current directory is also unavailable");
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

	bool EditorLayout::SubmitDockspace()
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
			m_HasSavedPanelState = false;
			m_LayoutInitialized = false;
			m_ResetRequested = false;
			ResetApplied = true;
		}

		if (!m_LayoutInitialized)
		{
			if (!HasSavedWorkspace(DockspaceID))
			{
				m_Panels.ResetToDefaults();
				BuildDefaultDockspace(DockspaceID);
				ImGui::MarkIniSettingsDirty();
			}
			else if (m_LoadedDefaultLayoutVersion != CurrentDefaultEditorLayoutVersion)
				ImGui::MarkIniSettingsDirty();
			m_LayoutInitialized = true;
		}

		ImGui::DockSpaceOverViewport(DockspaceID, Viewport);
		return ResetApplied;
	}

	bool EditorLayout::HasSavedWorkspace(uint32_t DockspaceID) const
	{
		if (ImGui::DockBuilderGetNode(DockspaceID) || m_HasSavedPanelState)
			return true;

		for (const EditorPanelDescriptor& Descriptor : EditorPanelDescriptors)
		{
			const std::string WindowName(Descriptor.WindowName);
			if (ImGui::FindWindowSettingsByID(ImHashStr(WindowName.c_str())))
				return true;
		}
		return false;
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

		ImGuiID Bottom = 0;
		ImGuiID Main = 0;
		ImGui::DockBuilderSplitNode(DockspaceID, ImGuiDir_Down, 0.26f, &Bottom, &Main);

		ImGuiID Left = 0;
		ImGuiID CenterAndRight = 0;
		ImGui::DockBuilderSplitNode(Main, ImGuiDir_Left, 0.20f, &Left, &CenterAndRight);

		ImGuiID Right = 0;
		ImGuiID Center = 0;
		ImGui::DockBuilderSplitNode(CenterAndRight, ImGuiDir_Right, 0.23f, &Right, &Center);

		ImGui::DockBuilderDockWindow("Hierarchy", Left);
		ImGui::DockBuilderDockWindow("Inspector", Right);
		ImGui::DockBuilderDockWindow("Scene Viewport", Center);
		ImGui::DockBuilderDockWindow("Scene", Bottom);
		ImGui::DockBuilderDockWindow("Console", Bottom);
		ImGui::DockBuilderDockWindow("Content Browser", Bottom);
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
		AppendPanelState(Contents, m_Panels);
		return WriteAtomically(m_SettingsPath, Contents);
	}

	void EditorLayout::LoadPanelState(std::string_view IniContents)
	{
		m_Panels.ResetToDefaults();
		m_HasSavedPanelState = false;
		bool InWorkspaceSection = false;
		bool FormatWasValid = false;
		bool HasRecognizedState = false;

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
		m_HasSavedPanelState = HasRecognizedState;
	}
}
