#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "Assets/Project.h"
#include "Scene/Entity.h"

namespace PulseForge
{
	class Application;
	class RenderTarget;
	class Scene;
	class SceneRenderer;
} // namespace PulseForge

namespace PulseForge::GpuValidation
{
	// Owns a temporary project, open-triangle mesh, and image-based winding scenarios.
	class MirroredWindingValidation final
	{
	public:
		MirroredWindingValidation(
			Application& Runtime, std::filesystem::path ShaderDirectory, std::chrono::steady_clock::time_point StartedAt) noexcept;
		~MirroredWindingValidation();
		MirroredWindingValidation(const MirroredWindingValidation&) = delete;
		MirroredWindingValidation& operator=(const MirroredWindingValidation&) = delete;

		void Initialize();
		[[nodiscard]] std::expected<bool, std::string> Update();
		[[nodiscard]] std::expected<void, std::string> Render();
		[[nodiscard]] bool IsComplete() const noexcept;
		void Reset() noexcept;

	private:
		Application& m_Runtime;
		std::filesystem::path m_ShaderDirectory;
		std::filesystem::path m_FixtureRoot;
		std::chrono::steady_clock::time_point m_StartedAt;
		std::optional<Project> m_Project;
		std::unique_ptr<Scene> m_Scene;
		std::unique_ptr<SceneRenderer> m_Renderer;
		std::unique_ptr<RenderTarget> m_OutputTarget;
		std::optional<Entity> m_Parent;
		std::optional<Entity> m_Mesh;
		std::optional<Entity> m_DoubleSidedMesh;
		std::optional<Entity> m_Camera;
		std::optional<Entity> m_Light;
		std::optional<Entity> m_SpotLight;
		std::optional<Entity> m_PointLight;
		size_t m_ScenarioIndex = 0;
		bool m_ScenePrepared = false;
		bool m_ReadbackPending = false;
	};
} // namespace PulseForge::GpuValidation
