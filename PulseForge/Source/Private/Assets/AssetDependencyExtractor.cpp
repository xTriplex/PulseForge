#include "Assets/AssetDependencyExtractor.h"

#include "Assets/AssetImportProvenance.h"
#include "Assets/AssetMetadata.h"
#include "Assets/AssetReferenceValidator.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Assets/Detail/GltfPrimitiveInspector.h"
#include "Assets/Detail/GltfSourceDocument.h"
#include "Assets/GltfMeshImporter.h"
#include "Assets/MaterialAsset.h"
#include "Assets/PrefabSerializer.h"
#include "Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
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
		constexpr uint64_t MaximumMetadataBytes = 1024ull * 1024ull;
		constexpr uint64_t MaximumProvenanceBytes = 1024ull * 1024ull;
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

		bool IsSafeReadSetKey(const std::filesystem::path& Relative)
		{
			if (Relative.empty() || Relative.is_absolute() || Relative.has_root_name() || Relative.has_root_directory() ||
				Relative.lexically_normal() != Relative)
				return false;
			auto Component = Relative.begin();
			if (Component == Relative.end() || *Component != "Assets") return false;
			for (; Component != Relative.end(); ++Component)
			{
				const std::u8string Text = Component->generic_u8string();
				if (*Component == "." || *Component == ".." ||
					(!Text.empty() && (Text.back() == u8'.' || Text.back() == u8' ')))
					return false;
			}
			return std::next(Relative.begin()) != Relative.end();
		}

		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Value = Path.generic_u8string();
			return { reinterpret_cast<const char*>(Value.data()), Value.size() };
		}

		AssetContentFingerprint FingerprintBytes(std::span<const std::byte> Bytes)
		{
			return ComputeAssetContentFingerprint(Bytes);
		}

		std::string ProjectRelativeKey(const std::filesystem::path& ProjectRoot, const std::filesystem::path& Path)
		{
			const auto Relative = Path.lexically_relative(ProjectRoot).lexically_normal();
			return PathToUtf8(Relative);
		}

		void AddReadReceipt(AssetDependencyExtraction& Output, const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& Path, std::span<const std::byte> Bytes)
		{
			Output.ReadSet.push_back({ ProjectRelativeKey(ProjectRoot, Path), true, static_cast<uint64_t>(Bytes.size()), FingerprintBytes(Bytes) });
		}

		void AddReadReceipt(AssetDependencyExtraction& Output, std::string RelativePath, uint64_t ByteLength,
			AssetContentFingerprint Content)
		{
			Output.ReadSet.push_back({ std::move(RelativePath), true, ByteLength, std::move(Content) });
		}

		std::expected<std::vector<std::byte>, std::string> ReadBoundedBytes(
			const std::filesystem::path& Path, uint64_t MaximumBytes);

		std::expected<AssetMetadata, AssetDependencyCatalogError> ReadAssetMetadata(
			AssetDependencyExtraction& Output, const std::filesystem::path& ProjectRoot, const std::filesystem::path& SourcePath,
			const AssetRecord& Record)
		{
			const std::filesystem::path Sidecar = AssetMetadataSerializer::GetSidecarPath(SourcePath);
			auto Bytes = ReadBoundedBytes(Sidecar, MaximumMetadataBytes);
			if (!Bytes) return std::unexpected(MakeError(Sidecar, "Asset metadata could not be read for dependency analysis: " + Bytes.error()));
			const std::string Data(Bytes->empty() ? std::string{} :
				std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size()));
			auto Metadata = AssetMetadataSerializer::Deserialize(Data, Sidecar);
			if (!Metadata) return std::unexpected(MakeError(Sidecar, "Asset metadata is invalid: " + Metadata.error().Message));
			if (Metadata->ID != Record.ID)
				return std::unexpected(MakeError(Sidecar,
					"Asset metadata UUID changed during dependency analysis; registry identity no longer matches its sidecar"));
			AddReadReceipt(Output, ProjectRoot, Sidecar, *Bytes);
			return *Metadata;
		}

		std::expected<AssetContentFingerprint, std::string> FingerprintFile(
			const std::filesystem::path& Path, uint64_t ExpectedLength, uint64_t MaximumBytes)
		{
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status))
				return std::unexpected("input is not a regular, non-symlink file");
			Error.clear();
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size != ExpectedLength || Size > MaximumBytes ||
				Size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				return std::unexpected("input length changed or exceeds its bounded read limit");
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open()) return std::unexpected("input could not be opened for fingerprinting");
			AssetFingerprintBuilder Builder("PulseForge.AssetInputBytes.v1");
			Builder.UpdateText("byteLength", std::to_string(ExpectedLength));
			std::array<std::byte, 64 * 1024> Buffer{};
			uint64_t Remaining = ExpectedLength;
			while (Remaining != 0)
			{
				const size_t Count = static_cast<size_t>(std::min<uint64_t>(Remaining, Buffer.size()));
				Input.read(reinterpret_cast<char*>(Buffer.data()), static_cast<std::streamsize>(Count));
				if (!Input || Input.bad()) return std::unexpected("input could not be read completely for fingerprinting");
				Builder.UpdateBytes("chunk", std::span<const std::byte>(Buffer.data(), Count));
				Remaining -= Count;
			}
			Error.clear();
			const uintmax_t FinalSize = std::filesystem::file_size(Path, Error);
			if (Error || FinalSize != Size) return std::unexpected("input length changed while fingerprinting");
			return Builder.Finish();
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
			const std::filesystem::path& ProjectRoot, const std::filesystem::path& SourcePath, const AssetRecord& Owner,
			const AssetRegistry& Registry)
		{
			auto Bytes = ReadBoundedBytes(SourcePath, 64ull * 1024ull * 1024ull);
			if (!Bytes) return std::unexpected(MakeError(SourcePath, "Scene dependency analysis failed: " + Bytes.error()));
			AddReadReceipt(Output, ProjectRoot, SourcePath, *Bytes);
			const std::string Data(Bytes->empty() ? std::string{} :
				std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size()));
			Scene Source;
			auto Loaded = SceneSerializer::Deserialize(Data, Source);
			if (!Loaded) return std::unexpected(MakeError(SourcePath, "Scene dependency analysis failed: " + Loaded.error().Message));
			return ExtractSceneReferences(Output, Owner, Registry, Source);
		}

		std::expected<void, AssetDependencyCatalogError> ExtractPrefabFile(AssetDependencyExtraction& Output,
			const std::filesystem::path& ProjectRoot, const std::filesystem::path& SourcePath, const AssetRecord& Owner,
			const AssetRegistry& Registry)
		{
			auto Bytes = ReadBoundedBytes(SourcePath, MaximumPrefabBytes);
			if (!Bytes) return std::unexpected(MakeError(SourcePath, "Prefab dependency analysis failed: " + Bytes.error()));
			AddReadReceipt(Output, ProjectRoot, SourcePath, *Bytes);
			const std::string Data(Bytes->empty() ? std::string{} :
				std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size()));

			// Use the production prefab path to validate its wrapper/root/subtree policy, then deserialize its nested
			// scene separately so edge owners retain the authored entity UUIDs rather than the instance UUIDs.
			Scene ValidationScene;
			auto ValidatedRoot = PrefabSerializer::Instantiate(Data, ValidationScene);
			if (!ValidatedRoot)
				return std::unexpected(MakeError(SourcePath, "Prefab dependency analysis failed: " + ValidatedRoot.error().Message));

			Json Document;
			try
			{
				Document = Json::parse(Data.begin(), Data.end());
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
			AssetContentFingerprint Content;
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
			AssetContentFingerprint Content;
		};

		bool ReadLittleEndian32(std::span<const std::byte> Data, size_t Offset, uint32_t& Value)
		{
			if (Offset > Data.size() || Data.size() - Offset < 4) return false;
			Value = 0;
			for (size_t Index = 0; Index < 4; ++Index)
				Value |= static_cast<uint32_t>(std::to_integer<uint8_t>(Data[Offset + Index])) << (Index * 8);
			return true;
		}

		std::expected<GltfSourceRead, std::string> ReadGltfSource(
			AssetDependencyExtraction& Output, const std::filesystem::path& ProjectRoot,
			const std::filesystem::path& SourcePath, bool IsGlb)
		{
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(SourcePath, Error);
			if (Error || Size > (IsGlb ? MaximumGltfContainerBytes : MaximumGltfJsonBytes))
				return std::unexpected("glTF source exceeds the supported bounded read limit");

			GltfSourceRead Result;
			Result.FileSize = static_cast<uint64_t>(Size);

			if (!IsGlb)
			{
				auto Bytes = ReadBoundedBytes(SourcePath, MaximumGltfJsonBytes);
				if (!Bytes) return std::unexpected(Bytes.error());
				if (!Bytes->empty()) Result.JsonText.assign(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
				Result.Content = FingerprintBytes(*Bytes);
				AddReadReceipt(Output, ProjectRoot, SourcePath, *Bytes);
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
			AssetFingerprintBuilder Fingerprint("PulseForge.AssetInputBytes.v1");
			Fingerprint.UpdateText("byteLength", std::to_string(Size));
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
				Fingerprint.UpdateBytes("chunk", Chunk);

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
			while (!Result.JsonText.empty() &&
				   (Result.JsonText.back() == '\0' || std::isspace(static_cast<unsigned char>(Result.JsonText.back()))))
				Result.JsonText.pop_back();
			Result.Content = Fingerprint.Finish();
			AddReadReceipt(Output, ProjectRelativeKey(ProjectRoot, SourcePath), Result.FileSize, Result.Content);
			return Result;
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
			const std::filesystem::path& ProjectRoot, const std::filesystem::path& SourcePath, const AssetRecord& Owner,
			const AssetMetadata& Metadata)
		{
			const bool IsGlb = HasAssetExtension(SourcePath, { ".glb" });
			auto SourceRead = ReadGltfSource(Output, ProjectRoot, SourcePath, IsGlb);
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
			auto Settings = GltfMeshImportSettings::Parse(Metadata.ImportSettingsJson);
			if (!Settings) return std::unexpected(MakeError(SourcePath, "glTF import settings are invalid: " + Settings.error()));
			auto CanonicalSettings = Settings->Canonicalize();
			if (!CanonicalSettings)
				return std::unexpected(MakeError(SourcePath, "glTF import settings could not be canonicalized: " + CanonicalSettings.error()));

			std::vector<BufferInput> ExternalBuffers;
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
						}
						continue;
					}

					auto Relative = Assets::Detail::DecodeGltfBufferUri(OriginalUri);
					const std::string Location = "buffers/" + std::to_string(Index) + ".uri";
					if (!Relative || !*Relative)
					{
						const AssetDependencyNode Target = SourceFileNode({});
						const AssetDependencyStatus Status = Assets::Detail::HasUriScheme(OriginalUri)
							? AssetDependencyStatus::Unsupported : AssetDependencyStatus::Invalid;
						Output.Edges.push_back({ Referrer, Target, AssetDependencyEdgeKind::ImportInput, Status, Location, OriginalUri });
						AppendIssue(Output, Referrer, Target, Status, Location,
							Assets::Detail::HasUriScheme(OriginalUri) ? "External glTF buffer URI uses an unsupported scheme"
								: Relative ? "External glTF buffer URI unexpectedly resolved as embedded" : Relative.error());
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
						else if (!Relative) Status = Assets::Detail::HasUriScheme(OriginalUri)
							? AssetDependencyStatus::Unsupported : AssetDependencyStatus::Invalid;
					}
					const std::string Location = "images/" + std::to_string(Index) + ".uri";
					Output.Edges.push_back({ Referrer, Target, AssetDependencyEdgeKind::ImportInput, Status, Location, OriginalUri });
					AppendIssue(Output, Referrer, Target, Status, Location,
						Status == AssetDependencyStatus::Invalid ? "glTF image URI is invalid for a contained project-relative source"
							: Status == AssetDependencyStatus::Unsupported ? "glTF image URI is not consumed by the current static mesh importer"
							: "glTF image URI is not consumed by the current static mesh importer");
				}
			}

			const auto Meshes = Document.find("meshes");
			if (Meshes == Document.end() || !Meshes->is_array() || Meshes->empty() || Meshes->size() > MaximumGltfCollectionEntries)
				return std::unexpected(MakeError(SourcePath, "glTF static mesh source requires a bounded, non-empty meshes array"));
			if (Meshes != Document.end())
			{
				const size_t FirstSubresource = Output.Subresources.size();
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
						const std::string Key = MeshPrimitiveKey(MeshIndex, PrimitiveIndex);
						// Index locators remain stable for unchanged structure; a locator is not proof that output was imported.
						const AssetDependencyNode PrimitiveNode{ Owner.ID, PathToUtf8(Owner.ProjectRelativePath), Key };
						Output.Subresources.push_back({ PrimitiveNode,
							MeshName + " / primitive " + std::to_string(PrimitiveIndex),
							AssetSubresourceClassification::DiscoveredLocator });
					}
				}
				std::vector<Assets::Detail::GltfPrimitiveLocator> Locators;
				Locators.reserve(TotalPrimitives);
				for (size_t MeshIndex = 0; MeshIndex < Meshes->size(); ++MeshIndex)
				{
					const Json& Mesh = (*Meshes)[MeshIndex];
					const auto Primitives = Mesh.find("primitives");
					for (size_t PrimitiveIndex = 0; PrimitiveIndex < Primitives->size(); ++PrimitiveIndex)
						Locators.push_back({ static_cast<uint32_t>(MeshIndex), static_cast<uint32_t>(PrimitiveIndex) });
				}
				const std::optional<uint64_t> BinaryLength = SourceRead->BinaryRange
					? std::optional<uint64_t>(SourceRead->BinaryRange->Length)
					: std::nullopt;
				auto Inspections = Assets::Detail::InspectStaticPrimitives(
					SourceRead->JsonText, SourcePath, ProjectRoot, Locators, BinaryLength);
				if (Inspections.size() != Output.Subresources.size() - FirstSubresource)
					return std::unexpected(MakeError(SourcePath, "glTF primitive inspection returned an inconsistent locator count"));
				for (size_t Index = 0; Index < Inspections.size(); ++Index)
				{
					AssetSubresourceRecord& Subresource = Output.Subresources[FirstSubresource + Index];
					Subresource.Classification = Inspections[Index].Classification;
					if (Subresource.Classification != AssetSubresourceClassification::Invalid &&
						Subresource.Classification != AssetSubresourceClassification::Unsupported)
						continue;
					const AssetDependencyStatus Status = Subresource.Classification == AssetSubresourceClassification::Invalid
						? AssetDependencyStatus::Invalid
						: AssetDependencyStatus::Unsupported;
					AppendIssue(Output, Subresource.Identity, SourceFileNode(PathToUtf8(Owner.ProjectRelativePath)), Status,
						"meshes/" + std::to_string(Locators[Index].MeshIndex) + "/primitives/" +
							std::to_string(Locators[Index].PrimitiveIndex), std::move(Inspections[Index].Diagnostic));
				}
			}

			std::sort(ExternalBuffers.begin(), ExternalBuffers.end(),
				[](const BufferInput& First, const BufferInput& Second)
				{
					if (First.ProjectRelativeKey != Second.ProjectRelativeKey) return First.ProjectRelativeKey < Second.ProjectRelativeKey;
					return First.ByteLength < Second.ByteLength;
				});
			std::vector<AssetImportInputFingerprint> RecipeInputs;
			RecipeInputs.push_back({ PathToUtf8(Owner.ProjectRelativePath), SourceRead->Content });
			for (BufferInput& Input : ExternalBuffers)
			{
				if (Input.Status != AssetDependencyStatus::Resolved) continue;
				std::error_code SizeError;
				const uintmax_t Size = std::filesystem::file_size(Input.AbsolutePath, SizeError);
				if (SizeError || Size < Input.ByteLength || Size > MaximumGltfBufferBytes)
					return std::unexpected(MakeError(Input.AbsolutePath,
						"Could not fingerprint glTF external buffer: file is missing, too short, or over the importer limit"));
				auto InputHash = FingerprintFile(Input.AbsolutePath, static_cast<uint64_t>(Size), MaximumGltfBufferBytes);
				if (!InputHash)
					return std::unexpected(
						MakeError(Input.AbsolutePath, "Could not fingerprint glTF external buffer: " + InputHash.error()));
				Input.Content = *InputHash;
				AddReadReceipt(Output, Input.ProjectRelativeKey, static_cast<uint64_t>(Size), *InputHash);
				RecipeInputs.push_back({ Input.ProjectRelativeKey, *InputHash });
			}
			if (auto Normalized = NormalizeAssetImportRecipeInputs(RecipeInputs); !Normalized)
				return std::unexpected(MakeError(SourcePath, Normalized.error()));

			const std::filesystem::path ProvenancePath = AssetImportProvenanceSerializer::GetSidecarPath(SourcePath);
			std::vector<AssetImportSuccessReceipt> LastSuccesses;
			bool ProvenanceUnavailable = false;
			std::error_code ProvenanceError;
			const auto ProvenanceStatus = std::filesystem::symlink_status(ProvenancePath, ProvenanceError);
			if (ProvenanceError == std::errc::no_such_file_or_directory)
			{
				Output.ReadSet.push_back({ ProjectRelativeKey(ProjectRoot, ProvenancePath), false, 0, {} });
			}
			else if (ProvenanceError || std::filesystem::is_symlink(ProvenanceStatus) || !std::filesystem::is_regular_file(ProvenanceStatus))
			{
				ProvenanceUnavailable = true;
				Output.Issues.push_back({ AssetDependencyIssueCode::InvalidImportProvenance, Referrer,
					SourceFileNode(ProjectRelativeKey(ProjectRoot, ProvenancePath)), "import-result",
					"Import success receipt is unreadable or is not a regular non-symlink file" });
			}
			else
			{
				auto ReceiptBytes = ReadBoundedBytes(ProvenancePath, MaximumProvenanceBytes);
				if (!ReceiptBytes)
				{
					ProvenanceUnavailable = true;
					Output.Issues.push_back({ AssetDependencyIssueCode::InvalidImportProvenance, Referrer,
						SourceFileNode(ProjectRelativeKey(ProjectRoot, ProvenancePath)), "import-result",
						"Import success receipt could not be read: " + ReceiptBytes.error() });
				}
				else
				{
					AddReadReceipt(Output, ProjectRoot, ProvenancePath, *ReceiptBytes);
					const std::string ReceiptText(ReceiptBytes->empty() ? std::string{} :
						std::string(reinterpret_cast<const char*>(ReceiptBytes->data()), ReceiptBytes->size()));
					auto Receipts = AssetImportProvenanceSerializer::DeserializeAll(ReceiptText, ProvenancePath);
					const bool HasForeignSource = Receipts && std::any_of(Receipts->begin(), Receipts->end(),
						[&Owner](const AssetImportSuccessReceipt& Receipt) { return Receipt.Recipe.SourceAsset != Owner.ID; });
					if (!Receipts || HasForeignSource)
					{
						ProvenanceUnavailable = true;
						Output.Issues.push_back({ AssetDependencyIssueCode::InvalidImportProvenance, Referrer,
							SourceFileNode(ProjectRelativeKey(ProjectRoot, ProvenancePath)), "import-result",
							Receipts ? "Import receipt belongs to a different asset UUID" : Receipts.error().Message });
					}
					else LastSuccesses = std::move(*Receipts);
				}
			}

			for (AssetSubresourceRecord& Subresource : Output.Subresources)
			{
				if (Subresource.Identity.Asset != Owner.ID) continue;
				AssetImportRecipe Recipe;
				Recipe.SourceAsset = Owner.ID;
				Recipe.SubresourceKey = Subresource.Identity.SubresourceKey;
				Recipe.ImporterID = GltfImporterID;
				Recipe.ImporterVersion = GltfImporterVersion;
				Recipe.OutputSchemaVersion = GltfOutputSchemaVersion;
				Recipe.EffectiveSettings = *CanonicalSettings;
				Recipe.TargetID = "cpu-generic-v1";
				Recipe.Inputs = RecipeInputs;
				auto Fingerprint = ComputeAssetImportInputFingerprint(Recipe);
				if (!Fingerprint) return std::unexpected(MakeError(SourcePath, "Could not compute glTF import recipe: " + Fingerprint.error()));
				Recipe.InputFingerprint = *Fingerprint;

				AssetImportObservation Observation;
				Observation.Asset = Owner.ID;
				Observation.SubresourceKey = Recipe.SubresourceKey;
				Observation.ImporterID = Recipe.ImporterID;
				Observation.ImporterVersion = Recipe.ImporterVersion;
				Observation.SettingsVersion = GltfMeshImportSettings::CurrentVersion;
				Observation.OutputSchemaVersion = Recipe.OutputSchemaVersion;
				Observation.EffectiveSettings = Recipe.EffectiveSettings;
				Observation.TargetID = Recipe.TargetID;
				Observation.Inputs = Recipe.Inputs;
				Observation.InputFingerprint = Recipe.InputFingerprint;
				if (Subresource.Classification == AssetSubresourceClassification::Invalid ||
					Subresource.Classification == AssetSubresourceClassification::Unsupported || ProvenanceUnavailable)
					Observation.ResultState = AssetImportResultState::Unavailable;
				else if (const auto SuccessfulRecord = std::lower_bound(LastSuccesses.begin(), LastSuccesses.end(),
					Recipe.SubresourceKey, [](const AssetImportSuccessReceipt& Receipt, std::string_view Key)
					{ return Receipt.Recipe.SubresourceKey < Key; });
					SuccessfulRecord != LastSuccesses.end() && SuccessfulRecord->Recipe.SubresourceKey == Recipe.SubresourceKey)
				{
					const AssetImportSuccessReceipt& LastSuccess = *SuccessfulRecord;
					const AssetImportRecipe& SuccessfulRecipe = LastSuccess.Recipe;
					Observation.LastSuccessfulResult = LastSuccess;
					const bool RecipeIdentityMatches = SuccessfulRecipe.SourceAsset == Recipe.SourceAsset &&
						SuccessfulRecipe.SubresourceKey == Recipe.SubresourceKey && SuccessfulRecipe.ImporterID == Recipe.ImporterID &&
						SuccessfulRecipe.ImporterVersion == Recipe.ImporterVersion &&
						SuccessfulRecipe.OutputSchemaVersion == Recipe.OutputSchemaVersion &&
						SuccessfulRecipe.EffectiveSettings == Recipe.EffectiveSettings && SuccessfulRecipe.TargetID == Recipe.TargetID;
					const bool InputsMatch = SuccessfulRecipe.InputFingerprint == Recipe.InputFingerprint;
					Observation.ResultState = RecipeIdentityMatches && InputsMatch
						? AssetImportResultState::SuccessfulInputsMatch
						: AssetImportResultState::SuccessfulInputsDiffer;
					if (RecipeIdentityMatches && InputsMatch && SuccessfulRecipe.Inputs == Recipe.Inputs)
						Subresource.Classification = AssetSubresourceClassification::Imported;
				}
				Output.ImportObservations.push_back(std::move(Observation));
			}
			return {};
		}

		std::expected<void, AssetDependencyCatalogError> AnalyzeMaterial(AssetDependencyExtraction& Output,
			const std::filesystem::path& ProjectRoot, const std::filesystem::path& SourcePath, const AssetRecord& Owner,
			const AssetRegistry& Registry)
		{
			auto Bytes = ReadBoundedBytes(SourcePath, 1024ull * 1024ull);
			if (!Bytes) return std::unexpected(MakeError(SourcePath, "Material dependency analysis failed: " + Bytes.error()));
			AddReadReceipt(Output, ProjectRoot, SourcePath, *Bytes);
			const std::string Data(Bytes->empty() ? std::string{} :
				std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size()));
			auto Material = MaterialAssetSerializer::Deserialize(Data);
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
			auto Metadata = ReadAssetMetadata(Output, AbsoluteProjectRoot, *Resolved, Record);
			if (!Metadata) return std::unexpected(Metadata.error());

			std::expected<void, AssetDependencyCatalogError> Result;
			if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Material))
				Result = AnalyzeMaterial(Output, AbsoluteProjectRoot, *Resolved, Record, Registry);
			else if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Scene))
				Result = ExtractSceneFile(Output, AbsoluteProjectRoot, *Resolved, Record, Registry);
			else if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Prefab))
				Result = ExtractPrefabFile(Output, AbsoluteProjectRoot, *Resolved, Record, Registry);
			else if (HasAssetTypeExtension(Record.ProjectRelativePath, AssetFileType::Mesh))
				Result = AnalyzeGltf(Output, AbsoluteProjectRoot, *Resolved, Record, *Metadata);
			else continue;
			if (!Result) return std::unexpected(std::move(Result.error()));
		}

		std::set<std::string> ReadPaths;
		for (const AssetDependencyInputReceipt& Receipt : Output.ReadSet) ReadPaths.insert(Receipt.ProjectRelativePath);
		for (const AssetDependencyEdge& Edge : Output.Edges)
		{
			if (Edge.Kind != AssetDependencyEdgeKind::RuntimeReference || Edge.Status != AssetDependencyStatus::Resolved ||
				Edge.Target.Asset.IsNil())
				continue;
			const auto TargetRecord = Registry.Find(Edge.Target.Asset);
			if (!TargetRecord) continue;
			const std::string Key = PathToUtf8(TargetRecord->ProjectRelativePath);
			if (!ReadPaths.insert(Key).second) continue;
			auto TargetPath = AssetPathResolver::ResolveManagedSourcePath(AbsoluteProjectRoot, TargetRecord->ProjectRelativePath);
			if (!TargetPath)
				return std::unexpected(MakeError(TargetRecord->ProjectRelativePath,
					"Resolved runtime dependency became unavailable during source fingerprinting: " + TargetPath.error().Message));
			std::error_code SizeError;
			const uintmax_t Size = std::filesystem::file_size(*TargetPath, SizeError);
			if (SizeError || Size > MaximumGltfContainerBytes)
				return std::unexpected(MakeError(*TargetPath,
					"Resolved runtime dependency exceeds the bounded source fingerprint limit"));
			auto Content = FingerprintFile(*TargetPath, static_cast<uint64_t>(Size), MaximumGltfContainerBytes);
			if (!Content)
				return std::unexpected(MakeError(*TargetPath,
					"Resolved runtime dependency could not be fingerprinted: " + Content.error()));
			AddReadReceipt(Output, Key, static_cast<uint64_t>(Size), *Content);
		}
		auto Cycles = AppendRuntimeCycles(Output, AbsoluteProjectRoot, Records);
		if (!Cycles) return std::unexpected(std::move(Cycles.error()));

		std::sort(Output.ReadSet.begin(), Output.ReadSet.end(), [](const auto& First, const auto& Second)
			{ return First.ProjectRelativePath < Second.ProjectRelativePath; });
		std::vector<AssetDependencyInputReceipt> UniqueReadSet;
		UniqueReadSet.reserve(Output.ReadSet.size());
		for (AssetDependencyInputReceipt& Receipt : Output.ReadSet)
		{
			if (!UniqueReadSet.empty() && UniqueReadSet.back().ProjectRelativePath == Receipt.ProjectRelativePath)
			{
				if (UniqueReadSet.back() != Receipt)
					return std::unexpected(MakeError(AbsoluteProjectRoot / std::filesystem::u8path(Receipt.ProjectRelativePath),
						"One input changed while dependency analysis was reading it; candidate read set is inconsistent"));
				continue;
			}
			UniqueReadSet.push_back(std::move(Receipt));
		}
		Output.ReadSet = std::move(UniqueReadSet);

		return Output;
	}

	std::expected<void, AssetDependencyCatalogError> AssetDependencyExtractor::ValidateInputReceipts(
		const std::filesystem::path& ProjectRoot, std::span<const AssetDependencyInputReceipt> ReadSet)
	{
		std::error_code RootError;
		const std::filesystem::path AbsoluteRoot = std::filesystem::absolute(ProjectRoot, RootError).lexically_normal();
		if (RootError || !std::filesystem::is_directory(AbsoluteRoot, RootError) || RootError)
			return std::unexpected(MakeError(ProjectRoot, "Project root is not accessible during dependency read-set validation"));

		std::string PreviousKey;
		for (const AssetDependencyInputReceipt& Receipt : ReadSet)
		{
			const std::filesystem::path Relative = std::filesystem::u8path(Receipt.ProjectRelativePath);
			if (!IsSafeReadSetKey(Relative) || PathToUtf8(Relative) != Receipt.ProjectRelativePath ||
				(!PreviousKey.empty() && PreviousKey >= Receipt.ProjectRelativePath))
				return std::unexpected(MakeError(Relative, "Dependency input read set contains an invalid or non-deterministic path key"));
			PreviousKey = Receipt.ProjectRelativePath;

			if (!Receipt.Exists)
			{
				const std::filesystem::path Candidate = AbsoluteRoot / Relative;
				std::filesystem::path Current = AbsoluteRoot;
				auto Component = Relative.begin();
				for (; Component != Relative.end(); ++Component)
				{
					Current /= *Component;
					std::error_code StatusError;
					const auto Status = std::filesystem::symlink_status(Current, StatusError);
					const bool IsFinal = std::next(Component) == Relative.end();
					if (StatusError == std::errc::no_such_file_or_directory ||
						(!StatusError && Status.type() == std::filesystem::file_type::not_found))
					{
						if (IsFinal) break;
						return std::unexpected(MakeError(Candidate, "An input parent directory disappeared during dependency analysis"));
					}
					if (StatusError || std::filesystem::is_symlink(Status) || (IsFinal ? !std::filesystem::is_regular_file(Status)
						: !std::filesystem::is_directory(Status)))
						return std::unexpected(MakeError(Candidate, "An expected-absent input appeared or its path became unsafe during dependency analysis"));
					if (IsFinal)
						return std::unexpected(MakeError(Candidate, "An input recorded as absent appeared during dependency analysis"));
				}
				continue;
			}

			auto ManagedPath = AssetPathResolver::ResolveManagedSourcePath(AbsoluteRoot, Relative);
			if (!ManagedPath)
				return std::unexpected(MakeError(ManagedPath.error().Path,
					"Dependency input became unavailable or unsafe: " + ManagedPath.error().Message));
			auto Content = FingerprintFile(*ManagedPath, Receipt.ByteLength, MaximumGltfContainerBytes);
			if (!Content)
				return std::unexpected(MakeError(*ManagedPath, "Dependency input changed or could not be revalidated: " + Content.error()));
			if (*Content != Receipt.Content)
				return std::unexpected(MakeError(*ManagedPath, "Dependency input content changed after extraction and before publication"));
		}
		return {};
	}
} // namespace PulseForge
