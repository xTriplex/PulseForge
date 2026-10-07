#include "Client/PulseForgeGame.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "Events/Event.h"
#include "Renderer/SceneRenderer.h"
#include "Runtime/SceneRuntime.h"
#include "Scene/Scene.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <expected>
#include <string>

class ExampleLayer : public PulseForge::Layer
{
public:
	ExampleLayer()
		: Layer("Example")
	{
		if (PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan)
		{
			const std::filesystem::path ProjectRoot = std::filesystem::current_path();
			auto OpenedProject = PulseForge::Project::Open(ProjectRoot / "PulseForgeGame.pfproj");
			if (!OpenedProject)
				throw std::runtime_error("Could not open sample project: " + OpenedProject.error().Message);
			m_Project.emplace(std::move(*OpenedProject));
			if (!m_Project->GetDescription().StartScene)
				throw std::runtime_error("Sample project does not specify a startup scene asset");
			LoadValidationScene(ProjectRoot, m_Project->GetAssetRegistry(), *m_Project->GetDescription().StartScene);
			m_SceneRuntime = std::make_unique<PulseForge::SceneRuntime>(
				*m_Project,
				PulseForge::SceneRuntimeDesc{},
				PulseForge::SceneRuntimeServices{ .InputState = &PulseForge::Application::Get().GetInput() });
			if (auto StartResult = m_SceneRuntime->Start(m_Scene); !StartResult)
				throw std::runtime_error("Could not start sample scene runtime: " + StartResult.error().Message);
			const std::filesystem::path ShaderDirectory =
				std::filesystem::current_path() / PF_SAMPLE_SHADER_DIRECTORY;
			auto CreatedSceneRenderer = PulseForge::SceneRenderer::Create(
				PulseForge::Application::Get(),
				*m_Project,
				ShaderDirectory);
			if (!CreatedSceneRenderer)
				throw std::runtime_error(CreatedSceneRenderer.error().Message);
			m_SceneRenderer = std::move(CreatedSceneRenderer.value());
		}
	}

	void OnUpdate(PulseForge::Timestep DeltaTime) override
	{
		if (m_SceneRuntime)
		{
			if (auto RuntimeResult = m_SceneRuntime->Advance(m_Scene, DeltaTime); !RuntimeResult)
			{
				if (!m_LoggedRuntimeFailure)
				{
					PF_ERROR("Could not advance the sample scene runtime: {0}", RuntimeResult.error().Message);
					m_LoggedRuntimeFailure = true;
				}
			}
			else
				m_LoggedRuntimeFailure = false;
		}
		if (!m_SceneRenderer)
			return;

		const auto [FramebufferWidth, FramebufferHeight] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
		if (FramebufferWidth == 0 || FramebufferHeight == 0)
			return;

		const auto Prepared = m_SceneRenderer->PrepareScene(
			m_Scene,
			static_cast<float>(FramebufferWidth) / static_cast<float>(FramebufferHeight));
		if (!Prepared)
		{
			if (!m_LoggedPrepareFailure)
			{
				PF_ERROR("Could not prepare the sample scene renderer: {0}", Prepared.error().Message);
				m_LoggedPrepareFailure = true;
			}
			return;
		}
		m_LoggedPrepareFailure = false;
	}

	void OnRender() override
	{
		if (!m_SceneRenderer)
			return;
		const auto Rendered = m_SceneRenderer->RenderPreparedScene();
		if (!Rendered)
		{
			if (!m_LoggedRenderFailure)
			{
				PF_ERROR("Could not render the prepared sample scene: {0}", Rendered.error().Message);
				m_LoggedRenderFailure = true;
			}
			return;
		}
		m_LoggedRenderFailure = false;
		if (*Rendered > 0 && !m_LoggedSceneDraw)
		{
			m_LoggedSceneDraw = true;
			PF_INFO("Submitted {0} indexed mesh instance(s) from the prepared scene", *Rendered);
		}
	}

	void OnEvent(PulseForge::Event& Event) override
	{
		PF_TRACE("{0}", Event.ToString());
	}

private:
	void LoadValidationScene(
		const std::filesystem::path& ProjectRoot,
		const PulseForge::AssetRegistry& Registry,
		const PulseForge::AssetID& SceneAsset)
	{
		if (auto LoadResult = PulseForge::SceneAssetService::Load(SceneAsset, ProjectRoot, Registry, m_Scene);
			!LoadResult)
			throw std::runtime_error("Could not load validation scene: " + LoadResult.error().Message);

	}

	PulseForge::Scene m_Scene;
	std::optional<PulseForge::Project> m_Project;
	std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
	std::unique_ptr<PulseForge::SceneRuntime> m_SceneRuntime;
	bool m_LoggedPrepareFailure = false;
	bool m_LoggedRenderFailure = false;
	bool m_LoggedRuntimeFailure = false;
	bool m_LoggedSceneDraw = false;
};

PulseForgeGameApp::PulseForgeGameApp()
#if defined(PF_SAMPLE_RENDERER_OPENGL)
	: Application(PulseForge::RendererAPI::OpenGL)
#else
	: Application()
#endif

{
	PF_INFO("Hello, PulseForge!");

	PushLayer(std::make_unique<ExampleLayer>());
}

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication()
{
	return std::make_unique<PulseForgeGameApp>();
}
