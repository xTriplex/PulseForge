#pragma once

#include "Editor/EditorDocumentManager.h"

#include <array>
#include <cstdint>
#include <vector>

namespace PulseForgeEditor
{
	// Asset document workspaces are enabled only by explicit editor registration. The current
	// material property form is a utility panel, and no full asset editor is registered yet.
	inline constexpr std::array<EditorDocumentKind, 0> RegisteredAssetDocumentEditors{};

	[[nodiscard]] constexpr bool IsRegisteredAssetDocumentEditor(EditorDocumentKind Kind) noexcept
	{
		for (const EditorDocumentKind RegisteredKind : RegisteredAssetDocumentEditors)
			if (RegisteredKind == Kind)
				return true;
		return false;
	}

	enum class EditorDocumentCommandTarget : uint8_t
	{
		Level,
		Material,
		Unsupported
	};

	[[nodiscard]] inline EditorDocumentCommandTarget ResolveEditorDocumentCommandTarget(
		const EditorDocumentIdentity* FocusedDocument,
		const PulseForge::UUID& CurrentProject) noexcept
	{
		if (!FocusedDocument)
			return EditorDocumentCommandTarget::Level;
		if (FocusedDocument->Project != CurrentProject)
			return EditorDocumentCommandTarget::Unsupported;

		switch (FocusedDocument->Kind)
		{
			case EditorDocumentKind::Level: return EditorDocumentCommandTarget::Level;
			case EditorDocumentKind::Material: return EditorDocumentCommandTarget::Material;
			case EditorDocumentKind::Texture: return EditorDocumentCommandTarget::Unsupported;
		}
		return EditorDocumentCommandTarget::Unsupported;
	}

	[[nodiscard]] constexpr bool ShouldProcessNativeDocumentHostCloseRequests(
		bool ApplicationShutdownRequested) noexcept
	{
		return !ApplicationShutdownRequested;
	}

	template<typename Identity, typename IsDirtyPredicate>
	[[nodiscard]] std::vector<Identity> FilterDirtyDocumentIdentities(
		const std::vector<Identity>& Identities,
		IsDirtyPredicate&& IsDirty)
	{
		std::vector<Identity> DirtyIdentities;
		for (const Identity& Identity : Identities)
			if (IsDirty(Identity))
				DirtyIdentities.push_back(Identity);
		return DirtyIdentities;
	}
}
