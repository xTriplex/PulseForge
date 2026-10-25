#include "Assets/Detail/GltfSourceDocument.h"

#include <array>
#include <cstring>
#include <utility>

namespace PulseForge::Assets::Detail
{
	namespace
	{
		bool ReadUInt32(std::span<const std::byte> Data, size_t Offset, uint32_t& Value)
		{
			if (Offset > Data.size() || Data.size() - Offset < sizeof(uint32_t)) return false;

			Value = 0;
			for (size_t Byte = 0; Byte < sizeof(uint32_t); ++Byte)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Data[Offset + Byte])) << (Byte * 8);
			return true;
		}

		bool HasWindowsAmbiguousTrailingCharacter(const std::filesystem::path& Component)
		{
			const std::u8string Text = Component.generic_u8string();
			return !Text.empty() && (Text.back() == u8'.' || Text.back() == u8' ');
		}
	} // namespace

	std::expected<GlbDocumentLayout, std::string> InspectGlbLayout(
		uint64_t FileSize, uint64_t MaximumJsonBytes, const GlbReadRange& ReadRange)
	{
		if (!ReadRange) return std::unexpected("GLB range reader is not available");
		if (FileSize < 12) return std::unexpected("GLB header is truncated");

		std::array<std::byte, 12> Header{};
		if (!ReadRange(0, Header)) return std::unexpected("GLB header could not be read completely");

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
			if (!ReadRange(Offset, ChunkHeader)) return std::unexpected("GLB chunk header could not be read completely");

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
			else if (ChunkType == GlbJsonChunk) return std::unexpected("GLB contains more than one JSON chunk");
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

	std::expected<std::string, std::string> ReadGlbJsonChunk(uint64_t FileSize, uint64_t MaximumJsonBytes, const GlbReadRange& ReadRange)
	{
		auto Layout = InspectGlbLayout(FileSize, MaximumJsonBytes, ReadRange);
		if (!Layout) return std::unexpected(Layout.error());

		std::string Json(Layout->Json.Length, '\0');
		if (!ReadRange(Layout->Json.Offset, std::span<std::byte>(reinterpret_cast<std::byte*>(Json.data()), Json.size())))
			return std::unexpected("GLB JSON chunk could not be read completely");
		return Json;
	}

	std::expected<std::optional<std::filesystem::path>, std::string> DecodeGltfBufferUri(std::string_view Uri)
	{
		if (Uri.starts_with("data:")) return std::optional<std::filesystem::path>{};

		std::string Decoded;
		Decoded.reserve(Uri.size());
		for (size_t Index = 0; Index < Uri.size(); ++Index)
		{
			if (Uri[Index] != '%')
			{
				Decoded.push_back(Uri[Index]);
				continue;
			}
			if (Index + 2 >= Uri.size()) return std::unexpected("external buffer URI has an incomplete percent escape");

			const auto HexValue = [](char Character) -> int
			{
				if (Character >= '0' && Character <= '9') return Character - '0';
				if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
				if (Character >= 'A' && Character <= 'F') return Character - 'A' + 10;
				return -1;
			};
			const int High = HexValue(Uri[Index + 1]);
			const int Low = HexValue(Uri[Index + 2]);
			if (High < 0 || Low < 0) return std::unexpected("external buffer URI has an invalid percent escape");
			const char Character = static_cast<char>((High << 4) | Low);
			if (Character == '\0' || Character == '\\' || Character == ':' || Character == '?' || Character == '#')
				return std::unexpected("external buffer URI contains a forbidden path character");
			Decoded.push_back(Character);
			Index += 2;
		}

		if (Decoded.empty() || Decoded.front() == '/' || Decoded.find('\0') != std::string::npos ||
			Decoded.find('\\') != std::string::npos || Decoded.find(':') != std::string::npos || Decoded.find('?') != std::string::npos ||
			Decoded.find('#') != std::string::npos)
		{
			return std::unexpected("external buffer URI must be a relative file path");
		}

		try
		{
			std::filesystem::path RelativePath = std::filesystem::u8path(Decoded);
			if (RelativePath.is_absolute() || RelativePath.has_root_name() || RelativePath.has_root_directory())
				return std::unexpected("external buffer URI must be relative");
			for (const std::filesystem::path& Component : RelativePath)
			{
				if (Component == "." || Component == "..") continue;
				if (HasWindowsAmbiguousTrailingCharacter(Component))
					return std::unexpected("external buffer URI contains a Windows-ambiguous path component");
			}
			return std::optional<std::filesystem::path>{ std::move(RelativePath) };
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("external buffer URI is not a valid filesystem path: ") + Exception.what());
		}
	}
} // namespace PulseForge::Assets::Detail
