#include "Editor/EditorDocumentCloseRequest.h"

#include <algorithm>
#include <utility>

namespace PulseForgeEditor
{
	void EditorDocumentCloseRequest::BeginCloseDocuments(
		std::vector<EditorDocumentIdentity> Documents,
		std::string DialogHostID,
		Action Continuation)
	{
		Begin(Mode::CloseDocuments, std::move(Documents), std::move(DialogHostID), std::move(Continuation));
	}

	void EditorDocumentCloseRequest::BeginCloseAllDocuments(
		std::vector<EditorDocumentIdentity> Documents,
		std::string DialogHostID,
		Action Continuation)
	{
		BeginCloseDocuments(std::move(Documents), std::move(DialogHostID), std::move(Continuation));
		m_IncludeCurrentDirtyDocuments = true;
	}

	void EditorDocumentCloseRequest::BeginResolveChanges(std::string DialogHostID, Action Continuation)
	{
		Begin(Mode::ResolveChanges, {}, std::move(DialogHostID), std::move(Continuation));
	}

	void EditorDocumentCloseRequest::Begin(
		Mode RequestMode,
		std::vector<EditorDocumentIdentity> Documents,
		std::string DialogHostID,
		Action Continuation)
	{
		Reset();
		m_Mode = RequestMode;
		m_Documents = std::move(Documents);
		m_DialogHostID = std::move(DialogHostID);
		m_Continuation = std::move(Continuation);
		m_DialogOpenRequested = true;
	}

	bool EditorDocumentCloseRequest::HasPendingRequest() const noexcept
	{
		return m_Mode != Mode::None;
	}

	bool EditorDocumentCloseRequest::ClosesDocuments() const noexcept
	{
		return m_Mode == Mode::CloseDocuments;
	}

	bool EditorDocumentCloseRequest::ConsumeDialogOpenRequest() noexcept
	{
		return std::exchange(m_DialogOpenRequested, false);
	}

	bool EditorDocumentCloseRequest::ShouldOfferCloseRetry(bool NoDirtyDocuments) const noexcept
	{
		return NoDirtyDocuments && ClosesDocuments();
	}

	void EditorDocumentCloseRequest::SetError(std::string Error)
	{
		m_Error = std::move(Error);
	}

	void EditorDocumentCloseRequest::SetDialogHostIfEmpty(std::string DialogHostID)
	{
		if (m_DialogHostID.empty()) m_DialogHostID = std::move(DialogHostID);
	}

	void EditorDocumentCloseRequest::Cancel() noexcept
	{
		Reset();
	}

	std::vector<EditorDocumentIdentity> EditorDocumentCloseRequest::GetTargetDocuments(
		std::span<const EditorDocumentIdentity> CurrentDirtyDocuments) const
	{
		std::vector<EditorDocumentIdentity> Result;
		if (m_Mode == Mode::ResolveChanges || m_IncludeCurrentDirtyDocuments)
			Result.assign(CurrentDirtyDocuments.begin(), CurrentDirtyDocuments.end());
		if (m_Mode == Mode::CloseDocuments)
			for (const EditorDocumentIdentity& Identity : m_Documents)
				if (std::find(Result.begin(), Result.end(), Identity) == Result.end()) Result.push_back(Identity);
		return Result;
	}

	std::vector<EditorDocumentIdentity> EditorDocumentCloseRequest::GetDirtyPromptDocuments(
		std::span<const EditorDocumentIdentity> CurrentDirtyDocuments,
		const std::function<bool(const EditorDocumentIdentity&)>& IsDirty) const
	{
		std::vector<EditorDocumentIdentity> Result;
		if (m_Mode == Mode::ResolveChanges || m_IncludeCurrentDirtyDocuments)
			Result.assign(CurrentDirtyDocuments.begin(), CurrentDirtyDocuments.end());
		else if (m_Mode == Mode::CloseDocuments)
			for (const EditorDocumentIdentity& Identity : m_Documents)
				if (IsDirty && IsDirty(Identity)) Result.push_back(Identity);
		return Result;
	}

	std::expected<EditorDocumentCloseRequest::ResolveResult, std::string> EditorDocumentCloseRequest::Resolve(
		EditorDocumentManager& Documents,
		const EditorDocumentEditorRegistry& Editors,
		EditorDocumentCloseChoice Choice,
		std::span<const EditorDocumentIdentity> CurrentDirtyDocuments,
		const ResolveChanges& ResolveDirtyChanges,
		const EnqueueAction& Enqueue)
	{
		if (!HasPendingRequest()) return std::unexpected("There is no pending document-close request.");
		if (Choice != EditorDocumentCloseChoice::Save && Choice != EditorDocumentCloseChoice::Discard)
			return std::unexpected("Choose Save or Discard to resolve the pending document-close request.");
		if (!ResolveDirtyChanges || !Enqueue) return std::unexpected("The document-close operations are unavailable.");

		const std::vector<EditorDocumentIdentity> TargetDocuments = GetTargetDocuments(CurrentDirtyDocuments);
		if (ClosesDocuments())
		{
			std::vector<EditorDocumentIdentity> CloseableDocuments;
			CloseableDocuments.reserve(TargetDocuments.size());
			for (const EditorDocumentIdentity& Identity : TargetDocuments)
				if (Documents.Find(Identity)) CloseableDocuments.push_back(Identity);

			const auto ResolveChanges = [this, &TargetDocuments, &ResolveDirtyChanges, Choice]()
				-> std::expected<void, std::string>
			{ return ResolveDirtyChanges(TargetDocuments, Choice); };
			auto Closed = ResolveDocumentCloseBatchWithChanges(
				Documents, Editors, CloseableDocuments, Choice == EditorDocumentCloseChoice::Discard,
				ResolveChanges, m_Continuation, Enqueue);
			if (!Closed)
			{
				SetError(Closed.error());
				return std::unexpected(m_Error);
			}
			Reset();
			return ResolveResult{ true };
		}

		if (auto Resolved = ResolveDirtyChanges(TargetDocuments, Choice); !Resolved)
		{
			SetError(Resolved.error());
			return std::unexpected(m_Error);
		}

		Action Continuation = std::exchange(m_Continuation, {});
		Reset();
		if (Continuation) Enqueue(std::move(Continuation));
		return ResolveResult{ false };
	}

	void EditorDocumentCloseRequest::Reset() noexcept
	{
		m_Mode = Mode::None;
		m_Documents.clear();
		m_DialogHostID.clear();
		m_Error.clear();
		m_Continuation = {};
		m_IncludeCurrentDirtyDocuments = false;
		m_DialogOpenRequested = false;
	}
} // namespace PulseForgeEditor
