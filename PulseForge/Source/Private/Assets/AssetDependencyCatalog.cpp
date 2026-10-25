#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetDependencyExtractor.h"

#include <algorithm>
#include <limits>
#include <set>
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

		AssetDependencyCatalogError MakeRegistryError(
			const std::filesystem::path& ProjectRoot,
			const AssetRegistryError& RegistryError)
		{
			std::string Message = "Could not validate asset registry membership before dependency publication";
			std::filesystem::path Path = ProjectRoot;
			for (const AssetRegistryIssue& Issue : RegistryError.Issues)
			{
				if (!Issue.ProjectRelativePath.empty())
				{
					if (Path == ProjectRoot) Path = ProjectRoot / Issue.ProjectRelativePath;
					Message += "\n - " + Issue.ProjectRelativePath.generic_string();
				}
				if (!Issue.RelatedProjectRelativePath.empty())
					Message += " (conflicts with " + Issue.RelatedProjectRelativePath.generic_string() + ")";
				Message += ": " + Issue.Message;
			}
			return MakeBuildError(Path, std::move(Message));
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
		std::vector<AssetImportObservation> ImportObservations, std::filesystem::path ProjectRoot,
		std::vector<AssetDependencyInputReceipt> InputReadSet)
		: m_ProjectID(std::move(ProjectID)), m_Generation(Generation), m_Registry(std::move(Registry)), m_Edges(std::move(Edges)),
		  m_Issues(std::move(Issues)), m_Subresources(std::move(Subresources)), m_ImportObservations(std::move(ImportObservations)),
		  m_ProjectRoot(std::move(ProjectRoot)), m_InputReadSet(std::move(InputReadSet))
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
			[](const AssetImportObservation& First, const AssetImportObservation& Second)
			{
				return std::tie(First.Asset, First.SubresourceKey) < std::tie(Second.Asset, Second.SubresourceKey);
			});

		for (size_t Index = 0; Index < m_Edges.size(); ++Index)
		{
			m_ForwardIndex[m_Edges[Index].Referrer].push_back(Index);
			m_ReverseIndex[m_Edges[Index].Target].push_back(Index);
		}
	}

	std::vector<AssetDependencyEdge> AssetDependencyCatalogSnapshot::GetDirectDependencies(const AssetDependencyNode& Node) const
	{
		std::vector<AssetDependencyEdge> Result;
		const bool IsKnownSubresource = !Node.Asset.IsNil() && !Node.SubresourceKey.empty() &&
			std::any_of(m_Subresources.begin(), m_Subresources.end(), [&](const AssetSubresourceRecord& Record)
				{ return SameNodeIdentity(Record.Identity, Node); });
		if (IsKnownSubresource)
		{
			const AssetDependencyNode SourceNode{ Node.Asset, {}, {} };
			const auto It = m_ForwardIndex.find(SourceNode);
			if (It == m_ForwardIndex.end()) return Result;
			for (const size_t Index : It->second)
			{
				if (m_Edges[Index].Kind != AssetDependencyEdgeKind::ImportInput) continue;
				AssetDependencyEdge Inherited = m_Edges[Index];
				Inherited.Referrer = Node;
				Inherited.InheritedSourceInputs = true;
				Result.push_back(std::move(Inherited));
			}
			return Result;
		}
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

	std::expected<void, AssetDependencyCatalogError> AssetDependencyCatalog::Publish(
		const AssetDependencyCatalogTicket& Ticket,
		SnapshotPtr Candidate,
		const AssetDependencyCatalogPublishOptions& Options)
	{
		if (!Candidate)
			return std::unexpected(MakeBuildError({}, "Cannot publish an empty asset dependency snapshot"));
		{
			std::lock_guard Lock(m_Mutex);
			if (Ticket.ProjectID != m_RequestedProject || Ticket.Generation != m_LatestGeneration ||
				Candidate->GetProjectID() != Ticket.ProjectID || Candidate->GetGeneration() != Ticket.Generation)
				return std::unexpected(MakeBuildError(Candidate->GetProjectRootPath(),
					"Asset dependency publication request was superseded or does not match the candidate snapshot"));
		}

		AssetRegistry CurrentRegistry;
		if (auto RegistryResult = CurrentRegistry.Rebuild(Candidate->m_ProjectRoot); !RegistryResult)
			return std::unexpected(MakeRegistryError(Candidate->m_ProjectRoot, RegistryResult.error()));

		const std::vector<AssetRecord> CandidateAssets = Candidate->m_Registry.GetAssets();
		const std::vector<AssetRecord> CurrentAssets = CurrentRegistry.GetAssets();
		const bool SameMembership = CandidateAssets.size() == CurrentAssets.size() &&
			std::equal(CandidateAssets.begin(), CandidateAssets.end(), CurrentAssets.begin(), [](const AssetRecord& First,
				const AssetRecord& Second)
			{
				return First.ID == Second.ID && First.ProjectRelativePath == Second.ProjectRelativePath;
			});
		if (!SameMembership)
			return std::unexpected(MakeBuildError(Candidate->m_ProjectRoot,
				"Asset registry membership changed after dependency analysis; candidate snapshot was discarded"));

		if (auto ReadSetResult = AssetDependencyExtractor::ValidateInputReceipts(
				Candidate->m_ProjectRoot, Candidate->m_InputReadSet);
			!ReadSetResult)
			return std::unexpected(ReadSetResult.error());

		if (Options.BeforeFinalTicketValidation) Options.BeforeFinalTicketValidation();

		std::lock_guard Lock(m_Mutex);
		if (Ticket.ProjectID != m_RequestedProject || Ticket.Generation != m_LatestGeneration ||
			Candidate->GetProjectID() != Ticket.ProjectID || Candidate->GetGeneration() != Ticket.Generation)
			return std::unexpected(MakeBuildError(Candidate->GetProjectRootPath(),
				"Asset dependency publication request was superseded during filesystem validation"));
		m_Current = std::move(Candidate);
		return {};
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
		return Build(ProjectRoot, ProjectID, Generation, AssetDependencyCatalogBuildOptions{});
	}

	std::expected<AssetDependencyCatalog::SnapshotPtr, AssetDependencyCatalogError> AssetDependencyCatalogBuilder::Build(
		const std::filesystem::path& ProjectRoot, const UUID& ProjectID, uint64_t Generation,
		const AssetDependencyCatalogBuildOptions& Options)
	{
		if (ProjectRoot.empty() || ProjectID.IsNil() || Generation == 0)
			return std::unexpected(
				MakeBuildError(ProjectRoot, "Asset catalog build requires a project path, non-nil project ID, and nonzero generation"));
		std::error_code RootError;
		const std::filesystem::path AbsoluteProjectRoot = std::filesystem::absolute(ProjectRoot, RootError).lexically_normal();
		if (RootError || !std::filesystem::is_directory(AbsoluteProjectRoot, RootError) || RootError)
			return std::unexpected(MakeBuildError(ProjectRoot, "Asset catalog build requires an accessible project directory"));

		AssetRegistry Registry;
		if (auto RegistryResult = Registry.Rebuild(AbsoluteProjectRoot); !RegistryResult)
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

		auto ExtractionResult = AssetDependencyExtractor::Extract(AbsoluteProjectRoot, ProjectID, Registry);
		if (!ExtractionResult) return std::unexpected(std::move(ExtractionResult.error()));

		AssetDependencyExtraction Extraction = std::move(*ExtractionResult);
		if (Options.BeforeFinalInputValidation) Options.BeforeFinalInputValidation();
		AssetRegistry FinalRegistry;
		if (auto FinalRegistryResult = FinalRegistry.Rebuild(AbsoluteProjectRoot); !FinalRegistryResult)
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
		if (!AssetDependencyExtractor::ValidateInputReceipts(AbsoluteProjectRoot, Extraction.ReadSet))
			return std::unexpected(MakeBuildError(ProjectRoot,
				"Asset dependency inputs changed during analysis; candidate snapshot was discarded"));

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

		const std::map<AssetDependencyNode, bool, AssetDependencyNodeLess> KnownSubresources = [&]
		{
			std::map<AssetDependencyNode, bool, AssetDependencyNodeLess> Result;
			for (const AssetSubresourceRecord& Subresource : Extraction.Subresources)
				Result.emplace(Subresource.Identity, true);
			return Result;
		}();

		std::map<std::pair<AssetID, std::string>, bool> ObservedAssets;
		for (const AssetImportObservation& Observation : Extraction.ImportObservations)
		{
			if (Observation.Asset.IsNil() || !Registry.Find(Observation.Asset))
				return std::unexpected(
					MakeBuildError(ProjectRoot, "Dependency extractor returned import observations for an unregistered asset"));
			if (!ObservedAssets.try_emplace(std::make_pair(Observation.Asset, Observation.SubresourceKey), true).second)
				return std::unexpected(MakeBuildError(ProjectRoot,
					"Dependency extractor returned more than one import observation for asset/subresource " + Observation.Asset.ToString() +
					Observation.SubresourceKey));
			if (!Observation.SubresourceKey.empty() &&
				!KnownSubresources.contains(AssetDependencyNode{ Observation.Asset, {}, Observation.SubresourceKey }))
				return std::unexpected(MakeBuildError(ProjectRoot,
					"Dependency extractor returned import observation for an undeclared subresource '" + Observation.SubresourceKey + "'"));
			if ((Observation.ResultState == AssetImportResultState::SuccessfulInputsMatch ||
					Observation.ResultState == AssetImportResultState::SuccessfulInputsDiffer) &&
				!Observation.LastSuccessfulResult)
				return std::unexpected(MakeBuildError(ProjectRoot,
					"Catalog observation cannot compare successful inputs without a persisted success receipt"));
			if (Observation.ResultState == AssetImportResultState::NoSuccessfulResultRecorded && Observation.LastSuccessfulResult)
				return std::unexpected(MakeBuildError(ProjectRoot,
					"Catalog observation attached a success receipt while claiming that no success was recorded"));
		}

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
				std::move(Extraction.Issues), std::move(Extraction.Subresources), std::move(Extraction.ImportObservations),
				AbsoluteProjectRoot, std::move(Extraction.ReadSet)));
		return std::static_pointer_cast<const AssetDependencyCatalogSnapshot>(std::move(Candidate));
	}

	AssetDependencyChangeImpact ComputeAssetDependencyChangeImpact(
		const AssetDependencyCatalogSnapshot& Previous, const AssetDependencyCatalogSnapshot& Current)
	{
		AssetDependencyChangeImpact Impact;
		using ObservationKey = std::pair<AssetID, std::string>;
		std::map<ObservationKey, const AssetImportObservation*> OldObservations;
		std::map<ObservationKey, const AssetImportObservation*> NewObservations;
		for (const AssetImportObservation& Observation : Previous.GetImportObservations())
			OldObservations.emplace(ObservationKey{ Observation.Asset, Observation.SubresourceKey }, &Observation);
		for (const AssetImportObservation& Observation : Current.GetImportObservations())
			NewObservations.emplace(ObservationKey{ Observation.Asset, Observation.SubresourceKey }, &Observation);

		const auto AddUnique = [](std::vector<AssetDependencyImpactEntry>& Entries, const AssetID& Asset,
			AssetDependencyImpactReason Reason)
		{
			const AssetDependencyImpactEntry Entry{ Asset, Reason };
			if (std::find(Entries.begin(), Entries.end(), Entry) == Entries.end()) Entries.push_back(Entry);
		};
		for (const auto& [Key, NewObservation] : NewObservations)
		{
			const AssetID& Asset = Key.first;
			const auto OldIt = OldObservations.find(Key);
			if (OldIt == OldObservations.end())
			{
				AddUnique(Impact.ImportWork, Asset, AssetDependencyImpactReason::Added);
				continue;
			}
			const AssetImportObservation& Old = *OldIt->second;
			if (Old.Inputs != NewObservation->Inputs)
				AddUnique(Impact.ImportWork, Asset, AssetDependencyImpactReason::InputChanged);
			if (Old.EffectiveSettings != NewObservation->EffectiveSettings || Old.SettingsVersion != NewObservation->SettingsVersion)
				AddUnique(Impact.ImportWork, Asset, AssetDependencyImpactReason::SettingsChanged);
			if (Old.ImporterID != NewObservation->ImporterID || Old.ImporterVersion != NewObservation->ImporterVersion)
				AddUnique(Impact.ImportWork, Asset, AssetDependencyImpactReason::ImporterChanged);
			if (Old.OutputSchemaVersion != NewObservation->OutputSchemaVersion)
				AddUnique(Impact.ImportWork, Asset, AssetDependencyImpactReason::SchemaChanged);
			if (Old.TargetID != NewObservation->TargetID)
				AddUnique(Impact.ImportWork, Asset, AssetDependencyImpactReason::TargetChanged);
			if (Old.InputFingerprint != NewObservation->InputFingerprint &&
				std::none_of(Impact.ImportWork.begin(), Impact.ImportWork.end(), [&](const AssetDependencyImpactEntry& Entry)
					{ return Entry.Asset == Asset; }))
				AddUnique(Impact.ImportWork, Asset, AssetDependencyImpactReason::InputChanged);
		}
		for (const auto& [Key, OldObservation] : OldObservations)
		{
			(void)OldObservation;
			if (!NewObservations.contains(Key)) AddUnique(Impact.ImportWork, Key.first, AssetDependencyImpactReason::Removed);
		}

		const auto SameRuntimeRelationship = [](const AssetDependencyEdge& First, const AssetDependencyEdge& Second)
		{
			return First.Kind == AssetDependencyEdgeKind::RuntimeReference && Second.Kind == AssetDependencyEdgeKind::RuntimeReference &&
				SameNodeIdentity(First.Referrer, Second.Referrer) && SameNodeIdentity(First.Target, Second.Target) &&
				First.Location == Second.Location && First.OriginalReference == Second.OriginalReference;
		};
		const auto AddRuntimeEdgeChanges = [&](const std::vector<AssetDependencyEdge>& OldEdges,
			const std::vector<AssetDependencyEdge>& NewEdges)
		{
			for (const AssetDependencyEdge& Old : OldEdges)
			{
				if (Old.Kind != AssetDependencyEdgeKind::RuntimeReference) continue;
				const auto NewIt = std::find_if(NewEdges.begin(), NewEdges.end(), [&](const AssetDependencyEdge& New)
					{ return SameRuntimeRelationship(Old, New); });
				if (NewIt == NewEdges.end())
					AddUnique(Impact.RuntimeRefresh, Old.Referrer.Asset, AssetDependencyImpactReason::RuntimeReferenceChanged);
				else if (Old.Status != NewIt->Status)
					AddUnique(Impact.RuntimeRefresh, Old.Referrer.Asset,
						NewIt->Status == AssetDependencyStatus::Resolved ? AssetDependencyImpactReason::DependencyResolved
																								: AssetDependencyImpactReason::DependencyMissing);
			}
			for (const AssetDependencyEdge& New : NewEdges)
			{
				if (New.Kind != AssetDependencyEdgeKind::RuntimeReference) continue;
				if (std::none_of(OldEdges.begin(), OldEdges.end(), [&](const AssetDependencyEdge& Old)
						{ return SameRuntimeRelationship(Old, New); }))
					AddUnique(Impact.RuntimeRefresh, New.Referrer.Asset, AssetDependencyImpactReason::RuntimeReferenceChanged);
			}
		};
		AddRuntimeEdgeChanges(Previous.GetEdges(), Current.GetEdges());

		// A changed imported asset makes its authored runtime referencers stale, but does not turn those references into
		// importer work. Traverse the reverse runtime-reference relation separately and retain the edge distinction.
		std::vector<AssetID> ChangedImports;
		for (const AssetDependencyImpactEntry& Entry : Impact.ImportWork)
			if (Entry.Reason != AssetDependencyImpactReason::Removed) ChangedImports.push_back(Entry.Asset);

		std::map<std::string, AssetDependencyInputReceipt> PreviousInputs;
		std::map<std::string, AssetDependencyInputReceipt> CurrentInputs;
		for (const AssetDependencyInputReceipt& Receipt : Previous.GetInputReadSet())
			PreviousInputs.emplace(Receipt.ProjectRelativePath, Receipt);
		for (const AssetDependencyInputReceipt& Receipt : Current.GetInputReadSet())
			CurrentInputs.emplace(Receipt.ProjectRelativePath, Receipt);
		std::set<std::string> ChangedPaths;
		for (const auto& [Path, OldReceipt] : PreviousInputs)
		{
			const auto NewIt = CurrentInputs.find(Path);
			if (NewIt == CurrentInputs.end() || NewIt->second != OldReceipt) ChangedPaths.insert(Path);
		}
		for (const auto& [Path, NewReceipt] : CurrentInputs)
		{
			(void)NewReceipt;
			if (!PreviousInputs.contains(Path)) ChangedPaths.insert(Path);
		}

		std::map<std::string, AssetID> PreviousAssetsByPath;
		std::map<std::string, AssetID> CurrentAssetsByPath;
		for (const AssetRecord& Asset : Previous.GetRegistry().GetAssets())
			PreviousAssetsByPath.emplace(PathToUtf8(Asset.ProjectRelativePath), Asset.ID);
		for (const AssetRecord& Asset : Current.GetRegistry().GetAssets())
			CurrentAssetsByPath.emplace(PathToUtf8(Asset.ProjectRelativePath), Asset.ID);
		std::vector<AssetID> ChangedRuntimeTargets;
		for (const std::string& Path : ChangedPaths)
		{
			if (Path.ends_with(".meta") || Path.ends_with(".importresult")) continue;
			if (const auto It = PreviousAssetsByPath.find(Path); It != PreviousAssetsByPath.end()) ChangedRuntimeTargets.push_back(It->second);
			if (const auto It = CurrentAssetsByPath.find(Path); It != CurrentAssetsByPath.end()) ChangedRuntimeTargets.push_back(It->second);
		}
		ChangedRuntimeTargets.insert(ChangedRuntimeTargets.end(), ChangedImports.begin(), ChangedImports.end());
		for (const AssetID& Changed : ChangedRuntimeTargets)
		{
			for (const AssetDependencyCatalogSnapshot* Snapshot : { &Previous, &Current })
			{
				std::vector<AssetID> Pending{ Changed };
				std::map<AssetID, bool> Visited;
				while (!Pending.empty())
				{
					const AssetID Target = Pending.back();
					Pending.pop_back();
					if (!Visited.try_emplace(Target, true).second) continue;
					for (const AssetDependencyEdge& Edge : Snapshot->GetDirectReferencers(AssetDependencyNode{ Target, {}, {} }))
					{
						if (Edge.Kind != AssetDependencyEdgeKind::RuntimeReference || Edge.Referrer.Asset.IsNil()) continue;
						AddUnique(Impact.RuntimeRefresh, Edge.Referrer.Asset, AssetDependencyImpactReason::RuntimeReferenceChanged);
						Pending.push_back(Edge.Referrer.Asset);
					}
				}
			}
		}

		std::sort(Impact.ImportWork.begin(), Impact.ImportWork.end(), [](const auto& First, const auto& Second)
			{ return std::tie(First.Asset, First.Reason) < std::tie(Second.Asset, Second.Reason); });
		std::sort(Impact.RuntimeRefresh.begin(), Impact.RuntimeRefresh.end(), [](const auto& First, const auto& Second)
			{ return std::tie(First.Asset, First.Reason) < std::tie(Second.Asset, Second.Reason); });
		for (const AssetRecord& Asset : Current.GetRegistry().GetAssets())
		{
			const bool HasImportImpact = std::any_of(Impact.ImportWork.begin(), Impact.ImportWork.end(),
				[&](const AssetDependencyImpactEntry& Entry) { return Entry.Asset == Asset.ID; });
			const bool HasRuntimeImpact = std::any_of(Impact.RuntimeRefresh.begin(), Impact.RuntimeRefresh.end(),
				[&](const AssetDependencyImpactEntry& Entry) { return Entry.Asset == Asset.ID; });
			if (!HasImportImpact && !HasRuntimeImpact) Impact.UnchangedAssets.push_back(Asset.ID);
		}
		std::sort(Impact.UnchangedAssets.begin(), Impact.UnchangedAssets.end());
		return Impact;
	}
} // namespace PulseForge
