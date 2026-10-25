#include "Assets/AssetImportProvenance.h"

#include "Core/PlatformServices.h"
#include "Scene/UUID.h"

#include <nlohmann/json.hpp>

#define XXH_INLINE_ALL
#include <tracy_xxhash.h>

static_assert(XXH_VERSION_NUMBER == 802, "Asset import fingerprints require the pinned xxHash 0.8.2 implementation");

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr size_t MaxReceiptBytes = 1024 * 1024;
		constexpr std::string_view FingerprintAlgorithm = "xxh3-128-v0.8.2";
		constexpr size_t ContentFingerprintChunkBytes = 64 * 1024;

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
				throw std::runtime_error("JSON object contains a duplicate key");
			return Document;
		}

		AssetImportProvenanceError MakeError(
			AssetImportProvenanceErrorCode Code,
			const std::filesystem::path& Path,
			std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		bool IsCanonicalProjectKey(std::string_view Key)
		{
			if (Key.empty() || Key.front() == '/' || Key.find('\\') != std::string_view::npos || Key.find(':') != std::string_view::npos)
				return false;
			std::filesystem::path Path = std::filesystem::u8path(Key);
			if (Path.is_absolute() || Path.has_root_name() || Path.has_root_directory())
				return false;
			for (const auto& Part : Path)
				if (Part == ".." || Part == ".") return false;
			const std::u8string Normalized = Path.generic_u8string();
			return std::string_view(reinterpret_cast<const char*>(Normalized.data()), Normalized.size()) == Key;
		}

		Json FingerprintToJson(const AssetContentFingerprint& Fingerprint)
		{
			return Json::object({ { "algorithm", Fingerprint.Algorithm }, { "digest", Fingerprint.DigestHex } });
		}

		std::expected<AssetContentFingerprint, std::string> ParseFingerprint(const Json& Value)
		{
			if (!Value.is_object()) return std::unexpected("Fingerprint must be an object");
			const auto Algorithm = Value.find("algorithm");
			const auto Digest = Value.find("digest");
			if (Algorithm == Value.end() || !Algorithm->is_string() || Digest == Value.end() || !Digest->is_string())
				return std::unexpected("Fingerprint requires string algorithm and digest fields");
			AssetContentFingerprint Result{ Algorithm->get<std::string>(), Digest->get<std::string>() };
			if (Result.Algorithm != FingerprintAlgorithm || Result.DigestHex.size() != 32 ||
				!std::all_of(Result.DigestHex.begin(), Result.DigestHex.end(), [](char Character)
				{
					return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f');
				}))
				return std::unexpected("Fingerprint algorithm or digest encoding is unsupported");
			return Result;
		}

		bool ReadUInt32(const Json& Value, uint32_t& Result)
		{
			if (Value.is_number_unsigned())
			{
				const uint64_t Number = Value.get<uint64_t>();
				if (Number > std::numeric_limits<uint32_t>::max()) return false;
				Result = static_cast<uint32_t>(Number);
				return true;
			}
			if (Value.is_number_integer())
			{
				const int64_t Number = Value.get<int64_t>();
				if (Number < 0 || static_cast<uint64_t>(Number) > std::numeric_limits<uint32_t>::max()) return false;
				Result = static_cast<uint32_t>(Number);
				return true;
			}
			return false;
		}

		Json RecipeToJson(const AssetImportRecipe& Recipe)
		{
			Json Inputs = Json::array();
			for (const AssetImportInputFingerprint& Input : Recipe.Inputs)
				Inputs.push_back(Json::object({ { "path", Input.ProjectRelativePath }, { "content", FingerprintToJson(Input.Content) } }));
			return Json::object({
				{ "sourceAsset", Recipe.SourceAsset.ToString() },
				{ "subresource", Recipe.SubresourceKey },
				{ "importer", Recipe.ImporterID },
				{ "importerVersion", Recipe.ImporterVersion },
				{ "outputSchemaVersion", Recipe.OutputSchemaVersion },
				{ "effectiveSettings", Recipe.EffectiveSettings },
				{ "target", Recipe.TargetID },
				{ "inputs", std::move(Inputs) },
				{ "inputFingerprint", FingerprintToJson(Recipe.InputFingerprint) }
			});
		}

		std::expected<AssetImportRecipe, std::string> RecipeFromJson(const Json& Value)
		{
			if (!Value.is_object()) return std::unexpected("Import recipe must be an object");
			const auto ReadString = [&Value](std::string_view Name) -> std::expected<std::string, std::string>
			{
				const auto Field = Value.find(Name);
				if (Field == Value.end() || !Field->is_string()) return std::unexpected("Recipe field '" + std::string(Name) + "' must be a string");
				return Field->get<std::string>();
			};
			auto Source = ReadString("sourceAsset");
			auto Subresource = ReadString("subresource");
			auto Importer = ReadString("importer");
			auto ImporterVersion = ReadString("importerVersion");
			auto Settings = ReadString("effectiveSettings");
			auto Target = ReadString("target");
			if (!Source || !Subresource || !Importer || !ImporterVersion || !Settings || !Target)
				return std::unexpected("Import recipe is missing a required string field");
			const auto ParsedAsset = UUID::Parse(*Source);
			if (!ParsedAsset || ParsedAsset->IsNil()) return std::unexpected("Import recipe source UUID is invalid");
			const auto Schema = Value.find("outputSchemaVersion");
			uint32_t OutputSchemaVersion = 0;
			if (Schema == Value.end() || !ReadUInt32(*Schema, OutputSchemaVersion) || OutputSchemaVersion == 0)
				return std::unexpected("Import recipe output schema version is invalid");
			AssetImportRecipe Recipe;
			Recipe.SourceAsset = *ParsedAsset;
			Recipe.SubresourceKey = std::move(*Subresource);
			Recipe.ImporterID = std::move(*Importer);
			Recipe.ImporterVersion = std::move(*ImporterVersion);
			Recipe.OutputSchemaVersion = OutputSchemaVersion;
			Recipe.EffectiveSettings = std::move(*Settings);
			Recipe.TargetID = std::move(*Target);
			const auto Inputs = Value.find("inputs");
			if (Inputs == Value.end() || !Inputs->is_array() || Inputs->size() > 4096)
				return std::unexpected("Import recipe input list is invalid or exceeds its limit");
			for (const Json& InputValue : *Inputs)
			{
				if (!InputValue.is_object()) return std::unexpected("Import recipe input must be an object");
				const auto Path = InputValue.find("path");
				const auto Content = InputValue.find("content");
				if (Path == InputValue.end() || !Path->is_string() || Content == InputValue.end())
					return std::unexpected("Import recipe input requires path and content fields");
				std::string Key = Path->get<std::string>();
				if (!IsCanonicalProjectKey(Key)) return std::unexpected("Import recipe input path is not a canonical project-relative key");
				auto Fingerprint = ParseFingerprint(*Content);
				if (!Fingerprint) return std::unexpected(Fingerprint.error());
				Recipe.Inputs.push_back({ std::move(Key), std::move(*Fingerprint) });
			}
			if (!std::is_sorted(Recipe.Inputs.begin(), Recipe.Inputs.end(), [](const auto& A, const auto& B)
				{ return A.ProjectRelativePath < B.ProjectRelativePath; }))
				return std::unexpected("Import recipe inputs are not sorted");
			for (size_t Index = 1; Index < Recipe.Inputs.size(); ++Index)
				if (Recipe.Inputs[Index - 1].ProjectRelativePath == Recipe.Inputs[Index].ProjectRelativePath)
					return std::unexpected("Import recipe contains duplicate input paths");
			const auto InputFingerprint = Value.find("inputFingerprint");
			if (InputFingerprint == Value.end()) return std::unexpected("Import recipe has no input fingerprint");
			auto ParsedInputFingerprint = ParseFingerprint(*InputFingerprint);
			if (!ParsedInputFingerprint) return std::unexpected(ParsedInputFingerprint.error());
			Recipe.InputFingerprint = std::move(*ParsedInputFingerprint);
			auto Calculated = ComputeAssetImportInputFingerprint(Recipe);
			if (!Calculated || *Calculated != Recipe.InputFingerprint)
				return std::unexpected("Import recipe input fingerprint does not match its contents");
			return Recipe;
		}

		std::string HexDigest(XXH128_hash_t Digest)
		{
			constexpr char Hex[] = "0123456789abcdef";
			std::string Result(32, '0');
			for (size_t Index = 0; Index < 16; ++Index)
			{
				const uint64_t Value = Index < 8 ? Digest.high64 : Digest.low64;
				const size_t Byte = Index < 8 ? Index : Index - 8;
				const uint8_t NibblePair = static_cast<uint8_t>(Value >> ((7 - Byte) * 8));
				Result[Index * 2] = Hex[NibblePair >> 4];
				Result[Index * 2 + 1] = Hex[NibblePair & 0x0f];
			}
			return Result;
		}
	}

	struct AssetFingerprintBuilder::State
	{
		XXH3_state_t* Value = XXH3_createState();
		bool Finished = false;
		~State() { if (Value) (void)XXH3_freeState(Value); }
	};

	AssetFingerprintBuilder::AssetFingerprintBuilder(std::string_view Domain)
		: m_State(std::make_unique<State>())
	{
		if (!m_State->Value || XXH3_128bits_reset(m_State->Value) != XXH_OK)
			throw std::runtime_error("Could not initialize asset fingerprint state");
		UpdateText("domain", Domain);
	}

	AssetFingerprintBuilder::~AssetFingerprintBuilder() = default;
	AssetFingerprintBuilder::AssetFingerprintBuilder(AssetFingerprintBuilder&&) noexcept = default;
	AssetFingerprintBuilder& AssetFingerprintBuilder::operator=(AssetFingerprintBuilder&&) noexcept = default;

	void AssetFingerprintBuilder::UpdateBytes(std::string_view Label, std::span<const std::byte> Bytes)
	{
		if (!m_State || !m_State->Value || m_State->Finished) throw std::logic_error("Fingerprint builder is not writable");
		const uint64_t LabelLength = Label.size();
		const uint64_t DataLength = Bytes.size();
		std::array<std::byte, sizeof(uint64_t)> EncodedLength{};
		for (size_t Index = 0; Index < EncodedLength.size(); ++Index)
			EncodedLength[Index] = static_cast<std::byte>((LabelLength >> (Index * 8)) & 0xff);
		(void)XXH3_128bits_update(m_State->Value, EncodedLength.data(), EncodedLength.size());
		if (!Label.empty()) (void)XXH3_128bits_update(m_State->Value, Label.data(), Label.size());
		for (size_t Index = 0; Index < EncodedLength.size(); ++Index)
			EncodedLength[Index] = static_cast<std::byte>((DataLength >> (Index * 8)) & 0xff);
		(void)XXH3_128bits_update(m_State->Value, EncodedLength.data(), EncodedLength.size());
		if (!Bytes.empty()) (void)XXH3_128bits_update(m_State->Value, Bytes.data(), Bytes.size());
	}

	void AssetFingerprintBuilder::UpdateText(std::string_view Label, std::string_view Text)
	{
		UpdateBytes(Label, std::as_bytes(std::span(Text.data(), Text.size())));
	}

	AssetContentFingerprint AssetFingerprintBuilder::Finish()
	{
		if (!m_State || !m_State->Value || m_State->Finished) throw std::logic_error("Fingerprint builder was already finished");
		m_State->Finished = true;
		return { std::string(FingerprintAlgorithm), HexDigest(XXH3_128bits_digest(m_State->Value)) };
	}

	AssetContentFingerprint ComputeAssetContentFingerprint(std::span<const std::byte> Bytes)
	{
		AssetFingerprintBuilder Builder("PulseForge.AssetInputBytes.v1");
		std::array<char, 32> LengthBuffer{};
		const auto [LengthEnd, LengthError] = std::to_chars(
			LengthBuffer.data(), LengthBuffer.data() + LengthBuffer.size(), Bytes.size());
		if (LengthError != std::errc{})
			throw std::runtime_error("Could not encode asset content length");
		Builder.UpdateText("byteLength", std::string_view(LengthBuffer.data(), static_cast<size_t>(LengthEnd - LengthBuffer.data())));
		for (size_t Offset = 0; Offset < Bytes.size(); Offset += ContentFingerprintChunkBytes)
		{
			const size_t ChunkSize = std::min(ContentFingerprintChunkBytes, Bytes.size() - Offset);
			Builder.UpdateBytes("chunk", Bytes.subspan(Offset, ChunkSize));
		}
		return Builder.Finish();
	}

	std::expected<AssetContentFingerprint, std::string> ComputeAssetImportInputFingerprint(const AssetImportRecipe& Recipe)
	{
		if (Recipe.SourceAsset.IsNil() || Recipe.SubresourceKey.empty() || Recipe.ImporterID.empty() || Recipe.ImporterVersion.empty() ||
			Recipe.OutputSchemaVersion == 0 || Recipe.TargetID.empty())
			return std::unexpected("Import recipe identity is incomplete");
		if (Recipe.Inputs.size() > 4096) return std::unexpected("Import recipe has too many inputs");
		for (size_t Index = 0; Index < Recipe.Inputs.size(); ++Index)
		{
			const auto& Input = Recipe.Inputs[Index];
			if (!IsCanonicalProjectKey(Input.ProjectRelativePath) || Input.Content.Algorithm != FingerprintAlgorithm ||
				Input.Content.DigestHex.size() != 32 ||
				!std::all_of(Input.Content.DigestHex.begin(), Input.Content.DigestHex.end(), [](char Character)
					{ return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f'); }))
				return std::unexpected("Import recipe contains an invalid input key or fingerprint");
			if (Index > 0 && Recipe.Inputs[Index - 1].ProjectRelativePath >= Input.ProjectRelativePath)
				return std::unexpected("Import recipe inputs must be uniquely sorted by project-relative path");
		}
		AssetFingerprintBuilder Builder("PulseForge.AssetImportRecipe.v1");
		Builder.UpdateText("sourceAsset", Recipe.SourceAsset.ToString());
		Builder.UpdateText("subresource", Recipe.SubresourceKey);
		Builder.UpdateText("importer", Recipe.ImporterID);
		Builder.UpdateText("importerVersion", Recipe.ImporterVersion);
		std::array<char, 16> VersionText{};
		const auto [VersionEnd, VersionError] = std::to_chars(VersionText.data(), VersionText.data() + VersionText.size(),
			Recipe.OutputSchemaVersion);
		if (VersionError != std::errc{}) return std::unexpected("Could not encode output schema version");
		Builder.UpdateText("outputSchemaVersion", std::string_view(VersionText.data(), static_cast<size_t>(VersionEnd - VersionText.data())));
		Builder.UpdateText("effectiveSettings", Recipe.EffectiveSettings);
		Builder.UpdateText("target", Recipe.TargetID);
		for (const auto& Input : Recipe.Inputs)
		{
			Builder.UpdateText("inputPath", Input.ProjectRelativePath);
			Builder.UpdateText("inputAlgorithm", Input.Content.Algorithm);
			Builder.UpdateText("inputDigest", Input.Content.DigestHex);
		}
		return Builder.Finish();
	}

	std::filesystem::path AssetImportProvenanceSerializer::GetSidecarPath(const std::filesystem::path& SourceAssetPath)
	{
		std::filesystem::path Path = SourceAssetPath;
		Path += ".importresult";
		return Path;
	}

	std::expected<AssetImportSuccessReceipt, AssetImportProvenanceError> AssetImportProvenanceSerializer::Deserialize(
		std::string_view Data,
		const std::filesystem::path& Path)
	{
		if (Data.size() > MaxReceiptBytes)
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path, "Import receipt exceeds its size limit"));
		try
		{
			const Json Document = ParseUniqueJson(Data);
			if (!Document.is_object()) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
				"Import receipt root must be an object"));
			const auto Format = Document.find("format");
			const auto Version = Document.find("version");
			uint32_t ReceiptVersion = 0;
			if (Format == Document.end() || !Format->is_string() || Format->get<std::string>() != "PulseForgeImportResult" ||
				Version == Document.end() || !ReadUInt32(*Version, ReceiptVersion))
				return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
					"Import receipt requires a format identifier and numeric version"));
			if (ReceiptVersion != CurrentVersion)
				return std::unexpected(MakeError(AssetImportProvenanceErrorCode::UnsupportedVersion, Path,
					"Import receipt version is not supported"));
			const auto RecipeValue = Document.find("recipe");
			const auto ResultValue = Document.find("resultFingerprint");
			const auto IdentityValue = Document.find("resultIdentity");
			const auto ArtifactValue = Document.find("artifactAvailability");
			const auto ResidencyValue = Document.find("gpuResidency");
			if (RecipeValue == Document.end() || ResultValue == Document.end() || IdentityValue == Document.end() ||
				!IdentityValue->is_string() || IdentityValue->get<std::string>().empty() ||
				ArtifactValue == Document.end() || !ArtifactValue->is_string() || ArtifactValue->get<std::string>() != "notPersisted" ||
				ResidencyValue == Document.end() || !ResidencyValue->is_string() || ResidencyValue->get<std::string>() != "unknown")
				return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
					"Import receipt has invalid result, artifact, or residency fields"));
			auto Recipe = RecipeFromJson(*RecipeValue);
			auto ResultFingerprint = ParseFingerprint(*ResultValue);
			if (!Recipe || !ResultFingerprint)
				return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
					Recipe ? ResultFingerprint.error() : Recipe.error()));
			const std::string ResultIdentity = IdentityValue->get<std::string>();
			if (ResultIdentity != Recipe->SubresourceKey)
				return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
					"Import result identity does not match its recipe subresource"));
			return AssetImportSuccessReceipt{ std::move(*Recipe), std::move(*ResultFingerprint), ResultIdentity };
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
				std::string("Import receipt JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
				std::string("Could not decode import receipt: ") + Exception.what()));
		}
	}

	std::expected<AssetImportSuccessReceipt, AssetImportProvenanceError> AssetImportProvenanceSerializer::Load(
		const std::filesystem::path& Path)
	{
		if (Path.empty()) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidPath, Path,
			"Import receipt path must not be empty"));
		std::ifstream Input(Path, std::ios::binary);
		if (!Input.is_open()) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::FileOpenFailed, Path,
			"Could not open import receipt"));
		std::string Data;
		Data.resize(MaxReceiptBytes + 1);
		Input.read(Data.data(), static_cast<std::streamsize>(Data.size()));
		const std::streamsize Count = Input.gcount();
		if (Input.bad()) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::FileReadFailed, Path,
			"Could not read import receipt"));
		if (Count > static_cast<std::streamsize>(MaxReceiptBytes))
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path, "Import receipt exceeds its size limit"));
		Data.resize(static_cast<size_t>(Count));
		return Deserialize(Data, Path);
	}

	std::expected<void, AssetImportProvenanceError> AssetImportProvenanceSerializer::Save(
		const std::filesystem::path& Path,
		const AssetImportSuccessReceipt& Receipt)
	{
		if (Path.empty()) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidPath, Path,
			"Import receipt path must not be empty"));
		const auto CalculatedInput = ComputeAssetImportInputFingerprint(Receipt.Recipe);
		if (!CalculatedInput || *CalculatedInput != Receipt.Recipe.InputFingerprint ||
			Receipt.ResultIdentity != Receipt.Recipe.SubresourceKey ||
			Receipt.ResultFingerprint.Algorithm != FingerprintAlgorithm || Receipt.ResultFingerprint.DigestHex.size() != 32 ||
			!std::all_of(Receipt.ResultFingerprint.DigestHex.begin(), Receipt.ResultFingerprint.DigestHex.end(), [](char Character)
				{ return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f'); }))
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
				CalculatedInput ? "Import receipt fields are invalid" : CalculatedInput.error()));
		const Json Document = Json::object({
			{ "format", "PulseForgeImportResult" },
			{ "version", CurrentVersion },
			{ "recipe", RecipeToJson(Receipt.Recipe) },
			{ "resultFingerprint", FingerprintToJson(Receipt.ResultFingerprint) },
			{ "resultIdentity", Receipt.ResultIdentity },
			{ "artifactAvailability", "notPersisted" },
			{ "gpuResidency", "unknown" }
		});
		const std::string Serialized = Document.dump(2) + "\n";
		if (Serialized.size() > MaxReceiptBytes)
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::InvalidDocument, Path,
				"Serialized import receipt exceeds its size limit"));
		std::error_code DestinationError;
		const std::filesystem::file_status DestinationStatus = std::filesystem::symlink_status(Path, DestinationError);
		if (DestinationError != std::errc::no_such_file_or_directory && DestinationError)
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::WriteFailed, Path,
				"Could not inspect existing import receipt: " + DestinationError.message()));
		if (!DestinationError && DestinationStatus.type() != std::filesystem::file_type::not_found &&
			(std::filesystem::is_symlink(DestinationStatus) || !std::filesystem::is_regular_file(DestinationStatus)))
			return std::unexpected(MakeError(AssetImportProvenanceErrorCode::WriteFailed, Path,
				"Import receipt destination must be a regular non-symlink file"));
		auto Identifier = UUID::Generate();
		if (!Identifier) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::WriteFailed, Path, Identifier.error().Message));
		std::filesystem::path Temporary = Path;
		Temporary += "." + Identifier->ToString() + ".tmp";
		struct Cleanup
		{
			std::filesystem::path Path;
			~Cleanup() { std::error_code Error; std::filesystem::remove(Path, Error); }
		} CleanupTemporary{ Temporary };
		std::ofstream Output(Temporary, std::ios::binary | std::ios::trunc);
		if (!Output.is_open()) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::WriteFailed, Path,
			"Could not open temporary import receipt"));
		Output.write(Serialized.data(), static_cast<std::streamsize>(Serialized.size()));
		Output.flush();
		Output.close();
		if (!Output) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::WriteFailed, Path,
			"Could not write complete import receipt"));
		auto Replaced = Platform::ReplaceFileAtomically(Temporary, Path);
		if (!Replaced) return std::unexpected(MakeError(AssetImportProvenanceErrorCode::WriteFailed, Path, Replaced.error().Message));
		return {};
	}
}
