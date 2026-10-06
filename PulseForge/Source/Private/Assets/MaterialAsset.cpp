#include "Core/PulseForgePCH.h"
#include "Assets/MaterialAsset.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr int64_t MaterialFormatVersion = 1;
		constexpr size_t MaximumMaterialFileSize = 1024 * 1024;
		constexpr std::string_view MaterialFormatName = "PulseForgeMaterial";

		MaterialAssetError MakeError(MaterialAssetErrorCode Code, std::string Message)
		{
			return { Code, {}, std::move(Message) };
		}

		std::string PathForMessage(const std::filesystem::path& Path)
		{
			const std::u8string UTF8Path = Path.u8string();
			return { reinterpret_cast<const char*>(UTF8Path.data()), UTF8Path.size() };
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

	std::expected<void, MaterialAssetError> ValidateMaterialAssetDescription(const MaterialAssetDesc& Description)
	{
		if (Description.BaseColorTexture.IsNil())
		{
			return std::unexpected(MakeError(
				MaterialAssetErrorCode::InvalidDescription,
				"Material requires a non-nil base-color texture asset UUID"));
		}

		for (int Component = 0; Component < 4; ++Component)
		{
			const float Value = Description.BaseColorFactor[Component];
			if (!std::isfinite(Value) || Value < 0.0f || Value > 1.0f)
			{
				return std::unexpected(MakeError(
					MaterialAssetErrorCode::InvalidDescription,
					"Base-color factor components must be finite values in the range [0, 1]"));
			}
		}
		return {};
	}

	std::expected<std::string, MaterialAssetError> MaterialAssetSerializer::Serialize(const MaterialAssetDesc& Material)
	{
		if (auto Validation = ValidateMaterialAssetDescription(Material); !Validation)
			return std::unexpected(Validation.error());

		try
		{
			Json Document = Json::object();
			Document["format"] = MaterialFormatName;
			Document["version"] = MaterialFormatVersion;
			Document["baseColorTexture"] = Material.BaseColorTexture.ToString();
			Document["baseColorFactor"] = {
				Material.BaseColorFactor.r,
				Material.BaseColorFactor.g,
				Material.BaseColorFactor.b,
				Material.BaseColorFactor.a
			};
			return Document.dump(2) + "\n";
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				MaterialAssetErrorCode::InvalidDescription,
				std::string("Material serialization failed: ") + Exception.what()));
		}
	}

	std::expected<MaterialAssetDesc, MaterialAssetError> MaterialAssetSerializer::Deserialize(std::string_view Data)
	{
		try
		{
			const Json Document = Json::parse(Data.begin(), Data.end());
			if (!Document.is_object())
				return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "Material document root must be an object"));

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string())
				return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "Material document is missing its format identifier"));
			if (Format->get<std::string>() != MaterialFormatName)
				return std::unexpected(MakeError(MaterialAssetErrorCode::UnsupportedFormat, "Document is not a PulseForge material"));

			const auto Version = Document.find("version");
			if (Version == Document.end() || !Version->is_number_integer() || Version->get<int64_t>() != MaterialFormatVersion)
				return std::unexpected(MakeError(MaterialAssetErrorCode::UnsupportedVersion, "Material document version is not supported"));

			const auto Texture = Document.find("baseColorTexture");
			const auto Factor = Document.find("baseColorFactor");
			if (Texture == Document.end() || !Texture->is_string() || Factor == Document.end() ||
				!Factor->is_array() || Factor->size() != 4)
			{
				return std::unexpected(MakeError(
					MaterialAssetErrorCode::InvalidDocument,
					"Material requires a base-color texture UUID and a four-component base-color factor"));
			}

			const auto TextureAsset = UUID::Parse(Texture->get<std::string>());
			if (!TextureAsset || TextureAsset->IsNil())
			{
				return std::unexpected(MakeError(
					MaterialAssetErrorCode::InvalidDocument,
					TextureAsset ? "Base-color texture UUID must not be nil" : TextureAsset.error().Message));
			}

			MaterialAssetDesc Material;
			Material.BaseColorTexture = *TextureAsset;
			for (size_t Index = 0; Index < 4; ++Index)
			{
				if (!(*Factor)[Index].is_number())
					return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "Base-color factor must contain numeric values"));
				Material.BaseColorFactor[static_cast<int>(Index)] = (*Factor)[Index].get<float>();
			}

			if (auto Validation = ValidateMaterialAssetDescription(Material); !Validation)
				return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, Validation.error().Message));
			return Material;
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(
				MaterialAssetErrorCode::InvalidDocument,
				std::string("Material JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				MaterialAssetErrorCode::InvalidDocument,
				std::string("Material deserialization failed: ") + Exception.what()));
		}
	}

	std::expected<void, MaterialAssetError> MaterialAssetSerializer::SaveToFile(
		const MaterialAssetDesc& Material,
		const std::filesystem::path& Path)
	{
		if (Path.empty())
			return std::unexpected(MakeError(MaterialAssetErrorCode::FileWriteFailed, "Material file path must not be empty"));
		const auto Serialized = Serialize(Material);
		if (!Serialized)
			return std::unexpected(Serialized.error());
		if (Serialized->size() > MaximumMaterialFileSize ||
			Serialized->size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
		{
			return std::unexpected(MakeError(MaterialAssetErrorCode::FileWriteFailed, "Serialized material exceeds the supported file size"));
		}

		try
		{
			const auto TemporaryIdentifier = UUID::Generate();
			if (!TemporaryIdentifier)
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileWriteFailed, TemporaryIdentifier.error().Message));

			std::filesystem::path TemporaryPath = Path;
			TemporaryPath += "." + TemporaryIdentifier->ToString() + ".tmp";
			TemporaryFileCleanup Cleanup{ TemporaryPath };
			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output.is_open())
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileWriteFailed, "Could not open material file for writing: " + PathForMessage(Path)));
			Output.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
			Output.flush();
			if (!Output)
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileWriteFailed, "Could not write the complete material document"));
			Output.close();
			if (Output.fail())
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileWriteFailed, "Could not finish writing the material document"));

			std::error_code ReplaceError;
			std::filesystem::rename(TemporaryPath, Path, ReplaceError);
			if (ReplaceError)
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileReplaceFailed, ReplaceError.message()));
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				MaterialAssetErrorCode::FileWriteFailed,
				std::string("Material file save failed: ") + Exception.what()));
		}
	}

	std::expected<MaterialAssetDesc, MaterialAssetError> MaterialAssetSerializer::LoadFromFile(const std::filesystem::path& Path)
	{
		if (Path.empty())
			return std::unexpected(MakeError(MaterialAssetErrorCode::FileReadFailed, "Material file path must not be empty"));
		try
		{
			std::error_code Error;
			const uintmax_t FileSize = std::filesystem::file_size(Path, Error);
			if (Error)
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileReadFailed, "Could not inspect material file: " + Error.message()));
			if (FileSize > MaximumMaterialFileSize)
				return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "Material file exceeds the 1 MiB size limit"));

			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileReadFailed, "Could not open material file: " + PathForMessage(Path)));
			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
				return std::unexpected(MakeError(MaterialAssetErrorCode::FileReadFailed, "Could not read the complete material file"));
			return Deserialize(Data);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				MaterialAssetErrorCode::FileReadFailed,
				std::string("Material file load failed: ") + Exception.what()));
		}
	}
}
