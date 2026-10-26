#pragma once

#include "Assets/AssetID.h"
#include "Assets/AssetImportProvenance.h"
#include "Core/Core.h"
#include "Scene/UUID.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge::Assets::Detail
{
	enum class AssetEditRecoveryOperationKind : uint8_t
	{
		AssetMove,
		AssetBatchMove,
		FolderMove,
		AssetDuplicate,
		FolderDuplicate
	};

	enum class AssetEditRecoveryState : uint8_t
	{
		Prepared,
		Applying,
		Committed,
		RollingBack
	};

	enum class AssetEditRecoveryActionKind : uint8_t
	{
		Move,
		Rewrite,
		MoveAndRewrite,
		Create,
		DirectoryMove,
		DirectoryCreate
	};

	enum class AssetEditRecoveryEntryType : uint8_t
	{
		File,
		Directory
	};

	struct AssetEditRecoveryEntryState
	{
		AssetEditRecoveryEntryType Type = AssetEditRecoveryEntryType::File;
		bool Exists = false;
		uint64_t ByteLength = 0;
		AssetContentFingerprint Content;
		std::optional<AssetID> Identity;
	};

	struct AssetEditRecoveryAction
	{
		AssetEditRecoveryActionKind Kind = AssetEditRecoveryActionKind::Move;
		// Logical source and destination paths are canonical project-relative paths. Create actions may omit their source.
		std::filesystem::path OriginalPath;
		std::filesystem::path CurrentPath;
		AssetEditRecoveryEntryState Before;
		AssetEditRecoveryEntryState After;
		// Transit paths are project-relative and are used for temporary case-only renames.
		std::optional<std::filesystem::path> TransitPath;
		// Staged output path is relative to the owning transaction directory.
		std::optional<std::filesystem::path> StagedPath;
		// Backup paths are relative to the owning .pf-asset-edit-{OperationID} transaction directory.
		std::optional<std::filesystem::path> BackupPath;
		std::optional<AssetEditRecoveryEntryState> BackupState;
	};

	struct AssetEditRecoveryJournalRecord
	{
		uint32_t Version = 1;
		UUID OperationID;
		UUID ProjectID;
		// Canonical normalized absolute project-root path encoded as generic UTF-8.
		std::string ProjectRootUtf8;
		AssetEditRecoveryOperationKind OperationKind = AssetEditRecoveryOperationKind::AssetMove;
		AssetEditRecoveryState State = AssetEditRecoveryState::Prepared;
		// Advisory progress only. Recovery must reconcile filesystem state instead of trusting these indices.
		std::vector<uint32_t> CompletedActions;
		// Ordered in transaction execution order; a moved-and-rewritten file is represented by one action.
		std::vector<AssetEditRecoveryAction> Actions;
	};

	enum class AssetEditRecoveryJournalErrorCode : uint8_t
	{
		InvalidJournal,
		UnsupportedVersion,
		LinkedPath,
		ReadFailed,
		WriteFailed
	};

	struct AssetEditRecoveryJournalError
	{
		AssetEditRecoveryJournalErrorCode Code = AssetEditRecoveryJournalErrorCode::InvalidJournal;
		std::filesystem::path Path;
		std::string Message;
	};

	class PULSEFORGE_API AssetEditRecoveryJournal final
	{
	public:
		static constexpr uint32_t CurrentVersion = 1;
		static constexpr size_t MaximumDocumentBytes = 64 * 1024 * 1024;
		static constexpr size_t MaximumActions = 100000;
		static constexpr size_t MaximumPathUtf8Bytes = 32768;
		// JSON v1 root keys are format, version, operationId, projectId, projectRootUtf8, operationKind, state,
		// completedActions, and actions. Each action has kind, originalPath, currentPath, transitPath, stagedPath, backupPath,
		// backupState, before, and after. File states contain type, exists, byteLength, fingerprint, and assetId; directory states contain
		// only type and exists. Empty originalPath is valid only for source-less Create and DirectoryCreate actions.

		// Checks schema values, identities, canonical path transitions, action semantics, and the project-root association.
		[[nodiscard]] static std::expected<void, AssetEditRecoveryJournalError> Validate(const AssetEditRecoveryJournalRecord& Record);

		// Reads a bounded version-1 RecoveryRecord.json from its owning transaction directory. Duplicate or unknown JSON keys fail.
		[[nodiscard]] static std::expected<AssetEditRecoveryJournalRecord, AssetEditRecoveryJournalError> Read(
			const std::filesystem::path& JournalPath);

		// Atomically replaces RecoveryRecord.json using a unique sibling temporary file. This provides process-interruption
		// consistency, not power-loss durability.
		[[nodiscard]] static std::expected<void, AssetEditRecoveryJournalError> WriteAtomic(
			const std::filesystem::path& JournalPath, const AssetEditRecoveryJournalRecord& Record);
	};
} // namespace PulseForge::Assets::Detail
