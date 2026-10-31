#include "Editor/EditorScriptAuthoringService.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"

#include <fstream>
#include <string_view>

namespace PulseForgeEditor
{
	namespace
	{
		constexpr uint64_t MaximumEditorScriptSourceBytes = 2ull * 1024ull * 1024ull;

		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.generic_u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}
	}

	std::expected<std::filesystem::path, std::string> ResolveScriptSourcePath(
		const std::filesystem::path& ProjectRoot, const PulseForge::AssetRegistry& Registry,
		const PulseForge::AssetID& ScriptAsset)
	{
		const auto Record = Registry.Find(ScriptAsset);
		if (!Record) return std::unexpected("The script asset is no longer registered in the active project.");
		if (!PulseForge::HasAssetTypeExtension(Record->ProjectRelativePath, PulseForge::AssetFileType::Script))
			return std::unexpected("The selected asset is not a managed Lua script.");
		auto Path = PulseForge::AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
		if (!Path) return std::unexpected("The script asset path is not a safe managed source file: " + Path.error().Message);
		return *Path;
	}

	std::expected<PulseForge::ScriptSourceMetadata, std::string> ReadScriptSourceMetadata(
		const std::filesystem::path& ProjectRoot, const PulseForge::AssetRegistry& Registry,
		const PulseForge::AssetID& ScriptAsset)
	{
		auto Path = ResolveScriptSourcePath(ProjectRoot, Registry, ScriptAsset);
		if (!Path) return std::unexpected(Path.error());
		std::ifstream Input(*Path, std::ios::binary);
		if (!Input) return std::unexpected("Could not open managed Lua script source.");
		Input.seekg(0, std::ios::end);
		const std::streamoff Length = Input.tellg();
		if (Length < 0 || static_cast<uint64_t>(Length) > MaximumEditorScriptSourceBytes)
			return std::unexpected("Lua script source exceeds the 2 MiB authoring inspection limit.");
		std::string Source(static_cast<size_t>(Length), '\0');
		Input.seekg(0, std::ios::beg);
		if (!Source.empty()) Input.read(Source.data(), static_cast<std::streamsize>(Source.size()));
		if (!Input || static_cast<size_t>(Input.gcount()) != Source.size())
			return std::unexpected("Could not read the complete managed Lua script source.");
		auto Metadata = PulseForge::ParseScriptSourceMetadata(Source);
		if (!Metadata)
			return std::unexpected(PathToUtf8(*Path) + ":" + std::to_string(Metadata.error().Line) + ": " + Metadata.error().Message);
		return std::move(*Metadata);
	}
} // namespace PulseForgeEditor
