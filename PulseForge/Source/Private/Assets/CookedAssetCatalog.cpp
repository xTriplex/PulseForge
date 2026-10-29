#include "Assets/CookedAssetCatalog.h"

#include "Scene/UUID.h"

#include <nlohmann/json.hpp>

#define XXH_INLINE_ALL
#include <tracy_xxhash.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#include <tuple>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

static_assert(XXH_VERSION_NUMBER == 802, "Cooked payload digests use the pinned xxHash 0.8.2 implementation");

namespace PulseForge
{
	std::string_view GetCookedBuildConfigurationName(CookedBuildConfiguration Configuration) noexcept
	{
		switch (Configuration)
		{
		case CookedBuildConfiguration::Debug: return "Debug";
		case CookedBuildConfiguration::Release: return "Release";
		case CookedBuildConfiguration::Dist: return "Dist";
		}
		return {};
	}

	std::expected<CookedBuildConfiguration, std::string> ParseCookedBuildConfiguration(std::string_view Configuration)
	{
		if (Configuration == "Debug") return CookedBuildConfiguration::Debug;
		if (Configuration == "Release") return CookedBuildConfiguration::Release;
		if (Configuration == "Dist") return CookedBuildConfiguration::Dist;
		return std::unexpected("Cook configuration must be Debug, Release, or Dist");
	}

	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr std::string_view Format = "PulseForgeCookManifest";
		constexpr std::string_view DigestID = "xxh3-128-v0.8.2/pf-cooked-payload-v1";
		constexpr uint64_t MaximumCatalogPayloadBytes = 8ull * 1024ull * 1024ull * 1024ull;

		CookedAssetCatalogError MakeError(
			CookedAssetCatalogErrorCode Code, const std::filesystem::path& Path, std::string Message,
			std::optional<AssetID> Asset = std::nullopt)
		{
			return { Code, Path, Asset, std::move(Message) };
		}

		std::string Hex(XXH128_hash_t Hash)
		{
			std::array<char, 33> Buffer{};
			const auto [HighEnd, HighError] = std::to_chars(Buffer.data(), Buffer.data() + 16, Hash.high64, 16);
			const auto [LowEnd, LowError] = std::to_chars(Buffer.data() + 16, Buffer.data() + 32, Hash.low64, 16);
			if (HighError != std::errc{} || LowError != std::errc{}) return {};
			auto Pad = [](char* Begin, char* End)
			{
				const size_t Length = static_cast<size_t>(End - Begin);
				if (Length < 16)
				{
					std::move_backward(Begin, End, Begin + 16);
					std::fill(Begin, Begin + (16 - Length), '0');
				}
			};
			Pad(Buffer.data(), const_cast<char*>(HighEnd));
			Pad(Buffer.data() + 16, const_cast<char*>(LowEnd));
			return { Buffer.data(), 32 };
		}

		std::string_view ProductName(CookedAssetProductKind Product) noexcept
		{
			switch (Product)
			{
			case CookedAssetProductKind::Scene: return "scene";
			case CookedAssetProductKind::Prefab: return "prefab";
			case CookedAssetProductKind::Material: return "material";
			case CookedAssetProductKind::Script: return "script";
			case CookedAssetProductKind::Audio: return "audio";
			case CookedAssetProductKind::MeshPrimitive: return "mesh-primitive";
			case CookedAssetProductKind::Texture: return "texture";
			case CookedAssetProductKind::Environment: return "environment";
			case CookedAssetProductKind::GameplayInput: return "gameplay-input";
			}
			return {};
		}

		std::optional<CookedAssetProductKind> ParseProduct(std::string_view Name) noexcept
		{
			for (const CookedAssetProductKind Kind : { CookedAssetProductKind::Scene, CookedAssetProductKind::Prefab,
				 CookedAssetProductKind::Material, CookedAssetProductKind::Script, CookedAssetProductKind::Audio,
				 CookedAssetProductKind::MeshPrimitive, CookedAssetProductKind::Texture,
				 CookedAssetProductKind::Environment, CookedAssetProductKind::GameplayInput })
				if (ProductName(Kind) == Name) return Kind;
			return std::nullopt;
		}

		bool IsSafeRelativePayloadPath(std::string_view Name)
		{
			if (Name.empty() || Name.size() > 1024 || Name.front() == '/' || Name.front() == '\\' || Name.find(':') != std::string_view::npos)
				return false;
			if (Name.find('\\') != std::string_view::npos) return false;
			std::filesystem::path Path = std::filesystem::u8path(Name.begin(), Name.end());
			if (Path.is_absolute() || Path.has_root_name() || Path.has_root_directory()) return false;
			for (const auto& Component : Path)
				if (Component.empty() || Component == "." || Component == "..") return false;
			const std::u8string Canonical = Path.generic_u8string();
			return Canonical.size() == Name.size() &&
				std::equal(Canonical.begin(), Canonical.end(), Name.begin(),
					[](char8_t A, char B) { return static_cast<unsigned char>(A) == static_cast<unsigned char>(B); });
		}

		bool IsLinkedStatus(const std::filesystem::path& Path, const std::filesystem::file_status& Status)
		{
			if (std::filesystem::is_symlink(Status)) return true;
#if defined(_WIN32)
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			(void)Path;
			return false;
#endif
		}

		bool IsLinked(const std::filesystem::path& Path, std::error_code& Error)
		{
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error) return false;
			return IsLinkedStatus(Path, Status);
		}

		bool HasLinkedComponent(const std::filesystem::path& Path, std::error_code& Error)
		{
			std::filesystem::path Current;
			for (const auto& Component : Path)
			{
				Current /= Component;
				const auto Status = std::filesystem::symlink_status(Current, Error);
				if (Error == std::errc::no_such_file_or_directory)
				{
					Error.clear();
					continue;
				}
				if (Error) return false;
				if (IsLinkedStatus(Current, Status)) return true;
			}
			return false;
		}

		std::expected<uint64_t, std::string> ReadUInt64(const Json& Value)
		{
			if (Value.is_number_unsigned()) return Value.get<uint64_t>();
			if (Value.is_number_integer())
			{
				const int64_t Signed = Value.get<int64_t>();
				if (Signed >= 0) return static_cast<uint64_t>(Signed);
			}
			return std::unexpected("Expected a non-negative integer");
		}

		bool HasExactFields(const Json& Value, std::initializer_list<std::string_view> Fields)
		{
			if (!Value.is_object() || Value.size() != Fields.size()) return false;
			for (const auto Field : Fields)
				if (!Value.contains(Field)) return false;
			return true;
		}

		auto EntryKey(const CookedAssetEntry& Entry)
		{
			return std::tuple{ Entry.Asset, static_cast<uint8_t>(Entry.Product), Entry.Subresource };
		}
	}

	CookedAssetCatalog::CookedAssetCatalog(
		std::filesystem::path ContentRoot, UUID ProjectID, AssetID StartupScene, CookedAssetCompatibility Compatibility,
		std::vector<AssetID> DynamicRoots, std::vector<CookedAssetEntry> Entries)
		: m_ContentRoot(std::move(ContentRoot)), m_ProjectID(ProjectID), m_StartupScene(StartupScene),
		  m_Compatibility(std::move(Compatibility)), m_DynamicRoots(std::move(DynamicRoots)), m_Entries(std::move(Entries))
	{
	}

	CookedAssetDigest CookedAssetCatalog::ComputePayloadDigest(std::span<const std::byte> Bytes)
	{
		auto Digest = ComputePayloadDigestCancellable(Bytes, {});
		return Digest ? std::move(*Digest) : CookedAssetDigest{};
	}

	std::expected<CookedAssetDigest, std::string> CookedAssetCatalog::ComputePayloadDigestCancellable(
		std::span<const std::byte> Bytes, std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested()) return std::unexpected("Payload digesting was cancelled");
		XXH3_state_t* State = XXH3_createState();
		if (!State || XXH3_128bits_reset(State) != XXH_OK)
		{
			if (State) (void)XXH3_freeState(State);
			return std::unexpected("Could not initialize the cooked payload digest state");
		}
		struct StateGuard final
		{
			XXH3_state_t* State;
			~StateGuard() { if (State) (void)XXH3_freeState(State); }
		} Guard{ State };
		constexpr std::string_view Domain = "PulseForge.CookedPayload.v1";
		std::array<std::byte, sizeof(uint64_t)> EncodedLength{};
		const uint64_t Length = Bytes.size();
		for (size_t I = 0; I < EncodedLength.size(); ++I)
			EncodedLength[I] = static_cast<std::byte>((Length >> (I * 8)) & 0xffu);
		(void)XXH3_128bits_update(State, Domain.data(), Domain.size());
		(void)XXH3_128bits_update(State, EncodedLength.data(), EncodedLength.size());
		constexpr size_t ChunkBytes = 1024 * 1024;
		for (size_t Offset = 0; Offset < Bytes.size();)
		{
			if (CancellationToken.stop_requested()) return std::unexpected("Payload digesting was cancelled");
			const size_t Count = std::min(ChunkBytes, Bytes.size() - Offset);
			if (XXH3_128bits_update(State, Bytes.data() + Offset, Count) != XXH_OK)
				return std::unexpected("Cooked payload digest update failed");
			Offset += Count;
		}
		if (CancellationToken.stop_requested()) return std::unexpected("Payload digesting was cancelled");
		const XXH128_hash_t Hash = XXH3_128bits_digest(State);
		return CookedAssetDigest{ std::string(DigestID), Hex(Hash) };
	}

	std::expected<std::string, CookedAssetCatalogError> CookedAssetCatalog::SerializeManifest(
		const UUID& ProjectID, const AssetID& StartupScene, const CookedAssetCompatibility& Compatibility,
		std::vector<AssetID> DynamicRoots, std::vector<CookedAssetEntry> Entries, std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled"));
		if (ProjectID.IsNil() || StartupScene.IsNil() || Compatibility.TargetPlatform.empty() ||
			Compatibility.RendererBackend.empty() || Compatibility.RuntimeABI.empty() ||
			GetCookedBuildConfigurationName(Compatibility.Configuration).empty())
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Manifest identity or compatibility is incomplete"));
		if (Entries.empty() || Entries.size() > MaximumEntries || DynamicRoots.size() > MaximumEntries)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Manifest entry or root count is invalid"));

		std::sort(DynamicRoots.begin(), DynamicRoots.end());
		for (size_t Index = 0; Index < DynamicRoots.size(); ++Index)
		{
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled"));
			if (DynamicRoots[Index].IsNil() || (Index > 0 && DynamicRoots[Index - 1] == DynamicRoots[Index]))
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Dynamic root IDs must be unique and non-nil"));
		}

		std::sort(Entries.begin(), Entries.end(), [](const auto& A, const auto& B) { return EntryKey(A) < EntryKey(B); });
		Json JsonEntries = Json::array();
		std::optional<decltype(EntryKey(Entries.front()))> Previous;
		bool HasStartupScene = false;
		uint64_t AggregatePayloadBytes = 0;
		std::set<AssetID> Assets;
		std::set<std::tuple<AssetID, uint8_t, std::string>> Identities;
		for (CookedAssetEntry& Entry : Entries)
		{
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled", Entry.Asset));
			if (Entry.Asset.IsNil() || ProductName(Entry.Product).empty() || Entry.PayloadVersion == 0 ||
				!IsSafeRelativePayloadPath(Entry.RelativePayloadPath) || Entry.PayloadBytes == 0 ||
				Entry.PayloadBytes > MaximumPayloadBytes || Entry.DigestAlgorithm != DigestID || Entry.Digest.size() != 32)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Manifest entry has an invalid identity or payload descriptor", Entry.Asset));
			const auto Key = EntryKey(Entry);
			if (Previous && *Previous == Key)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::DuplicateIdentity, {}, "Manifest repeats an asset product identity", Entry.Asset));
			Previous = Key;
			Identities.insert(Key);
			if (!std::all_of(Entry.Digest.begin(), Entry.Digest.end(), [](unsigned char C)
				{ return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); }))
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Payload digest must be lowercase hexadecimal", Entry.Asset));
			if (Entry.Asset == StartupScene && Entry.Product == CookedAssetProductKind::Scene) HasStartupScene = true;
			if (AggregatePayloadBytes > MaximumCatalogPayloadBytes - Entry.PayloadBytes)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Manifest aggregate payload bytes exceed the catalog limit"));
			AggregatePayloadBytes += Entry.PayloadBytes;
			Assets.insert(Entry.Asset);
			std::sort(Entry.RuntimeDependencies.begin(), Entry.RuntimeDependencies.end(), [](const auto& A, const auto& B)
			{
				return std::tuple{ A.Asset, static_cast<uint8_t>(A.Product), A.Subresource } <
					std::tuple{ B.Asset, static_cast<uint8_t>(B.Product), B.Subresource };
			});
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled", Entry.Asset));
			if (std::any_of(Entry.RuntimeDependencies.begin(), Entry.RuntimeDependencies.end(), [](const auto& D) { return D.Asset.IsNil(); }) ||
				std::adjacent_find(Entry.RuntimeDependencies.begin(), Entry.RuntimeDependencies.end()) != Entry.RuntimeDependencies.end())
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Runtime dependencies must be unique non-nil identities", Entry.Asset));
			Json Dependencies = Json::array();
			for (const auto& Dependency : Entry.RuntimeDependencies)
			{
				if (CancellationToken.stop_requested())
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled", Entry.Asset));
				Dependencies.push_back(Json::object({ { "assetId", Dependency.Asset.ToString() },
					{ "product", ProductName(Dependency.Product) }, { "subresource", Dependency.Subresource } }));
			}
			JsonEntries.push_back(Json::object({ { "assetId", Entry.Asset.ToString() },
				{ "product", ProductName(Entry.Product) }, { "subresource", Entry.Subresource },
				{ "payload", Entry.RelativePayloadPath }, { "payloadVersion", Entry.PayloadVersion },
				{ "bytes", Entry.PayloadBytes }, { "digestAlgorithm", Entry.DigestAlgorithm }, { "digest", Entry.Digest },
				{ "runtimeDependencies", std::move(Dependencies) } }));
		}
		for (const CookedAssetEntry& Entry : Entries)
			for (const CookedAssetDependency& Dependency : Entry.RuntimeDependencies)
			{
				if (CancellationToken.stop_requested())
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled", Entry.Asset));
				if (!Identities.contains({ Dependency.Asset, static_cast<uint8_t>(Dependency.Product), Dependency.Subresource }))
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {},
						"Runtime dependency target is absent from cooked entries", Dependency.Asset));
			}
		if (!HasStartupScene)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Startup scene has no cooked scene payload", StartupScene));
		for (const AssetID DynamicRoot : DynamicRoots)
		{
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled", DynamicRoot));
			if (!Assets.contains(DynamicRoot))
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, {}, "Dynamic root is absent from cooked entries", DynamicRoot));
		}

		Json Roots = Json::array();
		for (const AssetID Root : DynamicRoots) Roots.push_back(Root.ToString());
		Json Manifest = Json::object({ { "format", Format }, { "version", ManifestVersion },
			{ "projectId", ProjectID.ToString() }, { "startupScene", StartupScene.ToString() },
			{ "targetPlatform", Compatibility.TargetPlatform }, { "rendererBackend", Compatibility.RendererBackend },
			{ "runtimeABI", Compatibility.RuntimeABI },
			{ "configuration", GetCookedBuildConfigurationName(Compatibility.Configuration) },
			{ "dynamicRoots", std::move(Roots) }, { "entries", std::move(JsonEntries) } });
		std::string Data = Manifest.dump(2) + "\n";
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::Cancelled, {}, "Cook manifest serialization was cancelled"));
		if (Data.size() > MaximumManifestBytes)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::ManifestTooLarge, {}, "Cook manifest exceeds its 16 MiB limit"));
		return Data;
	}

	std::expected<std::shared_ptr<const CookedAssetCatalog>, CookedAssetCatalogError> CookedAssetCatalog::Open(
		const std::filesystem::path& ContentRoot, const CookedAssetCompatibility& ExpectedCompatibility)
	{
		std::error_code Error;
		const std::filesystem::path AbsoluteRoot = std::filesystem::absolute(ContentRoot, Error).lexically_normal();
		if (Error || !std::filesystem::is_directory(AbsoluteRoot, Error) || Error)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidRoot, ContentRoot, "Cooked content root must be an existing directory"));
		if (HasLinkedComponent(AbsoluteRoot, Error) || Error)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidRoot, AbsoluteRoot, "Cooked content root traverses a linked or unobservable path"));
		if (GetCookedBuildConfigurationName(ExpectedCompatibility.Configuration).empty())
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::UnsupportedConfiguration, AbsoluteRoot,
				"Runtime expected an unsupported build configuration identity"));
		const std::filesystem::path ManifestPath = AbsoluteRoot / ManifestFileName;
		if (IsLinked(ManifestPath, Error) || Error)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidRoot, ManifestPath, "Cook manifest is linked or cannot be inspected"));
		const uintmax_t ManifestBytes = std::filesystem::file_size(ManifestPath, Error);
		if (Error || ManifestBytes > MaximumManifestBytes)
			return std::unexpected(MakeError(Error ? CookedAssetCatalogErrorCode::ManifestReadFailed : CookedAssetCatalogErrorCode::ManifestTooLarge,
				ManifestPath, Error ? "Could not stat cook manifest: " + Error.message() : "Cook manifest exceeds its 16 MiB limit"));
		std::ifstream Input(ManifestPath, std::ios::binary);
		if (!Input) return std::unexpected(MakeError(CookedAssetCatalogErrorCode::ManifestReadFailed, ManifestPath, "Could not open cook manifest"));
		std::string Data(static_cast<size_t>(ManifestBytes), '\0');
		Input.read(Data.data(), static_cast<std::streamsize>(Data.size()));
		if (!Input || Input.bad())
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::ManifestReadFailed, ManifestPath, "Could not read complete cook manifest"));

		try
		{
			const Json Root = Json::parse(Data);
			if (!HasExactFields(Root, { "format", "version", "projectId", "startupScene", "targetPlatform", "rendererBackend",
				"runtimeABI", "configuration", "dynamicRoots", "entries" }))
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cook manifest fields are missing or unsupported"));
			if (!Root["format"].is_string() || Root["format"].get<std::string>() != Format)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cook manifest format identifier is invalid"));
			if (!Root["version"].is_number_integer() || Root["version"].get<int64_t>() != ManifestVersion)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::UnsupportedVersion, ManifestPath, "Cook manifest version is unsupported"));
			const auto ReadUUID = [&ManifestPath](const Json& Value, std::string_view Field) -> std::expected<UUID, CookedAssetCatalogError>
			{
				if (!Value.is_string()) return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Manifest field '" + std::string(Field) + "' must be a UUID string"));
				auto Parsed = UUID::Parse(Value.get<std::string>());
				if (!Parsed || Parsed->IsNil()) return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Manifest field '" + std::string(Field) + "' has an invalid UUID"));
				return *Parsed;
			};
			auto ProjectID = ReadUUID(Root["projectId"], "projectId");
			auto Startup = ReadUUID(Root["startupScene"], "startupScene");
			if (!ProjectID) return std::unexpected(ProjectID.error());
			if (!Startup) return std::unexpected(Startup.error());
			for (const std::string_view Field : { "targetPlatform", "rendererBackend", "runtimeABI" })
				if (!Root[std::string(Field)].is_string() || Root[std::string(Field)].get<std::string>().empty())
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Manifest compatibility field is invalid"));
			if (!Root["configuration"].is_string())
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath,
					"Manifest configuration must be Debug, Release, or Dist"));
			auto Configuration = ParseCookedBuildConfiguration(Root["configuration"].get<std::string>());
			if (!Configuration)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::UnsupportedConfiguration, ManifestPath, Configuration.error()));
			CookedAssetCompatibility Compatibility{ Root["targetPlatform"].get<std::string>(), Root["rendererBackend"].get<std::string>(),
				Root["runtimeABI"].get<std::string>(), *Configuration };
			if (Compatibility.TargetPlatform != ExpectedCompatibility.TargetPlatform ||
				Compatibility.RendererBackend != ExpectedCompatibility.RendererBackend || Compatibility.RuntimeABI != ExpectedCompatibility.RuntimeABI)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::IncompatibleTarget, ManifestPath, "Cooked target, renderer or runtime ABI is incompatible"));
			// Configuration is provenance only today: payload adapters produce equivalent bytes in Debug, Release and Dist.
			if (!Root["dynamicRoots"].is_array() || Root["dynamicRoots"].size() > MaximumEntries ||
				!Root["entries"].is_array() || Root["entries"].empty() || Root["entries"].size() > MaximumEntries)
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Manifest roots or entries exceed their limits"));
			std::vector<AssetID> DynamicRoots;
			for (const Json& Value : Root["dynamicRoots"])
			{
				auto ID = ReadUUID(Value, "dynamicRoots");
				if (!ID) return std::unexpected(ID.error());
				DynamicRoots.push_back(*ID);
			}
			if (!std::is_sorted(DynamicRoots.begin(), DynamicRoots.end()) ||
				std::adjacent_find(DynamicRoots.begin(), DynamicRoots.end()) != DynamicRoots.end())
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Dynamic roots are not unique and sorted"));

			std::vector<CookedAssetEntry> Entries;
			Entries.reserve(Root["entries"].size());
			std::set<std::tuple<AssetID, uint8_t, std::string>> Identities;
			std::set<std::string> Payloads;
			std::set<AssetID> Assets;
			uint64_t Aggregate = 0;
			for (const Json& Value : Root["entries"])
			{
				if (!HasExactFields(Value, { "assetId", "product", "subresource", "payload", "payloadVersion", "bytes", "digestAlgorithm", "digest", "runtimeDependencies" }))
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cooked entry fields are missing or unsupported"));
				auto ID = ReadUUID(Value["assetId"], "assetId");
				if (!ID) return std::unexpected(ID.error());
				if (!Value["product"].is_string() || !Value["subresource"].is_string() || !Value["payload"].is_string() ||
					!Value["payloadVersion"].is_number_integer() || !Value["digestAlgorithm"].is_string() || !Value["digest"].is_string() ||
					!Value["runtimeDependencies"].is_array())
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cooked entry has invalid field types", *ID));
				auto Product = ParseProduct(Value["product"].get<std::string>());
				if (!Product) return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cooked entry product type is unsupported", *ID));
				auto Version = ReadUInt64(Value["payloadVersion"]);
				auto Bytes = ReadUInt64(Value["bytes"]);
				if (!Version || !Bytes || *Version == 0 || *Version > std::numeric_limits<uint32_t>::max() || *Bytes == 0 || *Bytes > MaximumPayloadBytes)
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cooked payload version or length is invalid", *ID));
				CookedAssetEntry Entry{ *ID, *Product, Value["subresource"].get<std::string>(), Value["payload"].get<std::string>(),
					static_cast<uint32_t>(*Version), *Bytes, Value["digestAlgorithm"].get<std::string>(), Value["digest"].get<std::string>(), {} };
				if (!IsSafeRelativePayloadPath(Entry.RelativePayloadPath) || Entry.DigestAlgorithm != DigestID || Entry.Digest.size() != 32 ||
					!std::all_of(Entry.Digest.begin(), Entry.Digest.end(), [](unsigned char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); }))
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cooked payload path or digest is invalid", *ID));
				if (!Identities.emplace(Entry.Asset, static_cast<uint8_t>(Entry.Product), Entry.Subresource).second)
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::DuplicateIdentity, ManifestPath, "Manifest repeats an asset product identity", *ID));
				if (!Payloads.emplace(Entry.RelativePayloadPath).second)
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::DuplicateIdentity, ManifestPath, "Manifest repeats a payload path", *ID));
				if (Aggregate > MaximumCatalogPayloadBytes - *Bytes)
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Manifest aggregate payload bytes exceed the catalog limit"));
				Aggregate += *Bytes;
				Assets.insert(*ID);
				for (const Json& DependencyValue : Value["runtimeDependencies"])
				{
					if (!HasExactFields(DependencyValue, { "assetId", "product", "subresource" }) || !DependencyValue["product"].is_string() || !DependencyValue["subresource"].is_string())
						return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Runtime dependency record is malformed", *ID));
					auto DependencyID = ReadUUID(DependencyValue["assetId"], "runtimeDependencies.assetId");
					auto DependencyProduct = ParseProduct(DependencyValue["product"].get<std::string>());
					if (!DependencyID || !DependencyProduct)
						return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Runtime dependency identity is invalid", *ID));
					Entry.RuntimeDependencies.push_back({ *DependencyID, *DependencyProduct, DependencyValue["subresource"].get<std::string>() });
				}
				if (!std::is_sorted(Entry.RuntimeDependencies.begin(), Entry.RuntimeDependencies.end(), [](const auto& A, const auto& B)
					{ return std::tuple{ A.Asset, static_cast<uint8_t>(A.Product), A.Subresource } < std::tuple{ B.Asset, static_cast<uint8_t>(B.Product), B.Subresource }; }) ||
					std::adjacent_find(Entry.RuntimeDependencies.begin(), Entry.RuntimeDependencies.end()) != Entry.RuntimeDependencies.end())
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Runtime dependencies are not unique and sorted", *ID));
				Entries.push_back(std::move(Entry));
			}
			if (!std::is_sorted(Entries.begin(), Entries.end(), [](const auto& A, const auto& B) { return EntryKey(A) < EntryKey(B); }))
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Cooked entries are not deterministically sorted"));
			for (const CookedAssetEntry& Entry : Entries)
				for (const CookedAssetDependency& Dependency : Entry.RuntimeDependencies)
					if (!Identities.contains({ Dependency.Asset, static_cast<uint8_t>(Dependency.Product), Dependency.Subresource }))
						return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath,
							"Runtime dependency target is absent from the manifest", Dependency.Asset));
			if (std::none_of(Entries.begin(), Entries.end(), [&](const auto& E) { return E.Asset == *Startup && E.Product == CookedAssetProductKind::Scene; }))
				return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Startup scene has no scene payload", *Startup));
			for (const AssetID DynamicRoot : DynamicRoots)
				if (!Assets.contains(DynamicRoot))
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, "Dynamic root has no cooked product", DynamicRoot));

			for (const CookedAssetEntry& Entry : Entries)
			{
				const std::filesystem::path PayloadPath = AbsoluteRoot / std::filesystem::u8path(Entry.RelativePayloadPath);
				if (HasLinkedComponent(PayloadPath, Error) || Error)
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::UnsafePayloadPath, PayloadPath, "Payload path is linked or cannot be inspected", Entry.Asset));
				const auto Status = std::filesystem::status(PayloadPath, Error);
				if (Error || !std::filesystem::is_regular_file(Status))
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::MissingPayload, PayloadPath, "Cooked payload is missing or not a regular file", Entry.Asset));
				const uintmax_t ActualBytes = std::filesystem::file_size(PayloadPath, Error);
				if (Error || ActualBytes != Entry.PayloadBytes)
					return std::unexpected(MakeError(CookedAssetCatalogErrorCode::PayloadReadFailed, PayloadPath, "Cooked payload length differs from the manifest", Entry.Asset));
			}
			return std::shared_ptr<const CookedAssetCatalog>(new CookedAssetCatalog(
				AbsoluteRoot, *ProjectID, *Startup, std::move(Compatibility), std::move(DynamicRoots), std::move(Entries)));
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, std::string("Cook manifest JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::InvalidManifest, ManifestPath, std::string("Cook manifest parsing failed: ") + Exception.what()));
		}
	}

	const CookedAssetEntry* CookedAssetCatalog::Find(
		const AssetID& Asset, CookedAssetProductKind Product, std::string_view Subresource) const noexcept
	{
		const auto Iter = std::lower_bound(m_Entries.begin(), m_Entries.end(), std::tuple{ Asset, static_cast<uint8_t>(Product), Subresource },
			[](const CookedAssetEntry& Entry, const auto& Key)
			{
				return EntryKey(Entry) < Key;
			});
		return Iter != m_Entries.end() && Iter->Asset == Asset && Iter->Product == Product && Iter->Subresource == Subresource ? &*Iter : nullptr;
	}

	std::expected<std::vector<std::byte>, CookedAssetCatalogError> CookedAssetCatalog::ReadPayload(const CookedAssetEntry& Entry) const
	{
		const CookedAssetEntry* Owned = Find(Entry.Asset, Entry.Product, Entry.Subresource);
		if (!Owned || *Owned != Entry)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::AssetNotFound, m_ContentRoot, "Payload descriptor is not owned by this catalog", Entry.Asset));
		const std::filesystem::path Path = m_ContentRoot / std::filesystem::u8path(Owned->RelativePayloadPath);
		std::error_code Error;
		if (HasLinkedComponent(Path, Error) || Error)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::UnsafePayloadPath, Path, "Cooked payload path is linked or cannot be inspected", Entry.Asset));
		const uintmax_t Bytes = std::filesystem::file_size(Path, Error);
		if (Error || Bytes != Owned->PayloadBytes || Bytes > MaximumPayloadBytes || Bytes > std::numeric_limits<size_t>::max() ||
			Bytes > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::PayloadTooLarge, Path, "Cooked payload length is unavailable, changed or exceeds limits", Entry.Asset));
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::unexpected(MakeError(CookedAssetCatalogErrorCode::PayloadReadFailed, Path, "Could not open cooked payload", Entry.Asset));
		std::vector<std::byte> Payload(static_cast<size_t>(Bytes));
		Input.read(reinterpret_cast<char*>(Payload.data()), static_cast<std::streamsize>(Payload.size()));
		if (!Input || Input.bad()) return std::unexpected(MakeError(CookedAssetCatalogErrorCode::PayloadReadFailed, Path, "Could not read complete cooked payload", Entry.Asset));
		const CookedAssetDigest ActualDigest = ComputePayloadDigest(Payload);
		if (ActualDigest.Algorithm != Owned->DigestAlgorithm || ActualDigest.Value != Owned->Digest)
			return std::unexpected(MakeError(CookedAssetCatalogErrorCode::PayloadDigestMismatch, Path, "Cooked payload digest differs from the manifest", Entry.Asset));
		return Payload;
	}
} // namespace PulseForge
