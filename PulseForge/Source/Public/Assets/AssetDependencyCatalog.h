#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/AssetDependencyTypes.h"
#include "Assets/AssetImportProvenance.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace PulseForge
{
	enum class AssetDependencyEdgeKind : uint8_t
	{
		ImportInput,
		RuntimeReference
	};

	enum class AssetDependencyStatus : uint8_t
	{
		Resolved,
		Missing,
		WrongType,
		Unsupported,
		Invalid
	};

	// A managed source or imported subresource is identified by its sidecar UUID. A source-file node has no UUID and
	// uses a project-relative UTF-8 path. Subresource keys are source-local locators, never display names.
	struct AssetDependencyNode
	{
		AssetID Asset;
		std::string SourceFileKey;
		std::string SubresourceKey;

		[[nodiscard]] bool IsManagedAsset() const noexcept { return !Asset.IsNil(); }
		friend bool operator==(const AssetDependencyNode& First, const AssetDependencyNode& Second) noexcept
		{
			if (First.Asset != Second.Asset) return false;
			if (!First.Asset.IsNil()) return First.SubresourceKey == Second.SubresourceKey;
			return First.SourceFileKey == Second.SourceFileKey && First.SubresourceKey == Second.SubresourceKey;
		}
	};

	struct AssetDependencyNodeLess
	{
		[[nodiscard]] bool operator()(const AssetDependencyNode& First, const AssetDependencyNode& Second) const noexcept
		{
			if (First.Asset != Second.Asset) return First.Asset < Second.Asset;
			if (First.Asset.IsNil() && First.SourceFileKey != Second.SourceFileKey) return First.SourceFileKey < Second.SourceFileKey;
			return First.SubresourceKey < Second.SubresourceKey;
		}
	};

	struct AssetDependencyEdge
	{
		AssetDependencyNode Referrer;
		AssetDependencyNode Target;
		AssetDependencyEdgeKind Kind = AssetDependencyEdgeKind::RuntimeReference;
		AssetDependencyStatus Status = AssetDependencyStatus::Resolved;
		std::string Location;
		std::string OriginalReference;
		// Set only on query copies for an imported subresource: the source-level processing inputs apply to every
		// primitive because the current importer reads the complete glTF source and all declared buffers.
		bool InheritedSourceInputs = false;
	};

	enum class AssetDependencyIssueCode : uint8_t
	{
		MissingDependency,
		WrongDependencyType,
		UnsupportedDependency,
		InvalidDependency,
		InvalidImportProvenance,
		Cycle
	};

	struct AssetDependencyIssue
	{
		AssetDependencyIssueCode Code = AssetDependencyIssueCode::InvalidDependency;
		AssetDependencyNode Referrer;
		AssetDependencyNode Target;
		std::string Location;
		std::string Message;
	};

	struct AssetSubresourceRecord
	{
		AssetDependencyNode Identity;
		std::string DisplayName;
		AssetSubresourceClassification Classification = AssetSubresourceClassification::DiscoveredLocator;
	};

	enum class AssetImportResultState : uint8_t
	{
		NoSuccessfulResultRecorded,
		SuccessfulInputsMatch,
		SuccessfulInputsDiffer,
		Unavailable
	};

	enum class AssetImportArtifactAvailability : uint8_t
	{
		NotTracked,
		Available,
		Missing
	};

	struct AssetImportObservation
	{
		AssetID Asset;
		std::string SubresourceKey;
		std::string ImporterID;
		std::string ImporterVersion;
		uint32_t SettingsVersion = 1;
		uint32_t OutputSchemaVersion = 0;
		// Canonical effective settings used as fingerprint input. Empty means the importer has no settings.
		std::string EffectiveSettings;
		std::string TargetID;
		std::vector<AssetImportInputFingerprint> Inputs;
		AssetContentFingerprint InputFingerprint;
		AssetImportResultState ResultState = AssetImportResultState::NoSuccessfulResultRecorded;
		std::optional<AssetImportSuccessReceipt> LastSuccessfulResult;
		AssetImportArtifactAvailability ArtifactAvailability = AssetImportArtifactAvailability::NotTracked;
	};

	enum class AssetDependencyImpactReason : uint8_t
	{
		Added,
		Removed,
		InputChanged,
		SettingsChanged,
		ImporterChanged,
		SchemaChanged,
		TargetChanged,
		RuntimeReferenceChanged,
		DependencyResolved,
		DependencyMissing
	};

	struct AssetDependencyImpactEntry
	{
		AssetID Asset;
		AssetDependencyImpactReason Reason = AssetDependencyImpactReason::InputChanged;
		friend bool operator==(const AssetDependencyImpactEntry&, const AssetDependencyImpactEntry&) = default;
	};

	struct AssetDependencyChangeImpact
	{
		std::vector<AssetDependencyImpactEntry> ImportWork;
		std::vector<AssetDependencyImpactEntry> RuntimeRefresh;
		std::vector<AssetID> UnchangedAssets;
	};

	struct AssetDependencyCatalogError
	{
		std::filesystem::path Path;
		std::string Message;
	};

	class AssetDependencyCatalogBuilder;

	// A retained immutable view owns its registry records and both directed indexes for one project generation.
	class PULSEFORGE_API AssetDependencyCatalogSnapshot final
	{
	public:
		[[nodiscard]] const UUID& GetProjectID() const noexcept { return m_ProjectID; }
		[[nodiscard]] const std::filesystem::path& GetProjectRootPath() const noexcept { return m_ProjectRoot; }
		[[nodiscard]] uint64_t GetGeneration() const noexcept { return m_Generation; }
		[[nodiscard]] const AssetRegistry& GetRegistry() const noexcept { return m_Registry; }
		[[nodiscard]] const std::vector<AssetDependencyEdge>& GetEdges() const noexcept { return m_Edges; }
		[[nodiscard]] const std::vector<AssetDependencyIssue>& GetIssues() const noexcept { return m_Issues; }
		[[nodiscard]] const std::vector<AssetSubresourceRecord>& GetSubresources() const noexcept { return m_Subresources; }
		[[nodiscard]] const std::vector<AssetImportObservation>& GetImportObservations() const noexcept { return m_ImportObservations; }
		[[nodiscard]] std::vector<AssetDependencyEdge> GetDirectDependencies(const AssetDependencyNode& Node) const;
		[[nodiscard]] std::vector<AssetDependencyEdge> GetDirectReferencers(const AssetDependencyNode& Node) const;
		[[nodiscard]] const std::vector<AssetDependencyInputReceipt>& GetInputReadSet() const noexcept { return m_InputReadSet; }

	private:
		friend class AssetDependencyCatalogBuilder;
		friend class AssetDependencyCatalog;
		AssetDependencyCatalogSnapshot(UUID ProjectID, uint64_t Generation, AssetRegistry Registry, std::vector<AssetDependencyEdge> Edges,
			std::vector<AssetDependencyIssue> Issues, std::vector<AssetSubresourceRecord> Subresources,
			std::vector<AssetImportObservation> ImportObservations, std::filesystem::path ProjectRoot,
			std::vector<AssetDependencyInputReceipt> InputReadSet);

		UUID m_ProjectID;
		uint64_t m_Generation = 0;
		AssetRegistry m_Registry;
		std::vector<AssetDependencyEdge> m_Edges;
		std::vector<AssetDependencyIssue> m_Issues;
		std::vector<AssetSubresourceRecord> m_Subresources;
		std::vector<AssetImportObservation> m_ImportObservations;
		std::filesystem::path m_ProjectRoot;
		std::vector<AssetDependencyInputReceipt> m_InputReadSet;
		std::map<AssetDependencyNode, std::vector<size_t>, AssetDependencyNodeLess> m_ForwardIndex;
		std::map<AssetDependencyNode, std::vector<size_t>, AssetDependencyNodeLess> m_ReverseIndex;
	};

	struct AssetDependencyCatalogTicket
	{
		UUID ProjectID;
		uint64_t Generation = 0;
	};

	struct AssetDependencyCatalogPublishOptions
	{
		// Allows deterministic request supersession at the final commit boundary in tests. Filesystem validation
		// has completed and no catalog mutex is held when this callback runs.
		std::function<void()> BeforeFinalTicketValidation;
	};

	struct AssetDependencyCatalogBuildOptions;

	class PULSEFORGE_API AssetDependencyCatalog final
	{
	public:
		using Snapshot = AssetDependencyCatalogSnapshot;
		using SnapshotPtr = std::shared_ptr<const Snapshot>;

		// Beginning a new project invalidates publication from the prior project. Refreshes in the same project retain
		// the prior usable snapshot until a complete replacement is published.
		[[nodiscard]] AssetDependencyCatalogTicket BeginRequest(const UUID& ProjectID);
		[[nodiscard]] std::expected<void, AssetDependencyCatalogError> Publish(
			const AssetDependencyCatalogTicket& Ticket,
			SnapshotPtr Candidate,
			const AssetDependencyCatalogPublishOptions& Options = {});
		[[nodiscard]] SnapshotPtr GetSnapshot() const;
		[[nodiscard]] bool IsLatestRequest(const AssetDependencyCatalogTicket& Ticket) const;

	private:
		mutable std::mutex m_Mutex;
		UUID m_RequestedProject;
		uint64_t m_LatestGeneration = 0;
		SnapshotPtr m_Current;
	};

	class PULSEFORGE_API AssetDependencyCatalogBuilder final
	{
	public:
		[[nodiscard]] static std::expected<AssetDependencyCatalog::SnapshotPtr, AssetDependencyCatalogError> Build(
			const std::filesystem::path& ProjectRoot, const UUID& ProjectID, uint64_t Generation);
		[[nodiscard]] static std::expected<AssetDependencyCatalog::SnapshotPtr, AssetDependencyCatalogError> Build(
			const std::filesystem::path& ProjectRoot, const UUID& ProjectID, uint64_t Generation,
			const AssetDependencyCatalogBuildOptions& Options);
	};

	[[nodiscard]] PULSEFORGE_API AssetDependencyChangeImpact ComputeAssetDependencyChangeImpact(
		const AssetDependencyCatalogSnapshot& Previous, const AssetDependencyCatalogSnapshot& Current);
} // namespace PulseForge
