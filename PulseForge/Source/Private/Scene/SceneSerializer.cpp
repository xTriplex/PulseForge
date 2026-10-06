#include "Core/PulseForgePCH.h"
#include "Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr int64_t SceneFormatVersion = 1;
		constexpr std::string_view SceneFormatName = "PulseForgeScene";

		SceneSerializationError MakeError(SceneSerializationErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		bool ReadFiniteFloats(const Json& Value, std::span<float> Output)
		{
			if (!Value.is_array() || Value.size() != Output.size())
				return false;

			for (size_t Index = 0; Index < Output.size(); ++Index)
			{
				if (!Value[Index].is_number())
					return false;
				Output[Index] = Value[Index].get<float>();
				if (!std::isfinite(Output[Index]))
					return false;
			}
			return true;
		}

		SceneSerializationError SceneOperationError(const SceneError& Error)
		{
			if (Error.Code == SceneErrorCode::DuplicateUUID || Error.Code == SceneErrorCode::NilUUID ||
				Error.Code == SceneErrorCode::InvalidTransform || Error.Code == SceneErrorCode::ParentCycle)
			{
				return MakeError(SceneSerializationErrorCode::InvalidEntityData, Error.Message);
			}
			return MakeError(SceneSerializationErrorCode::SceneOperationFailed, Error.Message);
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

	std::expected<std::string, SceneSerializationError> SceneSerializer::Serialize(const Scene& Source)
	{
		try
		{
			Json Document = Json::object();
			Document["format"] = SceneFormatName;
			Document["version"] = SceneFormatVersion;
			Document["entities"] = Json::array();

			for (const Entity& Current : Source.GetEntities())
			{
				const auto Tag = Current.GetTag();
				const auto Transform = Current.GetTransform();
				const auto Parent = Current.GetParent();
				if (!Tag || !Transform || !Parent)
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::SceneOperationFailed,
						"Could not read all required components while serializing an entity"));

				Json Record = Json::object();
				Record["uuid"] = Current.GetUUID().ToString();
				Record["tag"] = Json::object({ { "name", Tag->Name } });
				Record["transform"] = Json::object({
					{ "translation", { Transform->Translation.x, Transform->Translation.y, Transform->Translation.z } },
					{ "rotation", { Transform->Rotation.w, Transform->Rotation.x, Transform->Rotation.y, Transform->Rotation.z } },
					{ "scale", { Transform->Scale.x, Transform->Scale.y, Transform->Scale.z } }
				});
				Record["parent"] = Parent->has_value()
					? Json((**Parent).GetUUID().ToString())
					: Json(nullptr);
				Document["entities"].push_back(std::move(Record));
			}

			return Document.dump(2) + "\n";
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::SceneOperationFailed,
				std::string("Scene serialization failed: ") + Exception.what()));
		}
	}

	std::expected<void, SceneSerializationError> SceneSerializer::Deserialize(std::string_view Data, Scene& Destination)
	{
		try
		{
			const Json Document = Json::parse(Data.begin(), Data.end());
			if (!Document.is_object())
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document root must be an object"));

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string())
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document is missing its format identifier"));
			if (Format->get<std::string>() != SceneFormatName)
				return std::unexpected(MakeError(SceneSerializationErrorCode::UnsupportedFormat, "Document is not a PulseForge scene"));

			const auto Version = Document.find("version");
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document is missing a numeric format version"));
			if (Version->get<int64_t>() != SceneFormatVersion)
				return std::unexpected(MakeError(SceneSerializationErrorCode::UnsupportedVersion, "Scene document version is not supported"));

			const auto SerializedEntities = Document.find("entities");
			if (SerializedEntities == Document.end() || !SerializedEntities->is_array())
				return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidDocument, "Scene document entities must be an array"));

			struct PendingParent
			{
				Entity Child;
				std::optional<UUID> Parent;
			};

			Scene Staging;
			std::vector<PendingParent> PendingParents;
			PendingParents.reserve(SerializedEntities->size());
			std::unordered_set<UUID, UUIDHash> EntityIdentifiers;
			EntityIdentifiers.reserve(SerializedEntities->size());
			for (const Json& SerializedEntity : *SerializedEntities)
			{
				if (!SerializedEntity.is_object())
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Each scene entity must be an object"));

				const auto UUIDValue = SerializedEntity.find("uuid");
				const auto Tag = SerializedEntity.find("tag");
				const auto Transform = SerializedEntity.find("transform");
				const auto Parent = SerializedEntity.find("parent");
				if (UUIDValue == SerializedEntity.end() || !UUIDValue->is_string() ||
					Tag == SerializedEntity.end() || !Tag->is_object() ||
					Transform == SerializedEntity.end() || !Transform->is_object() ||
					Parent == SerializedEntity.end())
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::InvalidEntityData,
						"Entity requires UUID, tag, transform, and parent fields"));
				}

				const auto ParsedUUID = UUID::Parse(UUIDValue->get<std::string>());
				if (!ParsedUUID)
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, ParsedUUID.error().Message));
				if (ParsedUUID->IsNil())
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity UUID must not be nil"));
				if (!EntityIdentifiers.insert(*ParsedUUID).second)
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Scene document contains a duplicate entity UUID"));

				const auto Name = Tag->find("name");
				if (Name == Tag->end() || !Name->is_string())
					return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity tag requires a string name"));

				const auto Translation = Transform->find("translation");
				const auto Rotation = Transform->find("rotation");
				const auto Scale = Transform->find("scale");
				if (Translation == Transform->end() || Rotation == Transform->end() || Scale == Transform->end())
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::InvalidEntityData,
						"Entity transform is missing translation, rotation, or scale"));
				}

				std::array<float, 3> TranslationValues{};
				std::array<float, 4> RotationValues{};
				std::array<float, 3> ScaleValues{};
				if (!ReadFiniteFloats(*Translation, TranslationValues) ||
					!ReadFiniteFloats(*Rotation, RotationValues) ||
					!ReadFiniteFloats(*Scale, ScaleValues))
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::InvalidEntityData,
						"Transform vectors/quaternion must contain the expected number of finite numeric values"));
				}

				std::optional<UUID> ParentIdentifier;
				if (!Parent->is_null())
				{
					if (!Parent->is_string())
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, "Entity parent must be a UUID string or null"));
					const auto ParsedParent = UUID::Parse(Parent->get<std::string>());
					if (!ParsedParent)
						return std::unexpected(MakeError(SceneSerializationErrorCode::InvalidEntityData, ParsedParent.error().Message));
					ParentIdentifier = ParsedParent.value();
				}

				auto Created = Staging.CreateEntityWithUUID(ParsedUUID.value(), Name->get<std::string>());
				if (!Created)
					return std::unexpected(SceneOperationError(Created.error()));

				TransformComponent ComponentTransform;
				ComponentTransform.Translation = { TranslationValues[0], TranslationValues[1], TranslationValues[2] };
				ComponentTransform.Rotation = glm::quat(RotationValues[0], RotationValues[1], RotationValues[2], RotationValues[3]);
				ComponentTransform.Scale = { ScaleValues[0], ScaleValues[1], ScaleValues[2] };
				if (auto TransformResult = Created->SetTransform(ComponentTransform); !TransformResult)
					return std::unexpected(SceneOperationError(TransformResult.error()));

				PendingParents.push_back({ *Created, ParentIdentifier });
			}

			for (const PendingParent& Relationship : PendingParents)
			{
				if (!Relationship.Parent)
					continue;
				const auto Parent = Staging.FindEntity(*Relationship.Parent);
				if (!Parent)
				{
					return std::unexpected(MakeError(
						SceneSerializationErrorCode::MissingParent,
						"Scene entity references a parent UUID that does not exist"));
				}
				if (auto ParentResult = Relationship.Child.SetParent(*Parent); !ParentResult)
					return std::unexpected(SceneOperationError(ParentResult.error()));
			}

			Destination.m_Storage.swap(Staging.m_Storage);
			return {};
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::InvalidDocument,
				std::string("Scene JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::InvalidDocument,
				std::string("Scene deserialization failed: ") + Exception.what()));
		}
	}

	std::expected<void, SceneSerializationError> SceneSerializer::SaveToFile(
		const Scene& Source,
		const std::filesystem::path& Path)
	{
		if (Path.empty())
			return std::unexpected(MakeError(SceneSerializationErrorCode::FileOpenFailed, "Scene file path must not be empty"));

		const auto Serialized = Serialize(Source);
		if (!Serialized)
			return std::unexpected(Serialized.error());

		try
		{
			const auto TemporaryIdentifier = UUID::Generate();
			if (!TemporaryIdentifier)
				return std::unexpected(MakeError(SceneSerializationErrorCode::SceneOperationFailed, TemporaryIdentifier.error().Message));

			std::filesystem::path TemporaryPath = Path;
			TemporaryPath += "." + TemporaryIdentifier->ToString() + ".tmp";
			TemporaryFileCleanup Cleanup{ TemporaryPath };

			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output.is_open())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileOpenFailed,
					"Could not open scene file for writing: " + PathForMessage(Path)));
			}
			if (Serialized->size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileWriteFailed,
					"Serialized scene is too large to write: " + PathForMessage(Path)));
			}

			Output.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
			Output.flush();
			if (!Output)
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileWriteFailed,
					"Could not write the complete scene document: " + PathForMessage(Path)));
			}
			Output.close();
			if (Output.fail())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileWriteFailed,
					"Could not finish writing the scene document: " + PathForMessage(Path)));
			}

			std::error_code ReplaceError;
			std::filesystem::rename(TemporaryPath, Path, ReplaceError);
			if (ReplaceError)
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileReplaceFailed,
					"Could not replace scene file '" + PathForMessage(Path) + "': " + ReplaceError.message()));
			}
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::FileWriteFailed,
				std::string("Scene file save failed: ") + Exception.what()));
		}
	}

	std::expected<void, SceneSerializationError> SceneSerializer::LoadFromFile(
		const std::filesystem::path& Path,
		Scene& Destination)
	{
		if (Path.empty())
			return std::unexpected(MakeError(SceneSerializationErrorCode::FileOpenFailed, "Scene file path must not be empty"));

		try
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileOpenFailed,
					"Could not open scene file for reading: " + PathForMessage(Path)));
			}

			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
			{
				return std::unexpected(MakeError(
					SceneSerializationErrorCode::FileReadFailed,
					"Could not read the complete scene file: " + PathForMessage(Path)));
			}
			return Deserialize(Data, Destination);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneSerializationErrorCode::FileReadFailed,
				std::string("Scene file load failed: ") + Exception.what()));
		}
	}
}
