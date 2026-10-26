#pragma once

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Detail/GltfRelocation.h"
#include "Core/Core.h"

#include <expected>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace PulseForge
{
	enum class AssetEditTransactionErrorCode : uint8_t
	{
		InvalidPlan,
		UnsupportedOperation,
		StaleProject,
		StaleInputs,
		RegistryInvalid,
		Conflict,
		FilesystemFailure,
		RecoveryRequired
	};

	struct AssetEditTransactionError
	{
		AssetEditTransactionErrorCode Code = AssetEditTransactionErrorCode::InvalidPlan;
		std::filesystem::path Path;
		std::string Message;
		std::optional<std::filesystem::path> RecoveryPath;
	};

	struct AssetEditFileReceipt
	{
		std::filesystem::path ProjectRelativePath;
		bool Exists = false;
		uint64_t ByteLength = 0;
		AssetContentFingerprint Content;
		friend bool operator==(const AssetEditFileReceipt&, const AssetEditFileReceipt&) = default;
	};

	struct AssetEditPathMapping
	{
		std::filesystem::path Source;
		std::filesystem::path Destination;
	};

	struct AssetEditGltfBufferReference
	{
		AssetID ReferringAsset;
		std::filesystem::path ReferringSource;
		std::filesystem::path TargetSource;
		std::filesystem::path NewTargetSource;
		std::string JsonLocation;
		std::string OriginalUri;
	};

	struct AssetEditInternalAssetReference
	{
		AssetID ReferringAsset;
		AssetID TargetAsset;
		std::string Location;
		std::string OriginalReference;
	};

	struct AssetEditRewrittenSource
	{
		AssetID Asset;
		std::filesystem::path ProjectRelativePath;
		AssetEditFileReceipt Before;
		std::string Contents;
		std::vector<AssetEditGltfBufferReference> References;
		bool IsGlb = false;
		std::vector<Assets::Detail::GltfBufferPathMapping> BufferMappings;
		std::filesystem::path NewOwnerPath;
	};

	enum class AssetEditProvenanceDisposition : uint8_t
	{
		NoReceipt,
		PreserveHistoricalReceiptAtNewPath
	};

	class PULSEFORGE_API AssetEditMovePlan final
	{
	public:
		[[nodiscard]] const UUID& GetOperationID() const noexcept { return m_OperationID; }
		[[nodiscard]] const UUID& GetProjectID() const noexcept { return m_ProjectID; }
		[[nodiscard]] const std::filesystem::path& GetProjectRoot() const noexcept { return m_ProjectRoot; }
		[[nodiscard]] uint64_t GetCatalogGeneration() const noexcept { return m_CatalogGeneration; }
		[[nodiscard]] const AssetID& GetAsset() const noexcept { return m_Asset; }
		[[nodiscard]] const AssetEditPathMapping& GetMapping() const noexcept { return m_Mapping; }
		[[nodiscard]] const std::vector<AssetRecord>& GetRegistryMembership() const noexcept { return m_RegistryMembership; }
		[[nodiscard]] const std::vector<AssetID>& GetMovedAssets() const noexcept { return m_MovedAssets; }
		[[nodiscard]] const std::vector<std::pair<AssetID, AssetID>>& GetIdentityRemap() const noexcept { return m_IdentityRemap; }
		[[nodiscard]] const std::vector<AssetEditPathMapping>& GetPathMappings() const noexcept { return m_PathMappings; }
		[[nodiscard]] const std::vector<AssetDependencyInputReceipt>& GetCatalogReceipts() const noexcept { return m_CatalogReceipts; }
		[[nodiscard]] const std::vector<AssetEditFileReceipt>& GetReceipts() const noexcept { return m_Receipts; }
		[[nodiscard]] const std::vector<AssetEditGltfBufferReference>& GetAffectedReferences() const noexcept
		{
			return m_AffectedReferences;
		}
		[[nodiscard]] const std::vector<AssetEditInternalAssetReference>& GetInternalAssetReferences() const noexcept
		{
			return m_InternalAssetReferences;
		}
		[[nodiscard]] const std::vector<AssetEditRewrittenSource>& GetRewrittenSources() const noexcept { return m_RewrittenSources; }
		[[nodiscard]] AssetEditProvenanceDisposition GetProvenanceDisposition() const noexcept { return m_ProvenanceDisposition; }
		[[nodiscard]] bool IsCaseOnlyAlias() const noexcept { return m_CaseOnlyAlias; }
		[[nodiscard]] bool IsFolderMove() const noexcept { return m_IsFolderMove; }
		[[nodiscard]] bool IsFolderDuplicate() const noexcept { return m_IsFolderDuplicate; }
		[[nodiscard]] bool IsAssetDuplicate() const noexcept { return m_IsAssetDuplicate; }

	private:
		friend class AssetEditTransaction;
		UUID m_OperationID;
		UUID m_ProjectID;
		std::filesystem::path m_ProjectRoot;
		uint64_t m_CatalogGeneration = 0;
		AssetID m_Asset;
		AssetEditPathMapping m_Mapping;
		std::vector<AssetRecord> m_RegistryMembership;
		std::vector<AssetID> m_MovedAssets;
		std::vector<std::pair<AssetID, AssetID>> m_IdentityRemap;
		std::vector<AssetEditPathMapping> m_PathMappings;
		std::vector<std::filesystem::path> m_FolderEntries;
		std::vector<AssetDependencyInputReceipt> m_CatalogReceipts;
		std::vector<AssetEditFileReceipt> m_Receipts;
		std::vector<AssetEditGltfBufferReference> m_AffectedReferences;
		std::vector<AssetEditInternalAssetReference> m_InternalAssetReferences;
		std::vector<AssetEditRewrittenSource> m_RewrittenSources;
		AssetEditProvenanceDisposition m_ProvenanceDisposition = AssetEditProvenanceDisposition::NoReceipt;
		bool m_CaseOnlyAlias = false;
		bool m_IsFolderMove = false;
		bool m_IsFolderDuplicate = false;
		bool m_IsAssetDuplicate = false;
	};

	enum class AssetEditTransactionStage : uint8_t
	{
		BeforeRevalidation,
		AfterRevalidation,
		BeforeFileMutation,
		AfterReferenceWrite,
		AfterAssetMove,
		BeforeRegistryPublication
	};

	struct AssetEditTransactionOptions
	{
		// Deterministic fault/mutation seam. Callbacks run outside internal locks; returning an error aborts the operation.
		std::function<std::expected<void, std::string>(AssetEditTransactionStage, const std::filesystem::path&)> StageHook;
	};

	struct AssetEditTransactionResult
	{
		AssetRecord MovedAsset;
		AssetEditProvenanceDisposition ProvenanceDisposition = AssetEditProvenanceDisposition::NoReceipt;
		std::vector<AssetRecord> CreatedAssets;
	};

	class PULSEFORGE_API AssetEditTransaction final
	{
	public:
		using PlanPtr = std::shared_ptr<const AssetEditMovePlan>;

		[[nodiscard]] static std::expected<PlanPtr, AssetEditTransactionError> PlanMove(
			const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Asset, const std::filesystem::path& DestinationProjectRelative);

		[[nodiscard]] static std::expected<PlanPtr, AssetEditTransactionError> PlanMoveFolder(
			const AssetDependencyCatalogSnapshot& Snapshot, const std::filesystem::path& SourceFolderProjectRelative,
			const std::filesystem::path& DestinationFolderProjectRelative);

		[[nodiscard]] static std::expected<PlanPtr, AssetEditTransactionError> PlanDuplicateFolder(
			const AssetDependencyCatalogSnapshot& Snapshot, const std::filesystem::path& SourceFolderProjectRelative,
			const std::filesystem::path& DestinationFolderProjectRelative);
		[[nodiscard]] static std::expected<PlanPtr, AssetEditTransactionError> PlanDuplicateAsset(
			const AssetDependencyCatalogSnapshot& Snapshot, const AssetID& Asset, const std::filesystem::path& DestinationProjectRelative);

		[[nodiscard]] static std::expected<AssetEditTransactionResult, AssetEditTransactionError> ApplyMove(const AssetEditMovePlan& Plan,
			const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID, uint64_t CurrentCatalogGeneration,
			AssetRegistry& Registry, const AssetEditTransactionOptions& Options = {});

		[[nodiscard]] static std::expected<AssetEditTransactionResult, AssetEditTransactionError> ApplyFolderMove(
			const AssetEditMovePlan& Plan, const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID,
			uint64_t CurrentCatalogGeneration, AssetRegistry& Registry, const AssetEditTransactionOptions& Options = {});

		[[nodiscard]] static std::expected<AssetEditTransactionResult, AssetEditTransactionError> ApplyFolderDuplicate(
			const AssetEditMovePlan& Plan, const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID,
			uint64_t CurrentCatalogGeneration, AssetRegistry& Registry, const AssetEditTransactionOptions& Options = {});
		[[nodiscard]] static std::expected<AssetEditTransactionResult, AssetEditTransactionError> ApplyAssetDuplicate(
			const AssetEditMovePlan& Plan, const std::filesystem::path& CurrentProjectRoot, const UUID& CurrentProjectID,
			uint64_t CurrentCatalogGeneration, AssetRegistry& Registry, const AssetEditTransactionOptions& Options = {});
	};
} // namespace PulseForge
