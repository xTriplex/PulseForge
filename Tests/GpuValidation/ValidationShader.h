#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace PulseForge::GpuValidation
{
	[[nodiscard]] std::vector<std::byte> LoadValidationShader(const std::filesystem::path& Path);
}
