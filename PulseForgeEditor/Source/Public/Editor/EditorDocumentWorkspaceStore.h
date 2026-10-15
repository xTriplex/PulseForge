#pragma once

#include "Editor/EditorDocumentWorkspace.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	struct EditorDocumentWorkspaceLoadResult final
	{
		enum class Status : uint8_t
		{
			Missing,
			Restored,
			Rejected
		};

		Status Result = Status::Missing;
		std::optional<EditorDocumentWorkspaceState> Workspace;
		std::string Error;
		std::optional<std::filesystem::path> RecoveryPath;
		std::string RecoveryError;
	};

	class EditorDocumentWorkspaceStore final
	{
	public:
		using Clock = std::chrono::steady_clock;

		void Configure(std::filesystem::path Path);
		void Clear() noexcept;
		[[nodiscard]] bool HasPath() const noexcept { return !m_Path.empty(); }
		[[nodiscard]] const std::filesystem::path& GetPath() const noexcept { return m_Path; }

		[[nodiscard]] std::expected<EditorDocumentWorkspaceLoadResult, std::string> Load(const PulseForge::UUID& Project);
		void MarkDirty(bool BoundsOnly, Clock::time_point Now = Clock::now()) noexcept;
		[[nodiscard]] bool HasPendingChanges() const noexcept;
		[[nodiscard]] bool IsSaveDue(Clock::time_point Now = Clock::now()) const noexcept;
		[[nodiscard]] std::expected<bool, std::string> SaveIfDue(
			const EditorDocumentWorkspaceState& Workspace, Clock::time_point Now = Clock::now());
		[[nodiscard]] std::expected<bool, std::string> SaveNow(const EditorDocumentWorkspaceState& Workspace);

	private:
		[[nodiscard]] static std::expected<std::optional<std::string>, std::string> ReadFile(const std::filesystem::path& Path);
		[[nodiscard]] static std::expected<void, std::string> WriteFileAtomically(
			const std::filesystem::path& Path, std::string_view Contents);

		std::filesystem::path m_Path;
		std::string m_LastSerializedContents;
		Clock::time_point m_SaveAt{};
		bool m_ImmediateDirty = false;
		bool m_BoundsDirty = false;
		bool m_RecoveryBlocked = false;
	};
} // namespace PulseForgeEditor
