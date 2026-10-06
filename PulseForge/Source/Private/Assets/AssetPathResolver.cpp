#include "Core/PulseForgePCH.h"
#include "Assets/AssetPathResolver.h"

#include <utility>

namespace PulseForge
{
	namespace
	{
		AssetPathError MakeError(AssetPathErrorCode Code, const std::filesystem::path& Path, std::string Message)
		{
			return { Code, Path, std::move(Message) };
		}

		bool HasWindowsAmbiguousTrailingCharacter(const std::filesystem::path& Component)
		{
			const std::u8string Text = Component.generic_u8string();
			return !Text.empty() && (Text.back() == u8'.' || Text.back() == u8' ');
		}
	}

	std::expected<std::filesystem::path, AssetPathError> AssetPathResolver::ResolveManagedSourcePath(
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& ProjectRelativePath)
	{
		if (ProjectRoot.empty())
		{
			return std::unexpected(MakeError(
				AssetPathErrorCode::InvalidProjectRoot,
				ProjectRoot,
				"Project root path must not be empty"));
		}

		std::error_code FileError;
		const std::filesystem::path AbsoluteProjectRoot =
			std::filesystem::absolute(ProjectRoot, FileError).lexically_normal();
		if (FileError || !std::filesystem::is_directory(AbsoluteProjectRoot, FileError) || FileError)
		{
			return std::unexpected(MakeError(
				AssetPathErrorCode::InvalidProjectRoot,
				ProjectRoot,
				"Project root must be an accessible directory"));
		}

		if (ProjectRelativePath.empty() || ProjectRelativePath.is_absolute() ||
			ProjectRelativePath.has_root_name() || ProjectRelativePath.has_root_directory())
		{
			return std::unexpected(MakeError(
				AssetPathErrorCode::InvalidRelativePath,
				ProjectRelativePath,
				"Managed asset path must be project-relative"));
		}

		const auto FirstComponent = ProjectRelativePath.begin();
		if (FirstComponent == ProjectRelativePath.end() || *FirstComponent != "Assets")
		{
			return std::unexpected(MakeError(
				AssetPathErrorCode::InvalidRelativePath,
				ProjectRelativePath,
				"Managed asset path must be under Assets"));
		}
		for (const auto& Component : ProjectRelativePath)
		{
			if (Component == "." || Component == ".." || HasWindowsAmbiguousTrailingCharacter(Component))
			{
				return std::unexpected(MakeError(
					AssetPathErrorCode::InvalidRelativePath,
					ProjectRelativePath,
					"Managed asset path contains traversal or a platform-ambiguous component"));
			}
		}

		const std::filesystem::path AssetRoot = AbsoluteProjectRoot / "Assets";
		FileError.clear();
		const std::filesystem::file_status RootStatus = std::filesystem::symlink_status(AssetRoot, FileError);
		if (FileError || std::filesystem::is_symlink(RootStatus) || !std::filesystem::is_directory(RootStatus))
		{
			return std::unexpected(MakeError(
				AssetPathErrorCode::InvalidAssetsDirectory,
				AssetRoot,
				"Project Assets root must be an accessible, non-symlink directory"));
		}

		std::filesystem::path Current = AssetRoot;
		auto Component = ProjectRelativePath.begin();
		++Component;
		if (Component == ProjectRelativePath.end())
		{
			return std::unexpected(MakeError(
				AssetPathErrorCode::InvalidRelativePath,
				ProjectRelativePath,
				"Managed asset path must name a source file below Assets"));
		}

		for (; Component != ProjectRelativePath.end(); ++Component)
		{
			Current /= *Component;
			FileError.clear();
			const std::filesystem::file_status Status = std::filesystem::symlink_status(Current, FileError);
			const bool IsFinal = std::next(Component) == ProjectRelativePath.end();
			if (FileError || std::filesystem::is_symlink(Status) ||
				(!IsFinal && !std::filesystem::is_directory(Status)) ||
				(IsFinal && !std::filesystem::is_regular_file(Status)))
			{
				return std::unexpected(MakeError(
					AssetPathErrorCode::InvalidAssetEntry,
					Current,
					"Managed asset path must resolve through existing, non-symlink directories to a regular file"));
			}
		}
		return Current;
	}
}
