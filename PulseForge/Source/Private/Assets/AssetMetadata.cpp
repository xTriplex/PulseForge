#include "Core/PulseForgePCH.h"
#include "Assets/AssetMetadata.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr std::string_view AssetMetadataFormat = "PulseForgeAssetMeta";

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

		struct TemporaryFileCleanup
		{
			std::filesystem::path Path;

			~TemporaryFileCleanup()
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		};
	}

	std::filesystem::path AssetMetadataSerializer::GetSidecarPath(const std::filesystem::path& SourceAssetPath)
	{
		std::filesystem::path SidecarPath = SourceAssetPath;
		SidecarPath += ".meta";
		return SidecarPath;
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

		try
		{
			std::ifstream Input(SidecarPath, std::ios::binary);
			if (!Input.is_open())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileOpenFailed,
					SidecarPath,
					"Could not open asset metadata: " + PathForMessage(SidecarPath)));
			}

			const std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
			{
				return std::unexpected(MakeMetadataError(
					AssetMetadataErrorCode::FileReadFailed,
					SidecarPath,
					"Could not read asset metadata completely: " + PathForMessage(SidecarPath)));
			}

			const Json Document = Json::parse(Data);
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
				? Version->get<uint64_t>() == CurrentVersion
				: Version->get<int64_t>() == CurrentVersion;
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

			return AssetMetadata{ *ParsedID, CurrentVersion };
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
				AssetMetadataErrorCode::FileReadFailed,
				SidecarPath,
				std::string("Could not load asset metadata: ") + Exception.what()));
		}
	}

	std::expected<AssetMetadata, AssetMetadataError> AssetMetadataSerializer::CreateForNewAsset(
		const std::filesystem::path& SourceAssetPath)
	{
		if (SourceAssetPath.empty() || SourceAssetPath.filename().empty() || SourceAssetPath.extension() == ".meta")
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
				{ "importSettings", Json::object() }
			});
			std::filesystem::path TemporaryPath = SidecarPath;
			TemporaryPath += "." + GeneratedID->ToString() + ".tmp";
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

			return AssetMetadata{ *GeneratedID, CurrentVersion };
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeMetadataError(
				AssetMetadataErrorCode::FileWriteFailed,
				SourceAssetPath,
				std::string("Asset metadata creation failed: ") + Exception.what()));
		}
	}
}
