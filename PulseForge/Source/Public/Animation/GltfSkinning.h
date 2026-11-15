#pragma once

#include "Assets/CookedProductCodec.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace PulseForge
{
	inline constexpr size_t MaximumGltfSkinnedVertices = 1'000'000;
	inline constexpr size_t MaximumGltfSkinPaletteJoints = 4096;

	enum class GltfSkinningErrorCode : uint8_t
	{
		InvalidMesh,
		InvalidPalette,
		InvalidInfluence,
		InvalidVertex,
		Cancelled,
		ResourceLimitExceeded
	};

	struct GltfSkinningError final
	{
		GltfSkinningErrorCode Code = GltfSkinningErrorCode::InvalidMesh;
		std::string Message;
	};

	// Builds mesh-local joint transforms from already-composed local node matrices and the immutable bind data.
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<std::array<float, 16>>, GltfSkinningError>
		BuildGltfJointPalette(
			const ImportedGltfSkeletalMesh& Mesh,
			std::span<const std::array<float, 16>> LocalNodeTransforms,
			std::stop_token CancellationToken = {});

	// Palette matrices are column-major mesh-local joint transforms; output retains the common renderer vertex layout.
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<GltfMeshVertex>, GltfSkinningError> SkinGltfMeshVertices(
		const ImportedGltfSkeletalMesh& Mesh,
		std::span<const std::array<float, 16>> JointPalette,
		std::stop_token CancellationToken = {});
}
