#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace PulseForge
{
	class Scene;
	class SceneRenderer;
} // namespace PulseForge

namespace PulseForge::GpuValidation
{
	class PointShadowImageValidation final
	{
	public:
		PointShadowImageValidation();
		~PointShadowImageValidation();
		PointShadowImageValidation(PointShadowImageValidation&&) noexcept;
		PointShadowImageValidation& operator=(PointShadowImageValidation&&) noexcept;
		PointShadowImageValidation(const PointShadowImageValidation&) = delete;
		PointShadowImageValidation& operator=(const PointShadowImageValidation&) = delete;

		void Initialize(Scene& StartupScene, SceneRenderer& Renderer, std::filesystem::path OutputDirectory);
		[[nodiscard]] std::expected<bool, std::string> Update(std::chrono::steady_clock::time_point StartedAt);
		[[nodiscard]] std::expected<void, std::string> Render();
		[[nodiscard]] std::expected<void, std::string> Release();
		[[nodiscard]] bool IsComplete() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
} // namespace PulseForge::GpuValidation
