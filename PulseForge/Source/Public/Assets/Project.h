#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	struct ProjectDesc
	{
		UUID ID;
		std::string Name;
		std::optional<AssetID> StartScene;
	};

	enum class ProjectErrorCode : uint8_t
	{
		InvalidPath,
		InvalidDescription,
		InvalidDocument,
		UnsupportedFormat,
		UnsupportedVersion,
		FileReadFailed,
		FileWriteFailed,
		FileReplaceFailed,
		RegistryBuildFailed,
		StartSceneNotFound,
		StartSceneWrongType
	};

	struct ProjectError
	{
		ProjectErrorCode Code;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API ProjectSerializer final
	{
	public:
		[[nodiscard]] static std::expected<std::string, ProjectError> Serialize(const ProjectDesc& Description);
		[[nodiscard]] static std::expected<ProjectDesc, ProjectError> Deserialize(std::string_view Data);
	};

	// Project paths are the location of the .pfproj document; all asset identity remains sidecar-UUID based.
	class PULSEFORGE_API Project final
	{
	public:
		[[nodiscard]] static std::expected<Project, ProjectError> Create(
			const std::filesystem::path& ProjectFile,
			std::string Name);
		[[nodiscard]] static std::expected<Project, ProjectError> Open(const std::filesystem::path& ProjectFile);

		Project(const Project&) = delete;
		Project& operator=(const Project&) = delete;
		Project(Project&&) noexcept = default;
		Project& operator=(Project&&) noexcept = default;

		[[nodiscard]] const std::filesystem::path& GetFilePath() const noexcept { return m_ProjectFile; }
		[[nodiscard]] const std::filesystem::path& GetRootPath() const noexcept { return m_ProjectRoot; }
		[[nodiscard]] const ProjectDesc& GetDescription() const noexcept { return m_Description; }
		[[nodiscard]] const AssetRegistry& GetAssetRegistry() const noexcept { return m_Registry; }
		[[nodiscard]] AssetRegistry& GetAssetRegistry() noexcept { return m_Registry; }

		[[nodiscard]] std::expected<void, ProjectError> SetStartScene(std::optional<AssetID> Scene);
		[[nodiscard]] std::expected<void, ProjectError> Save() const;

	private:
		Project(std::filesystem::path ProjectFile, ProjectDesc Description, AssetRegistry Registry);

		std::filesystem::path m_ProjectFile;
		std::filesystem::path m_ProjectRoot;
		ProjectDesc m_Description;
		AssetRegistry m_Registry;
	};
}
