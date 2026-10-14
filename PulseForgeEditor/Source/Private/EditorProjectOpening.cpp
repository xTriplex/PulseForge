#include "Editor/EditorProjectOpening.h"

#include "Assets/SceneAssetService.h"

#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
		}

		PreparedEditorProject MakePreparedProject(PulseForge::Project&& Project)
		{
			return { std::move(Project), {}, {}, {}, {}, false };
		}
	}

	std::expected<PreparedEditorProject, EditorProjectPreparationError> PrepareEditorProjectOpen(
		const std::filesystem::path& ProjectFile)
	{
		auto OpenedProject = PulseForge::Project::Open(ProjectFile);
		if (!OpenedProject)
			return std::unexpected(EditorProjectPreparationError{
				"Project could not be opened: " + OpenedProject.error().Message });

		PreparedEditorProject Result = MakePreparedProject(std::move(*OpenedProject));
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

	std::expected<PreparedEditorProject, EditorProjectPreparationError> PrepareEditorProjectCreation(
		const std::filesystem::path& ProjectFile)
	{
		const std::string ProjectName = PathToUtf8(ProjectFile.stem());
		auto CreatedProject = PulseForge::Project::Create(ProjectFile, ProjectName);
		if (!CreatedProject)
			return std::unexpected(EditorProjectPreparationError{
				"Project creation failed: " + CreatedProject.error().Message });

		PreparedEditorProject Result = MakePreparedProject(std::move(*CreatedProject));
		Result.StartupScene = std::make_unique<PulseForge::Scene>();
		const std::filesystem::path MainScenePath = std::filesystem::path("Assets") / "Main.scene";
		auto MainScene = PulseForge::SceneAssetService::Create(
			Result.Project.GetAssetRegistry(),
			Result.Project.GetRootPath(),
			MainScenePath,
			*Result.StartupScene);
		if (!MainScene)
		{
			if (MainScene.error().CommittedAsset)
			{
				Result.StartupSceneAsset = MainScene.error().CommittedAsset->ID;
				Result.SaveStartupSceneOnCommit = true;
				Result.StartupSceneWarning = "Project and Main.scene were created, but temporary cleanup needs attention: " +
					MainScene.error().Message;
				if (MainScene.error().RecoveryPath)
					Result.StartupSceneWarning += " Recovery data: " + PathToUtf8(*MainScene.error().RecoveryPath);
			}
			else
			{
				Result.StartupScene.reset();
				Result.StartupSceneError = "Project was created, but its initial scene could not be created: " +
					MainScene.error().Message;
			}
			return Result;
		}

		Result.StartupSceneAsset = MainScene->ID;
		Result.SaveStartupSceneOnCommit = true;
		return Result;
	}

	std::optional<std::string> CommitPreparedProjectSettings(PreparedEditorProject& Prepared)
	{
		if (!Prepared.SaveStartupSceneOnCommit || !Prepared.StartupSceneAsset)
			return std::nullopt;

		Prepared.SaveStartupSceneOnCommit = false;
		if (auto SetStartScene = Prepared.Project.SetStartScene(Prepared.StartupSceneAsset); !SetStartScene)
		{
			std::string Message = "Project and scene are open, but the startup scene could not be saved: " +
				SetStartScene.error().Message;
			if (!Prepared.StartupSceneWarning.empty())
				Message += " Scene creation also needs attention: " + Prepared.StartupSceneWarning;
			return Message;
		}
		return std::nullopt;
	}
}
