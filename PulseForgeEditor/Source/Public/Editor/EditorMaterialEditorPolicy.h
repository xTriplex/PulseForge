#pragma once

#include "Assets/AssetTypes.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace PulseForgeEditor
{
	enum class AssetBrowserOpenAction : uint8_t
	{
		None,
		OpenScene,
		ShowMaterialDetails
	};

	[[nodiscard]] constexpr AssetBrowserOpenAction ResolveAssetBrowserOpenAction(
		std::string_view Extension,
		bool Activated,
		bool LeftDoubleClick) noexcept
	{
		if (!Activated || !LeftDoubleClick)
			return AssetBrowserOpenAction::None;
		if (PulseForge::AssetExtensionsEqual(Extension, ".scene")) return AssetBrowserOpenAction::OpenScene;
		if (PulseForge::AssetExtensionsEqual(Extension, ".material")) return AssetBrowserOpenAction::ShowMaterialDetails;
		return AssetBrowserOpenAction::None;
	}

	[[nodiscard]] constexpr bool ShouldApplyFirstOpenPanelDocking(
		bool HasWindowSettings,
		bool HasReferenceDockNode) noexcept
	{
		return !HasWindowSettings && HasReferenceDockNode;
	}

	[[nodiscard]] inline std::string MakeMaterialDraftKey(
		std::string_view ProjectRoot,
		std::string_view AssetID)
	{
		std::string Key;
		Key.reserve(ProjectRoot.size() + 1 + AssetID.size());
		Key.append(ProjectRoot);
		Key.push_back(':');
		Key.append(AssetID);
		return Key;
	}
}
