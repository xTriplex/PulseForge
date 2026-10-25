#pragma once

#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace PulseForge::Assets::Detail
{
	inline constexpr uint32_t GlbMagic = 0x46546C67;
	inline constexpr uint32_t GlbJsonChunk = 0x4E4F534A;
	inline constexpr uint32_t GlbBinaryChunk = 0x004E4942;

	struct GlbChunkRange
	{
		uint64_t Offset = 0;
		uint32_t Length = 0;
	};

	struct GlbDocumentLayout
	{
		GlbChunkRange Json;
		std::optional<GlbChunkRange> Binary;
	};

	using GlbReadRange = std::function<bool(uint64_t, std::span<std::byte>)>;

	[[nodiscard]] PULSEFORGE_API std::expected<GlbDocumentLayout, std::string> InspectGlbLayout(
		uint64_t FileSize, uint64_t MaximumJsonBytes, const GlbReadRange& ReadRange);

	[[nodiscard]] PULSEFORGE_API std::expected<std::string, std::string> ReadGlbJsonChunk(
		uint64_t FileSize, uint64_t MaximumJsonBytes, const GlbReadRange& ReadRange);

	// Decodes one relative file URI exactly once. Data URIs return an empty optional. Consumers resolve the returned
	// relative path against the source directory and apply their own containment/move policy.
	[[nodiscard]] PULSEFORGE_API std::expected<std::optional<std::filesystem::path>, std::string> DecodeGltfBufferUri(std::string_view Uri);
} // namespace PulseForge::Assets::Detail
