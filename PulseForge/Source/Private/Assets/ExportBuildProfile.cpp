#include "Assets/ExportBuildProfile.h"

#include "Core/BoundedInput.h"
#include "Core/PlatformServices.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <limits>
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

		constexpr size_t MaximumDisplayNameBytes = 256;
		constexpr size_t MaximumOutputPathBytes = 4096;

		ExportBuildProfileError MakeError(
			ExportBuildProfileErrorCode Code,
			const std::filesystem::path& Path,
			std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		std::string PathForMessage(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}

		std::string ToUtf8(const std::u8string& Value)
		{
			return { reinterpret_cast<const char*>(Value.data()), Value.size() };
		}

		bool IsWhitespaceOnly(std::string_view Value)
		{
			return std::all_of(Value.begin(), Value.end(), [](unsigned char Character)
			{
				return std::isspace(Character) != 0;
			});
		}

		std::expected<void, std::string> ValidateRelativeOutputPath(const std::filesystem::path& Path)
		{
			if (Path.empty() || Path.has_root_name() || Path.has_root_directory() || Path.is_absolute())
				return std::unexpected("defaultOutputPath must be a non-empty relative path");

			const std::string GenericPath = ToUtf8(Path.generic_u8string());
			if (GenericPath.empty() || GenericPath.size() > MaximumOutputPathBytes || GenericPath.front() == '/' ||
				GenericPath.find('\\') != std::string::npos)
				return std::unexpected("defaultOutputPath must be a bounded relative path using forward slashes");

			size_t ComponentStart = 0;
			while (ComponentStart <= GenericPath.size())
			{
				const size_t Separator = GenericPath.find('/', ComponentStart);
				const size_t ComponentEnd = Separator == std::string::npos ? GenericPath.size() : Separator;
				const std::string_view Component(GenericPath.data() + ComponentStart, ComponentEnd - ComponentStart);
				if (Component.empty() || Component == "." || Component == ".." ||
					Component.find(':') != std::string_view::npos)
					return std::unexpected("defaultOutputPath contains an empty, rooted, or traversal component");
				if (Separator == std::string::npos) break;
				ComponentStart = Separator + 1;
			}

			return {};
		}

		std::expected<void, ExportBuildProfileError> ValidateProfile(
			const ExportBuildProfile& Profile,
			const std::filesystem::path& Path)
		{
			if (Profile.ProfileID.IsNil())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Profile UUID must not be nil"));
			if (Profile.DisplayName.empty() || Profile.DisplayName.size() > MaximumDisplayNameBytes || IsWhitespaceOnly(Profile.DisplayName))
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Profile display name must contain 1 to 256 non-whitespace UTF-8 bytes"));

			switch (Profile.TargetPlatform)
			{
			case ExportTargetPlatform::Windows:
			case ExportTargetPlatform::Linux:
			case ExportTargetPlatform::MacOS: break;
			default:
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Target platform is unsupported"));
			}
			switch (Profile.Architecture)
			{
			case ExportTargetArchitecture::X64:
			case ExportTargetArchitecture::Arm64: break;
			default:
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Target architecture is unsupported"));
			}
			if (Profile.TargetPlatform == ExportTargetPlatform::Windows && Profile.Architecture != ExportTargetArchitecture::X64)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Windows export currently supports x64 only"));
			if (Profile.TargetPlatform == ExportTargetPlatform::Linux && Profile.Architecture != ExportTargetArchitecture::X64)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Linux export currently supports x64 only"));
			if (Profile.TargetPlatform == ExportTargetPlatform::MacOS && Profile.Architecture != ExportTargetArchitecture::Arm64)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "macOS export currently supports Apple Silicon arm64 only"));

			if (Profile.Configuration != CookedBuildConfiguration::Debug &&
				Profile.Configuration != CookedBuildConfiguration::Release &&
				Profile.Configuration != CookedBuildConfiguration::Dist)
			{
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Build configuration must be Debug, Release, or Dist"));
			}
			if (Profile.Renderer != ExportRendererBackend::Vulkan)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Vulkan is the only supported export renderer"));
			if (Profile.StartupScene.IsNil())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "A non-nil startup scene UUID is required"));
			if (Profile.DynamicRoots.size() > ExportBuildProfileSerializer::MaximumDynamicRoots)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Dynamic root count exceeds the profile limit"));
			std::set<AssetID> UniqueRoots;
			for (const AssetID& Root : Profile.DynamicRoots)
			{
				if (Root.IsNil())
					return std::unexpected(MakeError(
						ExportBuildProfileErrorCode::InvalidProfile, Path, "Dynamic root UUIDs must not be nil"));
				if (!UniqueRoots.insert(Root).second)
					return std::unexpected(MakeError(
						ExportBuildProfileErrorCode::InvalidProfile, Path, "Dynamic root UUIDs must be unique"));
			}

			if (auto OutputValidation = ValidateRelativeOutputPath(Profile.DefaultOutputPath); !OutputValidation)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, OutputValidation.error()));
			if (Profile.Window && (Profile.Window->Width == 0 || Profile.Window->Height == 0 ||
				Profile.Window->Width > ExportBuildProfileSerializer::MaximumWindowDimension ||
				Profile.Window->Height > ExportBuildProfileSerializer::MaximumWindowDimension))
			{
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidProfile, Path, "Window width and height must be between 1 and 16384"));
			}
			return {};
		}

		std::optional<std::string> FindUnknownField(const Json& Object, std::initializer_list<std::string_view> AllowedFields)
		{
			for (const auto& [Key, Value] : Object.items())
			{
				(void)Value;
				if (std::find(AllowedFields.begin(), AllowedFields.end(), Key) == AllowedFields.end()) return Key;
			}
			return std::nullopt;
		}

		class DuplicateJsonKeyError final : public std::runtime_error
		{
		public:
			DuplicateJsonKeyError() : std::runtime_error("JSON object contains a duplicate field") {}
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
			Json Document = Json::parse(Data.begin(), Data.end(), Callback);
			if (HasDuplicateKey) throw DuplicateJsonKeyError();
			return Document;
		}

		std::expected<AssetID, ExportBuildProfileError> ParseAssetID(
			const Json& Value,
			const std::filesystem::path& Path,
			std::string_view Field)
		{
			if (!Value.is_string())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, std::string(Field) + " must be a UUID string"));
			const auto Parsed = UUID::Parse(Value.get<std::string>());
			if (!Parsed || Parsed->IsNil())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path,
					Parsed ? std::string(Field) + " UUID must not be nil" : std::string(Field) + ": " + Parsed.error().Message));
			return *Parsed;
		}

		std::expected<ExportTargetPlatform, ExportBuildProfileError> ParseTargetPlatform(
			const Json& Value,
			const std::filesystem::path& Path)
		{
			if (!Value.is_string())
				return std::unexpected(MakeError(ExportBuildProfileErrorCode::InvalidDocument, Path, "targetPlatform must be a string"));
			const std::string& Name = Value.get_ref<const std::string&>();
			if (Name == "Windows") return ExportTargetPlatform::Windows;
			if (Name == "Linux") return ExportTargetPlatform::Linux;
			if (Name == "macOS") return ExportTargetPlatform::MacOS;
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::InvalidDocument, Path, "targetPlatform must be Windows, Linux, or macOS"));
		}

		std::expected<ExportTargetArchitecture, ExportBuildProfileError> ParseArchitecture(
			const Json& Value,
			const std::filesystem::path& Path)
		{
			if (!Value.is_string())
				return std::unexpected(MakeError(ExportBuildProfileErrorCode::InvalidDocument, Path, "architecture must be a string"));
			const std::string& Name = Value.get_ref<const std::string&>();
			if (Name == "x64") return ExportTargetArchitecture::X64;
			if (Name == "arm64") return ExportTargetArchitecture::Arm64;
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::InvalidDocument, Path, "architecture must be x64 or arm64"));
		}

		std::expected<uint32_t, ExportBuildProfileError> ParseDimension(
			const Json& Value,
			const std::filesystem::path& Path,
			std::string_view Field)
		{
			if (!Value.is_number_integer() && !Value.is_number_unsigned())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, std::string(Field) + " must be an integer"));
			const int64_t Dimension = Value.is_number_unsigned()
				? (Value.get<uint64_t>() > std::numeric_limits<uint32_t>::max()
						? -1
						: static_cast<int64_t>(Value.get<uint64_t>()))
				: Value.get<int64_t>();
			if (Dimension <= 0 || Dimension > ExportBuildProfileSerializer::MaximumWindowDimension)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, std::string(Field) + " must be between 1 and 16384"));
			return static_cast<uint32_t>(Dimension);
		}
	} // namespace

	std::expected<std::string, ExportBuildProfileError> ExportBuildProfileSerializer::Serialize(
		const ExportBuildProfile& Profile,
		const std::filesystem::path& Path)
	{
		try
		{
			if (auto Validation = ValidateProfile(Profile, Path); !Validation)
				return std::unexpected(std::move(Validation.error()));

			Json Document = Json::object();
			Document["format"] = std::string(FormatIdentifier);
			Document["version"] = CurrentVersion;
			Document["profileId"] = Profile.ProfileID.ToString();
			Document["displayName"] = Profile.DisplayName;
			switch (Profile.TargetPlatform)
			{
			case ExportTargetPlatform::Windows: Document["targetPlatform"] = "Windows"; break;
			case ExportTargetPlatform::Linux: Document["targetPlatform"] = "Linux"; break;
			case ExportTargetPlatform::MacOS: Document["targetPlatform"] = "macOS"; break;
			default: break;
			}
			Document["architecture"] = Profile.Architecture == ExportTargetArchitecture::X64 ? "x64" : "arm64";
			Document["configuration"] = std::string(GetCookedBuildConfigurationName(Profile.Configuration));
			Document["renderer"] = "Vulkan";
			Document["startupScene"] = Profile.StartupScene.ToString();
			std::vector<AssetID> SortedRoots = Profile.DynamicRoots;
			std::sort(SortedRoots.begin(), SortedRoots.end());
			Json Roots = Json::array();
			for (const AssetID& Root : SortedRoots) Roots.push_back(Root.ToString());
			Document["dynamicRoots"] = std::move(Roots);
			Document["defaultOutputPath"] = ToUtf8(Profile.DefaultOutputPath.generic_u8string());
			if (Profile.Window)
			{
				Document["window"] = Json{ { "width", Profile.Window->Width }, { "height", Profile.Window->Height } };
			}

			std::string Data = Document.dump(2, ' ', false, Json::error_handler_t::strict) + "\n";
			if (Data.size() > MaximumDocumentBytes)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::DocumentTooLarge, Path, "Serialized build profile exceeds the 64 KiB document limit"));
			return Data;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::InvalidProfile, Path, std::string("Build profile serialization failed: ") + Exception.what()));
		}
	}

	std::expected<ExportBuildProfile, ExportBuildProfileError> ExportBuildProfileSerializer::Parse(
		std::string_view Data,
		const std::filesystem::path& Path)
	{
		if (Data.size() > MaximumDocumentBytes)
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::DocumentTooLarge, Path, "Build profile exceeds the 64 KiB document limit"));

		try
		{
			const Json Document = ParseUniqueJson(Data);
			if (!Document.is_object())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "Build profile root must be a JSON object"));
			if (const auto Unknown = FindUnknownField(Document,
				{ "format", "version", "profileId", "displayName", "targetPlatform", "architecture", "configuration",
					"renderer", "startupScene", "dynamicRoots", "defaultOutputPath", "window" }))
			{
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "Build profile contains unsupported field '" + *Unknown + "'"));
			}

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "Build profile is missing its format identifier"));
			if (Format->get<std::string>() != FormatIdentifier)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::UnsupportedFormat, Path, "Document is not a PulseForge build profile"));
			const auto Version = Document.find("version");
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "Build profile requires an integer version"));
			const bool SupportedVersion = Version->is_number_unsigned()
				? Version->get<uint64_t>() == CurrentVersion
				: Version->get<int64_t>() == CurrentVersion;
			if (!SupportedVersion)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::UnsupportedVersion, Path, "Build profile version is not supported"));

			const auto ProfileIDValue = Document.find("profileId");
			const auto NameValue = Document.find("displayName");
			const auto PlatformValue = Document.find("targetPlatform");
			const auto ArchitectureValue = Document.find("architecture");
			const auto ConfigurationValue = Document.find("configuration");
			const auto RendererValue = Document.find("renderer");
			const auto StartupValue = Document.find("startupScene");
			const auto RootsValue = Document.find("dynamicRoots");
			const auto OutputValue = Document.find("defaultOutputPath");
			if (ProfileIDValue == Document.end() || NameValue == Document.end() || PlatformValue == Document.end() ||
				ArchitectureValue == Document.end() || ConfigurationValue == Document.end() || RendererValue == Document.end() ||
				StartupValue == Document.end() || RootsValue == Document.end() || OutputValue == Document.end())
			{
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "Build profile is missing one or more required fields"));
			}
			if (!NameValue->is_string() || !RendererValue->is_string() || RendererValue->get<std::string>() != "Vulkan")
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "displayName must be a string and renderer must be Vulkan"));
			if (!ConfigurationValue->is_string())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "configuration must be a string"));

			const auto ProfileID = ParseAssetID(*ProfileIDValue, Path, "profileId");
			const auto Platform = ParseTargetPlatform(*PlatformValue, Path);
			const auto Architecture = ParseArchitecture(*ArchitectureValue, Path);
			const auto Configuration = ParseCookedBuildConfiguration(ConfigurationValue->get<std::string>());
			const auto Startup = ParseAssetID(*StartupValue, Path, "startupScene");
			if (!ProfileID) return std::unexpected(ProfileID.error());
			if (!Platform) return std::unexpected(Platform.error());
			if (!Architecture) return std::unexpected(Architecture.error());
			if (!Configuration)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, Configuration.error()));
			if (!Startup) return std::unexpected(Startup.error());
			if (!RootsValue->is_array() || RootsValue->size() > MaximumDynamicRoots)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "dynamicRoots must be an array within the profile limit"));
			if (!OutputValue->is_string())
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "defaultOutputPath must be a string"));

			const std::string OutputUtf8 = OutputValue->get<std::string>();
			if (OutputUtf8.size() > MaximumOutputPathBytes)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, "defaultOutputPath exceeds its size limit"));
			ExportBuildProfile Profile;
			Profile.ProfileID = *ProfileID;
			Profile.DisplayName = NameValue->get<std::string>();
			Profile.TargetPlatform = *Platform;
			Profile.Architecture = *Architecture;
			Profile.Configuration = *Configuration;
			Profile.Renderer = ExportRendererBackend::Vulkan;
			Profile.StartupScene = *Startup;
			Profile.DefaultOutputPath = std::filesystem::u8path(OutputUtf8.begin(), OutputUtf8.end());
			for (const Json& RootValue : *RootsValue)
			{
				const auto Root = ParseAssetID(RootValue, Path, "dynamicRoots entry");
				if (!Root) return std::unexpected(Root.error());
				Profile.DynamicRoots.push_back(*Root);
			}
			std::sort(Profile.DynamicRoots.begin(), Profile.DynamicRoots.end());

			if (const auto WindowValue = Document.find("window"); WindowValue != Document.end())
			{
				if (!WindowValue->is_object())
					return std::unexpected(MakeError(
						ExportBuildProfileErrorCode::InvalidDocument, Path, "window must be an object when present"));
				if (const auto Unknown = FindUnknownField(*WindowValue, { "width", "height" }))
					return std::unexpected(MakeError(
						ExportBuildProfileErrorCode::InvalidDocument, Path, "window contains unsupported field '" + *Unknown + "'"));
				const auto WidthValue = WindowValue->find("width");
				const auto HeightValue = WindowValue->find("height");
				if (WidthValue == WindowValue->end() || HeightValue == WindowValue->end())
					return std::unexpected(MakeError(
						ExportBuildProfileErrorCode::InvalidDocument, Path, "window requires both width and height"));
				const auto Width = ParseDimension(*WidthValue, Path, "window.width");
				const auto Height = ParseDimension(*HeightValue, Path, "window.height");
				if (!Width) return std::unexpected(Width.error());
				if (!Height) return std::unexpected(Height.error());
				Profile.Window = ExportWindowSettings{ *Width, *Height };
			}

			if (auto Validation = ValidateProfile(Profile, Path); !Validation)
				return std::unexpected(MakeError(
					ExportBuildProfileErrorCode::InvalidDocument, Path, Validation.error().Message));
			return Profile;
		}
		catch (const DuplicateJsonKeyError& Exception)
		{
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::InvalidDocument, Path, Exception.what()));
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::InvalidDocument, Path, std::string("Build profile JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::InvalidDocument, Path, std::string("Build profile parsing failed: ") + Exception.what()));
		}
	}

	std::expected<ExportBuildProfile, ExportBuildProfileError> ExportBuildProfileSerializer::Load(
		const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input.is_open())
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileOpenFailed, Path, "Could not open build profile: " + PathForMessage(Path)));
		auto Data = Detail::ReadBoundedInput(Input, MaximumDocumentBytes);
		if (!Data)
			return std::unexpected(MakeError(
				Data.error().Code == Detail::BoundedInputErrorCode::TooLarge
					? ExportBuildProfileErrorCode::DocumentTooLarge
					: ExportBuildProfileErrorCode::FileReadFailed,
				Path,
				Data.error().Code == Detail::BoundedInputErrorCode::TooLarge
					? "Build profile exceeds the 64 KiB document limit"
					: "Could not read the complete build profile"));
		auto Profile = Parse(*Data, Path);
		if (!Profile) Profile.error().Path = Path;
		return Profile;
	}

	std::expected<void, ExportBuildProfileError> ExportBuildProfileSerializer::Save(
		const std::filesystem::path& Path,
		const ExportBuildProfile& Profile)
	{
		if (Path.empty())
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::InvalidProfile, Path, "Build profile destination must not be empty"));
		const auto Serialized = Serialize(Profile, Path);
		if (!Serialized) return std::unexpected(Serialized.error());

		std::error_code Error;
		const std::filesystem::path AbsolutePath = std::filesystem::absolute(Path, Error).lexically_normal();
		if (Error)
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileWriteFailed, Path, "Could not resolve build profile destination: " + Error.message()));
		const std::filesystem::path ParentPath = AbsolutePath.parent_path();
		const auto ParentStatus = std::filesystem::symlink_status(ParentPath, Error);
		if (Error || !std::filesystem::is_directory(ParentStatus) || std::filesystem::is_symlink(ParentStatus))
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileWriteFailed, ParentPath, "Build profile parent must be an accessible, non-linked directory"));

		const auto DestinationStatus = std::filesystem::symlink_status(AbsolutePath, Error);
		if (Error != std::errc::no_such_file_or_directory && Error)
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileWriteFailed, AbsolutePath, "Could not inspect build profile destination: " + Error.message()));
		if (!Error && (!std::filesystem::is_regular_file(DestinationStatus) || std::filesystem::is_symlink(DestinationStatus)))
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileWriteFailed, AbsolutePath, "Build profile destination must be a regular, non-symlink file"));

		const auto TemporaryID = UUID::Generate();
		if (!TemporaryID)
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileWriteFailed, AbsolutePath, TemporaryID.error().Message));
		std::filesystem::path TemporaryPath = AbsolutePath;
		TemporaryPath += "." + TemporaryID->ToString() + ".tmp";
		struct TemporaryCleanup final
		{
			std::filesystem::path Path;
			~TemporaryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove(Path, CleanupError);
			}
		} Cleanup{ TemporaryPath };

		std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
		if (!Output.is_open())
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileWriteFailed, TemporaryPath, "Could not open temporary build profile for writing"));
		Output.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
		Output.flush();
		Output.close();
		if (!Output)
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileWriteFailed, TemporaryPath, "Could not finish writing build profile"));

		auto Replaced = Platform::ReplaceFileAtomically(TemporaryPath, AbsolutePath);
		if (!Replaced)
			return std::unexpected(MakeError(
				ExportBuildProfileErrorCode::FileReplaceFailed, AbsolutePath, Replaced.error().Message));
		return {};
	}
} // namespace PulseForge
