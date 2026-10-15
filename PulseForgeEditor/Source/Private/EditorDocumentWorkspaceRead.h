#pragma once

#include <expected>
#include <istream>
#include <string>

namespace PulseForgeEditor::Detail
{
	[[nodiscard]] std::expected<void, std::string> ReadCompleteWorkspaceBytes(std::istream& Input, std::string& Contents);
}
