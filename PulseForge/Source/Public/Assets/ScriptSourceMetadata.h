#pragma once

#include "Core/Core.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	struct ScriptFloatPropertyDeclaration final
	{
		std::string ID;
		std::string Label;
		float DefaultValue = 0.0f;
		float Minimum = -1.0e9f;
		float Maximum = 1.0e9f;
	};

	struct ScriptSourceMetadata final
	{
		bool HasDeclarations = false;
		std::vector<ScriptFloatPropertyDeclaration> FloatProperties;
		// Extensionless, importer-relative module names. Resolution is relative to the importer under Assets/Scripts.
		std::vector<std::string> Modules;
	};

	struct ScriptSourceMetadataError final
	{
		size_t Line = 1;
		std::string Message;
	};

	// Reads only the optional leading `--[[PulseForgeScript:1` JSON block. It never executes Lua source.
	[[nodiscard]] PULSEFORGE_API std::expected<ScriptSourceMetadata, ScriptSourceMetadataError> ParseScriptSourceMetadata(
		std::string_view Source);

	// Resolves an extensionless module declaration to a normalized project-relative Assets/Scripts/*.lua path.
	// The returned spelling uses generic UTF-8 path syntax and never escapes the managed script root.
	[[nodiscard]] PULSEFORGE_API std::expected<std::filesystem::path, ScriptSourceMetadataError> ResolveScriptModuleAssetPath(
		std::string_view ImporterProjectRelativePath,
		std::string_view DeclaredRelativePath);
} // namespace PulseForge
