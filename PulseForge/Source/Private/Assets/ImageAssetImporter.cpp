#include "Assets/ImageAssetImporter.h"
#include "Assets/AssetPathResolver.h"

#include <stb_image.h>
#include <stb_image_resize2.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <cmath>
#include <span>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		constexpr uint64_t MaxSourceBytes = 128ull * 1024ull * 1024ull;
		constexpr uint64_t MaxDecodedPixels = 32ull * 1024ull * 1024ull;
		constexpr uint64_t MaxHdrDecodedPixels = 8ull * 1024ull * 1024ull;
		constexpr int MaxImageDimension = 32768;
		constexpr uint64_t MaxPreviewSourceBytes = 64ull * 1024ull * 1024ull;
		constexpr uint64_t MaxPreviewDecodedPixels = 8ull * 1024ull * 1024ull;

		std::array<uint64_t, 2> Fingerprint(std::span<const std::byte> Bytes)
		{
			uint64_t FingerprintA = 14695981039346656037ull;
			uint64_t FingerprintB = 7809847782465536322ull;
			for (const std::byte Byte : Bytes)
			{
				const uint8_t Value = std::to_integer<uint8_t>(Byte);
				FingerprintA = (FingerprintA ^ Value) * 1099511628211ull;
				FingerprintB = (FingerprintB ^ (Value + 0x9du)) * 14029467366897019727ull;
				FingerprintB ^= FingerprintB >> 29;
			}
			return { FingerprintA, FingerprintB };
		}

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
			Image.SourceFingerprint = Fingerprint(*SourceBytes);
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

	std::expected<ImportedImage, ImageAssetImportError> ImageAssetImporter::DecodePreviewRGBA8(
		const std::filesystem::path& SourcePath,
		uint32_t MaximumEdge)
	{
		try
		{
			std::error_code FileError;
			const uintmax_t FileSize = std::filesystem::file_size(SourcePath, FileError);
			if (FileError)
				return std::unexpected(MakeError(ImageAssetImportErrorCode::FileReadFailed, SourcePath,
					"Could not determine image preview source size: " + FileError.message()));
			if (MaximumEdge == 0 || FileSize == 0 || FileSize > MaxPreviewSourceBytes)
				return std::unexpected(MakeError(ImageAssetImportErrorCode::ResourceLimitExceeded, SourcePath,
					"Image preview source is empty or exceeds its bounded source/size policy"));
			auto SourceBytes = ReadSourceFile(SourcePath);
			if (!SourceBytes)
				return std::unexpected(std::move(SourceBytes.error()));
			if (SourceBytes->size() > MaxPreviewSourceBytes)
				return std::unexpected(MakeError(ImageAssetImportErrorCode::ResourceLimitExceeded, SourcePath,
					"Image preview source grew beyond its bounded source-size policy while being read"));
			const auto* InputBytes = reinterpret_cast<const stbi_uc*>(SourceBytes->data());
			const int InputLength = static_cast<int>(SourceBytes->size());
			int Width = 0;
			int Height = 0;
			int SourceChannels = 0;
			if (!stbi_info_from_memory(InputBytes, InputLength, &Width, &Height, &SourceChannels))
			{
				const char* Failure = stbi_failure_reason();
				return std::unexpected(MakeError(ImageAssetImportErrorCode::InvalidImage, SourcePath,
					std::string("Could not inspect image preview: ") + (Failure ? Failure : "unknown decoder error")));
			}
			if (Width <= 0 || Height <= 0 || Width > MaxImageDimension || Height > MaxImageDimension ||
				static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height) > MaxPreviewDecodedPixels)
				return std::unexpected(MakeError(ImageAssetImportErrorCode::ResourceLimitExceeded, SourcePath,
					"Image preview dimensions are invalid or exceed the eight-million-pixel limit"));
			const int InspectedWidth = Width;
			const int InspectedHeight = Height;
			stbi_uc* Decoded = stbi_load_from_memory(
				InputBytes, InputLength, &Width, &Height, &SourceChannels, STBI_rgb_alpha);
			if (!Decoded)
			{
				const char* Failure = stbi_failure_reason();
				return std::unexpected(MakeError(ImageAssetImportErrorCode::InvalidImage, SourcePath,
					std::string("Could not decode image preview: ") + (Failure ? Failure : "unknown decoder error")));
			}
			const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> PixelOwner(Decoded, &stbi_image_free);
			if (Width != InspectedWidth || Height != InspectedHeight)
				return std::unexpected(MakeError(ImageAssetImportErrorCode::InvalidImage, SourcePath,
					"Image preview decoder dimensions did not match the inspected header"));

			const uint32_t Edge = (std::max)(static_cast<uint32_t>(Width), static_cast<uint32_t>(Height));
			const double Scale = (std::min)(1.0, static_cast<double>(MaximumEdge) / static_cast<double>(Edge));
			const uint32_t PreviewWidth = (std::max)(1u, static_cast<uint32_t>(std::lround(Width * Scale)));
			const uint32_t PreviewHeight = (std::max)(1u, static_cast<uint32_t>(std::lround(Height * Scale)));
			const size_t OutputPixels = static_cast<size_t>(PreviewWidth) * PreviewHeight;
			ImportedImage Image;
			Image.Width = PreviewWidth;
			Image.Height = PreviewHeight;
			Image.SourceFingerprint = Fingerprint(*SourceBytes);
			Image.RGBA8Pixels.resize(OutputPixels * 4);
			if (PreviewWidth == static_cast<uint32_t>(Width) && PreviewHeight == static_cast<uint32_t>(Height))
				std::memcpy(Image.RGBA8Pixels.data(), PixelOwner.get(), Image.RGBA8Pixels.size());
			else if (!stbir_resize_uint8_srgb(PixelOwner.get(), Width, Height, 0,
				Image.RGBA8Pixels.data(), static_cast<int>(PreviewWidth), static_cast<int>(PreviewHeight), 0, STBIR_RGBA))
				return std::unexpected(MakeError(ImageAssetImportErrorCode::InvalidImage, SourcePath,
					"Image preview downsampling failed"));
			return Image;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakeError(ImageAssetImportErrorCode::ResourceLimitExceeded, SourcePath,
				"Image preview could not allocate its bounded decode buffers"));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(ImageAssetImportErrorCode::FilesystemFailure, SourcePath,
				std::string("Image preview failed: ") + Exception.what()));
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
			uint64_t FingerprintA = 14695981039346656037ull;
			uint64_t FingerprintB = 7809847782465536322ull;
			for (const std::byte Byte : *SourceBytes)
			{
				const uint8_t Value = std::to_integer<uint8_t>(Byte);
				FingerprintA = (FingerprintA ^ Value) * 1099511628211ull;
				FingerprintB = (FingerprintB ^ (Value + 0x9du)) * 14029467366897019727ull;
				FingerprintB ^= FingerprintB >> 29;
			}
			Image.SourceFingerprint = { FingerprintA, FingerprintB };
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
