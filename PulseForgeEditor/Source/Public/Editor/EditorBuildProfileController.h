#pragma once

#include "Assets/AssetExportService.h"
#include "Editor/EditorBuildProfilePanel.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	enum class EditorBuildProfileDialogKind : uint8_t
	{
		LoadProfile,
		SaveProfileAs,
		SelectTemplate
	};

	struct EditorBuildProfileDialogIntent final
	{
		EditorBuildProfileDialogKind Kind = EditorBuildProfileDialogKind::LoadProfile;
		std::filesystem::path InitialDirectory;
		std::string SuggestedFilename;
	};

	struct EditorBuildProfileProjectContext final
	{
		std::optional<std::filesystem::path> SavedProjectFile;
		std::optional<PulseForge::AssetID> ProjectStartupScene;
		std::optional<PulseForge::AssetID> CurrentSceneAsset;
		std::string DirtyReason;
	};

	struct EditorBuildProfilePanelState final
	{
		std::optional<PulseForge::ExportBuildProfile> Profile;
		std::filesystem::path ProfilePath;
		std::filesystem::path TemplateDescriptorPath;
		bool ProjectOpen = false;
		bool SavedContentReady = false;
		PulseForge::AssetExportSnapshot Export;
		std::string StatusMessage;
	};

	class EditorBuildProfileExportRunner
	{
	public:
		virtual ~EditorBuildProfileExportRunner() = default;
		[[nodiscard]] virtual std::expected<uint64_t, PulseForge::AssetExportError> Start(
			PulseForge::AssetExportRequest Request) = 0;
		[[nodiscard]] virtual PulseForge::AssetExportSnapshot GetSnapshot(uint64_t RequestID) const = 0;
		[[nodiscard]] virtual bool RequestCancel(uint64_t RequestID) noexcept = 0;
		virtual void Shutdown() noexcept = 0;
	};

	class EditorBuildProfileController final
	{
	public:
		EditorBuildProfileController();
		explicit EditorBuildProfileController(std::unique_ptr<EditorBuildProfileExportRunner> ExportRunner);
		EditorBuildProfileController(const EditorBuildProfileController&) = delete;
		EditorBuildProfileController& operator=(const EditorBuildProfileController&) = delete;

		[[nodiscard]] EditorBuildProfilePanelState GetPanelState(const EditorBuildProfileProjectContext& Context) const;
		void ApplyProfileEdit(std::optional<PulseForge::ExportBuildProfile> EditedProfile);
		[[nodiscard]] std::optional<EditorBuildProfileDialogIntent> HandleAction(
			const EditorBuildProfileAction& Action,
			const EditorBuildProfileProjectContext& Context);
		void ResolveDialogResult(const EditorBuildProfileDialogIntent& Intent,
			const std::optional<std::filesystem::path>& SelectedPath);
		void ProjectChanged(std::string_view Reason);
		void Shutdown(std::string_view Reason);

	private:
		void SaveProfile();
		void StartExport(const EditorBuildProfileProjectContext& Context);
		[[nodiscard]] bool CancelActiveExport(std::string_view Reason);

		std::unique_ptr<EditorBuildProfileExportRunner> m_ExportRunner;
		std::optional<PulseForge::ExportBuildProfile> m_Profile;
		std::filesystem::path m_ProfilePath;
		std::filesystem::path m_TemplateDescriptorPath;
		std::optional<uint64_t> m_ExportRequestID;
		std::string m_StatusMessage;
	};
} // namespace PulseForgeEditor
