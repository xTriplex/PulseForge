#include "Renderer/SceneRenderer.h"

#include <filesystem>
#include <memory>
#include <type_traits>

namespace
{
	using SceneRendererFactory = std::expected<std::unique_ptr<PulseForge::SceneRenderer>, PulseForge::SceneRendererError> (*)(
		PulseForge::RenderDevice&, const PulseForge::Project&, const std::filesystem::path&);

	static_assert(std::is_same_v<decltype(&PulseForge::SceneRenderer::Create), SceneRendererFactory>);

	[[nodiscard]] SceneRendererFactory GetSceneRendererFactory() noexcept
	{
		return &PulseForge::SceneRenderer::Create;
	}
} // namespace

int main()
{
	return GetSceneRendererFactory() == nullptr ? 1 : 0;
}
