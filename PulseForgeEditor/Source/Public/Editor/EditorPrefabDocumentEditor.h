#pragma once

#include "Assets/PrefabInstanceReconciler.h"
#include "Editor/EditorDocumentEditorRegistry.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForgeEditor
{
	class EditorStyle;

	enum class EditorPrefabDocumentIntent : uint8_t
	{
		PlanPreviewReconcile,
		ApplyPreviewReconcile
	};

	struct EditorPrefabDocumentSummary
	{
		PulseForge::AssetID Asset;
		PulseForge::UUID SourceRoot;
		std::string SourceFingerprint;
		size_t SourceEntityCount = 0;
		size_t PreviewEntityCount = 0;
		bool DraftDirty = false;
		bool CanUndo = false;
		bool CanRedo = false;
		bool HasPendingReconcile = false;
		bool CanApplyReconcile = false;
		PulseForge::PrefabReconcileSummary Reconcile;
		std::vector<PulseForge::PrefabConflictRecord> Conflicts;
		std::string Status;
		std::string Error;
	};

	struct EditorPrefabDocumentOperationResult
	{
		EditorPrefabDocumentIntent Intent = EditorPrefabDocumentIntent::PlanPreviewReconcile;
		PulseForge::PrefabReconcileSummary Reconcile;
		std::vector<PulseForge::PrefabConflictRecord> Conflicts;
		bool CanApply = false;
		bool Changed = false;
	};

	// Owns one production prefab document adapter. The owner must outlive sessions loaded by its registry callbacks.
	// Kind is supplied by editor integration so this file remains independent of the shared kind-enum edit.
	class EditorPrefabDocumentEditor final
	{
	public:
		EditorPrefabDocumentEditor(
			EditorDocumentManager& Documents,
			EditorDocumentEditorRegistry& Editors,
			EditorStyle& Style);

		[[nodiscard]] std::expected<void, std::string> Register(EditorDocumentKind PrefabDocumentKind);
		[[nodiscard]] bool Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] bool Save(EditorDocumentSession& Session) const;
		[[nodiscard]] bool Discard(EditorDocumentSession& Session) const;
		[[nodiscard]] bool DispatchFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command) const;

		[[nodiscard]] std::expected<EditorPrefabDocumentOperationResult, std::string> ExecuteIntent(
			EditorDocumentSession& Session,
			EditorPrefabDocumentIntent Intent);
		[[nodiscard]] std::expected<void, std::string> RecordPreviewOverride(
			EditorDocumentSession& Session,
			const PulseForge::PrefabOverrideRecord& Override);
		[[nodiscard]] std::expected<void, std::string> ResetPreviewOverride(
			EditorDocumentSession& Session,
			PulseForge::PrefabEntityAddress Entity,
			std::string_view ComponentID,
			std::string_view PropertyID);
		[[nodiscard]] std::expected<PulseForge::PrefabOverrideEditSummary, std::string> ApplyInstanceOverridesToSourceDraft(
			EditorDocumentSession& Session,
			const PulseForge::PrefabInstanceComponent& Instance,
			std::optional<PulseForge::PrefabEntityAddress> OnlyEntity = std::nullopt);
		[[nodiscard]] std::expected<PulseForge::PrefabOverrideEditSummary, std::string> RevertInstanceOverrides(
			EditorDocumentSession& Session,
			PulseForge::Scene& Destination,
			const PulseForge::Entity& LinkedRoot,
			std::optional<PulseForge::PrefabEntityAddress> OnlyEntity = std::nullopt);
		[[nodiscard]] std::expected<void, std::string> ResetInstancePropertyOverride(
			EditorDocumentSession& Session,
			const PulseForge::Entity& LinkedRoot,
			PulseForge::PrefabEntityAddress Address,
			std::string_view ComponentID,
			std::string_view PropertyID);
		[[nodiscard]] std::expected<EditorPrefabDocumentSummary, std::string> GetSummary(
			const EditorDocumentSession& Session) const;

	private:
		[[nodiscard]] bool IsEligible(const EditorDocumentEditorContext& Context) const;
		[[nodiscard]] std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string> LoadSession(
			const EditorDocumentEditorContext& Context) const;
		void DrawWorkspace(EditorDocumentSession& Session);
		[[nodiscard]] bool SaveSession(EditorDocumentSession& Session, uint64_t SavedRevision) const;
		[[nodiscard]] bool DiscardSession(EditorDocumentSession& Session) const;
		[[nodiscard]] bool HandleFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command) const;
		[[nodiscard]] bool UpdateAssetLocation(
			EditorDocumentSession& Session,
			const std::filesystem::path& ProjectRelativePath,
			std::string_view DisplayName) const;
		[[nodiscard]] std::expected<void, std::string> PrepareClose(const EditorDocumentCloseContext& Context) const;

		EditorDocumentManager& m_Documents;
		EditorDocumentEditorRegistry& m_Editors;
		EditorStyle& m_Style;
		std::optional<EditorDocumentKind> m_DocumentKind;
	};
} // namespace PulseForgeEditor
