#pragma once

#include "Assets/CookedProductCodec.h"
#include "Core/Core.h"

#include <array>
#include <expected>
#include <string>
#include <stop_token>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace PulseForge
{
	enum class EnvironmentProcessingErrorCode : uint8_t
	{
		InvalidSource,
		InvalidDescription,
		ResourceLimitExceeded,
		Cancelled
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
		const EnvironmentProcessingDesc& Description = {},
		std::stop_token StopToken = {},
		const ImportedFloatImage* CachedBrdfLut = nullptr);
	[[nodiscard]] PULSEFORGE_API std::expected<ImportedFloatImage, EnvironmentProcessingError> GenerateBrdfIntegrationLut(
		uint32_t Size,
		uint32_t Samples,
		std::stop_token StopToken = {});
}
