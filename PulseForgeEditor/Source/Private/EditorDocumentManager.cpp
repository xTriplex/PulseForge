#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorNativeWindowBoundsPolicy.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <type_traits>

namespace PulseForgeEditor
{
#if defined(PULSEFORGE_EDITOR_DOCUMENT_MANAGER_TESTING)
	namespace
	{
		thread_local const EditorDocumentManager* TransferAllocationFailureManager = nullptr;
	}

	void EditorDocumentManager::FailNextTransferStagingAllocationForTesting() const noexcept
	{
		TransferAllocationFailureManager = this;
	}
#endif

	static_assert(std::is_nothrow_copy_constructible_v<EditorDocumentIdentity>);
	static_assert(std::is_nothrow_copy_assignable_v<EditorDocumentIdentity>);
	static_assert(std::is_nothrow_copy_assignable_v<std::optional<EditorDocumentIdentity>>);
	static_assert(std::is_nothrow_move_constructible_v<EditorDocumentHost>);
	static_assert(std::is_nothrow_move_assignable_v<EditorDocumentPlacement>);
	static_assert(std::is_nothrow_move_constructible_v<EditorDocumentTransferResult>);

	size_t EditorDocumentIdentityHash::operator()(const EditorDocumentIdentity& Identity) const noexcept
	{
		size_t Hash = PulseForge::UUIDHash{}(Identity.Project);
		Hash ^= PulseForge::UUIDHash{}(Identity.Asset) + 0x9e3779b9u + (Hash << 6u) + (Hash >> 2u);
		Hash ^= static_cast<size_t>(Identity.Kind) + 0x9e3779b9u + (Hash << 6u) + (Hash >> 2u);
		return Hash;
	}

	size_t EditorDocumentPlacementKeyHash::operator()(const EditorDocumentPlacementKey& Key) const noexcept
	{
		size_t Hash = PulseForge::UUIDHash{}(Key.Project);
		Hash ^= static_cast<size_t>(Key.Kind) + 0x9e3779b9u + (Hash << 6u) + (Hash >> 2u);
		return Hash;
	}

	EditorDocumentManager::EditorDocumentManager()
	{
		(void)CreateHost(std::string(MainHostID), EditorDocumentHostRole::Main, EditorDocumentKind::Level);
	}

	EditorDocumentHost* EditorDocumentManager::CreateHost(std::string ID, EditorDocumentHostRole Role, EditorDocumentKind PlacementKind)
	{
		if (FindHost(ID)) return nullptr;
		m_Hosts.push_back({ std::move(ID), Role, PlacementKind, {}, std::nullopt, {}, std::nullopt });
		return &m_Hosts.back();
	}

	std::expected<std::string, std::string> EditorDocumentManager::CreateNativeHost(EditorDocumentKind PlacementKind)
	{
		if (RejectMutationDuringTransferPreparation()) return std::unexpected("Document hosts cannot change during transfer preparation");
		if (IsCloseOperationActive()) return std::unexpected("Document hosts cannot change during document close");
		const auto HostUUID = PulseForge::UUID::Generate();
		if (!HostUUID) return std::unexpected(HostUUID.error().Message);
		return CreateNativeHost("native:" + HostUUID->ToString(), PlacementKind);
	}

	std::expected<std::string, std::string> EditorDocumentManager::CreateNativeHost(
		std::string HostID, EditorDocumentKind PlacementKind, EditorDocumentHost::WindowBounds Bounds)
	{
		if (RejectMutationDuringTransferPreparation()) return std::unexpected("Document hosts cannot change during transfer preparation");
		if (IsCloseOperationActive()) return std::unexpected("Document hosts cannot change during document close");
		if (HostID.empty() || HostID == MainHostID || PlacementKind == EditorDocumentKind::Level || FindHost(HostID))
			return std::unexpected("The native document host identity or editor kind is invalid");
		EditorDocumentHost* Host = CreateHost(HostID, EditorDocumentHostRole::Native, PlacementKind);
		if (!Host) return std::unexpected("Could not register a unique native document host");
		Host->Bounds = std::move(Bounds);
		return std::move(HostID);
	}

	std::expected<EditorDocumentOpenResult, std::string> EditorDocumentManager::Open(const EditorDocumentIdentity& Identity,
		const std::filesystem::path& ProjectRelativePath, std::string DisplayName, EditorDocumentOpenPreference Preference)
	{
		if (RejectMutationDuringTransferPreparation()) return std::unexpected("Documents cannot open during transfer preparation");
		if (IsCloseOperationActive()) return std::unexpected("Documents cannot open during document close");
		if (auto Existing = m_Sessions.find(Identity); Existing != m_Sessions.end())
		{
			EditorDocumentHost* Host = FindHost(Existing->second.HostID);
			if (!Host) return std::unexpected("The open document session is detached from its registered host");
			if (std::count(Host->Documents.begin(), Host->Documents.end(), Identity) != 1)
				return std::unexpected("The open document session does not have exactly one host membership");
			try
			{
				auto PreparedPath = ProjectRelativePath;
				std::string PreparedFocus = Existing->second.HostID;
				EditorDocumentOpenResult Result{ Identity, Existing->second.HostID, false };
				Existing->second.ProjectRelativePath.swap(PreparedPath);
				Existing->second.DisplayName.swap(DisplayName);
				Host->ActiveDocument = Identity;
				m_FocusedHostID.swap(PreparedFocus);
				return Result;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(std::string("Could not prepare the existing document session: ") + Exception.what());
			}
			catch (...)
			{
				return std::unexpected("Could not prepare the existing document session.");
			}
		}
		if (m_NextSessionGeneration == std::numeric_limits<uint64_t>::max())
			return std::unexpected("The document session generation space is exhausted");

		const EditorDocumentPlacementKey PlacementKey{ Identity.Project, Identity.Kind };
		auto ExistingPlacement = m_RememberedPlacements.find(PlacementKey);
		std::string HostID;
		std::optional<EditorDocumentHost> NewHost;
		std::optional<EditorDocumentPlacement> PlacementToCommit;
		try
		{
			HostID = std::string(MainHostID);
			if (Identity.Kind != EditorDocumentKind::Level && Preference == EditorDocumentOpenPreference::NewNativeWindow)
			{
				HostID = "native:" + Identity.Project.ToString() + ":" + Identity.Asset.ToString() + ":" +
						 std::to_string(static_cast<uint32_t>(Identity.Kind));
				if (FindHost(HostID)) return std::unexpected("Could not register a unique native document host");
				const EditorDocumentHost::WindowBounds Bounds = ExistingPlacement == m_RememberedPlacements.end()
																	? EditorDocumentHost::WindowBounds{}
																	: ExistingPlacement->second.Bounds;
				const EditorDocumentHostRole RememberedRole =
					ExistingPlacement == m_RememberedPlacements.end() ? EditorDocumentHostRole::Native : ExistingPlacement->second.Role;
				NewHost.emplace(EditorDocumentHost{
					HostID, EditorDocumentHostRole::Native, Identity.Kind, Bounds, Identity.Project, {}, std::nullopt });
				PlacementToCommit.emplace(EditorDocumentPlacement{ RememberedRole, Bounds, HostID });
			}
			else if (Identity.Kind != EditorDocumentKind::Level && Preference == EditorDocumentOpenPreference::RememberedPlacement)
			{
				if (ExistingPlacement != m_RememberedPlacements.end() && ExistingPlacement->second.Role == EditorDocumentHostRole::Main)
					HostID = std::string(MainHostID);
				else
				{
					const EditorDocumentPlacement* Placement =
						ExistingPlacement == m_RememberedPlacements.end() ? nullptr : &ExistingPlacement->second;
					EditorDocumentHost* ReusableHost = Placement && Placement->LiveHostID ? FindHost(*Placement->LiveHostID) : nullptr;
					const bool Reusable =
						ReusableHost && ReusableHost->Role == EditorDocumentHostRole::Native &&
						ReusableHost->PlacementKind == Identity.Kind &&
						(!ReusableHost->Project || *ReusableHost->Project == Identity.Project) &&
						std::all_of(ReusableHost->Documents.begin(), ReusableHost->Documents.end(),
							[&Identity](const EditorDocumentIdentity& Existing) { return Existing.Project == Identity.Project; });
					if (Reusable) HostID = ReusableHost->ID;
					else
					{
						EditorDocumentHost::WindowBounds Bounds = Placement ? Placement->Bounds : EditorDocumentHost::WindowBounds{};
						if (!IsValidNativeWindowBounds(Bounds)) Bounds.HasValue = false;
						const auto HostUUID = PulseForge::UUID::Generate();
						if (!HostUUID) return std::unexpected(HostUUID.error().Message);
						HostID = "native:" + HostUUID->ToString();
						if (FindHost(HostID)) return std::unexpected("Could not register a unique native document host");
						NewHost.emplace(EditorDocumentHost{
							HostID, EditorDocumentHostRole::Native, Identity.Kind, std::move(Bounds), Identity.Project, {}, std::nullopt });
						PlacementToCommit.emplace(EditorDocumentPlacement{ EditorDocumentHostRole::Native, NewHost->Bounds, HostID });
					}
				}
			}
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not prepare document opening: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not prepare document opening.");
		}

		EditorDocumentHost* ExistingHost = NewHost ? nullptr : FindHost(HostID);
		if (!NewHost && !ExistingHost) return std::unexpected("The target document host is no longer registered");
		EditorDocumentHost* Host = NewHost ? &*NewHost : ExistingHost;
		if (Host->Project && *Host->Project != Identity.Project)
			return std::unexpected("Documents from different projects cannot share a host");
		for (const EditorDocumentIdentity& ExistingDocument : Host->Documents)
			if (ExistingDocument.Project != Identity.Project)
				return std::unexpected("Documents from different projects cannot share a host");
		if (Host->Role == EditorDocumentHostRole::Native && Host->PlacementKind != Identity.Kind)
			return std::unexpected("The target native document host does not support this document type");
		if (std::find(Host->Documents.begin(), Host->Documents.end(), Identity) != Host->Documents.end())
			return std::unexpected("The target document host already contains this identity without a session");
		EditorDocumentSession Session;
		EditorDocumentOpenResult Result;
		std::string PreparedFocus;
		try
		{
			Session = EditorDocumentSession{ Identity, ProjectRelativePath, std::move(DisplayName), HostID, 0, 0 };
			Session.SessionGeneration = m_NextSessionGeneration;
			Result = EditorDocumentOpenResult{ Identity, HostID, true };
			PreparedFocus = HostID;
			Host->Documents.reserve(Host->Documents.size() + 1);
			if (NewHost) m_Hosts.reserve(m_Hosts.size() + 1);
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not reserve document-host membership: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not reserve document-host membership.");
		}

		std::unordered_map<EditorDocumentIdentity, EditorDocumentSession, EditorDocumentIdentityHash>::iterator InsertedSession;
		try
		{
			auto Inserted = m_Sessions.emplace(Identity, std::move(Session));
			InsertedSession = Inserted.first;
			if (!Inserted.second) return std::unexpected("The document session already exists");
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not register the document session: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not register the document session.");
		}

		if (PlacementToCommit)
		{
			if (ExistingPlacement == m_RememberedPlacements.end())
			{
				try
				{
					(void)m_RememberedPlacements.try_emplace(PlacementKey, std::move(*PlacementToCommit));
				}
				catch (const std::exception& Exception)
				{
					m_Sessions.erase(InsertedSession);
					return std::unexpected(std::string("Could not save remembered document placement: ") + Exception.what());
				}
				catch (...)
				{
					m_Sessions.erase(InsertedSession);
					return std::unexpected("Could not save remembered document placement.");
				}
			}
			else ExistingPlacement->second = std::move(*PlacementToCommit);
		}

		if (NewHost)
		{
			NewHost->Documents.push_back(Identity);
			NewHost->ActiveDocument = Identity;
			m_Hosts.push_back(std::move(*NewHost));
			Host = &m_Hosts.back();
		}
		else
		{
			Host->Documents.push_back(Identity);
			Host->Project = Identity.Project;
			Host->ActiveDocument = Identity;
		}
		m_FocusedHostID.swap(PreparedFocus);
		++m_NextSessionGeneration;
		return Result;
	}

	bool EditorDocumentManager::UpdateAssetLocation(
		const EditorDocumentIdentity& Identity, const std::filesystem::path& ProjectRelativePath, std::string DisplayName)
	{
		if (RejectMutationDuringTransferPreparation()) return false;
		if (IsCloseOperationActive()) return false;
		EditorDocumentSession* Session = Find(Identity);
		if (!Session || Identity.Kind == EditorDocumentKind::Level) return false;
		Session->ProjectRelativePath = ProjectRelativePath;
		Session->DisplayName = std::move(DisplayName);
		return true;
	}

	bool EditorDocumentManager::RemoveEmptyNativeHost(std::string_view HostID)
	{
		if (RejectMutationDuringTransferPreparation()) return false;
		if (IsCloseOperationActive()) return false;
		const auto Host =
			std::find_if(m_Hosts.begin(), m_Hosts.end(), [HostID](const EditorDocumentHost& Candidate) { return Candidate.ID == HostID; });
		if (Host == m_Hosts.end() || Host->Role != EditorDocumentHostRole::Native || !Host->Documents.empty()) return false;

		for (auto& [Key, Placement] : m_RememberedPlacements)
		{
			(void)Key;
			if (Placement.LiveHostID && *Placement.LiveHostID == Host->ID)
			{
				Placement.Bounds = Host->Bounds;
				Placement.LiveHostID.reset();
			}
		}
		if (m_FocusedHostID == Host->ID) m_FocusedHostID = MainHostID;
		m_Hosts.erase(Host);
		return true;
	}

	std::expected<EditorDocumentTransferResult, std::string> EditorDocumentManager::Transfer(const EditorDocumentIdentity& Identity,
		std::string_view DestinationHostID, size_t DestinationIndex, const TransferPreparation& Prepare, EditorDocumentTransferCause Cause)
	{
		if (RejectMutationDuringTransferPreparation()) return std::unexpected("Documents cannot transfer during transfer preparation");
		if (IsCloseOperationActive()) return std::unexpected("Documents cannot transfer during document close");
		const auto InitialSession = m_Sessions.find(Identity);
		if (InitialSession == m_Sessions.end()) return std::unexpected("The document session is no longer open");
		if (Identity.Kind == EditorDocumentKind::Level)
			return std::unexpected("The Level Editor remains attached to the main document host");

		const uint64_t SessionGeneration = InitialSession->second.SessionGeneration;
		std::string SourceHostID;
		std::string DestinationID;
		try
		{
			SourceHostID = InitialSession->second.HostID;
			DestinationID = DestinationHostID;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not prepare document transfer: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not prepare document transfer.");
		}

		EditorDocumentHost* Destination = FindHost(DestinationID);
		EditorDocumentHost* Source = FindHost(SourceHostID);
		if (!Destination || !Source) return std::unexpected("Document transfer requires registered source and destination strips");
		if (std::count(Source->Documents.begin(), Source->Documents.end(), Identity) != 1)
			return std::unexpected("The source host does not contain exactly one document session membership");
		if (Source != Destination &&
			std::find(Destination->Documents.begin(), Destination->Documents.end(), Identity) != Destination->Documents.end())
			return std::unexpected("The destination host already contains the document session");
		if (Destination->Role == EditorDocumentHostRole::Native && Destination->PlacementKind != Identity.Kind)
			return std::unexpected("The destination native host does not support this document type");
		if (Destination->Project && *Destination->Project != Identity.Project)
			return std::unexpected("Documents from different projects cannot share a host");
		for (const EditorDocumentIdentity& ExistingDocument : Destination->Documents)
			if (ExistingDocument.Project != Identity.Project)
				return std::unexpected("Documents from different projects cannot share a host");
		if (Prepare)
		{
			bool Prepared = false;
			bool MutationAttempted = false;
			try
			{
				TransferPreparationScope CallbackScope(*this);
				if (!CallbackScope.Entered()) return std::unexpected("Document transfer preparation is already active");
				const auto CallbackSession = m_Sessions.find(Identity);
				const auto& ReadOnlyManager = static_cast<const EditorDocumentManager&>(*this);
				const EditorDocumentHost* CallbackDestination = ReadOnlyManager.FindHost(DestinationID);
				if (CallbackSession == m_Sessions.end() || CallbackSession->second.SessionGeneration != SessionGeneration ||
					CallbackSession->second.HostID != SourceHostID || !CallbackDestination)
					return std::unexpected("The document or destination changed before transfer preparation");
				const EditorDocumentHost* CallbackSource = ReadOnlyManager.FindHost(SourceHostID);
				if (!CallbackSource) return std::unexpected("The source host changed before transfer preparation");
				Prepared = Prepare(CallbackSession->second, *CallbackDestination);
				MutationAttempted = CallbackScope.MutationAttempted();
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(std::string("Document transfer preparation failed: ") + Exception.what());
			}
			catch (...)
			{
				return std::unexpected("Document transfer preparation failed with an unknown exception");
			}
			if (MutationAttempted) return std::unexpected("The transfer preparation callback attempted a prohibited manager mutation");
			if (!Prepared) return std::unexpected("The destination host could not prepare the document presentation");
		}

		auto SessionIt = m_Sessions.find(Identity);
		Source = FindHost(SourceHostID);
		Destination = FindHost(DestinationID);
		if (SessionIt == m_Sessions.end() || SessionIt->second.SessionGeneration != SessionGeneration ||
			SessionIt->second.HostID != SourceHostID || !Source || !Destination)
			return std::unexpected("The document hosts changed while preparing the transfer");
		if (std::count(Source->Documents.begin(), Source->Documents.end(), Identity) != 1 ||
			(Source != Destination &&
				std::find(Destination->Documents.begin(), Destination->Documents.end(), Identity) != Destination->Documents.end()))
			return std::unexpected("The document host membership changed while preparing the transfer");

		const auto ExistingPlacement = m_RememberedPlacements.find({ Identity.Project, Identity.Kind });
		const size_t ExistingIndex = static_cast<size_t>(
			std::distance(Source->Documents.begin(), std::find(Source->Documents.begin(), Source->Documents.end(), Identity)));
		const bool SameHost = Source == Destination;
		const size_t FinalDestinationIndex = SameHost ? (std::min)(DestinationIndex, Source->Documents.size() - 1)
													  : (std::min)(DestinationIndex, Destination->Documents.size());
		EditorDocumentTransferResult Result;
		EditorDocumentPlacement UpdatedPlacement;
		std::vector<EditorDocumentIdentity> PreparedSourceDocuments;
		std::vector<EditorDocumentIdentity> PreparedDestinationDocuments;
		std::string PreparedFocusedHostID;
		std::string PreparedSessionHostID;
		std::optional<EditorDocumentIdentity> PreparedSourceActive;
		std::optional<std::string> PreparedEmptiedSourceHost;
		try
		{
			Result.Changed = !SameHost || ExistingIndex != FinalDestinationIndex;
#if defined(PULSEFORGE_EDITOR_DOCUMENT_MANAGER_TESTING)
			if (TransferAllocationFailureManager == this)
			{
				TransferAllocationFailureManager = nullptr;
				throw std::bad_alloc();
			}
#endif
			if (SameHost)
			{
				PreparedSourceDocuments = Source->Documents;
				PreparedSourceDocuments.erase(PreparedSourceDocuments.begin() + static_cast<std::ptrdiff_t>(ExistingIndex));
				PreparedSourceDocuments.insert(
					PreparedSourceDocuments.begin() + static_cast<std::ptrdiff_t>(FinalDestinationIndex), Identity);
			}
			else
			{
				PreparedSourceDocuments = Source->Documents;
				PreparedSourceDocuments.erase(PreparedSourceDocuments.begin() + static_cast<std::ptrdiff_t>(ExistingIndex));
				PreparedDestinationDocuments = Destination->Documents;
				PreparedDestinationDocuments.insert(
					PreparedDestinationDocuments.begin() + static_cast<std::ptrdiff_t>(FinalDestinationIndex), Identity);
				PreparedSourceActive = Source->ActiveDocument;
				if (PreparedSourceActive == Identity)
				{
					PreparedSourceActive.reset();
					if (!PreparedSourceDocuments.empty())
						PreparedSourceActive = PreparedSourceDocuments[(std::min)(ExistingIndex, PreparedSourceDocuments.size() - 1)];
				}
				if (Source->Role == EditorDocumentHostRole::Native && PreparedSourceDocuments.empty())
					PreparedEmptiedSourceHost = SourceHostID;
				PreparedSessionHostID = DestinationID;
			}
			PreparedFocusedHostID = DestinationID;

			if (Cause == EditorDocumentTransferCause::User)
			{
				UpdatedPlacement.Role = Destination->Role;
				UpdatedPlacement.Bounds =
					Destination->Role == EditorDocumentHostRole::Native
						? Destination->Bounds
						: (Source->Role == EditorDocumentHostRole::Native
								  ? Source->Bounds
								  : (ExistingPlacement == m_RememberedPlacements.end() ? EditorDocumentHost::WindowBounds{}
																					   : ExistingPlacement->second.Bounds));
				if (Destination->Role == EditorDocumentHostRole::Native) UpdatedPlacement.LiveHostID = DestinationID;
			}
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not prepare an atomic document transfer: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not prepare an atomic document transfer.");
		}

		if (Cause == EditorDocumentTransferCause::User)
		{
			if (ExistingPlacement == m_RememberedPlacements.end())
			{
				try
				{
					(void)m_RememberedPlacements.try_emplace(
						EditorDocumentPlacementKey{ Identity.Project, Identity.Kind }, std::move(UpdatedPlacement));
				}
				catch (const std::exception& Exception)
				{
					return std::unexpected(std::string("Could not reserve remembered document placement: ") + Exception.what());
				}
				catch (...)
				{
					return std::unexpected("Could not reserve remembered document placement.");
				}
			}
			else ExistingPlacement->second = std::move(UpdatedPlacement);
		}

		if (SameHost)
		{
			Source->Documents.swap(PreparedSourceDocuments);
			Source->ActiveDocument = Identity;
		}
		else
		{
			Source->Documents.swap(PreparedSourceDocuments);
			Source->ActiveDocument = PreparedSourceActive;
			Destination->Documents.swap(PreparedDestinationDocuments);
			Destination->Project = Identity.Project;
			Destination->ActiveDocument = Identity;
			SessionIt->second.HostID.swap(PreparedSessionHostID);
		}
		m_FocusedHostID.swap(PreparedFocusedHostID);
		Result.EmptiedSourceHost = std::move(PreparedEmptiedSourceHost);
		return Result;
	}

	std::expected<size_t, std::string> EditorDocumentManager::RecoverAllToMain(std::string_view SourceHostID)
	{
		if (RejectMutationDuringTransferPreparation()) return std::unexpected("Documents cannot transfer during transfer preparation");
		if (IsCloseOperationActive()) return std::unexpected("Documents cannot transfer during document close");
		EditorDocumentHost* Source = FindHost(SourceHostID);
		EditorDocumentHost* Destination = FindHost(MainHostID);
		if (!Source || !Destination || Source == Destination || Source->Role != EditorDocumentHostRole::Native)
			return std::unexpected("Batch recovery is restricted to a registered native host returning to Main");
		if (Source->Documents.empty()) return size_t{ 0 };
		if (Source->Documents.size() > Destination->Documents.max_size() - Destination->Documents.size())
			return std::unexpected("The main document host cannot hold every document from the failed native window");

		std::optional<PulseForge::UUID> SourceProject;
		for (size_t Index = 0; Index < Source->Documents.size(); ++Index)
		{
			const EditorDocumentIdentity& Identity = Source->Documents[Index];
			const auto Session = m_Sessions.find(Identity);
			if (Identity.Kind == EditorDocumentKind::Level || Session == m_Sessions.end() || Session->second.HostID != Source->ID)
				return std::unexpected("The source host contains a document that cannot be transferred safely");
			if (std::find(Source->Documents.begin(), Source->Documents.begin() + static_cast<std::ptrdiff_t>(Index), Identity) !=
				Source->Documents.begin() + static_cast<std::ptrdiff_t>(Index))
				return std::unexpected("The source host contains a duplicate document identity");
			if (std::find(Destination->Documents.begin(), Destination->Documents.end(), Identity) != Destination->Documents.end())
				return std::unexpected("The destination host already contains a source document identity");
			if (SourceProject && *SourceProject != Identity.Project)
				return std::unexpected("Documents from different projects cannot share a host");
			SourceProject = Identity.Project;
			if (Destination->Project && *Destination->Project != Identity.Project)
				return std::unexpected("Documents from different projects cannot share a host");
			for (const EditorDocumentIdentity& Existing : Destination->Documents)
				if (Existing.Project != Identity.Project) return std::unexpected("Documents from different projects cannot share a host");
		}
		if (Source->ActiveDocument &&
			std::find(Source->Documents.begin(), Source->Documents.end(), *Source->ActiveDocument) == Source->Documents.end())
			return std::unexpected("The source host's active document is not in its document list");

		try
		{
			Destination->Documents.reserve(Destination->Documents.size() + Source->Documents.size());
			m_FocusedHostID.reserve(Destination->ID.size());
			for (const EditorDocumentIdentity& Identity : Source->Documents)
				m_Sessions.find(Identity)->second.HostID.reserve(Destination->ID.size());
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not reserve space for a safe document-host recovery: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not reserve space for a safe document-host recovery.");
		}

		const size_t TransferCount = Source->Documents.size();
		const std::optional<EditorDocumentIdentity> ActiveDocument =
			Source->ActiveDocument ? Source->ActiveDocument : std::optional<EditorDocumentIdentity>(Source->Documents.back());
		for (const EditorDocumentIdentity& Identity : Source->Documents)
		{
			Destination->Documents.push_back(Identity);
			m_Sessions.find(Identity)->second.HostID.assign(Destination->ID);
		}
		Destination->ActiveDocument = ActiveDocument;
		if (SourceProject) Destination->Project = *SourceProject;
		Source->Documents.clear();
		Source->ActiveDocument.reset();
		m_FocusedHostID.assign(Destination->ID);
		return TransferCount;
	}

	bool EditorDocumentManager::Activate(const EditorDocumentIdentity& Identity) noexcept
	{
		if (RejectMutationDuringTransferPreparation()) return false;
		if (IsCloseOperationActive()) return false;
		const auto Session = m_Sessions.find(Identity);
		if (Session == m_Sessions.end()) return false;
		EditorDocumentHost* Host = FindHost(Session->second.HostID);
		if (!Host) return false;
		Host->ActiveDocument = Identity;
		m_FocusedHostID = Host->ID;
		return true;
	}

	bool EditorDocumentManager::FocusHost(std::string_view HostID) noexcept
	{
		if (RejectMutationDuringTransferPreparation()) return false;
		if (IsCloseOperationActive()) return false;
		if (!FindHost(HostID)) return false;
		m_FocusedHostID = HostID;
		return true;
	}

	bool EditorDocumentManager::SetRememberedPlacement(const PulseForge::UUID& Project, EditorDocumentKind Kind,
		EditorDocumentHostRole Role, EditorDocumentHost::WindowBounds Bounds, std::optional<std::string> LiveHostID)
	{
		if (RejectMutationDuringTransferPreparation()) return false;
		if (IsCloseOperationActive()) return false;
		if (Project.IsNil() || Kind == EditorDocumentKind::Level ||
			(Role != EditorDocumentHostRole::Main && Role != EditorDocumentHostRole::Native))
			return false;
		if (Role == EditorDocumentHostRole::Main)
		{
			if (LiveHostID)
			{
				const EditorDocumentHost* Host = FindHost(*LiveHostID);
				if (!Host || Host->Role != EditorDocumentHostRole::Native || Host->PlacementKind != Kind ||
					(Host->Project && *Host->Project != Project) ||
					!std::all_of(Host->Documents.begin(), Host->Documents.end(),
						[&Project](const auto& Identity) { return Identity.Project == Project; }))
					return false;
			}
			if (!IsValidNativeWindowBounds(Bounds)) Bounds = {};
			m_RememberedPlacements[{ Project, Kind }] = EditorDocumentPlacement{ Role, std::move(Bounds), std::move(LiveHostID) };
			return true;
		}
		if (LiveHostID)
		{
			const EditorDocumentHost* Host = FindHost(*LiveHostID);
			if (!Host || Host->Role != EditorDocumentHostRole::Native || Host->PlacementKind != Kind ||
				(Host->Project && *Host->Project != Project) ||
				!std::all_of(Host->Documents.begin(), Host->Documents.end(),
					[&Project](const auto& Identity) { return Identity.Project == Project; }))
				return false;
		}
		if (!IsValidNativeWindowBounds(Bounds)) Bounds.HasValue = false;
		m_RememberedPlacements[{ Project, Kind }] = EditorDocumentPlacement{ Role, std::move(Bounds), std::move(LiveHostID) };
		return true;
	}

	bool EditorDocumentManager::SetHostBounds(std::string_view HostID, EditorDocumentHost::WindowBounds Bounds)
	{
		if (RejectMutationDuringTransferPreparation()) return false;
		if (IsCloseOperationActive()) return false;
		EditorDocumentHost* Host = FindHost(HostID);
		if (!Host || Host->Role != EditorDocumentHostRole::Native) return false;
		const auto& Current = Host->Bounds;
		if (Current.X == Bounds.X && Current.Y == Bounds.Y && Current.Width == Bounds.Width && Current.Height == Bounds.Height &&
			Current.DpiScale == Bounds.DpiScale && Current.MonitorName == Bounds.MonitorName && Current.Maximized == Bounds.Maximized &&
			Current.HasValue == Bounds.HasValue)
			return false;
		Host->Bounds = std::move(Bounds);
		for (auto& [Key, Placement] : m_RememberedPlacements)
			if (Placement.LiveHostID && *Placement.LiveHostID == Host->ID && Key.Kind == Host->PlacementKind)
				Placement.Bounds = Host->Bounds;
		return true;
	}

	std::vector<std::pair<EditorDocumentKind, EditorDocumentPlacement>> EditorDocumentManager::GetRememberedPlacements(
		const PulseForge::UUID& Project) const
	{
		std::vector<std::pair<EditorDocumentKind, EditorDocumentPlacement>> Placements;
		for (const auto& [Key, Placement] : m_RememberedPlacements)
			if (Key.Project == Project)
			{
				EditorDocumentPlacement Persisted = Placement;
				Persisted.LiveHostID.reset();
				Placements.emplace_back(Key.Kind, std::move(Persisted));
			}
		return Placements;
	}

	bool EditorDocumentManager::MarkEdited(const EditorDocumentIdentity& Identity) noexcept
	{
		if (m_CloseTransitionActive) return false;
		const auto Session = m_Sessions.find(Identity);
		if (Session == m_Sessions.end() || Session->second.Revision == std::numeric_limits<uint64_t>::max()) return false;
		++Session->second.Revision;
		return true;
	}

	bool EditorDocumentManager::MarkSaved(const EditorDocumentIdentity& Identity, uint64_t Revision) noexcept
	{
		if (RejectMutationDuringTransferPreparation()) return false;
		if (IsCloseOperationActive()) return false;
		EditorDocumentSession* Session = Find(Identity);
		if (!Session || Revision > Session->Revision) return false;
		Session->SavedRevision = Revision;
		return true;
	}

	std::expected<std::vector<std::string>, std::string> EditorDocumentManager::ClearProject(const PulseForge::UUID& Project)
	{
		if (RejectMutationDuringTransferPreparation())
			return std::unexpected("Project documents cannot be cleared during transfer preparation.");
		if (IsCloseOperationActive()) return std::unexpected("Project documents cannot be cleared during a document close transition.");
		size_t ProjectSessionCount = 0;
		for (const auto& [Identity, Session] : m_Sessions)
		{
			(void)Session;
			if (Identity.Project == Project) ++ProjectSessionCount;
		}
		std::vector<std::unique_ptr<EditorDocumentEditorState>> RetiredEditorStates;
		std::vector<std::string> EmptyHosts;
		try
		{
			RetiredEditorStates.reserve(ProjectSessionCount);
			EmptyHosts.reserve(m_Hosts.size());
			for (const EditorDocumentHost& Host : m_Hosts)
			{
				if (Host.Role != EditorDocumentHostRole::Native || Host.Documents.empty()) continue;
				bool HasProjectDocument = false;
				bool HasOtherProjectDocument = false;
				for (size_t Index = 0; Index < Host.Documents.size(); ++Index)
				{
					const EditorDocumentIdentity& Identity = Host.Documents[Index];
					const auto PreviousEnd = Host.Documents.begin() + static_cast<std::ptrdiff_t>(Index);
					if (std::find(Host.Documents.begin(), PreviousEnd, Identity) != PreviousEnd)
						return std::unexpected("A project document host contains a duplicate session reference.");
					if (Identity.Project != Project)
					{
						HasOtherProjectDocument = true;
						continue;
					}
					HasProjectDocument = true;
					const auto Session = m_Sessions.find(Identity);
					if (Session == m_Sessions.end() || Session->second.HostID != Host.ID)
						return std::unexpected("A project document host contains an invalid session reference.");
				}
				if (HasProjectDocument && !HasOtherProjectDocument) EmptyHosts.push_back(Host.ID);
			}

			for (const auto& [Identity, Session] : m_Sessions)
			{
				if (Identity.Project != Project) continue;
				const EditorDocumentHost* Host = FindHost(Session.HostID);
				if (!Host || std::find(Host->Documents.begin(), Host->Documents.end(), Identity) == Host->Documents.end())
					return std::unexpected("A project document is detached from its registered host.");
			}
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not prepare project document cleanup: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not prepare project document cleanup.");
		}

		for (auto Session = m_Sessions.begin(); Session != m_Sessions.end();)
		{
			if (Session->first.Project != Project)
			{
				++Session;
				continue;
			}

			RetiredEditorStates.push_back(std::move(Session->second.EditorState));
			RemoveSessionFromHost(Session->first, Session->second.HostID);
			Session = m_Sessions.erase(Session);
		}
		if (EditorDocumentHost* MainHost = FindHost(MainHostID); MainHost && MainHost->Project == Project && MainHost->Documents.empty())
			MainHost->Project.reset();

		for (const std::string& HostID : EmptyHosts)
		{
			if (!RemoveEmptyNativeHost(HostID)) return std::unexpected("A prepared empty document host could not be removed.");
		}
		CloseMutationScope TeardownScope(*this);
		if (!TeardownScope.Entered()) return std::unexpected("Project document teardown could not acquire its mutation boundary.");
		RetiredEditorStates.clear();
		return EmptyHosts;
	}

	EditorDocumentCloseStatus EditorDocumentManager::Close(
		const EditorDocumentIdentity& Identity, EditorDocumentCloseChoice Choice, const SaveDocument& Save, const DiscardDocument& Discard)
	{
		if (RejectMutationDuringTransferPreparation()) return EditorDocumentCloseStatus::MutationRejectedDuringTransferPreparation;
		if (IsCloseOperationActive()) return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
		auto Session = m_Sessions.find(Identity);
		if (Session == m_Sessions.end()) return EditorDocumentCloseStatus::Missing;
		if (Identity.Kind == EditorDocumentKind::Level) return EditorDocumentCloseStatus::Protected;
		const uint64_t SessionGeneration = Session->second.SessionGeneration;

		if (Session->second.IsDirty())
		{
			if (Choice == EditorDocumentCloseChoice::Cancel) return EditorDocumentCloseStatus::Cancelled;
			if (Choice == EditorDocumentCloseChoice::Save)
			{
				const uint64_t SaveRevision = Session->second.Revision;
				if (!Save) return EditorDocumentCloseStatus::SaveFailed;
				Session = m_Sessions.end();
				bool Saved = false;
				try
				{
					CloseCallbackScope CallbackScope(*this);
					if (!CallbackScope.Entered()) return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
					const auto CallbackSession = m_Sessions.find(Identity);
					if (CallbackSession == m_Sessions.end()) return EditorDocumentCloseStatus::Missing;
					if (CallbackSession->second.SessionGeneration != SessionGeneration)
						return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
					Saved = Save(CallbackSession->second, SaveRevision);
				}
				catch (const std::exception&)
				{
					return EditorDocumentCloseStatus::SaveFailed;
				}
				catch (...)
				{
					return EditorDocumentCloseStatus::SaveFailed;
				}

				Session = m_Sessions.find(Identity);
				if (Session == m_Sessions.end()) return EditorDocumentCloseStatus::Missing;
				if (Session->second.SessionGeneration != SessionGeneration)
					return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
				if (!Saved) return EditorDocumentCloseStatus::SaveFailed;
				Session->second.SavedRevision = SaveRevision;
				if (Session->second.IsDirty()) return EditorDocumentCloseStatus::StillDirty;
			}
			else if (Choice == EditorDocumentCloseChoice::Discard)
			{
				if (!Discard) return EditorDocumentCloseStatus::DiscardFailed;
				const uint64_t RevisionBeforeDiscard = Session->second.Revision;
				const uint64_t SavedRevisionBeforeDiscard = Session->second.SavedRevision;
				Session = m_Sessions.end();
				bool Discarded = false;
				try
				{
					CloseCallbackScope CallbackScope(*this);
					if (!CallbackScope.Entered()) return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
					const auto CallbackSession = m_Sessions.find(Identity);
					if (CallbackSession == m_Sessions.end()) return EditorDocumentCloseStatus::Missing;
					if (CallbackSession->second.SessionGeneration != SessionGeneration)
						return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
					Discarded = Discard(CallbackSession->second);
				}
				catch (const std::exception&)
				{
					return EditorDocumentCloseStatus::DiscardFailed;
				}
				catch (...)
				{
					return EditorDocumentCloseStatus::DiscardFailed;
				}

				Session = m_Sessions.find(Identity);
				if (Session == m_Sessions.end()) return EditorDocumentCloseStatus::Missing;
				if (Session->second.SessionGeneration != SessionGeneration)
					return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
				if (!Discarded) return EditorDocumentCloseStatus::DiscardFailed;
				if (Session->second.Revision != RevisionBeforeDiscard) return EditorDocumentCloseStatus::StillDirty;
				Session->second.Revision = SavedRevisionBeforeDiscard;
			}
		}

		CloseMutationScope TeardownScope(*this);
		if (!TeardownScope.Entered()) return EditorDocumentCloseStatus::MutationRejectedDuringCloseTransition;
		std::unique_ptr<EditorDocumentEditorState> RetiredEditorState = std::move(Session->second.EditorState);
		RemoveSessionFromHost(Identity, Session->second.HostID);
		m_Sessions.erase(Session);
		RetiredEditorState.reset();
		return EditorDocumentCloseStatus::Closed;
	}

	std::expected<void, std::string> EditorDocumentManager::ClosePreparedBatch(
		std::span<const EditorDocumentIdentity> Identities, std::span<const std::string> HostIDs)
	{
		if (RejectMutationDuringTransferPreparation()) return std::unexpected("Documents cannot close during transfer preparation.");
		if (IsCloseOperationActive()) return std::unexpected("Documents cannot close during a document close transition.");
		for (size_t Index = 0; Index < Identities.size(); ++Index)
		{
			const EditorDocumentIdentity& Identity = Identities[Index];
			const auto PreviousEnd = Identities.begin() + static_cast<std::ptrdiff_t>(Index);
			if (std::find(Identities.begin(), PreviousEnd, Identity) != PreviousEnd)
				return std::unexpected("The close request contains a duplicate document.");
			const auto Session = m_Sessions.find(Identity);
			if (Session == m_Sessions.end() || Identity.Kind == EditorDocumentKind::Level || Session->second.IsDirty())
				return std::unexpected("A prepared document is missing, protected, or dirty.");
			const EditorDocumentHost* Host = FindHost(Session->second.HostID);
			if (!Host || std::find(Host->Documents.begin(), Host->Documents.end(), Identity) == Host->Documents.end())
				return std::unexpected("A prepared document is detached from its registered host.");
		}

		std::vector<std::unique_ptr<EditorDocumentEditorState>> RetiredEditorStates;
		try
		{
			RetiredEditorStates.reserve(Identities.size());
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not reserve document teardown state: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not reserve document teardown state.");
		}

		for (const EditorDocumentIdentity& Identity : Identities)
		{
			auto Session = m_Sessions.find(Identity);
			RetiredEditorStates.push_back(std::move(Session->second.EditorState));
			RemoveSessionFromHost(Identity, Session->second.HostID);
			m_Sessions.erase(Session);
		}
		for (const std::string& HostID : HostIDs)
			(void)RemoveEmptyNativeHost(HostID);
		CloseMutationScope TeardownScope(*this);
		if (!TeardownScope.Entered()) return std::unexpected("Document teardown could not acquire its mutation boundary.");
		RetiredEditorStates.clear();
		return {};
	}

	const EditorDocumentSession* EditorDocumentManager::Find(const EditorDocumentIdentity& Identity) const noexcept
	{
		const auto Session = m_Sessions.find(Identity);
		return Session == m_Sessions.end() ? nullptr : &Session->second;
	}

	EditorDocumentSession* EditorDocumentManager::Find(const EditorDocumentIdentity& Identity) noexcept
	{
		if (RejectMutationDuringTransferPreparation()) return nullptr;
		if (IsCloseOperationActive()) return nullptr;
		const auto Session = m_Sessions.find(Identity);
		return Session == m_Sessions.end() ? nullptr : &Session->second;
	}

	const EditorDocumentHost* EditorDocumentManager::FindHost(std::string_view HostID) const noexcept
	{
		const auto Host =
			std::find_if(m_Hosts.begin(), m_Hosts.end(), [HostID](const EditorDocumentHost& Candidate) { return Candidate.ID == HostID; });
		return Host == m_Hosts.end() ? nullptr : &*Host;
	}

	EditorDocumentHost* EditorDocumentManager::FindHost(std::string_view HostID) noexcept
	{
		if (RejectMutationDuringTransferPreparation()) return nullptr;
		if (IsCloseOperationActive()) return nullptr;
		const auto Host =
			std::find_if(m_Hosts.begin(), m_Hosts.end(), [HostID](const EditorDocumentHost& Candidate) { return Candidate.ID == HostID; });
		return Host == m_Hosts.end() ? nullptr : &*Host;
	}

	const EditorDocumentHost* EditorDocumentManager::GetFocusedHost() const noexcept
	{
		return FindHost(m_FocusedHostID);
	}

	void EditorDocumentManager::RemoveSessionFromHost(const EditorDocumentIdentity& Identity, const std::string& HostID) noexcept
	{
		const auto HostEntry =
			std::find_if(m_Hosts.begin(), m_Hosts.end(), [&HostID](const EditorDocumentHost& Candidate) { return Candidate.ID == HostID; });
		if (HostEntry == m_Hosts.end()) return;
		EditorDocumentHost* Host = &*HostEntry;
		const auto Existing = std::find(Host->Documents.begin(), Host->Documents.end(), Identity);
		if (Existing == Host->Documents.end()) return;
		const size_t OldIndex = static_cast<size_t>(std::distance(Host->Documents.begin(), Existing));
		Host->Documents.erase(Existing);
		if (Host->ActiveDocument == Identity)
		{
			Host->ActiveDocument.reset();
			if (!Host->Documents.empty()) Host->ActiveDocument = Host->Documents[(std::min)(OldIndex, Host->Documents.size() - 1)];
		}
	}
} // namespace PulseForgeEditor
