#include "EditorViewportSceneRenderView.h"

#include "Assets/Project.h"
#include "Core/Log.h"
#include "Editor/EditorImGuiRenderer.h"
#include "Editor/EditorViewportPolicy.h"
#include "EditorViewportCamera.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <filesystem>

namespace PulseForgeEditor
{
	EditorViewportSceneRenderView::EditorViewportSceneRenderView() = default;

	EditorViewportSceneRenderView::~EditorViewportSceneRenderView()
	{
		ResetTarget();
	}

	std::expected<bool, std::string> EditorViewportSceneRenderView::EnsureTarget(float LogicalWidth, float LogicalHeight, float ScaleX,
		float ScaleY, PulseForge::RenderDevice& Runtime, EditorImGuiRenderer& ImGuiRenderer)
	{
#ifdef PF_EDITOR_RENDERER_OPENGL
		(void)LogicalWidth;
		(void)LogicalHeight;
		(void)ScaleX;
		(void)ScaleY;
		(void)Runtime;
		(void)ImGuiRenderer;
		return std::unexpected("The OpenGL fallback does not support the engine scene viewport");
#else
		const auto Extent = CalculateEditorViewportTargetExtent(LogicalWidth, LogicalHeight, ScaleX, ScaleY);
		if (!Extent)
		{
			SetTargetError("Could not resize the scene viewport: " + Extent.error());
			return std::unexpected(Extent.error());
		}

		if (m_Target && m_Width == Extent->Width && m_Height == Extent->Height)
		{
			ClearTargetError();
			return false;
		}

		PulseForge::RenderTargetDesc Description;
		Description.Width = Extent->Width;
		Description.Height = Extent->Height;
		Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
		Description.DebugName = "PulseForge editor scene viewport";
		auto Created = Runtime.CreateRenderTarget(Description);
		if (!Created)
		{
			SetTargetError("Could not resize the scene viewport: " + Created.error().Message);
			return std::unexpected(Created.error().Message);
		}

		const PulseForge::Texture* ColorTexture = (*Created)->GetColorTexture();
		if (!ColorTexture)
		{
			constexpr std::string_view Error = "The editor scene viewport requires a color attachment";
			SetTargetError("Could not resize the scene viewport: " + std::string(Error));
			return std::unexpected(std::string(Error));
		}

		auto TextureID = ImGuiRenderer.RegisterTexture(*ColorTexture);
		if (!TextureID)
		{
			SetTargetError("Could not resize the scene viewport: " + TextureID.error());
			return std::unexpected(TextureID.error());
		}

		if (m_TextureID != 0 && m_ImGuiRenderer) m_ImGuiRenderer->UnregisterTexture(m_TextureID);
		m_Target = std::move(*Created);
		m_ImGuiRenderer = &ImGuiRenderer;
		m_TextureID = *TextureID;
		m_Width = Extent->Width;
		m_Height = Extent->Height;
		m_SceneReady = false;
		ClearTargetError();
		return true;
#endif
	}

	void EditorViewportSceneRenderView::PrepareScene(PulseForge::RenderDevice& Runtime, const PulseForge::Project* SourceProject,
		const PulseForge::Scene* EditScene, const PulseForge::Scene* RuntimeScene, bool RuntimeActive, const EditorViewportCamera& Camera,
		std::span<const PulseForge::UUID> HiddenEntities, const std::filesystem::path& CompiledShaderDirectory)
	{
		m_SceneReady = false;
		m_ViewProjectionValid = false;
		m_EnvironmentLightingPending = false;
#ifdef PF_EDITOR_RENDERER_OPENGL
		(void)Runtime;
		(void)SourceProject;
		(void)EditScene;
		(void)RuntimeScene;
		(void)RuntimeActive;
		(void)Camera;
		(void)HiddenEntities;
		(void)CompiledShaderDirectory;
#else
		if (!SourceProject || !EditScene || m_Width == 0 || m_Height == 0 || m_SceneRendererInitializationFailed) return;

		if (!m_SceneRenderer)
		{
			auto Created = PulseForge::SceneRenderer::Create(Runtime, *SourceProject, CompiledShaderDirectory);
			if (!Created)
			{
				m_SceneRendererInitializationFailed = true;
				SetSceneError("Could not initialize the scene renderer: " + Created.error().Message);
				return;
			}
			m_SceneRenderer = std::move(*Created);
		}

		const auto SceneMode = GetEditorViewportSceneMode(RuntimeActive, EditScene != nullptr, RuntimeScene != nullptr);
		const PulseForge::Scene& ActiveScene = SceneMode.UseRuntimeScene ? *RuntimeScene : *EditScene;
		const float AspectRatio = static_cast<float>(m_Width) / static_cast<float>(m_Height);
		std::expected<void, PulseForge::SceneRendererError> Prepared;
		if (SceneMode.UseRuntimeScene) Prepared = m_SceneRenderer->PrepareScene(ActiveScene, AspectRatio);
		else
		{
			auto ViewProjection = Camera.GetViewProjection(AspectRatio);
			if (!ViewProjection)
			{
				SetSceneError("Could not prepare the editor camera: " + ViewProjection.error());
				return;
			}
			m_ViewProjection = *ViewProjection;
			auto Projection = Camera.GetProjection(AspectRatio);
			if (!Projection)
			{
				SetSceneError("Could not prepare the editor camera projection: " + Projection.error());
				return;
			}
			Prepared = m_SceneRenderer->PrepareSceneFiltered(
				ActiveScene, Camera.GetView(), *Projection, Camera.GetPosition(), 0.1f, 1000.0f, HiddenEntities);
		}
		if (!Prepared)
		{
			SetSceneError("Could not prepare the scene viewport: " + Prepared.error().Message);
			return;
		}

		ClearSceneError();
		m_ViewProjectionValid = SceneMode.ViewProjectionValid;
		m_SceneReady = true;
		m_EnvironmentLightingPending = m_SceneRenderer->IsEnvironmentLightingPending();
#endif
	}

	bool EditorViewportSceneRenderView::Render(
		PulseForge::RenderDevice& Runtime, bool PanelVisible, float ExposureEV, const PulseForge::RenderTargetClearValue& Background)
	{
#ifdef PF_EDITOR_RENDERER_OPENGL
		(void)Runtime;
		(void)PanelVisible;
		(void)ExposureEV;
		(void)Background;
		return false;
#else
		if (!PanelVisible || !m_Target) return false;

		if (m_SceneRenderer && m_SceneReady)
		{
			PulseForge::ToneMappingSettings ToneSettings;
			ToneSettings.ExposureEV = ExposureEV;
			if (!m_SceneRenderer->SetToneMappingSettings(ToneSettings))
			{
				SetSceneError("The viewport exposure value is outside the supported range");
				return false;
			}
			if (auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_Target); !Rendered)
			{
				SetSceneError("Scene viewport rendering failed: " + Rendered.error().Message);
				(void)ClearTarget(Runtime, Background, "Could not clear the scene viewport after rendering failed");
				return false;
			}
			ClearSceneError();
			return true;
		}

		(void)ClearTarget(Runtime, Background, "Could not clear the scene viewport");
		return false;
#endif
	}

	void EditorViewportSceneRenderView::ResetScene() noexcept
	{
		m_SceneRenderer.reset();
		m_SceneReady = false;
		m_ViewProjectionValid = false;
		m_EnvironmentLightingPending = false;
		m_SceneRendererInitializationFailed = false;
		ClearSceneError();
	}

	void EditorViewportSceneRenderView::InvalidatePreparedScene() noexcept
	{
		m_SceneReady = false;
		m_ViewProjectionValid = false;
		m_EnvironmentLightingPending = false;
		ClearSceneError();
	}

	void EditorViewportSceneRenderView::ClearEnvironmentLightingPending() noexcept
	{
		m_EnvironmentLightingPending = false;
	}

	void EditorViewportSceneRenderView::RefreshMaterialAssets() noexcept
	{
		if (m_SceneRenderer) m_SceneRenderer->RefreshMaterialAssets();
	}

	void EditorViewportSceneRenderView::ResetTarget() noexcept
	{
		if (m_ImGuiRenderer && m_TextureID != 0) m_ImGuiRenderer->UnregisterTexture(m_TextureID);
		m_TextureID = 0;
		m_ImGuiRenderer = nullptr;
		m_Target.reset();
		m_Width = 0;
		m_Height = 0;
		m_SceneReady = false;
		ClearTargetError();
	}

	bool EditorViewportSceneRenderView::HasPreparedEnvironmentLighting() const noexcept
	{
		return m_SceneRenderer && m_SceneRenderer->HasPreparedEnvironmentLighting();
	}

	void EditorViewportSceneRenderView::SetTargetError(std::string Message)
	{
		if (m_TargetError == Message) return;
		m_TargetError = std::move(Message);
		PF_WARN("{}", m_TargetError);
	}

	void EditorViewportSceneRenderView::ClearTargetError() noexcept
	{
		m_TargetError.clear();
	}

	void EditorViewportSceneRenderView::SetSceneError(std::string Message)
	{
		if (m_SceneError == Message) return;
		m_SceneError = std::move(Message);
		PF_WARN("{}", m_SceneError);
	}

	void EditorViewportSceneRenderView::ClearSceneError() noexcept
	{
		m_SceneError.clear();
	}

	bool EditorViewportSceneRenderView::ClearTarget(
		PulseForge::RenderDevice& Runtime, const PulseForge::RenderTargetClearValue& Clear, std::string_view FailureContext)
	{
		if (auto Begin = Runtime.BeginRenderTarget(*m_Target, Clear); !Begin)
		{
			SetSceneError(std::string(FailureContext) + ": " + Begin.error().Message);
			return false;
		}
		if (auto End = Runtime.EndRenderTarget(); !End)
		{
			SetSceneError(std::string(FailureContext) + ": " + End.error().Message);
			return false;
		}
		return true;
	}
} // namespace PulseForgeEditor
