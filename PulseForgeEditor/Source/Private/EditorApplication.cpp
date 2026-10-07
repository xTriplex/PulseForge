#define GLFW_EXPOSE_NATIVE_WIN32

#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Events/Event.h"
#include "Assets/AssetOperations.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Runtime/SceneRuntime.h"
#include "Window/Window.h"

#include <GLFW/glfw3.h>
#include <nfd.h>
#include <nfd_glfw3.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	std::string PathToUtf8(const std::filesystem::path& Path)
	{
		const std::u8string UTF8Path = Path.generic_u8string();
		return { reinterpret_cast<const char*>(UTF8Path.data()), UTF8Path.size() };
	}

	std::string NormalizedExtension(const std::filesystem::path& Path)
	{
		std::string Extension = PathToUtf8(Path.extension());
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](unsigned char Character)
		{
			return Character >= 'A' && Character <= 'Z'
				? static_cast<char>(Character - 'A' + 'a')
				: static_cast<char>(Character);
		});
		return Extension;
	}

	bool HasReservedMetadataExtension(const std::filesystem::path& Path)
	{
		return NormalizedExtension(Path) == ".meta";
	}

	class DialogPath final
	{
	public:
		~DialogPath()
		{
			if (m_Path)
				NFD_FreePathU8(m_Path);
		}

		[[nodiscard]] nfdu8char_t** GetAddress() noexcept { return &m_Path; }
		[[nodiscard]] std::filesystem::path GetPath() const { return std::filesystem::u8path(m_Path); }

	private:
		nfdu8char_t* m_Path = nullptr;
	};

	struct EditorConsoleMessage
	{
		std::string Logger;
		spdlog::level::level_enum Level;
		std::string Text;
	};

	class EditorConsoleSink final : public spdlog::sinks::base_sink<std::mutex>
	{
	public:
		static constexpr size_t Capacity = 2000;
		static constexpr size_t MaxMessageBytes = 8192;

		bool CopyMessagesIfChanged(
			uint64_t PreviousRevision,
			std::vector<EditorConsoleMessage>& Destination,
			uint64_t& CurrentRevision)
		{
			std::lock_guard<std::mutex> Lock(mutex_);
			CurrentRevision = m_Revision;
			if (CurrentRevision == PreviousRevision)
				return false;

			Destination.assign(m_Messages.begin(), m_Messages.end());
			return true;
		}

		void Clear()
		{
			std::lock_guard<std::mutex> Lock(mutex_);
			m_Messages.clear();
			++m_Revision;
		}

	protected:
		void sink_it_(const spdlog::details::log_msg& Message) override
		{
			const size_t MessageSize = (std::min)(Message.payload.size(), MaxMessageBytes);
			std::string Text = MessageSize == 0
				? std::string{}
				: std::string(Message.payload.data(), MessageSize);
			if (Message.payload.size() > MaxMessageBytes)
				Text += " [truncated]";
			std::replace(Text.begin(), Text.end(), '\r', ' ');
			std::replace(Text.begin(), Text.end(), '\n', ' ');

			m_Messages.push_back({
				std::string(Message.logger_name.data(), Message.logger_name.size()),
				Message.level,
				std::move(Text) });
			if (m_Messages.size() > Capacity)
				m_Messages.pop_front();
			++m_Revision;
		}

		void flush_() override { }

	private:
		std::deque<EditorConsoleMessage> m_Messages;
		uint64_t m_Revision = 0;
	};

	class EditorLayer final : public PulseForge::Layer
	{
	public:
		EditorLayer()
			: Layer("Editor")
		{
		}

		~EditorLayer() override
		{
			StopRuntime();
			DetachConsoleSink();
			ShutdownImGui();
			ShutdownFileDialog();
		}

		void OnAttach() override
		{
			m_Context = ImGui::CreateContext();
			if (!m_Context)
				throw std::runtime_error("Could not create the editor ImGui context");

			ImGui::SetCurrentContext(m_Context);
			ImGuiIO& IO = ImGui::GetIO();
			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable |
				ImGuiConfigFlags_ViewportsEnable;
			ImGui::StyleColorsDark();

			ImGuiStyle& Style = ImGui::GetStyle();
			Style.WindowRounding = 0.0f;
			Style.Colors[ImGuiCol_WindowBg].w = 1.0f;

			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			if (!ImGui_ImplGlfw_InitForOpenGL(Window, true))
			{
				ShutdownImGui();
				throw std::runtime_error("Could not initialize the editor GLFW ImGui backend");
			}
			m_GlfwBackendActive = true;

			if (!ImGui_ImplOpenGL3_Init("#version 410"))
			{
				ShutdownImGui();
				throw std::runtime_error("Could not initialize the editor OpenGL ImGui backend");
			}
			m_OpenGLBackendActive = true;
			AttachConsoleSink();

			if (NFD_Init() == NFD_OKAY)
			{
				m_FileDialogActive = true;
				PF_INFO("Native file dialogs initialized");
			}
			else
				SetError(NfdError("Could not initialize the native file dialog"));
		}

		void OnDetach() override
		{
			StopRuntime();
			DetachConsoleSink();
			m_Scene.reset();
			m_Project.reset();
			ShutdownImGui();
			ShutdownFileDialog();
		}

		void OnUpdate(PulseForge::Timestep DeltaTime) override
		{
			if (!m_SceneRuntime || !m_RuntimeScene)
				return;

			if (auto Result = m_SceneRuntime->Advance(*m_RuntimeScene, DeltaTime); !Result)
			{
				const std::string Message = "Runtime update failed: " + Result.error().Message;
				StopRuntime();
				SetError(Message);
			}
		}

		void OnRender() override
		{
			ImGui::SetCurrentContext(m_Context);
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGuiIO& IO = ImGui::GetIO();
			const auto [Width, Height] = PulseForge::Application::Get().GetWindow().GetFramebufferSize();
			IO.DisplaySize = ImVec2(static_cast<float>(Width), static_cast<float>(Height));
			ImGui::NewFrame();

			ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
			DrawMainMenu();
			DrawWorkspacePanels();

			ImGui::Render();
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

			if (IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				GLFWwindow* MainContext = glfwGetCurrentContext();
				ImGui::UpdatePlatformWindows();
				ImGui::RenderPlatformWindowsDefault();
				glfwMakeContextCurrent(MainContext);
			}
		}

		void OnEvent(PulseForge::Event& Event) override
		{
			if (Event.GetEventType() == PulseForge::EEventType::WindowClose && m_SceneDirty)
			{
				m_PendingAction = [] { PulseForge::Application::Get().RequestClose(); };
				m_OpenUnsavedDialog = true;
				Event.bHandled = true;
				return;
			}

			ImGui::SetCurrentContext(m_Context);
			const ImGuiIO& IO = ImGui::GetIO();
			Event.bHandled |= Event.IsInCategory(PulseForge::EventCategoryMouse) && IO.WantCaptureMouse;
			Event.bHandled |= Event.IsInCategory(PulseForge::EventCategoryKeyboard) && IO.WantCaptureKeyboard;
		}

	private:
		static std::string NfdError(std::string_view Context)
		{
			const char* Error = NFD_GetError();
			return std::string(Context) + ": " + (Error ? Error : "unknown NativeFileDialog error");
		}

		[[nodiscard]] nfdwindowhandle_t GetDialogParent() const
		{
			nfdwindowhandle_t Parent{};
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			(void)NFD_GetNativeWindowFromGLFWWindow(Window, &Parent);
			return Parent;
		}

		std::optional<std::filesystem::path> ShowOpenDialog(
			const nfdu8filteritem_t* Filter,
			const std::filesystem::path& DefaultPath = {})
		{
			if (!m_FileDialogActive)
			{
				SetError("Native file dialogs are unavailable. See the Console for initialization diagnostics.");
				return std::nullopt;
			}

			const std::string DefaultPathUTF8 = DefaultPath.empty() ? std::string{} : PathToUtf8(DefaultPath);
			nfdopendialogu8args_t Arguments{};
			Arguments.filterList = Filter;
			Arguments.filterCount = Filter ? 1 : 0;
			Arguments.defaultPath = DefaultPath.empty() ? nullptr : DefaultPathUTF8.c_str();
			Arguments.parentWindow = GetDialogParent();

			DialogPath Path;
			const nfdresult_t Result = NFD_OpenDialogU8_With(Path.GetAddress(), &Arguments);
			if (Result == NFD_CANCEL)
				return std::nullopt;
			if (Result == NFD_ERROR)
			{
				SetError(NfdError("Could not open the file dialog"));
				return std::nullopt;
			}
			return Path.GetPath();
		}

		std::optional<std::filesystem::path> ShowSaveDialog(
			const nfdu8filteritem_t* Filter,
			const std::filesystem::path& DefaultPath,
			const char* DefaultName)
		{
			if (!m_FileDialogActive)
			{
				SetError("Native file dialogs are unavailable. See the Console for initialization diagnostics.");
				return std::nullopt;
			}

			const std::string DefaultPathUTF8 = DefaultPath.empty() ? std::string{} : PathToUtf8(DefaultPath);
			nfdsavedialogu8args_t Arguments{};
			Arguments.filterList = Filter;
			Arguments.filterCount = Filter ? 1 : 0;
			Arguments.defaultPath = DefaultPath.empty() ? nullptr : DefaultPathUTF8.c_str();
			Arguments.defaultName = DefaultName;
			Arguments.parentWindow = GetDialogParent();

			DialogPath Path;
			const nfdresult_t Result = NFD_SaveDialogU8_With(Path.GetAddress(), &Arguments);
			if (Result == NFD_CANCEL)
				return std::nullopt;
			if (Result == NFD_ERROR)
			{
				SetError(NfdError("Could not open the save dialog"));
				return std::nullopt;
			}
			return Path.GetPath();
		}

		void CreateProject()
		{
			static constexpr nfdu8filteritem_t Filter{ "PulseForge Project", "pfproj" };
			auto ProjectFile = ShowSaveDialog(&Filter, {}, "NewProject.pfproj");
			if (!ProjectFile)
				return;
			if (ProjectFile->extension().empty())
				*ProjectFile += ".pfproj";
			const std::filesystem::path ChosenPath = *ProjectFile;
			QueueAfterSave([this, ChosenPath] { CreateProjectAt(ChosenPath); });
		}

		void CreateProjectAt(const std::filesystem::path& ProjectFile)
		{
			const std::string ProjectName = PathToUtf8(ProjectFile.stem());
			auto Created = PulseForge::Project::Create(ProjectFile, ProjectName);
			if (!Created)
			{
				SetError("Project creation failed: " + Created.error().Message);
				return;
			}

			auto NewScene = std::make_unique<PulseForge::Scene>();
			auto MainScene = PulseForge::SceneAssetService::Create(
				Created->GetAssetRegistry(),
				Created->GetRootPath(),
				std::filesystem::path("Assets") / "Main.scene",
				*NewScene);
			if (!MainScene && !MainScene.error().CommittedAsset)
			{
				StopRuntime();
				m_Project.emplace(std::move(*Created));
				ClearScene();
				UpdateAssetList();
				SetError("Project was created, but its initial scene could not be created: " + MainScene.error().Message);
				return;
			}

			const PulseForge::AssetRecord MainSceneRecord = MainScene
				? *MainScene
				: *MainScene.error().CommittedAsset;
			StopRuntime();
			m_Project.emplace(std::move(*Created));
			m_Scene = std::move(NewScene);
			m_SceneAsset = MainSceneRecord.ID;
			m_SelectedEntity.reset();
			m_SelectedAsset = MainSceneRecord.ID;
			m_PendingDeleteAsset.reset();
			m_OpenDeleteAssetDialog = false;
			m_SceneDirty = false;
			if (!MainScene)
			{
				std::string Message = "Project and Main.scene were created, but temporary cleanup needs attention: ";
				Message += MainScene.error().Message;
				if (MainScene.error().RecoveryPath)
					Message += " Recovery data: " + PathToUtf8(*MainScene.error().RecoveryPath);
				SetWarning(std::move(Message));
			}
			else
				SetStatus("Created project " + ProjectName + " with Assets/Main.scene.");
			UpdateAssetList();

			if (auto StartScene = m_Project->SetStartScene(MainSceneRecord.ID); !StartScene)
			{
				std::string Message = "Project and scene are open, but the startup scene could not be saved: ";
				Message += StartScene.error().Message;
				if (!MainScene)
					Message += " Temporary cleanup also needs attention: " + MainScene.error().Message;
				if (!MainScene && MainScene.error().RecoveryPath)
					Message += " Recovery data: " + PathToUtf8(*MainScene.error().RecoveryPath);
				SetWarning(std::move(Message));
			}
		}

		void OpenProject()
		{
			static constexpr nfdu8filteritem_t Filter{ "PulseForge Project", "pfproj" };
			auto ProjectFile = ShowOpenDialog(&Filter);
			if (!ProjectFile)
				return;
			const std::filesystem::path ChosenPath = *ProjectFile;
			QueueAfterSave([this, ChosenPath] { OpenProjectAt(ChosenPath); });
		}

		void OpenProjectAt(const std::filesystem::path& ProjectFile)
		{
			auto Opened = PulseForge::Project::Open(ProjectFile);
			if (!Opened)
			{
				SetError("Project could not be opened: " + Opened.error().Message);
				return;
			}

			std::unique_ptr<PulseForge::Scene> LoadedScene;
			std::optional<PulseForge::AssetID> LoadedSceneAsset;
			std::string SceneLoadWarning;
			if (const auto StartScene = Opened->GetDescription().StartScene)
			{
				LoadedScene = std::make_unique<PulseForge::Scene>();
				const auto Result = PulseForge::SceneAssetService::Load(
					*StartScene,
					Opened->GetRootPath(),
					Opened->GetAssetRegistry(),
					*LoadedScene);
				if (Result)
					LoadedSceneAsset = *StartScene;
				else
				{
					SceneLoadWarning = "Project opened, but its startup scene could not be loaded: " + Result.error().Message;
					LoadedScene.reset();
				}
			}

			const std::string ProjectName = Opened->GetDescription().Name;
			StopRuntime();
			m_Project.emplace(std::move(*Opened));
			m_Scene = std::move(LoadedScene);
			m_SceneAsset = LoadedSceneAsset;
			m_SelectedEntity.reset();
			m_SelectedAsset = LoadedSceneAsset;
			m_PendingDeleteAsset.reset();
			m_OpenDeleteAssetDialog = false;
			m_SceneDirty = false;
			UpdateAssetList();
			if (SceneLoadWarning.empty())
				SetStatus("Opened project " + ProjectName + ".");
			else
				SetWarning(std::move(SceneLoadWarning));
		}

		void ImportAsset()
		{
			if (!m_Project)
			{
				SetError("Create or open a project before importing assets.");
				return;
			}

			auto SourceFile = ShowOpenDialog(nullptr, m_Project->GetRootPath());
			if (!SourceFile)
				return;
			if (HasReservedMetadataExtension(*SourceFile))
			{
				SetError("Asset sidecar metadata files cannot be imported as source assets.");
				return;
			}

			const std::string DefaultName = PathToUtf8(SourceFile->filename());
			auto DestinationFile = ShowSaveDialog(
				nullptr,
				m_Project->GetRootPath() / "Assets",
				DefaultName.c_str());
			if (!DestinationFile)
				return;
			if (DestinationFile->extension().empty())
				*DestinationFile += SourceFile->extension();
			if (HasReservedMetadataExtension(*DestinationFile))
			{
				SetError("Asset destinations cannot use the reserved .meta extension.");
				return;
			}

			const std::filesystem::path SourcePath = *SourceFile;
			const std::filesystem::path DestinationPath = *DestinationFile;
			QueueAfterSave([this, SourcePath, DestinationPath]
			{
				ImportAssetAt(SourcePath, DestinationPath);
			});
		}

		void ImportAssetAt(
			const std::filesystem::path& SourceFile,
			const std::filesystem::path& DestinationFile)
		{
			if (!m_Project)
			{
				SetError("Create or open a project before importing assets.");
				return;
			}

			const auto RelativeDestination = GetProjectRelativePath(DestinationFile, "asset import");
			if (!RelativeDestination)
				return;

			auto Imported = PulseForge::AssetOperations::ImportFile(
				m_Project->GetAssetRegistry(),
				m_Project->GetRootPath(),
				SourceFile,
				*RelativeDestination);
			if (!Imported)
			{
				std::string Message = "Asset import failed: " + Imported.error().Message;
				if (!Imported.error().Path.empty())
					Message += " Path: " + PathToUtf8(Imported.error().Path) + ".";
				if (Imported.error().RecoveryPath)
					Message += " Recovery data: " + PathToUtf8(*Imported.error().RecoveryPath) + ".";
				if (Imported.error().CommittedAsset)
					Message += " The asset was committed with UUID " + Imported.error().CommittedAsset->ID.ToString() + ".";

				const auto RegistryRefresh = m_Project->GetAssetRegistry().Rebuild(m_Project->GetRootPath());
				UpdateAssetList();
				if (!RegistryRefresh)
				{
					Message += " Registry recovery also reported:";
					for (const PulseForge::AssetRegistryIssue& Issue : RegistryRefresh.error().Issues)
						Message += " " + Issue.Message;
				}
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList();
			m_SelectedAsset = Imported->ID;
			SetStatus(
				"Imported " + PathToUtf8(Imported->ProjectRelativePath) +
				" (UUID " + Imported->ID.ToString() + ").");
		}

		void MoveAsset(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return;
			const auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			if (!Asset)
			{
				std::string Message = "The selected asset is no longer registered.";
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			const std::string DefaultName = PathToUtf8(Asset->ProjectRelativePath.filename());
			auto Destination = ShowSaveDialog(
				nullptr,
				m_Project->GetRootPath() / Asset->ProjectRelativePath.parent_path(),
				DefaultName.c_str());
			if (!Destination)
				return;
			if (Destination->extension().empty())
				*Destination += Asset->ProjectRelativePath.extension();
			if (Destination->extension() != Asset->ProjectRelativePath.extension())
			{
				SetError("Moving an asset cannot change its file extension.");
				return;
			}
			if (HasReservedMetadataExtension(*Destination))
			{
				SetError("Asset destinations cannot use the reserved .meta extension.");
				return;
			}

			const auto RelativeDestination = GetProjectRelativePath(*Destination, "asset move");
			if (!RelativeDestination)
				return;
			const std::filesystem::path Target = *RelativeDestination;
			QueueAfterSave([this, Identifier, Target] { MoveAssetAt(Identifier, Target); });
		}

		void MoveAssetAt(
			const PulseForge::AssetID& Identifier,
			const std::filesystem::path& Destination)
		{
			if (!m_Project)
				return;
			auto Moved = PulseForge::AssetOperations::Move(
				m_Project->GetAssetRegistry(),
				m_Project->GetRootPath(),
				Identifier,
				Destination);
			if (!Moved)
			{
				std::string Message = DescribeAssetOperationError("Asset move failed", Moved.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList();
			m_SelectedAsset = Moved->ID;
			SetStatus("Moved " + PathToUtf8(Moved->ProjectRelativePath) + ".");
		}

		void DuplicateAsset(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return;
			const auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			if (!Asset)
			{
				std::string Message = "The selected asset is no longer registered.";
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			const std::filesystem::path Filename = Asset->ProjectRelativePath.filename();
			const std::string DefaultName =
				PathToUtf8(Filename.stem()) + " Copy" + PathToUtf8(Filename.extension());
			auto Destination = ShowSaveDialog(
				nullptr,
				m_Project->GetRootPath() / Asset->ProjectRelativePath.parent_path(),
				DefaultName.c_str());
			if (!Destination)
				return;
			if (Destination->extension().empty())
				*Destination += Asset->ProjectRelativePath.extension();
			if (Destination->extension() != Asset->ProjectRelativePath.extension())
			{
				SetError("Duplicating an asset cannot change its file extension.");
				return;
			}
			if (HasReservedMetadataExtension(*Destination))
			{
				SetError("Asset destinations cannot use the reserved .meta extension.");
				return;
			}

			const auto RelativeDestination = GetProjectRelativePath(*Destination, "asset duplication");
			if (!RelativeDestination)
				return;
			const std::filesystem::path Target = *RelativeDestination;
			QueueAfterSave([this, Identifier, Target] { DuplicateAssetAt(Identifier, Target); });
		}

		void DuplicateAssetAt(
			const PulseForge::AssetID& Identifier,
			const std::filesystem::path& Destination)
		{
			if (!m_Project)
				return;
			auto Duplicated = PulseForge::AssetOperations::Duplicate(
				m_Project->GetAssetRegistry(),
				m_Project->GetRootPath(),
				Identifier,
				Destination);
			if (!Duplicated)
			{
				std::string Message = DescribeAssetOperationError("Asset duplication failed", Duplicated.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList();
			m_SelectedAsset = Duplicated->ID;
			SetStatus(
				"Duplicated " + PathToUtf8(Duplicated->ProjectRelativePath) +
				" (new UUID " + Duplicated->ID.ToString() + ").");
		}

		void CreatePrefabFromSelectedEntity()
		{
			if (!m_Project || !m_Scene || !m_SelectedEntity)
			{
				SetError("Open a project and select an entity before creating a prefab.");
				return;
			}

			static constexpr nfdu8filteritem_t Filter{ "PulseForge Prefab", "prefab" };
			auto Destination = ShowSaveDialog(
				&Filter,
				m_Project->GetRootPath() / "Assets",
				"NewPrefab.prefab");
			if (!Destination)
				return;
			if (Destination->extension().empty())
				*Destination += ".prefab";
			if (Destination->extension() != ".prefab")
			{
				SetError("Prefab assets must use the .prefab extension.");
				return;
			}

			const auto RelativeDestination = GetProjectRelativePath(*Destination, "prefab creation");
			if (!RelativeDestination)
				return;
			const PulseForge::UUID Root = *m_SelectedEntity;
			const std::filesystem::path Target = *RelativeDestination;
			QueueAfterSave([this, Root, Target] { CreatePrefabAt(Root, Target); });
		}

		void CreatePrefabAt(
			const PulseForge::UUID& RootIdentifier,
			const std::filesystem::path& Destination)
		{
			if (!m_Project || !m_Scene)
			{
				SetError("The project or scene closed before prefab creation could run.");
				return;
			}

			const auto Root = m_Scene->FindEntity(RootIdentifier);
			if (!Root)
			{
				SetError("The selected entity no longer exists; no prefab was created.");
				return;
			}

			auto Created = PulseForge::PrefabAssetService::Create(
				m_Project->GetAssetRegistry(),
				m_Project->GetRootPath(),
				Destination,
				*m_Scene,
				*Root);
			if (!Created)
			{
				std::string Message = "Prefab creation failed: " + Created.error().Message;
				if (Created.error().RecoveryPath)
					Message += " Recovery data: " + PathToUtf8(*Created.error().RecoveryPath) + ".";
				if (Created.error().CommittedAsset)
				{
					UpdateAssetList();
					m_SelectedAsset = Created.error().CommittedAsset->ID;
					SetWarning(
						"Prefab was created with UUID " + Created.error().CommittedAsset->ID.ToString() +
						", but temporary cleanup failed: " + Message);
					return;
				}

				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList();
			m_SelectedAsset = Created->ID;
			SetStatus(
				"Created prefab " + PathToUtf8(Created->ProjectRelativePath) +
				" (UUID " + Created->ID.ToString() + ").");
		}

		void InstantiatePrefab(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project || !m_Scene)
			{
				SetError("Open a project and scene before instantiating a prefab.");
				return;
			}
			QueueAfterSave([this, Identifier] { InstantiatePrefabAt(Identifier); });
		}

		void InstantiatePrefabAt(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project || !m_Scene)
			{
				SetError("The project or scene closed before prefab instantiation could run.");
				return;
			}

			auto Instantiated = PulseForge::PrefabAssetService::Instantiate(
				Identifier,
				m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(),
				*m_Scene);
			if (!Instantiated)
			{
				SetError("Prefab instantiation failed: " + Instantiated.error().Message);
				return;
			}

			m_SelectedEntity = Instantiated->GetUUID();
			m_SceneDirty = true;
			SetStatus("Instantiated prefab " + Identifier.ToString() + " into the current scene.");
		}

		void RequestDeleteAsset(const PulseForge::AssetID& Identifier)
		{
			if (IsProtectedSceneAsset(Identifier))
			{
				SetError("The open or configured startup scene cannot be deleted. Choose another startup scene first.");
				return;
			}
			m_PendingDeleteAsset = Identifier;
			m_OpenDeleteAssetDialog = true;
		}

		void DrawDeleteAssetDialog()
		{
			if (m_OpenDeleteAssetDialog)
			{
				ImGui::OpenPopup("Delete Managed Asset");
				m_OpenDeleteAssetDialog = false;
			}

			bool PopupOpen = true;
			if (ImGui::BeginPopupModal("Delete Managed Asset", &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
			{
				const auto Asset = m_Project && m_PendingDeleteAsset
					? m_Project->GetAssetRegistry().Find(*m_PendingDeleteAsset)
					: std::optional<PulseForge::AssetRecord>{};
				if (Asset)
					ImGui::TextWrapped("Permanently delete %s and its sidecar metadata?", PathToUtf8(Asset->ProjectRelativePath).c_str());
				else
					ImGui::TextUnformatted("The selected asset is no longer registered.");
				ImGui::TextUnformatted("This cannot be undone and may leave UUID references unresolved.");

				if (Asset && ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
				{
					const PulseForge::AssetID Identifier = Asset->ID;
					m_PendingDeleteAsset.reset();
					ImGui::CloseCurrentPopup();
					QueueAfterSave([this, Identifier] { DeleteAssetAt(Identifier); });
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
				{
					m_PendingDeleteAsset.reset();
					m_OpenDeleteAssetDialog = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			if (!PopupOpen)
			{
				m_PendingDeleteAsset.reset();
				m_OpenDeleteAssetDialog = false;
			}
		}

		void DeleteAssetAt(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return;
			if (IsProtectedSceneAsset(Identifier))
			{
				SetError("The open or configured startup scene cannot be deleted. Choose another startup scene first.");
				return;
			}

			auto Deleted = PulseForge::AssetOperations::Delete(
				m_Project->GetAssetRegistry(),
				m_Project->GetRootPath(),
				Identifier);
			if (!Deleted)
			{
				std::string Message = DescribeAssetOperationError("Asset deletion failed", Deleted.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList();
			if (m_SelectedAsset && *m_SelectedAsset == Identifier)
				m_SelectedAsset.reset();
			SetStatus("Deleted asset " + Identifier.ToString() + ".");
		}

		bool IsProtectedSceneAsset(const PulseForge::AssetID& Identifier) const
		{
			return (m_SceneAsset && *m_SceneAsset == Identifier) ||
				(m_Project && m_Project->GetDescription().StartScene &&
					*m_Project->GetDescription().StartScene == Identifier);
		}

		[[nodiscard]] std::optional<std::filesystem::path> GetProjectRelativePath(
			const std::filesystem::path& AbsolutePath,
			std::string_view Operation)
		{
			if (!m_Project)
				return std::nullopt;

			std::error_code Error;
			const std::filesystem::path Root =
				std::filesystem::absolute(m_Project->GetRootPath(), Error).lexically_normal();
			if (Error)
			{
				SetError("Could not resolve the project root for " + std::string(Operation) + ": " + Error.message());
				return std::nullopt;
			}
			Error.clear();
			const std::filesystem::path Absolute = std::filesystem::absolute(AbsolutePath, Error).lexically_normal();
			if (Error)
			{
				SetError("Could not resolve the destination for " + std::string(Operation) + ": " + Error.message());
				return std::nullopt;
			}

			const std::filesystem::path Relative = Absolute.lexically_relative(Root);
			if (Relative.empty() || Relative.is_absolute())
			{
				SetError("Could not make the destination relative to the project for " + std::string(Operation) + ".");
				return std::nullopt;
			}
			return Relative;
		}

		static std::string DescribeAssetOperationError(
			std::string_view Operation,
			const PulseForge::AssetOperationError& Error)
		{
			std::string Message = std::string(Operation) + ": " + Error.Message;
			if (!Error.Path.empty())
				Message += " Path: " + PathToUtf8(Error.Path) + ".";
			if (Error.RecoveryPath)
				Message += " Recovery data: " + PathToUtf8(*Error.RecoveryPath) + ".";
			if (Error.CommittedAsset)
				Message += " The asset was committed with UUID " + Error.CommittedAsset->ID.ToString() + ".";
			return Message;
		}

		void RefreshAssetsAfterOperationFailure(std::string& Message)
		{
			if (!m_Project)
				return;
			const auto Rebuild = m_Project->GetAssetRegistry().Rebuild(m_Project->GetRootPath());
			UpdateAssetList();
			if (!Rebuild)
			{
				Message += " Registry refresh also reported:";
				for (const PulseForge::AssetRegistryIssue& Issue : Rebuild.error().Issues)
					Message += " " + Issue.Message;
			}
		}

		void CreateScene(bool SaveAs)
		{
			if (!m_Project)
			{
				SetError("Create or open a project before creating a scene.");
				return;
			}
			if (SaveAs && !m_Scene)
			{
				SetError("There is no open scene to save under a new asset identity.");
				return;
			}

			static constexpr nfdu8filteritem_t Filter{ "PulseForge Scene", "scene" };
			auto SelectedPath = ShowSaveDialog(&Filter, m_Project->GetRootPath() / "Assets", "NewScene.scene");
			if (!SelectedPath)
				return;
			if (SelectedPath->extension().empty())
				*SelectedPath += ".scene";
			const std::filesystem::path ChosenPath = *SelectedPath;
			QueueAfterSave([this, ChosenPath, SaveAs] { CreateSceneAt(ChosenPath, SaveAs); });
		}

		void CreateSceneAt(const std::filesystem::path& SelectedPath, bool SaveAs)
		{
			if (!m_Project)
			{
				SetError("Create or open a project before creating a scene.");
				return;
			}
			if (SaveAs && !m_Scene)
			{
				SetError("There is no open scene to save under a new asset identity.");
				return;
			}

			std::error_code PathError;
			const std::filesystem::path AbsolutePath = std::filesystem::absolute(SelectedPath, PathError).lexically_normal();
			if (PathError)
			{
				SetError("Could not resolve the scene destination: " + PathError.message());
				return;
			}
			const std::filesystem::path RelativePath = AbsolutePath.lexically_relative(m_Project->GetRootPath());
			std::unique_ptr<PulseForge::Scene> NewScene;
			if (!SaveAs)
				NewScene = std::make_unique<PulseForge::Scene>();
			const PulseForge::Scene& Source = SaveAs ? *m_Scene : *NewScene;
			auto Created = PulseForge::SceneAssetService::Create(
				m_Project->GetAssetRegistry(),
				m_Project->GetRootPath(),
				RelativePath,
				Source);
			if (!Created && !Created.error().CommittedAsset)
			{
				SetError("Scene asset creation failed: " + Created.error().Message);
				return;
			}

			const PulseForge::AssetRecord Record = Created ? *Created : *Created.error().CommittedAsset;
			StopRuntime();
			if (!SaveAs)
				m_Scene = std::move(NewScene);
			m_SceneAsset = Record.ID;
			m_SelectedEntity.reset();
			m_SceneDirty = false;
			UpdateAssetList();
			if (auto StartScene = m_Project->SetStartScene(Record.ID); !StartScene)
			{
				std::string Message = "Scene was created, but the project startup scene could not be updated: ";
				Message += StartScene.error().Message;
				if (!Created)
					Message += " Temporary cleanup also needs attention: " + Created.error().Message;
				if (!Created && Created.error().RecoveryPath)
					Message += " Recovery data: " + PathToUtf8(*Created.error().RecoveryPath);
				SetWarning(std::move(Message));
			}
			else if (!Created)
			{
				std::string Message = "Scene was created, but temporary cleanup needs attention: ";
				Message += Created.error().Message;
				if (Created.error().RecoveryPath)
					Message += " Recovery data: " + PathToUtf8(*Created.error().RecoveryPath);
				SetWarning(std::move(Message));
			}
			else
				SetStatus("Created scene " + PathToUtf8(Record.ProjectRelativePath) + ".");
		}

		void OpenScene(const PulseForge::AssetID& Identifier)
		{
			QueueAfterSave([this, Identifier] { OpenSceneNow(Identifier); });
		}

		void OpenSceneNow(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return;
			auto Loaded = std::make_unique<PulseForge::Scene>();
			const auto Result = PulseForge::SceneAssetService::Load(
				Identifier,
				m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(),
				*Loaded);
			if (!Result)
			{
				SetError("Scene could not be opened: " + Result.error().Message);
				return;
			}

			StopRuntime();
			m_Scene = std::move(Loaded);
			m_SceneAsset = Identifier;
			m_SelectedAsset = Identifier;
			m_SelectedEntity.reset();
			m_SceneDirty = false;
			const auto Record = m_Project->GetAssetRegistry().Find(Identifier);
			SetStatus(Record ? "Opened scene " + PathToUtf8(Record->ProjectRelativePath) + "." : "Opened scene.");
		}

		void SetStartupScene(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return;
			if (auto Result = m_Project->SetStartScene(Identifier); !Result)
			{
				SetError("Could not set the project startup scene: " + Result.error().Message);
				return;
			}
			SetStatus("Updated the project startup scene to " + Identifier.ToString() + ".");
		}

		bool SaveScene()
		{
			if (!m_Project || !m_Scene || !m_SceneAsset)
			{
				SetError("There is no managed scene asset to save.");
				return false;
			}

			const auto Result = PulseForge::SceneAssetService::Save(
				*m_SceneAsset,
				m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(),
				*m_Scene);
			if (!Result)
			{
				SetError("Scene save failed: " + Result.error().Message);
				return false;
			}
			else
			{
				m_SceneDirty = false;
				SetStatus("Scene saved.");
			}
			return true;
		}

		void QueueAfterSave(std::function<void()> Action)
		{
			if (m_PendingAction)
			{
				SetWarning("Resolve the current unsaved-scene prompt before starting another editor operation.");
				return;
			}

			if (!m_SceneDirty)
			{
				Action();
				return;
			}

			m_PendingAction = std::move(Action);
			m_OpenUnsavedDialog = true;
		}

		bool DiscardSceneChanges()
		{
			if (!m_SceneDirty)
				return true;
			if (!m_Project || !m_Scene || !m_SceneAsset)
			{
				SetError("Cannot discard changes because the current scene has no managed source asset to reload.");
				return false;
			}

			auto ReloadedScene = std::make_unique<PulseForge::Scene>();
			const auto Result = PulseForge::SceneAssetService::Load(
				*m_SceneAsset,
				m_Project->GetRootPath(),
				m_Project->GetAssetRegistry(),
				*ReloadedScene);
			if (!Result)
			{
				SetError("Unsaved changes were preserved because the saved scene could not be reloaded: " + Result.error().Message);
				return false;
			}

			StopRuntime();
			m_Scene = std::move(ReloadedScene);
			m_SelectedEntity.reset();
			m_SceneDirty = false;
			SetStatus("Unsaved scene changes discarded.");
			return true;
		}

		void ExecutePendingAction()
		{
			auto Action = std::move(m_PendingAction);
			m_PendingAction = {};
			m_OpenUnsavedDialog = false;
			ImGui::CloseCurrentPopup();
			if (Action)
				Action();
		}

		void DrawUnsavedChangesDialog()
		{
			if (m_OpenUnsavedDialog)
			{
				ImGui::OpenPopup("Unsaved Scene Changes");
				m_OpenUnsavedDialog = false;
			}

			bool PopupOpen = true;
			if (ImGui::BeginPopupModal("Unsaved Scene Changes", &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextUnformatted("The current scene has unsaved changes.");
				ImGui::TextUnformatted("Save before continuing?");
				if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
				{
					if (SaveScene())
						ExecutePendingAction();
				}
				ImGui::SameLine();
				if (ImGui::Button("Discard", ImVec2(120.0f, 0.0f)))
				{
					if (DiscardSceneChanges())
						ExecutePendingAction();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
				{
					m_PendingAction = {};
					m_OpenUnsavedDialog = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			if (!PopupOpen)
			{
				m_PendingAction = {};
				m_OpenUnsavedDialog = false;
			}
		}

		void CloseProjectNow()
		{
			ClearScene();
			m_Project.reset();
			m_Assets.clear();
			SetStatus("Project closed.");
		}

		void DrawMainMenu()
		{
			if (!ImGui::BeginMainMenuBar())
				return;

			ImGui::TextUnformatted("PulseForge");
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New Project..."))
					CreateProject();
				if (ImGui::MenuItem("Open Project..."))
					OpenProject();
				if (m_Project && ImGui::MenuItem("Close Project"))
					QueueAfterSave([this] { CloseProjectNow(); });
				ImGui::Separator();
				if (ImGui::MenuItem("Exit"))
					QueueAfterSave([] { PulseForge::Application::Get().RequestClose(); });
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Scene", m_Project.has_value()))
			{
				if (ImGui::MenuItem("New Scene..."))
					CreateScene(false);
				if (ImGui::MenuItem("Save Scene", nullptr, false, m_Scene && m_SceneAsset))
					SaveScene();
				if (ImGui::MenuItem("Save Scene As...", nullptr, false, m_Scene != nullptr))
					CreateScene(true);
				if (ImGui::BeginMenu("Open Scene", m_Project.has_value()))
				{
					bool HasScenes = false;
					for (const PulseForge::AssetRecord& Asset : m_Assets)
					{
						if (Asset.ProjectRelativePath.extension() != ".scene")
							continue;
						HasScenes = true;
						const bool IsOpen = m_SceneAsset && *m_SceneAsset == Asset.ID;
						if (ImGui::MenuItem(PathToUtf8(Asset.ProjectRelativePath.filename()).c_str(), nullptr, IsOpen))
							OpenScene(Asset.ID);
					}
					if (!HasScenes)
						ImGui::MenuItem("No scene assets", nullptr, false, false);
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}

			ImGui::SameLine();
			if (!m_SceneRuntime)
			{
				ImGui::BeginDisabled(!m_Project || !m_Scene);
				if (ImGui::Button("Play"))
					StartRuntime();
				ImGui::EndDisabled();
			}
			else if (ImGui::Button("Stop"))
			{
				StopRuntime();
				SetStatus("Runtime stopped. The authored scene was not modified by simulation.");
			}

			ImGui::EndMainMenuBar();
		}

		void DrawWorkspacePanels()
		{
			if (ImGui::Begin("Scene"))
			{
				if (m_Project)
				{
					ImGui::Text("Project: %s", m_Project->GetDescription().Name.c_str());
					if (m_Scene && m_SceneAsset)
					{
						const auto Record = m_Project->GetAssetRegistry().Find(*m_SceneAsset);
						const std::string SceneName = Record
							? PathToUtf8(Record->ProjectRelativePath)
							: std::string("<unregistered>");
						ImGui::Text("Scene: %s%s", SceneName.c_str(), m_SceneDirty ? " *" : "");
						ImGui::Text("Entities: %zu", m_Scene->GetEntityCount());
						if (m_SceneRuntime && m_RuntimeScene)
						{
							ImGui::Separator();
							ImGui::Text("Runtime simulation: playing (%zu entities)", m_RuntimeScene->GetEntityCount());
							ImGui::TextWrapped("Running on an isolated scene copy. The editor viewport is not connected yet.");
						}
					}
					else
						ImGui::TextUnformatted("No scene is open. Create one from the Scene menu or open a scene asset.");
				}
				else
					ImGui::TextUnformatted("Create or open a project from the File menu.");

				ImGui::Separator();
				const ImVec4 Color = m_StatusIsError
					? ImVec4(1.0f, 0.38f, 0.32f, 1.0f)
					: m_StatusIsWarning ? ImVec4(1.0f, 0.75f, 0.28f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
				ImGui::PushStyleColor(ImGuiCol_Text, Color);
				ImGui::TextWrapped("%s", m_StatusMessage.c_str());
				ImGui::PopStyleColor();
			}
			DrawUnsavedChangesDialog();
			ImGui::End();

			DrawHierarchyPanel();
			DrawInspectorPanel();
			DrawContentBrowserPanel();

			DrawConsolePanel();
		}

		void AttachConsoleSink()
		{
			m_ConsoleSink = std::make_shared<EditorConsoleSink>();
			for (const char* LoggerName : { "PULSEFORGE", "APP" })
			{
				if (auto Logger = spdlog::get(LoggerName))
				{
					Logger->sinks().push_back(m_ConsoleSink);
					m_ConsoleLoggers.push_back(std::move(Logger));
				}
			}
			if (m_ConsoleLoggers.empty())
				m_ConsoleSink.reset();
		}

		void DetachConsoleSink() noexcept
		{
			if (!m_ConsoleSink)
				return;

			const std::shared_ptr<spdlog::sinks::sink> Sink = m_ConsoleSink;
			for (const std::shared_ptr<spdlog::logger>& Logger : m_ConsoleLoggers)
				std::erase(Logger->sinks(), Sink);
			m_ConsoleLoggers.clear();
			m_ConsoleSink.reset();
		}

		static const char* LogLevelName(spdlog::level::level_enum Level)
		{
			switch (Level)
			{
			case spdlog::level::trace: return "trace";
			case spdlog::level::debug: return "debug";
			case spdlog::level::info: return "info";
			case spdlog::level::warn: return "warning";
			case spdlog::level::err: return "error";
			case spdlog::level::critical: return "critical";
			default: return "unknown";
			}
		}

		static ImVec4 LogLevelColor(spdlog::level::level_enum Level)
		{
			switch (Level)
			{
			case spdlog::level::warn: return { 1.0f, 0.75f, 0.28f, 1.0f };
			case spdlog::level::err:
			case spdlog::level::critical: return { 1.0f, 0.38f, 0.32f, 1.0f };
			default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
			}
		}

		void DrawConsolePanel()
		{
			if (ImGui::Begin("Console"))
			{
				if (!m_ConsoleSink)
					ImGui::TextUnformatted("The editor could not connect to the engine loggers.");
				else
				{
					if (ImGui::Button("Clear"))
						m_ConsoleSink->Clear();
					ImGui::SameLine();
					ImGui::Checkbox("Auto-scroll", &m_ConsoleAutoScroll);

					ImGui::Separator();
					if (ImGui::BeginChild("ConsoleMessages", ImVec2(0.0f, 0.0f), true,
						ImGuiWindowFlags_HorizontalScrollbar))
					{
						const bool WasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
						uint64_t CurrentRevision = m_ConsoleRevision;
						if (m_ConsoleSink->CopyMessagesIfChanged(
							m_ConsoleRevision,
							m_ConsoleMessages,
							CurrentRevision))
						{
							m_ConsoleRevision = CurrentRevision;
						}

						ImGuiListClipper Clipper;
						Clipper.Begin(static_cast<int>(m_ConsoleMessages.size()));
						while (Clipper.Step())
						{
							for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
							{
								const EditorConsoleMessage& Message = m_ConsoleMessages[static_cast<size_t>(Index)];
								ImGui::TextColored(LogLevelColor(Message.Level), "[%s] %s:",
									LogLevelName(Message.Level), Message.Logger.c_str());
								ImGui::SameLine();
								ImGui::TextUnformatted(Message.Text.data(), Message.Text.data() + Message.Text.size());
							}
						}
						if (m_ConsoleAutoScroll && WasAtBottom)
							ImGui::SetScrollHereY(1.0f);
					}
					ImGui::EndChild();
				}
			}
			ImGui::End();
		}

		void DrawHierarchyPanel()
		{
			if (ImGui::Begin("Hierarchy"))
			{
				ImGui::BeginDisabled(!m_Scene);
				if (ImGui::Button("Create Entity"))
					CreateEntityFromEditor();
				ImGui::SameLine();
				ImGui::BeginDisabled(!m_SelectedEntity);
				if (ImGui::Button("Duplicate"))
					DuplicateSelectedEntity();
				ImGui::SameLine();
				if (ImGui::Button("Delete"))
					DeleteSelectedEntity();
				ImGui::EndDisabled();
				ImGui::BeginDisabled(!m_SelectedEntity);
				if (ImGui::Button("Create Prefab..."))
					CreatePrefabFromSelectedEntity();
				ImGui::EndDisabled();
				ImGui::EndDisabled();
				ImGui::Separator();

				if (!m_Scene)
					ImGui::TextUnformatted("No scene is open.");
				else if (m_Scene->GetEntityCount() == 0)
					ImGui::TextUnformatted("This scene has no entities.");
				else
				{
					std::function<void()> HierarchyAction;
					for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
					{
						const auto Parent = Entity.GetParent();
						if (Parent && !Parent->has_value())
							DrawEntityTree(Entity, HierarchyAction);
					}
					if (HierarchyAction)
						HierarchyAction();
				}
			}
			ImGui::End();
		}

		void CreateEntityFromEditor()
		{
			if (!m_Scene)
				return;
			auto Created = m_Scene->CreateEntity();
			if (!Created)
			{
				SetError("Entity creation failed: " + Created.error().Message);
				return;
			}
			m_SelectedEntity = Created->GetUUID();
			m_SceneDirty = true;
		}

		void DuplicateSelectedEntity()
		{
			if (!m_Scene || !m_SelectedEntity)
				return;
			const auto Source = m_Scene->FindEntity(*m_SelectedEntity);
			if (!Source)
			{
				m_SelectedEntity.reset();
				return;
			}
			auto Duplicated = m_Scene->DuplicateEntity(*Source);
			if (!Duplicated)
			{
				SetError("Entity duplication failed: " + Duplicated.error().Message);
				return;
			}
			m_SelectedEntity = Duplicated->GetUUID();
			m_SceneDirty = true;
		}

		void DeleteSelectedEntity()
		{
			if (!m_Scene || !m_SelectedEntity)
				return;
			const auto Target = m_Scene->FindEntity(*m_SelectedEntity);
			if (!Target)
			{
				m_SelectedEntity.reset();
				return;
			}
			if (auto Deleted = m_Scene->DestroyEntity(*Target); !Deleted)
			{
				SetError("Entity deletion failed: " + Deleted.error().Message);
				return;
			}
			m_SelectedEntity.reset();
			m_SceneDirty = true;
		}

		void ReparentEntity(
			const PulseForge::UUID& ChildIdentifier,
			std::optional<PulseForge::UUID> ParentIdentifier)
		{
			if (!m_Scene)
				return;
			const auto Child = m_Scene->FindEntity(ChildIdentifier);
			if (!Child)
			{
				SetError("The hierarchy entity no longer exists.");
				return;
			}

			std::expected<void, PulseForge::SceneError> Result;
			if (ParentIdentifier)
			{
				const auto Parent = m_Scene->FindEntity(*ParentIdentifier);
				if (!Parent)
				{
					SetError("The selected parent entity no longer exists.");
					return;
				}
				Result = Child->SetParent(*Parent);
			}
			else
				Result = Child->ClearParent();

			if (!Result)
			{
				SetError("Entity reparenting failed: " + Result.error().Message);
				return;
			}
			m_SceneDirty = true;
		}

		void DrawEntityTree(const PulseForge::Entity& Entity, std::function<void()>& DeferredHierarchyAction)
		{
			const auto Tag = Entity.GetTag();
			if (!Tag)
				return;

			const auto Parent = Entity.GetParent();
			if (!Parent)
			{
				SetError("Could not inspect an entity's hierarchy: " + Parent.error().Message);
				return;
			}
			const auto Children = Entity.GetChildren();
			const std::string Identifier = Entity.GetUUID().ToString();
			ImGui::PushID(Identifier.c_str());
			ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (m_SelectedEntity && *m_SelectedEntity == Entity.GetUUID())
				Flags |= ImGuiTreeNodeFlags_Selected;
			if (!Children || Children->empty())
				Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

			const bool IsOpen = ImGui::TreeNodeEx("Entity", Flags, "%s", Tag->Name.c_str());
			if (ImGui::IsItemClicked())
				m_SelectedEntity = Entity.GetUUID();
			if (ImGui::BeginPopupContextItem("Entity Actions"))
			{
				const PulseForge::UUID ChildIdentifier = Entity.GetUUID();
				const bool HasParent = Parent && Parent->has_value();
				if (ImGui::MenuItem("Make Root", nullptr, false, HasParent))
					DeferredHierarchyAction = [this, ChildIdentifier] { ReparentEntity(ChildIdentifier, std::nullopt); };

				const std::optional<PulseForge::UUID> ParentIdentifier = m_SelectedEntity;
				const bool CanParentUnderSelection = ParentIdentifier && *ParentIdentifier != ChildIdentifier;
				if (ImGui::MenuItem("Parent Under Selected", nullptr, false, CanParentUnderSelection))
				{
					DeferredHierarchyAction = [this, ChildIdentifier, ParentIdentifier]
					{
						ReparentEntity(ChildIdentifier, ParentIdentifier);
					};
				}
				ImGui::EndPopup();
			}
			if (IsOpen && Children && !Children->empty())
			{
				for (const PulseForge::Entity& Child : *Children)
					DrawEntityTree(Child, DeferredHierarchyAction);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		void DrawInspectorPanel()
		{
			if (ImGui::Begin("Inspector"))
			{
				if (!m_Scene || !m_SelectedEntity)
					ImGui::TextUnformatted("Select an entity in the Hierarchy.");
				else if (const auto Entity = m_Scene->FindEntity(*m_SelectedEntity))
				{
					auto Tag = Entity->GetTag();
					if (Tag && ImGui::InputText("Name", &Tag->Name))
					{
						if (auto Updated = Entity->SetTag(*Tag); !Updated)
							SetError("Entity name update failed: " + Updated.error().Message);
						else
							m_SceneDirty = true;
					}
					ImGui::Text("UUID: %s", Entity->GetUUID().ToString().c_str());

					if (const auto Transform = Entity->GetTransform())
					{
						ImGui::SeparatorText("Transform");
						float Translation[3]{ Transform->Translation.x, Transform->Translation.y, Transform->Translation.z };
						glm::vec3 RotationDegrees = glm::degrees(glm::eulerAngles(Transform->Rotation));
						float Rotation[3]{ RotationDegrees.x, RotationDegrees.y, RotationDegrees.z };
						float Scale[3]{ Transform->Scale.x, Transform->Scale.y, Transform->Scale.z };
						const bool TranslationChanged = ImGui::DragFloat3("Translation", Translation, 0.05f);
						const bool RotationChanged = ImGui::DragFloat3("Rotation (degrees)", Rotation, 0.5f);
						const bool ScaleChanged = ImGui::DragFloat3("Scale", Scale, 0.05f);
						if (TranslationChanged || RotationChanged || ScaleChanged)
						{
							PulseForge::TransformComponent UpdatedTransform = *Transform;
							UpdatedTransform.Translation = { Translation[0], Translation[1], Translation[2] };
							UpdatedTransform.Rotation = glm::quat(glm::radians(glm::vec3(Rotation[0], Rotation[1], Rotation[2])));
							UpdatedTransform.Scale = { Scale[0], Scale[1], Scale[2] };
							if (auto Updated = Entity->SetTransform(UpdatedTransform); !Updated)
								SetError("Entity transform update failed: " + Updated.error().Message);
							else
								m_SceneDirty = true;
						}
					}
					ImGui::Separator();
					DrawComponentAuthoring(*Entity);
				}
				else
				{
					m_SelectedEntity.reset();
					ImGui::TextUnformatted("The selected entity no longer exists.");
				}
			}
			ImGui::End();
		}

		template<typename Result>
		void RecordComponentOperation(Result&& Operation, std::string_view Context)
		{
			if (!Operation)
			{
				SetError(std::string(Context) + ": " + Operation.error().Message);
				return;
			}
			m_SceneDirty = true;
		}

		void SetCameraComponent(const PulseForge::Entity& Target, const PulseForge::CameraComponent& Camera)
		{
			if (auto Validation = Camera.Validate(); !Validation)
			{
				SetError("Camera update failed: " + Validation.error().Message);
				return;
			}
			if (Camera.IsPrimary && m_Scene)
			{
				for (const PulseForge::Entity& Other : m_Scene->GetEntities())
				{
					if (Other.GetUUID() == Target.GetUUID())
						continue;
					auto Existing = Other.GetCamera();
					if (!Existing)
					{
						SetError("Could not inspect camera components while selecting the primary camera: " + Existing.error().Message);
						return;
					}
					if (!Existing->has_value() || !Existing->value().IsPrimary)
						continue;
					PulseForge::CameraComponent Updated = Existing->value();
					Updated.IsPrimary = false;
					if (auto Result = Other.SetCamera(Updated); !Result)
					{
						SetError("Could not clear the previous primary camera: " + Result.error().Message);
						return;
					}
					m_SceneDirty = true;
				}
			}
			RecordComponentOperation(Target.SetCamera(Camera), "Camera update failed");
		}

		void SetAudioListenerComponent(
			const PulseForge::Entity& Target,
			const PulseForge::AudioListenerComponent& Listener)
		{
			if (Listener.IsPrimary && m_Scene)
			{
				for (const PulseForge::Entity& Other : m_Scene->GetEntities())
				{
					if (Other.GetUUID() == Target.GetUUID())
						continue;
					auto Existing = Other.GetAudioListener();
					if (!Existing)
					{
						SetError("Could not inspect audio listener components while selecting the primary listener: " + Existing.error().Message);
						return;
					}
					if (!Existing->has_value() || !Existing->value().IsPrimary)
						continue;
					PulseForge::AudioListenerComponent Updated = Existing->value();
					Updated.IsPrimary = false;
					if (auto Result = Other.SetAudioListener(Updated); !Result)
					{
						SetError("Could not clear the previous primary audio listener: " + Result.error().Message);
						return;
					}
					m_SceneDirty = true;
				}
			}
			RecordComponentOperation(Target.SetAudioListener(Listener), "Audio listener update failed");
		}

		bool IsAssetExtension(
			const PulseForge::AssetRecord& Asset,
			std::initializer_list<std::string_view> Extensions) const
		{
			const std::string Extension = Asset.ProjectRelativePath.extension().string();
			return std::any_of(Extensions.begin(), Extensions.end(), [&](std::string_view Candidate)
			{
				return Extension == Candidate;
			});
		}

		bool DrawAssetSelector(
			const char* Label,
			std::optional<PulseForge::AssetID>& Current,
			std::initializer_list<std::string_view> Extensions,
			bool AllowNone)
		{
			std::string Preview = "<None>";
			if (Current)
			{
				const auto Record = m_Project ? m_Project->GetAssetRegistry().Find(*Current) : std::nullopt;
				Preview = Record ? PathToUtf8(Record->ProjectRelativePath) : "<Missing asset: " + Current->ToString() + ">";
			}

			bool Changed = false;
			if (!ImGui::BeginCombo(Label, Preview.c_str()))
				return false;

			if (AllowNone)
			{
				const bool IsSelected = !Current;
				if (ImGui::Selectable("None", IsSelected))
				{
					Current.reset();
					Changed = true;
				}
				if (IsSelected)
					ImGui::SetItemDefaultFocus();
			}

			bool HasCandidates = false;
			if (m_Project)
			{
				for (const PulseForge::AssetRecord& Asset : m_Assets)
				{
					if (!IsAssetExtension(Asset, Extensions))
						continue;
					HasCandidates = true;
					const std::string Identifier = Asset.ID.ToString();
					const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
					const bool IsSelected = Current && *Current == Asset.ID;
					ImGui::PushID(Identifier.c_str());
					if (ImGui::Selectable(Path.c_str(), IsSelected))
					{
						Current = Asset.ID;
						Changed = true;
					}
					if (IsSelected)
						ImGui::SetItemDefaultFocus();
					ImGui::PopID();
				}
			}
			if (!HasCandidates)
				ImGui::TextDisabled("No compatible managed assets are available.");

			ImGui::EndCombo();
			return Changed;
		}

		void DrawComponentAuthoring(const PulseForge::Entity& Entity)
		{
			if (ImGui::Button("Add Component"))
				ImGui::OpenPopup("Add Component");
			if (ImGui::BeginPopup("Add Component"))
			{
				const auto Camera = Entity.GetCamera();
				const auto Mesh = Entity.GetMeshRenderer();
				const auto Rigidbody = Entity.GetRigidbody();
				const auto Collider = Entity.GetBoxCollider();
				const auto AudioSource = Entity.GetAudioSource();
				const auto AudioListener = Entity.GetAudioListener();
				const auto Script = Entity.GetScript();
				if (!Camera || !Mesh || !Rigidbody || !Collider || !AudioSource || !AudioListener || !Script)
					SetError("Could not inspect entity components.");

				if (Camera && !Camera->has_value() && ImGui::MenuItem("Camera"))
				{
					PulseForge::CameraComponent Component;
					Component.IsPrimary = true;
					SetCameraComponent(Entity, Component);
				}
				if (Rigidbody && !Rigidbody->has_value() && ImGui::MenuItem("Rigidbody"))
					RecordComponentOperation(Entity.SetRigidbody(PulseForge::RigidbodyComponent{}), "Rigidbody component add failed");
				if (Collider && !Collider->has_value() && ImGui::MenuItem("Box Collider"))
					RecordComponentOperation(Entity.SetBoxCollider(PulseForge::BoxColliderComponent{}), "Box collider add failed");
				if (AudioListener && !AudioListener->has_value() && ImGui::MenuItem("Audio Listener"))
					SetAudioListenerComponent(Entity, PulseForge::AudioListenerComponent{});

				if (Mesh && !Mesh->has_value() && ImGui::BeginMenu("Mesh Renderer"))
				{
					bool HasMeshAssets = false;
					for (const PulseForge::AssetRecord& Asset : m_Assets)
					{
						if (!IsAssetExtension(Asset, { ".gltf", ".glb" }))
							continue;
						HasMeshAssets = true;
						const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
						if (ImGui::MenuItem(Path.c_str()))
						{
							PulseForge::MeshRendererComponent Component;
							Component.MeshAsset = Asset.ID;
							RecordComponentOperation(Entity.SetMeshRenderer(Component), "Mesh renderer add failed");
							break;
						}
					}
					if (!HasMeshAssets)
						ImGui::MenuItem("Import a glTF asset first", nullptr, false, false);
					ImGui::EndMenu();
				}

				if (AudioSource && !AudioSource->has_value() && ImGui::BeginMenu("Audio Source"))
				{
					bool HasAudioAssets = false;
					for (const PulseForge::AssetRecord& Asset : m_Assets)
					{
						if (!IsAssetExtension(Asset, { ".wav", ".mp3", ".flac", ".ogg" }))
							continue;
						HasAudioAssets = true;
						const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
						if (ImGui::MenuItem(Path.c_str()))
						{
							PulseForge::AudioSourceComponent Component;
							Component.AudioAsset = Asset.ID;
							RecordComponentOperation(Entity.SetAudioSource(Component), "Audio source add failed");
							break;
						}
					}
					if (!HasAudioAssets)
						ImGui::MenuItem("Import an audio asset first", nullptr, false, false);
					ImGui::EndMenu();
				}

				if (Script && !Script->has_value() && ImGui::BeginMenu("Script"))
				{
					bool HasScripts = false;
					for (const PulseForge::AssetRecord& Asset : m_Assets)
					{
						if (!IsAssetExtension(Asset, { ".lua" }))
							continue;
						HasScripts = true;
						const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
						if (ImGui::MenuItem(Path.c_str()))
						{
							PulseForge::ScriptComponent Component;
							Component.ScriptAsset = Asset.ID;
							RecordComponentOperation(Entity.SetScript(Component), "Script component add failed");
							break;
						}
					}
					if (!HasScripts)
						ImGui::MenuItem("Import a Lua script first", nullptr, false, false);
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}

			DrawCameraComponent(Entity);
			DrawMeshRendererComponent(Entity);
			DrawRigidbodyComponent(Entity);
			DrawBoxColliderComponent(Entity);
			DrawAudioSourceComponent(Entity);
			DrawAudioListenerComponent(Entity);
			DrawScriptComponent(Entity);
		}

		void DrawCameraComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetCamera();
			if (!Result || !Result->has_value() || !ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
				return;

			PulseForge::CameraComponent Component = **Result;
			if (ImGui::SmallButton("Remove Camera"))
			{
				RecordComponentOperation(Entity.RemoveCamera(), "Camera component removal failed");
				return;
			}
			float FieldOfViewDegrees = glm::degrees(Component.VerticalFieldOfViewRadians);
			const bool FieldOfViewChanged = ImGui::DragFloat("Vertical FOV (degrees)", &FieldOfViewDegrees, 0.1f, 1.0f, 179.0f);
			const float MaxNearClip = (std::max)(0.002f, Component.FarClipPlane - 0.001f);
			const bool NearChanged = ImGui::DragFloat("Near Clip", &Component.NearClipPlane, 0.01f, 0.001f, MaxNearClip);
			const float MinFarClip = Component.NearClipPlane + 0.001f;
			const bool FarChanged = ImGui::DragFloat("Far Clip", &Component.FarClipPlane, 1.0f, MinFarClip, 1000000.0f);
			const bool PrimaryChanged = ImGui::Checkbox("Primary Camera", &Component.IsPrimary);
			if (FieldOfViewChanged || NearChanged || FarChanged || PrimaryChanged)
			{
				Component.VerticalFieldOfViewRadians = glm::radians(FieldOfViewDegrees);
				SetCameraComponent(Entity, Component);
			}
		}

		void DrawMeshRendererComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetMeshRenderer();
			if (!Result || !Result->has_value() || !ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen))
				return;

			PulseForge::MeshRendererComponent Component = **Result;
			if (ImGui::SmallButton("Remove Mesh Renderer"))
			{
				RecordComponentOperation(Entity.RemoveMeshRenderer(), "Mesh renderer removal failed");
				return;
			}
			std::optional<PulseForge::AssetID> Mesh = Component.MeshAsset;
			bool Changed = DrawAssetSelector("Mesh Asset", Mesh, { ".gltf", ".glb" }, false);
			std::optional<PulseForge::AssetID> Material = Component.MaterialAsset;
			Changed |= DrawAssetSelector("Material Asset", Material, { ".material" }, true);
			if (Changed && Mesh)
			{
				Component.MeshAsset = *Mesh;
				Component.MaterialAsset = Material;
				RecordComponentOperation(Entity.SetMeshRenderer(Component), "Mesh renderer update failed");
			}
		}

		void DrawRigidbodyComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetRigidbody();
			if (!Result || !Result->has_value() || !ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen))
				return;

			PulseForge::RigidbodyComponent Component = **Result;
			if (ImGui::SmallButton("Remove Rigidbody"))
			{
				RecordComponentOperation(Entity.RemoveRigidbody(), "Rigidbody removal failed");
				return;
			}
			int MotionType = Component.MotionType == PulseForge::RigidbodyMotionType::Static ? 0 : 1;
			const char* MotionTypes[]{ "Static", "Dynamic" };
			bool Changed = ImGui::Combo("Motion Type", &MotionType, MotionTypes, 2);
			if (Changed)
				Component.MotionType = MotionType == 0
					? PulseForge::RigidbodyMotionType::Static
					: PulseForge::RigidbodyMotionType::Dynamic;
			Changed |= ImGui::DragFloat("Mass", &Component.Mass, 0.05f, 0.001f, 1000000.0f);
			Changed |= ImGui::DragFloat("Friction", &Component.Friction, 0.01f, 0.0f, 1.0f);
			Changed |= ImGui::DragFloat("Restitution", &Component.Restitution, 0.01f, 0.0f, 1.0f);
			Changed |= ImGui::Checkbox("Allow Sleeping", &Component.AllowSleeping);
			if (Changed)
				RecordComponentOperation(Entity.SetRigidbody(Component), "Rigidbody update failed");
		}

		void DrawBoxColliderComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetBoxCollider();
			if (!Result || !Result->has_value() || !ImGui::CollapsingHeader("Box Collider", ImGuiTreeNodeFlags_DefaultOpen))
				return;

			PulseForge::BoxColliderComponent Component = **Result;
			if (ImGui::SmallButton("Remove Box Collider"))
			{
				RecordComponentOperation(Entity.RemoveBoxCollider(), "Box collider removal failed");
				return;
			}
			float HalfExtents[3]{ Component.HalfExtents.x, Component.HalfExtents.y, Component.HalfExtents.z };
			const bool Changed = ImGui::DragFloat3("Half Extents", HalfExtents, 0.01f, 0.001f, 1000000.0f);
			if (Changed)
			{
				Component.HalfExtents = { HalfExtents[0], HalfExtents[1], HalfExtents[2] };
				RecordComponentOperation(Entity.SetBoxCollider(Component), "Box collider update failed");
			}
		}

		void DrawAudioSourceComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetAudioSource();
			if (!Result || !Result->has_value() || !ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen))
				return;

			PulseForge::AudioSourceComponent Component = **Result;
			if (ImGui::SmallButton("Remove Audio Source"))
			{
				RecordComponentOperation(Entity.RemoveAudioSource(), "Audio source removal failed");
				return;
			}
			std::optional<PulseForge::AssetID> Audio = Component.AudioAsset;
			bool Changed = DrawAssetSelector("Audio Asset", Audio, { ".wav", ".mp3", ".flac", ".ogg" }, false);
			Changed |= ImGui::DragFloat("Volume", &Component.Volume, 0.01f, 0.0f, 1.0f);
			Changed |= ImGui::Checkbox("Looping", &Component.Looping);
			Changed |= ImGui::Checkbox("Play On Start", &Component.PlayOnStart);
			Changed |= ImGui::Checkbox("Spatialized", &Component.Spatialized);
			if (Changed && Audio)
			{
				Component.AudioAsset = *Audio;
				RecordComponentOperation(Entity.SetAudioSource(Component), "Audio source update failed");
			}
		}

		void DrawAudioListenerComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetAudioListener();
			if (!Result || !Result->has_value() || !ImGui::CollapsingHeader("Audio Listener", ImGuiTreeNodeFlags_DefaultOpen))
				return;

			PulseForge::AudioListenerComponent Component = **Result;
			if (ImGui::SmallButton("Remove Audio Listener"))
			{
				RecordComponentOperation(Entity.RemoveAudioListener(), "Audio listener removal failed");
				return;
			}
			if (ImGui::Checkbox("Primary Audio Listener", &Component.IsPrimary))
				SetAudioListenerComponent(Entity, Component);
		}

		void DrawScriptComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetScript();
			if (!Result || !Result->has_value() || !ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen))
				return;

			PulseForge::ScriptComponent Component = **Result;
			if (ImGui::SmallButton("Remove Script"))
			{
				RecordComponentOperation(Entity.RemoveScript(), "Script component removal failed");
				return;
			}
			std::optional<PulseForge::AssetID> Script = Component.ScriptAsset;
			bool Changed = DrawAssetSelector("Lua Script", Script, { ".lua" }, false);
			Changed |= ImGui::Checkbox("Enabled", &Component.Enabled);
			if (Changed && Script)
			{
				Component.ScriptAsset = *Script;
				RecordComponentOperation(Entity.SetScript(Component), "Script component update failed");
			}
		}

		void DrawContentBrowserPanel()
		{
			if (ImGui::Begin("Content Browser"))
			{
				if (!m_Project)
					ImGui::TextUnformatted("Open a project to browse managed assets.");
				else
				{
					if (ImGui::Button("Import Asset..."))
						ImportAsset();
					ImGui::SameLine();
					if (ImGui::Button("Refresh"))
						RefreshAssets();
					if (m_Assets.empty())
						ImGui::TextUnformatted("The project has no managed assets.");
					std::function<void()> ContextAction;
					for (const PulseForge::AssetRecord& Asset : m_Assets)
					{
						const std::string AssetID = Asset.ID.ToString();
						ImGui::PushID(AssetID.c_str());
						const bool IsSelected = m_SelectedAsset && *m_SelectedAsset == Asset.ID;
						const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
						if (ImGui::Selectable(Path.c_str(), IsSelected, ImGuiSelectableFlags_AllowDoubleClick))
						{
							m_SelectedAsset = Asset.ID;
							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
								Asset.ProjectRelativePath.extension() == ".scene")
								OpenScene(Asset.ID);
						}
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("UUID: %s", AssetID.c_str());
						if (ImGui::BeginPopupContextItem("Asset Actions"))
						{
							m_SelectedAsset = Asset.ID;
							const PulseForge::AssetID Identifier = Asset.ID;
							if (ImGui::MenuItem("Move / Rename..."))
								ContextAction = [this, Identifier] { MoveAsset(Identifier); };
							if (ImGui::MenuItem("Duplicate..."))
								ContextAction = [this, Identifier] { DuplicateAsset(Identifier); };
							if (Asset.ProjectRelativePath.extension() == ".prefab" &&
								ImGui::MenuItem("Instantiate in Scene", nullptr, false, m_Scene != nullptr))
								ContextAction = [this, Identifier] { InstantiatePrefab(Identifier); };
							const bool IsStartupScene = m_Project->GetDescription().StartScene &&
								*m_Project->GetDescription().StartScene == Identifier;
							if (Asset.ProjectRelativePath.extension() == ".scene" &&
								ImGui::MenuItem("Set as Startup Scene", nullptr, IsStartupScene, !IsStartupScene))
								ContextAction = [this, Identifier] { SetStartupScene(Identifier); };
							if (ImGui::MenuItem("Delete..."))
								ContextAction = [this, Identifier] { RequestDeleteAsset(Identifier); };
							ImGui::EndPopup();
						}
						ImGui::PopID();
					}
					if (ContextAction)
						ContextAction();

					if (m_SelectedAsset)
					{
						const auto Asset = m_Project->GetAssetRegistry().Find(*m_SelectedAsset);
						if (Asset)
						{
							ImGui::Separator();
							ImGui::TextWrapped("%s", PathToUtf8(Asset->ProjectRelativePath).c_str());
							ImGui::Text("UUID: %s", Asset->ID.ToString().c_str());
							if (Asset->ProjectRelativePath.extension() == ".scene")
							{
								const bool IsStartupScene = m_Project->GetDescription().StartScene &&
									*m_Project->GetDescription().StartScene == Asset->ID;
								if (IsStartupScene)
									ImGui::TextUnformatted("Project startup scene.");
								else if (ImGui::Button("Set as Startup Scene"))
									SetStartupScene(Asset->ID);
							}
							if (ImGui::Button("Move / Rename..."))
								MoveAsset(Asset->ID);
							ImGui::SameLine();
							if (ImGui::Button("Duplicate..."))
								DuplicateAsset(Asset->ID);
							ImGui::SameLine();
							const bool IsProtectedScene = IsProtectedSceneAsset(Asset->ID);
							ImGui::BeginDisabled(IsProtectedScene);
							if (ImGui::Button("Delete..."))
								RequestDeleteAsset(Asset->ID);
							ImGui::EndDisabled();
							if (Asset->ProjectRelativePath.extension() == ".prefab")
							{
								ImGui::BeginDisabled(!m_Scene);
								if (ImGui::Button("Instantiate in Scene"))
									InstantiatePrefab(Asset->ID);
								ImGui::EndDisabled();
							}
							if (IsProtectedScene)
								ImGui::TextUnformatted("Open and startup scenes cannot be deleted.");
						}
						else
							m_SelectedAsset.reset();
					}
					DrawDeleteAssetDialog();
				}
			}
			ImGui::End();
		}

		void SetStatus(std::string Message)
		{
			m_StatusMessage = std::move(Message);
			m_StatusIsError = false;
			m_StatusIsWarning = false;
			PF_INFO("{}", m_StatusMessage);
		}

		void StartRuntime()
		{
			if (!m_Project || !m_Scene)
			{
				SetError("Open a project and scene before starting the runtime.");
				return;
			}

			auto CandidateScene = PulseForge::SceneSerializer::Clone(*m_Scene);
			if (!CandidateScene)
			{
				SetError("Could not copy the authored scene for runtime: " + CandidateScene.error().Message);
				return;
			}

			PulseForge::SceneRuntimeServices Services;
			Services.InputState = &PulseForge::Application::Get().GetInput();
			auto CandidateRuntime = std::make_unique<PulseForge::SceneRuntime>(*m_Project,
				PulseForge::SceneRuntimeDesc{}, Services);
			if (auto Result = CandidateRuntime->Start(**CandidateScene); !Result)
			{
				SetError("Could not start the scene runtime: " + Result.error().Message);
				return;
			}

			m_RuntimeScene = std::move(*CandidateScene);
			m_SceneRuntime = std::move(CandidateRuntime);
			SetStatus("Runtime started on an isolated scene copy. The editor viewport is not connected yet.");
		}

		void StopRuntime() noexcept
		{
			if (m_SceneRuntime)
				m_SceneRuntime->Stop();
			m_SceneRuntime.reset();
			m_RuntimeScene.reset();
		}

		void RefreshAssets()
		{
			if (!m_Project)
				return;

			const auto Rebuild = m_Project->GetAssetRegistry().Rebuild(m_Project->GetRootPath());
			if (!Rebuild)
			{
				std::string Message = "Asset registry refresh failed:";
				if (Rebuild.error().Issues.empty())
					Message += " unknown registry error";
				for (const PulseForge::AssetRegistryIssue& Issue : Rebuild.error().Issues)
					Message += "\n - " + Issue.Message;
				SetError(std::move(Message));
				return;
			}
			UpdateAssetList();
			SetStatus("Asset registry refreshed.");
		}

		void UpdateAssetList()
		{
			if (m_Project)
				m_Assets = m_Project->GetAssetRegistry().GetAssets();
			else
				m_Assets.clear();
		}

		void SetWarning(std::string Message)
		{
			m_StatusMessage = std::move(Message);
			m_StatusIsError = false;
			m_StatusIsWarning = true;
			PF_WARN("{}", m_StatusMessage);
		}

		void SetError(std::string Message)
		{
			m_StatusMessage = std::move(Message);
			m_StatusIsError = true;
			m_StatusIsWarning = false;
			PF_ERROR("{}", m_StatusMessage);
		}

		void ClearScene() noexcept
		{
			StopRuntime();
			m_Scene.reset();
			m_SceneAsset.reset();
			m_SelectedEntity.reset();
			m_SelectedAsset.reset();
			m_PendingDeleteAsset.reset();
			m_OpenDeleteAssetDialog = false;
			m_SceneDirty = false;
		}

		void ShutdownImGui() noexcept
		{
			if (!m_Context)
				return;

			ImGui::SetCurrentContext(m_Context);
			if (m_OpenGLBackendActive)
				ImGui_ImplOpenGL3_Shutdown();
			if (m_GlfwBackendActive)
				ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext(m_Context);
			m_Context = nullptr;
			m_OpenGLBackendActive = false;
			m_GlfwBackendActive = false;
		}

		void ShutdownFileDialog() noexcept
		{
			if (m_FileDialogActive)
				NFD_Quit();
			m_FileDialogActive = false;
		}

		ImGuiContext* m_Context = nullptr;
		bool m_GlfwBackendActive = false;
		bool m_OpenGLBackendActive = false;
		bool m_FileDialogActive = false;
		bool m_SceneDirty = false;
		bool m_OpenUnsavedDialog = false;
		bool m_ConsoleAutoScroll = true;
		uint64_t m_ConsoleRevision = 0;
		std::shared_ptr<EditorConsoleSink> m_ConsoleSink;
		std::vector<EditorConsoleMessage> m_ConsoleMessages;
		std::vector<std::shared_ptr<spdlog::logger>> m_ConsoleLoggers;
		std::function<void()> m_PendingAction;
		std::optional<PulseForge::Project> m_Project;
		std::unique_ptr<PulseForge::Scene> m_Scene;
		std::unique_ptr<PulseForge::Scene> m_RuntimeScene;
		std::unique_ptr<PulseForge::SceneRuntime> m_SceneRuntime;
		std::optional<PulseForge::AssetID> m_SceneAsset;
		std::optional<PulseForge::UUID> m_SelectedEntity;
		std::optional<PulseForge::AssetID> m_SelectedAsset;
		std::optional<PulseForge::AssetID> m_PendingDeleteAsset;
		std::vector<PulseForge::AssetRecord> m_Assets;
		std::string m_StatusMessage = "Create or open a project to begin.";
		bool m_StatusIsError = false;
		bool m_StatusIsWarning = false;
		bool m_OpenDeleteAssetDialog = false;
	};

	class PulseForgeEditorApplication final : public PulseForge::Application
	{
	public:
		PulseForgeEditorApplication()
			: Application(PulseForge::RendererAPI::OpenGL)
		{
			PushLayer(std::make_unique<EditorLayer>());
			PF_INFO("PulseForge editor started with the OpenGL ImGui backend");
		}
	};
}

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication()
{
	return std::make_unique<PulseForgeEditorApplication>();
}
