#include "Core/PulseForgePCH.h"
#include "Editor/EditorDocumentManager.h"

#include <algorithm>
#include <limits>

namespace PulseForgeEditor
{
	size_t EditorDocumentIdentityHash::operator()(const EditorDocumentIdentity& Identity) const noexcept
	{
		size_t Hash = PulseForge::UUIDHash{}(Identity.Project);
		Hash ^= PulseForge::UUIDHash{}(Identity.Asset) + 0x9e3779b9u + (Hash << 6u) + (Hash >> 2u);
		Hash ^= static_cast<size_t>(Identity.Kind) + 0x9e3779b9u + (Hash << 6u) + (Hash >> 2u);
		return Hash;
	}

	EditorDocumentManager::EditorDocumentManager()
	{
		(void)CreateHost(std::string(MainHostID), EditorDocumentHostRole::Main, EditorDocumentKind::Level);
	}

	EditorDocumentHost* EditorDocumentManager::CreateHost(
		std::string ID,
		EditorDocumentHostRole Role,
		EditorDocumentKind PlacementKind)
	{
		if (FindHost(ID))
			return nullptr;
		m_Hosts.push_back({ std::move(ID), Role, PlacementKind, {}, std::nullopt });
		return &m_Hosts.back();
	}

	std::expected<std::string, std::string> EditorDocumentManager::CreateNativeHost(
		EditorDocumentKind PlacementKind)
	{
		const auto HostUUID = PulseForge::UUID::Generate();
		if (!HostUUID)
			return std::unexpected(HostUUID.error().Message);
		std::string ID = "native:" + HostUUID->ToString();
		if (!CreateHost(ID, EditorDocumentHostRole::Native, PlacementKind))
			return std::unexpected("Could not register a unique native document host");
		return ID;
	}

	std::expected<EditorDocumentOpenResult, std::string> EditorDocumentManager::Open(
		const EditorDocumentIdentity& Identity,
		const std::filesystem::path& ProjectRelativePath,
		std::string DisplayName,
		EditorDocumentOpenPreference Preference)
	{
		if (auto Existing = m_Sessions.find(Identity); Existing != m_Sessions.end())
		{
			Existing->second.ProjectRelativePath = ProjectRelativePath;
			Existing->second.DisplayName = std::move(DisplayName);
			if (EditorDocumentHost* Host = FindHost(Existing->second.HostID))
				Host->ActiveDocument = Identity;
			m_FocusedHostID = Existing->second.HostID;
			return EditorDocumentOpenResult{ Identity, Existing->second.HostID, false };
		}

		std::string HostID(MainHostID);
		if (Identity.Kind != EditorDocumentKind::Level)
		{
			if (Preference == EditorDocumentOpenPreference::NewNativeWindow)
			{
				auto Host = CreateNativeHost(Identity.Kind);
				if (!Host)
					return std::unexpected(Host.error());
				HostID = std::move(*Host);
			}
			else if (Preference == EditorDocumentOpenPreference::RememberedPlacement)
			{
				const auto Remembered = m_RememberedHosts.find(Identity.Kind);
				const EditorDocumentHost* Host = Remembered == m_RememberedHosts.end()
					? nullptr : FindHost(Remembered->second);
				if (Host && Host->Role == EditorDocumentHostRole::Native && Host->PlacementKind == Identity.Kind)
					HostID = Host->ID;
				else
				{
					auto CreatedHost = CreateNativeHost(Identity.Kind);
					if (!CreatedHost)
						return std::unexpected(CreatedHost.error());
					HostID = std::move(*CreatedHost);
					m_RememberedHosts[Identity.Kind] = HostID;
				}
			}
		}

		EditorDocumentHost* Host = FindHost(HostID);
		if (!Host)
			return std::unexpected("The target document host is no longer registered");
		EditorDocumentSession Session{ Identity, ProjectRelativePath, std::move(DisplayName), HostID, 0, 0 };
		m_Sessions.emplace(Identity, std::move(Session));
		Host->Documents.push_back(Identity);
		Host->ActiveDocument = Identity;
		m_FocusedHostID = HostID;
		return EditorDocumentOpenResult{ Identity, std::move(HostID), true };
	}

	bool EditorDocumentManager::RemoveEmptyNativeHost(std::string_view HostID) noexcept
	{
		const auto Host = std::find_if(m_Hosts.begin(), m_Hosts.end(), [HostID](const EditorDocumentHost& Candidate)
		{
			return Candidate.ID == HostID;
		});
		if (Host == m_Hosts.end() || Host->Role != EditorDocumentHostRole::Native || !Host->Documents.empty())
			return false;

		for (auto Remembered = m_RememberedHosts.begin(); Remembered != m_RememberedHosts.end();)
		{
			if (Remembered->second == Host->ID)
				Remembered = m_RememberedHosts.erase(Remembered);
			else
				++Remembered;
		}
		if (m_FocusedHostID == Host->ID)
			m_FocusedHostID = MainHostID;
		m_Hosts.erase(Host);
		return true;
	}

	std::expected<EditorDocumentTransferResult, std::string> EditorDocumentManager::Transfer(
		const EditorDocumentIdentity& Identity,
		std::string_view DestinationHostID,
		size_t DestinationIndex,
		const TransferPreparation& Prepare)
	{
		auto SessionIt = m_Sessions.find(Identity);
		if (SessionIt == m_Sessions.end())
			return std::unexpected("The document session is no longer open");
		if (Identity.Kind == EditorDocumentKind::Level)
			return std::unexpected("The Level Editor remains attached to the main document host");

		EditorDocumentHost* Destination = FindHost(DestinationHostID);
		EditorDocumentHost* Source = FindHost(SessionIt->second.HostID);
		if (!Destination || !Source)
			return std::unexpected("Document transfer requires registered source and destination strips");
		if (Prepare && !Prepare(SessionIt->second, *Destination))
			return std::unexpected("The destination host could not prepare the document presentation");

		if (Source == Destination)
		{
			const auto Existing = std::find(Source->Documents.begin(), Source->Documents.end(), Identity);
			if (Existing == Source->Documents.end())
				return std::unexpected("The source host does not contain the document session");
			const size_t OldIndex = static_cast<size_t>(std::distance(Source->Documents.begin(), Existing));
			Source->Documents.erase(Existing);
			DestinationIndex = (std::min)(DestinationIndex, Source->Documents.size());
			Source->Documents.insert(Source->Documents.begin() + static_cast<std::ptrdiff_t>(DestinationIndex), Identity);
			Source->ActiveDocument = Identity;
			m_FocusedHostID = Source->ID;
			return EditorDocumentTransferResult{ OldIndex != DestinationIndex, std::nullopt };
		}

		const std::string SourceID = Source->ID;
		const auto Existing = std::find(Source->Documents.begin(), Source->Documents.end(), Identity);
		if (Existing == Source->Documents.end())
			return std::unexpected("The source host does not contain the document session");
		const size_t OldIndex = static_cast<size_t>(std::distance(Source->Documents.begin(), Existing));
		Source->Documents.erase(Existing);
		if (Source->ActiveDocument == Identity)
		{
			Source->ActiveDocument.reset();
			if (!Source->Documents.empty())
				Source->ActiveDocument = Source->Documents[(std::min)(OldIndex, Source->Documents.size() - 1)];
		}
		DestinationIndex = (std::min)(DestinationIndex, Destination->Documents.size());
		Destination->Documents.insert(
			Destination->Documents.begin() + static_cast<std::ptrdiff_t>(DestinationIndex), Identity);
		Destination->ActiveDocument = Identity;
		SessionIt->second.HostID = Destination->ID;
		m_FocusedHostID = Destination->ID;
		return EditorDocumentTransferResult{
			true,
			Source->Role == EditorDocumentHostRole::Native && Source->Documents.empty()
				? std::optional<std::string>(SourceID) : std::nullopt
		};
	}

	bool EditorDocumentManager::Activate(const EditorDocumentIdentity& Identity) noexcept
	{
		const auto Session = m_Sessions.find(Identity);
		if (Session == m_Sessions.end())
			return false;
		EditorDocumentHost* Host = FindHost(Session->second.HostID);
		if (!Host)
			return false;
		Host->ActiveDocument = Identity;
		m_FocusedHostID = Host->ID;
		return true;
	}

	bool EditorDocumentManager::FocusHost(std::string_view HostID) noexcept
	{
		if (!FindHost(HostID))
			return false;
		m_FocusedHostID = HostID;
		return true;
	}

	bool EditorDocumentManager::MarkEdited(const EditorDocumentIdentity& Identity) noexcept
	{
		EditorDocumentSession* Session = Find(Identity);
		if (!Session || Session->Revision == std::numeric_limits<uint64_t>::max())
			return false;
		++Session->Revision;
		return true;
	}

	bool EditorDocumentManager::MarkSaved(const EditorDocumentIdentity& Identity, uint64_t Revision) noexcept
	{
		EditorDocumentSession* Session = Find(Identity);
		if (!Session || Revision > Session->Revision)
			return false;
		Session->SavedRevision = Revision;
		return true;
	}

	std::vector<std::string> EditorDocumentManager::ClearProject(const PulseForge::UUID& Project)
	{
		std::vector<std::string> CandidateHosts;
		for (auto Session = m_Sessions.begin(); Session != m_Sessions.end();)
		{
			if (Session->first.Project != Project)
			{
				++Session;
				continue;
			}

			const std::string HostID = Session->second.HostID;
			RemoveSessionFromHost(Session->first, HostID);
			if (HostID != MainHostID &&
				std::find(CandidateHosts.begin(), CandidateHosts.end(), HostID) == CandidateHosts.end())
				CandidateHosts.push_back(HostID);
			Session = m_Sessions.erase(Session);
		}

		std::vector<std::string> EmptyHosts;
		for (const std::string& HostID : CandidateHosts)
		{
			if (RemoveEmptyNativeHost(HostID))
				EmptyHosts.push_back(HostID);
		}
		return EmptyHosts;
	}

	EditorDocumentCloseStatus EditorDocumentManager::Close(
		const EditorDocumentIdentity& Identity,
		EditorDocumentCloseChoice Choice,
		const SaveDocument& Save,
		const DiscardDocument& Discard)
	{
		auto Session = m_Sessions.find(Identity);
		if (Session == m_Sessions.end())
			return EditorDocumentCloseStatus::Missing;
		if (Identity.Kind == EditorDocumentKind::Level)
			return EditorDocumentCloseStatus::Protected;

		if (Session->second.IsDirty())
		{
			if (Choice == EditorDocumentCloseChoice::Cancel)
				return EditorDocumentCloseStatus::Cancelled;
			if (Choice == EditorDocumentCloseChoice::Save)
			{
				const uint64_t SaveRevision = Session->second.Revision;
				if (!Save || !Save(Session->second, SaveRevision))
					return EditorDocumentCloseStatus::SaveFailed;
				Session->second.SavedRevision = SaveRevision;
				if (Session->second.IsDirty())
					return EditorDocumentCloseStatus::StillDirty;
			}
			else if (Choice == EditorDocumentCloseChoice::Discard)
			{
				if (!Discard || !Discard(Session->second))
					return EditorDocumentCloseStatus::DiscardFailed;
				Session->second.Revision = Session->second.SavedRevision;
			}
		}

		const std::string HostID = Session->second.HostID;
		RemoveSessionFromHost(Identity, HostID);
		m_Sessions.erase(Session);
		return EditorDocumentCloseStatus::Closed;
	}

	const EditorDocumentSession* EditorDocumentManager::Find(const EditorDocumentIdentity& Identity) const noexcept
	{
		const auto Session = m_Sessions.find(Identity);
		return Session == m_Sessions.end() ? nullptr : &Session->second;
	}

	EditorDocumentSession* EditorDocumentManager::Find(const EditorDocumentIdentity& Identity) noexcept
	{
		const auto Session = m_Sessions.find(Identity);
		return Session == m_Sessions.end() ? nullptr : &Session->second;
	}

	const EditorDocumentHost* EditorDocumentManager::FindHost(std::string_view HostID) const noexcept
	{
		const auto Host = std::find_if(m_Hosts.begin(), m_Hosts.end(), [HostID](const EditorDocumentHost& Candidate)
		{
			return Candidate.ID == HostID;
		});
		return Host == m_Hosts.end() ? nullptr : &*Host;
	}

	EditorDocumentHost* EditorDocumentManager::FindHost(std::string_view HostID) noexcept
	{
		const auto Host = std::find_if(m_Hosts.begin(), m_Hosts.end(), [HostID](const EditorDocumentHost& Candidate)
		{
			return Candidate.ID == HostID;
		});
		return Host == m_Hosts.end() ? nullptr : &*Host;
	}

	const EditorDocumentHost* EditorDocumentManager::GetFocusedHost() const noexcept
	{
		return FindHost(m_FocusedHostID);
	}

	void EditorDocumentManager::RemoveSessionFromHost(
		const EditorDocumentIdentity& Identity,
		const std::string& HostID) noexcept
	{
		EditorDocumentHost* Host = FindHost(HostID);
		if (!Host)
			return;
		const auto Existing = std::find(Host->Documents.begin(), Host->Documents.end(), Identity);
		if (Existing == Host->Documents.end())
			return;
		const size_t OldIndex = static_cast<size_t>(std::distance(Host->Documents.begin(), Existing));
		Host->Documents.erase(Existing);
		if (Host->ActiveDocument == Identity)
		{
			Host->ActiveDocument.reset();
			if (!Host->Documents.empty())
				Host->ActiveDocument = Host->Documents[(std::min)(OldIndex, Host->Documents.size() - 1)];
		}
	}
}
