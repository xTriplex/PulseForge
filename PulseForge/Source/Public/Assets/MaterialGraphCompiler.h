#pragma once

#include "Assets/MaterialGraphArtifact.h"
#include "Assets/MaterialGraphSource.h"
#include "Core/Core.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace PulseForge
{
	class DerivedDataArtifactStore;

	enum class MaterialGraphCompileTarget : uint8_t
	{
		VulkanSpirV,
		Direct3DDXIL,
		OpenGLSpirV
	};

	struct MaterialGraphCompileRequest final
	{
		MaterialGraphSource Source;
		std::filesystem::path CompilerExecutable;
		std::string ToolchainFingerprint;
		std::chrono::milliseconds Timeout{ 30'000 };
		size_t MaximumOutputBytes = 16 * 1024 * 1024;
		uint64_t Generation = 0;
		std::function<bool(uint64_t)> IsGenerationCurrent;
		DerivedDataArtifactStore* ArtifactStore = nullptr;
		std::stop_token CancellationToken;
		MaterialGraphCompileTarget Target = MaterialGraphCompileTarget::VulkanSpirV;
	};

	enum class MaterialGraphCompileErrorCode : uint8_t
	{
		InvalidGraph,
		InvalidRequest,
		CompilerUnavailable,
		CompilerFailed,
		UnsupportedTarget,
		TimedOut,
		Cancelled,
		StaleGeneration,
		OutputInvalid
	};

	struct MaterialGraphCompileError final
	{
		MaterialGraphCompileErrorCode Code = MaterialGraphCompileErrorCode::InvalidRequest;
		std::vector<MaterialGraphDiagnostic> Diagnostics;
		std::string Message;
	};

	class PULSEFORGE_API MaterialGraphCompiler final
	{
	public:
		[[nodiscard]] static std::expected<MaterialGraphCompileIdentity, MaterialGraphCompileError> Describe(
			const MaterialGraphSource& Source, std::string_view ToolchainFingerprint);
		[[nodiscard]] static std::expected<MaterialGraphCompiledArtifact, MaterialGraphCompileError> Compile(
			const MaterialGraphCompileRequest& Request);
	};

	[[nodiscard]] PULSEFORGE_API std::string GetMaterialGraphCompilerToolchainFingerprint();
}
