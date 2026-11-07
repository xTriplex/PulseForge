#pragma once

#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorGraphModel.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorGraphDocumentEditorTestAccess;
	struct EditorGraphPropertyEditEvent;
	struct EditorGraphPropertyEditResult;
	struct EditorGraphCanvasLinkRequest
	{
		uintptr_t StartPin = 0;
		uintptr_t EndPin = 0;
	};

	struct EditorGraphCanvasLinkRequestResult
	{
		bool SawRequest = false;
		bool Accepted = false;
		EditorGraphConnectionDecision Decision;
	};

	namespace GraphCanvasDetail
	{
		template <typename EndCreate>
		struct EditorGraphCreateEnd final
		{
			EndCreate& End;
			~EditorGraphCreateEnd() { End(); }
		};

		template <typename BeginCreate, typename EndCreate, typename Query, typename Resolve,
			typename Accept, typename Reject>
		[[nodiscard]] EditorGraphCanvasLinkRequestResult ProcessEditorGraphCanvasLinkRequestFrame(
			BeginCreate&& Begin, EndCreate&& End, Query&& QueryRequest, Resolve&& ResolveRequest,
			Accept&& AcceptRequest, Reject&& RejectRequest)
		{
			const bool Active = Begin();
			EditorGraphCreateEnd<std::remove_reference_t<EndCreate>> EndGuard{ End };
			if (!Active) return {};

			const auto Request = QueryRequest();
			if (!Request) return {};

			EditorGraphCanvasLinkRequestResult Result;
			Result.SawRequest = true;
			Result.Decision = ResolveRequest(*Request);
			if (Result.Decision.Allowed)
				Result.Accepted = AcceptRequest();
			else RejectRequest();
			return Result;
		}
	}

	enum class EditorGraphFixtureKind : uint8_t
	{
		MaterialExpression,
		GameplayEvent
	};

	struct EditorGraphFixtureLocation
	{
		PulseForge::UUID GraphID;
		std::filesystem::path ProjectRelativePath;
		std::string DisplayName;
	};

	class EditorGraphDocumentEditor final
	{
	public:
		EditorGraphDocumentEditor(EditorDocumentManager& Documents, EditorDocumentEditorRegistry& Editors);

		[[nodiscard]] std::expected<void, std::string> Register();
		[[nodiscard]] std::expected<EditorGraphFixtureLocation, std::string> CreateFixture(
			const std::filesystem::path& ProjectRoot, EditorGraphFixtureKind Kind) const;
		[[nodiscard]] static std::filesystem::path GetFixturePath(const PulseForge::UUID& GraphID);
		[[nodiscard]] bool Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] bool Save(EditorDocumentSession& Session) const;

	private:
		friend struct EditorGraphDocumentEditorTestAccess;

		[[nodiscard]] bool IsEligible(const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> LoadSession(
			const EditorDocumentEditorContext& Context) const;
		void DrawWorkspace(EditorDocumentSession& Session);
		[[nodiscard]] EditorGraphPropertyEditResult ProcessPropertyEditEvents(EditorDocumentSession& Session,
			const std::vector<EditorGraphPropertyEditEvent>& Events, bool CancelGesture);
		[[nodiscard]] bool SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision) const;
		[[nodiscard]] bool DiscardSession(EditorDocumentSession& Session) const;
		[[nodiscard]] bool HandleFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command) const;
		[[nodiscard]] bool UpdateAssetLocation(EditorDocumentSession& Session,
			const std::filesystem::path& Path, std::string_view DisplayName) const;
		[[nodiscard]] std::expected<void, std::string> PrepareClose(const EditorDocumentCloseContext& Context) const;
		[[nodiscard]] bool ReloadState(EditorDocumentSession& Session) const;

		EditorDocumentManager& m_Documents;
		EditorDocumentEditorRegistry& m_Editors;
		bool m_Registered = false;
	};
}
