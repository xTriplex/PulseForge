#pragma once

#include "Assets/ImageAssetImporter.h"
#include "Core/Core.h"

#include <array>
#include <expected>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace PulseForge
{
	enum class CubeFace : uint8_t
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ
	};

	struct FloatCubeLevel
	{
		uint32_t Size = 0;
		// Face order is +X, -X, +Y, -Y, +Z, -Z; each face is row-major RGBA32F.
		std::array<std::vector<float>, 6> Faces;
	};

	struct EnvironmentLightingData
	{
		FloatCubeLevel Environment;
		FloatCubeLevel DiffuseIrradiance;
		// Mip zero is sharp (roughness zero); the last mip is roughness one.
		std::vector<FloatCubeLevel> PrefilteredSpecular;
		ImportedFloatImage BrdfIntegrationLut;
	};

	struct EnvironmentProcessingDesc
	{
		uint32_t EnvironmentFaceSize = 256;
		uint32_t IrradianceFaceSize = 32;
		uint32_t SpecularFaceSize = 128;
		uint32_t BrdfLutSize = 256;
		uint32_t IrradianceSamples = 128;
		uint32_t PrefilterSamples = 128;
		uint32_t BrdfSamples = 128;
	};

	enum class EnvironmentProcessingErrorCode : uint8_t
	{
		InvalidSource,
		InvalidDescription,
		ResourceLimitExceeded
	};

	struct EnvironmentProcessingError
	{
		EnvironmentProcessingErrorCode Code;
		std::string Message;
	};

	[[nodiscard]] PULSEFORGE_API glm::vec2 EquirectangularUvFromDirection(const glm::vec3& Direction) noexcept;
	[[nodiscard]] PULSEFORGE_API glm::vec3 CubeFaceTexelDirection(CubeFace Face, float U, float V) noexcept;
	[[nodiscard]] PULSEFORGE_API std::expected<EnvironmentLightingData, EnvironmentProcessingError> ProcessEnvironmentImage(
		const ImportedFloatImage& Source,
		const EnvironmentProcessingDesc& Description = {});
}
