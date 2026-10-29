#include "Client/PulseForgeGame.h"
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#endif
#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Log.h"
#include "Core/PlatformServices.h"
#include "Input/GameplayInputStore.h"
#include "Renderer/SceneRenderer.h"
#include "Runtime/CookedContentProvider.h"
#include "Runtime/PlayerSession.h"
#include "Scene/Scene.h"

#include <cstdlib>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if defined(PF_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	class EditorShutdownChannel final
	{
	public:
		explicit EditorShutdownChannel(bool Enabled) noexcept : m_Enabled(Enabled)
		{
#if !defined(PF_PLATFORM_WINDOWS)
			if (m_Enabled)
			{
				const int Flags = ::fcntl(STDIN_FILENO, F_GETFL);
				if (Flags >= 0) (void)::fcntl(STDIN_FILENO, F_SETFL, Flags | O_NONBLOCK);
			}
#endif
		}

		bool ConsumeRequest() noexcept
		{
			if (!m_Enabled || m_Consumed) return false;
#if defined(PF_PLATFORM_WINDOWS)
			HANDLE Input = GetStdHandle(STD_INPUT_HANDLE);
			if (!Input || Input == INVALID_HANDLE_VALUE) return false;
			DWORD Available = 0;
			if (!PeekNamedPipe(Input, nullptr, 0, nullptr, &Available, nullptr))
			{
				if (GetLastError() == ERROR_BROKEN_PIPE) m_Consumed = true;
				return m_Consumed;
			}
			if (Available == 0) return false;
			char Request = 0;
			DWORD Read = 0;
			if (!ReadFile(Input, &Request, 1, &Read, nullptr) || Read == 0 || Request == 'S') m_Consumed = true;
			return m_Consumed;
#else
			char Request = 0;
			const ssize_t Read = ::read(STDIN_FILENO, &Request, 1);
			if (Read == 1 || Read == 0) m_Consumed = true;
			else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) m_Consumed = true;
			return m_Consumed;
#endif
		}

	private:
		bool m_Enabled = false;
		bool m_Consumed = false;
	};

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

#if !defined(PF_COOKED_RUNTIME_ONLY)
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
#endif

	std::string CurrentTargetPlatform()
	{
#if defined(PF_PLATFORM_WINDOWS)
		return "Windows";
#elif defined(PF_PLATFORM_MACOS)
		return "macOS";
#elif defined(PF_PLATFORM_LINUX)
		return "Linux";
#else
		return "Unsupported";
#endif
	}

	PulseForge::CookedBuildConfiguration CurrentBuildConfiguration()
	{
#if defined(PF_BUILD_CONFIGURATION)
		const auto Parsed = PulseForge::ParseCookedBuildConfiguration(PF_BUILD_CONFIGURATION);
		if (Parsed) return *Parsed;
		throw std::runtime_error("Player binary has an unsupported build configuration identity: " + Parsed.error());
#else
		throw std::runtime_error("Player binary is missing its build configuration identity");
#endif
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

	struct ParsedPlayerLaunch final
	{
		PulseForgeGame::PlayerLaunchConfiguration Configuration;
		bool EditorControlEnabled = false;
	};

	ParsedPlayerLaunch ParseArguments(int ArgumentCount, char** Arguments)
	{
		std::vector<std::string> DefaultArguments;
		std::vector<std::string_view> ParsedArguments;
		bool EditorControlEnabled = false;
		if (ArgumentCount <= 1)
		{
#if defined(PF_COOKED_RUNTIME_ONLY)
			auto ResourceDirectory = PulseForge::Platform::GetResourceDirectory();
			if (!ResourceDirectory)
				throw std::runtime_error("Could not locate cooked Player resources: " + ResourceDirectory.error().Message);
			DefaultArguments = { "--cooked-content", PathToUtf8(*ResourceDirectory / "Content") };
			const std::filesystem::path RuntimeConfiguration = *ResourceDirectory / "PulseForgeRuntime.cfg";
			std::error_code ConfigError;
			if (std::filesystem::exists(RuntimeConfiguration, ConfigError))
			{
				if (ConfigError || std::filesystem::file_size(RuntimeConfiguration, ConfigError) > 4096 || ConfigError)
					throw std::runtime_error("Packaged runtime configuration is unreadable or exceeds its limit");
				std::ifstream Config(RuntimeConfiguration, std::ios::binary);
				if (!Config) throw std::runtime_error("Could not read packaged runtime configuration");
				std::string Line;
				while (std::getline(Config, Line))
				{
					if (Line.starts_with("window="))
					{
						DefaultArguments.emplace_back("--window-size");
						DefaultArguments.emplace_back(Line.substr(7));
					}
					else if (!Line.empty() && !Line.starts_with("project="))
						throw std::runtime_error("Packaged runtime configuration contains an unsupported field");
				}
				if (!Config.eof()) throw std::runtime_error("Could not read complete packaged runtime configuration");
			}
			for (const std::string& Argument : DefaultArguments)
				ParsedArguments.push_back(Argument);
#else
			auto ResourceDirectory = PulseForge::Platform::GetResourceDirectory();
			if (!ResourceDirectory)
				throw std::runtime_error(
					"No --project was provided and the bundled sample path is unavailable: " + ResourceDirectory.error().Message);
			DefaultArguments = { "--project", PathToUtf8(*ResourceDirectory / "PulseForgeGame.pfproj") };
			for (const std::string& Argument : DefaultArguments)
				ParsedArguments.push_back(Argument);
#endif
		}
		else
		{
			ParsedArguments.reserve(static_cast<size_t>(ArgumentCount - 1));
			for (int Index = 1; Index < ArgumentCount; ++Index)
			{
				if (std::string_view(Arguments[Index]) == "--editor-control-stdin")
				{
					EditorControlEnabled = true;
					continue;
				}
				ParsedArguments.emplace_back(Arguments[Index]);
			}
		}

		std::error_code Error;
		const std::filesystem::path WorkingDirectory = std::filesystem::current_path(Error);
		if (Error) throw std::runtime_error("Could not resolve the player invocation directory: " + Error.message());
		auto Configuration = PulseForgeGame::ParsePlayerLaunchConfiguration(ParsedArguments, WorkingDirectory);
		if (!Configuration) throw std::runtime_error(Configuration.error().Message);

		if (Configuration->CookedContentRoot.empty())
		{
#if defined(PF_COOKED_RUNTIME_ONLY)
			throw std::runtime_error("Cooked Player requires --cooked-content; authoring project loading is not linked");
#else
			ValidateLaunchProject(*Configuration);
#endif
		}

		return { std::move(*Configuration), EditorControlEnabled };
	}
} // namespace

class ExampleLayer final : public PulseForge::Layer
{
public:
	explicit ExampleLayer(PulseForgeGame::PlayerLaunchConfiguration Configuration, bool EditorControlEnabled)
		: Layer("Player"), m_Configuration(std::move(Configuration)), m_EditorShutdownChannel(EditorControlEnabled)
	{
#if defined(PF_DEBUG)
		if (const char* Smoke = std::getenv("PULSEFORGE_PLAYER_SMOKE_EXIT_AFTER_RENDER"))
			m_PlayerSmokeExitAfterRender = std::string_view(Smoke) == "1";
		if (const char* Acceptance = std::getenv("PULSEFORGE_COOKED_PLAYER_ACCEPTANCE"))
			m_CookedPlayerAcceptance = std::string_view(Acceptance) == "1";
#endif
		PulseForge::SceneRuntimeServices Services{ .InputState = &PulseForge::Application::Get().GetInput(),
			.GameplayInput = &PulseForge::Application::Get().GetGameplayInput(),
			.Diagnostics = PulseForge::Application::Get().GetDiagnosticsRecorder() };
		auto& Application = PulseForge::Application::Get();
		PulseForge::SceneRuntimeDesc RuntimeDescription{};
#if defined(PF_DEBUG)
		if (m_CookedPlayerAcceptance)
		{
			RuntimeDescription.Audio.OutputBackend = PulseForge::AudioOutputBackend::Null;
			PF_INFO("PULSEFORGE_COOKED_AUDIO_BACKEND=NULL; deterministic output selected for process acceptance");
		}
#endif
		if (!m_Configuration.CookedContentRoot.empty())
		{
			const PulseForge::CookedAssetCompatibility Compatibility{
				.TargetPlatform = CurrentTargetPlatform(),
				.RendererBackend = Application.GetRendererAPI() == PulseForge::RendererAPI::Vulkan ? "Vulkan" : "OpenGL",
				.RuntimeABI = std::string(PulseForge::CookedRuntimeABIIdentifier),
				.Configuration = CurrentBuildConfiguration()
			};
			auto Opened = PulseForge::CookedContentProvider::Open(m_Configuration.CookedContentRoot, Compatibility);
			if (!Opened) throw std::runtime_error("Could not open cooked player content: " + Opened.error().Message);
			m_CookedContent = std::move(*Opened);
#if defined(PF_DEBUG)
			if (m_CookedPlayerAcceptance)
			{
				if (m_CookedContent->GetDynamicRoots().empty())
					throw std::runtime_error("Cooked Player acceptance requires an explicit dynamic root");
				for (const PulseForge::AssetID& Root : m_CookedContent->GetDynamicRoots())
				{
					if (!m_CookedContent->FindScript(Root))
						throw std::runtime_error("Cooked Player acceptance dynamic root is not a script: " + Root.ToString());
					auto Script = m_CookedContent->ReadScript(Root);
					if (!Script || Script->empty())
						throw std::runtime_error("Cooked Player acceptance could not read dynamic script root " + Root.ToString());
				}
				PF_INFO("PULSEFORGE_COOKED_DYNAMIC_ROOT=PASS; explicit script root read through provider");
			}
#endif
		}
		auto UserDataRoot = PulseForge::Platform::GetUserDataDirectory();
#if defined(PF_DEBUG)
		if (m_CookedPlayerAcceptance)
		{
			if (!UserDataRoot)
				throw std::runtime_error("Cooked Player acceptance could not resolve its isolated user-data directory");
			PF_INFO("PULSEFORGE_COOKED_USER_DATA=PASS; root={}", PathToUtf8(*UserDataRoot));
		}
#endif
		if (m_CookedContent)
		{
			if (Application.GetRendererAPI() != PulseForge::RendererAPI::Vulkan)
				throw std::runtime_error("Cooked scene rendering currently requires the Vulkan renderer backend");
			PulseForge::GameplayActionMap Defaults;
			if (m_CookedContent->FindGameplayInput(m_CookedContent->GetProjectID()))
			{
				auto Loaded = m_CookedContent->LoadGameplayInput(m_CookedContent->GetProjectID());
				if (!Loaded) throw std::runtime_error("Could not load cooked gameplay input map: " + Loaded.error().Message);
				Defaults = std::move(*Loaded);
			}
			if (UserDataRoot)
			{
				auto Loaded = PulseForge::ApplyGameplayInputOverrides(
					std::move(Defaults), m_CookedContent->GetProjectID(), *UserDataRoot);
				if (!Loaded) throw std::runtime_error("Could not apply cooked player input overrides: " + Loaded.error().Message);
				for (const std::string& Warning : Loaded->Warnings) PF_WARN("Player input map: {}", Warning);
				Defaults = std::move(Loaded->ActionMap);
			}
			else
				PF_WARN("Player input overrides are unavailable: {}; using cooked defaults", UserDataRoot.error().Message);
			if (auto Applied = Application.GetGameplayInput().SetActionMap(std::move(Defaults)); !Applied)
				throw std::runtime_error("Could not apply cooked player input map: " + Applied.error().Message);
#if defined(PF_DEBUG)
			if (m_CookedPlayerAcceptance && !Application.GetGameplayInput().GetActionMap().Actions.empty())
			{
				m_CookedInputMapReady = true;
				PF_INFO("PULSEFORGE_COOKED_INPUT_MAP=PASS; authored actions loaded through cooked provider");
			}
#endif
		}
		else
		{
#if !defined(PF_COOKED_RUNTIME_ONLY)
			auto ActionProject = PulseForge::Project::Open(m_Configuration.ProjectFile);
			if (!ActionProject) throw std::runtime_error("Could not load player input project settings: " + ActionProject.error().Message);
			if (UserDataRoot)
			{
				auto Loaded = PulseForge::LoadGameplayInputMap(
					ActionProject->GetRootPath(), ActionProject->GetDescription().ID, *UserDataRoot);
				if (!Loaded) throw std::runtime_error("Could not load player input map: " + Loaded.error().Message);
				for (const std::string& Warning : Loaded->Warnings) PF_WARN("Player input map: {}", Warning);
				if (auto Applied = Application.GetGameplayInput().SetActionMap(std::move(Loaded->ActionMap)); !Applied)
					throw std::runtime_error("Could not apply player input map: " + Applied.error().Message);
			}
			else
			{
				PF_WARN("Player input overrides are unavailable: {}; using project-authored defaults", UserDataRoot.error().Message);
				auto Defaults = PulseForge::LoadProjectGameplayActionMap(ActionProject->GetRootPath());
				if (!Defaults) throw std::runtime_error("Could not load player input defaults: " + Defaults.error().Message);
				if (auto Applied = Application.GetGameplayInput().SetActionMap(std::move(*Defaults)); !Applied)
					throw std::runtime_error("Could not apply player input defaults: " + Applied.error().Message);
			}
#else
			throw std::runtime_error("Cooked Player cannot load source-project input settings");
#endif
		}
#if defined(PF_COOKED_RUNTIME_ONLY)
		m_Session = std::make_unique<PulseForge::PlayerSession>(m_CookedContent, RuntimeDescription, Services);
#else
		m_Session = m_CookedContent
			? std::make_unique<PulseForge::PlayerSession>(m_CookedContent, RuntimeDescription, Services)
			: std::make_unique<PulseForge::PlayerSession>(m_Configuration.ProjectFile, RuntimeDescription, Services);
#endif
		if (auto Started = m_Session->Start(m_Configuration.StartupScene); !Started)
			throw std::runtime_error("Could not start player session: " + Started.error().Message);
#if defined(PF_DEBUG)
		if (m_CookedPlayerAcceptance && m_CookedContent)
		{
			const auto ActiveSceneAsset = m_Session->GetActiveSceneAsset();
			if (ActiveSceneAsset && *ActiveSceneAsset == m_CookedContent->GetStartupScene())
			{
				m_CookedStartupSceneReady = true;
				PF_INFO("PULSEFORGE_COOKED_STARTUP_SCENE=PASS; manifest startup scene is active");
			}
		}
#endif

		if (m_CookedContent && PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan)
		{
			auto CreatedRenderer = PulseForge::SceneRenderer::Create(
				PulseForge::Application::Get(), m_CookedContent, ResolveCompiledShaderDirectory());
			if (!CreatedRenderer) throw std::runtime_error(CreatedRenderer.error().Message);
			m_SceneRenderer = std::move(*CreatedRenderer);
		}
		else if (!m_CookedContent && PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan)
		{
#if !defined(PF_COOKED_RUNTIME_ONLY)
			auto CreatedRenderer = PulseForge::SceneRenderer::Create(
				PulseForge::Application::Get(), *m_Session->GetProject(), ResolveCompiledShaderDirectory());
			if (!CreatedRenderer) throw std::runtime_error(CreatedRenderer.error().Message);
			m_SceneRenderer = std::move(*CreatedRenderer);
#else
			throw std::runtime_error("Cooked Player cannot construct a source-project renderer");
#endif
		}
	}

	void OnUpdate(PulseForge::Timestep DeltaTime) override
	{
		auto& Application = PulseForge::Application::Get();
#if defined(PF_DEBUG)
		if (m_CookedReadbackPending && !m_CookedReadbackPassed)
			ValidateCookedFrameReadback();
#endif
		PulseForge::InputRoutingState Routing;
		Routing.FocusedWindow = Application.IsMainWindowFocused() ? Application.GetInputWindowID() : 0;
		if (m_Session && Routing.FocusedWindow != 0)
		{
			Routing.Contexts[Routing.ContextCount++] = { .Context = PulseForge::InputContext::Gameplay,
				.Consumes = PulseForge::InputDeviceMask::None, .OwnerWindow = Routing.FocusedWindow, .Active = true };
		}
		Application.GetGameplayInput().PublishFrame(Routing);

		if (m_EditorShutdownChannel.ConsumeRequest())
		{
			m_EditorShutdownRequested = true;
			PF_INFO("PULSEFORGE_EDITOR_CONTROL_ACCEPTED");
			PulseForge::Application::Get().RequestClose();
			return;
		}
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
#if defined(PF_DEBUG)
			if (m_CookedPlayerAcceptance && Advanced && Advanced->PhysicsSteps > 0 && !m_CookedRuntimeReady)
			{
				m_CookedRuntimeReady = true;
				PF_INFO("PULSEFORGE_COOKED_RUNTIME=PASS; physics steps {}; runtime audio update completed", Advanced->PhysicsSteps);
			}
			if (m_CookedPlayerAcceptance && m_Session->GetActiveScene() && !m_CookedScriptReady)
			{
				for (const PulseForge::Entity& Entity : m_Session->GetActiveScene()->GetEntities())
				{
					const auto Script = Entity.GetScript();
					const auto Transform = Entity.GetTransform();
					if (Entity.GetUUID().ToString() == "bfc10000-0000-4000-8000-000000000005" &&
						Script && *Script && Transform && Transform->Translation.x > 12.0f)
					{
						m_CookedScriptReady = true;
						PF_INFO("PULSEFORGE_COOKED_SCRIPT=PASS; cooked Lua changed the active scene through entity APIs");
						break;
					}
				}
			}
			if (m_CookedPlayerAcceptance && m_Session->GetActiveScene() && !m_CookedAudioReady)
			{
				for (const PulseForge::Entity& Entity : m_Session->GetActiveScene()->GetEntities())
				{
					const auto Audio = Entity.GetAudioSource();
					if (Entity.GetUUID().ToString() == "bfc10000-0000-4000-8000-000000000004" &&
						Audio && *Audio && (*Audio)->PlayOnStart)
					{
						m_CookedAudioReady = true;
						PF_INFO("PULSEFORGE_COOKED_AUDIO=PASS; cooked audio source started on Null output");
						break;
					}
				}
			}
#endif
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
#if defined(PF_DEBUG)
		if (m_CookedPlayerAcceptance && m_SceneRenderer->HasPreparedEnvironmentLighting() && !m_CookedEnvironmentReady)
		{
			m_CookedEnvironmentReady = true;
			PF_INFO("PULSEFORGE_COOKED_ENVIRONMENT=PASS; cooked environment product prepared for rendering");
		}
		if (m_CookedPlayerAcceptance && m_CookedReadbackPassed && m_CookedRuntimeReady && m_CookedScriptReady &&
			m_CookedStartupSceneReady && m_CookedInputMapReady && m_CookedEnvironmentReady && m_CookedAudioReady &&
			!m_CookedAcceptanceShutdownRequested)
		{
			m_CookedAcceptanceShutdownRequested = true;
			PF_INFO("PULSEFORGE_COOKED_PLAYER_ACCEPTANCE=PASS; requesting orderly shutdown");
			Application.RequestClose();
		}
#endif
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
#if defined(PF_DEBUG)
			if (m_CookedPlayerAcceptance && !m_CookedReadbackPassed)
				m_CookedReadbackPending = true;
#endif
		}
	}

	~ExampleLayer() override
	{
		m_SceneRenderer.reset();
		m_Session.reset();
		if (m_PlayerSmokeShutdownRequested) PF_INFO("PULSEFORGE_PLAYER_RELOCATION_SMOKE_SHUTDOWN=PASS");
#if defined(PF_DEBUG)
		if (m_CookedAcceptanceShutdownRequested) PF_INFO("PULSEFORGE_COOKED_PLAYER_SHUTDOWN=PASS");
#endif
		if (m_EditorShutdownRequested) PF_INFO("PULSEFORGE_PLAYER_ORDERLY_STOP=PASS");
	}

private:
#if defined(PF_DEBUG)
	void ValidateCookedFrameReadback()
	{
		m_CookedReadbackPending = false;
		const PulseForge::Texture* Color = m_SceneRenderer->GetHdrColorTextureForValidation();
		const PulseForge::Texture* Depth = m_SceneRenderer->GetHdrDepthTextureForValidation();
		if (!Color || !Depth)
		{
			PF_ERROR("Cooked Player acceptance could not retain HDR attachments for readback");
			return;
		}
		auto& Application = PulseForge::Application::Get();
		const auto ColorReadback = Application.ReadTextureForValidation(*Color);
		const auto DepthReadback = Application.ReadTextureForValidation(*Depth);
		if (!ColorReadback || !DepthReadback || ColorReadback->Format != PulseForge::TextureFormat::RGBA16_Float ||
			DepthReadback->Format != PulseForge::TextureFormat::Depth32Float || ColorReadback->Width == 0 ||
			ColorReadback->Height == 0 || ColorReadback->Width != DepthReadback->Width ||
			ColorReadback->Height != DepthReadback->Height ||
			ColorReadback->Pixels.size() != static_cast<size_t>(ColorReadback->Width) * ColorReadback->Height * 8 ||
			DepthReadback->Pixels.size() != static_cast<size_t>(DepthReadback->Width) * DepthReadback->Height * sizeof(float))
		{
			PF_ERROR("Cooked Player acceptance HDR/depth readback failed or returned invalid dimensions");
			return;
		}
		size_t WrittenDepthPixels = 0;
		for (size_t Offset = 0; Offset < DepthReadback->Pixels.size(); Offset += sizeof(float))
		{
			float Value = 1.0f;
			std::memcpy(&Value, DepthReadback->Pixels.data() + Offset, sizeof(Value));
			if (std::isfinite(Value) && Value >= 0.0f && Value < 0.99999f) ++WrittenDepthPixels;
		}
		std::set<std::array<uint16_t, 3>> DistinctColorSamples;
		for (size_t Offset = 0; Offset < ColorReadback->Pixels.size() && DistinctColorSamples.size() < 3; Offset += 8)
		{
			std::array<uint16_t, 4> Pixel{};
			std::memcpy(Pixel.data(), ColorReadback->Pixels.data() + Offset, 8);
			if (((Pixel[0] & 0x7c00) != 0x7c00) && ((Pixel[1] & 0x7c00) != 0x7c00) &&
				((Pixel[2] & 0x7c00) != 0x7c00))
				DistinctColorSamples.insert({ Pixel[0], Pixel[1], Pixel[2] });
		}
		if (WrittenDepthPixels < 16 || DistinctColorSamples.size() < 2)
		{
			PF_ERROR("Cooked Player acceptance readback lacked rendered geometry or varied finite HDR color samples");
			return;
		}
		m_CookedReadbackPassed = true;
		PF_INFO("PULSEFORGE_COOKED_RENDER_READBACK=PASS; written_depth_pixels={}; distinct_hdr_samples={}",
			WrittenDepthPixels, DistinctColorSamples.size());
	}
#endif
	PulseForgeGame::PlayerLaunchConfiguration m_Configuration;
	std::shared_ptr<const PulseForge::CookedContentProvider> m_CookedContent;
	std::unique_ptr<PulseForge::PlayerSession> m_Session;
	std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
	bool m_LoggedPrepareFailure = false;
	bool m_LoggedRenderFailure = false;
	bool m_LoggedRuntimeFailure = false;
	bool m_LoggedSceneDraw = false;
	bool m_PlayerSmokeExitAfterRender = false;
	bool m_PlayerSmokeShutdownRequested = false;
	bool m_PlayerRuntimeSmokeLogged = false;
#if defined(PF_DEBUG)
	bool m_CookedPlayerAcceptance = false;
	bool m_CookedStartupSceneReady = false;
	bool m_CookedInputMapReady = false;
	bool m_CookedAudioReady = false;
	bool m_CookedRuntimeReady = false;
	bool m_CookedScriptReady = false;
	bool m_CookedEnvironmentReady = false;
	bool m_CookedReadbackPending = false;
	bool m_CookedReadbackPassed = false;
	bool m_CookedAcceptanceShutdownRequested = false;
#endif
	EditorShutdownChannel m_EditorShutdownChannel;
	bool m_EditorShutdownRequested = false;
};

PulseForgeGameApp::PulseForgeGameApp(PulseForgeGame::PlayerLaunchConfiguration Configuration, bool EditorControlEnabled)
	: Application(ResolveRendererAPI(Configuration),
		  PulseForge::WindowProps("PulseForge Player", Configuration.WindowWidth.value_or(1280), Configuration.WindowHeight.value_or(720)))
{
	if (Configuration.DiagnosticsEnabled && !StartDiagnosticsCapture())
		throw std::runtime_error("Could not start the requested bounded diagnostics capture.");
	PushLayer(std::make_unique<ExampleLayer>(std::move(Configuration), EditorControlEnabled));
}

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication(int ArgumentCount, char** Arguments)
{
	auto Parsed = ParseArguments(ArgumentCount, Arguments);
	return std::make_unique<PulseForgeGameApp>(std::move(Parsed.Configuration), Parsed.EditorControlEnabled);
}
