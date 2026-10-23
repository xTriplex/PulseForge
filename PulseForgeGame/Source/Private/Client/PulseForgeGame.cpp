#include "Client/PulseForgeGame.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "Core/PlatformServices.h"
#include "Renderer/SceneRenderer.h"
#include "Runtime/PlayerSession.h"
#include "Scene/Scene.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	std::string PathToUtf8(const std::filesystem::path& Path)
	{
		const std::u8string Encoded = Path.u8string();
		return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
	}

	std::filesystem::path ResolveCompiledShaderDirectory()
	{
		auto ResourceDirectory = PulseForge::Platform::GetResourceDirectory();
		if (!ResourceDirectory)
			throw std::runtime_error("Could not locate the player resource directory: " + ResourceDirectory.error().Message);
		if (ResourceDirectory->empty() || !ResourceDirectory->is_absolute())
			throw std::runtime_error("Could not resolve an absolute player resource directory from the executable path");
		return *ResourceDirectory / PF_SAMPLE_SHADER_DIRECTORY;
	}

	void ValidateLaunchProject(const PulseForgeGame::PlayerLaunchConfiguration& Configuration)
	{
		auto ProjectResult = PulseForge::Project::Open(Configuration.ProjectFile);
		if (!ProjectResult) throw std::runtime_error("Could not open player project: " + ProjectResult.error().Message);

		const auto StartupScene = Configuration.StartupScene ? Configuration.StartupScene : ProjectResult->GetDescription().StartScene;
		if (!StartupScene) throw std::runtime_error("The player project has no startup scene; specify --start-scene <uuid>.");

		PulseForge::Scene Scene;
		if (auto Loaded =
				PulseForge::SceneAssetService::Load(*StartupScene, ProjectResult->GetRootPath(), ProjectResult->GetAssetRegistry(), Scene);
			!Loaded)
			throw std::runtime_error("Could not load player startup scene: " + Loaded.error().Message);
	}

	PulseForge::RendererAPI ResolveRendererAPI(const PulseForgeGame::PlayerLaunchConfiguration& Configuration)
	{
		if (Configuration.Renderer) return *Configuration.Renderer;
#if defined(PF_SAMPLE_RENDERER_OPENGL)
		return PulseForge::RendererAPI::OpenGL;
#else
		return PulseForge::RendererAPI::Vulkan;
#endif
	}

	PulseForgeGame::PlayerLaunchConfiguration ParseArguments(int ArgumentCount, char** Arguments)
	{
		std::vector<std::string> DefaultArguments;
		std::vector<std::string_view> ParsedArguments;
		if (ArgumentCount <= 1)
		{
			auto ResourceDirectory = PulseForge::Platform::GetResourceDirectory();
			if (!ResourceDirectory)
				throw std::runtime_error(
					"No --project was provided and the bundled sample path is unavailable: " + ResourceDirectory.error().Message);
			DefaultArguments = { "--project", PathToUtf8(*ResourceDirectory / "PulseForgeGame.pfproj") };
			for (const std::string& Argument : DefaultArguments)
				ParsedArguments.push_back(Argument);
		}
		else
		{
			ParsedArguments.reserve(static_cast<size_t>(ArgumentCount - 1));
			for (int Index = 1; Index < ArgumentCount; ++Index)
				ParsedArguments.emplace_back(Arguments[Index]);
		}

		std::error_code Error;
		const std::filesystem::path WorkingDirectory = std::filesystem::current_path(Error);
		if (Error) throw std::runtime_error("Could not resolve the player invocation directory: " + Error.message());
		auto Configuration = PulseForgeGame::ParsePlayerLaunchConfiguration(ParsedArguments, WorkingDirectory);
		if (!Configuration) throw std::runtime_error(Configuration.error().Message);
		ValidateLaunchProject(*Configuration);
		return std::move(*Configuration);
	}
} // namespace

class ExampleLayer final : public PulseForge::Layer
{
public:
	explicit ExampleLayer(PulseForgeGame::PlayerLaunchConfiguration Configuration)
		: Layer("Player"), m_Configuration(std::move(Configuration))
	{
#if defined(PF_DEBUG)
		if (const char* Smoke = std::getenv("PULSEFORGE_PLAYER_SMOKE_EXIT_AFTER_RENDER"))
			m_PlayerSmokeExitAfterRender = std::string_view(Smoke) == "1";
#endif
		PulseForge::SceneRuntimeServices Services{ .InputState = &PulseForge::Application::Get().GetInput(),
			.Diagnostics = PulseForge::Application::Get().GetDiagnosticsRecorder() };
		m_Session = std::make_unique<PulseForge::PlayerSession>(m_Configuration.ProjectFile, PulseForge::SceneRuntimeDesc{}, Services);
		if (auto Started = m_Session->Start(m_Configuration.StartupScene); !Started)
			throw std::runtime_error("Could not start player session: " + Started.error().Message);

		if (PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan)
		{
			auto CreatedRenderer = PulseForge::SceneRenderer::Create(
				PulseForge::Application::Get(), *m_Session->GetProject(), ResolveCompiledShaderDirectory());
			if (!CreatedRenderer) throw std::runtime_error(CreatedRenderer.error().Message);
			m_SceneRenderer = std::move(*CreatedRenderer);
		}
	}

	void OnUpdate(PulseForge::Timestep DeltaTime) override
	{
		if (m_Session)
		{
			const auto Advanced = m_Session->Advance(DeltaTime);
			if (!Advanced)
			{
				if (!m_LoggedRuntimeFailure)
				{
					PF_ERROR("Could not advance player session: {0}", Advanced.error().Message);
					m_LoggedRuntimeFailure = true;
				}
			}
			else
				m_LoggedRuntimeFailure = false;
			if (Advanced && m_PlayerSmokeExitAfterRender && !m_PlayerRuntimeSmokeLogged && Advanced->PhysicsSteps > 0)
			{
				m_PlayerRuntimeSmokeLogged = true;
				PF_INFO(
					"PULSEFORGE_PLAYER_RUNTIME_SMOKE=PASS; scripting advanced; physics_steps={}; audio advanced", Advanced->PhysicsSteps);
			}
		}
		if (!m_SceneRenderer || !m_Session || !m_Session->GetActiveScene()) return;

		const auto [FramebufferWidth, FramebufferHeight] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
		if (FramebufferWidth == 0 || FramebufferHeight == 0) return;
		const auto Prepared = m_SceneRenderer->PrepareScene(
			*m_Session->GetActiveScene(), static_cast<float>(FramebufferWidth) / static_cast<float>(FramebufferHeight));
		if (!Prepared)
		{
			if (!m_LoggedPrepareFailure)
			{
				PF_ERROR("Could not prepare the player scene renderer: {0}", Prepared.error().Message);
				m_LoggedPrepareFailure = true;
			}
			return;
		}
		m_LoggedPrepareFailure = false;
	}

	void OnRender() override
	{
		if (!m_SceneRenderer) return;
		const auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput();
		if (!Rendered)
		{
			if (!m_LoggedRenderFailure)
			{
				PF_ERROR("Could not render the prepared player scene: {0}", Rendered.error().Message);
				m_LoggedRenderFailure = true;
			}
			return;
		}
		m_LoggedRenderFailure = false;
		if (Rendered->GeometryDrawCount > 0)
		{
			if (!m_LoggedSceneDraw)
			{
				m_LoggedSceneDraw = true;
				PF_INFO("Submitted {0} indexed mesh instance(s) from the player scene", Rendered->GeometryDrawCount);
			}
			if (m_PlayerSmokeExitAfterRender && m_PlayerRuntimeSmokeLogged && !m_PlayerSmokeShutdownRequested)
			{
				m_PlayerSmokeShutdownRequested = true;
				PF_INFO("PULSEFORGE_PLAYER_RELOCATION_SMOKE=PASS; rendered scene geometry after runtime advancement and requesting orderly "
						"shutdown");
				PulseForge::Application::Get().RequestClose();
			}
		}
	}

	~ExampleLayer() override
	{
		if (m_PlayerSmokeShutdownRequested) PF_INFO("PULSEFORGE_PLAYER_RELOCATION_SMOKE_SHUTDOWN=PASS");
	}

private:
	PulseForgeGame::PlayerLaunchConfiguration m_Configuration;
	std::unique_ptr<PulseForge::PlayerSession> m_Session;
	std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
	bool m_LoggedPrepareFailure = false;
	bool m_LoggedRenderFailure = false;
	bool m_LoggedRuntimeFailure = false;
	bool m_LoggedSceneDraw = false;
	bool m_PlayerSmokeExitAfterRender = false;
	bool m_PlayerSmokeShutdownRequested = false;
	bool m_PlayerRuntimeSmokeLogged = false;
};

PulseForgeGameApp::PulseForgeGameApp(PulseForgeGame::PlayerLaunchConfiguration Configuration)
	: Application(ResolveRendererAPI(Configuration),
		  PulseForge::WindowProps("PulseForge Player", Configuration.WindowWidth.value_or(1280), Configuration.WindowHeight.value_or(720)))
{
	if (Configuration.DiagnosticsEnabled && !StartDiagnosticsCapture())
		throw std::runtime_error("Could not start the requested bounded diagnostics capture.");
	PushLayer(std::make_unique<ExampleLayer>(std::move(Configuration)));
}

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication(int ArgumentCount, char** Arguments)
{
	return std::make_unique<PulseForgeGameApp>(ParseArguments(ArgumentCount, Arguments));
}
