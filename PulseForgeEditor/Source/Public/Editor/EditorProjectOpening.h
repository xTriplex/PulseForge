#pragma once

#include "Assets/Project.h"
#include "Scene/Scene.h"

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace PulseForgeEditor
{
	struct PreparedEditorProject
	{
		PulseForge::Project Project;
		std::unique_ptr<PulseForge::Scene> StartupScene;
		std::optional<PulseForge::AssetID> StartupSceneAsset;
		std::string StartupSceneWarning;
		std::string StartupSceneError;
		bool SaveStartupSceneOnCommit = false;
	};

	struct EditorProjectPreparationError
	{
		std::string Message;
	};

	[[nodiscard]] std::expected<PreparedEditorProject, EditorProjectPreparationError> PrepareEditorProjectOpen(
		const std::filesystem::path& ProjectFile, PulseForge::DiagnosticRecorder* Diagnostics = nullptr);
	[[nodiscard]] std::expected<PreparedEditorProject, EditorProjectPreparationError> PrepareEditorProjectCreation(
		const std::filesystem::path& ProjectFile);
	[[nodiscard]] std::optional<std::string> CommitPreparedProjectSettings(PreparedEditorProject& Prepared);
}
