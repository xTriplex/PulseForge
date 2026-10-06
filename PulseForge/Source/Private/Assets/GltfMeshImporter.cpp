#include "Core/PulseForgePCH.h"
#include "Assets/GltfMeshImporter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::json;
		using ByteVector = std::vector<std::byte>;
		constexpr uint64_t MaxContainerBytes = 512ull * 1024ull * 1024ull;
		constexpr uint64_t MaxJsonBytes = 64ull * 1024ull * 1024ull;
		constexpr uint64_t MaxDecodedBufferBytes = 512ull * 1024ull * 1024ull;
		constexpr uint64_t MaxVertexCount = 10'000'000;
		constexpr uint64_t MaxIndexCount = 30'000'000;
		constexpr uint32_t GlbMagic = 0x46546C67;
		constexpr uint32_t GlbJsonChunk = 0x4E4F534A;
		constexpr uint32_t GlbBinaryChunk = 0x004E4942;

		GltfMeshImportError MakeError(
			GltfMeshImportErrorCode Code,
			const std::filesystem::path& Path,
			std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		bool CheckedAdd(size_t First, size_t Second, size_t& Result)
		{
			if (Second > std::numeric_limits<size_t>::max() - First)
				return false;
			Result = First + Second;
			return true;
		}

		bool CheckedMultiply(size_t First, size_t Second, size_t& Result)
		{
			if (First != 0 && Second > std::numeric_limits<size_t>::max() / First)
				return false;
			Result = First * Second;
			return true;
		}

		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Utf8Path = Path.u8string();
			return { reinterpret_cast<const char*>(Utf8Path.data()), Utf8Path.size() };
		}

		bool HasWindowsAmbiguousTrailingCharacter(const std::filesystem::path& Path)
		{
			const std::u8string Component = Path.generic_u8string();
			return !Component.empty() && (Component.back() == u8'.' || Component.back() == u8' ');
		}

		bool ReadUInt32LE(std::span<const std::byte> Data, size_t Offset, uint32_t& Value)
		{
			if (Offset > Data.size() || Data.size() - Offset < sizeof(uint32_t))
				return false;

			Value = 0;
			for (size_t Byte = 0; Byte < sizeof(uint32_t); ++Byte)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Data[Offset + Byte])) << (Byte * 8);
			return true;
		}

		std::expected<ByteVector, GltfMeshImportError> ReadFile(
			const std::filesystem::path& Path,
			uint64_t MaximumBytes)
		{
			std::error_code FileError;
			const std::filesystem::file_status Status = std::filesystem::symlink_status(Path, FileError);
			if (FileError || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::FileReadFailed,
					Path,
					"glTF source or buffer is not a regular, non-symlink file"));
			}

			FileError.clear();
			const uintmax_t FileSize = std::filesystem::file_size(Path, FileError);
			if (FileError)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::FileReadFailed,
					Path,
					"Could not determine glTF file size: " + FileError.message()));
			}
			if (FileSize > MaximumBytes || FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()) ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::ResourceLimitExceeded,
					Path,
					"glTF source or buffer exceeds the importer size limit"));
			}

			ByteVector Data(static_cast<size_t>(FileSize));
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::FileReadFailed,
					Path,
					"Could not open glTF source or buffer"));
			}
			if (!Data.empty())
				Input.read(reinterpret_cast<char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
			if (!Input || Input.bad())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::FileReadFailed,
					Path,
					"Could not read glTF source or buffer completely"));
			}
			return Data;
		}

		struct ParsedContainer
		{
			std::string JsonText;
			// This span borrows from the FileBytes owned by ImportStaticPrimitive for the duration of the import.
			std::optional<std::span<const std::byte>> BinaryChunk;
		};

		std::expected<ParsedContainer, GltfMeshImportError> ParseGlb(
			std::span<const std::byte> FileBytes,
			const std::filesystem::path& Path)
		{
			uint32_t Magic = 0;
			uint32_t Version = 0;
			uint32_t DeclaredLength = 0;
			if (FileBytes.size() < 12 || !ReadUInt32LE(FileBytes, 0, Magic) ||
				!ReadUInt32LE(FileBytes, 4, Version) || !ReadUInt32LE(FileBytes, 8, DeclaredLength) ||
				Magic != GlbMagic || Version != 2 || DeclaredLength != FileBytes.size())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidDocument,
					Path,
					"GLB header must contain the glTF magic, version 2, and exact file length"));
			}

			ParsedContainer Container;
			bool FoundJson = false;
			size_t Offset = 12;
			size_t ChunkIndex = 0;
			while (Offset < FileBytes.size())
			{
				uint32_t ChunkLength = 0;
				uint32_t ChunkType = 0;
				if (!ReadUInt32LE(FileBytes, Offset, ChunkLength) ||
					!ReadUInt32LE(FileBytes, Offset + sizeof(uint32_t), ChunkType))
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidDocument,
						Path,
						"GLB contains a truncated chunk header"));
				}

				Offset += 8;
				if (ChunkLength % 4 != 0 || ChunkLength > FileBytes.size() - Offset)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidDocument,
						Path,
						"GLB chunk length is misaligned or extends beyond the file"));
				}

				const auto Chunk = FileBytes.subspan(Offset, ChunkLength);
				if (!FoundJson)
				{
					if (ChunkType != GlbJsonChunk)
					{
						return std::unexpected(MakeError(
							GltfMeshImportErrorCode::InvalidDocument,
							Path,
							"The first GLB chunk must contain JSON"));
					}
					if (Chunk.size() > MaxJsonBytes)
					{
						return std::unexpected(MakeError(
							GltfMeshImportErrorCode::ResourceLimitExceeded,
							Path,
							"GLB JSON chunk exceeds the importer limit"));
					}
					Container.JsonText.assign(reinterpret_cast<const char*>(Chunk.data()), Chunk.size());
					FoundJson = true;
				}
				else if (ChunkType == GlbJsonChunk)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidDocument,
						Path,
						"GLB contains more than one JSON chunk"));
				}
				else if (ChunkType == GlbBinaryChunk)
				{
					if (Container.BinaryChunk || ChunkIndex != 1)
					{
						return std::unexpected(MakeError(
							GltfMeshImportErrorCode::InvalidDocument,
							Path,
							"GLB binary chunk must appear once, immediately after the JSON chunk"));
					}
					Container.BinaryChunk = Chunk;
				}
				Offset += ChunkLength;
				++ChunkIndex;
			}

			if (!FoundJson)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidDocument,
					Path,
					"GLB does not contain a JSON chunk"));
			}

			while (!Container.JsonText.empty())
			{
				const unsigned char Character = static_cast<unsigned char>(Container.JsonText.back());
				if (Character != '\0' && !std::isspace(Character))
					break;
				Container.JsonText.pop_back();
			}
			return Container;
		}

		int DecodeBase64Character(char Character)
		{
			if (Character >= 'A' && Character <= 'Z')
				return Character - 'A';
			if (Character >= 'a' && Character <= 'z')
				return Character - 'a' + 26;
			if (Character >= '0' && Character <= '9')
				return Character - '0' + 52;
			if (Character == '+')
				return 62;
			if (Character == '/')
				return 63;
			return -1;
		}

		std::expected<ByteVector, GltfMeshImportError> DecodeBase64(
			std::string_view Encoded,
			const std::filesystem::path& Path)
		{
			if (Encoded.empty() || Encoded.size() % 4 != 0 || Encoded.size() > MaxDecodedBufferBytes * 2)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidBuffer,
					Path,
					"Embedded glTF buffer has invalid base64 length"));
			}

			ByteVector Decoded;
			Decoded.reserve((Encoded.size() / 4) * 3);
			for (size_t Offset = 0; Offset < Encoded.size(); Offset += 4)
			{
				const bool IsLast = Offset + 4 == Encoded.size();
				const char C0 = Encoded[Offset];
				const char C1 = Encoded[Offset + 1];
				const char C2 = Encoded[Offset + 2];
				const char C3 = Encoded[Offset + 3];
				const int V0 = DecodeBase64Character(C0);
				const int V1 = DecodeBase64Character(C1);
				const int V2 = C2 == '=' ? 0 : DecodeBase64Character(C2);
				const int V3 = C3 == '=' ? 0 : DecodeBase64Character(C3);
				const bool Pad2 = C2 == '=';
				const bool Pad3 = C3 == '=';
				if (V0 < 0 || V1 < 0 || V2 < 0 || V3 < 0 ||
					(Pad2 && (!Pad3 || !IsLast || (V1 & 0x0f) != 0)) ||
					(Pad3 && (!IsLast || (!Pad2 && (V2 & 0x03) != 0))))
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidBuffer,
						Path,
						"Embedded glTF buffer contains invalid base64 data"));
				}

				Decoded.push_back(static_cast<std::byte>((V0 << 2) | (V1 >> 4)));
				if (!Pad2)
					Decoded.push_back(static_cast<std::byte>(((V1 & 0x0f) << 4) | (V2 >> 2)));
				if (!Pad3)
					Decoded.push_back(static_cast<std::byte>(((V2 & 0x03) << 6) | V3));
			}
			return Decoded;
		}

		std::expected<std::string, GltfMeshImportError> DecodeRelativeUri(
			std::string_view Uri,
			const std::filesystem::path& Path)
		{
			std::string Decoded;
			Decoded.reserve(Uri.size());
			for (size_t Index = 0; Index < Uri.size(); ++Index)
			{
				if (Uri[Index] != '%')
				{
					Decoded.push_back(Uri[Index]);
					continue;
				}
				if (Index + 2 >= Uri.size())
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidBuffer,
						Path,
						"External glTF buffer URI contains an incomplete percent escape"));
				}

				const auto HexValue = [](char Character) -> int
				{
					if (Character >= '0' && Character <= '9')
						return Character - '0';
					if (Character >= 'a' && Character <= 'f')
						return Character - 'a' + 10;
					if (Character >= 'A' && Character <= 'F')
						return Character - 'A' + 10;
					return -1;
				};
				const int High = HexValue(Uri[Index + 1]);
				const int Low = HexValue(Uri[Index + 2]);
				if (High < 0 || Low < 0)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidBuffer,
						Path,
						"External glTF buffer URI contains an invalid percent escape"));
				}
				const char DecodedCharacter = static_cast<char>((High << 4) | Low);
				if (DecodedCharacter == '\0' || DecodedCharacter == '\\' || DecodedCharacter == ':' ||
					DecodedCharacter == '?' || DecodedCharacter == '#')
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidBuffer,
						Path,
						"External glTF buffer URI contains a forbidden path character"));
				}
				Decoded.push_back(DecodedCharacter);
				Index += 2;
			}

			if (Decoded.empty() || Decoded.find('\0') != std::string::npos || Decoded.front() == '/' ||
				Decoded.find('\\') != std::string::npos ||
				Decoded.find(':') != std::string::npos || Decoded.find('?') != std::string::npos ||
				Decoded.find('#') != std::string::npos)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidBuffer,
					Path,
					"External glTF buffer URI must be a relative file path"));
			}
			return Decoded;
		}

		std::expected<std::filesystem::path, GltfMeshImportError> ValidateManagedFilePath(
			const std::filesystem::path& AssetRoot,
			const std::filesystem::path& RelativePath)
		{
			const std::filesystem::path Normalized = RelativePath.lexically_normal();
			if (Normalized.empty() || Normalized.is_absolute() || Normalized.has_root_name() ||
				Normalized.has_root_directory() || Normalized.begin() == Normalized.end() || *Normalized.begin() != "Assets")
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidProjectPath,
					RelativePath,
					"Asset registry path is not project-relative under Assets"));
			}
			for (const auto& Component : Normalized)
			{
				if (Component == "." || Component == "..")
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidProjectPath,
						RelativePath,
						"Asset registry path contains a traversal component"));
				}
				if (HasWindowsAmbiguousTrailingCharacter(Component))
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidProjectPath,
						RelativePath,
						"Managed asset path contains a component that is ambiguous on Windows"));
				}
			}

			std::error_code FileError;
			const std::filesystem::file_status AssetRootStatus = std::filesystem::symlink_status(AssetRoot, FileError);
			if (FileError || std::filesystem::is_symlink(AssetRootStatus) || !std::filesystem::is_directory(AssetRootStatus))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidProjectPath,
					AssetRoot,
					"Project Assets root must be an accessible, non-symlink directory"));
			}

			const std::filesystem::path RelativeToAssets = Normalized.lexically_relative("Assets");
			std::filesystem::path Current = AssetRoot;
			for (auto Component = RelativeToAssets.begin(); Component != RelativeToAssets.end(); ++Component)
			{
				Current /= *Component;
				FileError.clear();
				const std::filesystem::file_status Status = std::filesystem::symlink_status(Current, FileError);
				if (FileError || std::filesystem::is_symlink(Status))
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidProjectPath,
						Current,
						"Managed asset and buffer paths must not traverse missing entries or symbolic links"));
				}
				const bool IsFinal = std::next(Component) == RelativeToAssets.end();
				if ((!IsFinal && !std::filesystem::is_directory(Status)) ||
					(IsFinal && !std::filesystem::is_regular_file(Status)))
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidProjectPath,
						Current,
						"Managed asset path contains an invalid filesystem entry"));
				}
			}
			return Current;
		}

		std::expected<uint64_t, GltfMeshImportError> ReadUnsignedInteger(
			const Json* Value,
			const std::filesystem::path& Path,
			std::string_view Name)
		{
			if (Value == nullptr || (!Value->is_number_unsigned() && !Value->is_number_integer()))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidDocument,
					Path,
				"Expected non-negative integer field '" + std::string(Name) + "'"));
			}
			if (Value->is_number_unsigned())
				return Value->get<uint64_t>();
			const int64_t SignedValue = Value->get<int64_t>();
			if (SignedValue < 0)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidDocument,
					Path,
					"Field '" + std::string(Name) + "' must not be negative"));
			}
			return static_cast<uint64_t>(SignedValue);
		}

		const Json* FindField(const Json& Object, std::string_view Name)
		{
			if (!Object.is_object())
				return nullptr;
			const auto Field = Object.find(Name);
			return Field == Object.end() ? nullptr : &*Field;
		}

		bool HasNamedExtension(const Json& Object, std::string_view Name)
		{
			const Json* Extensions = FindField(Object, "extensions");
			return Extensions != nullptr && Extensions->is_object() && FindField(*Extensions, Name) != nullptr;
		}

		struct LoadedBufferData
		{
			std::vector<ByteVector> OwnedBuffers;
			// Views borrow from OwnedBuffers or the source GLB container; both owners outlive accessor decoding.
			std::vector<std::span<const std::byte>> Views;
		};

		std::expected<LoadedBufferData, GltfMeshImportError> LoadBuffers(
			const Json& Document,
			const std::filesystem::path& SourcePath,
			const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& AssetRoot,
			const std::optional<std::span<const std::byte>>& GlbBinaryChunk)
		{
			const Json* Buffers = FindField(Document, "buffers");
			if (Buffers == nullptr || !Buffers->is_array() || Buffers->empty())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidBuffer,
					SourcePath,
					"glTF document requires at least one buffer"));
			}

			LoadedBufferData LoadedBuffers;
			LoadedBuffers.OwnedBuffers.reserve(Buffers->size());
			LoadedBuffers.Views.reserve(Buffers->size());
			uint64_t TotalBufferBytes = 0;
			bool UsedGlbBinaryChunk = false;
			for (size_t BufferOrdinal = 0; BufferOrdinal < Buffers->size(); ++BufferOrdinal)
			{
				const Json& Buffer = (*Buffers)[BufferOrdinal];
				const auto DeclaredLength = ReadUnsignedInteger(FindField(Buffer, "byteLength"), SourcePath, "buffers.byteLength");
				if (!DeclaredLength || *DeclaredLength == 0 || *DeclaredLength > MaxDecodedBufferBytes ||
					TotalBufferBytes > MaxDecodedBufferBytes - *DeclaredLength)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::ResourceLimitExceeded,
						SourcePath,
						"glTF buffer length is missing, zero, or exceeds the importer limit"));
				}
				TotalBufferBytes += *DeclaredLength;

				const Json* Uri = FindField(Buffer, "uri");
				std::span<const std::byte> BufferData;
				ByteVector OwnedData;
				if (Uri == nullptr)
				{
					if (BufferOrdinal != 0 || !GlbBinaryChunk || UsedGlbBinaryChunk ||
						GlbBinaryChunk->size() < *DeclaredLength ||
						GlbBinaryChunk->size() - static_cast<size_t>(*DeclaredLength) > 3)
					{
						return std::unexpected(MakeError(
							GltfMeshImportErrorCode::InvalidBuffer,
							SourcePath,
							"A buffer without a URI requires an adequate GLB binary chunk"));
					}
					UsedGlbBinaryChunk = true;
					BufferData = GlbBinaryChunk->first(static_cast<size_t>(*DeclaredLength));
				}
				else if (!Uri->is_string())
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidBuffer,
						SourcePath,
						"glTF buffer URI must be a string"));
				}
				else
				{
					const std::string UriText = Uri->get<std::string>();
					if (UriText.starts_with("data:"))
					{
						constexpr std::string_view OctetStreamPrefix = "data:application/octet-stream;base64,";
						constexpr std::string_view GltfBufferPrefix = "data:application/gltf-buffer;base64,";
						std::string_view Encoded;
						if (UriText.starts_with(OctetStreamPrefix))
							Encoded = std::string_view(UriText).substr(OctetStreamPrefix.size());
						else if (UriText.starts_with(GltfBufferPrefix))
							Encoded = std::string_view(UriText).substr(GltfBufferPrefix.size());
						else
						{
							return std::unexpected(MakeError(
								GltfMeshImportErrorCode::UnsupportedFeature,
								SourcePath,
								"Only base64 application/octet-stream and application/gltf-buffer data URIs are supported"));
						}
						auto Decoded = DecodeBase64(Encoded, SourcePath);
						if (!Decoded)
							return std::unexpected(std::move(Decoded.error()));
						OwnedData = std::move(*Decoded);
					}
					else
					{
						auto DecodedUri = DecodeRelativeUri(UriText, SourcePath);
						if (!DecodedUri)
							return std::unexpected(std::move(DecodedUri.error()));
						const std::filesystem::path UriPath = std::filesystem::u8path(*DecodedUri);
						if (UriPath.is_absolute() || UriPath.has_root_name() || UriPath.has_root_directory())
						{
							return std::unexpected(MakeError(
								GltfMeshImportErrorCode::InvalidBuffer,
								SourcePath,
								"External glTF buffer URI must be relative"));
						}
						for (const auto& Component : UriPath)
						{
							if (Component == "." || Component == "..")
								continue;
							if (HasWindowsAmbiguousTrailingCharacter(Component))
							{
								return std::unexpected(MakeError(
									GltfMeshImportErrorCode::InvalidBuffer,
									SourcePath,
									"External glTF buffer URI contains a path component that is ambiguous on Windows"));
							}
						}
						const std::filesystem::path BufferPath = (SourcePath.parent_path() / UriPath).lexically_normal();
						const std::filesystem::path RelativeBufferPath = BufferPath.lexically_relative(ProjectRoot);
						auto ManagedBufferPath = ValidateManagedFilePath(AssetRoot, RelativeBufferPath);
						if (!ManagedBufferPath)
							return std::unexpected(std::move(ManagedBufferPath.error()));
						auto ReadBuffer = ReadFile(*ManagedBufferPath, MaxDecodedBufferBytes);
						if (!ReadBuffer)
							return std::unexpected(std::move(ReadBuffer.error()));
						OwnedData = std::move(*ReadBuffer);
					}

					if (OwnedData.size() < *DeclaredLength)
					{
						return std::unexpected(MakeError(
							GltfMeshImportErrorCode::InvalidBuffer,
							SourcePath,
							"glTF buffer data is shorter than its declared byteLength"));
					}
					OwnedData.resize(static_cast<size_t>(*DeclaredLength));
					LoadedBuffers.OwnedBuffers.push_back(std::move(OwnedData));
					BufferData = LoadedBuffers.OwnedBuffers.back();
				}
				LoadedBuffers.Views.push_back(BufferData);
			}
			if (GlbBinaryChunk && !UsedGlbBinaryChunk)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidBuffer,
					SourcePath,
					"GLB binary chunk is present but no URI-less buffer references it"));
			}
			return LoadedBuffers;
		}

		struct AccessorView
		{
			const std::byte* Data = nullptr;
			size_t Count = 0;
			size_t ComponentCount = 0;
			size_t ComponentSize = 0;
			size_t Stride = 0;
			uint32_t ComponentType = 0;
			bool Normalized = false;
		};

		std::expected<AccessorView, GltfMeshImportError> ResolveAccessor(
			const Json& Document,
			const LoadedBufferData& Buffers,
			uint64_t AccessorIndex,
			const std::filesystem::path& Path)
		{
			const Json* Accessors = FindField(Document, "accessors");
			if (Accessors == nullptr || !Accessors->is_array() || AccessorIndex >= Accessors->size())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"Mesh attribute refers to an accessor outside the glTF accessor array"));
			}
			const Json& Accessor = (*Accessors)[static_cast<size_t>(AccessorIndex)];
			if (!Accessor.is_object())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor must be an object"));
			}
			if (FindField(Accessor, "sparse") != nullptr)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					Path,
					"Sparse glTF accessors are not supported by this importer"));
			}
			if (const Json* Extensions = FindField(Accessor, "extensions"); Extensions != nullptr &&
				(!Extensions->is_object() || !Extensions->empty()))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					Path,
					"Accessor extensions are not supported by this importer"));
			}

			const auto ViewIndex = ReadUnsignedInteger(FindField(Accessor, "bufferView"), Path, "accessors.bufferView");
			const auto Count = ReadUnsignedInteger(FindField(Accessor, "count"), Path, "accessors.count");
			const auto ComponentTypeValue = ReadUnsignedInteger(
				FindField(Accessor, "componentType"), Path, "accessors.componentType");
			const Json* TypeValue = FindField(Accessor, "type");
			if (!ViewIndex || !Count || !ComponentTypeValue || TypeValue == nullptr || !TypeValue->is_string() ||
				*Count == 0 || *Count > MaxIndexCount || *ComponentTypeValue > std::numeric_limits<uint32_t>::max())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor has missing, invalid, or excessive dimensions"));
			}

			size_t ComponentCount = 0;
			const std::string Type = TypeValue->get<std::string>();
			if (Type == "SCALAR") ComponentCount = 1;
			else if (Type == "VEC2") ComponentCount = 2;
			else if (Type == "VEC3") ComponentCount = 3;
			else if (Type == "VEC4") ComponentCount = 4;
			else
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					Path,
					"Matrix and unknown glTF accessor types are not supported for static triangle geometry"));
			}

			size_t ComponentSize = 0;
			switch (*ComponentTypeValue)
			{
			case 5121: ComponentSize = 1; break;
			case 5123: ComponentSize = 2; break;
			case 5125:
			case 5126: ComponentSize = 4; break;
			default:
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					Path,
					"glTF accessor component type is not supported"));
			}

			const Json* BufferViews = FindField(Document, "bufferViews");
			if (BufferViews == nullptr || !BufferViews->is_array() || *ViewIndex >= BufferViews->size())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor refers to a bufferView outside the document"));
			}
			const Json& BufferView = (*BufferViews)[static_cast<size_t>(*ViewIndex)];
			if (HasNamedExtension(BufferView, "EXT_meshopt_compression"))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					Path,
					"EXT_meshopt_compression bufferViews are not supported by this importer"));
			}
			const auto BufferIndex = ReadUnsignedInteger(FindField(BufferView, "buffer"), Path, "bufferViews.buffer");
			const auto ViewLength = ReadUnsignedInteger(FindField(BufferView, "byteLength"), Path, "bufferViews.byteLength");
			if (!BufferIndex || !ViewLength ||
				*BufferIndex >= Buffers.Views.size() || *ViewLength == 0 ||
				*ViewLength > std::numeric_limits<size_t>::max())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidBuffer,
					Path,
					"glTF bufferView has invalid buffer or byteLength"));
			}
			const auto ViewOffsetValue = ReadUnsignedInteger(FindField(BufferView, "byteOffset"), Path, "bufferViews.byteOffset");
			if (ViewOffsetValue && *ViewOffsetValue > std::numeric_limits<size_t>::max())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidBuffer,
					Path,
					"glTF bufferView byteOffset exceeds the platform address range"));
			}
			const size_t ViewOffset = ViewOffsetValue ? static_cast<size_t>(*ViewOffsetValue) : 0;
			if (!ViewOffsetValue && FindField(BufferView, "byteOffset") != nullptr)
				return std::unexpected(std::move(ViewOffsetValue.error()));

			size_t ViewEnd = 0;
			if (!CheckedAdd(ViewOffset, static_cast<size_t>(*ViewLength), ViewEnd) ||
				ViewEnd > Buffers.Views[static_cast<size_t>(*BufferIndex)].size())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidBuffer,
					Path,
					"glTF bufferView range exceeds its buffer"));
			}

			size_t ElementSize = 0;
			if (!CheckedMultiply(ComponentCount, ComponentSize, ElementSize))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor element size overflows"));
			}
			const auto AccessorOffsetValue = ReadUnsignedInteger(FindField(Accessor, "byteOffset"), Path, "accessors.byteOffset");
			if (AccessorOffsetValue && *AccessorOffsetValue > std::numeric_limits<size_t>::max())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor byteOffset exceeds the platform address range"));
			}
			const size_t AccessorOffset = AccessorOffsetValue ? static_cast<size_t>(*AccessorOffsetValue) : 0;
			if (!AccessorOffsetValue && FindField(Accessor, "byteOffset") != nullptr)
				return std::unexpected(std::move(AccessorOffsetValue.error()));

			size_t Stride = ElementSize;
			if (const Json* StrideValue = FindField(BufferView, "byteStride"))
			{
				const auto ParsedStride = ReadUnsignedInteger(StrideValue, Path, "bufferViews.byteStride");
				if (!ParsedStride || *ParsedStride > std::numeric_limits<size_t>::max())
					return std::unexpected(MakeError(GltfMeshImportErrorCode::InvalidBuffer, Path, "glTF byteStride is invalid"));
				Stride = static_cast<size_t>(*ParsedStride);
				if (Stride < 4 || Stride > 252 || Stride % 4 != 0 || Stride < ElementSize)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidBuffer,
						Path,
						"glTF byteStride must be between 4 and 252, aligned to four bytes, and fit the element"));
				}
			}

			size_t AbsoluteOffset = 0;
			if (*Count > std::numeric_limits<size_t>::max() || AccessorOffset % ComponentSize != 0 ||
				!CheckedAdd(ViewOffset, AccessorOffset, AbsoluteOffset) || AbsoluteOffset % ComponentSize != 0)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor count or alignment is invalid"));
			}
			size_t StridedBytes = 0;
			size_t RequiredEnd = 0;
			if (!CheckedMultiply(Stride, static_cast<size_t>(*Count - 1), StridedBytes) ||
				!CheckedAdd(AccessorOffset, StridedBytes, RequiredEnd) ||
				!CheckedAdd(RequiredEnd, ElementSize, RequiredEnd) || RequiredEnd > *ViewLength)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor byte range exceeds its bufferView"));
			}

			const Json* NormalizedValue = FindField(Accessor, "normalized");
			if (NormalizedValue != nullptr && !NormalizedValue->is_boolean())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF accessor normalized flag must be boolean"));
			}
			const bool Normalized = NormalizedValue != nullptr && NormalizedValue->get<bool>();
			return AccessorView{
				Buffers.Views[static_cast<size_t>(*BufferIndex)].data() + AbsoluteOffset,
				static_cast<size_t>(*Count),
				ComponentCount,
				ComponentSize,
				Stride,
				static_cast<uint32_t>(*ComponentTypeValue),
				Normalized
			};
		}

		std::expected<float, GltfMeshImportError> ReadFloatComponent(
			const AccessorView& Accessor,
			size_t Element,
			size_t Component,
			const std::filesystem::path& Path)
		{
			if (Accessor.ComponentType != 5126 || Accessor.ComponentSize != sizeof(uint32_t) ||
				Element >= Accessor.Count || Component >= Accessor.ComponentCount)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF floating-point attribute has an unsupported accessor type or range"));
			}
			const size_t Offset = Element * Accessor.Stride + Component * sizeof(uint32_t);
			uint32_t Bits = 0;
			const std::span<const std::byte> Data(Accessor.Data + Offset, sizeof(uint32_t));
			if (!ReadUInt32LE(Data, 0, Bits))
				return std::unexpected(MakeError(GltfMeshImportErrorCode::InvalidAccessor, Path, "Could not read glTF float"));
			const float Value = std::bit_cast<float>(Bits);
			if (!std::isfinite(Value))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF geometry contains a non-finite floating-point value"));
			}
			return Value;
		}

		std::expected<uint32_t, GltfMeshImportError> ReadIndex(
			const AccessorView& Accessor,
			size_t Element,
			const std::filesystem::path& Path)
		{
			if (Accessor.ComponentCount != 1 || Accessor.Normalized || Element >= Accessor.Count ||
				(Accessor.ComponentType != 5121 && Accessor.ComponentType != 5123 && Accessor.ComponentType != 5125))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidAccessor,
					Path,
					"glTF indices must use an unsigned, unnormalized scalar accessor"));
			}

			const size_t Offset = Element * Accessor.Stride;
			const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Accessor.Data + Offset);
			if (Accessor.ComponentType == 5121)
				return Bytes[0];
			if (Accessor.ComponentType == 5123)
				return static_cast<uint32_t>(Bytes[0]) | (static_cast<uint32_t>(Bytes[1]) << 8);
			uint32_t Value = 0;
			const std::span<const std::byte> Data(Accessor.Data + Offset, sizeof(uint32_t));
			if (!ReadUInt32LE(Data, 0, Value))
				return std::unexpected(MakeError(GltfMeshImportErrorCode::InvalidAccessor, Path, "Could not read glTF index"));
			return Value;
		}

		std::expected<AccessorView, GltfMeshImportError> ResolveFloatAttribute(
			const Json& Document,
			const LoadedBufferData& Buffers,
			const Json& Attributes,
			std::string_view Name,
			uint64_t VertexCount,
			std::span<const size_t> AllowedComponentCounts,
			const std::filesystem::path& Path)
		{
			const Json* AccessorIndexValue = FindField(Attributes, Name);
			const auto AccessorIndex = ReadUnsignedInteger(AccessorIndexValue, Path, Name);
			if (!AccessorIndex)
				return std::unexpected(std::move(AccessorIndex.error()));
			auto Accessor = ResolveAccessor(Document, Buffers, *AccessorIndex, Path);
			if (!Accessor)
				return std::unexpected(std::move(Accessor.error()));
			if (Accessor->ComponentType != 5126 || Accessor->Normalized || Accessor->Count != VertexCount ||
				std::find(AllowedComponentCounts.begin(), AllowedComponentCounts.end(), Accessor->ComponentCount) ==
					AllowedComponentCounts.end())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					Path,
					"This importer requires matching vertex counts and non-normalized float attribute accessors for " +
						std::string(Name)));
			}
			return Accessor;
		}

		bool HasExtension(const std::filesystem::path& Path, std::string_view Extension)
		{
			std::string Actual = PathToUtf8(Path.extension());
			std::transform(Actual.begin(), Actual.end(), Actual.begin(), [](unsigned char Character)
			{
				return static_cast<char>(std::tolower(Character));
			});
			return Actual == Extension;
		}
	}

	MeshDesc ImportedGltfMesh::GetMeshDescription() const
	{
		MeshDesc Description;
		Description.VertexLayout.Stride = sizeof(GltfMeshVertex);
		Description.VertexLayout.Attributes = {
			{ VertexSemantic::Position, VertexFormat::Float3, offsetof(GltfMeshVertex, Position) },
			{ VertexSemantic::Color, VertexFormat::Float3, offsetof(GltfMeshVertex, Color) },
			{ VertexSemantic::TexCoord, VertexFormat::Float2, offsetof(GltfMeshVertex, TexCoord) }
		};
		Description.VertexData = std::as_bytes(std::span(Vertices));
		Description.Indices = Indices;
		Description.DebugName = Name;
		return Description;
	}

	std::expected<ImportedGltfMesh, GltfMeshImportError> GltfMeshImporter::ImportStaticPrimitive(
		const AssetID& Asset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		uint32_t MeshIndex,
		uint32_t PrimitiveIndex)
	{
		const auto Record = Registry.Find(Asset);
		if (!Record)
		{
			return std::unexpected(MakeError(
				GltfMeshImportErrorCode::AssetNotFound,
				{},
				"Asset UUID is not present in the project asset registry: " + Asset.ToString()));
		}
		if (ProjectRoot.empty())
		{
			return std::unexpected(MakeError(
				GltfMeshImportErrorCode::InvalidProjectPath,
				ProjectRoot,
				"Project root must not be empty"));
		}

		try
		{
			std::error_code FileError;
			const std::filesystem::path AbsoluteProjectRoot = std::filesystem::absolute(ProjectRoot, FileError).lexically_normal();
			if (FileError)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidProjectPath,
					ProjectRoot,
					"Could not resolve project root: " + FileError.message()));
			}
			const std::filesystem::path AssetRoot = AbsoluteProjectRoot / "Assets";
			auto Source = ValidateManagedFilePath(AssetRoot, Record->ProjectRelativePath);
			if (!Source)
				return std::unexpected(std::move(Source.error()));
			if (!HasExtension(*Source, ".gltf") && !HasExtension(*Source, ".glb"))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFileType,
					*Source,
					"Static mesh import requires a .gltf or .glb source asset"));
			}

			auto FileBytesResult = ReadFile(*Source, HasExtension(*Source, ".glb") ? MaxContainerBytes : MaxJsonBytes);
			if (!FileBytesResult)
				return std::unexpected(std::move(FileBytesResult.error()));
			ByteVector FileBytes = std::move(*FileBytesResult);
			ParsedContainer Container;
			if (HasExtension(*Source, ".glb"))
			{
				auto Parsed = ParseGlb(FileBytes, *Source);
				if (!Parsed)
					return std::unexpected(std::move(Parsed.error()));
				Container = std::move(*Parsed);
				if (Container.JsonText.size() > MaxJsonBytes)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::ResourceLimitExceeded,
						*Source,
						"glTF JSON chunk exceeds the importer limit"));
				}
			}
			else
			{
				if (FileBytes.size() > MaxJsonBytes)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::ResourceLimitExceeded,
						*Source,
						"glTF JSON source exceeds the importer limit"));
				}
				if (!FileBytes.empty())
					Container.JsonText.assign(reinterpret_cast<const char*>(FileBytes.data()), FileBytes.size());
			}

			Json Document;
			try
			{
				Document = Json::parse(Container.JsonText);
			}
			catch (const nlohmann::json::exception& Exception)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidDocument,
					*Source,
					std::string("glTF JSON is malformed: ") + Exception.what()));
			}

			const Json* AssetDocument = FindField(Document, "asset");
			const Json* Version = AssetDocument == nullptr ? nullptr : FindField(*AssetDocument, "version");
			if (Version == nullptr || !Version->is_string() || Version->get<std::string>() != "2.0")
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidDocument,
					*Source,
					"glTF asset.version must be \"2.0\""));
			}
			if (const Json* RequiredExtensions = FindField(Document, "extensionsRequired"); RequiredExtensions != nullptr)
			{
				if (!RequiredExtensions->is_array())
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidDocument,
						*Source,
						"glTF extensionsRequired must be an array"));
				}
				for (const Json& Extension : *RequiredExtensions)
				{
					if (!Extension.is_string())
					{
						return std::unexpected(MakeError(
							GltfMeshImportErrorCode::InvalidDocument,
							*Source,
							"glTF extensionsRequired entries must be strings"));
					}
					const std::string ExtensionName = Extension.get<std::string>();
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::UnsupportedFeature,
						*Source,
						"Required glTF extension is not supported by the static mesh importer: " + ExtensionName));
				}
			}

			auto Buffers = LoadBuffers(Document, *Source, AbsoluteProjectRoot, AssetRoot, Container.BinaryChunk);
			if (!Buffers)
				return std::unexpected(std::move(Buffers.error()));

			const Json* Meshes = FindField(Document, "meshes");
			if (Meshes == nullptr || !Meshes->is_array() || MeshIndex >= Meshes->size())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidMesh,
					*Source,
					"Requested glTF mesh index is outside the meshes array"));
			}
			const Json& Mesh = (*Meshes)[MeshIndex];
			const Json* Primitives = FindField(Mesh, "primitives");
			if (Primitives == nullptr || !Primitives->is_array() || PrimitiveIndex >= Primitives->size())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidMesh,
					*Source,
					"Requested primitive index is outside the mesh primitives array"));
			}
			const Json& Primitive = (*Primitives)[PrimitiveIndex];
			if (HasNamedExtension(Primitive, "KHR_draco_mesh_compression"))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					*Source,
					"KHR_draco_mesh_compression primitives are not supported by this importer"));
			}
			if (const Json* Targets = FindField(Primitive, "targets"); Targets != nullptr &&
				(!Targets->is_array() || !Targets->empty()))
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					*Source,
					"Morph target geometry is not supported by this static mesh importer"));
			}
			const Json* ModeValue = FindField(Primitive, "mode");
			if (ModeValue != nullptr)
			{
				const auto Mode = ReadUnsignedInteger(ModeValue, *Source, "primitives.mode");
				if (!Mode || *Mode != 4)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::UnsupportedFeature,
						*Source,
						"Only glTF triangle-list primitives are supported"));
				}
			}

			const Json* Attributes = FindField(Primitive, "attributes");
			if (Attributes == nullptr || !Attributes->is_object())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidMesh,
					*Source,
					"glTF primitive requires an attributes object"));
			}
			const Json* PositionIndexValue = FindField(*Attributes, "POSITION");
			const auto PositionIndex = ReadUnsignedInteger(PositionIndexValue, *Source, "attributes.POSITION");
			if (!PositionIndex)
				return std::unexpected(std::move(PositionIndex.error()));
			auto Positions = ResolveAccessor(Document, *Buffers, *PositionIndex, *Source);
			if (!Positions)
				return std::unexpected(std::move(Positions.error()));
			if (Positions->ComponentType != 5126 || Positions->Normalized || Positions->ComponentCount != 3 ||
				Positions->Count > MaxVertexCount || Positions->Count > std::numeric_limits<uint32_t>::max())
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::UnsupportedFeature,
					*Source,
					"POSITION must use a bounded, dense, non-normalized float VEC3 accessor"));
			}

			ImportedGltfMesh Imported;
			if (const Json* Name = FindField(Mesh, "name"); Name != nullptr && Name->is_string())
				Imported.Name = Name->get<std::string>();
			if (Imported.Name.empty())
				Imported.Name = PathToUtf8(Source->stem());
			Imported.Vertices.resize(Positions->Count);
			for (size_t VertexIndex = 0; VertexIndex < Positions->Count; ++VertexIndex)
			{
				for (size_t Component = 0; Component < 3; ++Component)
				{
					auto Value = ReadFloatComponent(*Positions, VertexIndex, Component, *Source);
					if (!Value)
						return std::unexpected(std::move(Value.error()));
					Imported.Vertices[VertexIndex].Position[Component] = *Value;
				}
			}

			constexpr std::array<size_t, 2> TexCoordCounts = { 2, 2 };
			if (FindField(*Attributes, "TEXCOORD_0") != nullptr)
			{
				auto TexCoords = ResolveFloatAttribute(
					Document, *Buffers, *Attributes, "TEXCOORD_0", Positions->Count, TexCoordCounts, *Source);
				if (!TexCoords)
					return std::unexpected(std::move(TexCoords.error()));
				for (size_t VertexIndex = 0; VertexIndex < Positions->Count; ++VertexIndex)
				{
					for (size_t Component = 0; Component < 2; ++Component)
					{
						auto Value = ReadFloatComponent(*TexCoords, VertexIndex, Component, *Source);
						if (!Value)
							return std::unexpected(std::move(Value.error()));
						Imported.Vertices[VertexIndex].TexCoord[Component] = *Value;
					}
				}
			}

			constexpr std::array<size_t, 2> ColorCounts = { 3, 4 };
			if (FindField(*Attributes, "COLOR_0") != nullptr)
			{
				auto Colors = ResolveFloatAttribute(
					Document, *Buffers, *Attributes, "COLOR_0", Positions->Count, ColorCounts, *Source);
				if (!Colors)
					return std::unexpected(std::move(Colors.error()));
				for (size_t VertexIndex = 0; VertexIndex < Positions->Count; ++VertexIndex)
				{
					for (size_t Component = 0; Component < 3; ++Component)
					{
						auto Value = ReadFloatComponent(*Colors, VertexIndex, Component, *Source);
						if (!Value)
							return std::unexpected(std::move(Value.error()));
						Imported.Vertices[VertexIndex].Color[Component] = *Value;
					}
				}
			}

			if (const Json* IndicesIndexValue = FindField(Primitive, "indices"); IndicesIndexValue != nullptr)
			{
				const auto IndicesIndex = ReadUnsignedInteger(IndicesIndexValue, *Source, "primitives.indices");
				if (!IndicesIndex)
					return std::unexpected(std::move(IndicesIndex.error()));
				auto IndexAccessor = ResolveAccessor(Document, *Buffers, *IndicesIndex, *Source);
				if (!IndexAccessor)
					return std::unexpected(std::move(IndexAccessor.error()));
				if (IndexAccessor->Count > MaxIndexCount || IndexAccessor->Count > std::numeric_limits<uint32_t>::max())
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::ResourceLimitExceeded,
						*Source,
						"glTF index count exceeds the importer limit"));
				}
				const Json* IndexBufferViews = FindField(Document, "bufferViews");
				const Json& Accessor = (*FindField(Document, "accessors"))[static_cast<size_t>(*IndicesIndex)];
				const auto IndexViewIndex = ReadUnsignedInteger(FindField(Accessor, "bufferView"), *Source, "indices.bufferView");
				if (!IndexViewIndex || IndexBufferViews == nullptr || !IndexBufferViews->is_array() ||
					*IndexViewIndex >= IndexBufferViews->size() ||
					FindField((*IndexBufferViews)[static_cast<size_t>(*IndexViewIndex)], "byteStride") != nullptr)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::InvalidAccessor,
						*Source,
						"glTF index accessors must use a valid bufferView without byteStride"));
				}
				Imported.Indices.reserve(IndexAccessor->Count);
				for (size_t Index = 0; Index < IndexAccessor->Count; ++Index)
				{
					auto Value = ReadIndex(*IndexAccessor, Index, *Source);
					if (!Value)
						return std::unexpected(std::move(Value.error()));
					if (*Value >= Imported.Vertices.size())
					{
						return std::unexpected(MakeError(
							GltfMeshImportErrorCode::InvalidMesh,
							*Source,
							"glTF primitive index references a vertex outside POSITION"));
					}
					Imported.Indices.push_back(*Value);
				}
			}
			else
			{
				if (Imported.Vertices.size() > MaxIndexCount)
				{
					return std::unexpected(MakeError(
						GltfMeshImportErrorCode::ResourceLimitExceeded,
						*Source,
						"Unindexed glTF primitive exceeds the importer index limit"));
				}
				Imported.Indices.resize(Imported.Vertices.size());
				for (size_t Index = 0; Index < Imported.Indices.size(); ++Index)
					Imported.Indices[Index] = static_cast<uint32_t>(Index);
			}

			if (Imported.Indices.empty() || Imported.Indices.size() % 3 != 0)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidMesh,
					*Source,
					"Triangle-list glTF geometry must contain a non-zero multiple of three indices"));
			}
			Imported.Name += " primitive " + std::to_string(PrimitiveIndex);
			if (auto Validation = ValidateMeshDescription(Imported.GetMeshDescription()); !Validation)
			{
				return std::unexpected(MakeError(
					GltfMeshImportErrorCode::InvalidMesh,
					*Source,
					"Imported glTF geometry failed PulseForge mesh validation: " + Validation.error().Message));
			}
			return Imported;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakeError(
				GltfMeshImportErrorCode::ResourceLimitExceeded,
				ProjectRoot,
				"Insufficient memory while importing glTF geometry"));
		}
		catch (const std::filesystem::filesystem_error& Exception)
		{
			return std::unexpected(MakeError(
				GltfMeshImportErrorCode::FileReadFailed,
				ProjectRoot,
				std::string("Filesystem error while importing glTF geometry: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				GltfMeshImportErrorCode::InvalidDocument,
				ProjectRoot,
				std::string("Unexpected error while importing glTF geometry: ") + Exception.what()));
		}
	}
}
