#pragma once

#include "Assets/AssetID.h"

#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	struct MeshRendererComponent
	{
		AssetID MeshAsset;
		std::optional<AssetID> MaterialAsset;
		// Stable cooked-product identity, independent of a source glTF node/array locator.
		std::optional<std::string> SkeletalProductKey;
		// Stable animation clip identity from the same skeletal product.
		std::optional<std::string> AnimationClipKey;
	};

	[[nodiscard]] PULSEFORGE_API bool IsValidGltfSkeletalProductKey(std::string_view Key) noexcept;
	[[nodiscard]] PULSEFORGE_API bool IsValidGltfAnimationClipKey(std::string_view Key) noexcept;
}
