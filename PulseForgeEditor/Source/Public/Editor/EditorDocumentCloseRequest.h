#pragma once

#include "Editor/EditorDocumentCloseBatch.h"

#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace PulseForgeEditor
{
	class EditorDocumentCloseRequest final
	{
	public:
		using Action = std::function<void()>;
		using ResolveChanges =
			std::function<std::expected<void, std::string>(std::span<const EditorDocumentIdentity>, EditorDocumentCloseChoice)>;
		using EnqueueAction = std::function<void(Action)>;

		struct ResolveResult final
		{
			bool DocumentsClosed = false;
		};

		void BeginCloseDocuments(std::vector<EditorDocumentIdentity> Documents, std::string DialogHostID, Action Continuation = {});
		void BeginCloseAllDocuments(std::vector<EditorDocumentIdentity> Documents, std::string DialogHostID, Action Continuation = {});
		void BeginResolveChanges(std::string DialogHostID, Action Continuation);

		[[nodiscard]] bool HasPendingRequest() const noexcept;
		[[nodiscard]] bool ClosesDocuments() const noexcept;
		[[nodiscard]] bool ConsumeDialogOpenRequest() noexcept;
		[[nodiscard]] const std::string& GetDialogHostID() const noexcept { return m_DialogHostID; }
		[[nodiscard]] const std::string& GetError() const noexcept { return m_Error; }
		[[nodiscard]] bool ShouldOfferCloseRetry(bool NoDirtyDocuments) const noexcept;

		void SetError(std::string Error);
		void SetDialogHostIfEmpty(std::string DialogHostID);
		void Cancel() noexcept;

		[[nodiscard]] std::vector<EditorDocumentIdentity> GetTargetDocuments(
			std::span<const EditorDocumentIdentity> CurrentDirtyDocuments) const;
		[[nodiscard]] std::vector<EditorDocumentIdentity> GetDirtyPromptDocuments(
			std::span<const EditorDocumentIdentity> CurrentDirtyDocuments,
			const std::function<bool(const EditorDocumentIdentity&)>& IsDirty) const;

		[[nodiscard]] std::expected<ResolveResult, std::string> Resolve(EditorDocumentManager& Documents,
			const EditorDocumentEditorRegistry& Editors, EditorDocumentCloseChoice Choice,
			std::span<const EditorDocumentIdentity> CurrentDirtyDocuments, const ResolveChanges& ResolveDirtyChanges,
			const EnqueueAction& Enqueue);

	private:
		enum class Mode
		{
			None,
			ResolveChanges,
			CloseDocuments
		};

		void Begin(Mode RequestMode, std::vector<EditorDocumentIdentity> Documents, std::string DialogHostID, Action Continuation);
		void Reset() noexcept;

		Mode m_Mode = Mode::None;
		std::vector<EditorDocumentIdentity> m_Documents;
		std::string m_DialogHostID;
		std::string m_Error;
		Action m_Continuation;
		bool m_IncludeCurrentDirtyDocuments = false;
		bool m_DialogOpenRequested = false;
	};
} // namespace PulseForgeEditor
