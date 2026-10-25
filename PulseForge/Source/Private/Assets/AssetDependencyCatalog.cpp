#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetDependencyExtractor.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace PulseForge
{
	namespace
	{
		bool NodeLess(const AssetDependencyNode& First, const AssetDependencyNode& Second)
		{
			return AssetDependencyNodeLess{}(First, Second);
		}

		bool SameNodeIdentity(const AssetDependencyNode& First, const AssetDependencyNode& Second)
		{
			if (First.Asset != Second.Asset) return false;
			if (!First.Asset.IsNil()) return First.SubresourceKey == Second.SubresourceKey;
			return First.SourceFileKey == Second.SourceFileKey && First.SubresourceKey == Second.SubresourceKey;
		}

		bool EdgeLess(const AssetDependencyEdge& First, const AssetDependencyEdge& Second)
		{
			if (NodeLess(First.Referrer, Second.Referrer)) return true;
			if (NodeLess(Second.Referrer, First.Referrer)) return false;
			if (NodeLess(First.Target, Second.Target)) return true;
			if (NodeLess(Second.Target, First.Target)) return false;
			return std::tie(First.Kind, First.Status, First.Location, First.OriginalReference) <
				   std::tie(Second.Kind, Second.Status, Second.Location, Second.OriginalReference);
		}

		bool SameEdge(const AssetDependencyEdge& First, const AssetDependencyEdge& Second)
		{
			return SameNodeIdentity(First.Referrer, Second.Referrer) && SameNodeIdentity(First.Target, Second.Target) &&
				   First.Kind == Second.Kind && First.Status == Second.Status && First.Location == Second.Location &&
				   First.OriginalReference == Second.OriginalReference;
		}

		bool IssueLess(const AssetDependencyIssue& First, const AssetDependencyIssue& Second)
		{
			if (NodeLess(First.Referrer, Second.Referrer)) return true;
			if (NodeLess(Second.Referrer, First.Referrer)) return false;
			if (NodeLess(First.Target, Second.Target)) return true;
			if (NodeLess(Second.Target, First.Target)) return false;
			return std::tie(First.Code, First.Location, First.Message) < std::tie(Second.Code, Second.Location, Second.Message);
		}

		bool SameIssue(const AssetDependencyIssue& First, const AssetDependencyIssue& Second)
		{
			return First.Code == Second.Code && SameNodeIdentity(First.Referrer, Second.Referrer) &&
				   SameNodeIdentity(First.Target, Second.Target) && First.Location == Second.Location && First.Message == Second.Message;
		}

		AssetDependencyCatalogError MakeBuildError(const std::filesystem::path& Path, std::string Message)
		{
			return { Path, std::move(Message) };
		}

		bool IsSubresourceNode(const AssetDependencyNode& Node)
		{
			return !Node.Asset.IsNil() && Node.SubresourceKey.starts_with("mesh/");
		}

		bool IsSceneEntityNode(const AssetDependencyNode& Node)
		{
			if (Node.Asset.IsNil() || !Node.SubresourceKey.starts_with("entity/")) return false;
			const std::string_view EntityID = std::string_view(Node.SubresourceKey).substr(7);
			const auto Parsed = UUID::Parse(EntityID);
			return Parsed.has_value() && !Parsed->IsNil();
		}

		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Text = Path.generic_u8string();
			return { reinterpret_cast<const char*>(Text.data()), Text.size() };
		}
	} // namespace

	AssetDependencyCatalogSnapshot::AssetDependencyCatalogSnapshot(UUID ProjectID, uint64_t Generation, AssetRegistry Registry,
		std::vector<AssetDependencyEdge> Edges, std::vector<AssetDependencyIssue> Issues, std::vector<AssetSubresourceRecord> Subresources,
		std::vector<AssetImportObservation> ImportObservations)
		: m_ProjectID(std::move(ProjectID)), m_Generation(Generation), m_Registry(std::move(Registry)), m_Edges(std::move(Edges)),
		  m_Issues(std::move(Issues)), m_Subresources(std::move(Subresources)), m_ImportObservations(std::move(ImportObservations))
	{
		std::sort(m_Edges.begin(), m_Edges.end(), EdgeLess);
		m_Edges.erase(std::unique(m_Edges.begin(), m_Edges.end(), SameEdge), m_Edges.end());
		std::sort(m_Issues.begin(), m_Issues.end(), IssueLess);
		m_Issues.erase(std::unique(m_Issues.begin(), m_Issues.end(), SameIssue), m_Issues.end());

		std::sort(m_Subresources.begin(), m_Subresources.end(),
			[](const AssetSubresourceRecord& First, const AssetSubresourceRecord& Second)
			{
				if (NodeLess(First.Identity, Second.Identity)) return true;
				if (NodeLess(Second.Identity, First.Identity)) return false;
				return First.DisplayName < Second.DisplayName;
			});
		std::sort(m_ImportObservations.begin(), m_ImportObservations.end(),
			[](const AssetImportObservation& First, const AssetImportObservation& Second) { return First.Asset < Second.Asset; });

		for (size_t Index = 0; Index < m_Edges.size(); ++Index)
		{
			m_ForwardIndex[m_Edges[Index].Referrer].push_back(Index);
			m_ReverseIndex[m_Edges[Index].Target].push_back(Index);
		}
	}

	std::vector<AssetDependencyEdge> AssetDependencyCatalogSnapshot::GetDirectDependencies(const AssetDependencyNode& Node) const
	{
		std::vector<AssetDependencyEdge> Result;
		if (!Node.Asset.IsNil() && Node.SubresourceKey.empty())
		{
			for (const AssetDependencyEdge& Edge : m_Edges)
				if (Edge.Referrer.Asset == Node.Asset) Result.push_back(Edge);
			return Result;
		}
		const auto It = m_ForwardIndex.find(Node);
		if (It == m_ForwardIndex.end()) return Result;
		Result.reserve(It->second.size());
		for (const size_t Index : It->second)
			Result.push_back(m_Edges[Index]);
		return Result;
	}

	std::vector<AssetDependencyEdge> AssetDependencyCatalogSnapshot::GetDirectReferencers(const AssetDependencyNode& Node) const
	{
		std::vector<AssetDependencyEdge> Result;
		if (!Node.Asset.IsNil() && Node.SubresourceKey.empty())
		{
			for (const AssetDependencyEdge& Edge : m_Edges)
				if (Edge.Target.Asset == Node.Asset) Result.push_back(Edge);
			return Result;
		}
		const auto It = m_ReverseIndex.find(Node);
		if (It == m_ReverseIndex.end()) return Result;
		Result.reserve(It->second.size());
		for (const size_t Index : It->second)
			Result.push_back(m_Edges[Index]);
		return Result;
	}

	AssetDependencyCatalogTicket AssetDependencyCatalog::BeginRequest(const UUID& ProjectID)
	{
		std::lock_guard Lock(m_Mutex);
		if (m_RequestedProject != ProjectID)
		{
			m_RequestedProject = ProjectID;
			m_Current.reset();
		}
		if (m_LatestGeneration == std::numeric_limits<uint64_t>::max())
		{
			// Exhaustion is unreachable in practice. Poison publication rather than reusing a ticket identity,
			// which could otherwise allow an ancient asynchronous result to publish after project switches.
			m_RequestedProject = {};
			m_Current.reset();
			return {};
		}
		++m_LatestGeneration;
		return { ProjectID, m_LatestGeneration };
	}

	bool AssetDependencyCatalog::Publish(const AssetDependencyCatalogTicket& Ticket, SnapshotPtr Candidate)
	{
		if (!Candidate) return false;
		std::lock_guard Lock(m_Mutex);
		if (Ticket.ProjectID != m_RequestedProject || Ticket.Generation != m_LatestGeneration ||
			Candidate->GetProjectID() != Ticket.ProjectID || Candidate->GetGeneration() != Ticket.Generation)
			return false;
		m_Current = std::move(Candidate);
		return true;
	}

	AssetDependencyCatalog::SnapshotPtr AssetDependencyCatalog::GetSnapshot() const
	{
		std::lock_guard Lock(m_Mutex);
		return m_Current;
	}

	bool AssetDependencyCatalog::IsLatestRequest(const AssetDependencyCatalogTicket& Ticket) const
	{
		std::lock_guard Lock(m_Mutex);
		return Ticket.ProjectID == m_RequestedProject && Ticket.Generation == m_LatestGeneration;
	}

	std::expected<AssetDependencyCatalog::SnapshotPtr, AssetDependencyCatalogError> AssetDependencyCatalogBuilder::Build(
		const std::filesystem::path& ProjectRoot, const UUID& ProjectID, uint64_t Generation)
	{
		if (ProjectRoot.empty() || ProjectID.IsNil() || Generation == 0)
			return std::unexpected(
				MakeBuildError(ProjectRoot, "Asset catalog build requires a project path, non-nil project ID, and nonzero generation"));

		AssetRegistry Registry;
		if (auto RegistryResult = Registry.Rebuild(ProjectRoot); !RegistryResult)
		{
			std::string Message = "Could not build the asset registry for the dependency catalog";
			for (const AssetRegistryIssue& Issue : RegistryResult.error().Issues)
			{
				Message += "\n - ";
				if (!Issue.ProjectRelativePath.empty()) Message += Issue.ProjectRelativePath.generic_string() + ": ";
				Message += Issue.Message;
			}
			return std::unexpected(MakeBuildError(ProjectRoot, std::move(Message)));
		}

		auto ExtractionResult = AssetDependencyExtractor::Extract(ProjectRoot, ProjectID, Registry);
		if (!ExtractionResult) return std::unexpected(std::move(ExtractionResult.error()));

		AssetDependencyExtraction Extraction = std::move(*ExtractionResult);
		AssetRegistry FinalRegistry;
		if (auto FinalRegistryResult = FinalRegistry.Rebuild(ProjectRoot); !FinalRegistryResult)
			return std::unexpected(MakeBuildError(
				ProjectRoot, "Asset catalog inputs changed or became unreadable during dependency analysis; final registry scan failed"));
		const std::vector<AssetRecord> InitialAssets = Registry.GetAssets();
		const std::vector<AssetRecord> FinalAssets = FinalRegistry.GetAssets();
		bool SameRegistryMembership = InitialAssets.size() == FinalAssets.size();
		for (size_t Index = 0; SameRegistryMembership && Index < InitialAssets.size(); ++Index)
			SameRegistryMembership = InitialAssets[Index].ID == FinalAssets[Index].ID &&
									 InitialAssets[Index].ProjectRelativePath == FinalAssets[Index].ProjectRelativePath;
		if (!SameRegistryMembership ||
			Registry.GetSourceByteAccounting().KnownBytes != FinalRegistry.GetSourceByteAccounting().KnownBytes ||
			Registry.GetSourceByteAccounting().UnavailableAssetCount != FinalRegistry.GetSourceByteAccounting().UnavailableAssetCount)
			return std::unexpected(MakeBuildError(ProjectRoot,
				"Asset registry membership or source sizes changed during dependency analysis; candidate snapshot was discarded"));

		std::map<AssetDependencyNode, std::string, AssetDependencyNodeLess> SubresourceNames;
		std::vector<AssetSubresourceRecord> UniqueSubresources;
		UniqueSubresources.reserve(Extraction.Subresources.size());
		for (AssetSubresourceRecord& Subresource : Extraction.Subresources)
		{
			if (!IsSubresourceNode(Subresource.Identity))
				return std::unexpected(MakeBuildError(
					ProjectRoot, "Dependency extractor returned a subresource without a managed source UUID or mesh locator key"));
			const auto Source = Registry.Find(Subresource.Identity.Asset);
			if (!Source)
				return std::unexpected(
					MakeBuildError(ProjectRoot, "Dependency extractor returned a subresource for an unregistered source UUID " +
													Subresource.Identity.Asset.ToString()));
			if (!Subresource.Identity.SourceFileKey.empty() &&
				Subresource.Identity.SourceFileKey != PathToUtf8(Source->ProjectRelativePath))
				return std::unexpected(MakeBuildError(
					ProjectRoot, "Dependency extractor returned a subresource source path inconsistent with its registered asset UUID"));

			const auto [Existing, Inserted] = SubresourceNames.try_emplace(Subresource.Identity, Subresource.DisplayName);
			if (!Inserted)
			{
				if (Existing->second != Subresource.DisplayName)
					return std::unexpected(
						MakeBuildError(ProjectRoot, "Dependency extractor produced conflicting display names for stable subresource key '" +
														Subresource.Identity.SubresourceKey + "'"));
				continue;
			}
			UniqueSubresources.push_back(std::move(Subresource));
		}

		std::sort(UniqueSubresources.begin(), UniqueSubresources.end(),
			[](const AssetSubresourceRecord& First, const AssetSubresourceRecord& Second)
			{
				if (NodeLess(First.Identity, Second.Identity)) return true;
				if (NodeLess(Second.Identity, First.Identity)) return false;
				return First.DisplayName < Second.DisplayName;
			});
		Extraction.Subresources = std::move(UniqueSubresources);

		std::map<AssetID, bool> ObservedAssets;
		for (const AssetImportObservation& Observation : Extraction.ImportObservations)
		{
			if (Observation.Asset.IsNil() || !Registry.Find(Observation.Asset))
				return std::unexpected(
					MakeBuildError(ProjectRoot, "Dependency extractor returned import observations for an unregistered asset"));
			if (!ObservedAssets.try_emplace(Observation.Asset, true).second)
				return std::unexpected(MakeBuildError(ProjectRoot,
					"Dependency extractor returned more than one import observation for asset UUID " + Observation.Asset.ToString()));
			if (Observation.ResultState == AssetImportResultState::Current || Observation.ResultState == AssetImportResultState::Stale ||
				Observation.SuccessfulResultFingerprint)
				return std::unexpected(MakeBuildError(ProjectRoot,
					"Catalog scan cannot claim successful import provenance because no persisted successful result is recorded"));
		}

		const std::map<AssetDependencyNode, bool, AssetDependencyNodeLess> KnownSubresources = [&]
		{
			std::map<AssetDependencyNode, bool, AssetDependencyNodeLess> Result;
			for (const AssetSubresourceRecord& Subresource : Extraction.Subresources)
				Result.emplace(Subresource.Identity, true);
			return Result;
		}();
		for (const AssetDependencyEdge& Edge : Extraction.Edges)
		{
			if (Edge.Referrer.Asset.IsNil() || !Registry.Find(Edge.Referrer.Asset))
				return std::unexpected(
					MakeBuildError(ProjectRoot, "Dependency extractor returned an edge whose referring managed asset is not registered"));
			if (IsSubresourceNode(Edge.Referrer) && !KnownSubresources.contains(Edge.Referrer))
				return std::unexpected(
					MakeBuildError(ProjectRoot, "Dependency extractor returned an edge from an undeclared imported subresource"));
			if (!Edge.Referrer.SubresourceKey.empty() && !IsSubresourceNode(Edge.Referrer) && !IsSceneEntityNode(Edge.Referrer))
				return std::unexpected(
					MakeBuildError(ProjectRoot, "Dependency extractor returned an edge from an unsupported source-local identity"));
			if (Edge.Target.Asset.IsNil() && Edge.Target.SourceFileKey.empty() && Edge.Status != AssetDependencyStatus::Unsupported &&
				Edge.Status != AssetDependencyStatus::Invalid)
				return std::unexpected(
					MakeBuildError(ProjectRoot, "Dependency extractor returned a target with neither an asset UUID nor a source-file key"));
		}

		auto Candidate = std::shared_ptr<AssetDependencyCatalogSnapshot>(
			new AssetDependencyCatalogSnapshot(ProjectID, Generation, std::move(Registry), std::move(Extraction.Edges),
				std::move(Extraction.Issues), std::move(Extraction.Subresources), std::move(Extraction.ImportObservations)));
		return std::static_pointer_cast<const AssetDependencyCatalogSnapshot>(std::move(Candidate));
	}
} // namespace PulseForge
