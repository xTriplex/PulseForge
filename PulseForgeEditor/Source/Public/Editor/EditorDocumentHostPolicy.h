#pragma once

#include "Editor/EditorDocumentManager.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace PulseForgeEditor
{
	enum class EditorDocumentCommandTarget : uint8_t
	{
		Level,
		Material,
		Unsupported
	};

	enum class EditorCommandContextKind : uint8_t
	{
		Unavailable,
		Level,
		MaterialDetails,
		RegisteredDocument
	};

	struct EditorCommandContext final
	{
		EditorCommandContextKind Kind = EditorCommandContextKind::Unavailable;
		PulseForge::UUID Project;
		std::optional<EditorDocumentIdentity> Document;
		std::optional<PulseForge::AssetID> MaterialAsset;
	};

	[[nodiscard]] inline EditorCommandContext CaptureEditorCommandContext(const std::optional<EditorDocumentIdentity>& FocusedDocument,
		const PulseForge::UUID& CurrentProject, bool MaterialDetailsFocused, const std::optional<PulseForge::AssetID>& MaterialDetailsAsset,
		const std::optional<EditorDocumentIdentity>& RegisteredDocument, const std::optional<EditorDocumentIdentity>& LevelDocument,
		bool LevelWorkspaceAvailable)
	{
		if (MaterialDetailsFocused)
		{
			if (!MaterialDetailsAsset || CurrentProject == PulseForge::UUID{}) return {};
			return { .Kind = EditorCommandContextKind::MaterialDetails, .Project = CurrentProject, .MaterialAsset = MaterialDetailsAsset };
		}
		if (FocusedDocument)
		{
			if (FocusedDocument->Project != CurrentProject) return {};
			if (FocusedDocument->Kind == EditorDocumentKind::Level)
			{
				if (!LevelWorkspaceAvailable || LevelDocument != FocusedDocument) return {};
				return { .Kind = EditorCommandContextKind::Level, .Project = CurrentProject, .Document = FocusedDocument };
			}
			if (!RegisteredDocument || *RegisteredDocument != *FocusedDocument) return {};
			return { .Kind = EditorCommandContextKind::RegisteredDocument, .Project = CurrentProject, .Document = FocusedDocument };
		}
		if (!LevelWorkspaceAvailable || !LevelDocument || LevelDocument->Project != CurrentProject) return {};
		return { .Kind = EditorCommandContextKind::Level, .Project = CurrentProject, .Document = LevelDocument };
	}

	[[nodiscard]] inline std::optional<EditorCommandContext> ResolveEditorCommandContext(const EditorCommandContext& Context,
		const PulseForge::UUID& CurrentProject, const std::optional<EditorDocumentIdentity>& LevelDocument, bool LevelWorkspaceAvailable,
		bool RegisteredDocumentAvailable, bool MaterialDetailsAvailable) noexcept
	{
		if (Context.Project != CurrentProject || CurrentProject == PulseForge::UUID{}) return std::nullopt;
		switch (Context.Kind)
		{
		case EditorCommandContextKind::Level:
			return Context.Document && LevelDocument == Context.Document && LevelWorkspaceAvailable
					   ? std::optional<EditorCommandContext>(Context)
					   : std::nullopt;
		case EditorCommandContextKind::MaterialDetails:
			return Context.MaterialAsset && MaterialDetailsAvailable ? std::optional<EditorCommandContext>(Context) : std::nullopt;
		case EditorCommandContextKind::RegisteredDocument:
			return Context.Document && Context.Document->Project == CurrentProject && RegisteredDocumentAvailable
					   ? std::optional<EditorCommandContext>(Context)
					   : std::nullopt;
		case EditorCommandContextKind::Unavailable:
			return std::nullopt;
		}
		return std::nullopt;
	}

	[[nodiscard]] inline EditorDocumentCommandTarget ResolveEditorDocumentCommandTarget(
		const EditorDocumentIdentity* FocusedDocument, const PulseForge::UUID& CurrentProject) noexcept
	{
		if (!FocusedDocument) return EditorDocumentCommandTarget::Level;
		if (FocusedDocument->Project != CurrentProject) return EditorDocumentCommandTarget::Unsupported;

		switch (FocusedDocument->Kind)
		{
		case EditorDocumentKind::Level:
			return EditorDocumentCommandTarget::Level;
		case EditorDocumentKind::Material:
			return EditorDocumentCommandTarget::Material;
		case EditorDocumentKind::Texture:
			return EditorDocumentCommandTarget::Unsupported;
		}
		return EditorDocumentCommandTarget::Unsupported;
	}

	[[nodiscard]] constexpr bool ShouldProcessNativeDocumentHostCloseRequests(bool ApplicationShutdownRequested) noexcept
	{
		return !ApplicationShutdownRequested;
	}

	template <typename TIdentity, typename IsDirtyPredicate>
	[[nodiscard]] std::vector<TIdentity> FilterDirtyDocumentIdentities(
		const std::vector<TIdentity>& Identities, IsDirtyPredicate&& IsDirty)
	{
		std::vector<TIdentity> DirtyIdentities;
		for (const TIdentity& Identity : Identities)
			if (IsDirty(Identity)) DirtyIdentities.push_back(Identity);
		return DirtyIdentities;
	}
} // namespace PulseForgeEditor
