#include "Client/PulseForgeGame.h"
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#endif
#include "Assets/AudioAssetCache.h"
#include "Audio/AudioSession.h"
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
#include <algorithm>
#include <array>
#include <charconv>
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
		if (const char* UISmoke = std::getenv("PULSEFORGE_RUNTIME_UI_PACKAGE_SMOKE"))
			m_RuntimeUIPackageSmoke = std::string_view(UISmoke) == "1";
		if (const char* UIAcceptance = std::getenv("PULSEFORGE_RUNTIME_UI_PACKAGE_ACCEPTANCE"))
			m_RuntimeUIPackageAcceptance = std::string_view(UIAcceptance) == "1";
		if (const char* Acceptance = std::getenv("PULSEFORGE_COOKED_PLAYER_ACCEPTANCE"))
			m_CookedPlayerAcceptance = std::string_view(Acceptance) == "1";
		if (const char* AudioAcceptance = std::getenv("PULSEFORGE_AUDIO_MIXING_PACKAGE_ACCEPTANCE"))
		{
			const std::string_view Value(AudioAcceptance);
			m_AudioMixingPackageAcceptance = Value == "exercise" || Value == "verify";
			m_AudioMixingPackageVerify = Value == "verify";
			if (!m_AudioMixingPackageAcceptance)
				throw std::runtime_error("Audio mixing package acceptance mode must be 'exercise' or 'verify'");
		}
#endif
		PulseForge::SceneRuntimeServices Services{ .InputState = &PulseForge::Application::Get().GetInput(),
			.GameplayInput = &PulseForge::Application::Get().GetGameplayInput(),
			.Diagnostics = PulseForge::Application::Get().GetDiagnosticsRecorder() };
		auto& Application = PulseForge::Application::Get();
		PulseForge::SceneRuntimeDesc RuntimeDescription{};
#if defined(PF_DEBUG)
		if (m_CookedPlayerAcceptance || m_RuntimeUIPackageAcceptance || m_AudioMixingPackageAcceptance)
		{
			RuntimeDescription.Audio.OutputBackend = PulseForge::AudioOutputBackend::Null;
			if (m_CookedPlayerAcceptance)
				PF_INFO("PULSEFORGE_COOKED_AUDIO_BACKEND=NULL; deterministic output selected for process acceptance");
			if (m_RuntimeUIPackageAcceptance)
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_AUDIO_BACKEND=NULL; deterministic output selected for G18 package acceptance");
			if (m_AudioMixingPackageAcceptance)
				PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_BACKEND=NULL; explicit silent output selected for package acceptance");
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
		if (m_CookedPlayerAcceptance || m_RuntimeUIPackageAcceptance || m_AudioMixingPackageAcceptance)
		{
			if (!UserDataRoot)
				throw std::runtime_error("Player package acceptance could not resolve its isolated user-data directory");
			if (m_CookedPlayerAcceptance)
				PF_INFO("PULSEFORGE_COOKED_USER_DATA=PASS; root={}", PathToUtf8(*UserDataRoot));
			if (m_RuntimeUIPackageAcceptance)
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_USER_DATA=PASS; root={}", PathToUtf8(*UserDataRoot));
			if (m_AudioMixingPackageAcceptance)
				PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_USER_DATA=PASS; root={}", PathToUtf8(*UserDataRoot));
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
#if defined(PF_DEBUG)
		if (m_RuntimeUIPackageAcceptance && Application.GetInputWindowID() != 0)
			Routing.FocusedWindow = Application.GetInputWindowID();
		AdvanceRuntimeUIPackageAcceptanceInput(Application, DeltaTime, Routing.FocusedWindow);
		if (m_AudioMixingPackageAcceptance && Application.GetInputWindowID() != 0)
			Routing.FocusedWindow = Application.GetInputWindowID();
		AdvanceAudioMixingPackageInput(Application, DeltaTime, Routing.FocusedWindow);
#endif
		if (m_Session && Routing.FocusedWindow != 0)
		{
			if (m_Session->HasRuntimeUI())
				(void)PulseForge::AppendRuntimeUIInputContext(
					Routing, m_Session->GetRuntimeUIInputPolicy(), Routing.FocusedWindow);
			Routing.Contexts[Routing.ContextCount++] = { .Context = PulseForge::InputContext::Gameplay,
				.Consumes = PulseForge::InputDeviceMask::None, .OwnerWindow = Routing.FocusedWindow, .Active = true };
		}
		Application.GetGameplayInput().PublishFrame(Routing);
		if (m_Session && m_Session->HasRuntimeUI())
		{
			const PulseForge::RuntimeUIInputPolicy Policy = m_Session->GetRuntimeUIInputPolicy();
			const auto [FramebufferWidth, FramebufferHeight] = Application.GetWindow().GetFramebufferSize();
			const PulseForge::RuntimeUIViewport Viewport{
				.LogicalSize = { static_cast<float>(Application.GetWindow().GetWidth()),
					static_cast<float>(Application.GetWindow().GetHeight()) },
				.FramebufferWidth = FramebufferWidth, .FramebufferHeight = FramebufferHeight };
			PulseForge::RuntimeUIHostInputSample Sample{
				.SessionGeneration = m_Session->GetRuntimeUISessionGeneration(),
				.TreeGeneration = m_Session->GetRuntimeUIInputGeneration(), .InputOwner = Routing.FocusedWindow,
				.Viewport = Viewport, .DeltaSeconds = DeltaTime.GetSeconds(), .Focused = Routing.FocusedWindow != 0 };
			if (Policy != PulseForge::RuntimeUIInputPolicy::Passive)
			{
				const auto PointerPosition = Routing.FocusedWindow != 0
					? Application.GetGameplayInput().GetPointerPosition(Routing.FocusedWindow) : std::nullopt;
				const auto* Navigate = Application.GetGameplayInput().FindAction("ui.navigate");
				const auto* Activate = Application.GetGameplayInput().FindAction("ui.activate");
				const auto* Cancel = Application.GetGameplayInput().FindAction("ui.cancel");
				const auto* Pointer = Application.GetGameplayInput().FindAction("ui.pointer");
				Sample.PointerPosition = PointerPosition
					? std::optional<glm::vec2>{{ PointerPosition->X, PointerPosition->Y }} : std::nullopt;
				Sample.PointerPressed = Pointer && Pointer->Pressed;
				Sample.PointerHeld = Pointer && Pointer->Held;
				Sample.PointerReleased = Pointer && Pointer->Released;
				Sample.Navigation = Navigate ? glm::vec2{ Navigate->X, Navigate->Y } : glm::vec2{};
				Sample.ActivatePressed = Activate && Activate->Pressed;
				Sample.ActivateHeld = Activate && Activate->Held;
				Sample.CancelPressed = Cancel && Cancel->Pressed;
				Sample.CancelHeld = Cancel && Cancel->Held;
			}
			const PulseForge::RuntimeUIHostFrame Frame = PulseForge::BuildRuntimeUIHostFrame(
				m_RuntimeUIInputAdapter, std::move(Sample), Policy);
			(void)m_Session->SubmitRuntimeUIFrame(Frame.Viewport, Frame.Events);
		}

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
			if (m_RuntimeUIPackageAcceptance)
				AdvanceRuntimeUIPackageAcceptanceChecks(Application);
			if (m_AudioMixingPackageAcceptance)
				AdvanceAudioMixingPackageAcceptanceChecks(Application);
#endif
			if (Advanced && m_PlayerSmokeExitAfterRender && !m_PlayerRuntimeSmokeLogged && Advanced->PhysicsSteps > 0)
			{
				m_PlayerRuntimeSmokeLogged = true;
				PF_INFO(
					"PULSEFORGE_PLAYER_RUNTIME_SMOKE=PASS; scripting advanced; physics_steps={}; audio advanced", Advanced->PhysicsSteps);
			}
		}
		if (!m_SceneRenderer || !m_Session || !m_Session->GetActiveScene()) return;
		if (m_Session->HasRuntimeUI())
			m_SceneRenderer->SetRuntimeUIFrame(
				m_Session->GetRuntimeUIDrawCommands(), m_Session->GetRuntimeUIViewport(), true);
		else
			m_SceneRenderer->SetRuntimeUIFrame({}, {}, false);

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
#if defined(PF_DEBUG)
		if (m_RuntimeUIPackageAcceptance && !m_RuntimeUIPackageMenuRendered && m_Session && m_Session->HasRuntimeUI())
		{
			const auto Commands = m_Session->GetRuntimeUIDrawCommands();
			const bool HasTitle = std::any_of(Commands.begin(), Commands.end(),
				[](const PulseForge::RuntimeUIDrawCommand& Command) { return Command.Text.find("Café PulseForge") != std::string::npos; });
			const bool HasStartButton = std::any_of(Commands.begin(), Commands.end(),
				[](const PulseForge::RuntimeUIDrawCommand& Command)
				{ return Command.Kind == PulseForge::RuntimeUIElementKind::Button && Command.Text == "Start"; });
			const bool HasImage = std::any_of(Commands.begin(), Commands.end(),
				[](const PulseForge::RuntimeUIDrawCommand& Command)
				{ return Command.Kind == PulseForge::RuntimeUIElementKind::Image && !Command.Image.IsNil(); });
			if (HasTitle && HasStartButton && HasImage && Rendered->RuntimeUIQuadCount > 0 &&
				Rendered->RuntimeUITextureCount >= 2 && Rendered->RuntimeUIFontAtlasCount > 0)
			{
				m_RuntimeUIPackageMenuRendered = true;
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_MENU=PASS; rendered sampled image and Café PulseForge/Start glyphs; "
					"quads={}; batches={}; textures={}; font_atlases={}; input=synthetic-mapped",
					Rendered->RuntimeUIQuadCount, Rendered->RuntimeUIBatchCount, Rendered->RuntimeUITextureCount,
					Rendered->RuntimeUIFontAtlasCount);
			}
		}
		if (m_RuntimeUIPackageAcceptance && m_RuntimeUIPackageAcceptanceComplete && !m_RuntimeUIPackageAcceptanceShutdownRequested)
		{
			m_RuntimeUIPackageAcceptanceShutdownRequested = true;
			PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_ACCEPTANCE=PASS; normal shutdown requested");
			PulseForge::Application::Get().RequestClose();
		}
		if (m_AudioMixingPackageAcceptance && !m_AudioMixingPackageInitialFrameRendered && m_Session &&
			m_Session->HasRuntimeUI() && m_Session->GetAudioSession())
		{
			const auto Commands = m_Session->GetRuntimeUIDrawCommands();
			const auto HasText = [&Commands](std::string_view Text)
			{
				return std::any_of(Commands.begin(), Commands.end(), [Text](const PulseForge::RuntimeUIDrawCommand& Command)
					{ return Command.Text.find(Text) != std::string::npos; });
			};
			const PulseForge::AudioMixState Mix = m_Session->GetAudioSession()->GetMixState();
			const bool InitialValuesVisible = m_AudioMixingPackageVerify
				? HasText("Music: 90%") && HasText("Effects: 85%") && HasText("UI: 90%")
				: HasText("Music: 80%") && HasText("Effects: 75%") && HasText("UI: 90%");
			const bool InitialMixValid = m_AudioMixingPackageVerify
				? std::abs(Mix.Gains.Music - 0.9f) < 0.001f && std::abs(Mix.Gains.Effects - 0.85f) < 0.001f &&
					std::abs(Mix.Gains.UI - 0.9f) < 0.001f && Mix.MusicMuted
				: std::abs(Mix.Gains.Music - 0.8f) < 0.001f && std::abs(Mix.Gains.Effects - 0.75f) < 0.001f &&
					std::abs(Mix.Gains.UI - 0.9f) < 0.001f;
			const size_t LoadedCookedClips = m_Session->GetAudioSession()->GetAssets().GetLoadedCount();
			if (HasText("Audio authoring and mixing") && InitialValuesVisible && InitialMixValid &&
				Rendered->RuntimeUIQuadCount > 0 && Rendered->RuntimeUIFontAtlasCount > 0 && LoadedCookedClips > 0)
			{
				m_AudioMixingPackageInitialFrameRendered = true;
				PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_UI=PASS; rendered the cooked sample labels/font; quads={}; batches={}; "
					"font_atlases={}; cooked_audio_clips={}; output=null",
					Rendered->RuntimeUIQuadCount, Rendered->RuntimeUIBatchCount, Rendered->RuntimeUIFontAtlasCount, LoadedCookedClips);
			}
		}
		if (m_AudioMixingPackageAcceptance && m_AudioMixingPackageComplete && !m_AudioMixingPackageShutdownRequested)
		{
			m_AudioMixingPackageShutdownRequested = true;
			PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_ACCEPTANCE=PASS; normal shutdown requested; synthetic mapped input");
			PulseForge::Application::Get().RequestClose();
		}
		if (m_RuntimeUIPackageSmoke && !m_RuntimeUIPackageSmokeShutdownRequested && Rendered->RuntimeUIQuadCount > 0)
		{
			m_RuntimeUIPackageSmokeShutdownRequested = true;
			PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE=PASS; quads={}; batches={}; textures={}; font_atlases={}; requesting orderly shutdown",
				Rendered->RuntimeUIQuadCount, Rendered->RuntimeUIBatchCount, Rendered->RuntimeUITextureCount,
				Rendered->RuntimeUIFontAtlasCount);
			PulseForge::Application::Get().RequestClose();
		}
#endif
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
		if (m_RuntimeUIPackageSmokeShutdownRequested) PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_SHUTDOWN=PASS");
		if (m_RuntimeUIPackageAcceptanceComplete && !m_RuntimeUIPackageAcceptanceFailed)
			PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_SHUTDOWN=PASS; Player exited normally after menu/gameplay/pause/resume interaction");
		if (m_AudioMixingPackageComplete && !m_AudioMixingPackageFailed)
			PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_SHUTDOWN=PASS; Player exited normally after settings restore or mixing/pause interaction");
#endif
		if (m_EditorShutdownRequested) PF_INFO("PULSEFORGE_PLAYER_ORDERLY_STOP=PASS");
	}

private:
#if defined(PF_DEBUG)
	enum class RuntimeUIPackageAcceptanceStage : uint8_t
	{
		ObserveMenu,
		StartPressed,
		StartReleased,
		AwaitGameplay,
		GameplayActionPressed,
		GameplayActionReleased,
		AwaitGameplayAction,
		MovePausePointer,
		PausePressed,
		PauseReleased,
		AwaitPaused,
		BlockedActionPressed,
		BlockedActionReleased,
		AwaitBlockedAction,
		MoveResumePointer,
		ResumePressed,
		ResumeReleased,
		AwaitResumed,
		ResumedActionPressed,
		ResumedActionReleased,
		AwaitResumedAction,
		Complete,
		Failed
	};

	enum class AudioMixingPackageAcceptanceStage : uint8_t
	{
		WaitForRenderedUI,
		MovePointer,
		PressPointer,
		ReleasePointer,
		WaitForAction,
		Complete,
		Failed
	};

	struct AudioMixingPackageButton final
	{
		float X;
		float Y;
		std::string_view Name;
	};

	static constexpr std::array<AudioMixingPackageButton, 9> AudioMixingPackageButtons = {{
		{ 387.5f, 118.5f, "Music +" },
		{ 387.5f, 198.5f, "Effects +" },
		{ 387.5f, 278.5f, "UI +" },
		{ 510.0f, 118.5f, "Mute music" },
		{ 527.5f, 198.5f, "Play effect" },
		{ 185.0f, 430.0f, "Pause gameplay" },
		{ 302.5f, 278.5f, "UI -" },
		{ 440.0f, 430.0f, "Resume" },
		{ 707.5f, 430.0f, "Save settings" }
	}};

	void AdvanceAudioMixingPackageInput(
		PulseForge::Application& Application, PulseForge::Timestep DeltaTime, PulseForge::InputWindowID Window)
	{
		if (!m_AudioMixingPackageAcceptance || m_AudioMixingPackageVerify || m_AudioMixingPackageComplete ||
			m_AudioMixingPackageFailed)
			return;
		m_AudioMixingPackageElapsed += std::clamp(DeltaTime.GetSeconds(), 0.0, 0.25);
		if (m_AudioMixingPackageElapsed > 45.0)
		{
			FailAudioMixingPackageAcceptance(Application, "the bounded mapped-input sequence exceeded 45 seconds");
			return;
		}
		if (Window == 0 || !m_Session || !m_AudioMixingPackageInitialFrameRendered)
			return;
		if (m_AudioMixingPackageActionIndex >= AudioMixingPackageButtons.size())
			return;
		const AudioMixingPackageButton& Button = AudioMixingPackageButtons[m_AudioMixingPackageActionIndex];
		switch (m_AudioMixingPackageStage)
		{
			case AudioMixingPackageAcceptanceStage::WaitForRenderedUI:
			case AudioMixingPackageAcceptanceStage::MovePointer:
				if (QueueAudioMixingPointer(Application, Window, PulseForge::InputEventKind::PointerMoved, Button.X, Button.Y))
					m_AudioMixingPackageStage = AudioMixingPackageAcceptanceStage::PressPointer;
				break;
			case AudioMixingPackageAcceptanceStage::PressPointer:
				if (QueueAudioMixingPointer(Application, Window, PulseForge::InputEventKind::Pressed, Button.X, Button.Y))
					m_AudioMixingPackageStage = AudioMixingPackageAcceptanceStage::ReleasePointer;
				break;
			case AudioMixingPackageAcceptanceStage::ReleasePointer:
				if (QueueAudioMixingPointer(Application, Window, PulseForge::InputEventKind::Released, Button.X, Button.Y))
					m_AudioMixingPackageStage = AudioMixingPackageAcceptanceStage::WaitForAction;
				break;
			case AudioMixingPackageAcceptanceStage::WaitForAction:
			case AudioMixingPackageAcceptanceStage::Complete:
			case AudioMixingPackageAcceptanceStage::Failed:
				break;
		}
	}

	bool QueueAudioMixingPointer(
		PulseForge::Application& Application,
		PulseForge::InputWindowID Window,
		PulseForge::InputEventKind Kind,
		float X,
		float Y)
	{
		PulseForge::InputEvent Event;
		Event.Kind = Kind;
		Event.Window = Window;
		if (Kind == PulseForge::InputEventKind::PointerMoved)
		{
			Event.Control = { .Device = PulseForge::InputDevice::MouseMotion,
				.Code = static_cast<uint16_t>(PulseForge::MouseControl::MotionX) };
			Event.X = X;
			Event.Y = Y;
		}
		else
			Event.Control = { .Device = PulseForge::InputDevice::MouseButton,
				.Code = static_cast<uint16_t>(PulseForge::MouseControl::Left) };
		if (Application.GetGameplayInput().QueueEvent(Event))
			return true;
		FailAudioMixingPackageAcceptance(Application, "the mapped Player input queue rejected a synthetic pointer event");
		return false;
	}

	bool HasAudioMixingPackageText(std::string_view Text)
	{
		if (!m_Session) return false;
		const auto Commands = m_Session->GetRuntimeUIDrawCommands();
		return std::any_of(Commands.begin(), Commands.end(), [Text](const PulseForge::RuntimeUIDrawCommand& Command)
			{ return Command.Text.find(Text) != std::string::npos; });
	}

	void AdvanceAudioMixingPackageAcceptanceChecks(PulseForge::Application& Application)
	{
		if (!m_AudioMixingPackageAcceptance || m_AudioMixingPackageComplete || m_AudioMixingPackageFailed || !m_Session)
			return;
		if (m_AudioMixingPackageElapsed > 45.0)
		{
			FailAudioMixingPackageAcceptance(Application, "the bounded package interaction exceeded 45 seconds");
			return;
		}
		if (!m_AudioMixingPackageInitialFrameRendered)
			return;
		auto* Audio = m_Session->GetAudioSession();
		if (!Audio)
		{
			FailAudioMixingPackageAcceptance(Application, "PlayerSession did not retain its cooked AudioSession");
			return;
		}
		const PulseForge::AudioMixState Mix = Audio->GetMixState();
		if (m_AudioMixingPackageVerify)
		{
			if (!Audio->GetSettingsDiagnostic().empty() || std::abs(Mix.Gains.Music - 0.9f) >= 0.001f ||
				std::abs(Mix.Gains.Effects - 0.85f) >= 0.001f || std::abs(Mix.Gains.UI - 0.9f) >= 0.001f ||
				!Mix.MusicMuted)
			{
				FailAudioMixingPackageAcceptance(Application, "a new Player process did not restore the saved project-scoped mix");
				return;
			}
			m_AudioMixingPackageComplete = true;
			PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_SETTINGS_RESTORED=PASS; second Player process restored gains and music mute");
			return;
		}

		switch (m_AudioMixingPackageStage)
		{
			case AudioMixingPackageAcceptanceStage::WaitForRenderedUI:
			case AudioMixingPackageAcceptanceStage::MovePointer:
			case AudioMixingPackageAcceptanceStage::PressPointer:
			case AudioMixingPackageAcceptanceStage::ReleasePointer:
			case AudioMixingPackageAcceptanceStage::Complete:
			case AudioMixingPackageAcceptanceStage::Failed:
				break;
			case AudioMixingPackageAcceptanceStage::WaitForAction:
			{
				bool ActionObserved = false;
				switch (m_AudioMixingPackageActionIndex)
				{
					case 0: ActionObserved = std::abs(Mix.Gains.Music - 0.9f) < 0.001f && HasAudioMixingPackageText("Music: 90%"); break;
					case 1: ActionObserved = std::abs(Mix.Gains.Effects - 0.85f) < 0.001f && HasAudioMixingPackageText("Effects: 85%"); break;
					case 2: ActionObserved = std::abs(Mix.Gains.UI - 1.0f) < 0.001f && HasAudioMixingPackageText("UI: 100%"); break;
					case 3: ActionObserved = Mix.MusicMuted && HasAudioMixingPackageText("Music: 90% (muted)"); break;
					case 4: ActionObserved = Audio->GetAssets().GetLoadedCount() >= 3; break;
					case 5: ActionObserved = m_Session->IsPaused(); break;
					case 6: ActionObserved = m_Session->IsPaused() && std::abs(Mix.Gains.UI - 0.9f) < 0.001f &&
						HasAudioMixingPackageText("UI: 90%"); break;
					case 7: ActionObserved = !m_Session->IsPaused(); break;
					case 8: ActionObserved = !Audio->HasUnsavedUserSettings() && !Audio->GetSettingsPath().empty() &&
						std::filesystem::exists(Audio->GetSettingsPath()); break;
				}
				if (ActionObserved)
				{
					PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; {}", AudioMixingPackageButtons[m_AudioMixingPackageActionIndex].Name);
					++m_AudioMixingPackageActionIndex;
					if (m_AudioMixingPackageActionIndex == AudioMixingPackageButtons.size())
					{
						m_AudioMixingPackageComplete = true;
						m_AudioMixingPackageStage = AudioMixingPackageAcceptanceStage::Complete;
						PF_INFO("PULSEFORGE_AUDIO_MIXING_PACKAGE_SETTINGS_SAVED=PASS; dirty state cleared after atomic publication");
					}
					else
						m_AudioMixingPackageStage = AudioMixingPackageAcceptanceStage::MovePointer;
				}
				break;
			}
		}
	}

	void FailAudioMixingPackageAcceptance(PulseForge::Application& Application, std::string_view Reason)
	{
		if (m_AudioMixingPackageFailed) return;
		m_AudioMixingPackageFailed = true;
		m_AudioMixingPackageStage = AudioMixingPackageAcceptanceStage::Failed;
		PF_ERROR("PULSEFORGE_AUDIO_MIXING_PACKAGE_ACCEPTANCE=FAIL; {}", Reason);
		Application.RequestClose();
	}

	void AdvanceRuntimeUIPackageAcceptanceInput(
		PulseForge::Application& Application, PulseForge::Timestep DeltaTime, PulseForge::InputWindowID Window)
	{
		if (!m_RuntimeUIPackageAcceptance || m_RuntimeUIPackageAcceptanceComplete || m_RuntimeUIPackageAcceptanceFailed) return;
		m_RuntimeUIPackageAcceptanceElapsed += std::clamp(DeltaTime.GetSeconds(), 0.0, 0.25);
		if (m_RuntimeUIPackageAcceptanceElapsed > 30.0)
		{
			FailRuntimeUIPackageAcceptance(Application, "the bounded interaction sequence exceeded 30 seconds");
			return;
		}
		if (Window == 0) return;
		switch (m_RuntimeUIPackageAcceptanceStage)
		{
		case RuntimeUIPackageAcceptanceStage::ObserveMenu:
			if (m_RuntimeUIPackageMenuRendered) m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::StartPressed;
			break;
		case RuntimeUIPackageAcceptanceStage::StartPressed:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Enter, PulseForge::InputEventKind::Pressed))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::StartReleased;
			break;
		case RuntimeUIPackageAcceptanceStage::StartReleased:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Enter, PulseForge::InputEventKind::Released))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::AwaitGameplay;
			break;
		case RuntimeUIPackageAcceptanceStage::GameplayActionPressed:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Space, PulseForge::InputEventKind::Pressed))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::GameplayActionReleased;
			break;
		case RuntimeUIPackageAcceptanceStage::GameplayActionReleased:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Space, PulseForge::InputEventKind::Released))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::AwaitGameplayAction;
			break;
		case RuntimeUIPackageAcceptanceStage::MovePausePointer:
			if (QueueRuntimeUIPackagePointer(Application, Window, 840.0f, 58.0f))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::PausePressed;
			break;
		case RuntimeUIPackageAcceptanceStage::PausePressed:
			if (QueueRuntimeUIPackagePointerButton(Application, Window, PulseForge::InputEventKind::Pressed))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::PauseReleased;
			break;
		case RuntimeUIPackageAcceptanceStage::PauseReleased:
			if (QueueRuntimeUIPackagePointerButton(Application, Window, PulseForge::InputEventKind::Released))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::AwaitPaused;
			break;
		case RuntimeUIPackageAcceptanceStage::BlockedActionPressed:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Space, PulseForge::InputEventKind::Pressed))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::BlockedActionReleased;
			break;
		case RuntimeUIPackageAcceptanceStage::BlockedActionReleased:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Space, PulseForge::InputEventKind::Released))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::AwaitBlockedAction;
			break;
		case RuntimeUIPackageAcceptanceStage::MoveResumePointer:
			if (QueueRuntimeUIPackagePointer(Application, Window, 480.0f, 251.0f))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::ResumePressed;
			break;
		case RuntimeUIPackageAcceptanceStage::ResumePressed:
			if (QueueRuntimeUIPackagePointerButton(Application, Window, PulseForge::InputEventKind::Pressed))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::ResumeReleased;
			break;
		case RuntimeUIPackageAcceptanceStage::ResumeReleased:
			if (QueueRuntimeUIPackagePointerButton(Application, Window, PulseForge::InputEventKind::Released))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::AwaitResumed;
			break;
		case RuntimeUIPackageAcceptanceStage::ResumedActionPressed:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Space, PulseForge::InputEventKind::Pressed))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::ResumedActionReleased;
			break;
		case RuntimeUIPackageAcceptanceStage::ResumedActionReleased:
			if (QueueRuntimeUIPackageKey(Application, Window, PulseForge::InputKey::Space, PulseForge::InputEventKind::Released))
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::AwaitResumedAction;
			break;
		case RuntimeUIPackageAcceptanceStage::AwaitGameplay:
		case RuntimeUIPackageAcceptanceStage::AwaitGameplayAction:
		case RuntimeUIPackageAcceptanceStage::AwaitPaused:
		case RuntimeUIPackageAcceptanceStage::AwaitBlockedAction:
		case RuntimeUIPackageAcceptanceStage::AwaitResumed:
		case RuntimeUIPackageAcceptanceStage::AwaitResumedAction:
		case RuntimeUIPackageAcceptanceStage::Complete:
		case RuntimeUIPackageAcceptanceStage::Failed:
			break;
		}
	}

	void AdvanceRuntimeUIPackageAcceptanceChecks(PulseForge::Application& Application)
	{
		if (!m_RuntimeUIPackageAcceptance || m_RuntimeUIPackageAcceptanceComplete || m_RuntimeUIPackageAcceptanceFailed || !m_Session)
			return;
		const auto Score = ReadRuntimeUIPackageScore();
		switch (m_RuntimeUIPackageAcceptanceStage)
		{
		case RuntimeUIPackageAcceptanceStage::AwaitGameplay:
			if (Score && HasRuntimeUIPackageText("Pause"))
			{
				m_RuntimeUIPackageScoreBeforeAction = *Score;
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::GameplayActionPressed;
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_GAMEPLAY=PASS; menu activation loaded the cooked gameplay UI and image");
			}
			break;
		case RuntimeUIPackageAcceptanceStage::AwaitGameplayAction:
			if (Score && *Score >= m_RuntimeUIPackageScoreBeforeAction + 10)
			{
				m_RuntimeUIPackageScoreBeforeAction = *Score;
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::MovePausePointer;
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_HUD=PASS; mapped gameplay action changed the visible HUD score from {0} to {1}",
					m_RuntimeUIPackageScoreBeforeAction - 10, *Score);
			}
			break;
		case RuntimeUIPackageAcceptanceStage::AwaitPaused:
			if (m_Session->IsPaused() && Score)
			{
				m_RuntimeUIPackagePausedScore = *Score;
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::BlockedActionPressed;
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_PAUSE=PASS; pause action left the runtime UI live and modal");
			}
			break;
		case RuntimeUIPackageAcceptanceStage::AwaitBlockedAction:
			if (m_Session->IsPaused() && Score && *Score == m_RuntimeUIPackagePausedScore)
			{
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::MoveResumePointer;
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_MODAL_INPUT=PASS; Space did not advance gameplay while paused");
			}
			break;
		case RuntimeUIPackageAcceptanceStage::AwaitResumed:
			if (!m_Session->IsPaused() && Score)
			{
				m_RuntimeUIPackageScoreBeforeAction = *Score;
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::ResumedActionPressed;
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_RESUME=PASS; pointer activation resumed gameplay");
			}
			break;
		case RuntimeUIPackageAcceptanceStage::AwaitResumedAction:
			if (Score && *Score >= m_RuntimeUIPackageScoreBeforeAction + 10)
			{
				m_RuntimeUIPackageAcceptanceComplete = true;
				m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::Complete;
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_RESUMED_GAMEPLAY=PASS; mapped gameplay input advanced after resume");
				PF_INFO("PULSEFORGE_RUNTIME_UI_PACKAGE_ACCEPTANCE=PASS; normal shutdown requested");
				m_RuntimeUIPackageAcceptanceShutdownRequested = true;
				Application.RequestClose();
			}
			break;
		default:
			break;
		}
		if (m_RuntimeUIPackageAcceptanceFailed) return;
		if (m_RuntimeUIPackageAcceptanceComplete) Application.RequestClose();
	}

	bool QueueRuntimeUIPackageKey(PulseForge::Application& Application, PulseForge::InputWindowID Window,
		PulseForge::InputKey Key, PulseForge::InputEventKind Kind)
	{
		if (!Application.GetGameplayInput().QueueEvent({ .Kind = Kind,
			.Control = { .Device = PulseForge::InputDevice::Keyboard, .Code = static_cast<uint16_t>(Key) }, .Window = Window }))
		{
			FailRuntimeUIPackageAcceptance(Application, "could not queue a synthetic mapped keyboard event");
			return false;
		}
		return true;
	}

	bool QueueRuntimeUIPackagePointer(PulseForge::Application& Application, PulseForge::InputWindowID Window, float X, float Y)
	{
		if (!Application.GetGameplayInput().QueueEvent({ .Kind = PulseForge::InputEventKind::PointerMoved,
			.Control = { .Device = PulseForge::InputDevice::MouseMotion,
				.Code = static_cast<uint16_t>(PulseForge::MouseControl::MotionX) }, .Window = Window, .X = X, .Y = Y }))
		{
			FailRuntimeUIPackageAcceptance(Application, "could not queue a synthetic mapped pointer position");
			return false;
		}
		return true;
	}

	bool QueueRuntimeUIPackagePointerButton(PulseForge::Application& Application, PulseForge::InputWindowID Window,
		PulseForge::InputEventKind Kind)
	{
		if (!Application.GetGameplayInput().QueueEvent({ .Kind = Kind,
			.Control = { .Device = PulseForge::InputDevice::MouseButton,
				.Code = static_cast<uint16_t>(PulseForge::MouseControl::Left) }, .Window = Window }))
		{
			FailRuntimeUIPackageAcceptance(Application, "could not queue a synthetic mapped pointer-button event");
			return false;
		}
		return true;
	}

	void FailRuntimeUIPackageAcceptance(PulseForge::Application& Application, std::string_view Reason)
	{
		if (m_RuntimeUIPackageAcceptanceFailed) return;
		m_RuntimeUIPackageAcceptanceFailed = true;
		PF_ERROR("PULSEFORGE_RUNTIME_UI_PACKAGE_ACCEPTANCE=FAIL; {0}", Reason);
		Application.RequestClose();
	}

	bool HasRuntimeUIPackageText(std::string_view Text)
	{
		if (!m_Session) return false;
		const auto Commands = m_Session->GetRuntimeUIDrawCommands();
		return std::any_of(Commands.begin(), Commands.end(), [Text](const PulseForge::RuntimeUIDrawCommand& Command)
			{ return Command.Text.find(Text) != std::string::npos; });
	}

	std::optional<int> ReadRuntimeUIPackageScore()
	{
		if (!m_Session) return std::nullopt;
		const auto Commands = m_Session->GetRuntimeUIDrawCommands();
		for (const PulseForge::RuntimeUIDrawCommand& Command : Commands)
		{
			constexpr std::string_view Prefix = "Score: ";
			if (!Command.Text.starts_with(Prefix)) continue;
			int Value = 0;
			const char* Begin = Command.Text.data() + Prefix.size();
			const char* End = Command.Text.data() + Command.Text.size();
			const auto Parsed = std::from_chars(Begin, End, Value);
			if (Parsed.ec == std::errc{} && Parsed.ptr == End) return Value;
		}
		return std::nullopt;
	}

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
	PulseForge::RuntimeUIInputAdapter m_RuntimeUIInputAdapter;
	std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
	bool m_LoggedPrepareFailure = false;
	bool m_LoggedRenderFailure = false;
	bool m_LoggedRuntimeFailure = false;
	bool m_LoggedSceneDraw = false;
	bool m_PlayerSmokeExitAfterRender = false;
	bool m_PlayerSmokeShutdownRequested = false;
	bool m_PlayerRuntimeSmokeLogged = false;
#if defined(PF_DEBUG)
	bool m_RuntimeUIPackageSmoke = false;
	bool m_RuntimeUIPackageSmokeShutdownRequested = false;
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
	bool m_RuntimeUIPackageAcceptance = false;
	bool m_RuntimeUIPackageMenuRendered = false;
	bool m_RuntimeUIPackageAcceptanceComplete = false;
	bool m_RuntimeUIPackageAcceptanceFailed = false;
	bool m_RuntimeUIPackageAcceptanceShutdownRequested = false;
	RuntimeUIPackageAcceptanceStage m_RuntimeUIPackageAcceptanceStage = RuntimeUIPackageAcceptanceStage::ObserveMenu;
	double m_RuntimeUIPackageAcceptanceElapsed = 0.0;
	int m_RuntimeUIPackageScoreBeforeAction = 0;
	int m_RuntimeUIPackagePausedScore = 0;
	bool m_AudioMixingPackageAcceptance = false;
	bool m_AudioMixingPackageVerify = false;
	bool m_AudioMixingPackageInitialFrameRendered = false;
	bool m_AudioMixingPackageComplete = false;
	bool m_AudioMixingPackageFailed = false;
	bool m_AudioMixingPackageShutdownRequested = false;
	AudioMixingPackageAcceptanceStage m_AudioMixingPackageStage = AudioMixingPackageAcceptanceStage::WaitForRenderedUI;
	double m_AudioMixingPackageElapsed = 0.0;
	size_t m_AudioMixingPackageActionIndex = 0;
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
