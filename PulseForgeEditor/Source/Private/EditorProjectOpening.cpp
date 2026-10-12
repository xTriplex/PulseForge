#include "Editor/EditorProjectOpening.h"

#include "Assets/SceneAssetService.h"

#include <utility>

namespace PulseForgeEditor
{
	std::expected<OpenedEditorProject, EditorProjectOpenError> OpenEditorProject(
		const std::filesystem::path& ProjectFile)
	{
		auto OpenedProject = PulseForge::Project::Open(ProjectFile);
		if (!OpenedProject)
			return std::unexpected(EditorProjectOpenError{
				"Project could not be opened: " + OpenedProject.error().Message });

		OpenedEditorProject Result{
			std::move(*OpenedProject),
			{},
			{},
			{} };
		if (const auto StartupScene = Result.Project.GetDescription().StartScene)
		{
			Result.StartupScene = std::make_unique<PulseForge::Scene>();
			const auto Loaded = PulseForge::SceneAssetService::Load(
				*StartupScene,
				Result.Project.GetRootPath(),
				Result.Project.GetAssetRegistry(),
				*Result.StartupScene);
			if (Loaded)
				Result.StartupSceneAsset = *StartupScene;
			else
			{
				Result.StartupSceneWarning =
					"Project opened, but its startup scene could not be loaded: " + Loaded.error().Message;
				Result.StartupScene.reset();
			}
		}

		return Result;
	}
}
