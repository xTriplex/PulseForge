#pragma once

#include "Assets/Project.h"
#include "Scene/Scene.h"

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace PulseForgeEditor
{
	struct OpenedEditorProject
	{
		PulseForge::Project Project;
		std::unique_ptr<PulseForge::Scene> StartupScene;
		std::optional<PulseForge::AssetID> StartupSceneAsset;
		std::string StartupSceneWarning;
	};

	struct EditorProjectOpenError
	{
		std::string Message;
	};

	[[nodiscard]] std::expected<OpenedEditorProject, EditorProjectOpenError> OpenEditorProject(
		const std::filesystem::path& ProjectFile);
}
