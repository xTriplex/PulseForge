#include "Assets/ScriptSourceMetadata.h"

#include "Assets/AssetTypes.h"
#include "Scene/Components/ScriptComponent.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::json;
		constexpr std::string_view MetadataMarker = "--[[PulseForgeScript:1";
		constexpr size_t MaximumMetadataBytes = 64u * 1024u;
		constexpr size_t MaximumPropertyDeclarations = 64;
		constexpr size_t MaximumModuleDeclarations = 64;
		constexpr size_t MaximumModuleNameBytes = 256;
		constexpr size_t MaximumLabelBytes = 128;

		ScriptSourceMetadataError Error(std::string Message, size_t Line = 1)
		{
			return { Line, std::move(Message) };
		}

		size_t LineForOffset(std::string_view Text, size_t Offset)
		{
			const size_t End = std::min(Offset, Text.size());
			return 1 + static_cast<size_t>(std::count(Text.begin(), Text.begin() + static_cast<std::ptrdiff_t>(End), '\n'));
		}

		std::string PathUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Text = Path.generic_u8string();
			return { reinterpret_cast<const char*>(Text.data()), Text.size() };
		}

		bool IsScriptsRootedPath(const std::filesystem::path& Path)
		{
			auto It = Path.begin();
			if (It == Path.end() || It->generic_string() != "Assets") return false;
			++It;
			return It != Path.end() && It->generic_string() == "Scripts";
		}

		bool IsFiniteFloat(const Json& Value, float& Result)
		{
			if (!Value.is_number()) return false;
			const double Number = Value.get<double>();
			if (!std::isfinite(Number) || Number < -static_cast<double>(MaximumScriptFloatPropertyMagnitude) ||
				Number > static_cast<double>(MaximumScriptFloatPropertyMagnitude))
				return false;
			Result = static_cast<float>(Number);
			return std::isfinite(Result);
		}

		bool HasOnlyKeys(const Json& Value, std::initializer_list<std::string_view> Allowed)
		{
			if (!Value.is_object()) return false;
			for (auto It = Value.begin(); It != Value.end(); ++It)
				if (std::find(Allowed.begin(), Allowed.end(), It.key()) == Allowed.end()) return false;
			return true;
		}

		std::string LowerAscii(std::string_view Value)
		{
			std::string Result(Value);
			std::transform(Result.begin(), Result.end(), Result.begin(), [](unsigned char Character)
			{
				return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character + ('a' - 'A')) : static_cast<char>(Character);
			});
			return Result;
		}
	} // namespace

	std::expected<std::filesystem::path, ScriptSourceMetadataError> ResolveScriptModuleAssetPath(
		std::string_view ImporterProjectRelativePath,
		std::string_view DeclaredRelativePath)
	{
		if (ImporterProjectRelativePath.empty() || ImporterProjectRelativePath.size() > 4096 ||
			DeclaredRelativePath.empty() || DeclaredRelativePath.size() > MaximumModuleNameBytes)
			return std::unexpected(Error("Script module path is empty or exceeds its size limit"));
		if (ImporterProjectRelativePath.find('\\') != std::string_view::npos ||
			DeclaredRelativePath.find('\\') != std::string_view::npos ||
			DeclaredRelativePath.find('\0') != std::string_view::npos)
			return std::unexpected(Error("Script module paths must use slash-separated UTF-8 names"));
		if (DeclaredRelativePath.find(':') != std::string_view::npos ||
			DeclaredRelativePath.front() == '/' || DeclaredRelativePath.back() == '/' ||
			DeclaredRelativePath.ends_with(".lua"))
			return std::unexpected(Error("Script module names must be relative, extensionless paths"));

		const std::filesystem::path Importer = std::filesystem::u8path(ImporterProjectRelativePath).lexically_normal();
		if (!IsScriptsRootedPath(Importer) || !AssetExtensionsEqual(Importer.extension().string(), ".lua"))
			return std::unexpected(Error("Script module importer must be a managed .lua asset under Assets/Scripts"));

		const std::filesystem::path RelativeModule = std::filesystem::u8path(DeclaredRelativePath);
		if (RelativeModule.is_absolute() || RelativeModule.has_root_name() || RelativeModule.has_root_directory())
			return std::unexpected(Error("Absolute script module paths are not supported"));
		const std::filesystem::path Resolved = (Importer.parent_path() / RelativeModule).lexically_normal().concat(".lua");
		if (!IsScriptsRootedPath(Resolved) || Resolved.extension() != ".lua")
			return std::unexpected(Error("Script module path escapes the managed Assets/Scripts root"));
		for (const auto& Part : Resolved)
			if (Part == "..") return std::unexpected(Error("Script module path is not normalized within Assets/Scripts"));
		return Resolved;
	}

	std::expected<ScriptSourceMetadata, ScriptSourceMetadataError> ParseScriptSourceMetadata(std::string_view Source)
	{
		ScriptSourceMetadata Result;
		if (!Source.starts_with(MetadataMarker)) return Result;
		size_t JsonStart = MetadataMarker.size();
		if (Source.substr(JsonStart).starts_with("\r\n")) JsonStart += 2;
		else if (Source.substr(JsonStart).starts_with('\n')) ++JsonStart;
		else return std::unexpected(Error("Script metadata marker must be followed by a line ending"));
		const size_t End = Source.find("\n]]", JsonStart);
		if (End == std::string_view::npos)
			return std::unexpected(Error("Script metadata block is not terminated with ]]"));
		const size_t JsonBytes = End - JsonStart;
		if (JsonBytes > MaximumMetadataBytes)
			return std::unexpected(Error("Script metadata block exceeds the 64 KiB limit"));

		Json Document;
		try
		{
			Document = Json::parse(Source.substr(JsonStart, JsonBytes));
		}
		catch (const Json::parse_error& Exception)
		{
			const std::string_view JsonText = Source.substr(JsonStart, JsonBytes);
			return std::unexpected(Error(std::string("Invalid script metadata JSON: ") + Exception.what(),
				LineForOffset(JsonText, Exception.byte)));
		}
		catch (const Json::exception& Exception)
		{
			return std::unexpected(Error(std::string("Invalid script metadata JSON: ") + Exception.what()));
		}
		if (!HasOnlyKeys(Document, { "version", "properties", "modules" }))
			return std::unexpected(Error("Script metadata must be an object containing only version, properties, and modules"));
		if (!Document.contains("version") || !Document["version"].is_number_integer() || Document["version"].get<int>() != 1)
			return std::unexpected(Error("Script metadata version must be the integer 1"));
		Result.HasDeclarations = true;

		if (Document.contains("properties"))
		{
			const Json& Properties = Document["properties"];
			if (!Properties.is_array() || Properties.size() > MaximumPropertyDeclarations)
				return std::unexpected(Error("Script properties must be an array of at most 64 declarations"));
			std::set<std::string, std::less<>> IDs;
			for (const Json& Property : Properties)
			{
				if (!HasOnlyKeys(Property, { "id", "label", "type", "default", "min", "max" }) ||
					Property.size() != 6 || !Property.contains("id") || !Property["id"].is_string() ||
					!Property.contains("label") || !Property["label"].is_string() ||
					!Property.contains("type") || !Property["type"].is_string() || Property["type"].get<std::string>() != "float")
					return std::unexpected(Error("Each script property requires id, label, type=\"float\", default, min, and max"));
				ScriptFloatPropertyDeclaration Declaration;
				Declaration.ID = Property["id"].get<std::string>();
				Declaration.Label = Property["label"].get<std::string>();
				if (!IsValidScriptPropertyIdentifier(Declaration.ID))
					return std::unexpected(Error("Script property ID is invalid; use a stable ScriptComponent property ID"));
				if (!IDs.emplace(Declaration.ID).second)
					return std::unexpected(Error("Script property IDs must be unique"));
				if (Declaration.Label.empty() || Declaration.Label.size() > MaximumLabelBytes ||
					std::any_of(Declaration.Label.begin(), Declaration.Label.end(), [](unsigned char Character) { return Character < 0x20; }))
					return std::unexpected(Error("Script property label must contain 1 to 128 printable UTF-8 bytes"));
				if (!IsFiniteFloat(Property["default"], Declaration.DefaultValue) ||
					!IsFiniteFloat(Property["min"], Declaration.Minimum) || !IsFiniteFloat(Property["max"], Declaration.Maximum) ||
					Declaration.Minimum > Declaration.DefaultValue || Declaration.DefaultValue > Declaration.Maximum)
					return std::unexpected(Error("Script property default and limits must be finite and satisfy min <= default <= max"));
				Result.FloatProperties.push_back(std::move(Declaration));
			}
		}

		if (Document.contains("modules"))
		{
			const Json& Modules = Document["modules"];
			if (!Modules.is_array() || Modules.size() > MaximumModuleDeclarations)
				return std::unexpected(Error("Script modules must be an array of at most 64 declarations"));
			std::set<std::string, std::less<>> Names;
			for (const Json& Module : Modules)
			{
				if (!Module.is_string()) return std::unexpected(Error("Script module declarations must be strings"));
				std::string Name = Module.get<std::string>();
				if (Name.empty() || Name.size() > MaximumModuleNameBytes)
					return std::unexpected(Error("Script module name is empty or exceeds 256 bytes"));
				const auto Resolved = ResolveScriptModuleAssetPath("Assets/Scripts/__metadata__.lua", Name);
				if (!Resolved) return std::unexpected(Resolved.error());
				const std::string Normalized = LowerAscii(Name);
				if (!Names.emplace(Normalized).second)
					return std::unexpected(Error("Script module declarations contain a duplicate path"));
				Result.Modules.push_back(std::move(Name));
			}
		}
		return Result;
	}
} // namespace PulseForge
