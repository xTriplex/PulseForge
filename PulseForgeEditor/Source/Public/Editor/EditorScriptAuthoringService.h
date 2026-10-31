#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/ScriptSourceMetadata.h"

#include <expected>
#include <filesystem>
#include <string>

namespace PulseForgeEditor
{
	[[nodiscard]] std::expected<std::filesystem::path, std::string> ResolveScriptSourcePath(
		const std::filesystem::path& ProjectRoot, const PulseForge::AssetRegistry& Registry,
		const PulseForge::AssetID& ScriptAsset);

	[[nodiscard]] std::expected<PulseForge::ScriptSourceMetadata, std::string> ReadScriptSourceMetadata(
		const std::filesystem::path& ProjectRoot, const PulseForge::AssetRegistry& Registry,
		const PulseForge::AssetID& ScriptAsset);
} // namespace PulseForgeEditor
