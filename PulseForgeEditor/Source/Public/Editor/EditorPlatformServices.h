#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct GLFWwindow;

namespace PulseForgeEditor
{
	struct EditorFileDialogFilter
	{
		std::string_view Name;
		std::string_view Extensions;
	};

	// Internal editor adapter contract. Its Public/ placement follows the project source layout;
	// it is not a supported external API.
	class EditorPlatformServices final
	{
	public:
		[[nodiscard]] static std::expected<void, std::string> InitializeNativeFileDialogs();
		static void ShutdownNativeFileDialogs() noexcept;

		[[nodiscard]] static std::expected<std::optional<std::filesystem::path>, std::string> ShowOpenFileDialog(
			GLFWwindow* Parent, const EditorFileDialogFilter* Filter, const std::filesystem::path& DefaultPath);

		[[nodiscard]] static std::expected<std::optional<std::filesystem::path>, std::string> ShowSaveFileDialog(GLFWwindow* Parent,
			const EditorFileDialogFilter* Filter, const std::filesystem::path& DefaultPath, std::string_view DefaultName);

		[[nodiscard]] static bool CanPositionNativeWindows() noexcept;
		static void ApplyMainWindowAppearance(GLFWwindow* Window) noexcept;
	};
} // namespace PulseForgeEditor
