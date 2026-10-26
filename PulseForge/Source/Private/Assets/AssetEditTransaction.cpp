#include "Assets/AssetEditTransaction.h"

#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetMetadata.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/MaterialAsset.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Assets/Detail/GltfRelocation.h"
#include "Assets/Detail/GltfSourceDocument.h"
#include "Core/PlatformServices.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <stdio.h>
#include <errno.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace PulseForge
{
	namespace
	{
		using Path = std::filesystem::path;
		constexpr uint64_t MaximumGltfJsonBytes = 64ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumAssetBytes = 4ull * 1024ull * 1024ull * 1024ull;

		AssetEditTransactionError MakeError(AssetEditTransactionErrorCode Code, Path File, std::string Message)
		{
			return { Code, std::move(File), std::move(Message), std::nullopt };
		}

		std::string PathKey(const Path& Value)
		{
			const std::u8string UTF8 = Value.generic_u8string();
			return { reinterpret_cast<const char*>(UTF8.data()), UTF8.size() };
		}

		std::string AsciiCaseFoldedPathKey(const Path& Value)
		{
			std::string Key = PathKey(Value);
			std::transform(Key.begin(), Key.end(), Key.begin(), [](unsigned char Character)
				{ return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : static_cast<char>(Character); });
			return Key;
		}

		bool IsPathWithin(const Path& Parent, const Path& Candidate)
		{
			auto ParentPart = Parent.begin();
			auto CandidatePart = Candidate.begin();
			for (; ParentPart != Parent.end(); ++ParentPart, ++CandidatePart)
				if (CandidatePart == Candidate.end() || *ParentPart != *CandidatePart) return false;
			return true;
		}

		Path MapPlannedPath(const AssetEditMovePlan& Plan, const Path& Source)
		{
			for (const AssetEditPathMapping& Mapping : Plan.GetPathMappings())
			{
				if (Source == Mapping.Source) return Mapping.Destination;
				if (Plan.IsFolderMove() && IsPathWithin(Mapping.Source, Source))
					return (Mapping.Destination / Source.lexically_relative(Mapping.Source)).lexically_normal();
			}
			return Source;
		}

		std::error_code RenameNoReplace(const Path& Source, const Path& Destination)
		{
#if defined(_WIN32)
			if (MoveFileW(Source.c_str(), Destination.c_str())) return {};
			return { static_cast<int>(GetLastError()), std::system_category() };
#elif defined(__APPLE__)
			if (renamex_np(Source.c_str(), Destination.c_str(), RENAME_EXCL) == 0) return {};
			return { errno, std::generic_category() };
#elif defined(__linux__) && defined(SYS_renameat2)
			if (syscall(SYS_renameat2, AT_FDCWD, Source.c_str(), AT_FDCWD, Destination.c_str(), RENAME_NOREPLACE) == 0) return {};
			return { errno, std::generic_category() };
#else
			(void)Source;
			(void)Destination;
			return std::make_error_code(std::errc::operation_not_supported);
#endif
		}

		bool IsLinkedOrReparsePath(const Path& Value, const std::filesystem::file_status& Status)
		{
			if (std::filesystem::is_symlink(Status)) return true;
#if defined(_WIN32)
			const DWORD Attributes = GetFileAttributesW(Value.c_str());
			return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
			(void)Value;
			return false;
#endif
		}

		bool HasLinkedOrReparseAncestor(const Path& Value)
		{
			std::error_code Error;
			const Path Absolute = std::filesystem::absolute(Value, Error).lexically_normal();
			if (Error) return true;
			Path Current = Absolute.root_path();
			Error.clear();
			for (auto It = Absolute.begin(); It != Absolute.end(); ++It)
			{
				if (It->has_root_path()) continue;
				Current /= *It;
				const auto Status = std::filesystem::symlink_status(Current, Error);
				if (Error)
				{
					if (Error == std::errc::no_such_file_or_directory) return false;
					return true;
				}
				if (IsLinkedOrReparsePath(Current, Status)) return true;
			}
			return false;
		}

		std::expected<void, AssetEditTransactionError> RejectIncompleteTransactions(const Path& Root)
		{
			std::error_code Error;
			for (std::filesystem::directory_iterator It(Root, std::filesystem::directory_options::none, Error), End; !Error && It != End;
				It.increment(Error))
			{
				const std::string Name = It->path().filename().string();
				constexpr std::string_view Prefix = ".pf-asset-edit-";
				if (!Name.starts_with(Prefix)) continue;
				const auto Status = It->symlink_status(Error);
				if (Error || IsLinkedOrReparsePath(It->path(), Status))
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::RecoveryRequired, It->path(),
						"An asset-edit staging entry is linked or cannot be inspected; preserve it and resolve the interrupted operation"));
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::RecoveryRequired, It->path(),
					"An interrupted asset-edit transaction is retained here. Resolve or restore its owned files before starting another "
					"asset edit"));
			}
			if (Error)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Root,
					"Could not check for interrupted asset-edit transactions: " + Error.message()));
			return {};
		}

		std::expected<void, AssetEditTransactionError> ValidateManagedPath(
			const Path& Root, const Path& Relative, bool MustExist, bool MustBeFile)
		{
			if (Relative.empty() || Relative.is_absolute() || Relative.has_root_name() || Relative.has_root_directory() ||
				Relative.lexically_normal() != Relative || Relative.begin() == Relative.end() || *Relative.begin() != "Assets")
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, Relative,
					"Path must be a normalized project-relative path beneath Assets"));

			Path Current = Root;
			std::error_code Error;
			for (const Path& Part : Relative)
			{
				if (Part == "." || Part == "..")
					return std::unexpected(
						MakeError(AssetEditTransactionErrorCode::InvalidPlan, Relative, "Path contains a traversal component"));
				Current /= Part;
				const auto Status = std::filesystem::symlink_status(Current, Error);
				const bool Missing =
					Error == std::errc::no_such_file_or_directory || (!Error && Status.type() == std::filesystem::file_type::not_found);
				if (Missing)
				{
					if (MustExist || Current != Root / Relative)
						return std::unexpected(
							MakeError(AssetEditTransactionErrorCode::StaleInputs, Current, "A required project path is missing"));
					Error.clear();
					continue;
				}
				if (Error || IsLinkedOrReparsePath(Current, Status))
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, Current,
						"Linked or inaccessible paths are not allowed in an asset edit"));
				const bool IsFinal = Current == Root / Relative;
				if (IsFinal ? (MustBeFile ? !std::filesystem::is_regular_file(Status) : !std::filesystem::is_directory(Status))
							: !std::filesystem::is_directory(Status))
					return std::unexpected(
						MakeError(AssetEditTransactionErrorCode::InvalidPlan, Current, "Path has an unexpected filesystem type"));
			}
			return {};
		}

		std::expected<AssetEditFileReceipt, AssetEditTransactionError> CaptureReceipt(
			const Path& Root, const Path& Relative, bool AllowMissing = false)
		{
			const Path Absolute = Root / Relative;
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Absolute, Error);
			const bool Missing =
				Error == std::errc::no_such_file_or_directory || (!Error && Status.type() == std::filesystem::file_type::not_found);
			if (Missing && AllowMissing) return AssetEditFileReceipt{ Relative, false, 0, {} };
			if (Error || Missing || IsLinkedOrReparsePath(Absolute, Status) || !std::filesystem::is_regular_file(Status))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Relative,
					"Expected a regular, non-linked file while recording the operation plan"));
			const uintmax_t Length = std::filesystem::file_size(Absolute, Error);
			if (Error || Length > MaximumAssetBytes)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Relative,
					"File is unavailable or exceeds the bounded asset-edit limit"));

			AssetFingerprintBuilder Fingerprint("PulseForge.AssetEditFile.v1");
			std::ifstream Input(Absolute, std::ios::binary);
			if (!Input)
				return std::unexpected(
					MakeError(AssetEditTransactionErrorCode::StaleInputs, Relative, "Could not open a file required by the operation"));
			std::array<std::byte, 64 * 1024> Buffer{};
			uint64_t Offset = 0;
			while (Input)
			{
				Input.read(reinterpret_cast<char*>(Buffer.data()), static_cast<std::streamsize>(Buffer.size()));
				const std::streamsize Count = Input.gcount();
				if (Count > 0)
				{
					Fingerprint.UpdateText("offset", std::to_string(Offset));
					Fingerprint.UpdateBytes("bytes", std::span<const std::byte>(Buffer.data(), static_cast<size_t>(Count)));
					Offset += static_cast<uint64_t>(Count);
				}
			}
			if (Input.bad() || Offset != Length)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Relative,
					"File changed or could not be read completely while planning the operation"));
			return AssetEditFileReceipt{ Relative, true, static_cast<uint64_t>(Length), Fingerprint.Finish() };
		}

		std::expected<void, AssetEditTransactionError> ValidateReceipt(const Path& Root, const AssetEditFileReceipt& Receipt)
		{
			if (!Receipt.Exists)
			{
				auto Current = CaptureReceipt(Root, Receipt.ProjectRelativePath, true);
				if (!Current || Current->Exists)
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, Receipt.ProjectRelativePath,
						"A destination reserved by the plan now exists"));
				return {};
			}
			auto Current = CaptureReceipt(Root, Receipt.ProjectRelativePath);
			if (!Current || *Current != Receipt)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Receipt.ProjectRelativePath,
					"A file used by the plan changed after preview"));
			return {};
		}

		std::expected<std::string, AssetEditTransactionError> ReadBoundedText(const Path& Absolute, uint64_t Limit)
		{
			std::error_code Error;
			const uintmax_t Length = std::filesystem::file_size(Absolute, Error);
			if (Error || Length > Limit || Length > std::numeric_limits<size_t>::max())
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::UnsupportedOperation, Absolute, "glTF JSON exceeds the supported bounded rewrite size"));
			std::string Text(static_cast<size_t>(Length), '\0');
			std::ifstream Input(Absolute, std::ios::binary);
			if (!Input || (!Text.empty() && !Input.read(Text.data(), static_cast<std::streamsize>(Text.size()))) || Input.bad())
				return std::unexpected(
					MakeError(AssetEditTransactionErrorCode::StaleInputs, Absolute, "Could not read glTF source completely"));
			return Text;
		}

		std::expected<std::string, AssetEditTransactionError> RewriteGltfSource(const Path& Absolute, const Path& OldOwner,
			const Path& NewOwner, const std::vector<Assets::Detail::GltfBufferPathMapping>& Mappings)
		{
			auto Text = ReadBoundedText(Absolute, MaximumGltfJsonBytes);
			if (!Text) return std::unexpected(Text.error());
			auto Rewritten = Assets::Detail::RewriteGltfBufferUris(*Text, OldOwner, NewOwner, Mappings);
			if (!Rewritten)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Absolute, Rewritten.error()));
			return *Rewritten;
		}

		struct StagedEditFile
		{
			Path Target;
			Path Staged;
			Path Backup;
			AssetEditFileReceipt ExpectedAfter;
		};

		std::expected<StagedEditFile, AssetEditTransactionError> StageRewrite(
			const AssetEditMovePlan& Plan, const AssetEditRewrittenSource& Rewrite, size_t Index, const Path& TransactionDirectory)
		{
			const Path Root = Plan.GetProjectRoot();
			const Path Original = Root / Rewrite.ProjectRelativePath;
			const Path Target =
				Plan.IsFolderDuplicate()
					? (TransactionDirectory / "copy" /
						  MapPlannedPath(Plan, Rewrite.ProjectRelativePath).lexically_relative(Plan.GetMapping().Destination))
				: Plan.IsAssetDuplicate() ? Root / Plan.GetMapping().Destination
										  : Root / MapPlannedPath(Plan, Rewrite.ProjectRelativePath);
			const Path Staged = TransactionDirectory / ("rewrite-" + std::to_string(Index) + ".stage");
			const Path Backup = TransactionDirectory / ("rewrite-" + std::to_string(Index) + ".backup");
			std::error_code Error;
			if (!std::filesystem::copy_file(Original, Backup, std::filesystem::copy_options::none, Error) || Error)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Original,
					"Could not retain a rollback copy of a rewritten source: " + (Error ? Error.message() : "backup already exists")));

			if (!Rewrite.IsGlb)
			{
				std::ofstream Output(Staged, std::ios::binary | std::ios::trunc);
				if (!Output)
					return std::unexpected(
						MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Staged, "Could not open staged glTF JSON rewrite"));
				Output.write(Rewrite.Contents.data(), static_cast<std::streamsize>(Rewrite.Contents.size()));
				Output.flush();
				Output.close();
				if (!Output)
					return std::unexpected(
						MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Staged, "Could not finish staged glTF JSON rewrite"));
			}
			else
			{
				std::error_code SizeError;
				const uintmax_t FileSize = std::filesystem::file_size(Original, SizeError);
				if (SizeError)
					return std::unexpected(
						MakeError(AssetEditTransactionErrorCode::StaleInputs, Original, "Could not inspect GLB size before relocation"));
				std::ifstream Input(Original, std::ios::binary);
				std::ofstream Output(Staged, std::ios::binary | std::ios::trunc);
				if (!Input || !Output)
					return std::unexpected(
						MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Staged, "Could not open GLB input or staged output"));
				const Assets::Detail::GlbReadRange ReadRange = [&Input](uint64_t Offset, std::span<std::byte> Bytes)
				{
					if (Offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
						Bytes.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
						return false;
					Input.clear();
					Input.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
					if (!Input) return false;
					if (!Bytes.empty()) Input.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
					return static_cast<bool>(Input) && !Input.bad();
				};
				const Assets::Detail::GltfRelocationWrite Write = [&Output](
																	  std::span<const std::byte> Bytes) -> std::expected<void, std::string>
				{
					if (!Bytes.empty())
						Output.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
					if (!Output) return std::unexpected("Could not write complete staged GLB");
					return {};
				};
				auto Rewritten = Assets::Detail::RewriteGlbBufferUris(
					FileSize, ReadRange, Rewrite.ProjectRelativePath, Rewrite.NewOwnerPath, Rewrite.BufferMappings, Write);
				Output.flush();
				Output.close();
				if (!Rewritten || !Output)
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Staged,
						Rewritten ? "Could not finish staged GLB" : Rewritten.error()));
			}

			const Path StagedRelative = Staged.lexically_relative(Root);
			auto ExpectedAfter = CaptureReceipt(Root, StagedRelative);
			if (!ExpectedAfter) return std::unexpected(ExpectedAfter.error());
			return StagedEditFile{ Target, Staged, Backup, *ExpectedAfter };
		}

		std::expected<void, AssetEditTransactionError> RestoreStagedEditFile(const Path& Root, const StagedEditFile& File)
		{
			auto Current = CaptureReceipt(Root, File.Target.lexically_relative(Root));
			if (!Current || Current->ByteLength != File.ExpectedAfter.ByteLength || Current->Content != File.ExpectedAfter.Content)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::RecoveryRequired, File.Target,
					"Rewritten source changed after this transaction published it; rollback left that file untouched"));
			auto ID = UUID::Generate();
			if (!ID)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::RecoveryRequired, File.Target,
					"Could not reserve a restore staging name; backup retained at " + PathKey(File.Backup)));
			Path Restore = File.Target;
			Restore += "." + ID->ToString() + ".assetedit-restore";
			std::error_code Error;
			if (!std::filesystem::copy_file(File.Backup, Restore, std::filesystem::copy_options::none, Error) || Error)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::RecoveryRequired, File.Target,
					"Could not stage rollback copy: " + Error.message() + "; backup retained at " + PathKey(File.Backup)));
			auto Replaced = Platform::ReplaceFileAtomically(Restore, File.Target);
			if (!Replaced)
			{
				std::filesystem::remove(Restore, Error);
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::RecoveryRequired, File.Target,
					"Could not restore rewritten source: " + Replaced.error().Message + "; backup retained at " + PathKey(File.Backup)));
			}
			return {};
		}

		bool SameMembership(std::vector<AssetRecord> Left, std::vector<AssetRecord> Right)
		{
			const auto Sort = [](auto& Records)
			{
				std::sort(Records.begin(), Records.end(),
					[](const AssetRecord& First, const AssetRecord& Second)
					{
						if (First.ID != Second.ID) return First.ID < Second.ID;
						return PathKey(First.ProjectRelativePath) < PathKey(Second.ProjectRelativePath);
					});
			};
			Sort(Left);
			Sort(Right);
			if (Left.size() != Right.size()) return false;
			for (size_t Index = 0; Index < Left.size(); ++Index)
				if (Left[Index].ID != Right[Index].ID || Left[Index].ProjectRelativePath != Right[Index].ProjectRelativePath) return false;
			return true;
		}

		std::error_code RestoreMovedPath(const Path& Current, const Path& Original, const Path& Temporary, bool CaseOnlyAlias)
		{
			std::error_code Error;
			if (CaseOnlyAlias)
			{
				Error = RenameNoReplace(Current, Temporary);
				if (Error) return Error;
				Error = RenameNoReplace(Temporary, Original);
				return Error;
			}
			return RenameNoReplace(Current, Original);
		}

		std::expected<void, AssetEditTransactionError> ValidatePlanInputs(const AssetEditMovePlan& Plan)
		{
			auto Validated = AssetDependencyExtractor::ValidateInputReceipts(Plan.GetProjectRoot(), Plan.GetCatalogReceipts());
			if (!Validated)
				return std::unexpected(
					MakeError(AssetEditTransactionErrorCode::StaleInputs, Validated.error().Path, Validated.error().Message));
			for (const AssetEditFileReceipt& Receipt : Plan.GetReceipts())
			{
				// Catalog receipts may cover a bounded subset; all explicit plan preconditions are independently rechecked here.
				auto Checked = ValidateReceipt(Plan.GetProjectRoot(), Receipt);
				if (!Checked) return Checked;
			}
			return {};
		}

		std::expected<void, AssetEditTransactionError> InvokeHook(
			const AssetEditTransactionOptions& Options, AssetEditTransactionStage Stage, const Path& Location)
		{
			if (!Options.StageHook) return {};
			auto Result = Options.StageHook(Stage, Location);
			if (!Result)
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::FilesystemFailure, Location, "Deterministic operation stage failed: " + Result.error()));
			return {};
		}
	} // namespace

	std::expected<AssetEditTransaction::PlanPtr, AssetEditTransactionError> AssetEditTransaction::PlanMove(
		const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Asset, const std::filesystem::path& DestinationProjectRelative)
	{
		if (Asset.IsNil() || Snapshot.GetProjectID().IsNil())
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::InvalidPlan, {}, "A move plan requires valid project and asset identities"));
		const Path Root = Snapshot.GetProjectRootPath();
		if (HasLinkedOrReparseAncestor(Root))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Root,
				"Asset edits do not traverse a project root or ancestor that is a symbolic link or reparse point"));
		if (auto Recovery = RejectIncompleteTransactions(Root); !Recovery) return std::unexpected(Recovery.error());
		const auto SourceRecord = Snapshot.GetRegistry().Find(Asset);
		if (!SourceRecord)
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::InvalidPlan, {}, "Selected asset is absent from the retained catalog snapshot"));
		if (DestinationProjectRelative.empty() || DestinationProjectRelative.is_absolute() ||
			DestinationProjectRelative.lexically_normal() != DestinationProjectRelative ||
			DestinationProjectRelative.begin() == DestinationProjectRelative.end() || *DestinationProjectRelative.begin() != "Assets" ||
			DestinationProjectRelative == SourceRecord->ProjectRelativePath)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, DestinationProjectRelative,
				"Destination must be a different normalized path beneath Assets"));
		if (auto Parent = ValidateManagedPath(Root, DestinationProjectRelative.parent_path(), true, false); !Parent)
			return std::unexpected(Parent.error());
		if (auto Source = ValidateManagedPath(Root, SourceRecord->ProjectRelativePath, true, true); !Source)
			return std::unexpected(Source.error());

		auto Plan = std::make_shared<AssetEditMovePlan>();
		auto OperationID = UUID::Generate();
		if (!OperationID)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceRecord->ProjectRelativePath,
				"Could not allocate a unique asset-edit operation identity"));
		Plan->m_OperationID = *OperationID;
		Plan->m_ProjectID = Snapshot.GetProjectID();
		Plan->m_ProjectRoot = Root;
		Plan->m_CatalogGeneration = Snapshot.GetGeneration();
		Plan->m_Asset = Asset;
		Plan->m_Mapping = { SourceRecord->ProjectRelativePath, DestinationProjectRelative };
		Plan->m_RegistryMembership = Snapshot.GetRegistry().GetAssets();
		Plan->m_MovedAssets.push_back(Asset);
		Plan->m_PathMappings.push_back(Plan->m_Mapping);

		Plan->m_CatalogReceipts = Snapshot.GetInputReadSet();
		if (auto Validated = AssetDependencyExtractor::ValidateInputReceipts(Root, Plan->m_CatalogReceipts); !Validated)
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::StaleInputs, Validated.error().Path, Validated.error().Message));
		for (const AssetDependencyInputReceipt& Receipt : Plan->m_CatalogReceipts)
		{
			auto Captured = CaptureReceipt(Root, std::filesystem::u8path(Receipt.ProjectRelativePath), !Receipt.Exists);
			if (!Captured) return std::unexpected(Captured.error());
			Plan->m_Receipts.push_back(std::move(*Captured));
		}
		const Path SourceMeta = AssetMetadataSerializer::GetSidecarPath(Root / SourceRecord->ProjectRelativePath).lexically_relative(Root);
		const Path DestinationAbsolute = Root / DestinationProjectRelative;
		const Path DestinationMeta = AssetMetadataSerializer::GetSidecarPath(DestinationAbsolute).lexically_relative(Root);
		const Path DestinationReceipt = AssetImportProvenanceSerializer::GetSidecarPath(DestinationAbsolute).lexically_relative(Root);
		auto SourceFileReceipt = CaptureReceipt(Root, SourceRecord->ProjectRelativePath);
		auto SourceMetaReceipt = CaptureReceipt(Root, SourceMeta);
		const Path SourceReceipt =
			AssetImportProvenanceSerializer::GetSidecarPath(Root / SourceRecord->ProjectRelativePath).lexically_relative(Root);
		auto SourceProvenanceReceipt = CaptureReceipt(Root, SourceReceipt, true);
		if (!SourceFileReceipt) return std::unexpected(SourceFileReceipt.error());
		if (!SourceMetaReceipt) return std::unexpected(SourceMetaReceipt.error());
		if (!SourceProvenanceReceipt) return std::unexpected(SourceProvenanceReceipt.error());
		Plan->m_Receipts.push_back(*SourceFileReceipt);
		Plan->m_Receipts.push_back(*SourceMetaReceipt);
		Plan->m_Receipts.push_back(*SourceProvenanceReceipt);
		std::error_code AliasError;
		const bool DestinationExists = std::filesystem::exists(DestinationAbsolute, AliasError);
		const auto DestinationStatus = std::filesystem::symlink_status(DestinationAbsolute, AliasError);
		const bool CaseOnlyAlias =
			DestinationExists && !AliasError && !std::filesystem::is_symlink(DestinationStatus) &&
			AsciiCaseFoldedPathKey(SourceRecord->ProjectRelativePath) == AsciiCaseFoldedPathKey(DestinationProjectRelative) &&
			SourceRecord->ProjectRelativePath != DestinationProjectRelative &&
			std::filesystem::equivalent(Root / SourceRecord->ProjectRelativePath, DestinationAbsolute, AliasError) && !AliasError;
		if (CaseOnlyAlias)
		{
			const bool MetadataAlias = std::filesystem::equivalent(Root / SourceMeta, Root / DestinationMeta, AliasError) && !AliasError;
			if (!MetadataAlias)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, DestinationMeta,
					"Case-only move does not resolve to the same metadata sidecar"));
			if (SourceProvenanceReceipt->Exists &&
				!std::filesystem::equivalent(Root / SourceReceipt, Root / DestinationReceipt, AliasError))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, DestinationReceipt,
					"Case-only move does not resolve to the same provenance sidecar"));
			if (AliasError)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, DestinationProjectRelative,
					"Could not verify case-only destination aliases: " + AliasError.message()));
			Plan->m_CaseOnlyAlias = true;
		}
		for (const Path& Destination : { DestinationProjectRelative, DestinationMeta, DestinationReceipt })
		{
			if (Plan->m_CaseOnlyAlias) continue;
			auto Absent = CaptureReceipt(Root, Destination, true);
			if (!Absent) return std::unexpected(Absent.error());
			if (Absent->Exists)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, Destination,
					"Destination source, metadata, or provenance path already exists"));
			Plan->m_Receipts.push_back(*Absent);
		}

		const std::string OldKey = PathKey(SourceRecord->ProjectRelativePath);
		std::map<Path, std::vector<AssetEditGltfBufferReference>> Rewrites;
		for (const AssetDependencyEdge& Edge : Snapshot.GetEdges())
		{
			if (Edge.Kind != AssetDependencyEdgeKind::ImportInput) continue;
			const bool Incoming = Edge.Target.Asset.IsNil() && Edge.Target.SourceFileKey == OldKey;
			const bool OutgoingFromMovedSource =
				Edge.Referrer.Asset == Asset && Edge.Target.Asset.IsNil() && !Edge.Target.SourceFileKey.empty();
			if (!Incoming && !OutgoingFromMovedSource) continue;
			if (Edge.Status != AssetDependencyStatus::Resolved)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation,
					Incoming ? SourceRecord->ProjectRelativePath : std::filesystem::u8path(Edge.Referrer.SourceFileKey),
					"Move cannot safely rewrite a missing, invalid, or unsupported glTF dependency"));
			if (Edge.Location.starts_with("images/"))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation,
					Edge.Referrer.SourceFileKey.empty() ? SourceRecord->ProjectRelativePath
														: std::filesystem::u8path(Edge.Referrer.SourceFileKey),
					"External glTF image references are not supported by the current importer and cannot be rewritten safely"));

			const auto ReferrerRecord = Snapshot.GetRegistry().Find(Edge.Referrer.Asset);
			if (!ReferrerRecord)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, {},
					"A supported buffer reference has no managed referrer in the retained registry"));
			const Path ReferrerPath = ReferrerRecord->ProjectRelativePath;
			const Path ReferrerNewPath = Edge.Referrer.Asset == Asset ? DestinationProjectRelative : ReferrerPath;
			std::string Extension = ReferrerPath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			const bool IsGlb = Extension == ".glb";
			Path TargetPath = std::filesystem::u8path(Edge.Target.SourceFileKey);
			Path NewTargetPath = TargetPath;
			if (Incoming) NewTargetPath = DestinationProjectRelative;
			if (!IsGlb && Extension != ".gltf")
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, ReferrerPath,
					"Dependency edge referrer is not a supported JSON glTF source"));
			AssetEditGltfBufferReference Reference{ Edge.Referrer.Asset, ReferrerPath, TargetPath, NewTargetPath, Edge.Location,
				Edge.OriginalReference };
			Rewrites[ReferrerPath].push_back(Reference);
			Plan->m_AffectedReferences.push_back(std::move(Reference));
		}

		for (auto& [ReferrerPath, References] : Rewrites)
		{
			auto Receipt = CaptureReceipt(Root, ReferrerPath);
			if (!Receipt) return std::unexpected(Receipt.error());
			Plan->m_Receipts.push_back(*Receipt);
			std::vector<Assets::Detail::GltfBufferPathMapping> Mappings;
			for (const AssetEditGltfBufferReference& Reference : References)
				Mappings.push_back({ Reference.TargetSource, Reference.NewTargetSource });
			const Path NewOwner = References.front().ReferringAsset == Asset ? DestinationProjectRelative : ReferrerPath;
			std::string Extension = ReferrerPath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension == ".glb")
			{
				Plan->m_RewrittenSources.push_back({ References.front().ReferringAsset, ReferrerPath, *Receipt, {}, std::move(References),
					true, std::move(Mappings), NewOwner });
			}
			else
			{
				auto Rewritten = RewriteGltfSource(Root / ReferrerPath, ReferrerPath, NewOwner, Mappings);
				if (!Rewritten) return std::unexpected(Rewritten.error());
				Plan->m_RewrittenSources.push_back({ References.front().ReferringAsset, ReferrerPath, *Receipt, std::move(*Rewritten),
					std::move(References), false, std::move(Mappings), NewOwner });
			}
		}

		std::sort(Plan->m_Receipts.begin(), Plan->m_Receipts.end(),
			[](const auto& First, const auto& Second) { return PathKey(First.ProjectRelativePath) < PathKey(Second.ProjectRelativePath); });
		Plan->m_Receipts.erase(std::unique(Plan->m_Receipts.begin(), Plan->m_Receipts.end(), [](const auto& First, const auto& Second)
								   { return First.ProjectRelativePath == Second.ProjectRelativePath; }),
			Plan->m_Receipts.end());
		Plan->m_ProvenanceDisposition = SourceProvenanceReceipt->Exists ? AssetEditProvenanceDisposition::PreserveHistoricalReceiptAtNewPath
																		: AssetEditProvenanceDisposition::NoReceipt;
		return std::static_pointer_cast<const AssetEditMovePlan>(Plan);
	}

	std::expected<AssetEditTransaction::PlanPtr, AssetEditTransactionError> AssetEditTransaction::PlanDuplicateAsset(
		const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Asset, const std::filesystem::path& DestinationProjectRelative)
	{
		if (Asset.IsNil() || Snapshot.GetProjectID().IsNil())
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::InvalidPlan, {}, "A duplicate plan requires valid project and asset identities"));
		const Path Root = Snapshot.GetProjectRootPath();
		if (HasLinkedOrReparseAncestor(Root))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Root,
				"Asset edits do not traverse a project root or ancestor that is a symbolic link or reparse point"));
		if (auto Recovery = RejectIncompleteTransactions(Root); !Recovery) return std::unexpected(Recovery.error());
		const auto SourceRecord = Snapshot.GetRegistry().Find(Asset);
		if (!SourceRecord)
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::InvalidPlan, {}, "Selected asset is absent from the retained catalog snapshot"));
		if (DestinationProjectRelative.empty() || DestinationProjectRelative.is_absolute() ||
			DestinationProjectRelative.lexically_normal() != DestinationProjectRelative ||
			DestinationProjectRelative.begin() == DestinationProjectRelative.end() || *DestinationProjectRelative.begin() != "Assets")
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, DestinationProjectRelative,
				"Duplicate destination must be a normalized project-relative path beneath Assets"));
		if (auto Parent = ValidateManagedPath(Root, DestinationProjectRelative.parent_path(), true, false); !Parent)
			return std::unexpected(Parent.error());
		if (auto Source = ValidateManagedPath(Root, SourceRecord->ProjectRelativePath, true, true); !Source)
			return std::unexpected(Source.error());

		auto Plan = std::make_shared<AssetEditMovePlan>();
		auto OperationID = UUID::Generate();
		auto DuplicateID = UUID::Generate();
		if (!OperationID || !DuplicateID)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceRecord->ProjectRelativePath,
				"Could not allocate unique asset-edit operation and duplicate identities"));
		while (Snapshot.GetRegistry().Find(*DuplicateID))
		{
			DuplicateID = UUID::Generate();
			if (!DuplicateID)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceRecord->ProjectRelativePath,
					"Could not allocate a unique asset identity for the duplicate"));
		}
		Plan->m_OperationID = *OperationID;
		Plan->m_ProjectID = Snapshot.GetProjectID();
		Plan->m_ProjectRoot = Root;
		Plan->m_CatalogGeneration = Snapshot.GetGeneration();
		Plan->m_Asset = Asset;
		Plan->m_Mapping = { SourceRecord->ProjectRelativePath, DestinationProjectRelative };
		Plan->m_IsAssetDuplicate = true;
		Plan->m_RegistryMembership = Snapshot.GetRegistry().GetAssets();
		Plan->m_IdentityRemap.emplace_back(Asset, *DuplicateID);
		Plan->m_CatalogReceipts = Snapshot.GetInputReadSet();
		if (auto Validated = AssetDependencyExtractor::ValidateInputReceipts(Root, Plan->m_CatalogReceipts); !Validated)
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::StaleInputs, Validated.error().Path, Validated.error().Message));
		for (const AssetDependencyInputReceipt& Receipt : Plan->m_CatalogReceipts)
		{
			auto Captured = CaptureReceipt(Root, std::filesystem::u8path(Receipt.ProjectRelativePath), !Receipt.Exists);
			if (!Captured) return std::unexpected(Captured.error());
			Plan->m_Receipts.push_back(std::move(*Captured));
		}

		const Path Source = SourceRecord->ProjectRelativePath;
		const Path SourceMetadata = AssetMetadataSerializer::GetSidecarPath(Root / Source).lexically_relative(Root);
		const Path SourceProvenance = AssetImportProvenanceSerializer::GetSidecarPath(Root / Source).lexically_relative(Root);
		const Path DestinationMetadata =
			AssetMetadataSerializer::GetSidecarPath(Root / DestinationProjectRelative).lexically_relative(Root);
		const Path DestinationProvenance =
			AssetImportProvenanceSerializer::GetSidecarPath(Root / DestinationProjectRelative).lexically_relative(Root);
		auto SourceReceipt = CaptureReceipt(Root, Source);
		auto MetadataReceipt = CaptureReceipt(Root, SourceMetadata);
		auto ProvenanceReceipt = CaptureReceipt(Root, SourceProvenance, true);
		if (!SourceReceipt) return std::unexpected(SourceReceipt.error());
		if (!MetadataReceipt) return std::unexpected(MetadataReceipt.error());
		if (!ProvenanceReceipt) return std::unexpected(ProvenanceReceipt.error());
		Plan->m_Receipts.push_back(*SourceReceipt);
		Plan->m_Receipts.push_back(*MetadataReceipt);
		Plan->m_Receipts.push_back(*ProvenanceReceipt);
		for (const Path& Destination : { DestinationProjectRelative, DestinationMetadata, DestinationProvenance })
		{
			auto Absent = CaptureReceipt(Root, Destination, true);
			if (!Absent) return std::unexpected(Absent.error());
			if (Absent->Exists)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, Destination,
					"Duplicate destination source, metadata, or provenance path already exists"));
			Plan->m_Receipts.push_back(std::move(*Absent));
		}

		std::map<Path, std::vector<AssetEditGltfBufferReference>> Rewrites;
		for (const AssetDependencyEdge& Edge : Snapshot.GetEdges())
		{
			if (Edge.Kind != AssetDependencyEdgeKind::ImportInput || Edge.Referrer.Asset != Asset) continue;
			if (Edge.Location.starts_with("images/"))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Source,
					"External glTF image references are unsupported by the current importer and cannot be rewritten safely"));
			if (Edge.Target.Asset.IsNil() && Edge.Target.SourceFileKey.empty()) continue;
			if (Edge.Status != AssetDependencyStatus::Resolved)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Source,
					"Duplicate cannot safely rewrite a missing, invalid, or unsupported glTF buffer dependency"));
			std::string Extension = Source.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension != ".gltf" && Extension != ".glb")
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Source,
					"Import-input reference belongs to a source without a supported glTF rewrite adapter"));
			Path Target;
			if (!Edge.Target.Asset.IsNil())
			{
				const auto TargetRecord = Snapshot.GetRegistry().Find(Edge.Target.Asset);
				if (!TargetRecord)
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Source,
						"Duplicate buffer dependency refers to an asset missing from the retained registry"));
				Target = TargetRecord->ProjectRelativePath;
			}
			else Target = std::filesystem::u8path(Edge.Target.SourceFileKey);
			AssetEditGltfBufferReference Reference{ Asset, Source, Target, Target, Edge.Location, Edge.OriginalReference };
			Rewrites[Source].push_back(Reference);
			Plan->m_AffectedReferences.push_back(std::move(Reference));
		}
		if (const auto It = Rewrites.find(Source); It != Rewrites.end())
		{
			std::vector<Assets::Detail::GltfBufferPathMapping> Mappings;
			for (const AssetEditGltfBufferReference& Reference : It->second)
				Mappings.push_back({ Reference.TargetSource, Reference.NewTargetSource });
			std::string Extension = Source.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension == ".glb")
				Plan->m_RewrittenSources.push_back(
					{ Asset, Source, *SourceReceipt, {}, It->second, true, std::move(Mappings), DestinationProjectRelative });
			else
			{
				auto Rewritten = RewriteGltfSource(Root / Source, Source, DestinationProjectRelative, Mappings);
				if (!Rewritten) return std::unexpected(Rewritten.error());
				Plan->m_RewrittenSources.push_back({ Asset, Source, *SourceReceipt, std::move(*Rewritten), It->second, false,
					std::move(Mappings), DestinationProjectRelative });
			}
		}
		std::sort(Plan->m_Receipts.begin(), Plan->m_Receipts.end(),
			[](const auto& First, const auto& Second) { return PathKey(First.ProjectRelativePath) < PathKey(Second.ProjectRelativePath); });
		Plan->m_Receipts.erase(std::unique(Plan->m_Receipts.begin(), Plan->m_Receipts.end(), [](const auto& First, const auto& Second)
								   { return First.ProjectRelativePath == Second.ProjectRelativePath; }),
			Plan->m_Receipts.end());
		return std::static_pointer_cast<const AssetEditMovePlan>(Plan);
	}

	std::expected<AssetEditTransaction::PlanPtr, AssetEditTransactionError> AssetEditTransaction::PlanMoveFolder(
		const AssetDependencyCatalogSnapshot& Snapshot, const std::filesystem::path& SourceFolderProjectRelative,
		const std::filesystem::path& DestinationFolderProjectRelative)
	{
		if (Snapshot.GetProjectID().IsNil() || SourceFolderProjectRelative.empty() || DestinationFolderProjectRelative.empty() ||
			SourceFolderProjectRelative.is_absolute() || DestinationFolderProjectRelative.is_absolute() ||
			SourceFolderProjectRelative.lexically_normal() != SourceFolderProjectRelative ||
			DestinationFolderProjectRelative.lexically_normal() != DestinationFolderProjectRelative ||
			SourceFolderProjectRelative == DestinationFolderProjectRelative ||
			SourceFolderProjectRelative.begin() == SourceFolderProjectRelative.end() ||
			DestinationFolderProjectRelative.begin() == DestinationFolderProjectRelative.end() ||
			*SourceFolderProjectRelative.begin() != "Assets" || *DestinationFolderProjectRelative.begin() != "Assets")
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceFolderProjectRelative,
				"Folder move requires distinct normalized paths beneath Assets"));
		if (IsPathWithin(SourceFolderProjectRelative, DestinationFolderProjectRelative))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, DestinationFolderProjectRelative,
				"A folder cannot be moved into itself or one of its descendants"));
		const Path Root = Snapshot.GetProjectRootPath();
		if (HasLinkedOrReparseAncestor(Root))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Root,
				"Asset edits do not traverse a project root or ancestor that is a symbolic link or reparse point"));
		if (auto Recovery = RejectIncompleteTransactions(Root); !Recovery) return std::unexpected(Recovery.error());
		if (auto Source = ValidateManagedPath(Root, SourceFolderProjectRelative, true, false); !Source)
			return std::unexpected(Source.error());
		if (auto Parent = ValidateManagedPath(Root, DestinationFolderProjectRelative.parent_path(), true, false); !Parent)
			return std::unexpected(Parent.error());
		auto DestinationAbsent = CaptureReceipt(Root, DestinationFolderProjectRelative, true);
		if (!DestinationAbsent || DestinationAbsent->Exists)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, DestinationFolderProjectRelative,
				"Folder move destination already exists or cannot be inspected"));

		auto Plan = std::make_shared<AssetEditMovePlan>();
		auto OperationID = UUID::Generate();
		if (!OperationID)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceFolderProjectRelative,
				"Could not allocate a unique asset-edit operation identity"));
		Plan->m_OperationID = *OperationID;
		Plan->m_ProjectID = Snapshot.GetProjectID();
		Plan->m_ProjectRoot = Root;
		Plan->m_CatalogGeneration = Snapshot.GetGeneration();
		Plan->m_IsFolderMove = true;
		Plan->m_Mapping = { SourceFolderProjectRelative, DestinationFolderProjectRelative };
		Plan->m_PathMappings.push_back(Plan->m_Mapping);
		Plan->m_RegistryMembership = Snapshot.GetRegistry().GetAssets();
		for (const AssetRecord& Record : Plan->m_RegistryMembership)
			if (IsPathWithin(SourceFolderProjectRelative, Record.ProjectRelativePath)) Plan->m_MovedAssets.push_back(Record.ID);
		if (Plan->m_MovedAssets.empty())
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceFolderProjectRelative,
				"Folder contains no managed assets in the retained registry snapshot"));
		Plan->m_Asset = Plan->m_MovedAssets.front();
		Plan->m_CatalogReceipts = Snapshot.GetInputReadSet();
		if (auto Validated = AssetDependencyExtractor::ValidateInputReceipts(Root, Plan->m_CatalogReceipts); !Validated)
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::StaleInputs, Validated.error().Path, Validated.error().Message));
		for (const AssetDependencyInputReceipt& Receipt : Plan->m_CatalogReceipts)
		{
			auto Captured = CaptureReceipt(Root, std::filesystem::u8path(Receipt.ProjectRelativePath), !Receipt.Exists);
			if (!Captured) return std::unexpected(Captured.error());
			Plan->m_Receipts.push_back(std::move(*Captured));
		}
		Plan->m_Receipts.push_back(*DestinationAbsent);

		constexpr size_t MaximumFolderEntries = 100000;
		constexpr uint64_t MaximumFolderBytes = 16ull * 1024ull * 1024ull * 1024ull;
		uint64_t TotalBytes = 0;
		std::error_code Error;
		std::filesystem::recursive_directory_iterator Iterator(
			Root / SourceFolderProjectRelative, std::filesystem::directory_options::none, Error);
		const std::filesystem::recursive_directory_iterator End;
		if (Error)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, SourceFolderProjectRelative,
				"Could not enumerate folder contents: " + Error.message()));
		while (Iterator != End)
		{
			if (Plan->m_FolderEntries.size() >= MaximumFolderEntries)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, SourceFolderProjectRelative,
					"Folder exceeds the bounded asset-edit entry limit"));
			const Path Absolute = Iterator->path();
			const auto Status = Iterator->symlink_status(Error);
			if (Error || IsLinkedOrReparsePath(Absolute, Status))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Absolute,
					"Folder moves do not traverse symbolic links or reparse points"));
			const Path Relative = Absolute.lexically_relative(Root);
			Plan->m_FolderEntries.push_back(Relative);
			if (std::filesystem::is_regular_file(Status))
			{
				auto Receipt = CaptureReceipt(Root, Relative);
				if (!Receipt) return std::unexpected(Receipt.error());
				if (Receipt->ByteLength > MaximumFolderBytes - TotalBytes)
					return std::unexpected(MakeError(
						AssetEditTransactionErrorCode::UnsupportedOperation, Relative, "Folder exceeds the bounded asset-edit byte limit"));
				TotalBytes += Receipt->ByteLength;
				Plan->m_Receipts.push_back(std::move(*Receipt));
			}
			else if (!std::filesystem::is_directory(Status))
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::UnsupportedOperation, Absolute, "Folder contains an unsupported filesystem object"));
			Iterator.increment(Error);
			if (Error)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Absolute,
					"Could not finish bounded folder enumeration: " + Error.message()));
		}
		std::sort(Plan->m_FolderEntries.begin(), Plan->m_FolderEntries.end(),
			[](const Path& First, const Path& Second) { return PathKey(First) < PathKey(Second); });

		std::set<AssetID> MovedAssetSet(Plan->m_MovedAssets.begin(), Plan->m_MovedAssets.end());
		std::map<Path, std::vector<AssetEditGltfBufferReference>> Rewrites;
		for (const AssetDependencyEdge& Edge : Snapshot.GetEdges())
		{
			if (Edge.Kind != AssetDependencyEdgeKind::ImportInput || Edge.Target.SourceFileKey.empty()) continue;
			const auto ReferrerRecord = Snapshot.GetRegistry().Find(Edge.Referrer.Asset);
			if (!ReferrerRecord) continue;
			const bool ReferrerMoves = MovedAssetSet.contains(Edge.Referrer.Asset);
			const Path TargetPath = std::filesystem::u8path(Edge.Target.SourceFileKey);
			const Path NewTargetPath = MapPlannedPath(*Plan, TargetPath);
			const bool TargetMoves = NewTargetPath != TargetPath;
			if (!ReferrerMoves && !TargetMoves) continue;
			if (ReferrerMoves && TargetMoves) continue;
			const Path ReferrerPath = ReferrerRecord->ProjectRelativePath;
			if (Edge.Status != AssetDependencyStatus::Resolved || Edge.Location.starts_with("images/"))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, ReferrerPath,
					"Folder move crosses an unresolved or unsupported dependency; no files were changed"));
			std::string Extension = ReferrerPath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension != ".gltf" && Extension != ".glb")
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, ReferrerPath,
					"Cross-boundary import dependency is not owned by a supported glTF/GLB source"));
			const Path NewOwner = MapPlannedPath(*Plan, ReferrerPath);
			AssetEditGltfBufferReference Reference{ Edge.Referrer.Asset, ReferrerPath, TargetPath, NewTargetPath, Edge.Location,
				Edge.OriginalReference };
			Rewrites[ReferrerPath].push_back(std::move(Reference));
		}
		for (auto& [ReferrerPath, References] : Rewrites)
		{
			auto Receipt = CaptureReceipt(Root, ReferrerPath);
			if (!Receipt) return std::unexpected(Receipt.error());
			Plan->m_Receipts.push_back(*Receipt);
			std::vector<Assets::Detail::GltfBufferPathMapping> Mappings;
			for (const AssetEditGltfBufferReference& Reference : References)
				Mappings.push_back({ Reference.TargetSource, Reference.NewTargetSource });
			const Path NewOwner = MapPlannedPath(*Plan, ReferrerPath);
			std::string Extension = ReferrerPath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension == ".glb")
				Plan->m_RewrittenSources.push_back({ References.front().ReferringAsset, ReferrerPath, *Receipt, {}, std::move(References),
					true, std::move(Mappings), NewOwner });
			else
			{
				auto Rewritten = RewriteGltfSource(Root / ReferrerPath, ReferrerPath, NewOwner, Mappings);
				if (!Rewritten) return std::unexpected(Rewritten.error());
				Plan->m_RewrittenSources.push_back({ References.front().ReferringAsset, ReferrerPath, *Receipt, std::move(*Rewritten),
					std::move(References), false, std::move(Mappings), NewOwner });
			}
		}
		for (const AssetEditRewrittenSource& Rewrite : Plan->m_RewrittenSources)
			for (const auto& Reference : Rewrite.References)
				Plan->m_AffectedReferences.push_back(Reference);
		std::sort(Plan->m_Receipts.begin(), Plan->m_Receipts.end(),
			[](const auto& First, const auto& Second) { return PathKey(First.ProjectRelativePath) < PathKey(Second.ProjectRelativePath); });
		Plan->m_Receipts.erase(std::unique(Plan->m_Receipts.begin(), Plan->m_Receipts.end(), [](const auto& First, const auto& Second)
								   { return First.ProjectRelativePath == Second.ProjectRelativePath; }),
			Plan->m_Receipts.end());
		Plan->m_ProvenanceDisposition = AssetEditProvenanceDisposition::PreserveHistoricalReceiptAtNewPath;
		return std::static_pointer_cast<const AssetEditMovePlan>(Plan);
	}

	std::expected<AssetEditTransaction::PlanPtr, AssetEditTransactionError> AssetEditTransaction::PlanDuplicateFolder(
		const AssetDependencyCatalogSnapshot& Snapshot, const std::filesystem::path& SourceFolderProjectRelative,
		const std::filesystem::path& DestinationFolderProjectRelative)
	{
		if (Snapshot.GetProjectID().IsNil() || SourceFolderProjectRelative.empty() || DestinationFolderProjectRelative.empty() ||
			SourceFolderProjectRelative.is_absolute() || DestinationFolderProjectRelative.is_absolute() ||
			SourceFolderProjectRelative.lexically_normal() != SourceFolderProjectRelative ||
			DestinationFolderProjectRelative.lexically_normal() != DestinationFolderProjectRelative ||
			SourceFolderProjectRelative == DestinationFolderProjectRelative ||
			SourceFolderProjectRelative.begin() == SourceFolderProjectRelative.end() ||
			DestinationFolderProjectRelative.begin() == DestinationFolderProjectRelative.end() ||
			*SourceFolderProjectRelative.begin() != "Assets" || *DestinationFolderProjectRelative.begin() != "Assets")
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceFolderProjectRelative,
				"Folder duplicate requires distinct normalized paths beneath Assets"));
		if (IsPathWithin(SourceFolderProjectRelative, DestinationFolderProjectRelative) ||
			IsPathWithin(DestinationFolderProjectRelative, SourceFolderProjectRelative))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, DestinationFolderProjectRelative,
				"Folder duplication cannot overlap its source tree"));
		const Path Root = Snapshot.GetProjectRootPath();
		if (HasLinkedOrReparseAncestor(Root))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Root,
				"Asset edits do not traverse a project root or ancestor that is a symbolic link or reparse point"));
		if (auto Recovery = RejectIncompleteTransactions(Root); !Recovery) return std::unexpected(Recovery.error());
		if (auto Source = ValidateManagedPath(Root, SourceFolderProjectRelative, true, false); !Source)
			return std::unexpected(Source.error());
		if (auto Parent = ValidateManagedPath(Root, DestinationFolderProjectRelative.parent_path(), true, false); !Parent)
			return std::unexpected(Parent.error());
		auto DestinationAbsent = CaptureReceipt(Root, DestinationFolderProjectRelative, true);
		if (!DestinationAbsent || DestinationAbsent->Exists)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, DestinationFolderProjectRelative,
				"Folder duplicate destination already exists or cannot be inspected"));

		auto Plan = std::make_shared<AssetEditMovePlan>();
		auto OperationID = UUID::Generate();
		if (!OperationID)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceFolderProjectRelative,
				"Could not allocate a unique asset-edit operation identity"));
		Plan->m_OperationID = *OperationID;
		Plan->m_ProjectID = Snapshot.GetProjectID();
		Plan->m_ProjectRoot = Root;
		Plan->m_CatalogGeneration = Snapshot.GetGeneration();
		Plan->m_IsFolderMove = true;
		Plan->m_IsFolderDuplicate = true;
		Plan->m_Mapping = { SourceFolderProjectRelative, DestinationFolderProjectRelative };
		Plan->m_PathMappings.push_back(Plan->m_Mapping);
		Plan->m_RegistryMembership = Snapshot.GetRegistry().GetAssets();
		for (const AssetRecord& Record : Plan->m_RegistryMembership)
			if (IsPathWithin(SourceFolderProjectRelative, Record.ProjectRelativePath)) Plan->m_MovedAssets.push_back(Record.ID);
		if (Plan->m_MovedAssets.empty())
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceFolderProjectRelative,
				"Folder contains no managed assets in the retained registry snapshot"));
		Plan->m_Asset = Plan->m_MovedAssets.front();
		std::set<AssetID> ReservedIDs;
		for (const AssetRecord& Record : Plan->m_RegistryMembership)
			ReservedIDs.insert(Record.ID);
		for (const AssetID& OriginalID : Plan->m_MovedAssets)
		{
			std::optional<AssetID> NewID;
			for (size_t Attempt = 0; Attempt < 16; ++Attempt)
			{
				auto Generated = UUID::Generate();
				if (!Generated)
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, SourceFolderProjectRelative,
						"Could not allocate a fresh UUID for duplicated folder asset: " + Generated.error().Message));
				if (!Generated->IsNil() && ReservedIDs.insert(*Generated).second)
				{
					NewID = *Generated;
					break;
				}
			}
			if (!NewID)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, SourceFolderProjectRelative,
					"Could not reserve a UUID that is unique within the current registry and duplicate plan"));
			Plan->m_IdentityRemap.emplace_back(OriginalID, *NewID);
		}
		Plan->m_CatalogReceipts = Snapshot.GetInputReadSet();
		if (auto Validated = AssetDependencyExtractor::ValidateInputReceipts(Root, Plan->m_CatalogReceipts); !Validated)
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::StaleInputs, Validated.error().Path, Validated.error().Message));
		for (const AssetDependencyInputReceipt& Receipt : Plan->m_CatalogReceipts)
		{
			auto Captured = CaptureReceipt(Root, std::filesystem::u8path(Receipt.ProjectRelativePath), !Receipt.Exists);
			if (!Captured) return std::unexpected(Captured.error());
			Plan->m_Receipts.push_back(std::move(*Captured));
		}
		Plan->m_Receipts.push_back(*DestinationAbsent);
		constexpr size_t MaximumFolderEntries = 100000;
		constexpr uint64_t MaximumFolderBytes = 16ull * 1024ull * 1024ull * 1024ull;
		uint64_t TotalBytes = 0;
		std::error_code Error;
		std::filesystem::recursive_directory_iterator Iterator(
			Root / SourceFolderProjectRelative, std::filesystem::directory_options::none, Error);
		const std::filesystem::recursive_directory_iterator End;
		if (Error)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, SourceFolderProjectRelative,
				"Could not enumerate folder contents: " + Error.message()));
		while (Iterator != End)
		{
			if (Plan->m_FolderEntries.size() >= MaximumFolderEntries)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, SourceFolderProjectRelative,
					"Folder exceeds the bounded asset-edit entry limit"));
			const Path Absolute = Iterator->path();
			const auto Status = Iterator->symlink_status(Error);
			if (Error || std::filesystem::is_symlink(Status))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Absolute,
					"Folder duplication does not traverse symbolic links or reparse points"));
			const Path Relative = Absolute.lexically_relative(Root);
			Plan->m_FolderEntries.push_back(Relative);
			if (std::filesystem::is_regular_file(Status))
			{
				auto Receipt = CaptureReceipt(Root, Relative);
				if (!Receipt) return std::unexpected(Receipt.error());
				if (Receipt->ByteLength > MaximumFolderBytes - TotalBytes)
					return std::unexpected(MakeError(
						AssetEditTransactionErrorCode::UnsupportedOperation, Relative, "Folder exceeds the bounded asset-edit byte limit"));
				TotalBytes += Receipt->ByteLength;
				Plan->m_Receipts.push_back(std::move(*Receipt));
			}
			else if (!std::filesystem::is_directory(Status))
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::UnsupportedOperation, Absolute, "Folder contains an unsupported filesystem object"));
			Iterator.increment(Error);
			if (Error)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Absolute,
					"Could not finish bounded folder enumeration: " + Error.message()));
		}
		std::sort(Plan->m_FolderEntries.begin(), Plan->m_FolderEntries.end(),
			[](const Path& First, const Path& Second) { return PathKey(First) < PathKey(Second); });
		std::set<Path> ManagedSidecars;
		for (const AssetRecord& Record : Plan->m_RegistryMembership)
			if (IsPathWithin(SourceFolderProjectRelative, Record.ProjectRelativePath))
				ManagedSidecars.insert(AssetMetadataSerializer::GetSidecarPath(Root / Record.ProjectRelativePath).lexically_relative(Root));
		for (const Path& Entry : Plan->m_FolderEntries)
		{
			const std::string Filename = Entry.filename().string();
			if (Filename.ends_with(".meta") && !ManagedSidecars.contains(Entry))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Entry,
					"Folder duplicate cannot safely classify an orphan metadata sidecar"));
		}
		const std::set<AssetID> MovedIDs(Plan->m_MovedAssets.begin(), Plan->m_MovedAssets.end());
		for (const AssetDependencyEdge& Edge : Snapshot.GetEdges())
		{
			if (Edge.Kind != AssetDependencyEdgeKind::RuntimeReference || !MovedIDs.contains(Edge.Referrer.Asset) ||
				!MovedIDs.contains(Edge.Target.Asset))
				continue;
			const auto Referrer = Snapshot.GetRegistry().Find(Edge.Referrer.Asset);
			const bool SupportedMaterialField = Edge.Location == "material.baseColorTexture" ||
												Edge.Location == "material.metallicRoughnessTexture" ||
												Edge.Location == "material.normalTexture" || Edge.Location == "material.occlusionTexture" ||
												Edge.Location == "material.emissiveTexture";
			if (!Referrer || !HasAssetTypeExtension(Referrer->ProjectRelativePath, AssetFileType::Material) || !SupportedMaterialField ||
				Edge.Status != AssetDependencyStatus::Resolved)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Edge.Location,
					"Folder duplicate contains an internal authored reference without a supported typed remap adapter"));
			Plan->m_InternalAssetReferences.push_back({ Edge.Referrer.Asset, Edge.Target.Asset, Edge.Location, Edge.OriginalReference });
		}

		std::map<Path, std::vector<AssetEditGltfBufferReference>> Rewrites;
		for (const AssetDependencyEdge& Edge : Snapshot.GetEdges())
		{
			if (Edge.Kind != AssetDependencyEdgeKind::ImportInput || Edge.Target.SourceFileKey.empty() ||
				!MovedIDs.contains(Edge.Referrer.Asset))
				continue;
			const Path ReferrerPath = Snapshot.GetRegistry().Find(Edge.Referrer.Asset)->ProjectRelativePath;
			const Path TargetPath = std::filesystem::u8path(Edge.Target.SourceFileKey);
			if (IsPathWithin(SourceFolderProjectRelative, TargetPath)) continue;
			if (Edge.Status != AssetDependencyStatus::Resolved || Edge.Location.starts_with("images/"))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, ReferrerPath,
					"Folder duplicate has an unresolved or unsupported external glTF input"));
			std::string Extension = ReferrerPath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension != ".gltf" && Extension != ".glb")
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, ReferrerPath,
					"Folder duplicate found an external input on an unsupported source format"));
			AssetEditGltfBufferReference Reference{ Edge.Referrer.Asset, ReferrerPath, TargetPath, TargetPath, Edge.Location,
				Edge.OriginalReference };
			Rewrites[ReferrerPath].push_back(std::move(Reference));
		}
		for (auto& [ReferrerPath, References] : Rewrites)
		{
			auto Receipt = CaptureReceipt(Root, ReferrerPath);
			if (!Receipt) return std::unexpected(Receipt.error());
			Plan->m_Receipts.push_back(*Receipt);
			std::vector<Assets::Detail::GltfBufferPathMapping> Mappings;
			for (const auto& Reference : References)
				Mappings.push_back({ Reference.TargetSource, Reference.NewTargetSource });
			const Path NewOwner = MapPlannedPath(*Plan, ReferrerPath);
			std::string Extension = ReferrerPath.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (Extension == ".glb")
				Plan->m_RewrittenSources.push_back({ References.front().ReferringAsset, ReferrerPath, *Receipt, {}, std::move(References),
					true, std::move(Mappings), NewOwner });
			else
			{
				auto Rewritten = RewriteGltfSource(Root / ReferrerPath, ReferrerPath, NewOwner, Mappings);
				if (!Rewritten) return std::unexpected(Rewritten.error());
				Plan->m_RewrittenSources.push_back({ References.front().ReferringAsset, ReferrerPath, *Receipt, std::move(*Rewritten),
					std::move(References), false, std::move(Mappings), NewOwner });
			}
		}
		for (const auto& Rewrite : Plan->m_RewrittenSources)
			for (const auto& Reference : Rewrite.References)
				Plan->m_AffectedReferences.push_back(Reference);
		std::sort(Plan->m_Receipts.begin(), Plan->m_Receipts.end(),
			[](const auto& First, const auto& Second) { return PathKey(First.ProjectRelativePath) < PathKey(Second.ProjectRelativePath); });
		Plan->m_Receipts.erase(std::unique(Plan->m_Receipts.begin(), Plan->m_Receipts.end(), [](const auto& First, const auto& Second)
								   { return First.ProjectRelativePath == Second.ProjectRelativePath; }),
			Plan->m_Receipts.end());
		Plan->m_ProvenanceDisposition = AssetEditProvenanceDisposition::NoReceipt;
		return std::static_pointer_cast<const AssetEditMovePlan>(Plan);
	}

	std::expected<AssetEditTransactionResult, AssetEditTransactionError> AssetEditTransaction::ApplyMove(const AssetEditMovePlan& Plan,
		const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID, uint64_t CurrentCatalogGeneration,
		AssetRegistry& Registry, const AssetEditTransactionOptions& Options)
	{
		if (Plan.IsAssetDuplicate())
			return ApplyAssetDuplicate(Plan, CurrentProjectRoot, CurrentProjectID, CurrentCatalogGeneration, Registry, Options);
		if (Plan.IsFolderMove())
		{
			if (Plan.IsFolderDuplicate())
				return ApplyFolderDuplicate(Plan, CurrentProjectRoot, CurrentProjectID, CurrentCatalogGeneration, Registry, Options);
			return ApplyFolderMove(Plan, CurrentProjectRoot, CurrentProjectID, CurrentCatalogGeneration, Registry, Options);
		}
		std::error_code RootError;
		const std::filesystem::path NormalizedRoot = std::filesystem::absolute(CurrentProjectRoot, RootError).lexically_normal();
		if (RootError || NormalizedRoot != Plan.GetProjectRoot() || HasLinkedOrReparseAncestor(NormalizedRoot) ||
			CurrentProjectID != Plan.GetProjectID() || CurrentCatalogGeneration != Plan.GetCatalogGeneration())
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleProject, Plan.GetMapping().Source,
				"Project identity or catalog generation changed after the operation preview"));
		if (auto Recovery = RejectIncompleteTransactions(NormalizedRoot); !Recovery) return std::unexpected(Recovery.error());
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());
		AssetRegistry DiskRegistry;
		auto RegistryResult = DiskRegistry.Rebuild(Plan.GetProjectRoot());
		if (!RegistryResult)
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::RegistryInvalid, "Assets", "Current registry failed validation before the move"));
		if (!SameMembership(Plan.GetRegistryMembership(), DiskRegistry.GetAssets()) ||
			!SameMembership(Plan.GetRegistryMembership(), Registry.GetAssets()))
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::StaleInputs, "Assets", "Registry membership changed after the operation preview"));
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		// Recheck after deterministic hooks and immediately before any authored-file mutation.
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());
		AssetRegistry FinalDiskRegistry;
		if (auto Result = FinalDiskRegistry.Rebuild(Plan.GetProjectRoot());
			!Result || !SameMembership(Plan.GetRegistryMembership(), FinalDiskRegistry.GetAssets()))
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::StaleInputs, "Assets", "Registry membership changed at the operation commit boundary"));
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeFileMutation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());

		const Path TransactionDirectory = Plan.GetProjectRoot() / (".pf-asset-edit-" + Plan.GetOperationID().ToString());
		std::error_code Error;
		if (!std::filesystem::create_directory(TransactionDirectory, Error) || Error)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, TransactionDirectory,
				"Could not create an exclusive transaction staging directory: " + Error.message()));
		std::vector<StagedEditFile> StagedFiles;
		for (size_t Index = 0; Index < Plan.GetRewrittenSources().size(); ++Index)
		{
			const AssetEditRewrittenSource& Rewrite = Plan.GetRewrittenSources()[Index];
			auto Staged = StageRewrite(Plan, Rewrite, Index, TransactionDirectory);
			if (!Staged)
			{
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(Staged.error());
			}
			auto AfterRead = CaptureReceipt(Plan.GetProjectRoot(), Rewrite.ProjectRelativePath);
			if (!AfterRead || *AfterRead != Rewrite.Before)
			{
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Rewrite.ProjectRelativePath,
					"A rewritten source changed while its transaction output was being staged"));
			}
			StagedFiles.push_back(std::move(*Staged));
		}
		std::vector<size_t> PublishedRewrites;
		auto RollbackReferences = [&]() -> std::string
		{
			std::string Failures;
			for (auto It = PublishedRewrites.rbegin(); It != PublishedRewrites.rend(); ++It)
			{
				auto Restored = RestoreStagedEditFile(Plan.GetProjectRoot(), StagedFiles[*It]);
				if (!Restored) Failures += PathKey(StagedFiles[*It].Target) + ": " + Restored.error().Message + "; ";
			}
			return Failures;
		};
		for (size_t Index = 0; Index < StagedFiles.size(); ++Index)
		{
			const AssetEditRewrittenSource& Rewrite = Plan.GetRewrittenSources()[Index];
			if (Rewrite.Asset == Plan.GetAsset()) continue;
			auto Replaced = Platform::ReplaceFileAtomically(StagedFiles[Index].Staged, StagedFiles[Index].Target);
			if (!Replaced)
			{
				const std::string Rollback = RollbackReferences();
				if (!Rollback.empty())
				{
					auto Recovery = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Rewrite.ProjectRelativePath,
						"Reference rewrite failed and rollback was incomplete: " + Rollback);
					Recovery.RecoveryPath = TransactionDirectory;
					return std::unexpected(std::move(Recovery));
				}
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Rewrite.ProjectRelativePath,
					"Could not publish staged reference rewrite: " + Replaced.error().Message));
			}
			PublishedRewrites.push_back(Index);
			if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterReferenceWrite, Rewrite.ProjectRelativePath); !Hook)
			{
				const std::string Rollback = RollbackReferences();
				if (!Rollback.empty())
				{
					auto Error = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Rewrite.ProjectRelativePath,
						"Injected failure and reference rollback was incomplete: " + Rollback);
					Error.RecoveryPath = TransactionDirectory;
					return std::unexpected(std::move(Error));
				}
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(Hook.error());
			}
		}

		const Path Source = Plan.GetProjectRoot() / Plan.GetMapping().Source;
		const Path Destination = Plan.GetProjectRoot() / Plan.GetMapping().Destination;
		const Path SourceMeta = AssetMetadataSerializer::GetSidecarPath(Source);
		const Path DestinationMeta = AssetMetadataSerializer::GetSidecarPath(Destination);
		const Path SourceReceipt = AssetImportProvenanceSerializer::GetSidecarPath(Source);
		const Path DestinationReceipt = AssetImportProvenanceSerializer::GetSidecarPath(Destination);
		Path SourceStage = Source;
		SourceStage += ".assetedit-" + Plan.GetOperationID().ToString() + ".stage";
		Path MetaStage = SourceMeta;
		MetaStage += ".assetedit-" + Plan.GetOperationID().ToString() + ".stage";
		Path ReceiptStage = SourceReceipt;
		ReceiptStage += ".assetedit-" + Plan.GetOperationID().ToString() + ".stage";
		if (Plan.IsCaseOnlyAlias())
		{
			for (const Path& Temporary : { SourceStage, MetaStage, ReceiptStage })
			{
				auto Absent = CaptureReceipt(Plan.GetProjectRoot(), Temporary.lexically_relative(Plan.GetProjectRoot()), true);
				if (!Absent || Absent->Exists)
				{
					const std::string Rollback = RollbackReferences();
					if (Rollback.empty()) std::filesystem::remove_all(TransactionDirectory, Error);
					auto Failure = MakeError(
						Rollback.empty() ? AssetEditTransactionErrorCode::Conflict : AssetEditTransactionErrorCode::RecoveryRequired,
						Temporary,
						"Case-only rename staging path is already occupied or inaccessible" +
							(Rollback.empty() ? std::string{} : "; rollback incomplete: " + Rollback));
					if (!Rollback.empty()) Failure.RecoveryPath = TransactionDirectory;
					return std::unexpected(std::move(Failure));
				}
			}
		}
		bool SourceInStage = false;
		bool MetaInStage = false;
		bool ReceiptInStage = false;
		if (Plan.IsCaseOnlyAlias())
		{
			Error = RenameNoReplace(Source, SourceStage);
			SourceInStage = !Error;
			if (!Error)
			{
				Error = RenameNoReplace(SourceStage, Destination);
				SourceInStage = static_cast<bool>(Error);
			}
			if (!Error)
			{
				Error = RenameNoReplace(SourceMeta, MetaStage);
				MetaInStage = !Error;
			}
			if (!Error)
			{
				Error = RenameNoReplace(MetaStage, DestinationMeta);
				MetaInStage = static_cast<bool>(Error);
			}
		}
		else
		{
			Error = RenameNoReplace(Source, Destination);
			if (!Error) Error = RenameNoReplace(SourceMeta, DestinationMeta);
		}
		bool ReceiptMoved = false;
		if (!Error)
		{
			const auto ReceiptStatus = std::filesystem::symlink_status(SourceReceipt, Error);
			if (!Error && ReceiptStatus.type() != std::filesystem::file_type::not_found)
			{
				Error.clear();
				if (Plan.IsCaseOnlyAlias())
				{
					Error = RenameNoReplace(SourceReceipt, ReceiptStage);
					ReceiptInStage = !Error;
					if (!Error)
					{
						Error = RenameNoReplace(ReceiptStage, DestinationReceipt);
						ReceiptInStage = static_cast<bool>(Error);
					}
				}
				else Error = RenameNoReplace(SourceReceipt, DestinationReceipt);
				ReceiptMoved = !Error;
			}
			else if (Error == std::errc::no_such_file_or_directory ||
					 (!Error && ReceiptStatus.type() == std::filesystem::file_type::not_found))
				Error.clear();
		}
		if (Error)
		{
			const std::string Failure = Error.message();
			std::string RollbackFailure;
			if (ReceiptMoved)
			{
				Error = RestoreMovedPath(
					ReceiptInStage ? ReceiptStage : DestinationReceipt, SourceReceipt, ReceiptStage, Plan.IsCaseOnlyAlias());
				if (Error) RollbackFailure += " receipt restore: " + Error.message();
			}
			else if (ReceiptInStage)
			{
				Error = RenameNoReplace(ReceiptStage, SourceReceipt);
				if (Error) RollbackFailure += " staged provenance restore: " + Error.message();
			}
			if (std::filesystem::exists(DestinationMeta))
			{
				Error = RestoreMovedPath(DestinationMeta, SourceMeta, MetaStage, Plan.IsCaseOnlyAlias());
				if (Error) RollbackFailure += " metadata restore: " + Error.message();
			}
			if (std::filesystem::exists(Destination))
			{
				Error = RestoreMovedPath(Destination, Source, SourceStage, Plan.IsCaseOnlyAlias());
				if (Error) RollbackFailure += " source restore: " + Error.message();
			}
			if (SourceInStage)
			{
				Error = RenameNoReplace(SourceStage, Source);
				if (Error) RollbackFailure += " staged source restore: " + Error.message();
			}
			if (MetaInStage)
			{
				Error = RenameNoReplace(MetaStage, SourceMeta);
				if (Error) RollbackFailure += " staged metadata restore: " + Error.message();
			}
			RollbackFailure += RollbackReferences();
			if (!RollbackFailure.empty())
			{
				auto Recovery = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Plan.GetMapping().Destination,
					"Asset move failed (" + Failure + ") and rollback needs recovery:" + RollbackFailure);
				Recovery.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(Recovery));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Plan.GetMapping().Destination,
				"Could not move the source/metadata/provenance pair: " + Failure));
		}
		auto RollbackCommittedMove = [&]() -> std::string
		{
			std::string Failures;
			Failures += RollbackReferences();
			std::error_code StatusError;
			const auto SourceStatus = std::filesystem::symlink_status(Source, StatusError);
			if (!Plan.IsCaseOnlyAlias() && ((StatusError != std::errc::no_such_file_or_directory && StatusError) ||
											   (!StatusError && SourceStatus.type() != std::filesystem::file_type::not_found)))
				return "original source path became occupied; destination and references retained for recovery";
			const auto MovedMetadata = AssetMetadataSerializer::LoadFromFile(DestinationMeta);
			if (!MovedMetadata || MovedMetadata->ID != Plan.GetAsset())
				return "destination metadata no longer identifies the moved asset; destination retained for recovery";
			StatusError.clear();
			StatusError = RestoreMovedPath(Destination, Source, SourceStage, Plan.IsCaseOnlyAlias());
			if (StatusError) Failures += "source restore: " + StatusError.message() + "; ";
			if (!StatusError)
			{
				StatusError.clear();
				StatusError = RestoreMovedPath(DestinationMeta, SourceMeta, MetaStage, Plan.IsCaseOnlyAlias());
				if (StatusError) Failures += "metadata restore: " + StatusError.message() + "; ";
			}
			if (!StatusError && ReceiptMoved)
			{
				StatusError.clear();
				StatusError = RestoreMovedPath(DestinationReceipt, SourceReceipt, ReceiptStage, Plan.IsCaseOnlyAlias());
				if (StatusError) Failures += "provenance restore: " + StatusError.message() + "; ";
			}
			return Failures;
		};
		for (size_t Index = 0; Index < StagedFiles.size(); ++Index)
		{
			if (Plan.GetRewrittenSources()[Index].Asset != Plan.GetAsset()) continue;
			auto Replaced = Platform::ReplaceFileAtomically(StagedFiles[Index].Staged, StagedFiles[Index].Target);
			if (!Replaced)
			{
				const std::string Rollback = RollbackCommittedMove();
				if (!Rollback.empty())
				{
					auto Recovery = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Destination,
						"Could not publish the moved glTF rewrite; rollback incomplete: " + Rollback);
					Recovery.RecoveryPath = TransactionDirectory;
					return std::unexpected(std::move(Recovery));
				}
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Destination,
					"Could not publish staged moved-source rewrite: " + Replaced.error().Message));
			}
			PublishedRewrites.push_back(Index);
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterAssetMove, Plan.GetMapping().Destination); !Hook)
		{
			const std::string Rollback = RollbackCommittedMove();
			if (!Rollback.empty())
			{
				auto Recovery = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Plan.GetMapping().Destination,
					"Injected move failure and rollback was incomplete: " + Rollback);
				Recovery.RecoveryPath = Destination;
				return std::unexpected(std::move(Recovery));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(Hook.error());
		}

		AssetRegistry Candidate;
		if (auto Refreshed = Candidate.Rebuild(Plan.GetProjectRoot()); !Refreshed)
		{
			const std::string Failure = "Candidate registry rejected moved files";
			const std::string Rollback = RollbackCommittedMove();
			if (!Rollback.empty())
			{
				auto Recovery = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Plan.GetMapping().Destination,
					Failure + "; filesystem rollback was incomplete: " + Rollback);
				Recovery.RecoveryPath = Destination;
				return std::unexpected(std::move(Recovery));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::RegistryInvalid, Plan.GetMapping().Destination, Failure));
		}
		if (!SameMembership(
				[&]
				{
					auto Expected = Plan.GetRegistryMembership();
					for (AssetRecord& Record : Expected)
						if (Record.ID == Plan.GetAsset()) Record.ProjectRelativePath = Plan.GetMapping().Destination;
					return Expected;
				}(),
				Candidate.GetAssets()))
		{
			const std::string Rollback = RollbackCommittedMove();
			if (!Rollback.empty())
			{
				auto Recovery = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Plan.GetMapping().Destination,
					"Post-move registry membership differs from the planned mapping; rollback incomplete: " + Rollback);
				Recovery.RecoveryPath = Destination;
				return std::unexpected(std::move(Recovery));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::RegistryInvalid, Plan.GetMapping().Destination,
				"Post-move registry membership differs from the planned identity/path mapping"));
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRegistryPublication, Plan.GetMapping().Destination); !Hook)
		{
			const std::string Rollback = RollbackCommittedMove();
			if (!Rollback.empty())
			{
				auto Recovery = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Plan.GetMapping().Destination,
					"Registry publication was rejected and rollback was incomplete: " + Rollback);
				Recovery.RecoveryPath = Destination;
				return std::unexpected(std::move(Recovery));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(Hook.error());
		}
		Registry = std::move(Candidate);
		std::filesystem::remove_all(TransactionDirectory, Error);
		return AssetEditTransactionResult{ { Plan.GetAsset(), Plan.GetMapping().Destination }, Plan.GetProvenanceDisposition() };
	}

	std::expected<AssetEditTransactionResult, AssetEditTransactionError> AssetEditTransaction::ApplyFolderMove(
		const AssetEditMovePlan& Plan, const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID,
		uint64_t CurrentCatalogGeneration, AssetRegistry& Registry, const AssetEditTransactionOptions& Options)
	{
		if (!Plan.IsFolderMove())
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::InvalidPlan, Plan.GetMapping().Source, "ApplyFolderMove requires a folder move plan"));
		std::error_code Error;
		const Path NormalizedRoot = std::filesystem::absolute(CurrentProjectRoot, Error).lexically_normal();
		if (Error || NormalizedRoot != Plan.GetProjectRoot() || HasLinkedOrReparseAncestor(NormalizedRoot) ||
			CurrentProjectID != Plan.GetProjectID() || CurrentCatalogGeneration != Plan.GetCatalogGeneration())
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleProject, Plan.GetMapping().Source,
				"Project identity, root, or catalog generation changed after folder preview"));
		if (auto Recovery = RejectIncompleteTransactions(NormalizedRoot); !Recovery) return std::unexpected(Recovery.error());
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());
		auto EnumerateEntries = [&]() -> std::expected<std::vector<Path>, AssetEditTransactionError>
		{
			std::vector<Path> Entries;
			std::error_code WalkError;
			std::filesystem::recursive_directory_iterator Iterator(
				Plan.GetProjectRoot() / Plan.GetMapping().Source, std::filesystem::directory_options::none, WalkError);
			const std::filesystem::recursive_directory_iterator End;
			if (WalkError)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
					"Could not revalidate folder membership: " + WalkError.message()));
			while (Iterator != End)
			{
				const Path Absolute = Iterator->path();
				const auto Status = Iterator->symlink_status(WalkError);
				if (WalkError || IsLinkedOrReparsePath(Absolute, Status))
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Absolute,
						"Folder membership now contains a linked or inaccessible path"));
				Entries.push_back(Absolute.lexically_relative(Plan.GetProjectRoot()));
				if (Entries.size() > 100000)
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
						"Folder membership exceeds the validated entry bound"));
				Iterator.increment(WalkError);
				if (WalkError)
					return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Absolute,
						"Could not complete folder membership validation: " + WalkError.message()));
			}
			std::sort(
				Entries.begin(), Entries.end(), [](const Path& First, const Path& Second) { return PathKey(First) < PathKey(Second); });
			return Entries;
		};
		auto Entries = EnumerateEntries();
		if (!Entries || *Entries != Plan.m_FolderEntries)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
				"Folder membership changed after the operation preview"));
		AssetRegistry DiskRegistry;
		if (auto Result = DiskRegistry.Rebuild(Plan.GetProjectRoot());
			!Result || !SameMembership(Plan.GetRegistryMembership(), DiskRegistry.GetAssets()) ||
			!SameMembership(Plan.GetRegistryMembership(), Registry.GetAssets()))
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::StaleInputs, "Assets", "Registry membership changed after the folder operation preview"));
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());
		Entries = EnumerateEntries();
		AssetRegistry FinalRegistry;
		if (!Entries || *Entries != Plan.m_FolderEntries || !FinalRegistry.Rebuild(Plan.GetProjectRoot()) ||
			!SameMembership(Plan.GetRegistryMembership(), FinalRegistry.GetAssets()))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
				"Folder contents or registry membership changed at the commit boundary"));
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeFileMutation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());
		Entries = EnumerateEntries();
		if (!Entries || *Entries != Plan.m_FolderEntries)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
				"Folder membership changed immediately before mutation"));
		std::error_code DestinationError;
		if (std::filesystem::exists(Plan.GetProjectRoot() / Plan.GetMapping().Destination, DestinationError) || DestinationError)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, Plan.GetMapping().Destination,
				"Folder move destination appeared before mutation"));

		const Path TransactionDirectory = Plan.GetProjectRoot() / (".pf-asset-edit-" + Plan.GetOperationID().ToString());
		if (!std::filesystem::create_directory(TransactionDirectory, Error) || Error)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, TransactionDirectory,
				"Could not create an exclusive folder-transaction staging directory: " + Error.message()));
		std::vector<StagedEditFile> StagedFiles;
		for (size_t Index = 0; Index < Plan.GetRewrittenSources().size(); ++Index)
		{
			const AssetEditRewrittenSource& Rewrite = Plan.GetRewrittenSources()[Index];
			auto Staged = StageRewrite(Plan, Rewrite, Index, TransactionDirectory);
			if (!Staged)
			{
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(Staged.error());
			}
			auto Current = CaptureReceipt(Plan.GetProjectRoot(), Rewrite.ProjectRelativePath);
			if (!Current || *Current != Rewrite.Before)
			{
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Rewrite.ProjectRelativePath,
					"A glTF/GLB source changed while its folder-move rewrite was staged"));
			}
			StagedFiles.push_back(std::move(*Staged));
		}
		std::vector<size_t> PublishedRewrites;
		bool FolderMoved = false;
		const Path Source = Plan.GetProjectRoot() / Plan.GetMapping().Source;
		const Path Destination = Plan.GetProjectRoot() / Plan.GetMapping().Destination;
		auto Rollback = [&]() -> std::string
		{
			std::string Failures;
			for (auto It = PublishedRewrites.rbegin(); It != PublishedRewrites.rend(); ++It)
			{
				auto Restored = RestoreStagedEditFile(Plan.GetProjectRoot(), StagedFiles[*It]);
				if (!Restored) Failures += PathKey(StagedFiles[*It].Target) + ": " + Restored.error().Message + "; ";
			}
			if (FolderMoved)
			{
				std::error_code MoveError = RenameNoReplace(Destination, Source);
				if (MoveError) Failures += "folder restore: " + MoveError.message() + "; ";
			}
			return Failures;
		};
		auto FailAfterMutation =
			[&](AssetEditTransactionError Failure) -> std::expected<AssetEditTransactionResult, AssetEditTransactionError>
		{
			const std::string RollbackFailure = Rollback();
			if (!RollbackFailure.empty())
			{
				Failure.Code = AssetEditTransactionErrorCode::RecoveryRequired;
				Failure.Message += "; rollback incomplete: " + RollbackFailure;
				Failure.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(Failure));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(std::move(Failure));
		};
		for (size_t Index = 0; Index < StagedFiles.size(); ++Index)
		{
			const AssetEditRewrittenSource& Rewrite = Plan.GetRewrittenSources()[Index];
			if (IsPathWithin(Plan.GetMapping().Source, Rewrite.ProjectRelativePath)) continue;
			auto BeforeReplace = CaptureReceipt(Plan.GetProjectRoot(), Rewrite.ProjectRelativePath);
			if (!BeforeReplace || *BeforeReplace != Rewrite.Before)
				return FailAfterMutation(MakeError(AssetEditTransactionErrorCode::StaleInputs, Rewrite.ProjectRelativePath,
					"External glTF/GLB source changed before reference rewrite publication"));
			auto Replaced = Platform::ReplaceFileAtomically(StagedFiles[Index].Staged, StagedFiles[Index].Target);
			if (!Replaced)
				return FailAfterMutation(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, StagedFiles[Index].Target,
					"Could not publish an external reference rewrite: " + Replaced.error().Message));
			PublishedRewrites.push_back(Index);
			if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterReferenceWrite, StagedFiles[Index].Target); !Hook)
				return FailAfterMutation(Hook.error());
		}
		Error = RenameNoReplace(Source, Destination);
		if (Error)
			return FailAfterMutation(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Destination,
				"Could not move the planned folder without overwriting a destination: " + Error.message()));
		FolderMoved = true;
		for (size_t Index = 0; Index < StagedFiles.size(); ++Index)
		{
			const AssetEditRewrittenSource& Rewrite = Plan.GetRewrittenSources()[Index];
			if (!IsPathWithin(Plan.GetMapping().Source, Rewrite.ProjectRelativePath)) continue;
			auto BeforeReplace = CaptureReceipt(Plan.GetProjectRoot(), MapPlannedPath(Plan, Rewrite.ProjectRelativePath));
			if (!BeforeReplace || BeforeReplace->ByteLength != Rewrite.Before.ByteLength ||
				BeforeReplace->Content != Rewrite.Before.Content)
				return FailAfterMutation(MakeError(AssetEditTransactionErrorCode::StaleInputs, Rewrite.ProjectRelativePath,
					"Moved glTF/GLB source changed before reference rewrite publication"));
			auto Replaced = Platform::ReplaceFileAtomically(StagedFiles[Index].Staged, StagedFiles[Index].Target);
			if (!Replaced)
				return FailAfterMutation(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, StagedFiles[Index].Target,
					"Could not publish a moved-source reference rewrite: " + Replaced.error().Message));
			PublishedRewrites.push_back(Index);
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterAssetMove, Destination); !Hook)
			return FailAfterMutation(Hook.error());

		AssetRegistry Candidate;
		if (auto Rebuilt = Candidate.Rebuild(Plan.GetProjectRoot()); !Rebuilt)
			return FailAfterMutation(
				MakeError(AssetEditTransactionErrorCode::RegistryInvalid, Destination, "Candidate registry rejected the moved folder"));
		auto ExpectedMembership = Plan.GetRegistryMembership();
		for (AssetRecord& Record : ExpectedMembership)
			if (IsPathWithin(Plan.GetMapping().Source, Record.ProjectRelativePath))
				Record.ProjectRelativePath = MapPlannedPath(Plan, Record.ProjectRelativePath);
		if (!SameMembership(std::move(ExpectedMembership), Candidate.GetAssets()))
			return FailAfterMutation(MakeError(AssetEditTransactionErrorCode::RegistryInvalid, Destination,
				"Post-move registry membership differs from the planned folder mapping"));
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRegistryPublication, Destination); !Hook)
			return FailAfterMutation(Hook.error());
		Registry = std::move(Candidate);
		std::filesystem::remove_all(TransactionDirectory, Error);
		const std::vector<AssetRecord> PublishedAssets = Registry.GetAssets();
		const auto FirstMoved = std::find_if(
			PublishedAssets.begin(), PublishedAssets.end(), [&](const AssetRecord& Record) { return Record.ID == Plan.GetAsset(); });
		return AssetEditTransactionResult{ FirstMoved == PublishedAssets.end() ? AssetRecord{} : *FirstMoved,
			Plan.GetProvenanceDisposition() };
	}

	std::expected<AssetEditTransactionResult, AssetEditTransactionError> AssetEditTransaction::ApplyFolderDuplicate(
		const AssetEditMovePlan& Plan, const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID,
		uint64_t CurrentCatalogGeneration, AssetRegistry& Registry, const AssetEditTransactionOptions& Options)
	{
		using Path = std::filesystem::path;
		if (!Plan.IsFolderDuplicate())
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, Plan.GetMapping().Source,
				"ApplyFolderDuplicate requires a folder-duplicate plan"));
		std::error_code Error;
		const Path NormalizedRoot = std::filesystem::absolute(CurrentProjectRoot, Error).lexically_normal();
		if (Error || NormalizedRoot != Plan.GetProjectRoot() || HasLinkedOrReparseAncestor(NormalizedRoot) ||
			CurrentProjectID != Plan.GetProjectID() || CurrentCatalogGeneration != Plan.GetCatalogGeneration())
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleProject, Plan.GetMapping().Source,
				"Project identity or catalog generation changed after the duplicate preview"));
		if (auto Recovery = RejectIncompleteTransactions(NormalizedRoot); !Recovery) return std::unexpected(Recovery.error());
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());
		AssetRegistry DiskRegistry;
		if (auto Rebuilt = DiskRegistry.Rebuild(Plan.GetProjectRoot()); !Rebuilt)
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::RegistryInvalid, "Assets", "Current registry failed validation before folder duplication"));
		if (!SameMembership(Plan.GetRegistryMembership(), DiskRegistry.GetAssets()) ||
			!SameMembership(Plan.GetRegistryMembership(), Registry.GetAssets()))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
				"Registry membership changed after the duplicate preview"));
		for (const auto& [OriginalID, NewID] : Plan.GetIdentityRemap())
		{
			(void)OriginalID;
			if (NewID.IsNil() || DiskRegistry.Find(NewID))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, Plan.GetMapping().Destination,
					"A planned duplicate identity now conflicts with registry membership"));
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());

		const Path Root = Plan.GetProjectRoot();
		const Path Source = Root / Plan.GetMapping().Source;
		const Path Destination = Root / Plan.GetMapping().Destination;
		if (auto Validated = ValidateManagedPath(Root, Plan.GetMapping().Destination.parent_path(), true, false); !Validated)
			return std::unexpected(Validated.error());
		auto DestinationReceipt = CaptureReceipt(Root, Plan.GetMapping().Destination, true);
		if (!DestinationReceipt || DestinationReceipt->Exists)
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::Conflict, Plan.GetMapping().Destination, "Duplicate destination was created after preview"));

		// Folder membership is part of the plan: a new unregistered file must not silently enter the copy.
		std::vector<Path> CurrentEntries;
		std::filesystem::recursive_directory_iterator Iterator(Source, std::filesystem::directory_options::none, Error);
		const std::filesystem::recursive_directory_iterator End;
		if (Error)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
				"Could not re-enumerate duplicate source folder: " + Error.message()));
		while (Iterator != End)
		{
			const auto Status = Iterator->symlink_status(Error);
			if (Error || IsLinkedOrReparsePath(Iterator->path(), Status))
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Iterator->path(),
					"Folder duplication does not traverse symbolic links or reparse points"));
			CurrentEntries.push_back(Iterator->path().lexically_relative(Root));
			Iterator.increment(Error);
			if (Error)
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
					"Could not finish duplicate source-folder validation: " + Error.message()));
		}
		std::sort(CurrentEntries.begin(), CurrentEntries.end(),
			[](const Path& First, const Path& Second) { return PathKey(First) < PathKey(Second); });
		if (CurrentEntries != Plan.m_FolderEntries)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, Plan.GetMapping().Source,
				"Folder membership changed after the duplicate preview"));
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeFileMutation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());

		const Path TransactionDirectory = Root / (".pf-asset-edit-" + Plan.GetOperationID().ToString());
		const Path CopyRoot = TransactionDirectory / "copy";
		std::filesystem::create_directories(CopyRoot, Error);
		if (Error)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, TransactionDirectory,
				"Could not create the owned duplicate staging directory: " + Error.message()));
		auto CleanupStaging = [&]()
		{
			std::error_code CleanupError;
			std::filesystem::remove_all(TransactionDirectory, CleanupError);
		};
		std::set<Path> ManagedSidecars;
		for (const AssetRecord& Record : Plan.GetRegistryMembership())
			if (IsPathWithin(Plan.GetMapping().Source, Record.ProjectRelativePath))
			{
				ManagedSidecars.insert(AssetMetadataSerializer::GetSidecarPath(Root / Record.ProjectRelativePath).lexically_relative(Root));
			}
		for (const Path& Entry : Plan.m_FolderEntries)
		{
			const Path Relative = Entry.lexically_relative(Plan.GetMapping().Source);
			const Path Staged = CopyRoot / Relative;
			const Path Absolute = Root / Entry;
			const Path Filename = Entry.filename();
			if (Filename.string().ends_with(".importresult") || ManagedSidecars.contains(Entry)) continue;
			const auto Status = std::filesystem::symlink_status(Absolute, Error);
			if (Error || IsLinkedOrReparsePath(Absolute, Status))
			{
				CleanupStaging();
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::StaleInputs, Entry, "Source folder changed or contains a linked path during staging"));
			}
			if (std::filesystem::is_directory(Status)) std::filesystem::create_directories(Staged, Error);
			else if (std::filesystem::is_regular_file(Status))
			{
				std::filesystem::create_directories(Staged.parent_path(), Error);
				if (!Error && !std::filesystem::copy_file(Absolute, Staged, std::filesystem::copy_options::none, Error) && !Error)
					Error = std::make_error_code(std::errc::file_exists);
			}
			else Error = std::make_error_code(std::errc::operation_not_supported);
			if (Error)
			{
				CleanupStaging();
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::FilesystemFailure, Entry, "Could not stage folder contents: " + Error.message()));
			}
		}

		for (const auto& [OriginalID, NewID] : Plan.GetIdentityRemap())
		{
			const auto Original = std::find_if(Plan.GetRegistryMembership().begin(), Plan.GetRegistryMembership().end(),
				[&](const AssetRecord& Record) { return Record.ID == OriginalID; });
			if (Original == Plan.GetRegistryMembership().end())
			{
				CleanupStaging();
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::InvalidPlan, OriginalID.ToString(), "Duplicate plan lost an original registry record"));
			}
			const Path DuplicatePath = CopyRoot / Original->ProjectRelativePath.lexically_relative(Plan.GetMapping().Source);
			auto Metadata = AssetMetadataSerializer::CreateDuplicateForNewAsset(Root / Original->ProjectRelativePath, DuplicatePath, NewID);
			if (!Metadata)
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, DuplicatePath,
					"Could not create planned duplicate metadata: " + Metadata.error().Message));
			}
		}

		for (const AssetEditInternalAssetReference& Reference : Plan.GetInternalAssetReferences())
		{
			const auto NewOwner = std::find_if(Plan.GetIdentityRemap().begin(), Plan.GetIdentityRemap().end(),
				[&](const auto& Pair) { return Pair.first == Reference.ReferringAsset; });
			const auto NewTarget = std::find_if(Plan.GetIdentityRemap().begin(), Plan.GetIdentityRemap().end(),
				[&](const auto& Pair) { return Pair.first == Reference.TargetAsset; });
			if (NewOwner == Plan.GetIdentityRemap().end() || NewTarget == Plan.GetIdentityRemap().end())
			{
				CleanupStaging();
				return std::unexpected(MakeError(
					AssetEditTransactionErrorCode::InvalidPlan, Reference.Location, "Internal reference is missing an identity remap"));
			}
			const auto OwnerRecord = std::find_if(Plan.GetRegistryMembership().begin(), Plan.GetRegistryMembership().end(),
				[&](const AssetRecord& Record) { return Record.ID == Reference.ReferringAsset; });
			if (OwnerRecord == Plan.GetRegistryMembership().end())
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, Reference.ReferringAsset.ToString(),
					"Internal reference owner is missing from the planned registry"));
			}
			const Path DuplicatePath = CopyRoot / OwnerRecord->ProjectRelativePath.lexically_relative(Plan.GetMapping().Source);
			auto Material = MaterialAssetSerializer::LoadFromFile(DuplicatePath);
			if (!Material)
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleInputs, DuplicatePath,
					"Could not read typed material data for internal-reference remapping: " + Material.error().Message));
			}
			if (Reference.Location == "material.baseColorTexture") Material->BaseColorTexture = NewTarget->second;
			else if (Reference.Location == "material.metallicRoughnessTexture") Material->MetallicRoughnessTexture = NewTarget->second;
			else if (Reference.Location == "material.normalTexture") Material->NormalTexture = NewTarget->second;
			else if (Reference.Location == "material.occlusionTexture") Material->OcclusionTexture = NewTarget->second;
			else if (Reference.Location == "material.emissiveTexture") Material->EmissiveTexture = NewTarget->second;
			else
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, Reference.Location,
					"No typed adapter is available for this internal asset reference"));
			}
			auto Saved = MaterialAssetSerializer::SaveToFile(*Material, DuplicatePath);
			if (!Saved)
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, DuplicatePath,
					"Could not save typed material reference remap: " + Saved.error().Message));
			}
		}

		std::vector<StagedEditFile> StagedRewrites;
		for (size_t Index = 0; Index < Plan.GetRewrittenSources().size(); ++Index)
		{
			const AssetEditRewrittenSource& Rewrite = Plan.GetRewrittenSources()[Index];
			auto Staged = StageRewrite(Plan, Rewrite, Index, TransactionDirectory);
			if (!Staged)
			{
				CleanupStaging();
				return std::unexpected(Staged.error());
			}
			const Path DuplicateSource = CopyRoot / Rewrite.ProjectRelativePath.lexically_relative(Plan.GetMapping().Source);
			auto Replaced = Platform::ReplaceFileAtomically(Staged->Staged, DuplicateSource);
			if (!Replaced)
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, DuplicateSource,
					"Could not apply planned glTF/GLB URI rewrite to duplicate: " + Replaced.error().Message));
			}
			StagedRewrites.push_back(std::move(*Staged));
		}
		for (const AssetEditFileReceipt& Receipt : Plan.GetReceipts())
		{
			auto Validated = ValidateReceipt(Root, Receipt);
			if (!Validated)
			{
				CleanupStaging();
				return std::unexpected(Validated.error());
			}
		}
		for (const AssetEditRewrittenSource& Rewrite : Plan.GetRewrittenSources())
		{
			auto Original = ValidateReceipt(Root, Rewrite.Before);
			if (!Original)
			{
				CleanupStaging();
				return std::unexpected(Original.error());
			}
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterReferenceWrite, Destination); !Hook)
		{
			CleanupStaging();
			return std::unexpected(Hook.error());
		}

		std::vector<AssetEditFileReceipt> ExpectedCopiedFiles;
		std::set<Path> ExpectedRelativeFiles;
		for (const Path& Entry : Plan.m_FolderEntries)
		{
			const std::string Filename = Entry.filename().string();
			if (Filename.ends_with(".importresult") || ManagedSidecars.contains(Entry)) continue;
			const Path RelativeToSource = Entry.lexically_relative(Plan.GetMapping().Source);
			const Path StagedRelative = (TransactionDirectory / "copy" / RelativeToSource).lexically_relative(Root);
			const Path FinalRelative = Plan.GetMapping().Destination / RelativeToSource;
			std::error_code StatusError;
			const auto StagedStatus = std::filesystem::symlink_status(Root / StagedRelative, StatusError);
			if (StatusError)
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, StagedRelative,
					"Could not inspect staged duplicate entry: " + StatusError.message()));
			}
			if (std::filesystem::is_directory(StagedStatus))
			{
				ExpectedRelativeFiles.insert(FinalRelative);
				continue;
			}
			if (!std::filesystem::is_regular_file(StagedStatus))
			{
				CleanupStaging();
				return std::unexpected(MakeError(AssetEditTransactionErrorCode::UnsupportedOperation, StagedRelative,
					"Staged duplicate contains an unsupported filesystem object"));
			}
			auto Receipt = CaptureReceipt(Root, StagedRelative);
			if (!Receipt)
			{
				CleanupStaging();
				return std::unexpected(Receipt.error());
			}
			Receipt->ProjectRelativePath = FinalRelative;
			ExpectedRelativeFiles.insert(FinalRelative);
			ExpectedCopiedFiles.push_back(std::move(*Receipt));
		}
		for (const auto& [OriginalID, NewID] : Plan.GetIdentityRemap())
		{
			(void)OriginalID;
			const auto Original = std::find_if(Plan.GetRegistryMembership().begin(), Plan.GetRegistryMembership().end(),
				[&](const AssetRecord& Record) { return Record.ID == OriginalID; });
			if (Original == Plan.GetRegistryMembership().end()) continue;
			const Path FinalMetadata = AssetMetadataSerializer::GetSidecarPath(
				Root / (Plan.GetMapping().Destination / Original->ProjectRelativePath.lexically_relative(Plan.GetMapping().Source)))
										   .lexically_relative(Root);
			auto Receipt = CaptureReceipt(Root,
				(TransactionDirectory / "copy" /
					AssetMetadataSerializer::GetSidecarPath(Original->ProjectRelativePath.lexically_relative(Plan.GetMapping().Source))));
			if (!Receipt)
			{
				CleanupStaging();
				return std::unexpected(Receipt.error());
			}
			Receipt->ProjectRelativePath = FinalMetadata;
			ExpectedRelativeFiles.insert(FinalMetadata);
			ExpectedCopiedFiles.push_back(std::move(*Receipt));
		}

		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeFileMutation, Destination); !Hook)
		{
			CleanupStaging();
			return std::unexpected(Hook.error());
		}
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs)
		{
			CleanupStaging();
			return std::unexpected(Inputs.error());
		}
		Error = RenameNoReplace(CopyRoot, Destination);
		if (Error)
		{
			CleanupStaging();
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Destination,
				"Could not publish duplicate folder without overwriting: " + Error.message()));
		}
		auto RollbackPublishedCopy = [&]() -> std::string
		{
			std::set<Path> ActualRelativeFiles;
			std::error_code EnumerateError;
			for (std::filesystem::recursive_directory_iterator
					 Current(Destination, std::filesystem::directory_options::none, EnumerateError),
				End;
				!EnumerateError && Current != End; Current.increment(EnumerateError))
			{
				const auto Status = Current->symlink_status(EnumerateError);
				if (EnumerateError || IsLinkedOrReparsePath(Current->path(), Status)) break;
				if (std::filesystem::is_regular_file(Status) || std::filesystem::is_directory(Status))
					ActualRelativeFiles.insert(Current->path().lexically_relative(Root));
				else break;
			}
			if (EnumerateError || ActualRelativeFiles != ExpectedRelativeFiles)
				return "published duplicate changed externally; retained at " + PathKey(Destination);
			for (const AssetEditFileReceipt& Receipt : ExpectedCopiedFiles)
				if (!ValidateReceipt(Root, Receipt))
					return "published duplicate file changed externally; retained at " + PathKey(Destination);
			Error = RenameNoReplace(Destination, CopyRoot);
			if (Error) return "could not return duplicate to owned staging: " + Error.message();
			return {};
		};
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterAssetMove, Destination); !Hook)
		{
			const std::string Failure = RollbackPublishedCopy();
			if (!Failure.empty())
			{
				auto ErrorValue = Hook.error();
				ErrorValue.Code = AssetEditTransactionErrorCode::RecoveryRequired;
				ErrorValue.Message += "; " + Failure;
				ErrorValue.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(ErrorValue));
			}
			CleanupStaging();
			return std::unexpected(Hook.error());
		}

		AssetRegistry Candidate;
		if (auto Rebuilt = Candidate.Rebuild(Root); !Rebuilt)
		{
			const std::string Failure = RollbackPublishedCopy();
			if (!Failure.empty())
			{
				auto FailureValue = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Destination,
					"Candidate registry failed and duplicate rollback was incomplete: " + Failure);
				FailureValue.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(FailureValue));
			}
			CleanupStaging();
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::RegistryInvalid, Destination, "Candidate registry rejected the duplicated folder"));
		}
		std::vector<AssetRecord> ExpectedMembership = Plan.GetRegistryMembership();
		for (const auto& [OriginalID, NewID] : Plan.GetIdentityRemap())
		{
			const auto Original = std::find_if(Plan.GetRegistryMembership().begin(), Plan.GetRegistryMembership().end(),
				[&](const AssetRecord& Record) { return Record.ID == OriginalID; });
			if (Original == Plan.GetRegistryMembership().end()) continue;
			ExpectedMembership.push_back(
				{ NewID, Plan.GetMapping().Destination / Original->ProjectRelativePath.lexically_relative(Plan.GetMapping().Source) });
		}
		if (!SameMembership(std::move(ExpectedMembership), Candidate.GetAssets()))
		{
			const std::string Failure = RollbackPublishedCopy();
			if (!Failure.empty())
			{
				auto FailureValue = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Destination,
					"Candidate membership differs from plan and rollback was incomplete: " + Failure);
				FailureValue.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(FailureValue));
			}
			CleanupStaging();
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::RegistryInvalid, Destination,
				"Post-duplicate registry membership differs from the planned identity map"));
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRegistryPublication, Destination); !Hook)
		{
			const std::string Failure = RollbackPublishedCopy();
			if (!Failure.empty())
			{
				auto ErrorValue = Hook.error();
				ErrorValue.Code = AssetEditTransactionErrorCode::RecoveryRequired;
				ErrorValue.Message += "; " + Failure;
				ErrorValue.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(ErrorValue));
			}
			CleanupStaging();
			return std::unexpected(Hook.error());
		}
		Registry = std::move(Candidate);
		std::filesystem::remove_all(TransactionDirectory, Error);
		AssetEditTransactionResult Result;
		Result.ProvenanceDisposition = AssetEditProvenanceDisposition::NoReceipt;
		for (const auto& [OriginalID, NewID] : Plan.GetIdentityRemap())
		{
			(void)OriginalID;
			if (auto Created = Registry.Find(NewID)) Result.CreatedAssets.push_back(*Created);
		}
		if (!Result.CreatedAssets.empty()) Result.MovedAsset = Result.CreatedAssets.front();
		return Result;
	}

	std::expected<AssetEditTransactionResult, AssetEditTransactionError> AssetEditTransaction::ApplyAssetDuplicate(
		const AssetEditMovePlan& Plan, const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID,
		uint64_t CurrentCatalogGeneration, AssetRegistry& Registry, const AssetEditTransactionOptions& Options)
	{
		if (!Plan.IsAssetDuplicate() || Plan.GetIdentityRemap().size() != 1)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::InvalidPlan, Plan.GetMapping().Source,
				"ApplyAssetDuplicate requires a single-asset duplicate plan"));
		std::error_code Error;
		const Path NormalizedRoot = std::filesystem::absolute(CurrentProjectRoot, Error).lexically_normal();
		if (Error || NormalizedRoot != Plan.GetProjectRoot() || HasLinkedOrReparseAncestor(NormalizedRoot) ||
			CurrentProjectID != Plan.GetProjectID() || CurrentCatalogGeneration != Plan.GetCatalogGeneration())
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::StaleProject, Plan.GetMapping().Source,
				"Project identity, root, or catalog generation changed after duplicate preview"));
		if (auto Recovery = RejectIncompleteTransactions(NormalizedRoot); !Recovery) return std::unexpected(Recovery.error());
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());
		AssetRegistry DiskRegistry;
		if (auto Rebuilt = DiskRegistry.Rebuild(Plan.GetProjectRoot()); !Rebuilt)
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::RegistryInvalid, "Assets", "Current registry failed validation before asset duplication"));
		if (!SameMembership(Plan.GetRegistryMembership(), DiskRegistry.GetAssets()) ||
			!SameMembership(Plan.GetRegistryMembership(), Registry.GetAssets()))
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::StaleInputs, "Assets", "Registry membership changed after the duplicate preview"));
		const auto [OriginalID, DuplicateID] = Plan.GetIdentityRemap().front();
		if (OriginalID != Plan.GetAsset() || DuplicateID.IsNil() || DiskRegistry.Find(DuplicateID))
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::Conflict, Plan.GetMapping().Destination,
				"Planned duplicate identity is invalid or already registered"));
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterRevalidation, Plan.GetMapping().Source); !Hook)
			return std::unexpected(Hook.error());
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeFileMutation, Plan.GetMapping().Destination); !Hook)
			return std::unexpected(Hook.error());
		if (auto Inputs = ValidatePlanInputs(Plan); !Inputs) return std::unexpected(Inputs.error());

		const Path Root = Plan.GetProjectRoot();
		const Path Source = Root / Plan.GetMapping().Source;
		const Path Destination = Root / Plan.GetMapping().Destination;
		const Path DestinationMetadata = AssetMetadataSerializer::GetSidecarPath(Destination);
		const Path TransactionDirectory = Root / (".pf-asset-edit-" + Plan.GetOperationID().ToString());
		if (!std::filesystem::create_directory(TransactionDirectory, Error) || Error)
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, TransactionDirectory,
				"Could not create an exclusive asset-duplicate staging directory: " + Error.message()));
		Path StagedSource = TransactionDirectory / "duplicate-source.stage";
		const AssetEditRewrittenSource* Rewrite = nullptr;
		for (const AssetEditRewrittenSource& Candidate : Plan.GetRewrittenSources())
			if (Candidate.Asset == OriginalID)
			{
				Rewrite = &Candidate;
				break;
			}
		if (Rewrite)
		{
			auto Staged = StageRewrite(Plan, *Rewrite, 0, TransactionDirectory);
			if (!Staged)
			{
				std::filesystem::remove_all(TransactionDirectory, Error);
				return std::unexpected(Staged.error());
			}
			StagedSource = Staged->Staged;
		}
		else if (!std::filesystem::copy_file(Source, StagedSource, std::filesystem::copy_options::none, Error) || Error)
		{
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(
				AssetEditTransactionErrorCode::FilesystemFailure, Source, "Could not stage duplicate source bytes: " + Error.message()));
		}
		const Path StagedMetadata = AssetMetadataSerializer::GetSidecarPath(StagedSource);
		if (auto Freshness = ValidatePlanInputs(Plan); !Freshness)
		{
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(Freshness.error());
		}
		auto Metadata = AssetMetadataSerializer::CreateDuplicateForNewAsset(Source, StagedSource, DuplicateID);
		if (!Metadata)
		{
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, StagedMetadata,
				"Could not create duplicate metadata with its planned identity: " + Metadata.error().Message));
		}
		auto ExpectedSource = CaptureReceipt(Root, StagedSource.lexically_relative(Root));
		auto ExpectedMetadata = CaptureReceipt(Root, StagedMetadata.lexically_relative(Root));
		if (!ExpectedSource || !ExpectedMetadata)
		{
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(!ExpectedSource ? ExpectedSource.error() : ExpectedMetadata.error());
		}

		auto RollbackPublished = [&]() -> std::string
		{
			for (const auto& [PathToRemove, Expected] :
				{ std::pair{ DestinationMetadata, *ExpectedMetadata }, std::pair{ Destination, *ExpectedSource } })
			{
				std::error_code StatusError;
				const auto Status = std::filesystem::symlink_status(PathToRemove, StatusError);
				if (StatusError == std::errc::no_such_file_or_directory ||
					(!StatusError && Status.type() == std::filesystem::file_type::not_found))
					continue;
				if (StatusError || IsLinkedOrReparsePath(PathToRemove, Status))
					return "published duplicate path is inaccessible or linked; retained at " + PathKey(PathToRemove);
				auto Current = CaptureReceipt(Root, PathToRemove.lexically_relative(Root));
				if (!Current || Current->ByteLength != Expected.ByteLength || Current->Content != Expected.Content)
					return "published duplicate changed externally; retained at " + PathKey(PathToRemove);
				Error = RenameNoReplace(PathToRemove, PathToRemove == Destination ? StagedSource : StagedMetadata);
				if (Error) return "could not return published duplicate to staging: " + Error.message();
			}
			return {};
		};
		Error = RenameNoReplace(StagedSource, Destination);
		if (Error)
		{
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, Destination,
				"Could not publish duplicate source without overwriting: " + Error.message()));
		}
		Error = RenameNoReplace(StagedMetadata, DestinationMetadata);
		if (Error)
		{
			const std::string Failure = Error.message();
			const std::string Rollback = RollbackPublished();
			if (!Rollback.empty())
			{
				auto Failure = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Destination,
					"Duplicate metadata publication failed and rollback needs recovery: " + Rollback);
				Failure.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(Failure));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::FilesystemFailure, DestinationMetadata,
				"Could not publish duplicate metadata without overwriting: " + Failure));
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::AfterAssetMove, Destination); !Hook)
		{
			const std::string Rollback = RollbackPublished();
			if (!Rollback.empty())
			{
				auto Failure = Hook.error();
				Failure.Code = AssetEditTransactionErrorCode::RecoveryRequired;
				Failure.Message += "; rollback needs recovery: " + Rollback;
				Failure.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(Failure));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(Hook.error());
		}

		AssetRegistry Candidate;
		if (auto Rebuilt = Candidate.Rebuild(Root); !Rebuilt)
		{
			const std::string Rollback = RollbackPublished();
			if (!Rollback.empty())
			{
				auto Failure = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Destination,
					"Candidate registry rejected the duplicate and rollback needs recovery: " + Rollback);
				Failure.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(Failure));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(
				MakeError(AssetEditTransactionErrorCode::RegistryInvalid, Destination, "Candidate registry rejected the duplicated asset"));
		}
		std::vector<AssetRecord> ExpectedMembership = Plan.GetRegistryMembership();
		ExpectedMembership.push_back({ DuplicateID, Plan.GetMapping().Destination });
		if (!SameMembership(std::move(ExpectedMembership), Candidate.GetAssets()))
		{
			const std::string Rollback = RollbackPublished();
			if (!Rollback.empty())
			{
				auto Failure = MakeError(AssetEditTransactionErrorCode::RecoveryRequired, Destination,
					"Candidate membership differs from the duplicate plan and rollback needs recovery: " + Rollback);
				Failure.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(Failure));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(MakeError(AssetEditTransactionErrorCode::RegistryInvalid, Destination,
				"Post-duplicate registry membership differs from the planned identity mapping"));
		}
		if (auto Hook = InvokeHook(Options, AssetEditTransactionStage::BeforeRegistryPublication, Destination); !Hook)
		{
			const std::string Rollback = RollbackPublished();
			if (!Rollback.empty())
			{
				auto Failure = Hook.error();
				Failure.Code = AssetEditTransactionErrorCode::RecoveryRequired;
				Failure.Message += "; rollback needs recovery: " + Rollback;
				Failure.RecoveryPath = TransactionDirectory;
				return std::unexpected(std::move(Failure));
			}
			std::filesystem::remove_all(TransactionDirectory, Error);
			return std::unexpected(Hook.error());
		}
		Registry = std::move(Candidate);
		std::filesystem::remove_all(TransactionDirectory, Error);
		AssetEditTransactionResult Result;
		Result.ProvenanceDisposition = AssetEditProvenanceDisposition::NoReceipt;
		if (auto Created = Registry.Find(DuplicateID))
		{
			Result.MovedAsset = *Created;
			Result.CreatedAssets.push_back(*Created);
		}
		return Result;
	}
} // namespace PulseForge
