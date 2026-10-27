#pragma once

#include "Assets/AssetID.h"
#include "Renderer/RendererAPI.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace PulseForgeGame
{
	struct PlayerLaunchConfiguration
	{
		std::filesystem::path ProjectFile;
		std::filesystem::path CookedContentRoot;
		std::optional<PulseForge::AssetID> StartupScene;
		std::optional<uint32_t> WindowWidth;
		std::optional<uint32_t> WindowHeight;
		std::optional<PulseForge::RendererAPI> Renderer;
		bool DiagnosticsEnabled = false;
	};

	enum class PlayerLaunchConfigurationErrorCode : uint8_t
	{
		InvalidWorkingDirectory,
		MissingProjectPath,
		MissingLaunchContent,
		MissingOptionValue,
		DuplicateOption,
		UnexpectedArgument,
		InvalidProjectPath,
		InvalidCookedContentPath,
		InvalidStartupScene,
		InvalidRenderer,
		InvalidWindowSize
	};

	struct PlayerLaunchConfigurationError
	{
		PlayerLaunchConfigurationErrorCode Code;
		std::string Message;
		size_t ArgumentIndex = 0;
	};

	[[nodiscard]] std::expected<PlayerLaunchConfiguration, PlayerLaunchConfigurationError> ParsePlayerLaunchConfiguration(
		std::span<const std::string_view> Arguments, const std::filesystem::path& InvocationDirectory);
} // namespace PulseForgeGame
