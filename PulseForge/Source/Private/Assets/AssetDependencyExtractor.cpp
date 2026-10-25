#include "Assets/AssetDependencyExtractor.h"

#include "Assets/AssetReferenceValidator.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Assets/Detail/GltfSourceDocument.h"
#include "Assets/MaterialAsset.h"
#include "Assets/PrefabSerializer.h"
#include "Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>
#define XXH_INLINE_ALL
#include <tracy_xxhash.h>

static_assert(XXH_VERSION_NUMBER == 802, "Asset input fingerprints require the pinned xxHash 0.8.2 implementation");

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr uint64_t MaximumGltfJsonBytes = 64ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumGltfContainerBytes = 512ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumGltfBufferBytes = 512ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumPrefabBytes = 64ull * 1024ull * 1024ull;
		constexpr size_t MaximumGltfCollectionEntries = 1'000'000;
		constexpr size_t MaximumCycleNodes = 1'000'000;
		constexpr size_t MaximumCycleEdges = 4'000'000;
		constexpr uint32_t GltfOutputSchemaVersion = 1;
		constexpr std::string_view GltfImporterID = "PulseForge.StaticGltfMeshImporter";
		constexpr std::string_view GltfImporterVersion = "1";

		AssetDependencyCatalogError MakeError(const std::filesystem::path& Path, std::string Message)
		{
			return { Path, std::move(Message) };
		}

		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Value = Path.generic_u8string();
			return { reinterpret_cast<const char*>(Value.data()), Value.size() };
		}

		std::expected<std::vector<std::byte>, std::string> ReadBoundedBytes(const std::filesystem::path& Path, uint64_t MaximumBytes)
		{
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status))
				return std::unexpected("source is not a regular, non-symlink file");

			Error.clear();
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size > MaximumBytes || Size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()) ||
				Size > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
				return std::unexpected("source exceeds its bounded read limit or its size could not be determined");

			std::vector<std::byte> Bytes(static_cast<size_t>(Size));
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open()) return std::unexpected("source file could not be opened");
			if (!Bytes.empty()) Input.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!Input || Input.bad()) return std::unexpected("source file could not be read completely");
			return Bytes;
		}

		std::expected<std::string, std::string> ReadBoundedText(const std::filesystem::path& Path, uint64_t MaximumBytes)
		{
			auto Bytes = ReadBoundedBytes(Path, MaximumBytes);
			if (!Bytes) return std::unexpected(Bytes.error());
			if (Bytes->empty()) return std::string{};
			return std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
		}

		std::optional<std::string> FindString(const Json& Object, std::string_view Key)
		{
			if (!Object.is_object()) return std::nullopt;
			const auto Value = Object.find(std::string(Key));
			if (Value == Object.end() || !Value->is_string()) return std::nullopt;
			return Value->get<std::string>();
		}

		AssetDependencyIssueCode ToIssueCode(AssetDependencyStatus Status)
		{
			switch (Status)
			{
			case AssetDependencyStatus::Missing:
				return AssetDependencyIssueCode::MissingDependency;
			case AssetDependencyStatus::WrongType:
				return AssetDependencyIssueCode::WrongDependencyType;
			case AssetDependencyStatus::Unsupported:
				return AssetDependencyIssueCode::UnsupportedDependency;
			case AssetDependencyStatus::Resolved:
				return AssetDependencyIssueCode::InvalidDependency;
			case AssetDependencyStatus::Invalid:
				return AssetDependencyIssueCode::InvalidDependency;
			}
			return AssetDependencyIssueCode::InvalidDependency;
		}

		void AppendIssue(AssetDependencyExtraction& Output, const AssetDependencyNode& Referrer, const AssetDependencyNode& Target,
			AssetDependencyStatus Status, std::string Location, std::string Message)
		{
			Output.Issues.push_back({ ToIssueCode(Status), Referrer, Target, std::move(Location), std::move(Message) });
		}

		AssetDependencyStatus ResolveAssetReference(const AssetRegistry& Registry, const AssetID& Target, AssetFileType ExpectedType)
		{
			const auto Record = Registry.Find(Target);
			if (!Record) return AssetDependencyStatus::Missing;
			return HasAssetTypeExtension(Record->ProjectRelativePath, ExpectedType) ? AssetDependencyStatus::Resolved
																					: AssetDependencyStatus::WrongType;
		}

		void AddRuntimeReference(AssetDependencyExtraction& Output, const AssetRegistry& Registry, const AssetDependencyNode& Referrer,
			const AssetID& TargetID, AssetFileType ExpectedType, std::string Location, bool Optional, std::string OriginalReference = {},
			std::optional<AssetDependencyStatus> ValidatedStatus = std::nullopt)
		{
			if (TargetID.IsNil() && Optional) return;
			const AssetDependencyNode Target{ TargetID, {}, {} };
			AssetDependencyStatus Status = ValidatedStatus.value_or(
				TargetID.IsNil() ? AssetDependencyStatus::Invalid : ResolveAssetReference(Registry, TargetID, ExpectedType));
			Output.Edges.push_back({ Referrer, Target, AssetDependencyEdgeKind::RuntimeReference, Status, Location, OriginalReference });
			if (Status != AssetDependencyStatus::Resolved)
			{
				const std::string Message = Status == AssetDependencyStatus::Missing
												? "Referenced asset UUID " + TargetID.ToString() + " is not present in the project catalog"
											: Status == AssetDependencyStatus::WrongType
												? "Referenced asset UUID " + TargetID.ToString() + " has an incompatible source type"
												: "Required asset reference is nil";
				AppendIssue(Output, Referrer, Target, Status, std::move(Location), Message);
			}
		}

		std::expected<void, AssetDependencyCatalogError> ExtractSceneReferences(
			AssetDependencyExtraction& Output, const AssetRecord& Owner, const AssetRegistry& Registry, const Scene& Source)
		{
			auto Validation = AssetReferenceValidator::Validate(Source, Registry);
			if (!Validation)
				return std::unexpected(
					MakeError(Owner.ProjectRelativePath, "Scene reference validation failed: " + Validation.error().Message));

			const auto FindValidationStatus = [&Validation](const UUID& EntityID, const AssetID& ID,
												  AssetReferenceKind Kind) -> std::optional<AssetDependencyStatus>
			{
				for (const AssetReferenceIssue& Issue : *Validation)
				{
					if (Issue.Entity != EntityID || Issue.Asset != ID || Issue.Kind != Kind) continue;
					return Issue.Code == AssetReferenceIssueCode::MissingAsset ? AssetDependencyStatus::Missing
																			   : AssetDependencyStatus::WrongType;
				}
				return std::nullopt;
			};

			for (const Entity& EntityValue : Source.GetEntities())
			{
				const std::string EntityKey = "entity/" + EntityValue.GetUUID().ToString();
				const AssetDependencyNode Referrer{ Owner.ID, {}, EntityKey };

				const auto Mesh = EntityValue.GetMeshRenderer();
				const auto Environment = EntityValue.GetEnvironmentLight();
				const auto Audio = EntityValue.GetAudioSource();
				const auto Script = EntityValue.GetScript();
				if (!Mesh || !Environment || !Audio || !Script)
					return std::unexpected(
						MakeError(Owner.ProjectRelativePath, "Validated scene entity could not be read through its scene accessors"));

				if (Mesh->has_value())
				{
					const MeshRendererComponent& Component = Mesh->value();
					AddRuntimeReference(Output, Registry, Referrer, Component.MeshAsset, AssetFileType::Mesh,
						"entities/" + EntityKey.substr(7) + "/meshRenderer.meshAsset", false, Component.MeshAsset.ToString(),
						FindValidationStatus(EntityValue.GetUUID(), Component.MeshAsset, AssetReferenceKind::Mesh));
					if (Component.MaterialAsset)
						AddRuntimeReference(Output, Registry, Referrer, *Component.MaterialAsset, AssetFileType::Material,
							"entities/" + EntityKey.substr(7) + "/meshRenderer.materialAsset", true, Component.MaterialAsset->ToString(),
							FindValidationStatus(EntityValue.GetUUID(), *Component.MaterialAsset, AssetReferenceKind::Material));
				}
				if (Environment->has_value())
					AddRuntimeReference(Output, Registry, Referrer, Environment->value().HdrImage, AssetFileType::Environment,
						"entities/" + EntityKey.substr(7) + "/environmentLight.hdrImage", false, Environment->value().HdrImage.ToString(),
						FindValidationStatus(EntityValue.GetUUID(), Environment->value().HdrImage, AssetReferenceKind::Environment));
				if (Audio->has_value())
					AddRuntimeReference(Output, Registry, Referrer, Audio->value().AudioAsset, AssetFileType::Audio,
						"entities/" + EntityKey.substr(7) + "/audioSource.asset", false, Audio->value().AudioAsset.ToString(),
						FindValidationStatus(EntityValue.GetUUID(), Audio->value().AudioAsset, AssetReferenceKind::Audio));
				if (Script->has_value())
					AddRuntimeReference(Output, Registry, Referrer, Script->value().ScriptAsset, AssetFileType::Script,
						"entities/" + EntityKey.substr(7) + "/script.asset", false, Script->value().ScriptAsset.ToString(),
						FindValidationStatus(EntityValue.GetUUID(), Script->value().ScriptAsset, AssetReferenceKind::Script));
			}
			return {};
		}

		std::expected<void, AssetDependencyCatalogError> ExtractSceneFile(AssetDependencyExtraction& Output,
			const std::filesystem::path& SourcePath, const AssetRecord& Owner, const AssetRegistry& Registry)
		{
			Scene Source;
			auto Loaded = SceneSerializer::LoadFromFile(SourcePath, Source);
			if (!Loaded) return std::unexpected(MakeError(SourcePath, "Scene dependency analysis failed: " + Loaded.error().Message));
			return ExtractSceneReferences(Output, Owner, Registry, Source);
		}

		std::expected<void, AssetDependencyCatalogError> ExtractPrefabFile(AssetDependencyExtraction& Output,
			const std::filesystem::path& SourcePath, const AssetRecord& Owner, const AssetRegistry& Registry)
		{
			auto Data = ReadBoundedText(SourcePath, MaximumPrefabBytes);
			if (!Data) return std::unexpected(MakeError(SourcePath, "Prefab dependency analysis failed: " + Data.error()));

			// Use the production prefab path to validate its wrapper/root/subtree policy, then deserialize its nested
			// scene separately so edge owners retain the authored entity UUIDs rather than the instance UUIDs.
			Scene ValidationScene;
			auto ValidatedRoot = PrefabSerializer::Instantiate(*Data, ValidationScene);
			if (!ValidatedRoot)
				return std::unexpected(MakeError(SourcePath, "Prefab dependency analysis failed: " + ValidatedRoot.error().Message));

			Json Document;
			try
			{
				Document = Json::parse(Data->begin(), Data->end());
			}
			catch (const Json::exception& Exception)
			{
				return std::unexpected(MakeError(SourcePath, std::string("Prefab JSON parse failed: ") + Exception.what()));
			}
			const auto SceneValue = Document.find("scene");
			if (SceneValue == Document.end() || !SceneValue->is_object())
				return std::unexpected(MakeError(SourcePath, "Validated prefab is missing its embedded scene object"));

			Scene AuthoredScene;
			const std::string SceneText = SceneValue->dump();
			auto SceneLoad = SceneSerializer::Deserialize(SceneText, AuthoredScene);
			if (!SceneLoad)
				return std::unexpected(MakeError(SourcePath, "Prefab scene dependency analysis failed: " + SceneLoad.error().Message));
			return ExtractSceneReferences(Output, Owner, Registry, AuthoredScene);
		}

		struct BufferInput
		{
			std::string Location;
			std::string OriginalUri;
			std::filesystem::path AbsolutePath;
			std::string ProjectRelativeKey;
			uint64_t ByteLength = 0;
			AssetDependencyStatus Status = AssetDependencyStatus::Resolved;
		};

		std::expected<uint64_t, std::string> ReadByteLength(const Json& Buffer)
		{
			if (!Buffer.is_object()) return std::unexpected("glTF buffer entry must be an object");
			const auto Length = Buffer.find("byteLength");
			if (Length == Buffer.end() || (!Length->is_number_unsigned() && !Length->is_number_integer()))
				return std::unexpected("glTF buffer byteLength must be a non-negative integer");
			if (Length->is_number_integer() && Length->get<int64_t>() < 0)
				return std::unexpected("glTF buffer byteLength must not be negative");
			const uint64_t Value = Length->get<uint64_t>();
			if (Value == 0 || Value > MaximumGltfBufferBytes)
				return std::unexpected("glTF buffer byteLength is zero or exceeds the importer limit");
			return Value;
		}

		bool IsSupportedEmbeddedBufferUri(std::string_view Uri)
		{
			return Uri.starts_with("data:application/octet-stream;base64,") || Uri.starts_with("data:application/gltf-buffer;base64,");
		}

		bool IsProjectRelativeAssetKey(const std::filesystem::path& Path)
		{
			if (Path.empty() || Path.is_absolute() || Path.has_root_name() || Path.has_root_directory()) return false;
			auto Component = Path.begin();
			if (Component == Path.end() || *Component != "Assets") return false;
			for (; Component != Path.end(); ++Component)
			{
				const std::u8string Text = Component->generic_u8string();
				if (*Component == "." || *Component == ".." || (!Text.empty() && (Text.back() == u8'.' || Text.back() == u8' ')))
					return false;
			}
			return true;
		}

		struct GltfSourceRead
		{
			std::string JsonText;
			uint64_t FileSize = 0;
			std::optional<Assets::Detail::GlbChunkRange> BinaryRange;
			XXH128_hash_t SourceDigest{};
		};

		bool ReadLittleEndian32(std::span<const std::byte> Data, size_t Offset, uint32_t& Value)
		{
			if (Offset > Data.size() || Data.size() - Offset < 4) return false;
			Value = 0;
			for (size_t Index = 0; Index < 4; ++Index)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Data[Offset + Index])) << (Index * 8);
			return true;
		}

		std::expected<GltfSourceRead, std::string> ReadGltfSource(const std::filesystem::path& SourcePath, bool IsGlb)
		{
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(SourcePath, Error);
			if (Error || Size > (IsGlb ? MaximumGltfContainerBytes : MaximumGltfJsonBytes))
				return std::unexpected("glTF source exceeds the supported bounded read limit");
			std::error_code TimeError;
			const auto InitialWriteTime = std::filesystem::last_write_time(SourcePath, TimeError);
			if (TimeError) return std::unexpected("glTF source modification state could not be inspected");

			GltfSourceRead Result;
			Result.FileSize = static_cast<uint64_t>(Size);
			XXH3_state_t* State = XXH3_createState();
			if (State == nullptr) return std::unexpected("Could not allocate a glTF fingerprint state");
			struct StateOwner
			{
				XXH3_state_t* Value;
				~StateOwner() { (void)XXH3_freeState(Value); }
			} StateCleanup{ State };
			if (XXH3_128bits_reset(State) != XXH_OK) return std::unexpected("Could not initialize a glTF fingerprint state");

			if (!IsGlb)
			{
				auto Bytes = ReadBoundedBytes(SourcePath, MaximumGltfJsonBytes);
				if (!Bytes) return std::unexpected(Bytes.error());
				if (!Bytes->empty())
				{
					(void)XXH3_128bits_update(State, Bytes->data(), Bytes->size());
					Result.JsonText.assign(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
				}
				Error.clear();
				const uintmax_t FinalSize = std::filesystem::file_size(SourcePath, Error);
				TimeError.clear();
				const auto FinalWriteTime = std::filesystem::last_write_time(SourcePath, TimeError);
				if (Error || TimeError || FinalSize != Size || FinalWriteTime != InitialWriteTime)
					return std::unexpected("glTF source changed while its parsed and fingerprinted bytes were being read");
				Result.SourceDigest = XXH3_128bits_digest(State);
				return Result;
			}

			std::ifstream Input(SourcePath, std::ios::binary);
			if (!Input.is_open()) return std::unexpected("GLB source could not be opened");
			const Assets::Detail::GlbReadRange ReadRange = [&Input](uint64_t Offset, std::span<std::byte> Output)
			{
				if (Offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
					Output.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
					return false;
				Input.clear();
				Input.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
				if (!Input) return false;
				if (!Output.empty()) Input.read(reinterpret_cast<char*>(Output.data()), static_cast<std::streamsize>(Output.size()));
				return static_cast<bool>(Input) && !Input.bad();
			};
			auto Layout = Assets::Detail::InspectGlbLayout(Size, MaximumGltfJsonBytes, ReadRange);
			if (!Layout) return std::unexpected(Layout.error());
			Result.BinaryRange = Layout->Binary;
			Result.JsonText.assign(Layout->Json.Length, '\0');
			Input.clear();
			Input.seekg(0, std::ios::beg);
			if (!Input) return std::unexpected("GLB source could not be rewound for a coherent parse and fingerprint read");

			std::array<std::byte, 64 * 1024> Buffer{};
			std::array<std::byte, 12> Header{};
			std::array<std::byte, 8> JsonChunkHeader{};
			std::array<std::byte, 8> BinaryChunkHeader{};
			uint64_t Offset = 0;
			uint64_t Remaining = Size;
			while (Remaining != 0)
			{
				const size_t Count = static_cast<size_t>(std::min<uint64_t>(Remaining, Buffer.size()));
				Input.read(reinterpret_cast<char*>(Buffer.data()), static_cast<std::streamsize>(Count));
				if (!Input || Input.bad()) return std::unexpected("GLB source could not be read completely for parsing and fingerprinting");
				const std::span<const std::byte> Chunk(Buffer.data(), Count);
				(void)XXH3_128bits_update(State, Chunk.data(), Chunk.size());

				const auto Capture = [Offset, Chunk, Count](uint64_t RangeOffset, std::span<std::byte> Destination)
				{
					const uint64_t RangeEnd = RangeOffset + Destination.size();
					const uint64_t ChunkEnd = Offset + Count;
					const uint64_t Begin = std::max(Offset, RangeOffset);
					const uint64_t End = std::min(ChunkEnd, RangeEnd);
					if (Begin >= End) return;
					const size_t SourceOffset = static_cast<size_t>(Begin - Offset);
					const size_t DestinationOffset = static_cast<size_t>(Begin - RangeOffset);
					std::memcpy(Destination.data() + DestinationOffset, Chunk.data() + SourceOffset, static_cast<size_t>(End - Begin));
				};
				Capture(0, Header);
				Capture(Layout->Json.Offset - 8, JsonChunkHeader);
				if (Layout->Binary) Capture(Layout->Binary->Offset - 8, BinaryChunkHeader);
				Capture(Layout->Json.Offset,
					std::span<std::byte>(reinterpret_cast<std::byte*>(Result.JsonText.data()), Result.JsonText.size()));
				Offset += Count;
				Remaining -= Count;
			}

			uint32_t Magic = 0;
			uint32_t Version = 0;
			uint32_t DeclaredFileSize = 0;
			uint32_t JsonLength = 0;
			uint32_t JsonType = 0;
			if (!ReadLittleEndian32(Header, 0, Magic) || !ReadLittleEndian32(Header, 4, Version) ||
				!ReadLittleEndian32(Header, 8, DeclaredFileSize) || Magic != Assets::Detail::GlbMagic || Version != 2 ||
				DeclaredFileSize != Size || !ReadLittleEndian32(JsonChunkHeader, 0, JsonLength) ||
				!ReadLittleEndian32(JsonChunkHeader, 4, JsonType) || JsonLength != Layout->Json.Length ||
				JsonType != Assets::Detail::GlbJsonChunk)
				return std::unexpected("GLB layout changed while source bytes were being fingerprinted");
			if (Layout->Binary)
			{
				uint32_t BinaryLength = 0;
				uint32_t BinaryType = 0;
				if (!ReadLittleEndian32(BinaryChunkHeader, 0, BinaryLength) || !ReadLittleEndian32(BinaryChunkHeader, 4, BinaryType) ||
					BinaryLength != Layout->Binary->Length || BinaryType != Assets::Detail::GlbBinaryChunk)
					return std::unexpected("GLB BIN layout changed while source bytes were being fingerprinted");
			}
			Error.clear();
			const uintmax_t FinalSize = std::filesystem::file_size(SourcePath, Error);
			TimeError.clear();
			const auto FinalWriteTime = std::filesystem::last_write_time(SourcePath, TimeError);
			if (Error || TimeError || FinalSize != Size || FinalWriteTime != InitialWriteTime)
				return std::unexpected("GLB source changed while its parsed and fingerprinted bytes were being read");

			while (!Result.JsonText.empty() &&
				   (Result.JsonText.back() == '\0' || std::isspace(static_cast<unsigned char>(Result.JsonText.back()))))
				Result.JsonText.pop_back();
			Result.SourceDigest = XXH3_128bits_digest(State);
			return Result;
		}

		struct XxHashState
		{
			XXH3_state_t* Value = XXH3_createState();
			~XxHashState()
			{
				if (Value) XXH3_freeState(Value);
			}
		};

		void HashBytes(XXH3_state_t* State, std::span<const std::byte> Bytes)
		{
			if (!Bytes.empty()) (void)XXH3_128bits_update(State, Bytes.data(), Bytes.size());
		}

		void HashString(XXH3_state_t* State, std::string_view Value)
		{
			const uint64_t Length = Value.size();
			std::array<std::byte, sizeof(Length)> EncodedLength{};
			for (size_t Index = 0; Index < EncodedLength.size(); ++Index)
				EncodedLength[Index] = static_cast<std::byte>((Length >> (Index * 8)) & 0xff);
			HashBytes(State, EncodedLength);
			HashBytes(State, std::as_bytes(std::span(Value.data(), Value.size())));
		}

		std::expected<void, std::string> HashFileRange(XXH3_state_t* State, const std::filesystem::path& Path, uint64_t ByteLength)
		{
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status))
				return std::unexpected("input is not a regular, non-symlink file");
			Error.clear();
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size < ByteLength) return std::unexpected("input is missing or shorter than the declared glTF buffer byteLength");

			const auto InitialWriteTime = std::filesystem::last_write_time(Path, Error);
			if (Error) return std::unexpected("input modification state could not be inspected");

			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open()) return std::unexpected("input could not be opened while fingerprinting");
			std::array<std::byte, 64 * 1024> Buffer{};
			uint64_t Remaining = ByteLength;
			while (Remaining != 0)
			{
				const size_t Count = static_cast<size_t>(std::min<uint64_t>(Remaining, Buffer.size()));
				Input.read(reinterpret_cast<char*>(Buffer.data()), static_cast<std::streamsize>(Count));
				if (!Input || Input.bad()) return std::unexpected("input could not be read completely while fingerprinting");
				HashBytes(State, std::span<const std::byte>(Buffer.data(), Count));
				Remaining -= Count;
			}
			Error.clear();
			const uintmax_t FinalSize = std::filesystem::file_size(Path, Error);
			std::error_code TimeError;
			const auto FinalWriteTime = std::filesystem::last_write_time(Path, TimeError);
			if (Error || TimeError || FinalSize != Size || FinalWriteTime != InitialWriteTime)
				return std::unexpected("input changed while its consumed bytes were fingerprinted");
			return {};
		}

		std::string DigestHex(XXH128_hash_t Digest)
		{
			std::array<char, 32> Digits{};
			constexpr char Hex[] = "0123456789abcdef";
			for (size_t Index = 0; Index < 16; ++Index)
			{
				const size_t Shift = (15 - Index) * 4;
				Digits[Index] = Hex[(Digest.high64 >> Shift) & 0xf];
				Digits[16 + Index] = Hex[(Digest.low64 >> Shift) & 0xf];
			}
			return { Digits.begin(), Digits.end() };
		}

		std::expected<void, std::string> VerifySourceDigest(
			const std::filesystem::path& Path, uint64_t ExpectedSize, XXH128_hash_t ExpectedDigest)
		{
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status))
				return std::unexpected("source ceased to be a regular, non-symlink file during analysis");
			Error.clear();
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size != ExpectedSize) return std::unexpected("source size changed during dependency analysis");

			XXH3_state_t* State = XXH3_createState();
			if (State == nullptr) return std::unexpected("could not allocate a source verification state");
			struct StateOwner
			{
				XXH3_state_t* Value;
				~StateOwner() { (void)XXH3_freeState(Value); }
			} Cleanup{ State };
			if (XXH3_128bits_reset(State) != XXH_OK) return std::unexpected("could not initialize a source verification state");

			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open()) return std::unexpected("source could not be reopened for mutation verification");
			std::array<std::byte, 64 * 1024> Buffer{};
			uint64_t Remaining = ExpectedSize;
			while (Remaining != 0)
			{
				const size_t Count = static_cast<size_t>(std::min<uint64_t>(Remaining, Buffer.size()));
				Input.read(reinterpret_cast<char*>(Buffer.data()), static_cast<std::streamsize>(Count));
				if (!Input || Input.bad()) return std::unexpected("source could not be reread completely for mutation verification");
				(void)XXH3_128bits_update(State, Buffer.data(), Count);
				Remaining -= Count;
			}
			const XXH128_hash_t ActualDigest = XXH3_128bits_digest(State);
			if (ActualDigest.high64 != ExpectedDigest.high64 || ActualDigest.low64 != ExpectedDigest.low64)
				return std::unexpected("source content changed during dependency analysis");
			return {};
		}

		AssetDependencyNode RootNode(const AssetRecord& Record)
		{
			return { Record.ID, PathToUtf8(Record.ProjectRelativePath), {} };
		}

		AssetDependencyNode SourceFileNode(std::string Key)
		{
			return { {}, std::move(Key), {} };
		}

		std::string MeshPrimitiveKey(size_t MeshIndex, size_t PrimitiveIndex)
		{
			return "mesh/" + std::to_string(MeshIndex) + "/primitive/" + std::to_string(PrimitiveIndex);
		}

		std::expected<void, AssetDependencyCatalogError> AnalyzeGltf(AssetDependencyExtraction& Output,
			const std::filesystem::path& ProjectRoot, const std::filesystem::path& SourcePath, const AssetRecord& Owner)
		{
			const bool IsGlb = HasAssetExtension(SourcePath, { ".glb" });
			auto SourceRead = ReadGltfSource(SourcePath, IsGlb);
			if (!SourceRead) return std::unexpected(MakeError(SourcePath, "glTF dependency analysis failed: " + SourceRead.error()));

			Json Document;
			try
			{
				Document = Json::parse(SourceRead->JsonText.begin(), SourceRead->JsonText.end());
			}
			catch (const Json::exception& Exception)
			{
				return std::unexpected(MakeError(SourcePath, std::string("glTF JSON parse failed: ") + Exception.what()));
			}
			const auto AssetObject = Document.find("asset");
			if (!Document.is_object() || AssetObject == Document.end() || !AssetObject->is_object() ||
				FindString(*AssetObject, "version") != std::optional<std::string>{ "2.0" })
				return std::unexpected(MakeError(SourcePath, "glTF dependency source must have an asset.version of 2.0"));

			std::vector<BufferInput> ExternalBuffers;
			bool AnyUnavailable = false;
			const AssetDependencyNode Referrer = RootNode(Owner);
			if (const auto Required = Document.find("extensionsRequired"); Required != Document.end())
			{
				if (!Required->is_array() || Required->size() > MaximumGltfCollectionEntries)
					return std::unexpected(MakeError(SourcePath, "glTF extensionsRequired must be a bounded array"));
				for (size_t Index = 0; Index < Required->size(); ++Index)
				{
					if (!(*Required)[Index].is_string())
						return std::unexpected(MakeError(SourcePath, "glTF required extension name must be a string"));
					const std::string Extension = (*Required)[Index].get<std::string>();
					const AssetDependencyNode Target = RootNode(Owner);
					AppendIssue(Output, Referrer, Target, AssetDependencyStatus::Unsupported, "extensionsRequired/" + std::to_string(Index),
						"Current static mesh importer does not support required glTF extension '" + Extension + "'");
					AnyUnavailable = true;
				}
			}
			const auto Buffers = Document.find("buffers");
			if (Buffers == Document.end() || !Buffers->is_array() || Buffers->empty() || Buffers->size() > MaximumGltfCollectionEntries)
				return std::unexpected(MakeError(SourcePath, "glTF static mesh source requires a bounded, non-empty buffers array"));
			if (Buffers != Document.end())
			{
				uint64_t TotalBufferBytes = 0;
				for (size_t Index = 0; Index < Buffers->size(); ++Index)
				{
					const Json& Buffer = (*Buffers)[Index];
					auto ByteLength = ReadByteLength(Buffer);
					if (!ByteLength || TotalBufferBytes > MaximumGltfBufferBytes - *ByteLength)
						return std::unexpected(MakeError(
							SourcePath, ByteLength ? "glTF total declared buffer bytes exceed the importer limit" : ByteLength.error()));
					TotalBufferBytes += *ByteLength;
					const auto Uri = Buffer.find("uri");
					if (Uri == Buffer.end())
					{
						if (!IsGlb || Index != 0 || !SourceRead->BinaryRange || *ByteLength > SourceRead->BinaryRange->Length ||
							SourceRead->BinaryRange->Length - *ByteLength > 3)
							return std::unexpected(
								MakeError(SourcePath, "glTF buffer without a URI has no matching supported GLB BIN chunk"));
						continue;
					}
					if (!Uri->is_string()) return std::unexpected(MakeError(SourcePath, "glTF buffer URI must be a string"));
					const std::string OriginalUri = Uri->get<std::string>();
					if (OriginalUri.starts_with("data:"))
					{
						if (!IsSupportedEmbeddedBufferUri(OriginalUri))
						{
							const AssetDependencyNode Target = SourceFileNode({});
							const std::string Location = "buffers/" + std::to_string(Index) + ".uri";
							Output.Edges.push_back({ Referrer, Target, AssetDependencyEdgeKind::ImportInput,
								AssetDependencyStatus::Unsupported, Location, OriginalUri });
							AppendIssue(Output, Referrer, Target, AssetDependencyStatus::Unsupported, Location,
								"Embedded glTF buffer URI uses an unsupported data URI encoding");
							AnyUnavailable = true;
						}
						continue;
					}

					auto Relative = Assets::Detail::DecodeGltfBufferUri(OriginalUri);
					const std::string Location = "buffers/" + std::to_string(Index) + ".uri";
					if (!Relative || !*Relative)
					{
						const AssetDependencyNode Target = SourceFileNode({});
						const AssetDependencyStatus Status = AssetDependencyStatus::Invalid;
						Output.Edges.push_back({ Referrer, Target, AssetDependencyEdgeKind::ImportInput, Status, Location, OriginalUri });
						AppendIssue(Output, Referrer, Target, Status, Location,
							Relative ? "External glTF buffer URI unexpectedly resolved as embedded" : Relative.error());
						AnyUnavailable = true;
						continue;
					}

					const std::filesystem::path AbsoluteDependency = (SourcePath.parent_path() / **Relative).lexically_normal();
					const std::filesystem::path ProjectRelative = AbsoluteDependency.lexically_relative(ProjectRoot);
					const bool HasSafeProjectKey = IsProjectRelativeAssetKey(ProjectRelative);
					BufferInput Input{ Location, OriginalUri, AbsoluteDependency,
						HasSafeProjectKey ? PathToUtf8(ProjectRelative) : std::string{}, *ByteLength };
					auto ManagedPath = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, ProjectRelative);
					if (!ManagedPath)
					{
						std::error_code ExistenceError;
						const bool Exists = std::filesystem::exists(AbsoluteDependency, ExistenceError) && !ExistenceError;
						Input.Status = !HasSafeProjectKey || Exists ? AssetDependencyStatus::Invalid : AssetDependencyStatus::Missing;
						const AssetDependencyNode Target = SourceFileNode(Input.ProjectRelativeKey);
						Output.Edges.push_back(
							{ Referrer, Target, AssetDependencyEdgeKind::ImportInput, Input.Status, Location, OriginalUri });
						AppendIssue(Output, Referrer, Target, Input.Status, Location,
							Exists ? "External glTF buffer is not a safe managed source file: " + ManagedPath.error().Message
								   : "External glTF buffer is missing: " + Input.ProjectRelativeKey);
						AnyUnavailable = true;
						ExternalBuffers.push_back(std::move(Input));
						continue;
					}
					const AssetDependencyNode Target = SourceFileNode(Input.ProjectRelativeKey);

					std::error_code SizeError;
					const uintmax_t DependencySize = std::filesystem::file_size(*ManagedPath, SizeError);
					if (SizeError || DependencySize < *ByteLength)
					{
						Input.Status = AssetDependencyStatus::Invalid;
						Output.Edges.push_back(
							{ Referrer, Target, AssetDependencyEdgeKind::ImportInput, Input.Status, Location, OriginalUri });
						AppendIssue(Output, Referrer, Target, Input.Status, Location,
							"External glTF buffer is shorter than its declared byteLength or its size cannot be read");
						AnyUnavailable = true;
						ExternalBuffers.push_back(std::move(Input));
						continue;
					}
					Input.AbsolutePath = *ManagedPath;
					Output.Edges.push_back(
						{ Referrer, Target, AssetDependencyEdgeKind::ImportInput, AssetDependencyStatus::Resolved, Location, OriginalUri });
					ExternalBuffers.push_back(std::move(Input));
				}
			}

			const auto Images = Document.find("images");
			if (Images != Document.end())
			{
				if (!Images->is_array() || Images->size() > MaximumGltfCollectionEntries)
					return std::unexpected(MakeError(SourcePath, "glTF images must be a bounded array"));
				for (size_t Index = 0; Index < Images->size(); ++Index)
				{
					const Json& Image = (*Images)[Index];
					if (!Image.is_object()) return std::unexpected(MakeError(SourcePath, "glTF image entry must be an object"));
					const auto Uri = Image.find("uri");
					if (Uri != Image.end() && Image.find("bufferView") != Image.end())
						return std::unexpected(MakeError(SourcePath, "glTF image cannot contain both uri and bufferView"));
					if (Uri == Image.end())
					{
						if (Image.find("bufferView") != Image.end())
						{
							const std::string Location = "images/" + std::to_string(Index) + ".bufferView";
							const AssetDependencyNode Target = SourceFileNode({});
							Output.Edges.push_back({ Referrer, Target, AssetDependencyEdgeKind::ImportInput,
								AssetDependencyStatus::Unsupported, Location, "embedded image bufferView" });
							AppendIssue(Output, Referrer, Target, AssetDependencyStatus::Unsupported, Location,
								"glTF images are not consumed by the current static mesh importer");
						}
						else return std::unexpected(MakeError(SourcePath, "glTF image must provide either a URI or bufferView"));
						continue;
					}
					if (!Uri->is_string()) return std::unexpected(MakeError(SourcePath, "glTF image URI must be a string"));
					const std::string OriginalUri = Uri->get<std::string>();
					AssetDependencyNode Target = SourceFileNode({});
					AssetDependencyStatus Status = AssetDependencyStatus::Unsupported;
					if (!OriginalUri.starts_with("data:"))
					{
						auto Relative = Assets::Detail::DecodeGltfBufferUri(OriginalUri);
						if (Relative && *Relative)
						{
							const auto Absolute = (SourcePath.parent_path() / **Relative).lexically_normal();
							const auto ProjectRelative = Absolute.lexically_relative(ProjectRoot);
							const bool SafeProjectKey = IsProjectRelativeAssetKey(ProjectRelative);
							if (SafeProjectKey) Target.SourceFileKey = PathToUtf8(ProjectRelative);
							else Status = AssetDependencyStatus::Invalid;
							auto Managed = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, ProjectRelative);
							if (Managed) Status = AssetDependencyStatus::Unsupported;
							else if (SafeProjectKey)
							{
								std::error_code ExistenceError;
								const bool Exists = std::filesystem::exists(Absolute, ExistenceError) && !ExistenceError;
								Status = Exists ? AssetDependencyStatus::Invalid : AssetDependencyStatus::Missing;
							}
						}
						else if (!Relative) Status = AssetDependencyStatus::Invalid;
					}
					const std::string Location = "images/" + std::to_string(Index) + ".uri";
					Output.Edges.push_back({ Referrer, Target, AssetDependencyEdgeKind::ImportInput, Status, Location, OriginalUri });
					AppendIssue(Output, Referrer, Target, Status, Location,
						Status == AssetDependencyStatus::Invalid ? "glTF image URI is invalid for a contained project-relative source"
																 : "glTF image URI is not consumed by the current static mesh importer");
				}
			}

			const auto Meshes = Document.find("meshes");
			if (Meshes == Document.end() || !Meshes->is_array() || Meshes->empty() || Meshes->size() > MaximumGltfCollectionEntries)
				return std::unexpected(MakeError(SourcePath, "glTF static mesh source requires a bounded, non-empty meshes array"));
			if (Meshes != Document.end())
			{
				size_t TotalPrimitives = 0;
				for (size_t MeshIndex = 0; MeshIndex < Meshes->size(); ++MeshIndex)
				{
					const Json& Mesh = (*Meshes)[MeshIndex];
					if (!Mesh.is_object()) return std::unexpected(MakeError(SourcePath, "glTF mesh entry must be an object"));
					const auto Primitives = Mesh.find("primitives");
					if (Primitives == Mesh.end() || !Primitives->is_array() || Primitives->empty() ||
						Primitives->size() > MaximumGltfCollectionEntries - TotalPrimitives)
						return std::unexpected(MakeError(SourcePath, "glTF mesh primitives must be a bounded array"));
					TotalPrimitives += Primitives->size();
					const std::string MeshName = FindString(Mesh, "name").value_or("mesh " + std::to_string(MeshIndex));
					for (size_t PrimitiveIndex = 0; PrimitiveIndex < Primitives->size(); ++PrimitiveIndex)
					{
						const Json& Primitive = (*Primitives)[PrimitiveIndex];
						if (!Primitive.is_object()) return std::unexpected(MakeError(SourcePath, "glTF primitive entry must be an object"));
						const std::string Key = MeshPrimitiveKey(MeshIndex, PrimitiveIndex);
						// Index locators are stable for unchanged source structure. A reorder/insertion has no guaranteed matching
						// identity in glTF and must be treated as a changed/ambiguous source, never name-matched silently.
						const Json* Extensions = nullptr;
						if (const auto Value = Primitive.find("extensions"); Value != Primitive.end() && Value->is_object())
							Extensions = &*Value;
						const bool Draco = Extensions != nullptr && Extensions->find("KHR_draco_mesh_compression") != Extensions->end();
						const auto Targets = Primitive.find("targets");
						if (Targets != Primitive.end() && !Targets->is_array())
							return std::unexpected(MakeError(SourcePath, "glTF primitive targets must be an array"));
						const bool HasMorphTargets = Targets != Primitive.end() && !Targets->empty();
						uint64_t Mode = 4;
						if (const auto ModeField = Primitive.find("mode"); ModeField != Primitive.end())
						{
							if (!ModeField->is_number_integer() || ModeField->get<int64_t>() < 0)
								return std::unexpected(MakeError(SourcePath, "glTF primitive mode must be a non-negative integer"));
							Mode = ModeField->get<uint64_t>();
						}
						if (!Draco && !HasMorphTargets && Mode == 4)
							Output.Subresources.push_back({ { Owner.ID, PathToUtf8(Owner.ProjectRelativePath), Key },
								MeshName + " / primitive " + std::to_string(PrimitiveIndex) });
						else
						{
							const AssetDependencyNode Target = SourceFileNode(PathToUtf8(Owner.ProjectRelativePath));
							const std::string Reason = Draco			 ? "Primitive uses unsupported KHR_draco_mesh_compression"
													   : HasMorphTargets ? "Primitive has unsupported morph targets"
																		 : "Only triangle-list primitives are importable";
							AppendIssue(Output, Referrer, Target, AssetDependencyStatus::Unsupported,
								"meshes/" + std::to_string(MeshIndex) + "/primitives/" + std::to_string(PrimitiveIndex), Reason);
							AnyUnavailable = true;
						}
					}
				}
			}

			XxHashState Hash;
			if (!Hash.Value || XXH3_128bits_reset(Hash.Value) != XXH_OK)
				return std::unexpected(MakeError(SourcePath, "Could not initialize the deterministic glTF fingerprint"));
			HashString(Hash.Value, "PulseForge.GltfImportInputs.v1");
			HashString(Hash.Value, std::string(GltfImporterID));
			HashString(Hash.Value, std::string(GltfImporterVersion));
			HashString(Hash.Value, std::to_string(GltfOutputSchemaVersion));
			HashString(Hash.Value, PathToUtf8(Owner.ProjectRelativePath));
			HashString(Hash.Value, std::to_string(SourceRead->FileSize));
			HashString(Hash.Value, DigestHex(SourceRead->SourceDigest));

			std::sort(ExternalBuffers.begin(), ExternalBuffers.end(),
				[](const BufferInput& First, const BufferInput& Second)
				{
					if (First.ProjectRelativeKey != Second.ProjectRelativeKey) return First.ProjectRelativeKey < Second.ProjectRelativeKey;
					return First.ByteLength < Second.ByteLength;
				});
			for (const BufferInput& Input : ExternalBuffers)
			{
				HashString(Hash.Value, "external-buffer");
				HashString(Hash.Value, Input.ProjectRelativeKey);
				HashString(Hash.Value, std::to_string(Input.ByteLength));
				if (Input.Status != AssetDependencyStatus::Resolved) continue;
				auto InputHash = HashFileRange(Hash.Value, Input.AbsolutePath, Input.ByteLength);
				if (!InputHash)
					return std::unexpected(
						MakeError(Input.AbsolutePath, "Could not fingerprint glTF external buffer: " + InputHash.error()));
			}
			auto SourceUnchanged = VerifySourceDigest(SourcePath, SourceRead->FileSize, SourceRead->SourceDigest);
			if (!SourceUnchanged)
				return std::unexpected(MakeError(SourcePath, "glTF source changed during dependency analysis: " + SourceUnchanged.error()));

			const XXH128_hash_t Digest = XXH3_128bits_digest(Hash.Value);
			AssetImportObservation Observation;
			Observation.Asset = Owner.ID;
			Observation.ImporterID = GltfImporterID;
			Observation.ImporterVersion = GltfImporterVersion;
			Observation.SettingsVersion = 1;
			Observation.EffectiveSettings.clear(); // Current importer has no authored settings; mesh/primitive are source locators.
			Observation.InputFingerprint = { "xxh3-128-v0.8.2-input-v1", DigestHex(Digest) };
			Observation.ResultState =
				AnyUnavailable ? AssetImportResultState::Unavailable : AssetImportResultState::NoSuccessfulResultRecorded;
			// This catalog inspection records inputs only. No successful artifact/import result is persisted today.
			Output.ImportObservations.push_back(std::move(Observation));
			return {};
		}

		std::expected<void, AssetDependencyCatalogError> AnalyzeMaterial(AssetDependencyExtraction& Output,
			const std::filesystem::path& SourcePath, const AssetRecord& Owner, const AssetRegistry& Registry)
		{
			auto Material = MaterialAssetSerializer::LoadFromFile(SourcePath);
			if (!Material)
				return std::unexpected(MakeError(SourcePath, "Material dependency analysis failed: " + Material.error().Message));
			const AssetDependencyNode Referrer = RootNode(Owner);
			const std::array<std::pair<std::string_view, AssetID>, 5> References{ { { "baseColorTexture", Material->BaseColorTexture },
				{ "metallicRoughnessTexture", Material->MetallicRoughnessTexture }, { "normalTexture", Material->NormalTexture },
				{ "occlusionTexture", Material->OcclusionTexture }, { "emissiveTexture", Material->EmissiveTexture } } };
			for (const auto& [Field, Asset] : References)
				AddRuntimeReference(Output, Registry, Referrer, Asset, AssetFileType::Image, "material." + std::string(Field), true,
					Asset.IsNil() ? "null" : Asset.ToString());
			return {};
		}

		std::expected<void, AssetDependencyCatalogError> AppendRuntimeCycles(
			AssetDependencyExtraction& Output, const std::filesystem::path& ProjectRoot, const std::vector<AssetRecord>& Records)
		{
			if (Records.size() > MaximumCycleNodes)
				return std::unexpected(MakeError(ProjectRoot, "Runtime dependency graph exceeds the bounded cycle-analysis node limit"));

			std::map<AssetID, std::vector<AssetID>> Adjacency;
			for (const AssetRecord& Record : Records)
				Adjacency.try_emplace(Record.ID);
			size_t EdgeCount = 0;
			for (const AssetDependencyEdge& Edge : Output.Edges)
			{
				if (Edge.Kind != AssetDependencyEdgeKind::RuntimeReference || Edge.Status != AssetDependencyStatus::Resolved ||
					Edge.Referrer.Asset.IsNil() || Edge.Target.Asset.IsNil())
					continue;
				if (++EdgeCount > MaximumCycleEdges)
					return std::unexpected(
						MakeError(ProjectRoot, "Runtime dependency graph exceeds the bounded cycle-analysis edge limit"));
				Adjacency[Edge.Referrer.Asset].push_back(Edge.Target.Asset);
			}
			for (auto& [Asset, Targets] : Adjacency)
			{
				(void)Asset;
				std::sort(Targets.begin(), Targets.end());
				Targets.erase(std::unique(Targets.begin(), Targets.end()), Targets.end());
			}

			struct Frame
			{
				AssetID Node;
				size_t Next = 0;
			};
			std::map<AssetID, uint8_t> Color;
			std::set<std::string> Reported;
			std::vector<Frame> Stack;
			for (const auto& [Root, Targets] : Adjacency)
			{
				(void)Targets;
				if (Color[Root] != 0) continue;
				Color[Root] = 1;
				Stack.push_back({ Root, 0 });
				while (!Stack.empty())
				{
					Frame& Current = Stack.back();
					const auto Found = Adjacency.find(Current.Node);
					if (Found == Adjacency.end() || Current.Next == Found->second.size())
					{
						Color[Current.Node] = 2;
						Stack.pop_back();
						continue;
					}

					const AssetID Target = Found->second[Current.Next++];
					const uint8_t TargetColor = Color[Target];
					if (TargetColor == 0)
					{
						Color[Target] = 1;
						Stack.push_back({ Target, 0 });
						continue;
					}
					if (TargetColor != 1) continue;

					auto CycleStart =
						std::find_if(Stack.begin(), Stack.end(), [&Target](const Frame& Entry) { return Entry.Node == Target; });
					if (CycleStart == Stack.end()) continue;
					std::string Message = "Runtime reference cycle: ";
					for (auto Entry = CycleStart; Entry != Stack.end(); ++Entry)
					{
						if (Entry != CycleStart) Message += " -> ";
						Message += Entry->Node.ToString();
					}
					Message += " -> " + Target.ToString();
					if (!Reported.insert(Message).second) continue;
					Output.Issues.push_back({ AssetDependencyIssueCode::Cycle, { Current.Node, {}, {} }, { Target, {}, {} },
						"runtime-reference-cycle", std::move(Message) });
				}
			}
			return {};
		}

	} // namespace

	std::expected<AssetDependencyExtraction, AssetDependencyCatalogError> AssetDependencyExtractor::Extract(
		const std::filesystem::path& ProjectRoot, const UUID& ProjectID, const AssetRegistry& Registry)
	{
		if (ProjectID.IsNil()) return std::unexpected(MakeError(ProjectRoot, "Dependency extraction requires a non-nil project identity"));
		// The current catalog node contract identifies managed sources and subresources, but has no stable project-file
		// pseudo-node. The project's startup-scene setting is therefore intentionally outside this asset graph.
		std::error_code Error;
		const std::filesystem::path AbsoluteProjectRoot = std::filesystem::absolute(ProjectRoot, Error).lexically_normal();
		if (Error || !std::filesystem::is_directory(AbsoluteProjectRoot, Error) || Error)
			return std::unexpected(MakeError(ProjectRoot, "Project root is not an accessible directory"));

		AssetDependencyExtraction Output;
		const std::vector<AssetRecord> Records = Registry.GetAssets();
		for (const AssetRecord& Record : Records)
		{
			auto Resolved = AssetPathResolver::ResolveManagedSourcePath(AbsoluteProjectRoot, Record.ProjectRelativePath);
			if (!Resolved) return std::unexpected(MakeError(Resolved.error().Path, Resolved.error().Message));

			std::expected<void, AssetDependencyCatalogError> Result;
			if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Material))
				Result = AnalyzeMaterial(Output, *Resolved, Record, Registry);
			else if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Scene))
				Result = ExtractSceneFile(Output, *Resolved, Record, Registry);
			else if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Prefab))
				Result = ExtractPrefabFile(Output, *Resolved, Record, Registry);
			else if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Mesh))
				Result = AnalyzeGltf(Output, AbsoluteProjectRoot, *Resolved, Record);
			else continue;
			if (!Result) return std::unexpected(std::move(Result.error()));
		}
		auto Cycles = AppendRuntimeCycles(Output, AbsoluteProjectRoot, Records);
		if (!Cycles) return std::unexpected(std::move(Cycles.error()));

		return Output;
	}
} // namespace PulseForge
