#pragma once

#include "Assets/AssetID.h"

#include <optional>
#include <string>
#include <string_view>

namespace PulseForge
{
	inline constexpr float MaximumGltfAnimationSpeed = 16.0f;

	struct MeshRendererComponent
	{
		AssetID MeshAsset;
		std::optional<AssetID> MaterialAsset;
		// Stable cooked-product identity, independent of a source glTF node/array locator.
		std::optional<std::string> SkeletalProductKey;
		// Stable animation clip identity from the same skeletal product.
		std::optional<std::string> AnimationClipKey;
		bool AnimationLooping = true;
		float AnimationSpeed = 1.0f;
	};

	[[nodiscard]] PULSEFORGE_API bool IsValidGltfSkeletalProductKey(std::string_view Key) noexcept;
	[[nodiscard]] PULSEFORGE_API bool IsValidGltfAnimationClipKey(std::string_view Key) noexcept;
}
