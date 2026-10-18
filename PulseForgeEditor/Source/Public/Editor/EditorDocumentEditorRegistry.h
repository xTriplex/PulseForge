#pragma once

#include "Editor/EditorDocumentManager.h"

#include <expected>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace PulseForgeEditor
{
	enum class EditorDocumentCommand : uint8_t
	{
		Undo,
		Redo
	};

	struct EditorDocumentEditorContext
	{
		EditorDocumentIdentity Identity;
		std::filesystem::path ProjectRoot;
		std::filesystem::path ProjectRelativePath;
		std::string DisplayName;
		const PulseForge::AssetRegistry* Registry = nullptr;
	};

	struct EditorDocumentEditorAdapter
	{
		EditorDocumentKind Kind = EditorDocumentKind::Material;
		std::function<bool(const EditorDocumentEditorContext&)> IsEligible;
		std::function<std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>(
			const EditorDocumentEditorContext&)> LoadSession;
		std::function<void(EditorDocumentSession&)> DrawWorkspace;
		std::function<bool(EditorDocumentSession&, uint64_t)> Save;
		std::function<bool(EditorDocumentSession&)> Discard;
		std::function<bool(EditorDocumentSession&, EditorDocumentCommand)> FocusedCommand;
		std::function<bool(EditorDocumentSession&, const std::filesystem::path&, std::string_view)> AssetLocationChanged;
		// This phase may reject closing but must not mutate the manager or editor state, and must not retain the borrowed state.
		std::function<std::expected<void, std::string>(const EditorDocumentCloseContext&)> PrepareClose;
	};

	class EditorDocumentEditorRegistry final
	{
	public:
		[[nodiscard]] std::expected<void, std::string> Register(EditorDocumentEditorAdapter Adapter)
		{
			if (Adapter.Kind == EditorDocumentKind::Level || !Adapter.IsEligible || !Adapter.LoadSession || !Adapter.DrawWorkspace ||
				!Adapter.Save || !Adapter.Discard || !Adapter.FocusedCommand || !Adapter.AssetLocationChanged || !Adapter.PrepareClose)
				return std::unexpected("A specialized document registration is incomplete");
			if (m_Adapters.contains(Adapter.Kind))
				return std::unexpected("A specialized editor is already registered for this document kind");
			m_Adapters.emplace(Adapter.Kind, std::move(Adapter));
			return {};
		}

		[[nodiscard]] bool IsRegistered(EditorDocumentKind Kind) const noexcept
		{
			return m_Adapters.contains(Kind);
		}

		[[nodiscard]] bool CanOpen(const EditorDocumentEditorContext& Context) const
		{
			const auto Adapter = m_Adapters.find(Context.Identity.Kind);
			return Adapter != m_Adapters.end() && Adapter->second.IsEligible(Context);
		}

		[[nodiscard]] bool Load(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const
		{
			const auto Adapter = m_Adapters.find(Session.Identity.Kind);
			if (Adapter == m_Adapters.end() || Context.Identity != Session.Identity ||
				!Adapter->second.IsEligible(Context))
				return false;
			if (Session.EditorState)
				return true;
			auto State = Adapter->second.LoadSession(Context);
			if (!State || !*State)
				return false;
			Session.EditorState = std::move(*State);
			return true;
		}

		[[nodiscard]] bool DrawWorkspace(EditorDocumentSession& Session) const
		{
			const auto Adapter = m_Adapters.find(Session.Identity.Kind);
			if (Adapter == m_Adapters.end() || !Session.EditorState)
				return false;
			Adapter->second.DrawWorkspace(Session);
			return true;
		}

		[[nodiscard]] bool Save(EditorDocumentSession& Session) const
		{
			const auto Adapter = m_Adapters.find(Session.Identity.Kind);
			if (Adapter == m_Adapters.end() || !Session.EditorState)
				return false;
			const uint64_t SavedRevision = Session.Revision;
			if (!Adapter->second.Save(Session, SavedRevision))
				return false;
			Session.SavedRevision = SavedRevision;
			return true;
		}

		[[nodiscard]] bool Discard(EditorDocumentSession& Session) const
		{
			const auto Adapter = m_Adapters.find(Session.Identity.Kind);
			if (Adapter == m_Adapters.end() || !Session.EditorState || !Adapter->second.Discard(Session))
				return false;
			Session.Revision = Session.SavedRevision;
			return true;
		}

		[[nodiscard]] bool DispatchFocusedCommand(
			EditorDocumentSession& Session,
			EditorDocumentCommand Command) const
		{
			const auto Adapter = m_Adapters.find(Session.Identity.Kind);
			return Adapter != m_Adapters.end() && Session.EditorState &&
				Adapter->second.FocusedCommand(Session, Command);
		}

		[[nodiscard]] bool UpdateAssetLocation(
			EditorDocumentSession& Session,
			const std::filesystem::path& ProjectRelativePath,
			std::string DisplayName) const
		{
			const auto Adapter = m_Adapters.find(Session.Identity.Kind);
			if (Adapter == m_Adapters.end() || !Session.EditorState ||
				!Adapter->second.AssetLocationChanged(Session, ProjectRelativePath, DisplayName))
				return false;
			Session.ProjectRelativePath = ProjectRelativePath;
			Session.DisplayName = std::move(DisplayName);
			return true;
		}

		[[nodiscard]] std::expected<void, std::string> PrepareClose(const EditorDocumentSession& Session) const
		{
			const auto Adapter = m_Adapters.find(Session.Identity.Kind);
			if (Adapter == m_Adapters.end() || !Session.EditorState)
				return std::unexpected("The document editor session is unavailable");
			try
			{
				const EditorDocumentCloseContext Context{ Session.Identity, Session.ProjectRelativePath, Session.DisplayName,
					Session.Revision, Session.SavedRevision, Session.EditorState.get() };
				return Adapter->second.PrepareClose(Context);
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(std::string("The document editor close preflight threw: ") + Exception.what());
			}
			catch (...)
			{
				return std::unexpected("The document editor close preflight threw an unknown exception");
			}
		}

	private:
		std::unordered_map<EditorDocumentKind, EditorDocumentEditorAdapter> m_Adapters;
	};
}
