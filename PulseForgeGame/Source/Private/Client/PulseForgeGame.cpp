#include "Client/PulseForgeGame.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "Events/Event.h"
#include "ImGui/UI.h"
#include "Renderer/SceneRenderer.h"
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
		(void)DeltaTime;
		if (!m_SceneRenderer)
			return;

		const auto [FramebufferWidth, FramebufferHeight] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
		if (FramebufferWidth == 0 || FramebufferHeight == 0)
			return;

		const auto Prepared = m_SceneRenderer->PrepareScene(
			m_Scene,
			m_CameraEntity,
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

	void OnImGuiRender() override
	{
		PulseForge::UI::BeginWindow("Pulseforge Debug");
		PulseForge::UI::Text("Welcome to the PulseForge UI System!");
		PulseForge::UI::Separator();

		if (PulseForge::UI::Button("Click Me!"))
		{
			PF_TRACE("Button was clicked from the UI!");
		}

		PulseForge::UI::EndWindow();

		PulseForge::UI::ShowDemoWindow();
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

		const auto CameraID = PulseForge::UUID::Parse("c312582b-32cb-4811-9b93-4917d7bb6096");
		if (!CameraID)
			throw std::runtime_error("Validation scene camera UUID is invalid");
		const auto Camera = m_Scene.FindEntity(*CameraID);
		if (!Camera)
			throw std::runtime_error("Validation scene is missing its expected camera entity");
		m_CameraEntity = *CameraID;
	}

	PulseForge::Scene m_Scene;
	PulseForge::UUID m_CameraEntity;
	std::optional<PulseForge::Project> m_Project;
	std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
	bool m_LoggedPrepareFailure = false;
	bool m_LoggedRenderFailure = false;
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
