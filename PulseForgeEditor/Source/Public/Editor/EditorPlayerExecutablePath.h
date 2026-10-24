#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace PulseForgeEditor
{
	enum class EditorPlayerPlatform
	{
		Windows,
		Linux,
		MacOS
	};

	[[nodiscard]] EditorPlayerPlatform GetEditorPlayerPlatform() noexcept;
	[[nodiscard]] std::expected<std::filesystem::path, std::string> ResolveEditorPlayerExecutablePath(
		const std::filesystem::path& EditorExecutable,
		EditorPlayerPlatform Platform);
} // namespace PulseForgeEditor
