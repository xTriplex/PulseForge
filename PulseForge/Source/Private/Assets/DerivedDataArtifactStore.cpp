#include "Assets/DerivedDataArtifactStore.h"

#include "Core/PlatformServices.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>

#if defined(_WIN32)
	#define NOMINMAX
	#include <Windows.h>
#endif

namespace PulseForge
{
	namespace
	{
		constexpr std::array<char, 8> ArtifactMagic{ 'P', 'F', 'D', 'A', 'T', 'A', '0', '1' };
		constexpr uint32_t ArtifactEnvelopeVersion = 1;
		constexpr size_t MaximumKindBytes = 128;
		constexpr size_t MaximumAlgorithmBytes = 32;
		constexpr size_t MaximumDigestBytes = 128;

		struct EncodedHeader
		{
			uint32_t KindSize = 0;
			uint32_t KeyAlgorithmSize = 0;
			uint32_t KeyDigestSize = 0;
			uint32_t ProductSchemaVersion = 0;
			uint32_t ChecksumAlgorithmSize = 0;
			uint32_t ChecksumDigestSize = 0;
			uint64_t PayloadSize = 0;
		};

		void AppendU32(std::vector<std::byte>& Bytes, uint32_t Value)
		{
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Bytes.push_back(static_cast<std::byte>((Value >> Shift) & 0xff));
		}

		void AppendU64(std::vector<std::byte>& Bytes, uint64_t Value)
		{
			for (unsigned Shift = 0; Shift < 64; Shift += 8)
				Bytes.push_back(static_cast<std::byte>((Value >> Shift) & 0xff));
		}

		bool ReadU32(std::span<const std::byte> Bytes, size_t& Offset, uint32_t& Value)
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < 4)
				return false;
			Value = 0;
			for (unsigned Shift = 0; Shift < 32; Shift += 8)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset++])) << Shift;
			return true;
		}

		bool ReadU64(std::span<const std::byte> Bytes, size_t& Offset, uint64_t& Value)
		{
			if (Offset > Bytes.size() || Bytes.size() - Offset < 8)
				return false;
			Value = 0;
			for (unsigned Shift = 0; Shift < 64; Shift += 8)
				Value |= static_cast<uint64_t>(std::to_integer<uint8_t>(Bytes[Offset++])) << Shift;
			return true;
		}

		void AppendText(std::vector<std::byte>& Bytes, std::string_view Text)
		{
			for (char Character : Text)
				Bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
		}

		bool ReadText(std::span<const std::byte> Bytes, size_t& Offset, size_t Length, std::string& Text)
		{
			if (Offset > Bytes.size() || Length > Bytes.size() - Offset)
				return false;
			Text.resize(Length);
			for (size_t Index = 0; Index < Length; ++Index)
				Text[Index] = static_cast<char>(std::to_integer<uint8_t>(Bytes[Offset++]));
			return true;
		}

		bool IsToken(std::string_view Text, size_t MaximumLength)
		{
			if (Text.empty() || Text.size() > MaximumLength)
				return false;
			return std::ranges::all_of(Text, [](unsigned char Character)
			{
				return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
					(Character >= '0' && Character <= '9') || Character == '-' || Character == '_' || Character == '.';
			});
		}

		bool IsHexDigest(std::string_view Text)
		{
			return !Text.empty() && Text.size() <= MaximumDigestBytes && Text.size() % 2 == 0 &&
				std::ranges::all_of(Text, [](unsigned char Character)
				{
					return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f') ||
						(Character >= 'A' && Character <= 'F');
				});
		}

		bool IsValidKey(const DerivedDataArtifactKey& Key)
		{
			return IsToken(Key.ProductKind, MaximumKindBytes) && IsToken(Key.RecipeDigest.Algorithm, MaximumAlgorithmBytes) &&
				IsHexDigest(Key.RecipeDigest.DigestHex) && Key.ProductSchemaVersion != 0;
		}

		std::filesystem::path GetArtifactPath(
			const std::filesystem::path& Root,
			const DerivedDataArtifactKey& Key)
		{
			AssetFingerprintBuilder Builder("PulseForge.DerivedDataArtifactPath.v1");
			Builder.UpdateText("product-kind", Key.ProductKind);
			Builder.UpdateText("recipe-algorithm", Key.RecipeDigest.Algorithm);
			Builder.UpdateText("recipe-digest", Key.RecipeDigest.DigestHex);
			std::array<std::byte, 4> SchemaBytes{};
			for (size_t Index = 0; Index < SchemaBytes.size(); ++Index)
				SchemaBytes[Index] = static_cast<std::byte>((Key.ProductSchemaVersion >> (Index * 8)) & 0xff);
			Builder.UpdateBytes("product-schema", SchemaBytes);
			return Root / (Builder.Finish().DigestHex + ".pfartifact");
		}

		std::vector<std::byte> MakeEnvelopeHeader(
			const DerivedDataArtifactKey& Key,
			const AssetContentFingerprint& Checksum,
			uint64_t PayloadSize)
		{
			std::vector<std::byte> Header;
			Header.reserve(8 + 7 * 4 + 8 + Key.ProductKind.size() + Key.RecipeDigest.Algorithm.size() +
				Key.RecipeDigest.DigestHex.size() + Checksum.Algorithm.size() + Checksum.DigestHex.size());
			for (char Character : ArtifactMagic)
				Header.push_back(static_cast<std::byte>(Character));
			AppendU32(Header, ArtifactEnvelopeVersion);
			AppendU32(Header, static_cast<uint32_t>(Key.ProductKind.size()));
			AppendU32(Header, static_cast<uint32_t>(Key.RecipeDigest.Algorithm.size()));
			AppendU32(Header, static_cast<uint32_t>(Key.RecipeDigest.DigestHex.size()));
			AppendU32(Header, Key.ProductSchemaVersion);
			AppendU32(Header, static_cast<uint32_t>(Checksum.Algorithm.size()));
			AppendU32(Header, static_cast<uint32_t>(Checksum.DigestHex.size()));
			AppendU64(Header, PayloadSize);
			AppendText(Header, Key.ProductKind);
			AppendText(Header, Key.RecipeDigest.Algorithm);
			AppendText(Header, Key.RecipeDigest.DigestHex);
			AppendText(Header, Checksum.Algorithm);
			AppendText(Header, Checksum.DigestHex);
			return Header;
		}

		struct CacheFile final
		{
			std::filesystem::path Path;
			uint64_t Size = 0;
			std::filesystem::file_time_type LastWrite{};
		};

		bool IsReparseOrSymlink(const std::filesystem::path& Path, std::error_code& Error)
		{
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status))
				return true;
#if defined(_WIN32)
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			return Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			return false;
#endif
		}

		std::expected<std::vector<CacheFile>, std::string> EnumerateOwnedFiles(
			const std::filesystem::path& Root)
		{
			std::vector<CacheFile> Files;
			std::error_code Error;
			const auto RootStatus = std::filesystem::symlink_status(Root, Error);
			if (Error == std::errc::no_such_file_or_directory ||
				(!Error && RootStatus.type() == std::filesystem::file_type::not_found))
				return Files;
			if (Error)
				return std::unexpected("Could not inspect artifact cache root: " + Error.message());
			if (IsReparseOrSymlink(Root, Error))
				return std::unexpected("Artifact cache root must not be a symbolic link or reparse point");
			if (!std::filesystem::is_directory(RootStatus))
				return std::unexpected("Artifact cache root is not a directory");

			std::filesystem::directory_iterator Iterator(Root, Error);
			if (Error)
				return std::unexpected("Could not enumerate artifact cache root: " + Error.message());
			size_t EntriesVisited = 0;
			for (const std::filesystem::directory_entry& Entry : Iterator)
			{
				if (++EntriesVisited > 131072)
					return std::unexpected("Artifact cache root exceeds the bounded directory-entry scan limit");
				const std::filesystem::path Path = Entry.path();
				if (Path.extension() != ".pfartifact")
					continue;
				const auto Status = Entry.symlink_status(Error);
				if (Error)
					return std::unexpected("Could not inspect artifact cache entry: " + Error.message());
				if (IsReparseOrSymlink(Path, Error) || !std::filesystem::is_regular_file(Status))
					continue;
				const uintmax_t FileSize = Entry.file_size(Error);
				if (Error)
					return std::unexpected("Could not size artifact cache entry: " + Error.message());
				const auto LastWrite = Entry.last_write_time(Error);
				if (Error)
					return std::unexpected("Could not inspect artifact cache entry time: " + Error.message());
				Files.push_back({ Path, static_cast<uint64_t>(FileSize), LastWrite });
			}
			return Files;
		}

		std::expected<void, std::string> EnsureCapacity(
			const std::filesystem::path& Root,
			const std::filesystem::path& Destination,
			uint64_t NewSize,
			uint64_t MaxTotalBytes,
			size_t MaxEntries)
		{
			auto FilesResult = EnumerateOwnedFiles(Root);
			if (!FilesResult)
				return std::unexpected(FilesResult.error());
			std::vector<CacheFile>& Files = *FilesResult;
			std::sort(Files.begin(), Files.end(), [](const CacheFile& Left, const CacheFile& Right)
			{
				if (Left.LastWrite != Right.LastWrite)
					return Left.LastWrite < Right.LastWrite;
				return Left.Path.native() < Right.Path.native();
			});

			uint64_t TotalBytes = 0;
			for (const CacheFile& File : Files)
				TotalBytes = File.Size > std::numeric_limits<uint64_t>::max() - TotalBytes ?
					std::numeric_limits<uint64_t>::max() : TotalBytes + File.Size;
			const auto Existing = std::ranges::find(Files, Destination, &CacheFile::Path);
			const bool Replacing = Existing != Files.end();
			const uint64_t ExistingBytes = Replacing ? Existing->Size : 0;
			size_t DesiredEntries = Files.size() + (Replacing ? 0 : 1);

			auto OverBudget = [&]
			{
				const uint64_t BaseBytes = TotalBytes >= ExistingBytes ? TotalBytes - ExistingBytes : 0;
				return DesiredEntries > MaxEntries || NewSize > MaxTotalBytes || BaseBytes > MaxTotalBytes - NewSize;
			};

			for (const CacheFile& File : Files)
			{
				if (!OverBudget())
					break;
				if (File.Path == Destination)
					continue;
				std::error_code RemoveError;
				if (std::filesystem::remove(File.Path, RemoveError) && !RemoveError)
				{
					TotalBytes = TotalBytes >= File.Size ? TotalBytes - File.Size : 0;
					if (DesiredEntries > 0)
						--DesiredEntries;
				}
			}
			if (OverBudget())
				return std::unexpected("Artifact cache limits cannot admit this artifact without evicting its existing entry");
			return {};
		}

		std::expected<std::filesystem::path, std::string> MakeUniqueTemporaryPath(const std::filesystem::path& Destination)
		{
			auto Identifier = UUID::Generate();
			if (!Identifier)
				return std::unexpected("Could not generate a unique artifact temporary-file identifier: " + Identifier.error().Message);
			std::filesystem::path Temporary = Destination;
			Temporary += "." + Identifier->ToString() + ".tmp";
			return Temporary;
		}
	}

	DerivedDataArtifactStore::DerivedDataArtifactStore(DerivedDataArtifactStoreOptions Options)
		: m_Options(std::move(Options))
	{
	}

	DerivedDataArtifactReadResult DerivedDataArtifactStore::Read(const DerivedDataArtifactKey& Key) const
	{
		std::lock_guard Lock(m_Mutex);
		try
		{
			return ReadUnlocked(Key);
		}
		catch (const std::exception& Exception)
		{
			return { DerivedDataArtifactReadStatus::Unavailable, {},
				std::string("Artifact read failed safely: ") + Exception.what() };
		}
		catch (...)
		{
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Artifact read failed with an unknown host error" };
		}
	}

	DerivedDataArtifactReadResult DerivedDataArtifactStore::ReadUnlocked(const DerivedDataArtifactKey& Key) const
	{
		if (!m_Options.Enabled || m_Options.Root.empty())
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Derived artifact cache is disabled or has no root" };
		if (!IsValidKey(Key))
			return { DerivedDataArtifactReadStatus::Incompatible, {}, "Artifact key has an invalid product token, digest or schema version" };

		const std::filesystem::path Path = GetArtifactPath(m_Options.Root, Key);
		std::error_code Error;
		const auto RootStatus = std::filesystem::symlink_status(m_Options.Root, Error);
		if (Error == std::errc::no_such_file_or_directory ||
			(!Error && RootStatus.type() == std::filesystem::file_type::not_found))
			return { DerivedDataArtifactReadStatus::Miss, {}, {} };
		if (Error)
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not inspect artifact cache root: " + Error.message() };
		if (IsReparseOrSymlink(m_Options.Root, Error))
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Artifact cache root must not be a symbolic link or reparse point" };
		if (!std::filesystem::is_directory(RootStatus))
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Artifact cache root is not a directory" };

		const auto FileStatus = std::filesystem::symlink_status(Path, Error);
		if (Error == std::errc::no_such_file_or_directory ||
			(!Error && FileStatus.type() == std::filesystem::file_type::not_found))
			return { DerivedDataArtifactReadStatus::Miss, {}, {} };
		if (Error)
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not inspect artifact file: " + Error.message() };
		if (IsReparseOrSymlink(Path, Error) || !std::filesystem::is_regular_file(FileStatus))
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact path is not an owned regular file" };

		const uintmax_t FileSize = std::filesystem::file_size(Path, Error);
		constexpr uint64_t MaximumEnvelopeOverhead = 8 + 7 * 4 + 8 + MaximumKindBytes +
			2 * MaximumAlgorithmBytes + MaximumDigestBytes * 2;
		if (Error)
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not read artifact file size: " + Error.message() };
		if (FileSize > std::numeric_limits<size_t>::max() || (FileSize > m_Options.MaxArtifactBytes &&
			FileSize - m_Options.MaxArtifactBytes > MaximumEnvelopeOverhead) || FileSize < 8 + 7 * 4 + 8)
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact size is outside the configured or structural bounds" };

		std::ifstream Input(Path, std::ios::binary);
		constexpr size_t FixedHeaderSize = 8 + 7 * 4 + 8;
		std::array<std::byte, FixedHeaderSize> FixedHeader{};
		if (!Input || !Input.read(reinterpret_cast<char*>(FixedHeader.data()), static_cast<std::streamsize>(FixedHeader.size())))
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not read artifact header" };
		const std::span<const std::byte> FixedView(FixedHeader);
		if (!std::equal(ArtifactMagic.begin(), ArtifactMagic.end(), reinterpret_cast<const char*>(FixedView.data())))
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact magic is invalid" };
		size_t Offset = ArtifactMagic.size();
		uint32_t Version = 0;
		EncodedHeader Header;
		if (!ReadU32(FixedView, Offset, Version) || !ReadU32(FixedView, Offset, Header.KindSize) ||
			!ReadU32(FixedView, Offset, Header.KeyAlgorithmSize) || !ReadU32(FixedView, Offset, Header.KeyDigestSize) ||
			!ReadU32(FixedView, Offset, Header.ProductSchemaVersion) || !ReadU32(FixedView, Offset, Header.ChecksumAlgorithmSize) ||
			!ReadU32(FixedView, Offset, Header.ChecksumDigestSize) || !ReadU64(FixedView, Offset, Header.PayloadSize))
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact header is truncated" };
		if (Version != ArtifactEnvelopeVersion)
			return { DerivedDataArtifactReadStatus::Incompatible, {}, "Artifact envelope version is unsupported" };
		if (Header.KindSize == 0 || Header.KindSize > MaximumKindBytes || Header.KeyAlgorithmSize == 0 ||
			Header.KeyAlgorithmSize > MaximumAlgorithmBytes || Header.KeyDigestSize == 0 ||
			Header.KeyDigestSize > MaximumDigestBytes || Header.ChecksumAlgorithmSize == 0 ||
			Header.ChecksumAlgorithmSize > MaximumAlgorithmBytes || Header.ChecksumDigestSize == 0 ||
			Header.ChecksumDigestSize > MaximumDigestBytes || Header.PayloadSize > m_Options.MaxArtifactBytes)
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact header declares values outside structural bounds" };

		const uint64_t IdentitySize = static_cast<uint64_t>(Header.KindSize) + Header.KeyAlgorithmSize + Header.KeyDigestSize +
			Header.ChecksumAlgorithmSize + Header.ChecksumDigestSize;
		if (IdentitySize > FileSize - FixedHeaderSize || Header.PayloadSize != FileSize - FixedHeaderSize - IdentitySize)
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact payload length does not match the file" };
		std::vector<std::byte> Identity;
		try
		{
			Identity.resize(static_cast<size_t>(IdentitySize));
		}
		catch (const std::bad_alloc&)
		{
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not allocate bounded artifact identity fields" };
		}
		if (!Input.read(reinterpret_cast<char*>(Identity.data()), static_cast<std::streamsize>(Identity.size())))
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not read artifact identity fields" };
		const std::span<const std::byte> IdentityView(Identity);
		Offset = 0;
		std::string Kind;
		AssetContentFingerprint StoredKey;
		AssetContentFingerprint StoredChecksum;
		if (!ReadText(IdentityView, Offset, Header.KindSize, Kind) ||
			!ReadText(IdentityView, Offset, Header.KeyAlgorithmSize, StoredKey.Algorithm) ||
			!ReadText(IdentityView, Offset, Header.KeyDigestSize, StoredKey.DigestHex) ||
			!ReadText(IdentityView, Offset, Header.ChecksumAlgorithmSize, StoredChecksum.Algorithm) ||
			!ReadText(IdentityView, Offset, Header.ChecksumDigestSize, StoredChecksum.DigestHex))
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact identity section is truncated" };
		if (Kind != Key.ProductKind || StoredKey != Key.RecipeDigest || Header.ProductSchemaVersion != Key.ProductSchemaVersion)
			return { DerivedDataArtifactReadStatus::Incompatible, {}, "Artifact identity does not match the requested product recipe" };

		DerivedDataArtifactReadResult Result;
		try
		{
			Result.Payload.resize(static_cast<size_t>(Header.PayloadSize));
		}
		catch (const std::bad_alloc&)
		{
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not allocate bounded artifact payload" };
		}
		if (!Input.read(reinterpret_cast<char*>(Result.Payload.data()), static_cast<std::streamsize>(Result.Payload.size())))
			return { DerivedDataArtifactReadStatus::Unavailable, {}, "Could not read artifact payload" };
		const AssetContentFingerprint ActualChecksum = ComputeAssetContentFingerprint(Result.Payload);
		if (StoredChecksum != ActualChecksum)
			return { DerivedDataArtifactReadStatus::Corrupt, {}, "Artifact payload checksum does not match" };

		Result.Status = DerivedDataArtifactReadStatus::Hit;
		std::filesystem::last_write_time(Path, std::filesystem::file_time_type::clock::now(), Error);
		return Result;
	}

	DerivedDataArtifactWriteResult DerivedDataArtifactStore::Write(
		const DerivedDataArtifactKey& Key,
		std::span<const std::byte> Payload)
	{
		try
		{
			return WriteWithReplacement(Key, Payload, [](const std::filesystem::path& Temporary, const std::filesystem::path& Destination)
			{
				auto Replaced = Platform::ReplaceFileAtomically(Temporary, Destination);
				if (!Replaced)
					return std::expected<void, std::string>(std::unexpected(Replaced.error().Message));
				return std::expected<void, std::string>{};
			});
		}
		catch (const std::exception& Exception)
		{
			return { DerivedDataArtifactWriteStatus::Unavailable,
				std::string("Artifact write failed safely: ") + Exception.what() };
		}
		catch (...)
		{
			return { DerivedDataArtifactWriteStatus::Unavailable, "Artifact write failed with an unknown host error" };
		}
	}

	DerivedDataArtifactWriteResult DerivedDataArtifactStore::WriteWithReplacement(
		const DerivedDataArtifactKey& Key,
		std::span<const std::byte> Payload,
		const ReplaceFunction& Replace)
	{
		std::lock_guard Lock(m_Mutex);
		if (!m_Options.Enabled || m_Options.Root.empty())
			return { DerivedDataArtifactWriteStatus::Unavailable, "Derived artifact cache is disabled or has no root" };
		if (!IsValidKey(Key))
			return { DerivedDataArtifactWriteStatus::Rejected, "Artifact key has an invalid product token, digest or schema version" };
		if (Payload.size() > m_Options.MaxArtifactBytes)
			return { DerivedDataArtifactWriteStatus::Rejected, "Artifact payload exceeds the configured per-artifact bound" };

		const AssetContentFingerprint Checksum = ComputeAssetContentFingerprint(Payload);
		const std::vector<std::byte> Header = MakeEnvelopeHeader(Key, Checksum, Payload.size());
		const uint64_t EnvelopeSize = Header.size() + Payload.size();
		const std::filesystem::path Destination = GetArtifactPath(m_Options.Root, Key);
		if (m_Options.MaxEntries == 0 || EnvelopeSize > m_Options.MaxTotalBytes)
			return { DerivedDataArtifactWriteStatus::Rejected, "Artifact cannot fit within configured cache bounds" };

		std::error_code Error;
		std::filesystem::create_directories(m_Options.Root, Error);
		if (Error)
			return { DerivedDataArtifactWriteStatus::Unavailable, "Could not create artifact cache root: " + Error.message() };
		if (IsReparseOrSymlink(m_Options.Root, Error))
			return { DerivedDataArtifactWriteStatus::Unavailable, "Artifact cache root must not be a symbolic link or reparse point" };
		const auto DestinationStatus = std::filesystem::symlink_status(Destination, Error);
		if (!Error && std::filesystem::exists(DestinationStatus) &&
			(IsReparseOrSymlink(Destination, Error) || !std::filesystem::is_regular_file(DestinationStatus)))
			return { DerivedDataArtifactWriteStatus::Unavailable, "Artifact destination is not an owned regular file" };
		if (Error && Error != std::errc::no_such_file_or_directory)
			return { DerivedDataArtifactWriteStatus::Unavailable, "Could not inspect artifact destination: " + Error.message() };

		DerivedDataArtifactReadResult Existing = ReadUnlocked(Key);
		if (Existing.Status == DerivedDataArtifactReadStatus::Hit)
			return { DerivedDataArtifactWriteStatus::ExistingValid, "A valid equivalent artifact already exists" };

		auto Capacity = EnsureCapacity(m_Options.Root, Destination, EnvelopeSize, m_Options.MaxTotalBytes, m_Options.MaxEntries);
		if (!Capacity)
			return { DerivedDataArtifactWriteStatus::Unavailable, Capacity.error() };

		auto TemporaryPath = MakeUniqueTemporaryPath(Destination);
		if (!TemporaryPath)
			return { DerivedDataArtifactWriteStatus::Unavailable, TemporaryPath.error() };
		struct TemporaryCleanup final
		{
			std::filesystem::path Path;
			~TemporaryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		} Cleanup{ *TemporaryPath };

		{
			std::ofstream Output(*TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output)
				return { DerivedDataArtifactWriteStatus::Unavailable, "Could not open unique artifact temporary file" };
			Output.write(reinterpret_cast<const char*>(Header.data()), static_cast<std::streamsize>(Header.size()));
			Output.write(reinterpret_cast<const char*>(Payload.data()), static_cast<std::streamsize>(Payload.size()));
			Output.flush();
			if (!Output)
				return { DerivedDataArtifactWriteStatus::Unavailable, "Could not complete artifact temporary file" };
		}

		auto Replaced = Replace(*TemporaryPath, Destination);
		if (!Replaced)
		{
			Existing = ReadUnlocked(Key);
			if (Existing.Status == DerivedDataArtifactReadStatus::Hit)
				return { DerivedDataArtifactWriteStatus::ExistingValid, "Another equivalent writer published a valid artifact" };
			return { DerivedDataArtifactWriteStatus::Unavailable, "Could not atomically publish artifact: " + Replaced.error() };
		}
		return { DerivedDataArtifactWriteStatus::Written, {} };
	}
} // namespace PulseForge
