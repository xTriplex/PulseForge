#pragma once

#include "Assets/AssetID.h"
#include "Assets/CookedAssetCatalog.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	enum class ExportTargetPlatform : uint8_t
	{
		Windows,
		Linux,
		MacOS
	};

	enum class ExportTargetArchitecture : uint8_t
	{
		X64,
		Arm64
	};

	enum class ExportRendererBackend : uint8_t
	{
		Vulkan
	};

	struct ExportWindowSettings final
	{
		uint32_t Width = 1280;
		uint32_t Height = 720;
		friend bool operator==(const ExportWindowSettings&, const ExportWindowSettings&) = default;
	};

	// Portable authored export intent. Toolchain, SDK and template paths belong in local machine settings.
	struct ExportBuildProfile final
	{
		UUID ProfileID;
		std::string DisplayName;
		ExportTargetPlatform TargetPlatform = ExportTargetPlatform::Windows;
		ExportTargetArchitecture Architecture = ExportTargetArchitecture::X64;
		CookedBuildConfiguration Configuration = CookedBuildConfiguration::Release;
		ExportRendererBackend Renderer = ExportRendererBackend::Vulkan;
		AssetID StartupScene;
		std::vector<AssetID> DynamicRoots;
		std::filesystem::path DefaultOutputPath = std::filesystem::path("Build") / "Exports" / "Default";
		std::optional<ExportWindowSettings> Window;
		friend bool operator==(const ExportBuildProfile&, const ExportBuildProfile&) = default;
	};

	enum class ExportBuildProfileErrorCode : uint8_t
	{
		InvalidProfile,
		InvalidDocument,
		UnsupportedFormat,
		UnsupportedVersion,
		DocumentTooLarge,
		FileOpenFailed,
		FileReadFailed,
		FileWriteFailed,
		FileReplaceFailed
	};

	struct ExportBuildProfileError final
	{
		ExportBuildProfileErrorCode Code = ExportBuildProfileErrorCode::InvalidProfile;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API ExportBuildProfileSerializer final
	{
	public:
		static constexpr uint32_t CurrentVersion = 1;
		static constexpr size_t MaximumDocumentBytes = 64u * 1024u;
		static constexpr size_t MaximumDynamicRoots = 4096;
		static constexpr uint32_t MaximumWindowDimension = 16384;
		static constexpr std::string_view FormatIdentifier = "PulseForgeBuildProfile";

		[[nodiscard]] static std::expected<ExportBuildProfile, ExportBuildProfileError> Parse(
			std::string_view Data,
			const std::filesystem::path& Path = {});
		[[nodiscard]] static std::expected<std::string, ExportBuildProfileError> Serialize(
			const ExportBuildProfile& Profile,
			const std::filesystem::path& Path = {});
		[[nodiscard]] static std::expected<ExportBuildProfile, ExportBuildProfileError> Load(
			const std::filesystem::path& Path);
		[[nodiscard]] static std::expected<void, ExportBuildProfileError> Save(
			const std::filesystem::path& Path,
			const ExportBuildProfile& Profile);
	};
} // namespace PulseForge
