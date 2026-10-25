#include "Assets/AssetMetadata.h"
#include "Assets/AssetTypes.h"
#include "Core/PlatformServices.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr std::string_view AssetMetadataFormat = "PulseForgeAssetMeta";
		constexpr size_t MaxMetadataBytes = 1024 * 1024;

		class DuplicateJsonKeyError final : public std::runtime_error
		{
		public:
			DuplicateJsonKeyError() : std::runtime_error("JSON object contains a duplicate key") {}
		};

		Json ParseUniqueJson(std::string_view Data)
		{
			std::vector<std::set<std::string>> ObjectKeys;
			bool HasDuplicateKey = false;
			const auto Callback = [&ObjectKeys, &HasDuplicateKey](int, Json::parse_event_t Event, Json& Parsed)
			{
				if (Event == Json::parse_event_t::object_start)
					ObjectKeys.emplace_back();
				else if (Event == Json::parse_event_t::key && !ObjectKeys.empty())
					HasDuplicateKey |= !ObjectKeys.back().insert(Parsed.get<std::string>()).second;
				else if (Event == Json::parse_event_t::object_end && !ObjectKeys.empty())
					ObjectKeys.pop_back();
				return true;
			};
			Json Document = Json::parse(Data, Callback);
			if (HasDuplicateKey)
				throw DuplicateJsonKeyError();
			return Document;
		}

		AssetMetadataError MakeMetadataError(
			AssetMetadataErrorCode Code,
			const std::filesystem::path& Path,
			std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		std::string PathForMessage(const std::filesystem::path& Path)
		{
			return Path.generic_string();
		}

		std::expected<AssetMetadata, AssetMetadataError> ValidateMetadataDocument(
			const Json& Document,
			const std::filesystem::path& SidecarPath)
		{
			if (!Document.is_object())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument,
					SidecarPath,
					"Asset metadata root must be a JSON object"));
			}

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string() || Format->get<std::string>() != AssetMetadataFormat)
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument,
					SidecarPath,
					"Asset metadata has a missing or unsupported format identifier"));
			}

			const auto Version = Document.find("version");
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument,
					SidecarPath,
					"Asset metadata requires a numeric version"));
			}

			const bool SupportedVersion = Version->is_number_unsigned()
				? Version->get<uint64_t>() == AssetMetadataSerializer::CurrentVersion
				: Version->get<int64_t>() == AssetMetadataSerializer::CurrentVersion;
			if (!SupportedVersion)
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::UnsupportedVersion,
					SidecarPath,
					"Asset metadata version is not supported"));
			}

			const auto SerializedID = Document.find("uuid");
			if (SerializedID == Document.end() || !SerializedID->is_string())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument,
					SidecarPath,
					"Asset metadata requires a UUID string"));
			}

			const auto ImportSettings = Document.find("importSettings");
			if (ImportSettings == Document.end() || !ImportSettings->is_object())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument,
					SidecarPath,
					"Asset metadata importSettings field must be an object"));
			}

			const auto ParsedID = UUID::Parse(SerializedID->get<std::string>());
			if (!ParsedID || ParsedID->IsNil())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidIdentifier,
					SidecarPath,
					ParsedID ? "Asset UUID must not be nil" : ParsedID.error().Message));
			}

			return AssetMetadata{ *ParsedID, AssetMetadataSerializer::CurrentVersion, ImportSettings->dump() };
		}

		std::expected<std::string, AssetMetadataError> ReadBoundedFile(const std::filesystem::path& Path)
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileOpenFailed, Path, "Could not open asset metadata: " + PathForMessage(Path)));
			std::string Data(MaxMetadataBytes + 1, '\0');
			Input.read(Data.data(), static_cast<std::streamsize>(Data.size()));
			const std::streamsize Count = Input.gcount();
			if (Input.bad())
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileReadFailed, Path, "Could not read asset metadata completely: " + PathForMessage(Path)));
			if (Count > static_cast<std::streamsize>(MaxMetadataBytes))
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument, Path, "Asset metadata exceeds its size limit"));
			Data.resize(static_cast<size_t>(Count));
			return Data;
		}

		std::expected<void, AssetMetadataError> ReplaceMetadataFile(
			const std::filesystem::path& SidecarPath,
			const AssetID& ID,
			const Json& Document)
		{
			const std::string Serialized = Document.dump(2) + "\n";
			if (Serialized.size() > MaxMetadataBytes)
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument, SidecarPath, "Serialized asset metadata exceeds its size limit"));
			std::error_code DestinationError;
			const std::filesystem::file_status DestinationStatus = std::filesystem::symlink_status(SidecarPath, DestinationError);
			if (DestinationError != std::errc::no_such_file_or_directory && DestinationError)
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed, SidecarPath,
					"Could not inspect asset metadata destination: " + DestinationError.message()));
			if (!DestinationError && (std::filesystem::is_symlink(DestinationStatus) ||
				!std::filesystem::is_regular_file(DestinationStatus)))
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed, SidecarPath,
					"Asset metadata destination must be a regular, non-symlink file"));
			auto TemporaryID = UUID::Generate();
			if (!TemporaryID)
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::UUIDGenerationFailed, SidecarPath, TemporaryID.error().Message));
			std::filesystem::path Temporary = SidecarPath;
			Temporary += "." + ID.ToString() + "." + TemporaryID->ToString() + ".settings.tmp";
			struct Cleanup
			{
				std::filesystem::path Path;
				~Cleanup() { std::error_code Error; std::filesystem::remove(Path, Error); }
			} CleanupTemporary{ Temporary };
			std::ofstream Output(Temporary, std::ios::binary | std::ios::trunc);
			if (!Output.is_open())
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed, SidecarPath, "Could not open temporary asset metadata"));
			Output.write(Serialized.data(), static_cast<std::streamsize>(Serialized.size()));
			Output.flush();
			Output.close();
			if (!Output)
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed, SidecarPath, "Could not write complete asset metadata"));
			auto Replaced = Platform::ReplaceFileAtomically(Temporary, SidecarPath);
			if (!Replaced)
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed, SidecarPath, Replaced.error().Message));
			return {};
		}

		Json DefaultGltfSettings(const std::filesystem::path& SourcePath)
		{
			std::string Extension = SourcePath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](unsigned char Character)
				{ return static_cast<char>(std::tolower(Character)); });
			if (Extension == ".gltf" || Extension == ".glb")
				return Json::object({ { "gltfStaticMesh", Json::object({
					{ "version", 1 }, { "missingNormalsPolicy", "GenerateSmooth" } }) } });
			return Json::object();
		}

		struct TemporaryFileCleanup
		{
			std::filesystem::path Path;

			~TemporaryFileCleanup()
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		};

		std::expected<void, AssetMetadataError> WriteNewMetadataFile(
			const std::filesystem::path& SidecarPath,
			const AssetID& ID,
			const Json& Document)
		{
			std::filesystem::path TemporaryPath = SidecarPath;
			TemporaryPath += "." + ID.ToString() + ".tmp";
			TemporaryFileCleanup Cleanup{ TemporaryPath };

			const std::string Serialized = Document.dump(2) + "\n";
			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output.is_open())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed,
					SidecarPath,
					"Could not create temporary asset metadata beside: " + PathForMessage(SidecarPath)));
			}
			Output.write(Serialized.data(), static_cast<std::streamsize>(Serialized.size()));
			Output.flush();
			if (!Output)
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed,
					SidecarPath,
					"Could not write asset metadata: " + PathForMessage(SidecarPath)));
			}
			Output.close();
			if (Output.fail())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed,
					SidecarPath,
					"Could not finish writing asset metadata: " + PathForMessage(SidecarPath)));
			}

			std::error_code FileError;
			const bool Created = std::filesystem::copy_file(
				TemporaryPath,
				SidecarPath,
				std::filesystem::copy_options::none,
				FileError);
			if (!Created || FileError)
			{
				if (FileError == std::errc::file_exists)
				{
					return std::unexpected(MakeMetadataError(
						AssetMetadataErrorCode::MetadataAlreadyExists,
						SidecarPath,
						"Asset metadata appeared during creation and was not overwritten: " + PathForMessage(SidecarPath)));
				}
				const std::string FailureReason = FileError ? FileError.message() : "filesystem did not create the sidecar";
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileWriteFailed,
					SidecarPath,
					"Could not create asset metadata: " + FailureReason));
			}

			return {};
		}
	}

	std::filesystem::path AssetMetadataSerializer::GetSidecarPath(const std::filesystem::path& SourceAssetPath)
	{
		std::filesystem::path SidecarPath = SourceAssetPath;
		SidecarPath += ".meta";
		return SidecarPath;
	}

	std::expected<AssetMetadata, AssetMetadataError> AssetMetadataSerializer::Deserialize(
		std::string_view Data,
		const std::filesystem::path& SidecarPath)
	{
		if (Data.size() > MaxMetadataBytes)
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument, SidecarPath, "Asset metadata exceeds its size limit"));
		try
		{
			return ValidateMetadataDocument(ParseUniqueJson(Data), SidecarPath);
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument,
				SidecarPath,
				std::string("Asset metadata JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument,
				SidecarPath,
				std::string("Could not decode asset metadata: ") + Exception.what()));
		}
	}

	std::expected<AssetMetadata, AssetMetadataError> AssetMetadataSerializer::LoadFromFile(
		const std::filesystem::path& SidecarPath)
	{
		if (SidecarPath.empty())
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidPath,
				SidecarPath,
				"Asset metadata path must not be empty"));
		}

		auto Data = ReadBoundedFile(SidecarPath);
		if (!Data) return std::unexpected(std::move(Data.error()));
		return Deserialize(*Data, SidecarPath);
	}

	std::expected<AssetMetadata, AssetMetadataError> AssetMetadataSerializer::CreateForNewAsset(
		const std::filesystem::path& SourceAssetPath)
	{
		if (SourceAssetPath.empty() || SourceAssetPath.filename().empty() || IsMetadataSidecarPath(SourceAssetPath))
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidPath,
				SourceAssetPath,
				"New managed assets require a non-empty source path that does not end in .meta"));
		}

		try
		{
			std::error_code FileError;
			const std::filesystem::file_status SourceStatus = std::filesystem::symlink_status(SourceAssetPath, FileError);
			if (FileError || !std::filesystem::is_regular_file(SourceStatus))
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::SourceAssetMissing,
					SourceAssetPath,
					"Cannot assign asset identity because the source file is missing or is not a regular file: " +
						PathForMessage(SourceAssetPath)));
			}

			const std::filesystem::path SidecarPath = GetSidecarPath(SourceAssetPath);
			FileError.clear();
			const std::filesystem::file_status ExistingSidecarStatus = std::filesystem::symlink_status(SidecarPath, FileError);
			if (FileError == std::errc::no_such_file_or_directory)
				FileError.clear();
			else if (FileError)
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileOpenFailed,
					SidecarPath,
					"Could not inspect the asset metadata path: " + FileError.message()));
			}
			else if (ExistingSidecarStatus.type() != std::filesystem::file_type::not_found)
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::MetadataAlreadyExists,
					SidecarPath,
					"Asset metadata already exists and will not be overwritten: " + PathForMessage(SidecarPath)));
			}

			const auto GeneratedID = UUID::Generate();
			if (!GeneratedID)
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::UUIDGenerationFailed,
					SourceAssetPath,
					GeneratedID.error().Message));
			}

			const Json Document = Json::object({
				{ "format", AssetMetadataFormat },
				{ "version", CurrentVersion },
				{ "uuid", GeneratedID->ToString() },
				{ "importSettings", DefaultGltfSettings(SourceAssetPath) }
			});

			if (auto WriteResult = WriteNewMetadataFile(SidecarPath, *GeneratedID, Document); !WriteResult)
				return std::unexpected(std::move(WriteResult.error()));

			return AssetMetadata{ *GeneratedID, CurrentVersion, Document["importSettings"].dump() };
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::FileWriteFailed,
				SourceAssetPath,
				std::string("Asset metadata creation failed: ") + Exception.what()));
		}
	}

	std::expected<void, AssetMetadataError> AssetMetadataSerializer::SaveImportSettings(
		const std::filesystem::path& SidecarPath,
		const AssetMetadata& ExpectedMetadata,
		std::string_view ImportSettingsJson)
	{
		if (SidecarPath.empty() || ExpectedMetadata.ID.IsNil())
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidPath, SidecarPath, "Settings update requires a sidecar path and non-nil asset identity"));
		if (ImportSettingsJson.size() > 64 * 1024)
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument, SidecarPath, "Import settings exceed their size limit"));
		auto ExistingData = ReadBoundedFile(SidecarPath);
		if (!ExistingData) return std::unexpected(std::move(ExistingData.error()));
		try
		{
			Json Document = ParseUniqueJson(*ExistingData);
			auto ExistingMetadata = ValidateMetadataDocument(Document, SidecarPath);
			if (!ExistingMetadata) return std::unexpected(std::move(ExistingMetadata.error()));
			if (ExistingMetadata->ID != ExpectedMetadata.ID || ExistingMetadata->Version != ExpectedMetadata.Version)
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidIdentifier, SidecarPath,
					"Asset metadata identity changed before import settings could be saved"));
			Json Settings = ParseUniqueJson(ImportSettingsJson);
			if (!Settings.is_object())
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::InvalidDocument, SidecarPath, "Import settings must be a JSON object"));
			Document["importSettings"] = std::move(Settings);
			return ReplaceMetadataFile(SidecarPath, ExpectedMetadata.ID, Document);
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument, SidecarPath,
				std::string("Import settings JSON is invalid: ") + Exception.what()));
		}
		catch (const DuplicateJsonKeyError& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument, SidecarPath, Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::FileWriteFailed, SidecarPath,
				std::string("Could not save import settings: ") + Exception.what()));
		}
	}

	std::expected<AssetMetadata, AssetMetadataError> AssetMetadataSerializer::CreateDuplicateForNewAsset(
		const std::filesystem::path& SourceAssetPath,
		const std::filesystem::path& DuplicateAssetPath)
	{
		if (SourceAssetPath.empty() || IsMetadataSidecarPath(SourceAssetPath) || DuplicateAssetPath.empty() ||
			DuplicateAssetPath.filename().empty() || IsMetadataSidecarPath(DuplicateAssetPath))
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidPath,
				DuplicateAssetPath,
				"Asset duplication requires valid source and destination file paths"));
		}

		try
		{
			std::error_code FileError;
			const auto SourceStatus = std::filesystem::symlink_status(SourceAssetPath, FileError);
			if (FileError || !std::filesystem::is_regular_file(SourceStatus))
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::SourceAssetMissing,
					SourceAssetPath,
					"Cannot duplicate metadata because the source asset is missing or is not a regular file: " +
						PathForMessage(SourceAssetPath)));
			}

			FileError.clear();
			const auto DuplicateStatus = std::filesystem::symlink_status(DuplicateAssetPath, FileError);
			if (FileError || !std::filesystem::is_regular_file(DuplicateStatus))
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::DestinationAssetMissing,
					DuplicateAssetPath,
					"Duplicate metadata requires an existing regular destination asset: " + PathForMessage(DuplicateAssetPath)));
			}

			const std::filesystem::path SourceSidecarPath = GetSidecarPath(SourceAssetPath);
			std::ifstream Input(SourceSidecarPath, std::ios::binary);
			if (!Input.is_open())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileOpenFailed,
					SourceSidecarPath,
					"Could not open source metadata for duplication: " + PathForMessage(SourceSidecarPath)));
			}
			std::string Data(MaxMetadataBytes + 1, '\0');
			Input.read(Data.data(), static_cast<std::streamsize>(Data.size()));
			const std::streamsize Count = Input.gcount();
			if (Input.bad())
				return std::unexpected(MakeMetadataError(AssetMetadataErrorCode::FileReadFailed, SourceSidecarPath,
					"Could not read source metadata completely: " + PathForMessage(SourceSidecarPath)));
			if (Count > static_cast<std::streamsize>(MaxMetadataBytes))
				return std::unexpected(MakeMetadataError(AssetMetadataErrorCode::InvalidDocument, SourceSidecarPath,
					"Source asset metadata exceeds its size limit"));
			Data.resize(static_cast<size_t>(Count));

			Json Document = ParseUniqueJson(Data);
			if (auto ParsedMetadata = ValidateMetadataDocument(Document, SourceSidecarPath); !ParsedMetadata)
				return std::unexpected(std::move(ParsedMetadata.error()));
			const auto GeneratedID = UUID::Generate();
			if (!GeneratedID)
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::UUIDGenerationFailed,
					DuplicateAssetPath,
					GeneratedID.error().Message));
			}
			Document["uuid"] = GeneratedID->ToString();

			const std::filesystem::path DuplicateSidecarPath = GetSidecarPath(DuplicateAssetPath);
			if (auto WriteResult = WriteNewMetadataFile(DuplicateSidecarPath, *GeneratedID, Document); !WriteResult)
				return std::unexpected(std::move(WriteResult.error()));

			return AssetMetadata{ *GeneratedID, CurrentVersion };
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument,
				SourceAssetPath,
				std::string("Source asset metadata became invalid during duplication: ") + Exception.what()));
		}
		catch (const DuplicateJsonKeyError& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::InvalidDocument, GetSidecarPath(SourceAssetPath), Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::FileWriteFailed,
				DuplicateAssetPath,
				std::string("Could not duplicate asset metadata: ") + Exception.what()));
		}
	}
}
