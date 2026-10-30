#include "Assets/PrefabAssetService.h"

#include "Assets/AssetOperations.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Assets/PrefabSerializer.h"

#include <array>
#include <fstream>
#include <limits>
#include <span>

namespace PulseForge
{
	namespace
	{
		std::expected<std::filesystem::path, PrefabAssetError> ResolvePrefabPath(
			const AssetID& PrefabAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry)
		{
			const auto Record = Registry.Find(PrefabAsset);
			if (!Record)
			{
				return std::unexpected(PrefabAssetError{
					PrefabAssetErrorCode::AssetNotFound,
					PrefabAsset,
					"Prefab asset UUID " + PrefabAsset.ToString() + " is not present in the project asset registry" });
			}

			if (!HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Prefab))
			{
				return std::unexpected(PrefabAssetError{
					PrefabAssetErrorCode::UnsupportedAssetType,
					PrefabAsset,
					"Asset UUID " + PrefabAsset.ToString() + " does not refer to a .prefab file" });
			}

			auto ResolvedPath = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!ResolvedPath)
			{
				return std::unexpected(PrefabAssetError{
					ResolvedPath.error().Code == AssetPathErrorCode::MissingAssetEntry
						? PrefabAssetErrorCode::SourceFileMissing
						: PrefabAssetErrorCode::PathResolutionFailed,
					PrefabAsset,
					ResolvedPath.error().Message });
			}
			return *ResolvedPath;
		}

		PrefabAssetError MakeAssetError(PrefabAssetErrorCode Code, AssetID Asset, std::string Message)
		{
			return { Code, Asset, std::move(Message) };
		}

		std::string FingerprintText(const AssetContentFingerprint& Fingerprint)
		{
			return Fingerprint.Algorithm + ":" + Fingerprint.DigestHex;
		}

		std::expected<std::string, PrefabAssetError> ReadPrefabDocument(const AssetID& Asset, const std::filesystem::path& Path)
		{
			constexpr size_t MaximumPrefabBytes = 64u * 1024u * 1024u;
			try
			{
				std::error_code StatusError;
				const std::filesystem::file_status Status = std::filesystem::status(Path, StatusError);
				if (StatusError)
					return std::unexpected(MakeAssetError(PrefabAssetErrorCode::PathResolutionFailed, Asset,
						"Could not inspect prefab source path: " + StatusError.message()));
				if (!std::filesystem::exists(Status))
					return std::unexpected(MakeAssetError(PrefabAssetErrorCode::SourceFileMissing, Asset,
						"Prefab source file is missing; the materialized linked instance was preserved"));
				if (!std::filesystem::is_regular_file(Status))
					return std::unexpected(MakeAssetError(PrefabAssetErrorCode::PathResolutionFailed, Asset,
						"Prefab source path does not name a regular file"));
				std::ifstream Input(Path, std::ios::binary);
				if (!Input.is_open())
					return std::unexpected(MakeAssetError(PrefabAssetErrorCode::PathResolutionFailed, Asset,
						"Could not open prefab source file for reading"));
				std::string Data;
				Data.reserve(4096);
				std::array<char, 8192> Buffer{};
				while (Input)
				{
					Input.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
					const std::streamsize ReadCount = Input.gcount();
					if (ReadCount > 0)
					{
						const size_t Count = static_cast<size_t>(ReadCount);
						if (Count > MaximumPrefabBytes - Data.size())
							return std::unexpected(MakeAssetError(PrefabAssetErrorCode::SerializationFailed, Asset,
								"Prefab document exceeds the supported 64 MiB size limit"));
						Data.append(Buffer.data(), Count);
					}
				}
				if (!Input.eof())
					return std::unexpected(MakeAssetError(PrefabAssetErrorCode::PathResolutionFailed, Asset,
						"Could not read the complete prefab source file"));
				return Data;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeAssetError(
					PrefabAssetErrorCode::PathResolutionFailed, Asset, std::string("Prefab source read failed: ") + Exception.what()));
			}
		}
	}

	std::expected<AssetRecord, PrefabAssetError> PrefabAssetService::Create(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& ProjectRelativePath,
		const Scene& Source,
		const Entity& Root)
	{
		if (!HasAssetTypeExtension(ProjectRelativePath, AssetFileType::Prefab))
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::UnsupportedAssetType,
				{},
				"New prefab assets must use the .prefab extension" });
		}

		const auto Serialized = PrefabSerializer::Serialize(Source, Root);
		if (!Serialized)
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::SerializationFailed,
				{},
				Serialized.error().Message });
		}

		const std::span<const char> Characters(Serialized->data(), Serialized->size());
		const auto Created = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			std::as_bytes(Characters),
			ProjectRelativePath);
		if (!Created)
		{
			PrefabAssetError Error{
				Created.error().Code == AssetOperationErrorCode::CommittedWithCleanupFailure
					? PrefabAssetErrorCode::CommittedWithCleanupFailure
					: PrefabAssetErrorCode::AssetOperationFailed,
				{},
				Created.error().Message };
			Error.CommittedAsset = Created.error().CommittedAsset;
			Error.RecoveryPath = Created.error().RecoveryPath;
			if (Error.CommittedAsset)
				Error.Asset = Error.CommittedAsset->ID;
			return std::unexpected(std::move(Error));
		}
		return *Created;
	}

	std::expected<Entity, PrefabAssetError> PrefabAssetService::Instantiate(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		Scene& Destination)
	{
		auto Path = ResolvePrefabPath(PrefabAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));

		auto Result = PrefabSerializer::InstantiateFromFile(*Path, Destination);
		if (!Result)
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::SerializationFailed,
				PrefabAsset,
				Result.error().Message });
		}
		return *Result;
	}

	std::expected<void, PrefabAssetError> PrefabAssetService::Save(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		const Scene& Source,
		const Entity& Root)
	{
		auto Path = ResolvePrefabPath(PrefabAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));

		if (auto Result = PrefabSerializer::SaveToFile(Source, Root, *Path); !Result)
		{
			return std::unexpected(PrefabAssetError{
				PrefabAssetErrorCode::SerializationFailed,
				PrefabAsset,
				Result.error().Message });
		}
		return {};
	}

	std::expected<PrefabSourceDocument, PrefabAssetError> PrefabAssetService::LoadSource(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry)
	{
		auto Path = ResolvePrefabPath(PrefabAsset, ProjectRoot, Registry);
		if (!Path) return std::unexpected(std::move(Path.error()));
		auto Data = ReadPrefabDocument(PrefabAsset, *Path);
		if (!Data) return std::unexpected(std::move(Data.error()));

		const auto Bytes = std::as_bytes(std::span(Data->data(), Data->size()));
		const std::string Fingerprint = FingerprintText(ComputeAssetContentFingerprint(Bytes));
		auto Source = PrefabSerializer::Parse(*Data, PrefabAsset, Fingerprint);
		if (!Source)
			return std::unexpected(MakeAssetError(PrefabAssetErrorCode::SerializationFailed, PrefabAsset, Source.error().Message));
		return std::move(*Source);
	}

	std::expected<void, PrefabAssetError> PrefabAssetService::SaveSource(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		PrefabSourceDocument& Source)
	{
		if (Source.GetSourceAsset() != PrefabAsset)
			return std::unexpected(MakeAssetError(
				PrefabAssetErrorCode::SerializationFailed, PrefabAsset, "Prefab source draft belongs to a different asset"));
		auto Path = ResolvePrefabPath(PrefabAsset, ProjectRoot, Registry);
		if (!Path) return std::unexpected(std::move(Path.error()));
		auto ExistingData = ReadPrefabDocument(PrefabAsset, *Path);
		if (!ExistingData) return std::unexpected(std::move(ExistingData.error()));
		const auto ExistingBytes = std::as_bytes(std::span(ExistingData->data(), ExistingData->size()));
		const std::string ExistingFingerprint = FingerprintText(ComputeAssetContentFingerprint(ExistingBytes));
		if (Source.GetFingerprint().empty() || Source.GetFingerprint() != ExistingFingerprint)
			return std::unexpected(MakeAssetError(
				PrefabAssetErrorCode::StaleSource, PrefabAsset,
				"Prefab source changed after this draft was loaded; reload or merge before saving"));
		const auto Serialized = PrefabSerializer::Serialize(Source);
		if (!Serialized)
			return std::unexpected(MakeAssetError(PrefabAssetErrorCode::SerializationFailed, PrefabAsset, Serialized.error().Message));
		const auto Root = Source.GetScene().FindEntity(Source.GetRootKey());
		if (!Root)
			return std::unexpected(MakeAssetError(PrefabAssetErrorCode::SerializationFailed, PrefabAsset, "Prefab source root no longer exists"));
		if (auto SaveResult = PrefabSerializer::SaveToFile(Source.GetScene(), *Root, *Path); !SaveResult)
			return std::unexpected(MakeAssetError(PrefabAssetErrorCode::SerializationFailed, PrefabAsset, SaveResult.error().Message));

		const auto Bytes = std::as_bytes(std::span(Serialized->data(), Serialized->size()));
		Source.m_Fingerprint = FingerprintText(ComputeAssetContentFingerprint(Bytes));
		Source.m_OriginalData = *Serialized;
		return {};
	}

	std::expected<Entity, PrefabAssetError> PrefabAssetService::InstantiateLinked(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		Scene& Destination,
		const PrefabPlacement& Placement)
	{
		auto Source = LoadSource(PrefabAsset, ProjectRoot, Registry);
		if (!Source) return std::unexpected(std::move(Source.error()));
		auto Result = PrefabInstanceReconciler::InstantiateLinked(*Source, Destination, Placement);
		if (!Result)
		{
			return std::unexpected(MakeAssetError(
				PrefabAssetErrorCode::ReconciliationFailed, PrefabAsset, Result.error().Message));
		}
		return Result->Root;
	}

	std::expected<PrefabReconcilePlan, PrefabAssetError> PrefabAssetService::PlanReconcile(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		const Scene& Destination,
		const Entity& LinkedRoot)
	{
		auto Source = LoadSource(PrefabAsset, ProjectRoot, Registry);
		if (!Source) return std::unexpected(std::move(Source.error()));
		auto Plan = PrefabInstanceReconciler::PlanReconcile(Destination, LinkedRoot, *Source);
		if (!Plan)
		{
			PrefabAssetError Error = MakeAssetError(
				PrefabAssetErrorCode::ReconciliationFailed, PrefabAsset, Plan.error().Message);
			Error.Conflicts = Plan.error().Conflicts;
			return std::unexpected(std::move(Error));
		}
		return std::move(*Plan);
	}

	std::expected<PrefabReconcileSummary, PrefabAssetError> PrefabAssetService::ApplyReconcile(
		const AssetID& PrefabAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		Scene& Destination,
		const PrefabReconcilePlan& Plan)
	{
		if (Plan.GetSourceAsset() != PrefabAsset)
			return std::unexpected(MakeAssetError(
				PrefabAssetErrorCode::StaleReconcilePlan, PrefabAsset, "Reconcile plan belongs to a different prefab asset"));
		auto Source = LoadSource(PrefabAsset, ProjectRoot, Registry);
		if (!Source) return std::unexpected(std::move(Source.error()));
		if (Source->GetFingerprint() != Plan.GetSourceFingerprint())
			return std::unexpected(MakeAssetError(
				PrefabAssetErrorCode::StaleReconcilePlan, PrefabAsset, "Prefab source file changed after the reconcile plan was built"));
		const auto Serialized = PrefabSerializer::Serialize(*Source);
		if (!Serialized)
			return std::unexpected(MakeAssetError(PrefabAssetErrorCode::SerializationFailed, PrefabAsset, Serialized.error().Message));
		const auto SnapshotBytes = std::as_bytes(std::span(Serialized->data(), Serialized->size()));
		if (FingerprintText(ComputeAssetContentFingerprint(SnapshotBytes)) != Plan.GetSourceSnapshotFingerprint())
			return std::unexpected(MakeAssetError(
				PrefabAssetErrorCode::StaleReconcilePlan, PrefabAsset, "Prefab source snapshot differs from the planned source"));

		auto Result = PrefabInstanceReconciler::ApplyReconcile(Destination, *Source, Plan);
		if (!Result)
		{
			PrefabAssetError Error = MakeAssetError(
				Result.error().Code == PrefabReconcileErrorCode::StalePlan
					? PrefabAssetErrorCode::StaleReconcilePlan
					: Result.error().Code == PrefabReconcileErrorCode::ConflictsDetected
						? PrefabAssetErrorCode::ReconcileConflicts
						: PrefabAssetErrorCode::ReconciliationFailed,
				PrefabAsset,
				Result.error().Message);
			Error.Conflicts = Result.error().Conflicts;
			return std::unexpected(std::move(Error));
		}
		return *Result;
	}
}
