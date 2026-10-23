#include "TestSupport.h"

#include "Assets/AssetTypes.h"
#include "Assets/AssetOperations.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Runtime/PlayerSession.h"
#include "Scene/Components/ScriptComponent.h"
#include "Scene/Scene.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace
{
	std::string ReadFile(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		return { std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
	}

	void TestPlayerSessionLifecycleAndTransitions(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto Identifier = UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return;

		std::error_code Error;
		const auto Root = std::filesystem::temp_directory_path(Error) / ("PulseForge Player Session " + Identifier->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return;
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Path, CleanupError);
			}
		} Cleanup{ Root };

		const auto ProjectFile = Root / "Player Project.pfproj";
		auto CreatedProjectResult = Project::Create(ProjectFile, "Player Project");
		PF_CHECK(Tests, CreatedProjectResult.has_value());
		if (!CreatedProjectResult) return;
		std::optional<Project> CreatedProject(std::move(*CreatedProjectResult));
		std::filesystem::create_directories(Root / "Assets" / "Scripts", Error);
		PF_CHECK(Tests, !Error);
		const std::string TimeScaleScript = "function OnUpdate(dt) assert(entity:set_translation(dt, 0, 0)) end\n";
		const auto ScriptAsset = AssetOperations::CreateAssetFromBytes(CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(),
			std::as_bytes(std::span(TimeScaleScript.data(), TimeScaleScript.size())), "Assets/Scripts/time_scale.lua");
		PF_CHECK(Tests, ScriptAsset.has_value());
		if (!ScriptAsset) return;

		Scene StartupScene;
		auto StartupEntity = StartupScene.CreateEntity("Startup Entity");
		PF_CHECK(Tests, StartupEntity.has_value());
		if (!StartupEntity) return;
		PF_CHECK(Tests, StartupEntity->SetScript(ScriptComponent{ ScriptAsset->ID }).has_value());
		auto StartupAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(), "Assets/Startup.scene", StartupScene);
		PF_CHECK(Tests, StartupAsset.has_value());
		if (!StartupAsset) return;
		PF_CHECK(Tests, CreatedProject->SetStartScene(StartupAsset->ID).has_value());

		Scene SecondScene;
		PF_CHECK(Tests, SecondScene.CreateEntity("Second Entity").has_value());
		auto SecondAsset = SceneAssetService::Create(
			CreatedProject->GetAssetRegistry(), CreatedProject->GetRootPath(), "Assets/Second.scene", SecondScene);
		PF_CHECK(Tests, SecondAsset.has_value());
		if (!SecondAsset) return;
		CreatedProject.reset();

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		PlayerSession Session(ProjectFile, RuntimeDescription);
		PF_CHECK(Tests, !Session.IsRunning());
		const auto TransitionBeforeStart = Session.RequestSceneTransition(SecondAsset->ID);
		PF_CHECK(Tests, !TransitionBeforeStart && TransitionBeforeStart.error().Code == PlayerSessionErrorCode::NotStarted);
		PF_CHECK(Tests, Session.Start().has_value());
		PF_CHECK(Tests, Session.IsRunning());
		PF_CHECK(Tests, Session.GetTimeScale() == 1.0);
		const auto ZeroTimeScale = Session.SetTimeScale(0.0);
		PF_CHECK(Tests, !ZeroTimeScale && ZeroTimeScale.error().Code == PlayerSessionErrorCode::InvalidTimeScale);
		PF_CHECK(Tests, Session.SetTimeScale(0.5).has_value());
		PF_CHECK(Tests, Session.GetTimeScale() == 0.5);
		PF_CHECK(Tests, Session.Advance(Timestep(0.2)).has_value());
		const auto ActiveStartupEntity = Session.GetActiveScene()->FindEntity(StartupEntity->GetUUID());
		PF_CHECK(Tests, ActiveStartupEntity.has_value());
		if (!ActiveStartupEntity) return;
		const auto ScaledTransform = ActiveStartupEntity->GetTransform();
		PF_CHECK(Tests, ScaledTransform && std::abs(ScaledTransform->Translation.x - 0.1f) < 0.0001f);
		PF_CHECK(Tests, Session.SetTimeScale(1.0).has_value());
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == StartupAsset->ID);
		PF_CHECK(Tests, Session.GetActiveScene() && Session.GetActiveScene()->GetEntityCount() == 1);
		const auto DuplicateStart = Session.Start();
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == PlayerSessionErrorCode::AlreadyStarted);

		const Scene* OriginalScene = Session.GetActiveScene();
		const auto NilTransition = Session.RequestSceneTransition(UUID{ 0, 0 });
		PF_CHECK(Tests, !NilTransition && NilTransition.error().Code == PlayerSessionErrorCode::InvalidSceneAsset);
		const auto MissingTransition = Session.RequestSceneTransition(UUID{ 44, 55 });
		PF_CHECK(Tests, !MissingTransition && MissingTransition.error().Code == PlayerSessionErrorCode::SceneLoadFailed);
		PF_CHECK(Tests, Session.GetActiveScene() == OriginalScene);
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == StartupAsset->ID);

		PF_CHECK(Tests, Session.RequestSceneTransition(SecondAsset->ID).has_value());
		PF_CHECK(Tests, Session.GetActiveScene() == OriginalScene);
		PF_CHECK(Tests, Session.Pause().has_value());
		PF_CHECK(Tests, Session.IsPaused());
		const auto PausedAdvance = Session.Advance(Timestep(0.1));
		PF_CHECK(Tests, PausedAdvance && PausedAdvance->PhysicsSteps == 0);
		PF_CHECK(Tests, Session.GetActiveSceneAsset() == SecondAsset->ID);
		PF_CHECK(Tests, Session.GetActiveScene() != OriginalScene);
		PF_CHECK(Tests, Session.GetActiveScene() && Session.GetActiveScene()->GetEntityCount() == 1);
		PF_CHECK(Tests, Session.Resume().has_value());
		PF_CHECK(Tests, !Session.IsPaused());
		PF_CHECK(Tests, Session.Advance(Timestep(1.0 / 60.0)).has_value());

		const std::string StartupBytes = ReadFile(Root / "Assets/Startup.scene");
		PF_CHECK(Tests, !StartupBytes.empty());
		PF_CHECK(Tests, Session.GetActiveScene()->CreateEntity("Runtime Only Mutation").has_value());
		Session.Stop();
		PF_CHECK(Tests, !Session.IsRunning());
		PF_CHECK(Tests, !Session.GetActiveScene());
		PF_CHECK(Tests, !Session.GetActiveSceneAsset());
		PF_CHECK(Tests, ReadFile(Root / "Assets/Startup.scene") == StartupBytes);
		const auto AdvanceAfterStop = Session.Advance(Timestep(0.01));
		PF_CHECK(Tests, !AdvanceAfterStop && AdvanceAfterStop.error().Code == PlayerSessionErrorCode::NotStarted);

		const std::array Override = { SecondAsset->ID };
		PlayerSession OverrideSession(ProjectFile, RuntimeDescription);
		PF_CHECK(Tests, OverrideSession.Start(Override.front()).has_value());
		PF_CHECK(Tests, OverrideSession.GetActiveSceneAsset() == SecondAsset->ID);
		OverrideSession.Stop();

		std::filesystem::create_directories(Root / "Other Project", Error);
		PF_CHECK(Tests, !Error);
		auto SecondProjectResult = Project::Create(Root / "Other Project" / "Other.pfproj", "Other Project");
		PF_CHECK(Tests, SecondProjectResult.has_value());
		if (!SecondProjectResult) return;
		std::optional<Project> SecondProject(std::move(*SecondProjectResult));
		Scene OtherProjectScene;
		PF_CHECK(Tests, OtherProjectScene.CreateEntity("Other Project Entity").has_value());
		std::filesystem::create_directories(SecondProject->GetRootPath() / "Assets" / "Levels", Error);
		PF_CHECK(Tests, !Error);
		auto OtherProjectAsset = SceneAssetService::Create(
			SecondProject->GetAssetRegistry(), SecondProject->GetRootPath(), "Assets/Levels/Other.scene", OtherProjectScene);
		PF_CHECK(Tests, OtherProjectAsset.has_value());
		if (!OtherProjectAsset) return;
		PF_CHECK(Tests, SecondProject->SetStartScene(OtherProjectAsset->ID).has_value());
		const auto SecondProjectID = SecondProject->GetDescription().ID;
		SecondProject.reset();
		PlayerSession OtherProjectSession(Root / "Other Project" / "Other.pfproj", RuntimeDescription);
		PF_CHECK(Tests, OtherProjectSession.Start().has_value());
		PF_CHECK(Tests, OtherProjectSession.GetProject() && OtherProjectSession.GetProject()->GetDescription().ID == SecondProjectID);
		PF_CHECK(Tests, OtherProjectSession.GetActiveSceneAsset() == OtherProjectAsset->ID);
		OtherProjectSession.Stop();

		std::filesystem::create_directories(Root / "Empty Project", Error);
		PF_CHECK(Tests, !Error);
		auto EmptyProjectResult = Project::Create(Root / "Empty Project" / "Empty.pfproj", "Empty Project");
		PF_CHECK(Tests, EmptyProjectResult.has_value());
		if (!EmptyProjectResult) return;
		std::optional<Project> EmptyProject(std::move(*EmptyProjectResult));
		EmptyProject.reset();
		PlayerSession MissingStartupSession(Root / "Empty Project" / "Empty.pfproj", RuntimeDescription);
		const auto MissingStartup = MissingStartupSession.Start();
		PF_CHECK(Tests, !MissingStartup && MissingStartup.error().Code == PlayerSessionErrorCode::MissingStartupScene);
	}
} // namespace

void RunPlayerSessionTests(TestRunner& Tests)
{
	TestPlayerSessionLifecycleAndTransitions(Tests);
}
