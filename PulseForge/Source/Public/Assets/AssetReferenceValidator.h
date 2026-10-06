#pragma once

#include "Assets/AssetRegistry.h"
#include "Core/Core.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <expected>
#include <vector>

namespace PulseForge
{
	enum class AssetReferenceKind : uint8_t
	{
		Mesh
	};

	enum class AssetReferenceIssueCode : uint8_t
	{
		MissingAsset
	};

	struct AssetReferenceIssue
	{
		AssetReferenceIssueCode Code;
		AssetReferenceKind Kind;
		UUID Entity;
		AssetID Asset;
	};

	class PULSEFORGE_API AssetReferenceValidator final
	{
	public:
		// Missing assets are reported without changing or rejecting the serialized scene references.
		[[nodiscard]] static std::expected<std::vector<AssetReferenceIssue>, SceneError> Validate(
			const Scene& SceneToValidate,
			const AssetRegistry& Registry);
	};
}
