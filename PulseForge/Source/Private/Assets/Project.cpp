#include "Assets/Project.h"
#include "Assets/AssetTypes.h"
#include "Core/BoundedInput.h"
#include "Core/Diagnostics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr std::string_view ProjectFormat = "PulseForgeProject";
		constexpr int64_t ProjectFormatVersion = 1;

		std::optional<std::string> FindUnknownField(const Json& Object, std::initializer_list<std::string_view> AllowedFields)
		{
			for (const auto& [Key, Value] : Object.items())
			{
				(void)Value;
				if (std::find(AllowedFields.begin(), AllowedFields.end(), Key) == AllowedFields.end()) return Key;
			}
			return std::nullopt;
		}

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
			if (Path.empty() || !HasAssetTypeExtension(Path, AssetFileType::Project))
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
			if (!HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Scene))
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
			return std::unexpected(MakeError(ProjectErrorCode::InvalidDescription, {}, "Project startup scene UUID must not be nil"));

		try
		{
			Json Document = Json::object();
			Document["format"] = ProjectFormat;
			Document["version"] = ProjectFormatVersion;
			Document["projectId"] = Description.ID.ToString();
			Document["name"] = Description.Name;
			if (Description.StartScene) Document["startScene"] = Description.StartScene->ToString();
			std::string Data = Document.dump(2) + "\n";
			if (Data.size() > MaximumDocumentBytes)
				return std::unexpected(
					MakeError(ProjectErrorCode::DocumentTooLarge, {}, "Serialized project exceeds the 1 MiB document limit"));
			return Data;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(
				MakeError(ProjectErrorCode::InvalidDescription, {}, std::string("Project serialization failed: ") + Exception.what()));
		}
	}

	std::expected<ProjectDesc, ProjectError> ProjectSerializer::Deserialize(std::string_view Data)
	{
		if (Data.size() > MaximumDocumentBytes)
			return std::unexpected(MakeError(ProjectErrorCode::DocumentTooLarge, {}, "Project document exceeds the 1 MiB document limit"));

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
			if (Version == Document.end() || (!Version->is_number_integer() && !Version->is_number_unsigned()))
				return std::unexpected(
					MakeError(ProjectErrorCode::InvalidDocument, {}, "Project document requires an integer format version"));
			const bool SupportedVersion = Version->is_number_unsigned()
											  ? Version->get<uint64_t>() == static_cast<uint64_t>(ProjectFormatVersion)
											  : Version->get<int64_t>() == ProjectFormatVersion;
			if (!SupportedVersion)
				return std::unexpected(MakeError(ProjectErrorCode::UnsupportedVersion, {}, "Project document version is not supported"));
			if (const auto Unknown = FindUnknownField(Document, { "format", "version", "projectId", "name", "startScene" }))
				return std::unexpected(
					MakeError(ProjectErrorCode::InvalidDocument, {}, "Project document contains unsupported field '" + *Unknown + "'"));

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

	std::expected<Project, ProjectError> Project::Open(
		const std::filesystem::path& ProjectFile,
		DiagnosticRecorder* Diagnostics,
		std::stop_token CancellationToken)
	{
		ScopedDiagnosticTimer TotalTimer(Diagnostics, "Project::Open", "Project");
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(ProjectErrorCode::Cancelled, ProjectFile, "Project opening was cancelled"));
		if (auto Validation = ValidateProjectPath(ProjectFile); !Validation) return std::unexpected(std::move(Validation.error()));

		std::error_code Error;
		const std::filesystem::path AbsoluteProjectFile = std::filesystem::absolute(ProjectFile, Error).lexically_normal();
		if (Error)
			return std::unexpected(
				MakeError(ProjectErrorCode::InvalidPath, ProjectFile, "Could not resolve project file path: " + Error.message()));
		std::expected<std::string, Detail::BoundedInputError> Data = std::unexpected(Detail::BoundedInputError{});
		{
			ScopedDiagnosticTimer ReadTimer(Diagnostics, "Project metadata read", "Project");
			std::ifstream Input(AbsoluteProjectFile, std::ios::binary);
			if (!Input.is_open())
				return std::unexpected(MakeError(ProjectErrorCode::FileReadFailed, AbsoluteProjectFile,
					"Could not open project file: " + PathForMessage(AbsoluteProjectFile)));
			Data = Detail::ReadBoundedInput(Input, ProjectSerializer::MaximumDocumentBytes);
		}
		if (!Data)
			return std::unexpected(MakeError(ProjectErrorCode::FileReadFailed, AbsoluteProjectFile,
				Data.error().Code == Detail::BoundedInputErrorCode::TooLarge ? "Project file exceeds the 1 MiB document limit"
																			 : "Could not read the complete project file"));
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(ProjectErrorCode::Cancelled, AbsoluteProjectFile, "Project opening was cancelled"));

		std::expected<ProjectDesc, ProjectError> Description = std::unexpected(ProjectError{});
		{
			ScopedDiagnosticTimer DeserializeTimer(Diagnostics, "Project metadata deserialize", "Project");
			Description = ProjectSerializer::Deserialize(*Data);
		}
		if (!Description)
		{
			Description.error().Path = AbsoluteProjectFile;
			return std::unexpected(std::move(Description.error()));
		}
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(ProjectErrorCode::Cancelled, AbsoluteProjectFile, "Project opening was cancelled"));

		AssetRegistry Registry;
		std::expected<void, AssetRegistryError> Rebuild;
		{
			ScopedDiagnosticTimer RegistryTimer(Diagnostics, "Project asset registry rebuild", "Project");
			Rebuild = Registry.Rebuild(AbsoluteProjectFile.parent_path(), CancellationToken);
		}
		if (!Rebuild)
		{
			if (CancellationToken.stop_requested())
				return std::unexpected(MakeError(ProjectErrorCode::Cancelled, AbsoluteProjectFile, "Project opening was cancelled during asset registry scanning"));
			const std::string Message = Rebuild.error().Issues.empty()
				? "Could not rebuild project asset registry"
				: Rebuild.error().Issues.front().Message;
			return std::unexpected(MakeError(ProjectErrorCode::RegistryBuildFailed, AbsoluteProjectFile.parent_path(), Message));
		}
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(ProjectErrorCode::Cancelled, AbsoluteProjectFile, "Project opening was cancelled after asset registry scanning"));
		{
			ScopedDiagnosticTimer ValidationTimer(Diagnostics, "Project startup scene validation", "Project");
			if (auto StartSceneValidation = ValidateStartScene(Description->StartScene, Registry, AbsoluteProjectFile);
				!StartSceneValidation)
				return std::unexpected(std::move(StartSceneValidation.error()));
		}

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
