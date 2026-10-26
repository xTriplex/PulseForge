#pragma once

#include "Input/GameplayInput.h"
#include "Scene/UUID.h"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace PulseForge
{
	struct GameplayInputMapLoad
	{
		GameplayActionMap ActionMap;
		std::vector<std::string> Warnings;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<GameplayActionMap, GameplayInputError> LoadProjectGameplayActionMap(
		const std::filesystem::path& ProjectRoot);

	// Project defaults and per-user overrides are deliberately stored separately.
	[[nodiscard]] PULSEFORGE_API std::expected<GameplayInputMapLoad, GameplayInputError> LoadGameplayInputMap(
		const std::filesystem::path& ProjectRoot,
		const UUID& ProjectId,
		const std::filesystem::path& UserDataRoot);
	[[nodiscard]] PULSEFORGE_API std::expected<GameplayInputMapLoad, GameplayInputError> ApplyGameplayInputOverrides(
		GameplayActionMap Defaults,
		const UUID& ProjectId,
		const std::filesystem::path& UserDataRoot);

	[[nodiscard]] PULSEFORGE_API std::expected<void, GameplayInputError> SaveGameplayInputBindingOverride(
		const std::filesystem::path& UserDataRoot,
		const UUID& ProjectId,
		std::string_view ActionId,
		std::string_view BindingId,
		const ActionBinding& Binding);
}
