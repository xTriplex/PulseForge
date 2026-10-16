#include "GpuValidation/ValidationShader.h"

#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace PulseForge::GpuValidation
{
	std::vector<std::byte> LoadValidationShader(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary | std::ios::ate);
		if (!Input) throw std::runtime_error("Could not open GPU-validation shader: " + Path.string());

		const std::streamoff FileSize = Input.tellg();
		if (FileSize < 20 || FileSize > 16 * 1024 * 1024 || FileSize % sizeof(uint32_t) != 0)
			throw std::runtime_error("GPU-validation shader has an invalid byte size: " + Path.string());

		std::vector<std::byte> Bytecode(static_cast<size_t>(FileSize));
		Input.seekg(0, std::ios::beg);
		Input.read(reinterpret_cast<char*>(Bytecode.data()), static_cast<std::streamsize>(FileSize));
		if (!Input) throw std::runtime_error("Could not read GPU-validation shader: " + Path.string());
		return Bytecode;
	}
} // namespace PulseForge::GpuValidation
