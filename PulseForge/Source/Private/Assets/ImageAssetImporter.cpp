#include "Core/PulseForgePCH.h"
#include "Assets/ImageAssetImporter.h"
#include "Assets/AssetPathResolver.h"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <cmath>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		constexpr uint64_t MaxSourceBytes = 128ull * 1024ull * 1024ull;
		constexpr uint64_t MaxDecodedPixels = 32ull * 1024ull * 1024ull;
		constexpr uint64_t MaxHdrDecodedPixels = 8ull * 1024ull * 1024ull;
		constexpr int MaxImageDimension = 32768;

		bool HasHdrExtension(const std::filesystem::path& Path)
		{
			std::string Extension = Path.extension().string();
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character)
			{
				return static_cast<char>(std::tolower(Character));
			});
			return Extension == ".hdr";
		}

		ImageAssetImportError MakeError(
			ImageAssetImportErrorCode Code,
			const std::filesystem::path& Path,
			std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		std::expected<std::vector<std::byte>, ImageAssetImportError> ReadSourceFile(const std::filesystem::path& Path)
		{
			std::error_code FileError;
			const uintmax_t FileSize = std::filesystem::file_size(Path, FileError);
			if (FileError)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::FileReadFailed,
					Path,
					"Could not determine image asset size: " + FileError.message()));
			}
			if (FileSize == 0 || FileSize > MaxSourceBytes ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<int>::max()) ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::ResourceLimitExceeded,
					Path,
					"Image asset is empty or exceeds the importer source-size limit"));
			}

			std::vector<std::byte> Bytes(static_cast<size_t>(FileSize));
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::FileReadFailed,
					Path,
					"Could not open image asset"));
			}
			Input.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!Input || Input.bad())
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::FileReadFailed,
					Path,
					"Could not read the complete image asset"));
			}
			return Bytes;
		}
	}

	std::expected<ImportedImage, ImageAssetImportError> ImageAssetImporter::ImportRGBA8(
		const AssetID& Asset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry)
	{
		try
		{
			const auto Record = Registry.Find(Asset);
			if (!Record)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::AssetNotFound,
					{},
					"Image asset UUID " + Asset.ToString() + " is not present in the project asset registry"));
			}
			if (HasHdrExtension(Record->ProjectRelativePath))
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					Record->ProjectRelativePath,
					"Radiance .hdr assets require ImportRGBA32F and cannot be quantized through the LDR image path"));
			}

			auto SourcePath = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!SourcePath)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidProjectPath,
					SourcePath.error().Path,
					SourcePath.error().Message));
			}

			auto SourceBytes = ReadSourceFile(*SourcePath);
			if (!SourceBytes)
				return std::unexpected(std::move(SourceBytes.error()));

			const auto* InputBytes = reinterpret_cast<const stbi_uc*>(SourceBytes->data());
			const int InputLength = static_cast<int>(SourceBytes->size());
			int Width = 0;
			int Height = 0;
			int SourceChannels = 0;
			if (stbi_info_from_memory(InputBytes, InputLength, &Width, &Height, &SourceChannels) == 0)
			{
				const char* Failure = stbi_failure_reason();
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					*SourcePath,
					std::string("Could not inspect image asset: ") + (Failure == nullptr ? "unknown decoder error" : Failure)));
			}
			if (Width <= 0 || Height <= 0 || Width > MaxImageDimension || Height > MaxImageDimension ||
				static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height) > MaxDecodedPixels)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::ResourceLimitExceeded,
					*SourcePath,
					"Image dimensions exceed the importer limits (32768 per dimension, 32 million pixels total)"));
			}
			const int InspectedWidth = Width;
			const int InspectedHeight = Height;

			stbi_uc* DecodedPixels = stbi_load_from_memory(
				InputBytes,
				InputLength,
				&Width,
				&Height,
				&SourceChannels,
				STBI_rgb_alpha);
			if (DecodedPixels == nullptr)
			{
				const char* Failure = stbi_failure_reason();
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					*SourcePath,
					std::string("Could not decode image asset as RGBA8: ") + (Failure == nullptr ? "unknown decoder error" : Failure)));
			}
			const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> PixelOwner(DecodedPixels, &stbi_image_free);
			if (Width != InspectedWidth || Height != InspectedHeight ||
				Width <= 0 || Height <= 0 || Width > MaxImageDimension || Height > MaxImageDimension ||
				static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height) > MaxDecodedPixels)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					*SourcePath,
					"Image decoder dimensions did not match the inspected image header"));
			}

			const size_t PixelCount = static_cast<size_t>(Width) * static_cast<size_t>(Height);
			if (PixelCount > std::numeric_limits<size_t>::max() / 4)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::ResourceLimitExceeded,
					*SourcePath,
					"Image RGBA8 storage size exceeds the platform address range"));
			}

			ImportedImage Image;
			Image.Width = static_cast<uint32_t>(Width);
			Image.Height = static_cast<uint32_t>(Height);
			Image.RGBA8Pixels.resize(PixelCount * 4);
			std::memcpy(Image.RGBA8Pixels.data(), PixelOwner.get(), Image.RGBA8Pixels.size());
			return Image;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakeError(
				ImageAssetImportErrorCode::ResourceLimitExceeded,
				{},
				"Image asset import could not allocate the bounded decode buffers"));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				ImageAssetImportErrorCode::FilesystemFailure,
				{},
				std::string("Image asset import failed: ") + Exception.what()));
		}
	}

	std::expected<ImportedFloatImage, ImageAssetImportError> ImageAssetImporter::ImportRGBA32F(
		const AssetID& Asset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry)
	{
		try
		{
			const auto Record = Registry.Find(Asset);
			if (!Record)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::AssetNotFound,
					{},
					"HDR image asset UUID " + Asset.ToString() + " is not present in the project asset registry"));
			}
			if (!HasHdrExtension(Record->ProjectRelativePath))
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					Record->ProjectRelativePath,
					"Floating-point environment import only accepts managed Radiance .hdr assets"));
			}

			auto SourcePath = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!SourcePath)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidProjectPath,
					SourcePath.error().Path,
					SourcePath.error().Message));
			}
			auto SourceBytes = ReadSourceFile(*SourcePath);
			if (!SourceBytes)
				return std::unexpected(std::move(SourceBytes.error()));

			const auto* InputBytes = reinterpret_cast<const stbi_uc*>(SourceBytes->data());
			const int InputLength = static_cast<int>(SourceBytes->size());
			if (stbi_is_hdr_from_memory(InputBytes, InputLength) == 0)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					*SourcePath,
					"Managed .hdr asset is not a valid Radiance floating-point image"));
			}

			int Width = 0;
			int Height = 0;
			int SourceChannels = 0;
			if (stbi_info_from_memory(InputBytes, InputLength, &Width, &Height, &SourceChannels) == 0)
			{
				const char* Failure = stbi_failure_reason();
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					*SourcePath,
					std::string("Could not inspect Radiance HDR image: ") +
						(Failure == nullptr ? "unknown decoder error" : Failure)));
			}
			if (Width <= 0 || Height <= 0 || Width > MaxImageDimension || Height > MaxImageDimension ||
				static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height) > MaxHdrDecodedPixels)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::ResourceLimitExceeded,
					*SourcePath,
					"HDR image dimensions are invalid or exceed importer limits"));
			}
			const int InspectedWidth = Width;
			const int InspectedHeight = Height;

			float* DecodedPixels = stbi_loadf_from_memory(
				InputBytes,
				InputLength,
				&Width,
				&Height,
				&SourceChannels,
				STBI_rgb_alpha);
			if (DecodedPixels == nullptr)
			{
				const char* Failure = stbi_failure_reason();
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					*SourcePath,
					std::string("Could not decode Radiance HDR image: ") +
						(Failure == nullptr ? "unknown decoder error" : Failure)));
			}
			const std::unique_ptr<float, decltype(&stbi_image_free)> PixelOwner(DecodedPixels, &stbi_image_free);
			if (Width != InspectedWidth || Height != InspectedHeight || Width <= 0 || Height <= 0 ||
				static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height) > MaxHdrDecodedPixels)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::InvalidImage,
					*SourcePath,
					"HDR decoder dimensions did not match the inspected image header"));
			}

			const size_t PixelCount = static_cast<size_t>(Width) * static_cast<size_t>(Height);
			if (PixelCount > std::numeric_limits<size_t>::max() / 4)
			{
				return std::unexpected(MakeError(
					ImageAssetImportErrorCode::ResourceLimitExceeded,
					*SourcePath,
					"HDR RGBA32F storage size exceeds the platform address range"));
			}
			const size_t ChannelCount = PixelCount * 4;
			for (size_t Channel = 0; Channel < ChannelCount; ++Channel)
			{
				if (!std::isfinite(PixelOwner.get()[Channel]))
				{
					return std::unexpected(MakeError(
						ImageAssetImportErrorCode::InvalidImage,
						*SourcePath,
						"HDR image contains a non-finite decoded channel"));
				}
			}

			ImportedFloatImage Image;
			Image.Width = static_cast<uint32_t>(Width);
			Image.Height = static_cast<uint32_t>(Height);
			Image.RGBA32FPixels.assign(PixelOwner.get(), PixelOwner.get() + ChannelCount);
			return Image;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakeError(
				ImageAssetImportErrorCode::ResourceLimitExceeded,
				{},
				"HDR image import could not allocate the bounded decode buffers"));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				ImageAssetImportErrorCode::FilesystemFailure,
				{},
				std::string("HDR image import failed: ") + Exception.what()));
		}
	}
}
