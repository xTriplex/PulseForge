#include "Core/PulseForgePCH.h"
#include "Assets/Project.h"
#include "Core/ScopedProfileTimer.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr std::string_view ProjectFormat = "PulseForgeProject";
		constexpr int64_t ProjectFormatVersion = 1;
		constexpr size_t MaximumProjectFileSize = 1024 * 1024;

		ProjectError MakeError(ProjectErrorCode Code, const std::filesystem::path& Path, std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		std::string PathForMessage(const std::filesystem::path& Path)
		{
			const std::u8string UTF8Path = Path.u8string();
			return { reinterpret_cast<const char*>(UTF8Path.data()), UTF8Path.size() };
		}

		std::expected<void, ProjectError> ValidateProjectPath(const std::filesystem::path& Path)
		{
			if (Path.empty() || Path.extension() != ".pfproj")
				return std::unexpected(MakeError(
					ProjectErrorCode::InvalidPath,
					Path,
					"Project file path must name a .pfproj document"));
			return {};
		}

		std::expected<void, ProjectError> ValidateStartScene(
			const std::optional<AssetID>& StartScene,
			const AssetRegistry& Registry,
			const std::filesystem::path& ProjectFile)
		{
			if (!StartScene)
				return {};
			const auto Record = Registry.Find(*StartScene);
			if (!Record)
				return std::unexpected(MakeError(
					ProjectErrorCode::StartSceneNotFound,
					ProjectFile,
					"Project startup scene UUID " + StartScene->ToString() + " is not present in the asset registry"));
			if (Record->ProjectRelativePath.extension() != ".scene")
				return std::unexpected(MakeError(
					ProjectErrorCode::StartSceneWrongType,
					Record->ProjectRelativePath,
					"Project startup scene UUID does not refer to a .scene asset"));
			return {};
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

	std::expected<std::string, ProjectError> ProjectSerializer::Serialize(const ProjectDesc& Description)
	{
		if (Description.ID.IsNil() || Description.Name.empty() || Description.Name.size() > 256)
			return std::unexpected(MakeError(
				ProjectErrorCode::InvalidDescription,
				{},
				"Project requires a non-nil UUID and a name between 1 and 256 bytes"));
		if (Description.StartScene && Description.StartScene->IsNil())
			return std::unexpected(MakeError(
				ProjectErrorCode::InvalidDescription,
				{},
				"Project startup scene UUID must not be nil"));

		try
		{
			Json Document = Json::object();
			Document["format"] = ProjectFormat;
			Document["version"] = ProjectFormatVersion;
			Document["projectId"] = Description.ID.ToString();
			Document["name"] = Description.Name;
			if (Description.StartScene)
				Document["startScene"] = Description.StartScene->ToString();
			return Document.dump(2) + "\n";
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				ProjectErrorCode::InvalidDescription,
				{},
				std::string("Project serialization failed: ") + Exception.what()));
		}
	}

	std::expected<ProjectDesc, ProjectError> ProjectSerializer::Deserialize(std::string_view Data)
	{
		try
		{
			const Json Document = Json::parse(Data.begin(), Data.end());
			if (!Document.is_object())
				return std::unexpected(MakeError(ProjectErrorCode::InvalidDocument, {}, "Project document root must be an object"));

			const auto Format = Document.find("format");
			if (Format == Document.end() || !Format->is_string())
				return std::unexpected(MakeError(ProjectErrorCode::InvalidDocument, {}, "Project document is missing its format identifier"));
			if (Format->get<std::string>() != ProjectFormat)
				return std::unexpected(MakeError(ProjectErrorCode::UnsupportedFormat, {}, "Document is not a PulseForge project"));

			const auto Version = Document.find("version");
			if (Version == Document.end() || !Version->is_number_integer() || Version->get<int64_t>() != ProjectFormatVersion)
				return std::unexpected(MakeError(ProjectErrorCode::UnsupportedVersion, {}, "Project document version is not supported"));

			const auto ProjectIDValue = Document.find("projectId");
			const auto NameValue = Document.find("name");
			if (ProjectIDValue == Document.end() || !ProjectIDValue->is_string() ||
				NameValue == Document.end() || !NameValue->is_string())
			{
				return std::unexpected(MakeError(
					ProjectErrorCode::InvalidDocument,
					{},
					"Project document requires string projectId and name fields"));
			}

			const auto ProjectID = UUID::Parse(ProjectIDValue->get<std::string>());
			if (!ProjectID || ProjectID->IsNil())
				return std::unexpected(MakeError(
					ProjectErrorCode::InvalidDocument,
					{},
					ProjectID ? "Project UUID must not be nil" : ProjectID.error().Message));

			ProjectDesc Description;
			Description.ID = *ProjectID;
			Description.Name = NameValue->get<std::string>();
			if (const auto StartSceneValue = Document.find("startScene"); StartSceneValue != Document.end())
			{
				if (!StartSceneValue->is_string())
					return std::unexpected(MakeError(
						ProjectErrorCode::InvalidDocument,
						{},
						"Project startScene must be a UUID string when present"));
				const auto StartScene = UUID::Parse(StartSceneValue->get<std::string>());
				if (!StartScene || StartScene->IsNil())
					return std::unexpected(MakeError(
						ProjectErrorCode::InvalidDocument,
						{},
						StartScene ? "Project startup scene UUID must not be nil" : StartScene.error().Message));
				Description.StartScene = *StartScene;
			}

			if (auto Validation = Serialize(Description); !Validation)
				return std::unexpected(MakeError(ProjectErrorCode::InvalidDocument, {}, Validation.error().Message));
			return Description;
		}
		catch (const nlohmann::json::exception& Exception)
		{
			return std::unexpected(MakeError(
				ProjectErrorCode::InvalidDocument,
				{},
				std::string("Project JSON is invalid: ") + Exception.what()));
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				ProjectErrorCode::InvalidDocument,
				{},
				std::string("Project deserialization failed: ") + Exception.what()));
		}
	}

	Project::Project(std::filesystem::path ProjectFile, ProjectDesc Description, AssetRegistry Registry)
		: m_ProjectFile(std::move(ProjectFile)),
		  m_ProjectRoot(m_ProjectFile.parent_path()),
		  m_Description(std::move(Description)),
		  m_Registry(std::move(Registry))
	{
	}

	std::expected<Project, ProjectError> Project::Create(
		const std::filesystem::path& ProjectFile,
		std::string Name)
	{
		if (auto Validation = ValidateProjectPath(ProjectFile); !Validation)
			return std::unexpected(std::move(Validation.error()));
		if (Name.empty() || Name.size() > 256)
			return std::unexpected(MakeError(ProjectErrorCode::InvalidDescription, ProjectFile, "Project name must be between 1 and 256 bytes"));

		std::error_code Error;
		const std::filesystem::path AbsoluteProjectFile = std::filesystem::absolute(ProjectFile, Error).lexically_normal();
		if (Error)
			return std::unexpected(MakeError(ProjectErrorCode::InvalidPath, ProjectFile, "Could not resolve project file path: " + Error.message()));
		if (std::filesystem::exists(AbsoluteProjectFile, Error) || Error)
			return std::unexpected(MakeError(ProjectErrorCode::InvalidPath, AbsoluteProjectFile, "Project file already exists or cannot be inspected"));

		const std::filesystem::path ProjectRoot = AbsoluteProjectFile.parent_path();
		std::filesystem::create_directories(ProjectRoot / "Assets", Error);
		if (Error)
			return std::unexpected(MakeError(ProjectErrorCode::InvalidPath, ProjectRoot, "Could not create the project Assets directory: " + Error.message()));

		const auto Identifier = UUID::Generate();
		if (!Identifier)
			return std::unexpected(MakeError(ProjectErrorCode::InvalidDescription, AbsoluteProjectFile, Identifier.error().Message));

		AssetRegistry Registry;
		if (auto Rebuild = Registry.Rebuild(ProjectRoot); !Rebuild)
			return std::unexpected(MakeError(ProjectErrorCode::RegistryBuildFailed, ProjectRoot, "Could not initialize project asset registry"));

		Project Result(AbsoluteProjectFile, ProjectDesc{ *Identifier, std::move(Name), {} }, std::move(Registry));
		if (auto Saved = Result.Save(); !Saved)
			return std::unexpected(std::move(Saved.error()));
		return Result;
	}

	std::expected<Project, ProjectError> Project::Open(const std::filesystem::path& ProjectFile)
	{
		Detail::ScopedProfileTimer Timer("Project::Open total");
		if (auto Validation = ValidateProjectPath(ProjectFile); !Validation)
			return std::unexpected(std::move(Validation.error()));

		std::error_code Error;
		const std::filesystem::path AbsoluteProjectFile = std::filesystem::absolute(ProjectFile, Error).lexically_normal();
		if (Error)
			return std::unexpected(MakeError(ProjectErrorCode::InvalidPath, ProjectFile, "Could not resolve project file path: " + Error.message()));
		const uintmax_t FileSize = std::filesystem::file_size(AbsoluteProjectFile, Error);
		if (Error || FileSize > MaximumProjectFileSize)
			return std::unexpected(MakeError(ProjectErrorCode::FileReadFailed, AbsoluteProjectFile, Error ? Error.message() : "Project file exceeds the 1 MiB size limit"));

		std::ifstream Input(AbsoluteProjectFile, std::ios::binary);
		if (!Input.is_open())
			return std::unexpected(MakeError(ProjectErrorCode::FileReadFailed, AbsoluteProjectFile, "Could not open project file: " + PathForMessage(AbsoluteProjectFile)));
		const std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
		if (Input.bad())
			return std::unexpected(MakeError(ProjectErrorCode::FileReadFailed, AbsoluteProjectFile, "Could not read the complete project file"));

		auto Description = ProjectSerializer::Deserialize(Data);
		if (!Description)
		{
			Description.error().Path = AbsoluteProjectFile;
			return std::unexpected(std::move(Description.error()));
		}

		AssetRegistry Registry;
		if (auto Rebuild = Registry.Rebuild(AbsoluteProjectFile.parent_path()); !Rebuild)
		{
			const std::string Message = Rebuild.error().Issues.empty()
				? "Could not rebuild project asset registry"
				: Rebuild.error().Issues.front().Message;
			return std::unexpected(MakeError(ProjectErrorCode::RegistryBuildFailed, AbsoluteProjectFile.parent_path(), Message));
		}
		if (auto StartSceneValidation = ValidateStartScene(Description->StartScene, Registry, AbsoluteProjectFile);
			!StartSceneValidation)
			return std::unexpected(std::move(StartSceneValidation.error()));

		return Project(AbsoluteProjectFile, std::move(*Description), std::move(Registry));
	}

	std::expected<void, ProjectError> Project::SetStartScene(std::optional<AssetID> Scene)
	{
		if (Scene && Scene->IsNil())
			return std::unexpected(MakeError(ProjectErrorCode::InvalidDescription, m_ProjectFile, "Project startup scene UUID must not be nil"));
		if (auto Validation = ValidateStartScene(Scene, m_Registry, m_ProjectFile); !Validation)
			return Validation;

		const std::optional<AssetID> Previous = m_Description.StartScene;
		m_Description.StartScene = Scene;
		if (auto Saved = Save(); !Saved)
		{
			m_Description.StartScene = Previous;
			return Saved;
		}
		return {};
	}

	std::expected<void, ProjectError> Project::Save() const
	{
		const auto Serialized = ProjectSerializer::Serialize(m_Description);
		if (!Serialized)
			return std::unexpected(MakeError(Serialized.error().Code, m_ProjectFile, Serialized.error().Message));

		const auto Identifier = UUID::Generate();
		if (!Identifier)
			return std::unexpected(MakeError(ProjectErrorCode::FileWriteFailed, m_ProjectFile, Identifier.error().Message));
		std::filesystem::path TemporaryPath = m_ProjectFile;
		TemporaryPath += "." + Identifier->ToString() + ".tmp";
		TemporaryFileCleanup Cleanup{ TemporaryPath };

		std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
		if (!Output.is_open())
			return std::unexpected(MakeError(ProjectErrorCode::FileWriteFailed, TemporaryPath, "Could not open temporary project file for writing"));
		Output.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
		Output.flush();
		if (!Output)
			return std::unexpected(MakeError(ProjectErrorCode::FileWriteFailed, TemporaryPath, "Could not write complete project document"));
		Output.close();
		if (Output.fail())
			return std::unexpected(MakeError(ProjectErrorCode::FileWriteFailed, TemporaryPath, "Could not finish writing project document"));

		std::error_code Error;
		std::filesystem::rename(TemporaryPath, m_ProjectFile, Error);
		if (Error)
			return std::unexpected(MakeError(
				ProjectErrorCode::FileReplaceFailed,
				m_ProjectFile,
				"Could not replace project document: " + Error.message()));
		return {};
	}
}
