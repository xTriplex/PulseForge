#include "Editor/EditorBuildProfilePanel.h"

#include "Editor/EditorStyle.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace PulseForgeEditor
{
	namespace
	{
		constexpr std::array<const char*, 3> TargetPlatformNames = { "Windows", "Linux", "macOS" };
		constexpr std::array<const char*, 2> TargetArchitectureNames = { "x64", "Apple Silicon (arm64)" };
		constexpr std::array<const char*, 3> ConfigurationNames = { "Debug", "Release", "Dist" };

		const char* PhaseName(PulseForge::AssetExportPhase Phase) noexcept
		{
			using PulseForge::AssetExportPhase;
			switch (Phase)
			{
			case AssetExportPhase::Idle: return "Idle";
			case AssetExportPhase::Validate: return "Validate request";
			case AssetExportPhase::ResolveTemplate: return "Resolve player template";
			case AssetExportPhase::Cook: return "Cook content";
			case AssetExportPhase::Stage: return "Stage package";
			case AssetExportPhase::Verify: return "Verify package";
			case AssetExportPhase::Publish: return "Publish package";
			case AssetExportPhase::Complete: return "Complete";
			case AssetExportPhase::Failed: return "Failed";
			case AssetExportPhase::Cancelled: return "Cancelled";
			default: return "Unknown";
			}
		}

		template <size_t Size>
		bool CopyToBuffer(std::string_view Value, std::array<char, Size>& Buffer) noexcept
		{
			const size_t Count = (std::min)(Value.size(), Buffer.size() - 1);
			std::memcpy(Buffer.data(), Value.data(), Count);
			Buffer[Count] = '\0';
			return Count == Value.size();
		}

		std::string PathToUTF8(const std::filesystem::path& Path)
		{
			const std::u8string Value = Path.u8string();
			return { reinterpret_cast<const char*>(Value.data()), Value.size() };
		}
	} // namespace

	EditorBuildProfileDrawResult EditorBuildProfilePanel::DrawContents(
		const PulseForge::ExportBuildProfile* Profile,
		std::string_view ProfilePath,
		std::string_view TemplateDescriptorPath,
		bool ProjectOpen,
		bool SavedContentReady,
		const PulseForge::AssetExportSnapshot& Export,
		std::string_view StatusMessage,
		EditorStyle& Style)
	{
		EditorBuildProfileDrawResult Result;
		std::optional<PulseForge::ExportBuildProfile> WorkingProfile;
		if (Profile) WorkingProfile = *Profile;
		PulseForge::ExportBuildProfile* EditableProfile = WorkingProfile ? &*WorkingProfile : nullptr;
		if (ImGui::Button("New profile")) Result.Action.Kind = EditorBuildProfileActionKind::NewProfile;
		ImGui::SameLine();
		if (ImGui::Button("Load profile...")) Result.Action.Kind = EditorBuildProfileActionKind::LoadProfile;
		ImGui::SameLine();
		if (ImGui::Button("Save") && Profile)
			Result.Action.Kind = ProfilePath.empty() ? EditorBuildProfileActionKind::SaveProfileAs : EditorBuildProfileActionKind::SaveProfile;
		ImGui::SameLine();
		if (ImGui::Button("Save as...") && Profile) Result.Action.Kind = EditorBuildProfileActionKind::SaveProfileAs;

		if (!EditableProfile)
		{
			Style.TextMuted(ProjectOpen ? "Load or create a build profile to begin." : "Open a project to create or export a profile.");
		}
		else
		{
			ImGui::Separator();
			if (!ProfilePath.empty()) ImGui::TextWrapped("Profile: %.*s", static_cast<int>(ProfilePath.size()), ProfilePath.data());
			else Style.TextMuted("Unsaved profile");
			std::array<char, 257> NameBuffer{};
			CopyToBuffer(EditableProfile->DisplayName, NameBuffer);
			if (ImGui::InputText("Name", NameBuffer.data(), NameBuffer.size()))
			{
				EditableProfile->DisplayName = NameBuffer.data();
			}

			int PlatformIndex = static_cast<int>(EditableProfile->TargetPlatform);
			if (PlatformIndex < 0 || PlatformIndex >= static_cast<int>(TargetPlatformNames.size())) PlatformIndex = 0;
			if (ImGui::Combo("Target platform", &PlatformIndex, TargetPlatformNames.data(), static_cast<int>(TargetPlatformNames.size())))
			{
				EditableProfile->TargetPlatform = static_cast<PulseForge::ExportTargetPlatform>(PlatformIndex);
			}

			int ArchitectureIndex = static_cast<int>(EditableProfile->Architecture);
			if (ArchitectureIndex < 0 || ArchitectureIndex >= static_cast<int>(TargetArchitectureNames.size())) ArchitectureIndex = 0;
			if (ImGui::Combo("Architecture", &ArchitectureIndex, TargetArchitectureNames.data(),
					static_cast<int>(TargetArchitectureNames.size())))
			{
				EditableProfile->Architecture = static_cast<PulseForge::ExportTargetArchitecture>(ArchitectureIndex);
			}

			int ConfigurationIndex = static_cast<int>(EditableProfile->Configuration);
			if (ConfigurationIndex < 0 || ConfigurationIndex >= static_cast<int>(ConfigurationNames.size())) ConfigurationIndex = 1;
			if (ImGui::Combo("Configuration", &ConfigurationIndex, ConfigurationNames.data(),
					static_cast<int>(ConfigurationNames.size())))
			{
				EditableProfile->Configuration = static_cast<PulseForge::CookedBuildConfiguration>(ConfigurationIndex);
			}
			ImGui::Text("Renderer: Vulkan");

			if (!m_StartupSceneBufferInitialized || m_StartupSceneProfileID != EditableProfile->ProfileID ||
				m_StartupSceneValue != EditableProfile->StartupScene)
			{
				CopyToBuffer(EditableProfile->StartupScene.ToString(), m_StartupSceneBuffer);
				m_StartupSceneProfileID = EditableProfile->ProfileID;
				m_StartupSceneValue = EditableProfile->StartupScene;
				m_StartupSceneBufferInitialized = true;
			}
			if (ImGui::InputText("Startup scene UUID", m_StartupSceneBuffer.data(), m_StartupSceneBuffer.size()))
			{
				auto Parsed = PulseForge::UUID::Parse(m_StartupSceneBuffer.data());
				if (Parsed)
				{
					EditableProfile->StartupScene = *Parsed;
					m_StartupSceneValue = *Parsed;
				}
			}
			auto StartupSceneInput = PulseForge::UUID::Parse(m_StartupSceneBuffer.data());
			const bool StartupSceneInputValid = StartupSceneInput && !StartupSceneInput->IsNil();
			if (!StartupSceneInputValid) ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.32f, 1.0f), "Enter a valid non-empty scene UUID.");

			ImGui::InputText("Dynamic root UUID", m_DynamicRootBuffer.data(), m_DynamicRootBuffer.size());
			if (ImGui::Button("Add dynamic root"))
			{
				auto Parsed = PulseForge::UUID::Parse(m_DynamicRootBuffer.data());
				if (Parsed && !Parsed->IsNil() &&
					std::find(EditableProfile->DynamicRoots.begin(), EditableProfile->DynamicRoots.end(), *Parsed) ==
						EditableProfile->DynamicRoots.end())
				{
					EditableProfile->DynamicRoots.push_back(*Parsed);
					m_DynamicRootBuffer.fill('\0');
				}
			}
			for (size_t Index = 0; Index < EditableProfile->DynamicRoots.size(); ++Index)
			{
				const std::string Label = EditableProfile->DynamicRoots[Index].ToString() + "##dynamic-root-" + std::to_string(Index);
				ImGui::BulletText("%s", Label.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton(("Remove##dynamic-root-" + std::to_string(Index)).c_str()))
				{
					EditableProfile->DynamicRoots.erase(EditableProfile->DynamicRoots.begin() + static_cast<std::ptrdiff_t>(Index));
					break;
				}
			}

			std::array<char, 1024> OutputBuffer{};
			const std::string OutputText = PathToUTF8(EditableProfile->DefaultOutputPath);
			CopyToBuffer(OutputText, OutputBuffer);
			if (ImGui::InputText("Default output", OutputBuffer.data(), OutputBuffer.size()))
			{
				EditableProfile->DefaultOutputPath = std::filesystem::u8path(OutputBuffer.data());
			}

			if (!TemplateDescriptorPath.empty())
				ImGui::TextWrapped("Player template: %.*s", static_cast<int>(TemplateDescriptorPath.size()), TemplateDescriptorPath.data());
			else Style.TextMuted("No local player template selected.");
			if (ImGui::Button("Select player template...")) Result.Action.Kind = EditorBuildProfileActionKind::SelectTemplate;

			ImGui::Separator();
			if (!ProjectOpen) Style.TextMuted("Open a project before exporting.");
			else if (!SavedContentReady)
				ImGui::TextWrapped("Export reads saved project files only. Save or close dirty scenes, materials, and documents before starting.");

			const bool CanStart = ProjectOpen && SavedContentReady && StartupSceneInputValid && !Export.Running &&
				!TemplateDescriptorPath.empty();
			ImGui::BeginDisabled(!CanStart);
			if (ImGui::Button("Export")) Result.Action.Kind = EditorBuildProfileActionKind::StartExport;
			ImGui::EndDisabled();
			if (Export.Running)
			{
				ImGui::SameLine();
				ImGui::BeginDisabled(Export.CancellationRequested);
				if (ImGui::Button(Export.CancellationRequested ? "Cancelling..." : "Cancel export"))
					Result.Action.Kind = EditorBuildProfileActionKind::CancelExport;
				ImGui::EndDisabled();
			}
		}
		if (WorkingProfile && Profile && *WorkingProfile != *Profile) Result.UpdatedProfile = std::move(WorkingProfile);

		if (Export.Phase != PulseForge::AssetExportPhase::Idle)
		{
			ImGui::Separator();
			ImGui::Text("Status: %s", PhaseName(Export.Phase));
			if (Export.Running) ImGui::ProgressBar(std::clamp(Export.Progress, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
			if (!Export.PublishedOutput.empty())
			{
				const std::string Path = PathToUTF8(Export.PublishedOutput);
				ImGui::TextWrapped("Output: %s", Path.c_str());
			}
			if (Export.Error) ImGui::TextWrapped("%s", Export.Error->Message.c_str());
			if (!Export.Log.empty())
			{
				ImGui::TextUnformatted(Export.LogTruncated ? "Recent output (older log lines were dropped):" : "Output:");
				if (ImGui::BeginChild("ExportLog", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders))
					ImGui::TextUnformatted(Export.Log.data(), Export.Log.data() + Export.Log.size());
				ImGui::EndChild();
			}
		}
		if (!StatusMessage.empty())
		{
			ImGui::Separator();
			ImGui::TextWrapped("%.*s", static_cast<int>(StatusMessage.size()), StatusMessage.data());
		}
		return Result;
	}
} // namespace PulseForgeEditor
