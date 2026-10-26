#include "Assets/Detail/GltfRelocation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace PulseForge::Assets::Detail
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr uint64_t MaximumGlbBytes = 512ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumJsonBytes = 64ull * 1024ull * 1024ull;
		constexpr size_t MaximumBufferEntries = 1'000'000;
		constexpr size_t CopyBlockBytes = 64 * 1024;

		uint32_t ReadUInt32(std::span<const std::byte> Bytes, size_t Offset)
		{
			return static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset])) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset + 1])) << 8) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset + 2])) << 16) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(Bytes[Offset + 3])) << 24);
		}

		void WriteUInt32(std::span<std::byte> Bytes, size_t Offset, uint32_t Value)
		{
			for (size_t Index = 0; Index < 4; ++Index)
				Bytes[Offset + Index] = static_cast<std::byte>((Value >> (Index * 8)) & 0xff);
		}

		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Text = Path.generic_u8string();
			return { reinterpret_cast<const char*>(Text.data()), Text.size() };
		}

		std::expected<std::filesystem::path, std::string> ValidateProjectPath(
			const std::filesystem::path& Path, std::string_view Description)
		{
			if (Path.empty() || Path.is_absolute() || Path.has_root_name() || Path.has_root_directory() || Path.lexically_normal() != Path)
				return std::unexpected(std::string(Description) + " must be a normalized project-relative path");

			auto Component = Path.begin();
			if (Component == Path.end() || *Component != "Assets")
				return std::unexpected(std::string(Description) + " must be under the managed Assets directory");
			++Component;
			if (Component == Path.end()) return std::unexpected(std::string(Description) + " must identify a file below Assets");
			for (; Component != Path.end(); ++Component)
			{
				if (Component->empty() || *Component == "." || *Component == "..")
					return std::unexpected(std::string(Description) + " contains an ambiguous path component");
				const std::string Text = PathToUtf8(*Component);
				if (Text.empty() || Text.back() == '.' || Text.back() == ' ')
					return std::unexpected(std::string(Description) + " contains a Windows-ambiguous path component");
				if (Text.find_first_of("\\:/?#") != std::string::npos || Text.find('\0') != std::string::npos)
					return std::unexpected(std::string(Description) + " contains a path character unsupported by glTF buffer URIs");
			}
			return Path;
		}

		std::expected<std::map<std::string, std::filesystem::path>, std::string> BuildMappingLookup(
			std::span<const GltfBufferPathMapping> Mappings)
		{
			std::map<std::string, std::filesystem::path> Lookup;
			for (const GltfBufferPathMapping& Mapping : Mappings)
			{
				auto OldPath = ValidateProjectPath(Mapping.OldProjectRelativePath, "Buffer mapping source");
				auto NewPath = ValidateProjectPath(Mapping.NewProjectRelativePath, "Buffer mapping destination");
				if (!OldPath) return std::unexpected(OldPath.error());
				if (!NewPath) return std::unexpected(NewPath.error());
				const std::string Key = PathToUtf8(*OldPath);
				auto [Position, Inserted] = Lookup.emplace(Key, *NewPath);
				if (!Inserted && Position->second != *NewPath)
					return std::unexpected("Buffer mappings assign conflicting destinations to " + Key);
			}
			return Lookup;
		}

		Json ParseUniqueJson(std::string_view Text)
		{
			std::vector<std::set<std::string>> ObjectKeys;
			bool HasDuplicateKey = false;
			const auto Callback = [&ObjectKeys, &HasDuplicateKey](int, Json::parse_event_t Event, Json& Parsed)
			{
				if (Event == Json::parse_event_t::object_start) ObjectKeys.emplace_back();
				else if (Event == Json::parse_event_t::key && !ObjectKeys.empty())
					HasDuplicateKey |= !ObjectKeys.back().insert(Parsed.get<std::string>()).second;
				else if (Event == Json::parse_event_t::object_end && !ObjectKeys.empty()) ObjectKeys.pop_back();
				return true;
			};
			Json Document = Json::parse(Text, Callback);
			if (HasDuplicateKey) throw std::runtime_error("JSON object contains a duplicate key");
			return Document;
		}

		struct ResolvedBufferTarget
		{
			std::filesystem::path Original;
			std::filesystem::path Relocated;
			bool IsEmbedded = false;
		};

		std::expected<ResolvedBufferTarget, std::string> ResolveBufferTarget(
			std::string_view Uri, const std::filesystem::path& SourcePath, const std::map<std::string, std::filesystem::path>& Mappings)
		{
			auto Decoded = DecodeGltfBufferUri(Uri);
			if (!Decoded) return std::unexpected(Decoded.error());
			if (!*Decoded) return ResolvedBufferTarget{ {}, {}, true };

			const std::filesystem::path OldTarget = (SourcePath.parent_path() / **Decoded).lexically_normal();
			auto ValidTarget = ValidateProjectPath(OldTarget, "Resolved buffer target");
			if (!ValidTarget) return std::unexpected("Buffer URI resolves outside the managed project: " + ValidTarget.error());

			const auto Mapping = Mappings.find(PathToUtf8(*ValidTarget));
			const std::filesystem::path RelocatedTarget = Mapping == Mappings.end() ? *ValidTarget : Mapping->second;
			return ResolvedBufferTarget{ *ValidTarget, RelocatedTarget, false };
		}

		std::expected<std::string, std::string> EncodeRelativeUri(
			const std::filesystem::path& SourcePath, const std::filesystem::path& TargetPath)
		{
			const std::filesystem::path Relative = TargetPath.lexically_relative(SourcePath.parent_path());
			if (Relative.empty() || Relative.is_absolute() || Relative.has_root_name() || Relative.has_root_directory())
				return std::unexpected("Could not encode a managed buffer target relative to the relocated source");

			const std::string Utf8 = PathToUtf8(Relative);
			if (Utf8.empty()) return std::unexpected("Relocated buffer URI would be empty");
			constexpr char Hex[] = "0123456789ABCDEF";
			std::string Encoded;
			Encoded.reserve(Utf8.size());
			for (const unsigned char Byte : Utf8)
			{
				const bool Unreserved = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') ||
										Byte == '-' || Byte == '.' || Byte == '_' || Byte == '~';
				if (Unreserved || Byte == '/') Encoded.push_back(static_cast<char>(Byte));
				else
				{
					Encoded.push_back('%');
					Encoded.push_back(Hex[Byte >> 4]);
					Encoded.push_back(Hex[Byte & 0x0f]);
				}
			}
			return Encoded;
		}

		std::expected<void, std::string> WriteBytes(const GltfRelocationWrite& Write, std::span<const std::byte> Bytes)
		{
			if (!Write) return std::unexpected("GLB relocation output writer is not available");
			return Write(Bytes);
		}
	} // namespace

	std::expected<std::string, std::string> RewriteGltfBufferUris(std::string_view JsonText,
		const std::filesystem::path& SourceOldProjectRelative, const std::filesystem::path& SourceNewProjectRelative,
		std::span<const GltfBufferPathMapping> BufferMappings)
	{
		if (JsonText.size() > MaximumJsonBytes) return std::unexpected("glTF JSON exceeds the relocation inspection limit");
		auto OldSource = ValidateProjectPath(SourceOldProjectRelative, "Original glTF source path");
		auto NewSource = ValidateProjectPath(SourceNewProjectRelative, "Relocated glTF source path");
		auto Mappings = BuildMappingLookup(BufferMappings);
		if (!OldSource) return std::unexpected(OldSource.error());
		if (!NewSource) return std::unexpected(NewSource.error());
		if (!Mappings) return std::unexpected(Mappings.error());

		try
		{
			Json Document = ParseUniqueJson(JsonText);
			if (!Document.is_object()) return std::unexpected("glTF JSON root must be an object");
			const auto Buffers = Document.find("buffers");
			if (Buffers == Document.end() || !Buffers->is_array() || Buffers->size() > MaximumBufferEntries)
				return std::unexpected("glTF buffers must be a bounded array for relocation");

			bool Changed = false;
			for (size_t Index = 0; Index < Buffers->size(); ++Index)
			{
				Json& Buffer = (*Buffers)[Index];
				if (!Buffer.is_object()) return std::unexpected("glTF buffer entry " + std::to_string(Index) + " must be an object");
				const auto Uri = Buffer.find("uri");
				if (Uri == Buffer.end()) continue; // GLB's URI-less BIN buffer is not a file URI.
				if (!Uri->is_string()) return std::unexpected("glTF buffers/" + std::to_string(Index) + "/uri must be a string");

				const std::string OriginalUri = Uri->get<std::string>();
				auto Target = ResolveBufferTarget(OriginalUri, *OldSource, *Mappings);
				if (!Target) return std::unexpected("glTF buffers/" + std::to_string(Index) + "/uri: " + Target.error());
				if (Target->IsEmbedded) continue;

				const bool SourceMoved = OldSource->parent_path() != NewSource->parent_path();
				const bool TargetMoved = Target->Original != Target->Relocated;
				if (!SourceMoved && !TargetMoved) continue;

				auto Encoded = EncodeRelativeUri(*NewSource, Target->Relocated);
				if (!Encoded) return std::unexpected("glTF buffers/" + std::to_string(Index) + "/uri: " + Encoded.error());
				if (*Encoded != OriginalUri)
				{
					*Uri = std::move(*Encoded);
					Changed = true;
				}
			}

			if (!Changed) return std::string(JsonText);
			std::string Rewritten = Document.dump(-1, ' ', false, Json::error_handler_t::strict);
			if (Rewritten.size() > MaximumJsonBytes) return std::unexpected("Rewritten glTF JSON exceeds the relocation inspection limit");
			return Rewritten;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not safely rewrite glTF buffer URIs: ") + Exception.what());
		}
	}

	std::expected<uint64_t, std::string> RewriteGlbBufferUris(uint64_t FileSize, const GlbReadRange& ReadRange,
		const std::filesystem::path& SourceOldProjectRelative, const std::filesystem::path& SourceNewProjectRelative,
		std::span<const GltfBufferPathMapping> BufferMappings, const GltfRelocationWrite& Write)
	{
		if (FileSize > MaximumGlbBytes || FileSize > std::numeric_limits<uint32_t>::max())
			return std::unexpected("GLB exceeds the bounded relocation size limit");
		if (!ReadRange || !Write) return std::unexpected("GLB relocation requires source and staging callbacks");

		auto Layout = InspectGlbLayout(FileSize, MaximumJsonBytes, ReadRange);
		if (!Layout) return std::unexpected("GLB layout is unsupported for relocation: " + Layout.error());
		auto JsonText = ReadGlbJsonChunk(FileSize, MaximumJsonBytes, ReadRange);
		if (!JsonText) return std::unexpected("GLB JSON chunk could not be read: " + JsonText.error());
		auto RewrittenJson = RewriteGltfBufferUris(*JsonText, SourceOldProjectRelative, SourceNewProjectRelative, BufferMappings);
		if (!RewrittenJson) return std::unexpected("GLB JSON rewrite failed: " + RewrittenJson.error());

		const uint64_t PaddedJsonSize = (static_cast<uint64_t>(RewrittenJson->size()) + 3u) & ~uint64_t{ 3u };
		if (PaddedJsonSize > MaximumJsonBytes || PaddedJsonSize > std::numeric_limits<uint32_t>::max())
			return std::unexpected("Rewritten GLB JSON chunk exceeds its format length limit");
		const uint64_t NewFileSize = FileSize - Layout->Json.Length + PaddedJsonSize;
		if (NewFileSize > MaximumGlbBytes || NewFileSize > std::numeric_limits<uint32_t>::max())
			return std::unexpected("Rewritten GLB exceeds the bounded container size limit");

		std::array<std::byte, 12> Header{};
		if (!ReadRange(0, Header)) return std::unexpected("GLB header changed or could not be reread");
		if (ReadUInt32(Header, 0) != GlbMagic || ReadUInt32(Header, 4) != 2 || ReadUInt32(Header, 8) != FileSize)
			return std::unexpected("GLB header changed after its layout was inspected");
		WriteUInt32(Header, 8, static_cast<uint32_t>(NewFileSize));
		if (auto Result = WriteBytes(Write, Header); !Result) return std::unexpected("Could not stage GLB header: " + Result.error());
		uint64_t Written = Header.size();

		uint64_t Offset = 12;
		bool ReplacedJson = false;
		std::array<std::byte, CopyBlockBytes> Block{};
		while (Offset < FileSize)
		{
			std::array<std::byte, 8> ChunkHeader{};
			if (!ReadRange(Offset, ChunkHeader))
				return std::unexpected("GLB chunk header changed or could not be reread during relocation");
			const uint32_t ChunkLength = ReadUInt32(ChunkHeader, 0);
			const uint32_t ChunkType = ReadUInt32(ChunkHeader, 4);
			const uint64_t PayloadOffset = Offset + ChunkHeader.size();
			if (ChunkLength % 4 != 0 || PayloadOffset > FileSize || ChunkLength > FileSize - PayloadOffset)
				return std::unexpected("GLB chunk changed beyond its validated input bounds");

			if (ChunkType == GlbJsonChunk)
			{
				if (ReplacedJson || Offset != 12) return std::unexpected("GLB JSON chunk layout changed during relocation");
				ReplacedJson = true;
				WriteUInt32(ChunkHeader, 0, static_cast<uint32_t>(PaddedJsonSize));
				if (auto Result = WriteBytes(Write, ChunkHeader); !Result)
					return std::unexpected("Could not stage GLB JSON header: " + Result.error());
				Written += ChunkHeader.size();
				const auto JsonBytes = std::as_bytes(std::span(RewrittenJson->data(), RewrittenJson->size()));
				if (auto Result = WriteBytes(Write, JsonBytes); !Result)
					return std::unexpected("Could not stage rewritten GLB JSON: " + Result.error());
				Written += JsonBytes.size();
				const size_t Padding = static_cast<size_t>(PaddedJsonSize - RewrittenJson->size());
				if (Padding != 0)
				{
					const std::array<std::byte, 3> Spaces{ static_cast<std::byte>(' '), static_cast<std::byte>(' '),
						static_cast<std::byte>(' ') };
					if (auto Result = WriteBytes(Write, std::span(Spaces).first(Padding)); !Result)
						return std::unexpected("Could not stage GLB JSON padding: " + Result.error());
					Written += Padding;
				}
			}
			else
			{
				if (auto Result = WriteBytes(Write, ChunkHeader); !Result)
					return std::unexpected("Could not stage GLB chunk header: " + Result.error());
				Written += ChunkHeader.size();
				uint64_t Copied = 0;
				while (Copied < ChunkLength)
				{
					const size_t Count = static_cast<size_t>(std::min<uint64_t>(Block.size(), ChunkLength - Copied));
					if (!ReadRange(PayloadOffset + Copied, std::span(Block).first(Count)))
						return std::unexpected("GLB payload changed or could not be read during relocation");
					if (auto Result = WriteBytes(Write, std::span<const std::byte>(Block).first(Count)); !Result)
						return std::unexpected("Could not stage GLB payload: " + Result.error());
					Copied += Count;
					Written += Count;
				}
			}
			Offset = PayloadOffset + ChunkLength;
		}
		if (!ReplacedJson || Written != NewFileSize) return std::unexpected("GLB relocation did not produce the validated output length");
		return Written;
	}
} // namespace PulseForge::Assets::Detail
