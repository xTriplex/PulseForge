#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Editor/EditorDocumentWorkspaceStore.h"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace PulseForgeEditor
{
	void EditorDocumentWorkspaceStore::Configure(std::filesystem::path Path)
	{
		Clear();
		m_Path = std::move(Path);
	}

	void EditorDocumentWorkspaceStore::Clear() noexcept
	{
		m_Path.clear();
		m_LastSerializedContents.clear();
		m_SaveAt = {};
		m_ImmediateDirty = false;
		m_BoundsDirty = false;
	}

	std::expected<EditorDocumentWorkspaceLoadResult, std::string> EditorDocumentWorkspaceStore::Load(
		const PulseForge::UUID& Project)
	{
		m_LastSerializedContents.clear();
		m_ImmediateDirty = false;
		m_BoundsDirty = false;
		if (m_Path.empty())
			return std::unexpected("The document workspace path is unavailable");

		auto File = ReadFile(m_Path);
		if (!File)
			return std::unexpected(std::move(File.error()));
		if (!*File)
		{
			m_ImmediateDirty = true;
			return EditorDocumentWorkspaceLoadResult{};
		}

		const std::string& Contents = **File;
		auto Workspace = EditorDocumentWorkspaceCodec::Deserialize(Contents, Project);
		if (!Workspace)
		{
			std::filesystem::path RecoveryPath = m_Path;
			RecoveryPath += L".recovery";
			std::error_code CopyError;
			if (!std::filesystem::exists(RecoveryPath, CopyError) && !CopyError)
				(void)std::filesystem::copy_file(m_Path, RecoveryPath, CopyError);
			m_ImmediateDirty = true;
			return EditorDocumentWorkspaceLoadResult{
				.Result = EditorDocumentWorkspaceLoadResult::Status::Rejected,
				.Workspace = std::nullopt,
				.Error = Workspace.error()
			};
		}

		m_LastSerializedContents = Contents;
		return EditorDocumentWorkspaceLoadResult{
			.Result = EditorDocumentWorkspaceLoadResult::Status::Restored,
			.Workspace = std::move(*Workspace),
			.Error = {}
		};
	}

	void EditorDocumentWorkspaceStore::MarkDirty(bool BoundsOnly, Clock::time_point Now) noexcept
	{
		if (m_Path.empty())
			return;
		if (BoundsOnly)
		{
			m_BoundsDirty = true;
			m_SaveAt = Now + std::chrono::milliseconds(500);
		}
		else
			m_ImmediateDirty = true;
	}

	bool EditorDocumentWorkspaceStore::HasPendingChanges() const noexcept
	{
		return m_ImmediateDirty || m_BoundsDirty;
	}

	bool EditorDocumentWorkspaceStore::IsSaveDue(Clock::time_point Now) const noexcept
	{
		return m_ImmediateDirty || (m_BoundsDirty && Now >= m_SaveAt);
	}

	std::expected<bool, std::string> EditorDocumentWorkspaceStore::SaveIfDue(
		const EditorDocumentWorkspaceState& Workspace,
		Clock::time_point Now)
	{
		if (m_Path.empty())
			return std::unexpected("The document workspace path is unavailable");
		if (!HasPendingChanges())
			return false;
		if (!m_ImmediateDirty && Now < m_SaveAt)
			return false;

		auto Serialized = EditorDocumentWorkspaceCodec::Serialize(Workspace);
		if (!Serialized)
			return std::unexpected(std::move(Serialized.error()));
		if (*Serialized != m_LastSerializedContents)
		{
			if (auto Written = WriteFileAtomically(m_Path, *Serialized); !Written)
				return std::unexpected(std::move(Written.error()));
			m_LastSerializedContents = std::move(*Serialized);
		}
		m_ImmediateDirty = false;
		m_BoundsDirty = false;
		return true;
	}

	std::expected<bool, std::string> EditorDocumentWorkspaceStore::SaveNow(
		const EditorDocumentWorkspaceState& Workspace)
	{
		MarkDirty(false);
		return SaveIfDue(Workspace);
	}

	std::expected<std::optional<std::string>, std::string> EditorDocumentWorkspaceStore::ReadFile(
		const std::filesystem::path& Path)
	{
		std::error_code Error;
		if (!std::filesystem::exists(Path, Error))
		{
			if (Error)
				return std::unexpected("Could not inspect document workspace: " + Error.message());
			return std::optional<std::string>{};
		}
		const uintmax_t Size = std::filesystem::file_size(Path, Error);
		if (Error || Size > 2u * 1024u * 1024u)
			return std::unexpected(Error ? "Could not inspect document workspace: " + Error.message()
				: "Document workspace exceeds the 2 MiB safety limit");
		std::ifstream Input(Path, std::ios::binary);
		if (!Input)
			return std::unexpected("Could not open the document workspace for reading");
		std::string Contents(static_cast<size_t>(Size), '\0');
		if (!Contents.empty())
			Input.read(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		if (!Input && !Input.eof())
			return std::unexpected("Could not read the complete document workspace");
		return std::optional<std::string>(std::move(Contents));
	}

	std::expected<void, std::string> EditorDocumentWorkspaceStore::WriteFileAtomically(
		const std::filesystem::path& Path,
		std::string_view Contents)
	{
		if (Path.empty())
			return std::unexpected("The document workspace path is unavailable");
		std::filesystem::path TemporaryPath = Path;
		TemporaryPath += L".tmp";
		{
			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output)
				return std::unexpected("Could not create a temporary document workspace file");
			Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			Output.flush();
			if (!Output)
			{
				Output.close();
				std::error_code RemoveError;
				std::filesystem::remove(TemporaryPath, RemoveError);
				return std::unexpected("Could not write the document workspace");
			}
		}
		if (!MoveFileExW(TemporaryPath.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD ErrorCode = GetLastError();
			std::error_code RemoveError;
			std::filesystem::remove(TemporaryPath, RemoveError);
			return std::unexpected("Could not atomically replace document workspace (Windows error " +
				std::to_string(ErrorCode) + ")");
		}
		return {};
	}
}
