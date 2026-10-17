#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"

#include <algorithm>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	template<typename Enqueue>
	[[nodiscard]] bool QueueDocumentCloseContinuation(
		bool CloseSucceeded,
		std::function<void()>& PendingContinuation,
		Enqueue&& EnqueueAction)
	{
		if (!CloseSucceeded)
			return false;
		auto Continuation = std::exchange(PendingContinuation, {});
		if (Continuation)
			EnqueueAction(std::move(Continuation));
		return true;
	}

	class EditorDocumentCloseBatch final
	{
	public:
		[[nodiscard]] static std::expected<EditorDocumentCloseBatch, std::string> Prepare(
			const EditorDocumentManager& Documents,
			const EditorDocumentEditorRegistry& Editors,
			std::span<const EditorDocumentIdentity> Identities,
			bool AllowDirtyDocuments = false)
		{
			EditorDocumentCloseBatch Batch;
			std::unordered_set<EditorDocumentIdentity, EditorDocumentIdentityHash> Unique;
			Batch.m_Identities.reserve(Identities.size());
			Batch.m_SessionGenerations.reserve(Identities.size());
			Batch.m_SessionHostIDs.reserve(Identities.size());
			Batch.m_HostIDs.reserve(Identities.size());
			for (const EditorDocumentIdentity& Identity : Identities)
			{
				if (!Unique.insert(Identity).second)
					return std::unexpected("The close request contains a duplicate document.");
				const EditorDocumentSession* Session = Documents.Find(Identity);
				if (!Session || Identity.Kind == EditorDocumentKind::Level)
					return std::unexpected("A document in the close request is missing or cannot be closed.");
				if (!AllowDirtyDocuments && Session->IsDirty())
					return std::unexpected("Save or discard the dirty document before closing it.");
				const std::string HostID = Session->HostID;
				const uint64_t SessionGeneration = Session->SessionGeneration;
				const uint64_t Revision = Session->Revision;
				const uint64_t SavedRevision = Session->SavedRevision;
				if (Editors.IsRegistered(Identity.Kind))
				{
					if (!Session->EditorState)
						return std::unexpected("The registered document editor session is unavailable.");
					std::expected<void, std::string> Result;
					{
						EditorDocumentManager::CloseMutationScope Scope(Documents);
						if (!Scope.Entered()) return std::unexpected("A document close transition is already in progress.");
						Result = Editors.PrepareClose(*Session);
					}
					if (!Result)
						return std::unexpected(Result.error().empty()
							? "The document editor is not ready to close." : Result.error());
					Session = Documents.Find(Identity);
					if (!Session || Session->HostID != HostID || Session->SessionGeneration != SessionGeneration ||
						Session->Revision != Revision || Session->SavedRevision != SavedRevision)
						return std::unexpected("A document changed during close preflight.");
				}
				Batch.m_Identities.push_back(Identity);
				Batch.m_SessionGenerations.push_back(SessionGeneration);
				Batch.m_SessionHostIDs.push_back(HostID);
				if (std::find(Batch.m_HostIDs.begin(), Batch.m_HostIDs.end(), HostID) == Batch.m_HostIDs.end())
					Batch.m_HostIDs.push_back(HostID);
			}
			return Batch;
		}

		[[nodiscard]] std::expected<void, std::string> Commit(
			EditorDocumentManager& Documents,
			const EditorDocumentEditorRegistry& Editors) &&
		{
			for (size_t Index = 0; Index < m_Identities.size(); ++Index)
			{
				const EditorDocumentIdentity& Identity = m_Identities[Index];
				const EditorDocumentSession* Session = Documents.Find(Identity);
				if (!Session || Session->IsDirty() || Session->HostID != m_SessionHostIDs[Index] ||
					Session->SessionGeneration != m_SessionGenerations[Index] ||
					(Editors.IsRegistered(Identity.Kind) != (Session->EditorState != nullptr)))
					return std::unexpected("A prepared document changed before its close could be committed.");
			}
			return Documents.ClosePreparedBatch(m_Identities, m_HostIDs);
		}

		[[nodiscard]] std::span<const EditorDocumentIdentity> GetIdentities() const noexcept
		{
			return m_Identities;
		}

	private:
		std::vector<EditorDocumentIdentity> m_Identities;
		std::vector<uint64_t> m_SessionGenerations;
		std::vector<std::string> m_SessionHostIDs;
		std::vector<std::string> m_HostIDs;
	};

	template<typename ResolveChanges, typename Enqueue>
	[[nodiscard]] std::expected<void, std::string> ResolveDocumentCloseBatchWithChanges(
		EditorDocumentManager& Documents,
		const EditorDocumentEditorRegistry& Editors,
		std::span<const EditorDocumentIdentity> Identities,
		bool PreflightBeforeResolvingChanges,
		ResolveChanges&& ResolveDirtyChanges,
		std::function<void()>& PendingContinuation,
		Enqueue&& EnqueueAction)
	{
		std::optional<EditorDocumentCloseBatch> Prepared;
		if (PreflightBeforeResolvingChanges)
		{
			// Discard mutates working models, so run every close veto before resolving it.
			auto Result = EditorDocumentCloseBatch::Prepare(Documents, Editors, Identities, true);
			if (!Result)
				return std::unexpected(Result.error());
			Prepared = std::move(*Result);
		}

		if (auto Result = std::forward<ResolveChanges>(ResolveDirtyChanges)(); !Result)
			return std::unexpected(Result.error());

		if (!Prepared)
		{
			auto Result = EditorDocumentCloseBatch::Prepare(Documents, Editors, Identities);
			if (!Result)
				return std::unexpected(Result.error());
			Prepared = std::move(*Result);
		}

		if (auto Closed = std::move(*Prepared).Commit(Documents, Editors); !Closed)
			return std::unexpected(Closed.error());
		(void)QueueDocumentCloseContinuation(true, PendingContinuation, std::forward<Enqueue>(EnqueueAction));
		return {};
	}

	template<typename Enqueue>
	[[nodiscard]] std::expected<void, std::string> ResolveDocumentCloseBatch(
		EditorDocumentManager& Documents,
		const EditorDocumentEditorRegistry& Editors,
		std::span<const EditorDocumentIdentity> Identities,
		std::function<void()>& PendingContinuation,
		Enqueue&& EnqueueAction)
	{
		return ResolveDocumentCloseBatchWithChanges(
			Documents, Editors, Identities, false,
			[]() -> std::expected<void, std::string> { return {}; },
			PendingContinuation, std::forward<Enqueue>(EnqueueAction));
	}
}
