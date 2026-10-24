#include "Assets/MaterialAsset.h"
#include "Core/BoundedInput.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr int64_t MaterialFormatVersion = 4;
		constexpr size_t MaximumMaterialFileSize = 1024 * 1024;
		constexpr std::string_view MaterialFormatName = "PulseForgeMaterial";

		std::optional<std::string> FindUnknownField(const Json& Object, int64_t Version)
		{
			for (const auto& [Key, Value] : Object.items())
			{
				(void)Value;
				const bool CommonField = Key == "format" || Key == "version" || Key == "baseColorTexture" || Key == "baseColorFactor";
				const bool VersionTwoField = Key == "metallicFactor" || Key == "roughnessFactor";
				const bool VersionThreeField = Key == "metallicRoughnessTexture" || Key == "normalTexture" || Key == "occlusionTexture" ||
											   Key == "emissiveTexture" || Key == "normalScale" || Key == "occlusionStrength" ||
											   Key == "emissiveFactor";
				const bool VersionFourField = Key == "alphaMode" || Key == "alphaCutoff" || Key == "doubleSided";
				if (!CommonField && !(Version >= 2 && VersionTwoField) && !(Version >= 3 && VersionThreeField) &&
					!(Version >= 4 && VersionFourField))
					return Key;
			}
			return std::nullopt;
		}

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
		if (!std::isfinite(Description.MetallicFactor) || Description.MetallicFactor < 0.0f || Description.MetallicFactor > 1.0f)
			return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDescription,
				"Metallic factor must be a finite value in the range [0, 1]"));
		if (!std::isfinite(Description.RoughnessFactor) || Description.RoughnessFactor < 0.0f || Description.RoughnessFactor > 1.0f)
			return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDescription,
				"Roughness factor must be a finite value in the range [0, 1]"));
		if (!std::isfinite(Description.NormalScale) || Description.NormalScale < 0.0f || Description.NormalScale > 4.0f)
			return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDescription,
				"Normal scale must be a finite value in the range [0, 4]"));
		if (!std::isfinite(Description.OcclusionStrength) ||
			Description.OcclusionStrength < 0.0f || Description.OcclusionStrength > 1.0f)
			return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDescription,
				"Occlusion strength must be a finite value in the range [0, 1]"));
		if (Description.AlphaMode != MaterialAlphaMode::Opaque && Description.AlphaMode != MaterialAlphaMode::Mask &&
			Description.AlphaMode != MaterialAlphaMode::Blend)
			return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDescription, "Material alpha mode is unsupported"));
		if (!std::isfinite(Description.AlphaCutoff) || Description.AlphaCutoff < 0.0f || Description.AlphaCutoff > 1.0f)
			return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDescription,
				"Alpha cutoff must be a finite value in the range [0, 1]"));
		for (int Component = 0; Component < 3; ++Component)
		{
			const float Value = Description.EmissiveFactor[Component];
			if (!std::isfinite(Value) || Value < 0.0f || Value > 10000.0f)
				return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDescription,
					"Emissive factor components must be finite values in the range [0, 10000]"));
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
			Document["baseColorTexture"] = Material.BaseColorTexture.IsNil()
				? Json(nullptr) : Json(Material.BaseColorTexture.ToString());
			Document["metallicRoughnessTexture"] = Material.MetallicRoughnessTexture.IsNil()
				? Json(nullptr) : Json(Material.MetallicRoughnessTexture.ToString());
			Document["normalTexture"] = Material.NormalTexture.IsNil()
				? Json(nullptr) : Json(Material.NormalTexture.ToString());
			Document["occlusionTexture"] = Material.OcclusionTexture.IsNil()
				? Json(nullptr) : Json(Material.OcclusionTexture.ToString());
			Document["emissiveTexture"] = Material.EmissiveTexture.IsNil()
				? Json(nullptr) : Json(Material.EmissiveTexture.ToString());
			Document["baseColorFactor"] = {
				Material.BaseColorFactor.r,
				Material.BaseColorFactor.g,
				Material.BaseColorFactor.b,
				Material.BaseColorFactor.a
			};
			Document["metallicFactor"] = Material.MetallicFactor;
			Document["roughnessFactor"] = Material.RoughnessFactor;
			Document["normalScale"] = Material.NormalScale;
			Document["occlusionStrength"] = Material.OcclusionStrength;
			Document["emissiveFactor"] = {
				Material.EmissiveFactor.r,
				Material.EmissiveFactor.g,
				Material.EmissiveFactor.b
			};
			Document["alphaMode"] = Material.AlphaMode == MaterialAlphaMode::Mask ? "MASK" :
				Material.AlphaMode == MaterialAlphaMode::Blend ? "BLEND" : "OPAQUE";
			Document["alphaCutoff"] = Material.AlphaCutoff;
			Document["doubleSided"] = Material.DoubleSided;
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
		if (Data.size() > MaximumMaterialFileSize)
			return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "Material document exceeds the 1 MiB size limit"));
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
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
				return std::unexpected(
					MakeError(MaterialAssetErrorCode::InvalidDocument, "Material document requires an integer format version"));
			const bool SupportedVersion =
				Version->is_number_unsigned()
					? Version->get<uint64_t>() >= 1 && Version->get<uint64_t>() <= static_cast<uint64_t>(MaterialFormatVersion)
					: Version->get<int64_t>() >= 1 && Version->get<int64_t>() <= MaterialFormatVersion;
			if (!SupportedVersion)
				return std::unexpected(MakeError(MaterialAssetErrorCode::UnsupportedVersion, "Material document version is not supported"));
			const int64_t DocumentVersion =
				Version->is_number_unsigned() ? static_cast<int64_t>(Version->get<uint64_t>()) : Version->get<int64_t>();
			if (const auto Unknown = FindUnknownField(Document, DocumentVersion))
				return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
					"Material version " + std::to_string(DocumentVersion) + " contains unsupported field '" + *Unknown + "'"));

			const auto Texture = Document.find("baseColorTexture");
			const auto Factor = Document.find("baseColorFactor");
			if (Factor == Document.end() ||
				!Factor->is_array() || Factor->size() != 4)
			{
				return std::unexpected(MakeError(
					MaterialAssetErrorCode::InvalidDocument,
				"Material requires a four-component base-color factor"));
			}

			MaterialAssetDesc Material;
			const auto ParseTexture = [&](std::string_view Name, AssetID& Destination) -> std::expected<void, MaterialAssetError>
			{
				const auto Value = Document.find(std::string(Name));
				if (Value == Document.end() || Value->is_null())
				{
					Destination = {};
					return {};
				}
				if (!Value->is_string())
					return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
						std::string(Name) + " must be a texture UUID string or null"));
				const auto Identifier = UUID::Parse(Value->get<std::string>());
				if (!Identifier)
					return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, Identifier.error().Message));
				Destination = *Identifier;
				return {};
			};

			if (DocumentVersion < 3)
			{
				if (Texture == Document.end() || !Texture->is_string())
					return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
						"Legacy material documents require a base-color texture UUID"));
				if (auto Parsed = ParseTexture("baseColorTexture", Material.BaseColorTexture); !Parsed)
					return std::unexpected(std::move(Parsed.error()));
				if (Material.BaseColorTexture.IsNil())
					return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
						"Legacy material base-color texture UUID must not be nil"));
			}
			else
			{
				const std::array<std::pair<std::string_view, AssetID*>, 5> TextureFields{{
					{ "baseColorTexture", &Material.BaseColorTexture },
					{ "metallicRoughnessTexture", &Material.MetallicRoughnessTexture },
					{ "normalTexture", &Material.NormalTexture },
					{ "occlusionTexture", &Material.OcclusionTexture },
					{ "emissiveTexture", &Material.EmissiveTexture }
				}};
				for (const auto& [Name, Destination] : TextureFields)
				{
					if (auto Parsed = ParseTexture(Name, *Destination); !Parsed)
						return std::unexpected(std::move(Parsed.error()));
				}
			}
			for (size_t Index = 0; Index < 4; ++Index)
			{
				if (!(*Factor)[Index].is_number())
					return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "Base-color factor must contain numeric values"));
				Material.BaseColorFactor[static_cast<int>(Index)] = (*Factor)[Index].get<float>();
			}
			if (DocumentVersion >= 2)
			{
				const auto Metallic = Document.find("metallicFactor");
				const auto Roughness = Document.find("roughnessFactor");
				if (Metallic == Document.end() || !Metallic->is_number() ||
					Roughness == Document.end() || !Roughness->is_number())
					return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
						"Version 2 material requires numeric metallicFactor and roughnessFactor fields"));
				Material.MetallicFactor = Metallic->get<float>();
				Material.RoughnessFactor = Roughness->get<float>();
			}
			if (DocumentVersion >= 3)
			{
				const auto ReadOptionalFloat = [&](std::string_view Name, float& Destination) -> std::expected<void, MaterialAssetError>
				{
					const auto Value = Document.find(std::string(Name));
					if (Value == Document.end())
						return {};
					if (!Value->is_number())
						return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
							std::string(Name) + " must be numeric"));
					Destination = Value->get<float>();
					return {};
				};
				if (auto Parsed = ReadOptionalFloat("normalScale", Material.NormalScale); !Parsed)
					return std::unexpected(std::move(Parsed.error()));
				if (auto Parsed = ReadOptionalFloat("occlusionStrength", Material.OcclusionStrength); !Parsed)
					return std::unexpected(std::move(Parsed.error()));
				if (const auto Emissive = Document.find("emissiveFactor"); Emissive != Document.end())
				{
					if (!Emissive->is_array() || Emissive->size() != 3)
						return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
							"emissiveFactor must be a three-component array"));
					for (size_t Component = 0; Component < 3; ++Component)
					{
						if (!(*Emissive)[Component].is_number())
							return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
								"emissiveFactor must contain numeric values"));
						Material.EmissiveFactor[static_cast<int>(Component)] = (*Emissive)[Component].get<float>();
					}
				}
			}
			if (DocumentVersion >= 4)
			{
				if (const auto AlphaMode = Document.find("alphaMode"); AlphaMode != Document.end())
				{
					if (!AlphaMode->is_string())
						return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
							"alphaMode must be OPAQUE, MASK, or BLEND"));
					const std::string Value = AlphaMode->get<std::string>();
					if (Value == "OPAQUE")
						Material.AlphaMode = MaterialAlphaMode::Opaque;
					else if (Value == "MASK")
						Material.AlphaMode = MaterialAlphaMode::Mask;
					else if (Value == "BLEND")
						Material.AlphaMode = MaterialAlphaMode::Blend;
					else
						return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument,
							"alphaMode must be OPAQUE, MASK, or BLEND"));
				}
				if (const auto AlphaCutoff = Document.find("alphaCutoff"); AlphaCutoff != Document.end())
				{
					if (!AlphaCutoff->is_number())
						return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "alphaCutoff must be numeric"));
					Material.AlphaCutoff = AlphaCutoff->get<float>();
				}
				if (const auto DoubleSided = Document.find("doubleSided"); DoubleSided != Document.end())
				{
					if (!DoubleSided->is_boolean())
						return std::unexpected(MakeError(MaterialAssetErrorCode::InvalidDocument, "doubleSided must be a boolean"));
					Material.DoubleSided = DoubleSided->get<bool>();
				}
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
			const auto Data = Detail::ReadBoundedInput(Input, MaximumMaterialFileSize);
			if (!Data)
				return std::unexpected(
					MakeError(Data.error().Code == Detail::BoundedInputErrorCode::TooLarge ? MaterialAssetErrorCode::InvalidDocument
																						   : MaterialAssetErrorCode::FileReadFailed,
						Data.error().Code == Detail::BoundedInputErrorCode::TooLarge ? "Material file exceeds the 1 MiB size limit"
																					 : "Could not read the complete material file"));
			return Deserialize(*Data);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				MaterialAssetErrorCode::FileReadFailed,
				std::string("Material file load failed: ") + Exception.what()));
		}
	}
}
