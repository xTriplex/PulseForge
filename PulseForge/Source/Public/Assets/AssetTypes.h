#pragma once

#include <filesystem>
#include <initializer_list>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace PulseForge
{
	enum class AssetFileType : uint8_t
	{
		Scene,
		Prefab,
		Mesh,
		Material,
		Script,
		Audio,
		Environment,
		Image,
		PreviewImage,
		ShaderSource,
		Project
	};

	[[nodiscard]] constexpr unsigned char FoldAssetExtensionCharacter(unsigned char Character) noexcept
	{
		return Character >= 'A' && Character <= 'Z' ? static_cast<unsigned char>(Character + ('a' - 'A')) : Character;
	}

	[[nodiscard]] constexpr bool AssetExtensionsEqual(std::string_view First, std::string_view Second) noexcept
	{
		if (First.size() != Second.size()) return false;
		for (size_t Index = 0; Index < First.size(); ++Index)
			if (FoldAssetExtensionCharacter(static_cast<unsigned char>(First[Index])) !=
				FoldAssetExtensionCharacter(static_cast<unsigned char>(Second[Index])))
				return false;
		return true;
	}

	[[nodiscard]] inline std::string GetNormalizedAssetExtension(const std::filesystem::path& Path)
	{
		std::string Extension = Path.extension().string();
		for (char& Character : Extension)
			Character = static_cast<char>(FoldAssetExtensionCharacter(static_cast<unsigned char>(Character)));
		return Extension;
	}

	[[nodiscard]] inline bool HasAssetExtension(const std::filesystem::path& Path, std::initializer_list<std::string_view> Extensions)
	{
		const std::string Extension = Path.extension().string();
		for (const std::string_view Candidate : Extensions)
			if (AssetExtensionsEqual(Extension, Candidate)) return true;
		return false;
	}

	[[nodiscard]] inline bool HasAssetTypeExtension(const std::filesystem::path& Path, AssetFileType Type)
	{
		switch (Type)
		{
		case AssetFileType::Scene:
			return HasAssetExtension(Path, { ".scene" });
		case AssetFileType::Prefab:
			return HasAssetExtension(Path, { ".prefab" });
		case AssetFileType::Mesh:
			return HasAssetExtension(Path, { ".gltf", ".glb" });
		case AssetFileType::Material:
			return HasAssetExtension(Path, { ".material" });
		case AssetFileType::Script:
			return HasAssetExtension(Path, { ".lua" });
		case AssetFileType::Audio:
			return HasAssetExtension(Path, { ".wav", ".mp3", ".flac", ".ogg" });
		case AssetFileType::Environment:
			return HasAssetExtension(Path, { ".hdr" });
		case AssetFileType::Image:
			return HasAssetExtension(
				Path, { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".gif", ".pic", ".ppm", ".pnm", ".pgm", ".pbm", ".hdr" });
		case AssetFileType::PreviewImage:
			return HasAssetExtension(
				Path, { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".gif", ".pic", ".ppm", ".pnm", ".pgm", ".pbm" });
		case AssetFileType::ShaderSource:
			return HasAssetExtension(Path, { ".hlsl", ".glsl" });
		case AssetFileType::Project:
			return HasAssetExtension(Path, { ".pfproj" });
		}
		return false;
	}

	[[nodiscard]] inline bool IsMetadataSidecarPath(const std::filesystem::path& Path)
	{
		return HasAssetExtension(Path, { ".meta" });
	}

	[[nodiscard]] inline bool IsReservedMetadataComponent(const std::filesystem::path& Component)
	{
		return AssetExtensionsEqual(Component.string(), ".meta");
	}
} // namespace PulseForge
