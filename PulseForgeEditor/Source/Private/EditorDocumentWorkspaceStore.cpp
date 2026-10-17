#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "Editor/EditorDocumentWorkspaceStore.h"
#include "Editor/EditorDocumentWorkspaceRead.h"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace PulseForgeEditor
{
	std::expected<void, std::string> Detail::ReadCompleteWorkspaceBytes(std::istream& Input, std::string& Contents)
	{
		if (Contents.empty()) return {};

		Input.read(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		const std::streamsize BytesRead = Input.gcount();
		if (Input.bad()) return std::unexpected("I/O error while reading document workspace bytes");
		if (BytesRead != static_cast<std::streamsize>(Contents.size()))
		{
			return std::unexpected("Document workspace changed during read; expected " + std::to_string(Contents.size()) +
								   " bytes but received " + std::to_string(BytesRead));
		}
		if (Input.fail()) return std::unexpected("Document workspace read stopped before all expected bytes were read");
		return {};
	}

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
		m_RecoveryBlocked = false;
	}

	std::expected<EditorDocumentWorkspaceLoadResult, std::string> EditorDocumentWorkspaceStore::Load(const PulseForge::UUID& Project)
	{
		m_LastSerializedContents.clear();
		m_ImmediateDirty = false;
		m_BoundsDirty = false;
		m_RecoveryBlocked = false;
		if (m_Path.empty()) return std::unexpected("The document workspace path is unavailable");

		auto File = ReadFile(m_Path);
		if (!File)
		{
			// Do not replace data that could not be read completely. A later edit may
			// retry the load; until then, saving must leave the source untouched.
			m_RecoveryBlocked = true;
			return std::unexpected(std::move(File.error()));
		}
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
			std::string RecoveryError;
			bool OriginalPreserved = false;
			auto ExistingRecovery = ReadFile(RecoveryPath);
			if (!ExistingRecovery) RecoveryError = "Could not inspect recovery copy: " + ExistingRecovery.error();
			else if (*ExistingRecovery)
			{
				OriginalPreserved = **ExistingRecovery == Contents;
				if (!OriginalPreserved) RecoveryError = "Recovery destination already contains different bytes: " + RecoveryPath.string();
			}
			else
			{
				std::error_code CopyError;
				const bool Copied = std::filesystem::copy_file(m_Path, RecoveryPath, std::filesystem::copy_options::none, CopyError);
				if (CopyError) RecoveryError = "Could not copy the rejected workspace to recovery storage: " + CopyError.message();
				else
				{
					auto VerifiedRecovery = ReadFile(RecoveryPath);
					if (!VerifiedRecovery) RecoveryError = "Could not verify recovery copy: " + VerifiedRecovery.error();
					else if (!*VerifiedRecovery || **VerifiedRecovery != Contents)
						RecoveryError = "Recovery copy does not match the rejected workspace bytes";
					else OriginalPreserved = true;
				}
				if (!Copied && !OriginalPreserved && RecoveryError.empty())
					RecoveryError = "Recovery destination was created concurrently and could not be verified";
			}
			m_RecoveryBlocked = !OriginalPreserved;
			m_ImmediateDirty = OriginalPreserved;
			return EditorDocumentWorkspaceLoadResult{ .Result = EditorDocumentWorkspaceLoadResult::Status::Rejected,
				.Workspace = std::nullopt,
				.Error = Workspace.error(),
				.RecoveryPath = RecoveryPath,
				.RecoveryError = std::move(RecoveryError) };
		}

		m_LastSerializedContents = Contents;
		return EditorDocumentWorkspaceLoadResult{ .Result = EditorDocumentWorkspaceLoadResult::Status::Restored,
			.Workspace = std::move(*Workspace),
			.Error = {},
			.RecoveryPath = std::nullopt,
			.RecoveryError = {} };
	}

	void EditorDocumentWorkspaceStore::MarkDirty(bool BoundsOnly, Clock::time_point Now) noexcept
	{
		if (m_Path.empty()) return;
		if (BoundsOnly)
		{
			m_BoundsDirty = true;
			m_SaveAt = Now + std::chrono::milliseconds(500);
		}
		else m_ImmediateDirty = true;
	}

	bool EditorDocumentWorkspaceStore::HasPendingChanges() const noexcept
	{
		return m_ImmediateDirty || m_BoundsDirty;
	}

	bool EditorDocumentWorkspaceStore::IsSaveDue(Clock::time_point Now) const noexcept
	{
		return !m_RecoveryBlocked && (m_ImmediateDirty || (m_BoundsDirty && Now >= m_SaveAt));
	}

	std::expected<bool, std::string> EditorDocumentWorkspaceStore::SaveIfDue(
		const EditorDocumentWorkspaceState& Workspace, Clock::time_point Now)
	{
		if (m_Path.empty()) return std::unexpected("The document workspace path is unavailable");
		if (m_RecoveryBlocked && HasPendingChanges())
			return std::unexpected("The rejected document workspace was not preserved; the original file will not be replaced");
		if (!HasPendingChanges()) return false;
		if (!m_ImmediateDirty && Now < m_SaveAt) return false;

		auto Serialized = EditorDocumentWorkspaceCodec::Serialize(Workspace);
		if (!Serialized) return std::unexpected(std::move(Serialized.error()));
		if (*Serialized != m_LastSerializedContents)
		{
			if (auto Written = WriteFileAtomically(m_Path, *Serialized); !Written) return std::unexpected(std::move(Written.error()));
			m_LastSerializedContents = std::move(*Serialized);
		}
		m_ImmediateDirty = false;
		m_BoundsDirty = false;
		return true;
	}

	std::expected<bool, std::string> EditorDocumentWorkspaceStore::SaveNow(const EditorDocumentWorkspaceState& Workspace)
	{
		MarkDirty(false);
		return SaveIfDue(Workspace);
	}

	std::expected<std::optional<std::string>, std::string> EditorDocumentWorkspaceStore::ReadFile(const std::filesystem::path& Path)
	{
		std::error_code Error;
		if (!std::filesystem::exists(Path, Error))
		{
			if (Error) return std::unexpected("Could not inspect document workspace: " + Error.message());
			return std::optional<std::string>{};
		}
		const uintmax_t Size = std::filesystem::file_size(Path, Error);
		if (Error || Size > 2u * 1024u * 1024u)
			return std::unexpected(
				Error ? "Could not inspect document workspace: " + Error.message() : "Document workspace exceeds the 2 MiB safety limit");
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return std::unexpected("Could not open the document workspace for reading");
		std::string Contents(static_cast<size_t>(Size), '\0');
		if (auto Read = Detail::ReadCompleteWorkspaceBytes(Input, Contents); !Read) return std::unexpected(std::move(Read.error()));
		return std::optional<std::string>(std::move(Contents));
	}

	std::expected<void, std::string> EditorDocumentWorkspaceStore::WriteFileAtomically(
		const std::filesystem::path& Path, std::string_view Contents)
	{
		if (Path.empty()) return std::unexpected("The document workspace path is unavailable");
		std::filesystem::path TemporaryPath = Path;
		TemporaryPath += L".tmp";
		{
			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output) return std::unexpected("Could not create a temporary document workspace file");
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
			return std::unexpected("Could not atomically replace document workspace (Windows error " + std::to_string(ErrorCode) + ")");
		}
		return {};
	}
} // namespace PulseForgeEditor
