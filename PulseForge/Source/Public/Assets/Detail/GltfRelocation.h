#pragma once

#include "Assets/Detail/GltfSourceDocument.h"
#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace PulseForge::Assets::Detail
{
	struct GltfBufferPathMapping
	{
		std::filesystem::path OldProjectRelativePath;
		std::filesystem::path NewProjectRelativePath;
	};

	using GltfRelocationWrite = std::function<std::expected<void, std::string>(std::span<const std::byte>)>;

	// Rewrites only external buffers[*].uri values. Paths are canonical project-relative source paths. Data URIs and
	// buffer array indices are preserved. Unknown JSON values are retained semantically.
	[[nodiscard]] PULSEFORGE_API std::expected<std::string, std::string> RewriteGltfBufferUris(std::string_view JsonText,
		const std::filesystem::path& SourceOldProjectRelative, const std::filesystem::path& SourceNewProjectRelative,
		std::span<const GltfBufferPathMapping> BufferMappings);

	// Streams a GLB rewrite to the caller-owned staging writer. Only the bounded JSON chunk is materialized; BIN and
	// unknown chunks are copied in bounded blocks. The caller must discard its staging output if this returns an error.
	[[nodiscard]] PULSEFORGE_API std::expected<uint64_t, std::string> RewriteGlbBufferUris(uint64_t FileSize, const GlbReadRange& ReadRange,
		const std::filesystem::path& SourceOldProjectRelative, const std::filesystem::path& SourceNewProjectRelative,
		std::span<const GltfBufferPathMapping> BufferMappings, const GltfRelocationWrite& Write);
} // namespace PulseForge::Assets::Detail
