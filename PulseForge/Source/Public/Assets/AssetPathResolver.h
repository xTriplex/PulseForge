#pragma once

#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace PulseForge
{
	enum class AssetPathErrorCode : uint8_t
	{
		InvalidProjectRoot,
		InvalidRelativePath,
		InvalidAssetsDirectory,
		InvalidAssetEntry
	};

	struct AssetPathError
	{
		AssetPathErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API AssetPathResolver final
	{
	public:
		// Resolves an existing, regular managed source file without following links below <ProjectRoot>/Assets.
		[[nodiscard]] static std::expected<std::filesystem::path, AssetPathError> ResolveManagedSourcePath(
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& ProjectRelativePath);
	};
}
