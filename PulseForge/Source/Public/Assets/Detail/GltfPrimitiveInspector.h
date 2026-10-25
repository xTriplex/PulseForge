#pragma once

#include "Assets/AssetDependencyTypes.h"
#include "Core/Core.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge::Assets::Detail
{
	struct GltfPrimitiveInspection
	{
		AssetSubresourceClassification Classification = AssetSubresourceClassification::DiscoveredLocator;
		std::string Diagnostic;
	};

	struct GltfPrimitiveLocator
	{
		uint32_t MeshIndex = 0;
		uint32_t PrimitiveIndex = 0;
	};

	// Checks the importer-supported structural subset without decoding geometry or claiming a successful import.
	// The JSON input is bounded by the caller. External URIs use the shared glTF URI parser and managed-path policy.
	[[nodiscard]] PULSEFORGE_API GltfPrimitiveInspection InspectStaticPrimitive(
		std::string_view JsonText,
		const std::filesystem::path& SourcePath,
		const std::filesystem::path& ProjectRoot,
		uint32_t MeshIndex,
		uint32_t PrimitiveIndex,
		std::optional<uint64_t> GlbBinaryByteLength);

	[[nodiscard]] PULSEFORGE_API std::vector<GltfPrimitiveInspection> InspectStaticPrimitives(
		std::string_view JsonText,
		const std::filesystem::path& SourcePath,
		const std::filesystem::path& ProjectRoot,
		std::span<const GltfPrimitiveLocator> Locators,
		std::optional<uint64_t> GlbBinaryByteLength);
} // namespace PulseForge::Assets::Detail
