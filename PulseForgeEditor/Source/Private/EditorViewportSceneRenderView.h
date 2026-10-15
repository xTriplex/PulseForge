#pragma once

#include "Renderer/RenderTarget.h"
#include "Renderer/RenderDevice.h"
#include "Scene/UUID.h"

#include <expected>
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace PulseForge
{
	class Project;
	class Scene;
	class SceneRenderer;
} // namespace PulseForge

namespace PulseForgeEditor
{
	class EditorImGuiRenderer;
	class EditorViewportCamera;

	class EditorViewportSceneRenderView final
	{
	public:
		EditorViewportSceneRenderView();
		EditorViewportSceneRenderView(const EditorViewportSceneRenderView&) = delete;
		EditorViewportSceneRenderView& operator=(const EditorViewportSceneRenderView&) = delete;
		~EditorViewportSceneRenderView();

		[[nodiscard]] std::expected<bool, std::string> EnsureTarget(float LogicalWidth, float LogicalHeight, float ScaleX, float ScaleY,
			PulseForge::RenderDevice& Runtime, EditorImGuiRenderer& ImGuiRenderer);
		void PrepareScene(PulseForge::RenderDevice& Runtime, const PulseForge::Project* Project, const PulseForge::Scene* EditScene,
			const PulseForge::Scene* RuntimeScene, bool RuntimeActive, const EditorViewportCamera& Camera,
			std::span<const PulseForge::UUID> HiddenEntities, const std::filesystem::path& CompiledShaderDirectory);
		[[nodiscard]] bool Render(
			PulseForge::RenderDevice& Runtime, bool PanelVisible, float ExposureEV, const PulseForge::RenderTargetClearValue& Background);

		void ResetScene() noexcept;
		void InvalidatePreparedScene() noexcept;
		void ClearEnvironmentLightingPending() noexcept;
		void RefreshMaterialAssets() noexcept;
		void ResetTarget() noexcept;

		[[nodiscard]] bool HasTarget() const noexcept { return static_cast<bool>(m_Target); }
		[[nodiscard]] const PulseForge::RenderTarget* GetTarget() const noexcept { return m_Target.get(); }
		[[nodiscard]] uint64_t GetTextureID() const noexcept { return m_TextureID; }
		[[nodiscard]] uint32_t GetWidth() const noexcept { return m_Width; }
		[[nodiscard]] uint32_t GetHeight() const noexcept { return m_Height; }
		[[nodiscard]] bool IsSceneReady() const noexcept { return m_SceneReady; }
		[[nodiscard]] bool IsViewProjectionValid() const noexcept { return m_ViewProjectionValid; }
		[[nodiscard]] bool IsEnvironmentLightingPending() const noexcept { return m_EnvironmentLightingPending; }
		[[nodiscard]] bool HasPreparedEnvironmentLighting() const noexcept;
		[[nodiscard]] const glm::mat4& GetViewProjection() const noexcept { return m_ViewProjection; }
		[[nodiscard]] const std::string& GetTargetError() const noexcept { return m_TargetError; }
		[[nodiscard]] const std::string& GetSceneError() const noexcept { return m_SceneError; }

	private:
		void SetTargetError(std::string Message);
		void ClearTargetError() noexcept;
		void SetSceneError(std::string Message);
		void ClearSceneError() noexcept;
		[[nodiscard]] bool ClearTarget(
			PulseForge::RenderDevice& Runtime, const PulseForge::RenderTargetClearValue& Clear, std::string_view FailureContext);

		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		PulseForge::RenderTargetHandle m_Target;
		EditorImGuiRenderer* m_ImGuiRenderer = nullptr;
		glm::mat4 m_ViewProjection{ 1.0f };
		uint64_t m_TextureID = 0;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		bool m_SceneReady = false;
		bool m_ViewProjectionValid = false;
		bool m_EnvironmentLightingPending = false;
		bool m_SceneRendererInitializationFailed = false;
		std::string m_TargetError;
		std::string m_SceneError;
	};
} // namespace PulseForgeEditor
