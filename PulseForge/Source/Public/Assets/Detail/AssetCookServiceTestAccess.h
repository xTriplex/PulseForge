#pragma once

#include "Assets/AssetCookService.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <system_error>

namespace PulseForge::Assets::Detail
{
	using CookStagedWriteFailureForTesting =
		std::function<std::error_code(const std::filesystem::path&, size_t)>;

	[[nodiscard]] PULSEFORGE_API std::expected<AssetCookReport, AssetCookError> CookWithStagedWriteFailureForTesting(
		const AssetCookRequest& Request,
		const CookStagedWriteFailureForTesting& WriteFailure);
}
