#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace PulseForge
{
	namespace Assets::Detail
	{
		inline constexpr uint32_t GlbMagic = 0x46546C67;
		inline constexpr uint32_t GlbJsonChunk = 0x4E4F534A;
		inline constexpr uint32_t GlbBinaryChunk = 0x004E4942;

		struct GlbChunkRange
		{
			uint64_t Offset = 0;
			uint32_t Length = 0;
		};

		struct GlbDocumentLayout
		{
			GlbChunkRange Json;
			std::optional<GlbChunkRange> Binary;
		};

		// ReadAt must fill the requested span without reading or retaining other file ranges.
		template <typename ReadAt>
		[[nodiscard]] std::expected<GlbDocumentLayout, std::string> InspectGlbLayout(
			uint64_t FileSize, uint64_t MaximumJsonBytes, ReadAt&& ReadRange)
		{
			const auto ReadUInt32 = [](std::span<const std::byte> Data, size_t Offset, uint32_t& Value)
			{
				if (Offset > Data.size() || Data.size() - Offset < sizeof(uint32_t)) return false;
				Value = 0;
				for (size_t Byte = 0; Byte < sizeof(uint32_t); ++Byte)
					Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Data[Offset + Byte])) << (Byte * 8);
				return true;
			};

			if (FileSize < 12) return std::unexpected("GLB header is truncated");
			std::array<std::byte, 12> Header{};
			if (!ReadRange(0, std::span<std::byte>(Header))) return std::unexpected("GLB header could not be read completely");

			uint32_t Magic = 0;
			uint32_t Version = 0;
			uint32_t DeclaredLength = 0;
			if (!ReadUInt32(Header, 0, Magic) || !ReadUInt32(Header, 4, Version) || !ReadUInt32(Header, 8, DeclaredLength) ||
				Magic != GlbMagic || Version != 2 || DeclaredLength != FileSize)
			{
				return std::unexpected("GLB header must contain the glTF magic, version 2, and exact file length");
			}

			GlbDocumentLayout Layout;
			bool FoundJson = false;
			size_t ChunkIndex = 0;
			uint64_t Offset = 12;
			while (Offset < FileSize)
			{
				if (FileSize - Offset < 8) return std::unexpected("GLB contains a truncated chunk header");
				std::array<std::byte, 8> ChunkHeader{};
				if (!ReadRange(Offset, std::span<std::byte>(ChunkHeader)))
					return std::unexpected("GLB chunk header could not be read completely");

				uint32_t ChunkLength = 0;
				uint32_t ChunkType = 0;
				if (!ReadUInt32(ChunkHeader, 0, ChunkLength) || !ReadUInt32(ChunkHeader, 4, ChunkType))
					return std::unexpected("GLB contains a truncated chunk header");

				const uint64_t PayloadOffset = Offset + 8;
				if (ChunkLength % 4 != 0 || static_cast<uint64_t>(ChunkLength) > FileSize - PayloadOffset)
					return std::unexpected("GLB chunk length is misaligned or extends beyond the file");

				if (!FoundJson)
				{
					if (ChunkType != GlbJsonChunk) return std::unexpected("The first GLB chunk must contain JSON");
					if (ChunkLength > MaximumJsonBytes) return std::unexpected("GLB JSON chunk exceeds the importer limit");
					Layout.Json = { PayloadOffset, ChunkLength };
					FoundJson = true;
				}
				else if (ChunkType == GlbJsonChunk)
				{
					return std::unexpected("GLB contains more than one JSON chunk");
				}
				else if (ChunkType == GlbBinaryChunk)
				{
					if (Layout.Binary || ChunkIndex != 1)
						return std::unexpected("GLB binary chunk must appear once, immediately after the JSON chunk");
					Layout.Binary = GlbChunkRange{ PayloadOffset, ChunkLength };
				}

				Offset = PayloadOffset + ChunkLength;
				++ChunkIndex;
			}

			if (!FoundJson) return std::unexpected("GLB does not contain a JSON chunk");
			return Layout;
		}

		template <typename ReadAt>
		[[nodiscard]] std::expected<std::string, std::string> ReadGlbJsonChunk(
			uint64_t FileSize, uint64_t MaximumJsonBytes, ReadAt&& ReadRange)
		{
			auto Layout = InspectGlbLayout(FileSize, MaximumJsonBytes, ReadRange);
			if (!Layout) return std::unexpected(Layout.error());
			std::string Json(Layout->Json.Length, '\0');
			if (!ReadRange(Layout->Json.Offset, std::span<std::byte>(reinterpret_cast<std::byte*>(Json.data()), Json.size())))
				return std::unexpected("GLB JSON chunk could not be read completely");
			return Json;
		}
	} // namespace Assets::Detail

	enum class AssetOperationErrorCode : uint8_t
	{
		InvalidProject,
		InvalidPath,
		InvalidProjectAssets,
		SourceNotManaged,
		DestinationExists,
		FilesystemFailure,
		MetadataFailure,
		RecoveryRequired,
		CommittedWithCleanupFailure,
		RegistryRefreshFailed,
		ImportSourceInvalid,
		AssetNotFound
	};

	struct AssetOperationError
	{
		AssetOperationErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
		// Set only when the operation committed the asset but failed to remove auxiliary recovery data.
		std::optional<AssetRecord> CommittedAsset;
		// Identifies temporary recovery data retained after explicit cleanup failed.
		std::optional<std::filesystem::path> RecoveryPath;
	};

	class PULSEFORGE_API AssetOperations final
	{
	public:
		using RegistryRefresh = std::function<std::expected<void, AssetRegistryError>(const std::filesystem::path&)>;
		// Destination paths are project-relative. Existing source assets are selected by UUID and resolved through the registry.
		// Operations validate and refresh the supplied registry; move preserves identity and duplicate creates a new UUID.
		// Import copies an external regular file into Assets and creates a fresh sidecar UUID.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> ImportFile(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourceFile,
			const std::filesystem::path& DestinationPath);
		// Creates a new managed source file from bytes, assigns a fresh UUID sidecar, and refreshes the registry.
		// On CommittedWithCleanupFailure, the returned error includes CommittedAsset; callers must not retry creation.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> CreateAssetFromBytes(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			std::span<const std::byte> Contents,
			const std::filesystem::path& DestinationPath);
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> Move(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath);
#if defined(PF_DEBUG)
		// The refresh callback is available only in Debug builds so tests can deterministically exercise
		// post-move registry failure and recovery without changing normal production behavior.
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> MoveWithRegistryRefreshForTesting(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath,
			const RegistryRefresh& Refresh);
#endif
		// Moves a managed Assets subdirectory beneath another existing managed directory. The folder's
		// asset sidecars move with it, preserving every AssetID. Source and destination are project-relative.
		[[nodiscard]] static std::expected<std::filesystem::path, AssetOperationError> MoveFolder(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourceFolderPath,
			const std::filesystem::path& DestinationParentPath);
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> Duplicate(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath);
		// Permanently removes both the managed source and its sidecar. The caller owns any UI confirmation policy.
		[[nodiscard]] static std::expected<void, AssetOperationError> Delete(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset);
	private:
		[[nodiscard]] static std::expected<AssetRecord, AssetOperationError> MoveImpl(
			AssetRegistry& Registry,
			const std::filesystem::path& ProjectRoot,
			const AssetID& SourceAsset,
			const std::filesystem::path& DestinationPath,
			const RegistryRefresh& Refresh);
	};
}
