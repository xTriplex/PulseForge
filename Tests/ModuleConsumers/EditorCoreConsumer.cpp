#include "Editor/EditorCommandLine.h"
#include "Editor/ViewportMath.h"

#include <array>
#include <filesystem>
#include <string_view>

int main()
{
	const std::array<std::string_view, 0> Arguments{};
	const auto Options = PulseForgeEditor::ParseEditorCommandLine(Arguments, std::filesystem::current_path());
	if (!Options || Options->ProjectFile) return 1;

	const PulseForgeEditor::ViewportImageRect ImageRect{ .Minimum = { 0.0f, 0.0f }, .Maximum = { 100.0f, 50.0f } };
	const auto Center = PulseForgeEditor::ViewportImageToNdc({ 50.0f, 25.0f }, ImageRect);
	if (!Center || glm::length(*Center) > 1.0e-6f) return 2;

	return 0;
}
