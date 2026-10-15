#pragma once

#include "Renderer/Texture.h"

#include <cstdint>
#include <expected>
#include <string>

namespace PulseForgeEditor
{
	// Owns the UI-side registration for a borrowed renderer texture. The texture must remain alive until it is unregistered.
	class EditorTextureRegistry
	{
	public:
		virtual ~EditorTextureRegistry() = default;
		[[nodiscard]] virtual std::expected<uint64_t, std::string> RegisterTexture(const PulseForge::Texture& Texture) = 0;
		virtual void UnregisterTexture(uint64_t TextureID) noexcept = 0;
	};
} // namespace PulseForgeEditor
