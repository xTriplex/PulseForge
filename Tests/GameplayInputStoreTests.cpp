#include "TestEngineSupport.h"

#include "Input/GameplayInputStore.h"

namespace
{
	void WriteText(const std::filesystem::path& Path, std::string_view Text)
	{
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(Text.data(), static_cast<std::streamsize>(Text.size()));
	}

	std::string ReadText(const std::filesystem::path& Path)
	{
		std::ifstream Stream(Path, std::ios::binary);
		return { std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>() };
	}

	void TestProjectDefaultsAndScopedOverrides(TestRunner& Tests)
	{
		using namespace PulseForge;
		auto AcceptanceMap = LoadProjectGameplayActionMap(
			std::filesystem::path(PULSEFORGE_TEST_SOURCE_DIR) / "Samples/VisualValidation/GameplayInput");
		PF_CHECK(Tests, AcceptanceMap.has_value());
		if (AcceptanceMap)
		{
			PF_CHECK(Tests, AcceptanceMap->Actions.size() == 3);
			PF_CHECK(Tests, AcceptanceMap->Actions[0].Id == "Move" &&
				AcceptanceMap->Actions[0].Type == ActionValueType::Axis2D);
			PF_CHECK(Tests, AcceptanceMap->Actions[0].Bindings.size() == 5);
			PF_CHECK(Tests, AcceptanceMap->Actions[1].Id == "Jump" &&
				AcceptanceMap->Actions[1].Type == ActionValueType::Digital);
			PF_CHECK(Tests, AcceptanceMap->Actions[2].Id == "Interact");
		}
		std::error_code Error;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;

		auto ProjectId = UUID::Generate();
		auto OtherProjectId = UUID::Generate();
		PF_CHECK(Tests, ProjectId.has_value() && OtherProjectId.has_value());
		if (!ProjectId || !OtherProjectId) return;
		const std::filesystem::path Root = TemporaryDirectory /
			std::filesystem::u8path("PulseForge Input Store Ω " + ProjectId->ToString());
		const std::filesystem::path ProjectRoot = Root / "Project with spaces";
		const std::filesystem::path UserDataRoot = Root / "User Data";
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup()
			{
				std::error_code Ignore;
				std::filesystem::remove_all(Path, Ignore);
			}
		} CleanupRoot{ Root };

		GameplayActionMap Defaults;
		GameplayActionDefinition Jump;
		Jump.Id = "Jump";
		Jump.Type = ActionValueType::Digital;
		ActionBinding JumpBinding;
		JumpBinding.Id = "primary";
		JumpBinding.Kind = ActionBindingKind::Button;
		JumpBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) };
		Jump.Bindings.push_back(JumpBinding);
		Defaults.Actions.push_back(Jump);

		GameplayActionDefinition Confirm;
		Confirm.Id = "Confirm";
		Confirm.Type = ActionValueType::Digital;
		ActionBinding ConfirmBinding = JumpBinding;
		ConfirmBinding.Id = "primary";
		ConfirmBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Enter) };
		Confirm.Bindings.push_back(ConfirmBinding);
		Defaults.Actions.push_back(Confirm);
		auto Serialized = SerializeGameplayActionMap(Defaults);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		WriteText(ProjectRoot / "InputActions.json", *Serialized);

		auto Loaded = LoadGameplayInputMap(ProjectRoot, *ProjectId, UserDataRoot);
		PF_CHECK(Tests, Loaded.has_value());
		if (!Loaded) return;
		PF_CHECK(Tests, Loaded->Warnings.empty());
		PF_CHECK(Tests, Loaded->ActionMap.Actions.size() == 2);
		PF_CHECK(Tests, Loaded->ActionMap.Actions[0].Bindings[0].Primary == JumpBinding.Primary);

		ActionBinding UserJumpBinding = Loaded->ActionMap.Actions[0].Bindings[0];
		UserJumpBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::K) };
		auto Saved = SaveGameplayInputBindingOverride(
			UserDataRoot, *ProjectId, "Jump", "primary", UserJumpBinding);
		PF_CHECK(Tests, Saved.has_value());
		auto Reloaded = LoadGameplayInputMap(ProjectRoot, *ProjectId, UserDataRoot);
		PF_CHECK(Tests, Reloaded.has_value());
		if (Reloaded)
			PF_CHECK(Tests, Reloaded->ActionMap.Actions[0].Bindings[0].Primary == UserJumpBinding.Primary);

		auto OtherProject = LoadGameplayInputMap(ProjectRoot, *OtherProjectId, UserDataRoot);
		PF_CHECK(Tests, OtherProject.has_value());
		if (OtherProject)
			PF_CHECK(Tests, OtherProject->ActionMap.Actions[0].Bindings[0].Primary == JumpBinding.Primary);

		GameplayActionDefinition NewDefault;
		NewDefault.Id = "NewDefault";
		NewDefault.Type = ActionValueType::Digital;
		ActionBinding NewDefaultBinding = JumpBinding;
		NewDefaultBinding.Id = "primary";
		NewDefaultBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::N) };
		NewDefault.Bindings.push_back(NewDefaultBinding);
		Defaults.Actions.push_back(NewDefault);
		Serialized = SerializeGameplayActionMap(Defaults);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		WriteText(ProjectRoot / "InputActions.json", *Serialized);
		Loaded = LoadGameplayInputMap(ProjectRoot, *ProjectId, UserDataRoot);
		PF_CHECK(Tests, Loaded.has_value());
		if (Loaded)
		{
			PF_CHECK(Tests, Loaded->ActionMap.Actions.size() == 3);
			PF_CHECK(Tests, Loaded->ActionMap.Actions[0].Bindings[0].Primary == UserJumpBinding.Primary);
			PF_CHECK(Tests, Loaded->ActionMap.Actions[2].Bindings[0].Primary == NewDefaultBinding.Primary);
		}

		const std::filesystem::path OverridePath = UserDataRoot / "PulseForge" / "InputOverrides" /
			(ProjectId->ToString() + ".json");
		auto OverrideJson = nlohmann::json::parse(ReadText(OverridePath));
		OverrideJson["futureSettings"] = { { "retained", true } };
		OverrideJson["overrides"].push_back({ { "actionId", "RemovedDefault" }, { "bindingId", "primary" },
			{ "primary", "Key.R" }, { "futureField", "preserve" } });
		WriteText(OverridePath, OverrideJson.dump(2));
		ActionBinding UserConfirmBinding = Defaults.Actions[1].Bindings[0];
		UserConfirmBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::K) };
		Saved = SaveGameplayInputBindingOverride(UserDataRoot, *ProjectId, "Confirm", "primary", UserConfirmBinding);
		PF_CHECK(Tests, Saved.has_value());
		Reloaded = LoadGameplayInputMap(ProjectRoot, *ProjectId, UserDataRoot);
		PF_CHECK(Tests, Reloaded.has_value() && Reloaded->Warnings.empty());
		if (Reloaded)
			PF_CHECK(Tests, Reloaded->ActionMap.Actions[0].Bindings[0].Primary == UserConfirmBinding.Primary &&
				Reloaded->ActionMap.Actions[1].Bindings[0].Primary == UserJumpBinding.Primary);
		OverrideJson = nlohmann::json::parse(ReadText(OverridePath));
		PF_CHECK(Tests, OverrideJson.contains("futureSettings"));
		PF_CHECK(Tests, OverrideJson["overrides"].size() == 3);
		PF_CHECK(Tests, OverrideJson["overrides"][1].contains("futureField"));

		const std::string NewerContents = R"({"version":2,"projectId":"newer","overrides":[],"future":true})";
		WriteText(OverridePath, NewerContents);
		Saved = SaveGameplayInputBindingOverride(UserDataRoot, *ProjectId, "Confirm", "primary", UserConfirmBinding);
		PF_CHECK(Tests, !Saved.has_value());
		PF_CHECK(Tests, ReadText(OverridePath) == NewerContents);
		Loaded = LoadGameplayInputMap(ProjectRoot, *ProjectId, UserDataRoot);
		PF_CHECK(Tests, Loaded.has_value());
		if (Loaded)
		{
			PF_CHECK(Tests, !Loaded->Warnings.empty());
			PF_CHECK(Tests, Loaded->ActionMap.Actions.size() == 3);
			PF_CHECK(Tests, Loaded->ActionMap.Actions[0].Bindings[0].Primary == JumpBinding.Primary);
		}

		const std::string CorruptContents = "{ invalid";
		WriteText(OverridePath, CorruptContents);
		Loaded = LoadGameplayInputMap(ProjectRoot, *ProjectId, UserDataRoot);
		PF_CHECK(Tests, Loaded.has_value() && !Loaded->Warnings.empty());
		if (Loaded)
			PF_CHECK(Tests, Loaded->ActionMap.Actions.size() == 3 &&
				Loaded->ActionMap.Actions[0].Bindings[0].Primary == JumpBinding.Primary);
		PF_CHECK(Tests, ReadText(OverridePath) == CorruptContents);
		Saved = SaveGameplayInputBindingOverride(UserDataRoot, *ProjectId, "Confirm", "primary", UserConfirmBinding);
		PF_CHECK(Tests, !Saved.has_value());
		PF_CHECK(Tests, ReadText(OverridePath) == CorruptContents);
	}
}

void RunGameplayInputStoreTests(TestRunner& Tests)
{
	TestProjectDefaultsAndScopedOverrides(Tests);
}
