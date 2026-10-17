#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/ImportedImageData.h"
#include "Core/Core.h"

#include <cstdint>
#include <array>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace PulseForge
{
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
		// Decodes a bounded UI preview from a previously validated managed source path. This performs CPU work only.
		[[nodiscard]] static std::expected<ImportedImage, ImageAssetImportError> DecodePreviewRGBA8(
			const std::filesystem::path& SourcePath,
			uint32_t MaximumEdge = 192);
		// Imports managed Radiance .hdr assets as linear RGBA32F data without gamma conversion or clamping.
		[[nodiscard]] static std::expected<ImportedFloatImage, ImageAssetImportError> ImportRGBA32F(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry);
	};
}
