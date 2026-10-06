#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace PulseForge
{
	struct ImportedImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		// Always four channels in top-to-bottom row order. The renderer chooses sRGB or linear interpretation.
		std::vector<uint8_t> RGBA8Pixels;
	};

	enum class ImageAssetImportErrorCode : uint8_t
	{
		AssetNotFound,
		InvalidProjectPath,
		FilesystemFailure,
		FileReadFailed,
		InvalidImage,
		ResourceLimitExceeded
	};

	struct ImageAssetImportError
	{
		ImageAssetImportErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API ImageAssetImporter final
	{
	public:
		// Decodes a managed image by UUID. Decoded pixels are owned by the result; source paths are only registry locations.
		[[nodiscard]] static std::expected<ImportedImage, ImageAssetImportError> ImportRGBA8(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry);
	};
}
