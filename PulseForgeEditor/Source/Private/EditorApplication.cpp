#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dwmapi.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#ifdef PF_EDITOR_RENDERER_OPENGL
#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#endif

#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Core/ScopedProfileTimer.h"
#include "Assets/GltfMeshImporter.h"
#include "Events/ApplicationEvent.h"
#include "Events/Event.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"
#include "Assets/AssetOperations.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/MaterialAssetService.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Scene.h"
#include "Scene/SceneRenderSnapshot.h"
#include "Scene/SceneSerializer.h"
#include "Runtime/SceneRuntime.h"
#include "Window/Window.h"
#include "Editor/EditorImGuiRenderer.h"
#include "Editor/EditorAssetPreviewService.h"
#include "Editor/EditorAssetPreviewPolicy.h"
#include "Editor/ConsoleFormatting.h"
#include "Editor/EditorLayout.h"
#include "Editor/EditorCommandHistory.h"
#include "Editor/EditorContentBrowserPolicy.h"
#include "Editor/EditorProjectOpening.h"
#include "EditorApplicationFactory.h"
#include "Editor/EditorInteractionPolicy.h"
#include "Editor/EditorSelection.h"
#include "Editor/EditorVisibility.h"
#include "Editor/EditorEntityIconPolicy.h"
#include "Editor/EditorHierarchyRowPolicy.h"
#include "Editor/EditorMaterialEditorPolicy.h"
#include "Editor/EditorMaterialPropertyLayoutPolicy.h"
#include "Editor/EditorStyle.h"
#include "Editor/EditorWorkspaceView.h"
#include "Editor/EditorViewportToolbarPolicy.h"
#include "Editor/ViewportMouseCapture.h"
#include "Editor/ViewportMath.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <nfd.h>
#include <nfd_glfw3.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_glfw.h>
#include <imgui_internal.h>
#ifdef PF_EDITOR_RENDERER_OPENGL
#include <backends/imgui_impl_opengl3.h>
#endif
#include <glm/gtc/quaternion.hpp>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <charconv>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <chrono>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	using EditorJson = nlohmann::ordered_json;

	PulseForge::OutputColorEncoding GetEditorUiColorEncoding(PulseForge::OutputColorEncoding OutputEncoding) noexcept
	{
		#ifdef PF_EDITOR_RENDERER_OPENGL
		return OutputEncoding;
		#else
		(void)OutputEncoding;
		// Vulkan ImGui always blends against an sRGB attachment: directly for an sRGB swapchain,
		// or in the editor-owned sRGB intermediate for a UNORM swapchain.
		return PulseForge::OutputColorEncoding::SrgbAttachment;
		#endif
	}

	void RequestDarkNativeCaption(GLFWwindow* Window)
	{
		const HWND NativeWindow = Window ? glfwGetWin32Window(Window) : nullptr;
		if (!NativeWindow)
			return;

		const BOOL UseDarkMode = TRUE;
		const HRESULT Result = DwmSetWindowAttribute(
			NativeWindow,
			DWMWA_USE_IMMERSIVE_DARK_MODE,
			&UseDarkMode,
			sizeof(UseDarkMode));
		if (FAILED(Result))
			PF_INFO("Dark native caption styling is unavailable on this Windows version; using the system caption style");
	}

	bool BeginEditorChromeBar(
		const char* ID,
		ImGuiDir Direction,
		float Height,
		PulseForgeEditor::EditorColorToken Background,
		const PulseForgeEditor::EditorStyle& Style)
	{
		ImGui::PushStyleColor(ImGuiCol_WindowBg, Style.GetColor(Background));
		ImGui::PushStyleColor(ImGuiCol_Border, Style.GetColor(PulseForgeEditor::EditorColorToken::Border));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		const bool Visible = ImGui::BeginViewportSideBar(
			ID,
			ImGui::GetMainViewport(),
			Direction,
			Height,
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
				ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
		if (!Visible)
		{
			ImGui::End();
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(2);
		}
		return Visible;
	}

	void EndEditorChromeBar()
	{
		ImGui::End();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(2);
	}

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

	PulseForgeEditor::EditorIcon GetEntityIcon(const PulseForge::Entity& Entity)
	{
		PulseForgeEditor::EditorEntityIconComponents Components;
		const auto Camera = Entity.GetCamera();
		Components.HasCamera = Camera && Camera->has_value();

		const auto Mesh = Entity.GetMeshRenderer();
		Components.HasMesh = Mesh && Mesh->has_value();
		if (Components.HasCamera || Components.HasMesh)
			return PulseForgeEditor::GetEditorEntityIcon(Components);

		const auto SpotLight = Entity.GetSpotLight();
		Components.HasSpotLight = SpotLight && SpotLight->has_value();
		if (Components.HasSpotLight)
			return PulseForgeEditor::GetEditorEntityIcon(Components);

		const auto PointLight = Entity.GetPointLight();
		Components.HasPointLight = PointLight && PointLight->has_value();
		if (Components.HasPointLight)
			return PulseForgeEditor::GetEditorEntityIcon(Components);

		const auto DirectionalLight = Entity.GetDirectionalLight();
		Components.HasDirectionalLight = DirectionalLight && DirectionalLight->has_value();
		if (Components.HasDirectionalLight)
			return PulseForgeEditor::GetEditorEntityIcon(Components);

		const auto EnvironmentLight = Entity.GetEnvironmentLight();
		Components.HasEnvironmentLight = EnvironmentLight && EnvironmentLight->has_value();
		if (Components.HasEnvironmentLight)
			return PulseForgeEditor::GetEditorEntityIcon(Components);

		const auto AudioSource = Entity.GetAudioSource();
		Components.HasAudioSource = AudioSource && AudioSource->has_value();
		if (Components.HasAudioSource)
			return PulseForgeEditor::GetEditorEntityIcon(Components);

		const auto AudioListener = Entity.GetAudioListener();
		Components.HasAudioListener = AudioListener && AudioListener->has_value();

		return PulseForgeEditor::GetEditorEntityIcon(Components);
	}

	std::string FitHierarchyEntityName(std::string_view Name, float AvailableWidth)
	{
		constexpr std::string_view Ellipsis = "\xe2\x80\xa6";
		constexpr size_t MaximumLabelBytes = 480;
		size_t PrefixLength = PulseForgeEditor::Utf8SafePrefixLength(Name, MaximumLabelBytes);
		while (PrefixLength > 0)
		{
			const std::string_view Prefix = Name.substr(0, PrefixLength);
			const float Width = ImGui::CalcTextSize(Prefix.data(), Prefix.data() + Prefix.size()).x +
				ImGui::CalcTextSize(Ellipsis.data(), Ellipsis.data() + Ellipsis.size()).x;
			if (Width <= AvailableWidth)
			{
				std::string Result(Prefix);
				Result.append(Ellipsis);
				return Result;
			}
			PrefixLength = PulseForgeEditor::Utf8SafePrefixLength(Name, PrefixLength - 1);
		}

		return ImGui::CalcTextSize(Ellipsis.data(), Ellipsis.data() + Ellipsis.size()).x <= AvailableWidth
			? std::string(Ellipsis)
			: std::string{};
	}

	std::string FitEditorContextLabel(std::string_view Label, float AvailableWidth)
	{
		constexpr std::string_view Ellipsis = "\xe2\x80\xa6";
		if (ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x <= AvailableWidth)
			return std::string(Label);

		size_t PrefixLength = PulseForgeEditor::Utf8SafePrefixLength(Label, 480);
		const float EllipsisWidth = ImGui::CalcTextSize(Ellipsis.data(), Ellipsis.data() + Ellipsis.size()).x;
		while (PrefixLength > 0)
		{
			const std::string_view Prefix = Label.substr(0, PrefixLength);
			if (ImGui::CalcTextSize(Prefix.data(), Prefix.data() + Prefix.size()).x + EllipsisWidth <= AvailableWidth)
				return std::string(Prefix) + std::string(Ellipsis);
			PrefixLength = PulseForgeEditor::Utf8SafePrefixLength(Label, PrefixLength - 1);
		}
		return EllipsisWidth <= AvailableWidth ? std::string(Ellipsis) : std::string{};
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
		uint64_t Sequence = 0;
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
				++m_NextSequence,
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
		uint64_t m_NextSequence = 0;
	};

	class EditorViewportCamera final
	{
	public:
		void Reset() noexcept
		{
			m_Position = { 0.0f, 0.0f, 3.1f };
			m_YawDegrees = -90.0f;
			m_PitchDegrees = 0.0f;
			m_MoveSpeed = 5.0f;
			m_HasFocusPivot = false;
		}

		void Rotate(float DeltaX, float DeltaY) noexcept
		{
			constexpr float Sensitivity = 0.1f;
			m_YawDegrees = std::remainder(m_YawDegrees + DeltaX * Sensitivity, 360.0f);
			m_PitchDegrees = std::clamp(m_PitchDegrees - DeltaY * Sensitivity, -89.0f, 89.0f);
			m_HasFocusPivot = false;
		}

		void Focus(const glm::vec3& Pivot, float Radius, float AspectRatio) noexcept
		{
			if (!PulseForgeEditor::Detail::IsFinite(Pivot) || !std::isfinite(Radius) || Radius < 0.0f ||
				!std::isfinite(AspectRatio) || AspectRatio <= 0.0f)
				return;
			constexpr float VerticalFov = glm::radians(45.0f);
			const float HorizontalFov = 2.0f * std::atan(std::tan(VerticalFov * 0.5f) * AspectRatio);
			const float LimitingFov = (std::min)(VerticalFov, HorizontalFov);
			const float Distance = std::clamp(Radius / std::sin(LimitingFov * 0.5f) * 1.2f, 0.2f, 100000.0f);
			if (!std::isfinite(Distance))
				return;
			m_FocusPivot = Pivot;
			m_HasFocusPivot = true;
			m_Position = Pivot - GetForward() * Distance;
		}

		void Orbit(float DeltaX, float DeltaY) noexcept
		{
			if (!m_HasFocusPivot)
				return;
			const float Distance = glm::length(m_Position - m_FocusPivot);
			constexpr float Sensitivity = 0.1f;
			m_YawDegrees = std::remainder(m_YawDegrees + DeltaX * Sensitivity, 360.0f);
			m_PitchDegrees = std::clamp(m_PitchDegrees - DeltaY * Sensitivity, -89.0f, 89.0f);
			m_Position = m_FocusPivot - GetForward() * Distance;
		}

		void Pan(float DeltaX, float DeltaY, float ViewportHeight) noexcept
		{
			if (!m_HasFocusPivot || !std::isfinite(ViewportHeight) || ViewportHeight < 1.0f)
				return;
			const float Distance = glm::length(m_Position - m_FocusPivot);
			const float UnitsPerPixel = 2.0f * Distance * std::tan(glm::radians(45.0f) * 0.5f) / ViewportHeight;
			const glm::vec3 Right = glm::normalize(glm::cross(GetForward(), glm::vec3(0.0f, 1.0f, 0.0f)));
			const glm::vec3 Up = glm::normalize(glm::cross(Right, GetForward()));
			const glm::vec3 Offset = (-Right * DeltaX + Up * DeltaY) * UnitsPerPixel;
			m_Position += Offset;
			m_FocusPivot += Offset;
		}

		void Dolly(float Delta) noexcept
		{
			if (!std::isfinite(Delta) || std::abs(Delta) < 1.0e-6f)
				return;
			const std::optional<glm::vec3> ExistingPivot = m_HasFocusPivot
				? std::optional<glm::vec3>{ m_FocusPivot }
				: std::nullopt;
			const auto Dolly = PulseForgeEditor::CalculateViewportCameraDolly(
				m_Position, GetForward(), ExistingPivot, m_MoveSpeed, Delta);
			if (!Dolly)
				return;
			m_Position = Dolly->Position;
			m_FocusPivot = Dolly->FocusPivot;
			m_HasFocusPivot = true;
		}

		void AdjustMoveSpeed(float WheelDelta) noexcept
		{
			if (!std::isfinite(WheelDelta))
				return;
			m_MoveSpeed = std::clamp(m_MoveSpeed * std::pow(1.2f, WheelDelta), 0.2f, 80.0f);
		}

		void Move(const PulseForge::Input& Input, double DeltaSeconds) noexcept
		{
			const glm::vec3 Forward = GetForward();
			const glm::vec3 Right = glm::normalize(glm::cross(Forward, glm::vec3(0.0f, 1.0f, 0.0f)));
			const PulseForgeEditor::ViewportCameraMoveKeys Keys{
				.Forward = Input.IsKeyPressed(GLFW_KEY_W),
				.Backward = Input.IsKeyPressed(GLFW_KEY_S),
				.Left = Input.IsKeyPressed(GLFW_KEY_A),
				.Right = Input.IsKeyPressed(GLFW_KEY_D),
				.Down = Input.IsKeyPressed(GLFW_KEY_Q),
				.Up = Input.IsKeyPressed(GLFW_KEY_E),
				.Fast = Input.IsKeyPressed(GLFW_KEY_LEFT_SHIFT) || Input.IsKeyPressed(GLFW_KEY_RIGHT_SHIFT)
			};
			const glm::vec3 Offset = PulseForgeEditor::CalculateViewportCameraMoveOffset(
				Forward, Right, Keys, m_MoveSpeed, DeltaSeconds);
			if (glm::dot(Offset, Offset) < 1.0e-12f)
				return;
			m_Position += Offset;
			if (m_HasFocusPivot)
				m_FocusPivot += Offset;
		}

		[[nodiscard]] std::expected<glm::mat4, std::string> GetViewProjection(float AspectRatio) const
		{
			PulseForge::CameraComponent Camera;
			auto Projection = Camera.GetProjectionMatrix(AspectRatio);
			if (!Projection)
				return std::unexpected(Projection.error().Message);
			return *Projection * GetView();
		}

		[[nodiscard]] glm::mat4 GetView() const
		{
			return glm::lookAtRH(m_Position, m_Position + GetForward(), glm::vec3(0.0f, 1.0f, 0.0f));
		}

		[[nodiscard]] std::expected<glm::mat4, std::string> GetProjection(float AspectRatio) const
		{
			PulseForge::CameraComponent Camera;
			auto Projection = Camera.GetProjectionMatrix(AspectRatio);
			if (!Projection)
				return std::unexpected(Projection.error().Message);
			return *Projection;
		}

		[[nodiscard]] glm::vec3 GetPosition() const noexcept { return m_Position; }

		[[nodiscard]] glm::vec3 GetForward() const noexcept
		{
			const float Yaw = glm::radians(m_YawDegrees);
			const float Pitch = glm::radians(m_PitchDegrees);
			return glm::normalize(glm::vec3(
				std::cos(Yaw) * std::cos(Pitch),
				std::sin(Pitch),
				std::sin(Yaw) * std::cos(Pitch)));
		}

	private:
		glm::vec3 m_Position{ 0.0f, 0.0f, 3.1f };
		glm::vec3 m_FocusPivot{ 0.0f };
		float m_YawDegrees = -90.0f;
		float m_PitchDegrees = 0.0f;
		float m_MoveSpeed = 5.0f;
		bool m_HasFocusPivot = false;
	};

	class EditorLayer final : public PulseForge::Layer
	{
		struct CpuPickingMesh
		{
			std::vector<glm::vec3> Positions;
			std::vector<uint32_t> Indices;
		};

		struct ContentFolderNode
		{
			std::string Name;
			std::filesystem::path RelativePath;
			std::vector<ContentFolderNode> Children;
		};

		struct EntityClipboard
		{
			PulseForge::UUID Project;
			std::optional<PulseForge::AssetID> Scene;
			std::vector<PulseForge::UUID> Roots;
			std::vector<EditorJson> Records;
		};

		struct AssetBrowserPresentation
		{
			std::string Identifier;
			std::string Name;
			std::string FullPath;
			std::string Extension;
			PulseForgeEditor::EditorIcon Icon = PulseForgeEditor::EditorIcon::File;
		};

		struct GizmoAxisScreen
		{
			glm::vec3 WorldDirection{ 0.0f };
			float WorldUnitsPerLocalUnit = 0.0f;
			glm::vec2 Endpoint{ 0.0f };
			bool IsValid = false;
		};

		struct ViewportGizmoGeometry
		{
			glm::vec3 PivotWorld{ 0.0f };
			glm::vec2 PivotScreen{ 0.0f };
			float HandleLengthWorld = 0.0f;
			std::array<GizmoAxisScreen, 3> Axes{};
			std::array<std::array<glm::vec2, 49>, 3> RotationRings{};
			std::array<bool, 3> RotationRingValid{};
			bool IsValid = false;
		};

		struct GizmoHandle
		{
			PulseForgeEditor::TransformGizmoOperation Operation;
			uint32_t Axis = 0;
		};

		enum class ViewportCameraGesture : uint8_t
		{
			None,
			Orbit,
			Pan,
			Dolly
		};

		struct GizmoEntityStart
		{
			PulseForge::UUID Identifier;
			PulseForge::TransformComponent LocalTransform;
			glm::mat4 WorldTransform{ 1.0f };
			glm::mat4 ParentWorldTransform{ 1.0f };
		};

		struct GizmoDrag
		{
			PulseForge::UUID Entity;
			const PulseForge::Scene* SceneIdentity = nullptr;
			PulseForge::TransformComponent InitialTransform;
			std::vector<GizmoEntityStart> Targets;
			std::vector<PulseForge::UUID> SourceRoots;
			PulseForgeEditor::TransformGizmoOperation Operation;
			uint32_t Axis = 0;
			glm::vec3 PivotWorld{ 0.0f };
			glm::vec3 WorldAxis{ 0.0f };
			glm::vec3 PlaneNormal{ 0.0f };
			glm::vec3 StartPoint{ 0.0f };
			glm::vec2 StartMouse{ 0.0f };
			float WorldUnitsPerLocalUnit = 0.0f;
			bool DuplicateOnDrag = false;
			bool HasDuplicated = false;
		};

	public:
		explicit EditorLayer(PulseForgeEditor::EditorCommandLineOptions Options)
			: Layer("Editor"),
			  m_StartupProjectPath(std::move(Options.ProjectFile)),
			  m_AssetPreviewStressManifest(std::move(Options.AssetPreviewStressManifest)),
			  m_AssetPreviewStressBudget(std::move(Options.AssetPreviewStressBudget))
		{
		}

		~EditorLayer() override
		{
			EndEditorCameraNavigation();
			CancelGizmoInteraction();
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
			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
			IO.ConfigDragClickToInputText = true;
			#ifdef PF_EDITOR_RENDERER_OPENGL
			IO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
			#endif
			if (auto Result = m_EditorStyle.Initialize(); !Result)
			{
				ShutdownImGui();
				throw std::runtime_error("Could not initialize editor typography: " + Result.error());
			}
			m_EditorStyle.ApplyTheme(GetEditorUiColorEncoding(
				PulseForge::Application::Get().GetOutputColorEncoding()));
			if (auto Result = m_Layout.Initialize(); !Result)
				ReportLayoutPersistenceError(Result.error());

			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			RequestDarkNativeCaption(Window);
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!ImGui_ImplGlfw_InitForOpenGL(Window, true))
			#else
			if (!ImGui_ImplGlfw_InitForOther(Window, true))
			#endif
			{
				ShutdownImGui();
				throw std::runtime_error("Could not initialize the editor GLFW ImGui backend");
			}
			m_GlfwBackendActive = true;

			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!ImGui_ImplOpenGL3_Init("#version 410"))
			{
				ShutdownImGui();
				throw std::runtime_error("Could not initialize the editor OpenGL ImGui backend");
			}
			m_OpenGLBackendActive = true;
			if (const char* CapturePath = std::getenv("PULSEFORGE_EDITOR_OPENGL_FRAME_CAPTURE"))
				m_OpenGLDiagnosticCapturePath = std::filesystem::u8path(CapturePath);
			#else
			m_ImGuiRenderer = std::make_unique<PulseForgeEditor::EditorImGuiRenderer>();
			auto& Runtime = PulseForge::Application::Get();
			if (auto Result = m_ImGuiRenderer->Initialize(Runtime, std::filesystem::path(PF_EDITOR_SHADER_DIRECTORY)); !Result)
			{
				const std::string Message = "Could not initialize the Vulkan editor UI renderer: " + Result.error();
				ShutdownImGui();
				throw std::runtime_error(Message);
			}
			PulseForgeEditor::AssetPreviewServiceLimits PreviewLimits;
			if (m_AssetPreviewStressManifest)
			{
				PreviewLimits.MaximumQueuedRequests = 32;
				PreviewLimits.MaximumCpuBytes = 2ull * 1024ull * 1024ull;
				PreviewLimits.MaximumUploadsPerUpdate = 4;
				PreviewLimits.MaximumUploadBytesPerUpdate = 1024ull * 1024ull;
				if (m_AssetPreviewStressBudget == "entries")
				{
					PreviewLimits.MaximumEntries = 8;
					PreviewLimits.MaximumGpuBytes = 64ull * 1024ull * 1024ull;
				}
				else
				{
					PreviewLimits.MaximumEntries = 512;
					PreviewLimits.MaximumGpuBytes = 4ull * 192ull * 192ull * 4ull;
				}
			}
			m_AssetPreviews.Initialize(Runtime, *m_ImGuiRenderer, PreviewLimits);
			if (m_AssetPreviewStressManifest)
				InitializeAssetPreviewStress();
			#endif
			AttachConsoleSink();

			if (NFD_Init() == NFD_OKAY)
			{
				m_FileDialogActive = true;
				PF_INFO("Native file dialogs initialized");
			}
			else
				SetError(NfdError("Could not initialize the native file dialog"));

			if (m_StartupProjectPath)
			{
				auto Opened = OpenProjectAt(*m_StartupProjectPath);
				if (!Opened)
					throw std::runtime_error("Startup project load failed: " + Opened.error());
			}
		}

		void OnDetach() override
		{
			EndEditorCameraNavigation();
			CancelGizmoInteraction();
			StopRuntime();
			DetachConsoleSink();
			ResetViewportSceneRenderer();
			ResetViewportTarget();
			m_Scene.reset();
			m_Project.reset();
			ShutdownImGui();
			ShutdownFileDialog();
		}

		void OnUpdate(PulseForge::Timestep DeltaTime) override
		{
			if (m_AssetPreviewProjectPending)
			{
				if (!m_AssetPreviewStressManifest)
					m_AssetPreviews.SetProject(m_Project ? m_Project->GetDescription().ID.ToString() : std::string{});
				m_AssetPreviewProjectPending = false;
			}
			if (m_AssetPreviewAssetsPending)
			{
				m_AssetPreviews.InvalidateAll();
				m_AssetPreviewAssetsPending = false;
			}
			UpdateEditorCamera(DeltaTime);
			if (m_SceneRuntime && m_RuntimeScene)
			{
				if (auto Result = m_SceneRuntime->Advance(*m_RuntimeScene, DeltaTime); !Result)
				{
					const std::string Message = "Runtime update failed: " + Result.error().Message;
					StopRuntime();
					SetError(Message);
				}
			}
			PrepareViewportScene();
			m_AssetPreviews.PumpUploads();
		}

		void InitializeAssetPreviewStress()
		{
			if (m_AssetPreviewStressBudget != "entries" && m_AssetPreviewStressBudget != "gpu")
				throw std::runtime_error("Asset preview stress is supported only by the Vulkan editor build");
			std::ifstream Manifest(*m_AssetPreviewStressManifest, std::ios::binary);
			if (!Manifest)
				throw std::runtime_error("Could not open asset preview stress manifest");
			std::string Line;
			while (std::getline(Manifest, Line))
			{
				if (!Line.empty() && Line.back() == '\r')
					Line.pop_back();
				const size_t Separator = Line.find('|');
				if (Separator == std::string::npos || Separator == 0 || Separator + 1 >= Line.size())
					throw std::runtime_error("Invalid asset preview stress manifest row");
				m_AssetPreviewStressAssets.emplace_back(
					Line.substr(0, Separator), std::filesystem::u8path(Line.substr(Separator + 1)));
			}
			if (m_AssetPreviewStressAssets.size() < 32 || m_AssetPreviewStressAssets.size() > 256)
				throw std::runtime_error("Stress manifest must contain between 32 and 256 deterministic assets");
			m_AssetPreviewStressReportPath = m_AssetPreviewStressManifest->parent_path() /
				("vulkan_stress_" + m_AssetPreviewStressBudget + ".json");
			m_AssetPreviews.SetProject(m_AssetPreviewStressProject);
			PF_INFO("Asset preview Vulkan stress enabled: {} assets, {} budget", m_AssetPreviewStressAssets.size(),
				m_AssetPreviewStressBudget);
		}

		void DrawAssetPreviewStress()
		{
			if (m_AssetPreviewStressFinished)
				return;
			constexpr size_t BatchSize = 4;
			const size_t BatchCount = m_AssetPreviewStressAssets.size() / BatchSize;
			if (m_AssetPreviewStressBatch >= BatchCount)
				return;
			const size_t First = m_AssetPreviewStressBatch * BatchSize;
			ImGui::SetNextWindowSize(ImVec2(500.0f, 210.0f), ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("Asset Preview GPU Stress (opt-in)"))
			{
				ImGui::End();
				return;
			}
			ImGui::Text("Budget: %s | Batch: %zu / %zu", m_AssetPreviewStressBudget.c_str(),
				m_AssetPreviewStressBatch + 1, BatchCount);
			bool AllReady = true;
			for (size_t Offset = 0; Offset < BatchSize; ++Offset)
			{
				const auto& [AssetID, Path] = m_AssetPreviewStressAssets[First + Offset];
				m_AssetPreviews.RequestImage(m_AssetPreviewStressProject, AssetID, Path);
				const auto Preview = m_AssetPreviews.FindPreview(m_AssetPreviewStressProject, AssetID);
				if (Preview)
				{
					if (Offset != 0)
						ImGui::SameLine();
					ImGui::Image(ImTextureRef(static_cast<ImTextureID>(Preview->TextureID)), ImVec2(88.0f, 88.0f));
					ImGui::Text("%s", AssetID.c_str());
					++m_AssetPreviewStressDraws;
				}
				else
				{
					AllReady = false;
					ImGui::Text("Waiting: %s", AssetID.c_str());
					ImGui::SameLine();
					ImGui::Dummy(ImVec2(4.0f, 1.0f));
				}
			}
			ImGui::End();
			m_AssetPreviewStressReadyThisFrame = AllReady;
		}

		void AdvanceAssetPreviewStressFrame()
		{
			if (m_AssetPreviewStressFinished)
				return;
			if (m_AssetPreviewStressRenderFailedThisFrame)
			{
				m_AssetPreviewStressFailed = true;
				m_AssetPreviewStressFinished = true;
				PulseForge::Application::Get().RequestClose();
				return;
			}
			if (m_AssetPreviewStressBatch == 0 && !m_AssetPreviewStressInvalidatedQueued)
			{
				m_AssetPreviews.InvalidateAll();
				m_AssetPreviewStressInvalidatedQueued = true;
			}
			if (!m_AssetPreviewStressReadyThisFrame)
				return;
			++m_AssetPreviewStressSubmissions;
			++m_AssetPreviewStressFrames;
			if (m_AssetPreviewStressFrames < 4)
				return;
			m_AssetPreviewStressFrames = 0;
			const size_t BatchCount = m_AssetPreviewStressAssets.size() / 4;
			++m_AssetPreviewStressBatch;
			if (!m_AssetPreviewStressRefreshed && m_AssetPreviewStressBatch >= BatchCount / 5)
			{
				m_AssetPreviews.InvalidateAll();
				m_AssetPreviewStressRefreshed = true;
			}
			if (!m_AssetPreviewStressSwitchedProject && m_AssetPreviewStressBatch >= BatchCount / 3)
			{
				m_AssetPreviews.InvalidateAll();
				m_AssetPreviews.SetProject("Phase5StressProjectB");
				m_AssetPreviewStressProject = "Phase5StressProjectB";
				m_AssetPreviewStressSwitchedProject = true;
			}
			if (m_AssetPreviewStressBatch >= BatchCount)
			{
				if (m_AssetPreviewStressPass == 0)
				{
					m_AssetPreviewStressPass = 1;
					m_AssetPreviewStressBatch = 0;
				}
				else
				{
					m_AssetPreviews.InvalidateAll();
					for (size_t Index = 0; Index < 8; ++Index)
					{
						const auto& [AssetID, Path] = m_AssetPreviewStressAssets[Index];
						m_AssetPreviews.RequestImage(m_AssetPreviewStressProject, "shutdown-" + AssetID, Path);
					}
					m_AssetPreviewStressShutdownPendingRequests = m_AssetPreviews.GetStats().PendingRequests;
					PF_INFO("Asset preview stress queued {} requests immediately before shutdown",
						m_AssetPreviewStressShutdownPendingRequests);
					m_AssetPreviewStressFinished = true;
					PF_INFO("Asset preview stress workload completed; requesting clean editor close");
					PulseForge::Application::Get().RequestClose();
				}
			}
		}

		void WriteAssetPreviewStressReport() noexcept
		{
			if (m_AssetPreviewStressReportPath.empty() || m_AssetPreviewStressReportWritten)
				return;
			m_AssetPreviewStressReportWritten = true;
			try
			{
				const auto Stats = m_AssetPreviews.GetStats();
				std::ofstream Report(m_AssetPreviewStressReportPath, std::ios::binary | std::ios::trunc);
				if (!Report)
					return;
				Report << "{\n"
					<< "  \"budget_mode\": \"" << m_AssetPreviewStressBudget << "\",\n"
					<< "  \"fixtures\": " << m_AssetPreviewStressAssets.size() << ",\n"
					<< "  \"project_switch\": " << (m_AssetPreviewStressSwitchedProject ? "true" : "false") << ",\n"
					<< "  \"queued_result_invalidation\": " << (m_AssetPreviewStressInvalidatedQueued ? "true" : "false") << ",\n"
					<< "  \"refresh_invalidation\": " << (m_AssetPreviewStressRefreshed ? "true" : "false") << ",\n"
					<< "  \"rendered_preview_images\": " << m_AssetPreviewStressDraws << ",\n"
					<< "  \"render_frames\": " << m_AssetPreviewStressSubmissions << ",\n"
					<< "  \"shutdown_pending_requests_before_close\": " << m_AssetPreviewStressShutdownPendingRequests << ",\n"
					<< "  \"cache_hits\": " << Stats.CacheHits << ",\n"
					<< "  \"cache_misses\": " << Stats.CacheMisses << ",\n"
					<< "  \"decodes_succeeded\": " << Stats.DecodesSucceeded << ",\n"
					<< "  \"decodes_failed\": " << Stats.DecodesFailed << ",\n"
					<< "  \"uploads\": " << Stats.Uploads << ",\n"
					<< "  \"evictions\": " << Stats.Evictions << ",\n"
					<< "  \"binding_registrations\": " << Stats.BindingRegistrations << ",\n"
					<< "  \"binding_unregistrations\": " << Stats.BindingUnregistrations << ",\n"
					<< "  \"stale_results_discarded\": " << Stats.StaleResultsDiscarded << ",\n"
					<< "  \"peak_entries\": " << Stats.PeakEntries << ",\n"
					<< "  \"peak_cpu_bytes\": " << Stats.PeakCpuBytes << ",\n"
					<< "  \"peak_gpu_bytes\": " << Stats.PeakGpuBytes << ",\n"
					<< "  \"final_entries\": " << Stats.ActiveEntries << ",\n"
					<< "  \"final_gpu_bytes\": " << Stats.GpuBytes << ",\n"
					<< "  \"final_pending_requests\": " << Stats.PendingRequests << ",\n"
					<< "  \"renderer_registered_textures_after_service_shutdown\": "
					<< (m_ImGuiRenderer ? m_ImGuiRenderer->GetRegisteredTextureCount() : 0) << ",\n"
					<< "  \"render_failed\": " << (m_AssetPreviewStressFailed ? "true" : "false") << ",\n"
					<< "  \"completed\": " << (m_AssetPreviewStressFinished && !m_AssetPreviewStressFailed
						? "true" : "false") << "\n}\n";
			}
			catch (...)
			{
			}
		}

		#ifdef PF_EDITOR_RENDERER_OPENGL
		void CaptureOpenGLFrameForValidation() noexcept
		{
			if (m_OpenGLDiagnosticCapturePath.empty() || m_OpenGLDiagnosticCaptureWritten ||
				m_OpenGLDiagnosticFramesSeen < 3)
				return;
			m_OpenGLDiagnosticCaptureWritten = true;
			CaptureOpenGLFramebuffer(m_OpenGLDiagnosticCapturePath, GL_BACK);
		}

		void CaptureOpenGLPlatformFramesForValidation(GLFWwindow* MainContext) noexcept
		{
			if (m_OpenGLDiagnosticCapturePath.empty() || m_OpenGLDiagnosticPlatformCaptureWritten ||
				m_OpenGLDiagnosticFramesSeen < 3)
				return;
			m_OpenGLDiagnosticPlatformCaptureWritten = true;
			ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
			for (int Index = 0; Index < PlatformIO.Viewports.Size; ++Index)
			{
				ImGuiViewport* Viewport = PlatformIO.Viewports[Index];
				const int VertexCount = Viewport && Viewport->DrawData ? Viewport->DrawData->TotalVtxCount : 0;
				PF_INFO("OpenGL platform viewport {}: id 0x{:08X}, {}x{}, ImGui vertices {}",
					Index,
					Viewport ? Viewport->ID : 0,
					Viewport ? Viewport->Size.x : 0.0f,
					Viewport ? Viewport->Size.y : 0.0f,
					VertexCount);
				if (Index == 0 || !Viewport || !Viewport->PlatformHandleRaw)
					continue;
				GLFWwindow* PlatformWindow = static_cast<GLFWwindow*>(Viewport->PlatformHandleRaw);
				glfwMakeContextCurrent(PlatformWindow);
				std::filesystem::path OutputPath = m_OpenGLDiagnosticCapturePath;
				OutputPath.replace_filename(OutputPath.stem().string() + "_viewport_" +
					std::to_string(Viewport->ID) + OutputPath.extension().string());
				CaptureOpenGLFramebuffer(OutputPath, GL_FRONT);
			}
			glfwMakeContextCurrent(MainContext);
		}

		void CaptureOpenGLFramebuffer(const std::filesystem::path& OutputPath, GLenum ReadBuffer) noexcept
		{
			GLint Viewport[4]{};
			GLint PreviousReadBuffer = GL_BACK;
			GLint PreviousPackAlignment = 4;
			glGetIntegerv(GL_VIEWPORT, Viewport);
			glGetIntegerv(GL_READ_BUFFER, &PreviousReadBuffer);
			glGetIntegerv(GL_PACK_ALIGNMENT, &PreviousPackAlignment);
			if (Viewport[2] <= 0 || Viewport[3] <= 0 ||
				static_cast<uint64_t>(Viewport[2]) * static_cast<uint64_t>(Viewport[3]) > 16'777'216ull)
			{
				PF_ERROR("OpenGL validation framebuffer has invalid or excessive dimensions");
				return;
			}
			const size_t PixelBytes = static_cast<size_t>(Viewport[2]) * static_cast<size_t>(Viewport[3]) * 4;
			std::vector<uint8_t> Rgba(PixelBytes);
			std::vector<uint8_t> Bgra(PixelBytes);
			glReadBuffer(ReadBuffer);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(Viewport[0], Viewport[1], Viewport[2], Viewport[3], GL_RGBA, GL_UNSIGNED_BYTE, Rgba.data());
			const GLenum ReadError = glGetError();
			glReadBuffer(static_cast<GLenum>(PreviousReadBuffer));
			glPixelStorei(GL_PACK_ALIGNMENT, PreviousPackAlignment);
			if (ReadError != GL_NO_ERROR)
			{
				PF_ERROR("OpenGL validation framebuffer readback failed with GL error 0x{:04X}", ReadError);
				return;
			}
			for (int Row = 0; Row < Viewport[3]; ++Row)
			{
				const size_t SourceRow = static_cast<size_t>(Viewport[3] - 1 - Row);
				for (int Column = 0; Column < Viewport[2]; ++Column)
				{
					const size_t Source = (SourceRow * static_cast<size_t>(Viewport[2]) + Column) * 4;
					const size_t Target = (static_cast<size_t>(Row) * static_cast<size_t>(Viewport[2]) + Column) * 4;
					Bgra[Target + 0] = Rgba[Source + 2];
					Bgra[Target + 1] = Rgba[Source + 1];
					Bgra[Target + 2] = Rgba[Source + 0];
					Bgra[Target + 3] = 255;
				}
			}
			const auto Put16 = [](std::array<uint8_t, 54>& Header, size_t Offset, uint16_t Value)
			{
				Header[Offset] = static_cast<uint8_t>(Value);
				Header[Offset + 1] = static_cast<uint8_t>(Value >> 8);
			};
			const auto Put32 = [](std::array<uint8_t, 54>& Header, size_t Offset, uint32_t Value)
			{
				for (size_t Byte = 0; Byte < 4; ++Byte)
					Header[Offset + Byte] = static_cast<uint8_t>(Value >> (Byte * 8));
			};
			std::array<uint8_t, 54> Header{};
			Header[0] = 'B'; Header[1] = 'M';
			Put32(Header, 2, static_cast<uint32_t>(Header.size() + Bgra.size()));
			Put32(Header, 10, static_cast<uint32_t>(Header.size()));
			Put32(Header, 14, 40);
			Put32(Header, 18, static_cast<uint32_t>(Viewport[2]));
			Put32(Header, 22, static_cast<uint32_t>(-Viewport[3]));
			Put16(Header, 26, 1);
			Put16(Header, 28, 32);
			Put32(Header, 34, static_cast<uint32_t>(Bgra.size()));
			std::ofstream Output(OutputPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Header.data()), static_cast<std::streamsize>(Header.size()));
			Output.write(reinterpret_cast<const char*>(Bgra.data()), static_cast<std::streamsize>(Bgra.size()));
			if (!Output)
				PF_ERROR("Could not write OpenGL validation framebuffer capture to {}",
					OutputPath.string());
			else
				PF_INFO("OpenGL validation framebuffer captured to {}: {}x{}",
					OutputPath.string(), Viewport[2], Viewport[3]);
		}
		#endif

		void OnRender() override
		{
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!m_OpenGLDiagnosticCapturePath.empty())
				++m_OpenGLDiagnosticFramesSeen;
			#endif
			m_AssetPreviewStressRenderFailedThisFrame = false;
			ImGui::SetCurrentContext(m_Context);
			#ifdef PF_EDITOR_RENDERER_OPENGL
			ImGui_ImplOpenGL3_NewFrame();
			#endif
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			m_AssetPreviews.BeginRequestFrame();
			if (m_AssetPreviewStressManifest)
				DrawAssetPreviewStress();
			m_EditorStyle.ApplyTheme(GetEditorUiColorEncoding(
				PulseForge::Application::Get().GetOutputColorEncoding()));
			CompleteReleasedGizmoInteractionBeforeShortcuts();
			HandleViewportToolShortcuts();
			HandleEditorHistoryShortcuts();
			HandleSceneEditingShortcuts();

			DrawMainMenu();
			DrawEditorToolbar();
			DrawStatusBar();
			if (m_Layout.SubmitDockspace())
				CancelViewportInteractionForLayoutChange();
			DrawWorkspacePanels();
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!m_OpenGLDiagnosticCapturePath.empty())
			{
				if (const char* Folder = std::getenv("PULSEFORGE_EDITOR_OPENGL_CONTENT_FOLDER");
					Folder && *Folder)
				{
					const std::filesystem::path RequestedFolder = std::filesystem::u8path(Folder);
					if (IsKnownContentFolder(RequestedFolder))
						m_CurrentContentFolder = RequestedFolder;
				}
				if (ImGuiWindow* ContentBrowser = ImGui::FindWindowByName("Content Browser"); ContentBrowser)
				{
					ImGui::SetWindowFocus("Content Browser");
					if (ContentBrowser->DockNode)
						ContentBrowser->DockNode->SelectedTabId = ContentBrowser->TabId;
					ImGui::MarkIniSettingsDirty();
					if (!m_OpenGLDiagnosticContentBrowserSelected)
					{
						PF_INFO("OpenGL validation selected Content Browser: window 0x{:08X}, tab 0x{:08X}, node 0x{:08X}",
							ContentBrowser->ID,
							ContentBrowser->TabId,
							ContentBrowser->DockNode ? ContentBrowser->DockNode->ID : 0);
						m_OpenGLDiagnosticContentBrowserSelected = true;
					}
				}
			}
			#endif
			RenderViewportScene();
			UpdateWindowTitle();
			if (PulseForgeEditor::ShouldFinishSceneEditForInspector(
				m_SceneHistory.IsActive(), m_InspectorHistoryItem, GImGui ? GImGui->ActiveId : 0))
				FinishSceneEdit();

			ImGui::Render();
			if (auto Result = m_Layout.SaveIfRequested(); !Result)
				ReportLayoutPersistenceError(Result.error());
			#ifdef PF_EDITOR_RENDERER_OPENGL
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			CaptureOpenGLFrameForValidation();

			ImGuiIO& IO = ImGui::GetIO();
			if (IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				GLFWwindow* MainContext = glfwGetCurrentContext();
				ImGui::UpdatePlatformWindows();
				ImGui::RenderPlatformWindowsDefault();
				CaptureOpenGLPlatformFramesForValidation(MainContext);
				glfwMakeContextCurrent(MainContext);
			}
			#else
			if (m_ImGuiRenderer)
			{
				if (auto Result = m_ImGuiRenderer->RenderDrawData(); !Result)
				{
					if (m_AssetPreviewStressManifest)
						m_AssetPreviewStressRenderFailedThisFrame = true;
					if (m_ImGuiRenderingError != Result.error())
					{
						m_ImGuiRenderingError = Result.error();
						PF_ERROR("Editor UI rendering failed: {}", m_ImGuiRenderingError);
					}
				}
				else
					m_ImGuiRenderingError.clear();
			}
			if (m_AssetPreviewStressManifest)
				AdvanceAssetPreviewStressFrame();
			#endif
		}

		void OnEvent(PulseForge::Event& Event) override
		{
			const bool NavigationOwnedMouseBeforeEvent = m_ViewportMouseCapture.OwnsMouse();
			bool BeganNavigationForEvent = false;
			if (Event.GetEventType() == PulseForge::EEventType::WindowClose && m_SceneDirty)
			{
				m_PendingAction = [] { PulseForge::Application::Get().RequestClose(); };
				m_OpenUnsavedDialog = true;
				Event.bHandled = true;
				return;
			}

			if (Event.GetEventType() == PulseForge::EEventType::WindowLostFocus)
			{
				EndEditorCameraNavigation(false);
				m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
					m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::ApplicationFocusLost);
				CancelGizmoInteraction();
				m_ViewportCameraGesture = ViewportCameraGesture::None;
			}
			else if (Event.GetEventType() == PulseForge::EEventType::MouseButtonPressed)
			{
				const auto& Mouse = static_cast<PulseForge::MouseButtonPressedEvent&>(Event);
				if (m_ViewportKeyboardFocused && !IsNativeCursorOverViewportImage())
					m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
						m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::OtherEditorPanelClicked);
				if (Mouse.GetMouseButton() == GLFW_MOUSE_BUTTON_RIGHT)
					BeganNavigationForEvent = BeginEditorCameraNavigation();
			}
			else if (Event.GetEventType() == PulseForge::EEventType::MouseButtonReleased)
			{
				const auto& Mouse = static_cast<PulseForge::MouseButtonReleasedEvent&>(Event);
				if (Mouse.GetMouseButton() == GLFW_MOUSE_BUTTON_RIGHT && m_ViewportMouseCapture.OwnsMouse())
					EndEditorCameraNavigation();
			}
			else if (Event.GetEventType() == PulseForge::EEventType::MouseScrolled)
			{
				if (m_Context)
					ImGui::SetCurrentContext(m_Context);
				const bool PopupOpen = m_Context &&
					ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
				const PulseForgeEditor::ViewportWheelAction WheelAction =
					PulseForgeEditor::ResolveViewportWheelAction({
						.RightMouseNavigationActive = m_EditorCameraNavigationActive,
						.RightMouseHeld = PulseForge::Application::Get().GetInput().IsMouseButtonPressed(
							GLFW_MOUSE_BUTTON_RIGHT),
						.RuntimeActive = m_SceneRuntime != nullptr,
						.PopupOpen = PopupOpen
					});
				if (WheelAction == PulseForgeEditor::ViewportWheelAction::AdjustFlySpeed)
				{
					const auto& Scroll = static_cast<PulseForge::MouseScrolledEvent&>(Event);
					m_EditorCamera.AdjustMoveSpeed(Scroll.GetYOffset());
				}
			}
			else if (Event.GetEventType() == PulseForge::EEventType::MouseMoved && m_ViewportMouseCapture.OwnsMouse())
			{
				const auto& Mouse = static_cast<PulseForge::MouseMovedEvent&>(Event);
				if (m_IgnoreFirstCursorDelta)
					m_IgnoreFirstCursorDelta = false;
				else
				{
					m_EditorCamera.Rotate(Mouse.GetX() - m_LastCursorX, Mouse.GetY() - m_LastCursorY);
				}
				m_LastCursorX = Mouse.GetX();
				m_LastCursorY = Mouse.GetY();
			}

			ImGui::SetCurrentContext(m_Context);
			const ImGuiIO& IO = ImGui::GetIO();
			Event.bHandled |= Event.IsInCategory(PulseForge::EventCategoryMouse) &&
				(NavigationOwnedMouseBeforeEvent || BeganNavigationForEvent || m_ViewportMouseCapture.OwnsMouse() || IO.WantCaptureMouse);
			Event.bHandled |= Event.IsInCategory(PulseForge::EventCategoryKeyboard) && IO.WantCaptureKeyboard;
		}

	private:
		[[nodiscard]] std::expected<std::string, std::string> CaptureSceneState() const
		{
			if (!m_Scene)
				return std::unexpected("There is no active authored scene to capture");
			auto Serialized = PulseForge::SceneSerializer::Serialize(*m_Scene);
			if (!Serialized)
				return std::unexpected(Serialized.error().Message);

			std::string State = std::to_string(Serialized->size());
			State.push_back('\n');
			State.append(*Serialized);
			State.push_back('\n');
			if (m_SelectedEntity)
				State.append(m_SelectedEntity->ToString());
			State.push_back('\n');
			State.append(std::to_string(m_Selection.GetEntities().size()));
			State.push_back('\n');
			for (const PulseForge::UUID& Identifier : m_Selection.GetEntities())
			{
				State.append(Identifier.ToString());
				State.push_back('\n');
			}
			const auto AppendVisibilitySet = [&State](
				const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>& Identifiers)
			{
				std::vector<std::string> SortedIdentifiers;
				SortedIdentifiers.reserve(Identifiers.size());
				for (const PulseForge::UUID& Identifier : Identifiers)
					SortedIdentifiers.push_back(Identifier.ToString());
				std::sort(SortedIdentifiers.begin(), SortedIdentifiers.end());
				State.append(std::to_string(SortedIdentifiers.size()));
				State.push_back('\n');
				for (const std::string& Identifier : SortedIdentifiers)
				{
					State.append(Identifier);
					State.push_back('\n');
				}
			};
			AppendVisibilitySet(m_EditorVisibility.GetHiddenEntities());
			const auto IsolationBaseline = m_EditorVisibility.GetIsolationBaseline();
			State.append(IsolationBaseline ? "1\n" : "0\n");
			if (IsolationBaseline)
				AppendVisibilitySet(*IsolationBaseline);
			return State;
		}

		[[nodiscard]] std::expected<void, std::string> RestoreSceneState(std::string_view State)
		{
			if (!m_Scene || m_SceneRuntime)
				return std::unexpected("Scene history is available only for the authored scene in Edit mode");
			const size_t LengthTerminator = State.find('\n');
			if (LengthTerminator == std::string_view::npos)
				return std::unexpected("Scene history snapshot is malformed");
			size_t SceneLength = 0;
			const auto ParsedLength = std::from_chars(
				State.data(), State.data() + LengthTerminator, SceneLength);
			if (ParsedLength.ec != std::errc{} || ParsedLength.ptr != State.data() + LengthTerminator ||
				State.size() < LengthTerminator + 2 ||
				SceneLength > State.size() - LengthTerminator - 2)
				return std::unexpected("Scene history snapshot has an invalid payload length");
			const size_t SceneStart = LengthTerminator + 1;
			const std::string_view SceneData = State.substr(SceneStart, SceneLength);
			const size_t SelectionStart = SceneStart + SceneLength + 1;
			if (State[SelectionStart - 1] != '\n')
				return std::unexpected("Scene history snapshot has an invalid editor-state separator");
			const std::string_view EditorState = State.substr(SelectionStart);
			const size_t PrimaryTerminator = EditorState.find('\n');
			if (PrimaryTerminator == std::string_view::npos)
				return std::unexpected("Scene history snapshot has no selection primary separator");
			const std::string_view PrimaryData = EditorState.substr(0, PrimaryTerminator);
			const size_t CountStart = PrimaryTerminator + 1;
			const size_t CountTerminator = EditorState.find('\n', CountStart);
			if (CountTerminator == std::string_view::npos)
				return std::unexpected("Scene history snapshot has no selection count separator");
			size_t SelectionCount = 0;
			const auto ParsedCount = std::from_chars(
				EditorState.data() + CountStart, EditorState.data() + CountTerminator, SelectionCount);
			if (ParsedCount.ec != std::errc{} || ParsedCount.ptr != EditorState.data() + CountTerminator ||
				SelectionCount > (EditorState.size() - CountTerminator - 1) / 37)
				return std::unexpected("Scene history snapshot has an invalid selection count");
			std::vector<PulseForge::UUID> Selection;
			Selection.reserve(SelectionCount);
			size_t SelectionCursor = CountTerminator + 1;
			for (size_t Index = 0; Index < SelectionCount; ++Index)
			{
				const size_t IdentifierEnd = EditorState.find('\n', SelectionCursor);
				if (IdentifierEnd == std::string_view::npos)
					return std::unexpected("Scene history snapshot contains a truncated selection identifier");
				const auto Identifier = PulseForge::UUID::Parse(
					EditorState.substr(SelectionCursor, IdentifierEnd - SelectionCursor));
				if (!Identifier || std::find(Selection.begin(), Selection.end(), *Identifier) != Selection.end())
					return std::unexpected("Scene history snapshot contains an invalid or duplicate selection identifier");
				Selection.push_back(*Identifier);
				SelectionCursor = IdentifierEnd + 1;
			}
			const auto ParseVisibilitySet = [&](size_t& Cursor, std::string_view FieldName)
				-> std::expected<std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>, std::string>
			{
				const size_t CountEnd = EditorState.find('\n', Cursor);
				if (CountEnd == std::string_view::npos)
					return std::unexpected("Scene history snapshot has no " + std::string(FieldName) + " count");
				size_t Count = 0;
				const auto Parsed = std::from_chars(EditorState.data() + Cursor, EditorState.data() + CountEnd, Count);
				if (Parsed.ec != std::errc{} || Parsed.ptr != EditorState.data() + CountEnd ||
					Count > (EditorState.size() - CountEnd - 1) / 37)
					return std::unexpected("Scene history snapshot has an invalid " + std::string(FieldName) + " count");
				Cursor = CountEnd + 1;
				std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Identifiers;
				for (size_t Index = 0; Index < Count; ++Index)
				{
					const size_t IdentifierEnd = EditorState.find('\n', Cursor);
					if (IdentifierEnd == std::string_view::npos)
						return std::unexpected("Scene history snapshot has a truncated " + std::string(FieldName) + " identifier");
					const auto Identifier = PulseForge::UUID::Parse(EditorState.substr(Cursor, IdentifierEnd - Cursor));
					if (!Identifier || !Identifiers.insert(*Identifier).second)
						return std::unexpected("Scene history snapshot has an invalid or duplicate " +
							std::string(FieldName) + " identifier");
					Cursor = IdentifierEnd + 1;
				}
				return Identifiers;
			};
			auto HiddenEntities = ParseVisibilitySet(SelectionCursor, "hidden-entity");
			if (!HiddenEntities)
				return std::unexpected(HiddenEntities.error());
			const size_t IsolationFlagEnd = EditorState.find('\n', SelectionCursor);
			if (IsolationFlagEnd == std::string_view::npos)
				return std::unexpected("Scene history snapshot has no isolation-state flag");
			const std::string_view IsolationFlag = EditorState.substr(SelectionCursor, IsolationFlagEnd - SelectionCursor);
			if (IsolationFlag != "0" && IsolationFlag != "1")
				return std::unexpected("Scene history snapshot has an invalid isolation-state flag");
			SelectionCursor = IsolationFlagEnd + 1;
			std::optional<std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>> IsolationBaseline;
			if (IsolationFlag == "1")
			{
				auto ParsedBaseline = ParseVisibilitySet(SelectionCursor, "isolation-baseline");
				if (!ParsedBaseline)
					return std::unexpected(ParsedBaseline.error());
				IsolationBaseline = std::move(*ParsedBaseline);
			}
			if (SelectionCursor != EditorState.size())
				return std::unexpected("Scene history snapshot contains trailing editor-state data");
			std::optional<PulseForge::UUID> Primary;
			if (!PrimaryData.empty())
			{
				const auto ParsedPrimary = PulseForge::UUID::Parse(PrimaryData);
				if (!ParsedPrimary)
					return std::unexpected("Scene history snapshot contains an invalid selection primary");
				Primary = *ParsedPrimary;
				if (!Primary || std::find(Selection.begin(), Selection.end(), *Primary) == Selection.end())
					return std::unexpected("Scene history snapshot contains an invalid selection primary");
			}
			if ((Selection.empty() && Primary) || (!Selection.empty() && !Primary))
				return std::unexpected("Scene history snapshot selection primary does not match its selection");
			if (auto Result = PulseForge::SceneSerializer::Deserialize(SceneData, *m_Scene); !Result)
				return std::unexpected(Result.error().Message);
			std::erase_if(Selection, [this](const PulseForge::UUID& Identifier)
			{
				return !m_Scene->FindEntity(Identifier).has_value();
			});
			if (Primary && std::find(Selection.begin(), Selection.end(), *Primary) == Selection.end())
				Primary.reset();
			const auto RemoveMissingEntities = [this](
				std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>& Identifiers)
			{
				std::erase_if(Identifiers, [this](const PulseForge::UUID& Identifier)
				{
					return !m_Scene->FindEntity(Identifier).has_value();
				});
			};
			RemoveMissingEntities(*HiddenEntities);
			if (IsolationBaseline)
				RemoveMissingEntities(*IsolationBaseline);
			m_Selection.Set(std::move(Selection), Primary);
			m_SelectedEntity = m_Selection.GetPrimary();
			m_EditorVisibility.RestoreState(std::move(*HiddenEntities), std::move(IsolationBaseline));
			m_GizmoDrag.reset();
			m_InspectorHistoryItem = 0;
			UpdateSceneDirty();
			return {};
		}

		void SetEditorSelection(std::vector<PulseForge::UUID> Identifiers,
			std::optional<PulseForge::UUID> Primary = std::nullopt)
		{
			m_Selection.Set(std::move(Identifiers), Primary);
			m_SelectedEntity = m_Selection.GetPrimary();
		}

		void SelectSingleEntity(std::optional<PulseForge::UUID> Identifier)
		{
			if (Identifier)
			{
				m_Selection.SelectSingle(*Identifier);
				m_SelectedEntity = Identifier;
			}
			else
			{
				m_Selection.Clear();
				m_SelectedEntity.reset();
			}
		}

		[[nodiscard]] bool IsEntitySelected(const PulseForge::UUID& Identifier) const noexcept
		{
			return m_Selection.Contains(Identifier);
		}

		[[nodiscard]] bool IsEntityEffectivelyVisible(const PulseForge::Entity& Entity) const
		{
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Visited;
			PulseForge::Entity Current = Entity;
			while (Current)
			{
				if (!Visited.insert(Current.GetUUID()).second ||
					!m_EditorVisibility.IsLocallyVisible(Current.GetUUID()))
					return false;
				const auto Parent = Current.GetParent();
				if (!Parent || !Parent->has_value())
					return Parent.has_value();
				Current = **Parent;
			}
			return false;
		}

		[[nodiscard]] std::vector<PulseForge::UUID> GetEffectivelyHiddenEntities() const
		{
			std::vector<PulseForge::UUID> Hidden;
			if (!m_Scene)
				return Hidden;
			for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
				if (!IsEntityEffectivelyVisible(Entity))
					Hidden.push_back(Entity.GetUUID());
			return Hidden;
		}

		void ToggleEntitySelection(const PulseForge::UUID& Identifier)
		{
			m_Selection.Toggle(Identifier);
			m_SelectedEntity = m_Selection.GetPrimary();
		}

		void SelectHierarchyRange(const PulseForge::UUID& Identifier, bool AddToSelection)
		{
			(void)m_Selection.SelectRange(m_HierarchyVisibleOrder, Identifier, AddToSelection);
			m_SelectedEntity = m_Selection.GetPrimary();
		}

		[[nodiscard]] std::expected<std::vector<PulseForge::Entity>, std::string> GetTopLevelSelectedEntities() const
		{
			if (!m_Scene)
				return std::unexpected("There is no active scene");

			const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Selected(
				m_Selection.GetEntities().begin(), m_Selection.GetEntities().end());
			std::vector<PulseForge::Entity> Roots;
			for (const PulseForge::UUID& Identifier : m_Selection.GetEntities())
			{
				const auto Candidate = m_Scene->FindEntity(Identifier);
				if (!Candidate)
					continue;
				bool HasSelectedAncestor = false;
				auto Parent = Candidate->GetParent();
				while (Parent && Parent->has_value())
				{
					if (Selected.contains((**Parent).GetUUID()))
					{
						HasSelectedAncestor = true;
						break;
					}
					Parent = (**Parent).GetParent();
				}
				if (!Parent)
					return std::unexpected("Could not inspect the selected entity hierarchy: " + Parent.error().Message);
				if (!HasSelectedAncestor)
					Roots.push_back(*Candidate);
			}
			return Roots;
		}

		[[nodiscard]] std::expected<std::vector<PulseForge::Entity>, std::string> CollectEntitySubtrees(
			const std::vector<PulseForge::Entity>& Roots) const
		{
			std::vector<PulseForge::Entity> Subtree;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Visited;
			std::function<std::expected<void, std::string>(const PulseForge::Entity&)> Visit =
				[&](const PulseForge::Entity& Current) -> std::expected<void, std::string>
			{
				if (!Visited.insert(Current.GetUUID()).second)
					return {};
				Subtree.push_back(Current);
				auto Children = Current.GetChildren();
				if (!Children)
					return std::unexpected(Children.error().Message);
				for (const PulseForge::Entity& Child : *Children)
					if (auto Result = Visit(Child); !Result)
						return Result;
				return {};
			};
			for (const PulseForge::Entity& Root : Roots)
				if (auto Result = Visit(Root); !Result)
					return std::unexpected(Result.error());
			return Subtree;
		}

		[[nodiscard]] std::string MakeUniqueEntityName(
			std::string BaseName,
			std::optional<PulseForge::UUID> IgnoredEntity = std::nullopt) const
		{
			if (!m_Scene)
				return BaseName;
			std::unordered_set<std::string> ExistingNames;
			for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
				if ((!IgnoredEntity || Entity.GetUUID() != *IgnoredEntity))
					if (const auto Tag = Entity.GetTag())
						ExistingNames.insert(Tag->Name);
			if (!ExistingNames.contains(BaseName))
				return BaseName;
			for (uint32_t Suffix = 2; Suffix < (std::numeric_limits<uint32_t>::max)(); ++Suffix)
			{
				std::string Candidate = BaseName + " " + std::to_string(Suffix);
				if (!ExistingNames.contains(Candidate))
					return Candidate;
			}
			return BaseName;
		}

		void ToggleEntityVisibility(const PulseForge::UUID& Identifier)
		{
			const auto Entity = m_Scene ? m_Scene->FindEntity(Identifier) : std::nullopt;
			if (!Entity || m_SceneRuntime)
				return;
			const bool MakeVisible = !m_EditorVisibility.IsLocallyVisible(Identifier);
			const auto Recorded = RunSceneEdit(MakeVisible ? "Show Entity" : "Hide Entity", [this, Identifier, MakeVisible]()
				-> std::expected<void, PulseForge::SceneError>
			{
				m_EditorVisibility.SetVisible(Identifier, MakeVisible);
				return {};
			});
			if (!Recorded)
				SetError("Could not update editor visibility: " + Recorded.error());
		}

		void HideSelectedEntities()
		{
			if (!m_Scene || m_SceneRuntime || m_Selection.GetEntities().empty())
				return;
			const std::vector<PulseForge::UUID> Identifiers = m_Selection.GetEntities();
			const auto Recorded = RunSceneEdit("Hide Selected", [this, Identifiers]()
				-> std::expected<void, PulseForge::SceneError>
			{
				(void)m_EditorVisibility.Hide(Identifiers);
				return {};
			});
			if (!Recorded)
				SetError("Could not hide the selection: " + Recorded.error());
			else
				SetStatus("Hid " + std::to_string(Identifiers.size()) + " selected entities in the editor viewport.");
		}

		void ShowAllEntities()
		{
			if (!m_Scene || m_SceneRuntime || m_EditorVisibility.GetHiddenEntities().empty())
				return;
			const size_t RestoredCount = m_EditorVisibility.GetHiddenEntities().size();
			const auto Recorded = RunSceneEdit("Show All Entities", [this]() -> std::expected<void, PulseForge::SceneError>
			{
				m_EditorVisibility.ShowAll();
				return {};
			});
			if (!Recorded)
				SetError("Could not restore editor visibility: " + Recorded.error());
			else
				SetStatus("Restored visibility for " + std::to_string(RestoredCount) + " hidden entities.");
		}

		void IsolateSelectedEntities()
		{
			if (!m_Scene || m_SceneRuntime || m_Selection.GetEntities().empty())
				return;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> KeepVisible;
			for (const PulseForge::UUID& Identifier : m_Selection.GetEntities())
			{
				const auto Selected = m_Scene->FindEntity(Identifier);
				if (!Selected)
					continue;
				KeepVisible.insert(Identifier);
				PulseForge::Entity ParentWalk = *Selected;
				while (ParentWalk)
				{
					const auto Parent = ParentWalk.GetParent();
					if (!Parent || !Parent->has_value())
						break;
					ParentWalk = **Parent;
					KeepVisible.insert(ParentWalk.GetUUID());
				}
				if (const auto Subtree = CollectEntitySubtrees(std::vector<PulseForge::Entity>{ *Selected }))
					for (const PulseForge::Entity& Descendant : *Subtree)
						KeepVisible.insert(Descendant.GetUUID());
			}
			std::vector<PulseForge::UUID> Keep(KeepVisible.begin(), KeepVisible.end());
			std::vector<PulseForge::UUID> All;
			for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
				All.push_back(Entity.GetUUID());
			const auto Recorded = RunSceneEdit("Isolate Selection", [this, Keep, All]()
				-> std::expected<void, PulseForge::SceneError>
			{
				if (!m_EditorVisibility.Isolate(Keep, All))
					return std::unexpected(PulseForge::SceneError{
						PulseForge::SceneErrorCode::InvalidEntity, "The selection could not be isolated" });
				return {};
			});
			if (!Recorded)
				SetError("Could not isolate the selection: " + Recorded.error());
			else
				SetStatus("Isolated the selection in the editor viewport.");
		}

		void RestoreIsolatedVisibility()
		{
			if (!m_Scene || m_SceneRuntime || !m_EditorVisibility.GetIsolationBaseline())
				return;
			const auto Recorded = RunSceneEdit("Restore Isolated Visibility", [this]()
				-> std::expected<void, PulseForge::SceneError>
			{
				(void)m_EditorVisibility.RestoreIsolation();
				return {};
			});
			if (!Recorded)
				SetError("Could not restore the prior visibility state: " + Recorded.error());
			else
				SetStatus("Restored visibility before isolation.");
		}

		void UpdateSceneDirty()
		{
			if (!m_Scene)
			{
				m_SceneDirty = false;
				return;
			}
			auto Serialized = PulseForge::SceneSerializer::Serialize(*m_Scene);
			m_SceneDirty = !Serialized || !m_SavedSceneSnapshot || *Serialized != *m_SavedSceneSnapshot;
		}

		void ResetSceneHistory()
		{
			m_SceneHistory.Reset();
			m_InspectorHistoryItem = 0;
			m_EditorVisibility.ShowAll();
			m_SavedSceneSnapshot.reset();
			if (m_Scene)
				if (auto Serialized = PulseForge::SceneSerializer::Serialize(*m_Scene))
					m_SavedSceneSnapshot = std::move(*Serialized);
			UpdateSceneDirty();
		}

		void CancelSceneEdit()
		{
			if (m_SceneHistory.IsActive())
			{
				const auto Result = m_SceneHistory.Cancel(
					[this](std::string_view State) { return RestoreSceneState(State); });
				if (!Result)
					SetError("Scene edit rollback failed: " + Result.error());
			}
			m_InspectorHistoryItem = 0;
			UpdateSceneDirty();
		}

		bool BeginSceneEdit(std::string Description)
		{
			if (m_SceneHistory.IsActive())
				FinishSceneEdit();
			if (!m_Scene || m_SceneRuntime)
				return false;
			auto Before = CaptureSceneState();
			if (!Before)
			{
				SetError("Could not begin the scene edit: " + Before.error());
				return false;
			}
			if (auto Begun = m_SceneHistory.Begin(std::move(Description), std::move(*Before)); !Begun)
			{
				SetError("Could not begin the scene edit: " + Begun.error());
				return false;
			}
			m_InspectorHistoryItem = PulseForgeEditor::NoInspectorHistoryItem;
			return true;
		}

		void MarkSceneSaved()
		{
			m_SceneHistory.MarkSaved();
			if (m_Scene)
				if (auto Serialized = PulseForge::SceneSerializer::Serialize(*m_Scene))
					m_SavedSceneSnapshot = std::move(*Serialized);
			UpdateSceneDirty();
		}

		void FinishSceneEdit()
		{
			if (!m_SceneHistory.IsActive())
			{
				m_InspectorHistoryItem = 0;
				return;
			}

			auto After = CaptureSceneState();
			if (!After)
			{
				const std::string Error = After.error();
				(void)m_SceneHistory.Cancel([this](std::string_view State) { return RestoreSceneState(State); });
				m_InspectorHistoryItem = 0;
				SetError("Could not finish the scene edit transaction: " + Error);
				return;
			}

			const auto Committed = m_SceneHistory.Commit(std::move(*After));
			if (!Committed)
			{
				const std::string Error = Committed.error();
				(void)m_SceneHistory.Cancel([this](std::string_view State) { return RestoreSceneState(State); });
				m_InspectorHistoryItem = 0;
				SetError("Scene edit was rolled back because it could not be stored in history: " + Error);
				return;
			}
			m_InspectorHistoryItem = 0;
			UpdateSceneDirty();
		}

		template<typename Action>
		[[nodiscard]] std::expected<void, std::string> RunSceneEdit(std::string Description, Action&& Operation)
		{
			if (!m_Scene || m_SceneRuntime)
				return std::unexpected("Scene editing is unavailable while the runtime scene is active");

			const uint32_t ActiveItem = GImGui ? GImGui->ActiveId : 0;
			if (m_SceneHistory.IsActive() && m_InspectorHistoryItem != ActiveItem)
				FinishSceneEdit();
			if (!m_SceneHistory.IsActive())
			{
				auto Before = CaptureSceneState();
				if (!Before)
					return std::unexpected("Could not begin the scene edit: " + Before.error());
				if (auto Begun = m_SceneHistory.Begin(std::move(Description), std::move(*Before)); !Begun)
					return std::unexpected(Begun.error());
				m_InspectorHistoryItem = ActiveItem;
			}

			auto Result = std::forward<Action>(Operation)();
			if (!Result)
			{
				const std::string Error = Result.error().Message;
				(void)m_SceneHistory.Cancel([this](std::string_view State) { return RestoreSceneState(State); });
				m_InspectorHistoryItem = 0;
				UpdateSceneDirty();
				return std::unexpected(Error);
			}

			if (ActiveItem == 0 || !GImGui || GImGui->ActiveId != ActiveItem)
				FinishSceneEdit();
			else
				m_SceneDirty = true;
			return {};
		}

		void UndoSceneEdit()
		{
			if (!m_Scene || m_SceneRuntime)
				return;
			FinishSceneEdit();
			const auto Result = m_SceneHistory.Undo(
				[this](std::string_view State) { return RestoreSceneState(State); });
			if (!Result)
			{
				SetError("Undo failed: " + Result.error());
				return;
			}
			if (*Result)
				SetStatus("Undid the last scene edit.");
			UpdateSceneDirty();
		}

		void RedoSceneEdit()
		{
			if (!m_Scene || m_SceneRuntime)
				return;
			FinishSceneEdit();
			const auto Result = m_SceneHistory.Redo(
				[this](std::string_view State) { return RestoreSceneState(State); });
			if (!Result)
			{
				SetError("Redo failed: " + Result.error());
				return;
			}
			if (*Result)
				SetStatus("Redid the last scene edit.");
			UpdateSceneDirty();
		}

		void HandleEditorHistoryShortcuts()
		{
			const ImGuiIO& IO = ImGui::GetIO();
			if (!PulseForgeEditor::CanRouteEditorHistoryShortcut({
				.SceneAvailable = m_Scene != nullptr,
				.RuntimeActive = m_SceneRuntime != nullptr,
				.CameraNavigationActive = m_EditorCameraNavigationActive,
				.GizmoInteractionActive = m_GizmoDrag.has_value(),
				.TextInputActive = IO.WantTextInput,
				.ImGuiItemActive = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui && GImGui->NavLayer == ImGuiNavLayer_Menu,
				.WindowNavigationActive = GImGui && GImGui->NavWindowingTarget != nullptr
			}))
				return;

			if (!IO.KeyCtrl || IO.KeyAlt || IO.KeySuper)
				return;
			if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
				RedoSceneEdit();
			else if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && IO.KeyShift)
				RedoSceneEdit();
			else if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
				UndoSceneEdit();
		}

		[[nodiscard]] bool IsContentBrowserFocused() const noexcept
		{
			if (!GImGui || !GImGui->NavWindow)
				return false;
			const ImGuiWindow* Root = GImGui->NavWindow->RootWindow;
			return Root && std::string_view(Root->Name) ==
				PulseForgeEditor::EditorPanelDescriptors[
					static_cast<size_t>(PulseForgeEditor::EditorPanel::ContentBrowser)].WindowName;
		}

		[[nodiscard]] bool IsEditorPanelFocused(PulseForgeEditor::EditorPanel Panel) const noexcept
		{
			if (!GImGui || !GImGui->NavWindow)
				return false;
			const ImGuiWindow* Root = GImGui->NavWindow->RootWindow;
			return Root && std::string_view(Root->Name) ==
				PulseForgeEditor::EditorPanelDescriptors[static_cast<size_t>(Panel)].WindowName;
		}

		void FocusActivePanelSearch()
		{
			if (IsContentBrowserFocused())
			{
				SetEditorPanelVisible(PulseForgeEditor::EditorPanel::ContentBrowser, true);
				m_FocusContentSearchNextFrame = true;
			}
			else
			{
				SetEditorPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy, true);
				m_FocusHierarchySearchNextFrame = true;
			}
		}

		[[nodiscard]] bool CanRouteEditorShortcut() const
		{
			return m_Scene && !m_SceneRuntime && !m_EditorCameraNavigationActive && !m_GizmoDrag &&
				!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
				!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
				GImGui->NavLayer != ImGuiNavLayer_Menu && GImGui->NavWindowingTarget == nullptr;
		}

		void HandleSceneEditingShortcuts()
		{
			const ImGuiIO& IO = ImGui::GetIO();
			const bool CanFocusSearch = m_Project && !m_SceneRuntime && !m_EditorCameraNavigationActive && !m_GizmoDrag &&
				!IO.WantTextInput && !ImGui::IsAnyItemActive() &&
				!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
				GImGui->NavLayer != ImGuiNavLayer_Menu && GImGui->NavWindowingTarget == nullptr &&
				!IO.KeyAlt && !IO.KeySuper;
			if (CanFocusSearch && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
			{
				FocusActivePanelSearch();
				return;
			}
			if (!CanRouteEditorShortcut())
				return;
			const bool HasUnsupportedModifier = IO.KeyAlt || IO.KeySuper;
			const bool Control = IO.KeyCtrl && !HasUnsupportedModifier;
			const bool Shift = IO.KeyShift;
			const bool ContentBrowser = IsContentBrowserFocused();

			if (Control)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !Shift && !ContentBrowser)
					ShowAllEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !ContentBrowser)
				{
					std::vector<PulseForge::UUID> Identifiers;
					for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
						Identifiers.push_back(Entity.GetUUID());
					SetEditorSelection(std::move(Identifiers));
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !ContentBrowser)
					(void)CopySelectedEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_X, false) && !ContentBrowser)
					CutSelectedEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_V, false) && !ContentBrowser)
					PasteEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_D, false))
				{
					if (ContentBrowser)
					{
						if (m_SelectedAsset)
							DuplicateAsset(*m_SelectedAsset);
					}
					else
						DuplicateSelectedEntity();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
				{
					if (Shift)
						CreateScene(true);
					else
						SaveScene();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_N, false) && Shift)
					CreateEntityFromEditor();
			}
			else if (!HasUnsupportedModifier)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
				{
					if (ContentBrowser)
					{
						if (m_SelectedAsset)
							RequestDeleteAsset(*m_SelectedAsset);
					}
					else
						DeleteSelectedEntity();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && ContentBrowser && m_SelectedAsset)
					MoveAsset(*m_SelectedAsset);
				else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && m_SelectedEntity && !ContentBrowser)
				{
					m_FocusEntityNameNextFrame = true;
					SetEditorPanelVisible(PulseForgeEditor::EditorPanel::Inspector, true);
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_F, false))
					FocusSelection();
				else if (ImGui::IsKeyPressed(ImGuiKey_End, false) && !ContentBrowser)
					SnapSelectionToSurface();
				else if (ImGui::IsKeyPressed(ImGuiKey_H, false))
				{
					if (Shift)
						IsolateSelectedEntities();
					else
						HideSelectedEntities();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_G, false))
				{
					m_ShowEditorOverlays = !m_ShowEditorOverlays;
					SetStatus(m_ShowEditorOverlays ? "Editor viewport overlays enabled." : "Game View enabled.");
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
					SelectSingleEntity(std::nullopt);
			}
		}

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
				ResetViewportSceneRenderer();
				m_Project.emplace(std::move(*Created));
				ClearScene();
				ResetContentBrowserNavigation();
				UpdateAssetList();
				SetError("Project was created, but its initial scene could not be created: " + MainScene.error().Message);
				return;
			}

			const PulseForge::AssetRecord MainSceneRecord = MainScene
				? *MainScene
				: *MainScene.error().CommittedAsset;
			StopRuntime();
			ResetViewportSceneRenderer();
			m_Project.emplace(std::move(*Created));
			ResetContentBrowserNavigation();
			m_Scene = std::move(NewScene);
			m_SceneAsset = MainSceneRecord.ID;
			SelectSingleEntity(std::nullopt);
			m_SelectedAsset = MainSceneRecord.ID;
			m_PendingDeleteAsset.reset();
			m_OpenDeleteAssetDialog = false;
			ResetSceneHistory();
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
			QueueAfterSave([this, ChosenPath] { (void)OpenProjectAt(ChosenPath); });
		}

		std::expected<void, std::string> OpenProjectAt(const std::filesystem::path& ProjectFile)
		{
			m_ProjectOpenStartedAt = std::chrono::steady_clock::now();
			m_ProjectOpenTimingActive = true;
			m_LoggedFirstViewportPrepared = false;
			m_LoggedFirstViewportRendered = false;
			m_LoggedEnvironmentReady = false;
			m_EnvironmentLightingPending = false;
			PulseForge::Detail::ScopedProfileTimer OpenTimer("Editor project open + startup scene load");
			auto Opened = PulseForgeEditor::OpenEditorProject(ProjectFile);
			if (!Opened)
			{
				SetError(Opened.error().Message);
				return std::unexpected(Opened.error().Message);
			}

			const std::string ProjectName = Opened->Project.GetDescription().Name;
			StopRuntime();
			ResetViewportSceneRenderer();
			m_Project.emplace(std::move(Opened->Project));
			m_OpenedMaterialAsset.reset();
			m_FocusMaterialEditorNextFrame = false;
			m_InitialMaterialEditorDockPending = false;
			ResetContentBrowserNavigation();
			m_Scene = std::move(Opened->StartupScene);
			m_SceneAsset = Opened->StartupSceneAsset;
			SelectSingleEntity(std::nullopt);
			m_SelectedAsset = Opened->StartupSceneAsset;
			m_PendingDeleteAsset.reset();
			m_OpenDeleteAssetDialog = false;
			ResetSceneHistory();
			UpdateAssetList();
			if (Opened->StartupSceneWarning.empty())
				SetStatus("Opened project " + ProjectName + ".");
			else
				SetWarning(std::move(Opened->StartupSceneWarning));
			return {};
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

		void MoveFolderAt(
			const std::filesystem::path& Source,
			const std::filesystem::path& DestinationParent)
		{
			if (!m_Project)
				return;
			auto Moved = PulseForge::AssetOperations::MoveFolder(
				m_Project->GetAssetRegistry(),
				m_Project->GetRootPath(),
				Source,
				DestinationParent);
			if (!Moved)
			{
				std::string Message = DescribeAssetOperationError("Folder move failed", Moved.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			const auto RebaseFolder = [&Source, &Moved](const std::filesystem::path& Folder)
				-> std::optional<std::filesystem::path>
			{
				if (Folder == Source)
					return *Moved;
				const std::filesystem::path Relative = Folder.lexically_relative(Source);
				if (Relative.empty() || Relative.is_absolute() ||
					(Relative.begin() != Relative.end() && *Relative.begin() == ".."))
				{
					return std::nullopt;
				}
				return *Moved / Relative;
			};
			if (const auto Rebased = RebaseFolder(m_CurrentContentFolder))
				m_CurrentContentFolder = *Rebased;
			if (m_SelectedContentFolder)
				if (const auto Rebased = RebaseFolder(*m_SelectedContentFolder))
					m_SelectedContentFolder = *Rebased;

			UpdateAssetList();
			SetStatus("Moved folder to " + PathToUtf8(*Moved) + ". Asset identities were preserved.");
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

			std::optional<PulseForge::UUID> InstantiatedIdentifier;
			const auto Recorded = RunSceneEdit("Instantiate Prefab", [this, Identifier, &InstantiatedIdentifier]
				-> std::expected<void, PulseForge::PrefabAssetError>
			{
				auto Instantiated = PulseForge::PrefabAssetService::Instantiate(
					Identifier,
					m_Project->GetRootPath(),
					m_Project->GetAssetRegistry(),
					*m_Scene);
				if (!Instantiated)
					return std::unexpected(Instantiated.error());
				InstantiatedIdentifier = Instantiated->GetUUID();
				SelectSingleEntity(InstantiatedIdentifier);
				return {};
			});
			if (!Recorded)
			{
				SetError("Prefab instantiation failed: " + Recorded.error());
				return;
			}
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
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(
					PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingX,
					PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingY));
				const auto Asset = m_Project && m_PendingDeleteAsset
					? m_Project->GetAssetRegistry().Find(*m_PendingDeleteAsset)
					: std::optional<PulseForge::AssetRecord>{};
				if (Asset)
					ImGui::TextWrapped("Permanently delete %s and its sidecar metadata?", PathToUtf8(Asset->ProjectRelativePath).c_str());
				else
					ImGui::TextUnformatted("The selected asset is no longer registered.");
				ImGui::TextUnformatted("This cannot be undone and may leave UUID references unresolved.");

				if (Asset && m_EditorStyle.DangerButton(PulseForgeEditor::EditorIcon::Delete, "Delete"))
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
				ImGui::PopStyleVar();
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
			if (m_OpenedMaterialAsset && *m_OpenedMaterialAsset == Identifier)
			{
				ForgetMaterialDraft(Identifier);
				m_OpenedMaterialAsset.reset();
			}
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
			{
				ResetEditorViewportCamera();
				m_Scene = std::move(NewScene);
			}
			m_SceneAsset = Record.ID;
			SelectSingleEntity(std::nullopt);
			if (SaveAs)
				MarkSceneSaved();
			else
				ResetSceneHistory();
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
			ResetEditorViewportCamera();
			m_Scene = std::move(Loaded);
			m_SceneAsset = Identifier;
			m_SelectedAsset = Identifier;
			SelectSingleEntity(std::nullopt);
			ResetSceneHistory();
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
			FinishSceneEdit();
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
				MarkSceneSaved();
				SetStatus("Scene saved.");
			}
			return true;
		}

		void QueueAfterSave(std::function<void()> Action)
		{
			FinishSceneEdit();
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
			ResetEditorViewportCamera();
			m_Scene = std::move(ReloadedScene);
			SelectSingleEntity(std::nullopt);
			ResetSceneHistory();
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
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(
					PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingX,
					PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingY));
				ImGui::TextUnformatted("The current scene has unsaved changes.");
				ImGui::TextUnformatted("Save before continuing?");
				if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
				{
					if (SaveScene())
						ExecutePendingAction();
				}
				ImGui::SameLine();
				if (m_EditorStyle.DangerButton(PulseForgeEditor::EditorIcon::Delete, "Discard"))
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
				ImGui::PopStyleVar();
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
			ResetViewportSceneRenderer();
			m_AssetPreviewProjectPending = true;
			m_Project.reset();
			m_OpenedMaterialAsset.reset();
			m_FocusMaterialEditorNextFrame = false;
			m_InitialMaterialEditorDockPending = false;
			m_Assets.clear();
			m_AssetBrowserPresentations.clear();
			ResetContentBrowserNavigation();
			RebuildContentFolderTree();
			SetStatus("Project closed.");
		}

		void DrawMainMenu()
		{
			if (!ImGui::BeginMainMenuBar())
				return;
			if (ImGui::BeginMenu("File"))
			{
				if (m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::NewDocument, "New Project..."))
					CreateProject();
				if (m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::FolderOpen, "Open Project..."))
					OpenProject();
				if (m_Project && m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::Close, "Close Project"))
					QueueAfterSave([this] { CloseProjectNow(); });
				ImGui::Separator();
				if (m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::Close, "Exit"))
					QueueAfterSave([] { PulseForge::Application::Get().RequestClose(); });
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Edit", m_Scene != nullptr))
			{
				const auto UndoCommand = m_SceneHistory.GetUndoCommand();
				const auto RedoCommand = m_SceneHistory.GetRedoCommand();
				const bool CanEditScene = m_Scene != nullptr && m_SceneRuntime == nullptr;
				const bool HasSelection = !m_Selection.GetEntities().empty();
				const std::string UndoLabel = UndoCommand
					? "Undo " + std::string(UndoCommand->Description)
					: "Undo";
				const std::string RedoLabel = RedoCommand
					? "Redo " + std::string(RedoCommand->Description)
					: "Redo";
				if (ImGui::MenuItem(UndoLabel.c_str(), "Ctrl+Z", false, CanEditScene && m_SceneHistory.CanUndo()))
					UndoSceneEdit();
				if (ImGui::MenuItem(RedoLabel.c_str(), "Ctrl+Y / Ctrl+Shift+Z", false,
					CanEditScene && m_SceneHistory.CanRedo()))
					RedoSceneEdit();
				ImGui::Separator();
				if (ImGui::MenuItem("Cut", "Ctrl+X", false, CanEditScene && HasSelection))
					CutSelectedEntities();
				if (ImGui::MenuItem("Copy", "Ctrl+C", false, CanEditScene && HasSelection))
					(void)CopySelectedEntities();
				const bool CanPasteEntities = CanEditScene && m_EntityClipboard && m_Project &&
					m_EntityClipboard->Project == m_Project->GetDescription().ID;
				if (ImGui::MenuItem("Paste", "Ctrl+V", false, CanPasteEntities))
					PasteEntities();
				if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, CanEditScene && HasSelection))
					DuplicateSelectedEntity();
				if (ImGui::MenuItem("Delete", "Delete", false, CanEditScene && HasSelection))
					DeleteSelectedEntity();
				ImGui::Separator();
				if (ImGui::MenuItem("Select All", "Ctrl+A", false, CanEditScene && m_Scene->GetEntityCount() > 0))
				{
					std::vector<PulseForge::UUID> Identifiers;
					for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
						Identifiers.push_back(Entity.GetUUID());
					SetEditorSelection(std::move(Identifiers));
				}
				if (ImGui::MenuItem("Rename", "F2", false, CanEditScene && m_SelectedEntity.has_value()))
				{
					m_FocusEntityNameNextFrame = true;
					SetEditorPanelVisible(PulseForgeEditor::EditorPanel::Inspector, true);
				}
				if (ImGui::MenuItem("Focus Selected", "F", false, CanEditScene && HasSelection))
					FocusSelection();
				if (ImGui::MenuItem("Snap to Supporting Surface", "End", false,
					CanEditScene && HasSelection))
					SnapSelectionToSurface();
				if (ImGui::MenuItem("Hide Selected", "H", false, CanEditScene && HasSelection))
					HideSelectedEntities();
				if (ImGui::MenuItem("Show All", "Ctrl+H", false,
					CanEditScene && !m_EditorVisibility.GetHiddenEntities().empty()))
					ShowAllEntities();
				if (ImGui::MenuItem("Isolate Selection", "Shift+H", false, CanEditScene && HasSelection))
					IsolateSelectedEntities();
				if (ImGui::MenuItem("Restore Isolation Visibility", nullptr, false,
					CanEditScene && m_EditorVisibility.GetIsolationBaseline().has_value()))
					RestoreIsolatedVisibility();
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Scene", m_Project.has_value()))
			{
				if (m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::NewDocument, "New Scene..."))
					CreateScene(false);
				if (m_EditorStyle.MenuItem(
					PulseForgeEditor::EditorIcon::Save, "Save Scene", nullptr, false, m_Scene && m_SceneAsset))
					SaveScene();
				if (m_EditorStyle.MenuItem(
					PulseForgeEditor::EditorIcon::Save, "Save Scene As...", nullptr, false, m_Scene != nullptr))
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

			if (ImGui::BeginMenu("Window"))
			{
				for (const PulseForgeEditor::EditorPanelDescriptor& Descriptor :
					PulseForgeEditor::EditorPanelDescriptors)
				{
					const bool IsVisible = m_Layout.IsPanelVisible(Descriptor.Panel);
					if (ImGui::MenuItem(Descriptor.WindowName.data(), nullptr, IsVisible))
						SetEditorPanelVisible(Descriptor.Panel, !IsVisible);
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Reset Layout"))
					m_Layout.RequestReset();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Help"))
			{
				if (ImGui::BeginMenu("Keyboard Shortcuts"))
				{
					ImGui::MenuItem("Select / Move / Rotate / Scale", "Q / W / E / R", false, false);
					ImGui::MenuItem("Focus selection / Snap to surface", "F / End", false, false);
					ImGui::MenuItem("Undo / Redo", "Ctrl+Z / Ctrl+Y", false, false);
					ImGui::MenuItem("Copy / Cut / Paste / Duplicate", "Ctrl+C / X / V / D", false, false);
					ImGui::MenuItem("Delete / Rename / Create entity", "Delete / F2 / Ctrl+Shift+N", false, false);
					ImGui::MenuItem("Hide / Isolate / Show all", "H / Shift+H / Ctrl+H", false, false);
					ImGui::MenuItem("Save / Save Scene As", "Ctrl+S / Ctrl+Shift+S", false, false);
					ImGui::MenuItem("Viewport orbit / pan / dolly", "Alt+LMB / MMB / RMB drag", false, false);
					ImGui::MenuItem("Duplicate while moving or rotating", "Alt+gizmo drag", false, false);
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		void DrawEditorToolbar()
		{
			constexpr float Height = 60.0f;
			if (!BeginEditorChromeBar("##PulseForgeGlobalToolbar", ImGuiDir_Up, Height,
				PulseForgeEditor::EditorColorToken::PanelRaised, m_EditorStyle))
				return;

			const float ContentWidth = ImGui::GetContentRegionAvail().x;
			if (ImGui::BeginTable("GlobalToolbarLayout", 3,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("Tools", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				const float RuntimeColumnWidth = ContentWidth < 720.0f ? 136.0f : 188.0f;
				ImGui::TableSetupColumn("Runtime", ImGuiTableColumnFlags_WidthFixed, RuntimeColumnWidth);
				ImGui::TableSetupColumn("Save", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableNextRow(ImGuiTableRowFlags_None, 50.0f);

				ImGui::TableSetColumnIndex(0);
				if (m_Project)
				{
					const std::string& ProjectName = m_Project->GetDescription().Name;
					const float ContextWidth = ImGui::GetContentRegionAvail().x;
					const float ProjectWidth = (std::max)(36.0f, (ContextWidth - 44.0f) * 0.48f);
					const float SceneWidth = (std::max)(36.0f, ContextWidth - ProjectWidth - 44.0f);
					const std::string ProjectLabel = FitEditorContextLabel(ProjectName, ProjectWidth);
					ImGui::AlignTextToFramePadding();
					{
						[[maybe_unused]] const auto Emphasis = m_EditorStyle.PushEmphasisFont();
						ImGui::TextUnformatted(ProjectLabel.c_str());
					}
					if (ProjectLabel != ProjectName && ImGui::IsItemHovered())
						ImGui::SetTooltip("Project: %s", ProjectName.c_str());
					ImGui::SameLine();
					m_EditorStyle.TextMuted("/");
					ImGui::SameLine();
					std::string SceneName = "No scene";
					if (m_SceneAsset)
						if (const auto Record = m_Project->GetAssetRegistry().Find(*m_SceneAsset))
							SceneName = PathToUtf8(Record->ProjectRelativePath.filename());
					if (m_SceneDirty)
						SceneName.push_back('*');
					const std::string SceneLabel = FitEditorContextLabel(SceneName, SceneWidth);
					ImGui::TextUnformatted(SceneLabel.c_str());
					if (SceneLabel != SceneName && ImGui::IsItemHovered())
						ImGui::SetTooltip("Scene: %s%s", SceneName.c_str(), m_SceneDirty ? " (unsaved changes)" : "");
					ImGui::SameLine(0.0f, 12.0f);
					ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
					ImGui::SameLine(0.0f, 10.0f);
				}
				ImGui::TableSetColumnIndex(1);
				const float RuntimeWidth = m_SceneRuntime ? 116.0f : 112.0f;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
				(std::max)(0.0f, (ImGui::GetContentRegionAvail().x - RuntimeWidth) * 0.5f));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24.0f, 8.0f));
				if (!m_SceneRuntime)
				{
					ImGui::BeginDisabled(!m_Project || !m_Scene);
					if (m_EditorStyle.AccentButton(PulseForgeEditor::EditorIcon::Play, "Play"))
						StartRuntime();
					ImGui::EndDisabled();
				}
				else if (m_EditorStyle.DangerButton(PulseForgeEditor::EditorIcon::Stop, "Stop"))
				{
					StopRuntime();
					SetStatus("Runtime stopped. The authored scene was not modified by simulation.");
				}
				ImGui::PopStyleVar();

				ImGui::TableSetColumnIndex(2);
				const float SaveWidth = 42.0f;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
					(std::max)(0.0f, ImGui::GetContentRegionAvail().x - SaveWidth));
				ImGui::BeginDisabled(!m_Scene || !m_SceneAsset || m_SceneRuntime);
				if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Save, "Save", "GlobalSaveScene",
					"Save the current scene", false, SaveWidth, 40.0f))
					SaveScene();
				ImGui::EndDisabled();
				ImGui::EndTable();
			}
			EndEditorChromeBar();
		}

		void UpdateWindowTitle()
		{
			std::string Title = "PulseForge";
			if (m_SceneAsset && m_Project)
			{
				if (const auto SceneRecord = m_Project->GetAssetRegistry().Find(*m_SceneAsset))
				{
					Title = PathToUtf8(SceneRecord->ProjectRelativePath.filename());
					if (m_SceneDirty)
						Title.push_back('*');
					Title += " - PulseForge Editor";
				}
			}
			else if (m_Project)
				Title = m_Project->GetDescription().Name + " - PulseForge Editor";

			if (Title == m_WindowTitle)
				return;
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			if (Window)
				glfwSetWindowTitle(Window, Title.c_str());
			m_WindowTitle = std::move(Title);
		}

		void DrawStatusBar()
		{
			if (!BeginEditorChromeBar("##PulseForgeStatusBar", ImGuiDir_Down, 28.0f,
				PulseForgeEditor::EditorColorToken::MenuBar, m_EditorStyle))
				return;

			if (ImGui::BeginTable("GlobalStatusLayout", 2,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 276.0f);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const ImVec4 MessageColor = m_StatusIsError
					? m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error)
					: m_StatusIsWarning
						? m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning)
						: m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::TextMuted);
				const float MessageWidth = ImGui::GetContentRegionAvail().x;
				ImGui::TextColored(MessageColor, "%s", m_StatusMessage.c_str());
				if (ImGui::IsItemHovered() && ImGui::CalcTextSize(m_StatusMessage.c_str()).x > MessageWidth)
					ImGui::SetTooltip("%s", m_StatusMessage.c_str());

				ImGui::TableSetColumnIndex(1);
				const bool ShowDirtyState = m_Scene && m_SceneDirty;
				if (ShowDirtyState)
					ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning), "Unsaved scene");
				else
					m_EditorStyle.TextMuted(!m_Scene ? "No scene" : "Scene saved");
				if (m_SceneRuntime)
				{
					ImGui::SameLine();
					m_EditorStyle.TextMuted("|");
					ImGui::SameLine();
					m_EditorStyle.TextMuted("Runtime");
				}
				ImGui::SameLine();
				m_EditorStyle.TextMuted("|");
				ImGui::SameLine();
				m_EditorStyle.TextMuted(PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan
					? "Vulkan" : "OpenGL");
				ImGui::EndTable();
			}
			EndEditorChromeBar();
		}

		void DrawWorkspacePanels()
		{
			if (m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Scene))
			{
				if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Scene))
				{
					if (m_Project)
					{
						(void)m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::FolderOpen, "Project");
						ImGui::TextUnformatted(m_Project->GetDescription().Name.c_str());
						if (m_Scene && m_SceneAsset)
						{
							const auto Record = m_Project->GetAssetRegistry().Find(*m_SceneAsset);
							const std::string SceneName = Record
								? PathToUtf8(Record->ProjectRelativePath)
								: std::string("<unregistered>");
							(void)m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Scene, "Scene");
							ImGui::TextUnformatted(SceneName.c_str());
							ImGui::SameLine();
							m_EditorStyle.TextMuted(m_SceneDirty ? "Unsaved" : "Saved");
							ImGui::Text("Entities  %zu", m_Scene->GetEntityCount());
							if (m_SceneRuntime && m_RuntimeScene)
							{
								(void)m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Play, "Runtime");
								ImGui::Text("Playing  |  %zu entities", m_RuntimeScene->GetEntityCount());
								m_EditorStyle.TextMuted("Isolated scene copy; the authored scene is unchanged by simulation.");
							}
						}
						else
							m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Scene,
								"No scene is open. Create or open one from the Scene menu.");
					}
					else
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::FolderOpen,
							"Create or open a project from the File menu.");

					ImGui::Separator();
					const ImVec4 Color = m_StatusIsError
						? m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error)
						: m_StatusIsWarning
							? m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning)
							: m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Text);
					ImGui::PushStyleColor(ImGuiCol_Text, Color);
					ImGui::TextWrapped("%s", m_StatusMessage.c_str());
					ImGui::PopStyleColor();
				}
				ImGui::End();
			}
			DrawUnsavedChangesDialog();

			DrawSceneViewportPanel();
			DrawHierarchyPanel();
			DrawInspectorPanel();
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!m_OpenGLDiagnosticCapturePath.empty() && !m_OpenGLDiagnosticContentBrowserSelected)
				ImGui::SetNextWindowFocus();
			#endif
			DrawContentBrowserPanel();
			if (m_InitialMaterialEditorDockPending)
			{
				(void)m_Layout.DockPanelBesideIfUnplaced(
					PulseForgeEditor::EditorPanel::MaterialEditor,
					PulseForgeEditor::EditorPanel::Inspector);
				m_InitialMaterialEditorDockPending = false;
			}
			DrawMaterialEditorPanel();
			DrawDeleteAssetDialog();
			DrawConsolePanel();
		}

		bool BeginEditorPanel(PulseForgeEditor::EditorPanel Panel)
		{
			bool* IsOpen = m_Layout.GetPanelVisibility(Panel);
			if (!IsOpen || !*IsOpen)
				return false;

			const bool WasOpen = *IsOpen;
			const bool ContentsVisible = ImGui::Begin(
				PulseForgeEditor::EditorPanelDescriptors[static_cast<size_t>(Panel)].WindowName.data(),
				IsOpen);
			if (Panel != PulseForgeEditor::EditorPanel::SceneViewport &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
					m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::OtherEditorPanelClicked);
			if (WasOpen != *IsOpen)
			{
				m_Layout.MarkSettingsDirty();
				HandleEditorPanelVisibilityChange(Panel);
			}
			return ContentsVisible;
		}

		void SetEditorPanelVisible(PulseForgeEditor::EditorPanel Panel, bool Visible)
		{
			if (Panel == PulseForgeEditor::EditorPanel::MaterialEditor && Visible)
			{
				m_InitialMaterialEditorDockPending = true;
				m_FocusMaterialEditorNextFrame = true;
			}
			if (m_Layout.SetPanelVisible(Panel, Visible))
				HandleEditorPanelVisibilityChange(Panel);
		}

		void HandleEditorPanelVisibilityChange(PulseForgeEditor::EditorPanel Panel)
		{
			if (Panel != PulseForgeEditor::EditorPanel::SceneViewport ||
				m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport))
				return;

			ResetViewportPanelInteraction();
		}

		void ResetViewportPanelInteraction()
		{
			EndEditorCameraNavigation();
			CancelGizmoInteraction();
			m_ViewportCameraGesture = ViewportCameraGesture::None;
			m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
				m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::ViewportUnavailable);
			m_ViewportImageHovered = false;
			m_ViewportImageRect = {};
		}

		void CancelViewportInteractionForLayoutChange()
		{
			ResetViewportPanelInteraction();
		}

		void ReportLayoutPersistenceError(const std::string& Message)
		{
			if (m_LayoutPersistenceError == Message)
				return;
			m_LayoutPersistenceError = Message;
			PF_WARN("Editor workspace persistence failed: {}", Message);
		}

		void DrawViewportOrientationWidget(ImDrawList* DrawList, ImVec2 ImageMinimum, ImVec2 ImageMaximum) const
		{
			const auto Axes = PulseForgeEditor::ProjectViewportOrientationAxes(m_EditorCamera.GetForward());
			if (!Axes)
				return;

			const ImVec2 Center(ImageMaximum.x - 51.0f, ImageMinimum.y + 78.0f);
			constexpr float AxisLength = 25.0f;
			DrawList->AddCircle(Center, 29.0f,
				ImGui::GetColorU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Border)), 24, 1.0f);

			constexpr std::array<const char*, 3> Labels = { "X", "Y", "Z" };
			constexpr std::array<PulseForgeEditor::EditorColorToken, 3> Tokens = {
				PulseForgeEditor::EditorColorToken::GizmoAxisX,
				PulseForgeEditor::EditorColorToken::GizmoAxisY,
				PulseForgeEditor::EditorColorToken::GizmoAxisZ
			};
			std::array<size_t, 3> DrawOrder = { 0, 1, 2 };
			std::sort(DrawOrder.begin(), DrawOrder.end(), [&](size_t Left, size_t Right)
			{
				return (*Axes)[Left].Depth > (*Axes)[Right].Depth;
			});

			for (const size_t Axis : DrawOrder)
			{
				const glm::vec2 Direction = (*Axes)[Axis].ScreenDirection;
				const float DirectionLength = glm::length(Direction);
				ImVec4 Color = m_EditorStyle.GetColor(Tokens[Axis]);
				if ((*Axes)[Axis].Depth > 0.2f)
					Color.w *= 0.55f;
				const ImU32 PackedColor = ImGui::GetColorU32(Color);
				if (DirectionLength < 0.16f)
				{
					DrawList->AddCircleFilled(Center, 3.0f, PackedColor, 12);
					DrawList->AddText(ImVec2(Center.x + 5.0f, Center.y + 2.0f), PackedColor, Labels[Axis]);
					continue;
				}

				const glm::vec2 NormalizedDirection = Direction / DirectionLength;
				const ImVec2 End(Center.x + NormalizedDirection.x * AxisLength,
					Center.y + NormalizedDirection.y * AxisLength);
				const ImVec2 ArrowBase(End.x - NormalizedDirection.x * 6.0f,
					End.y - NormalizedDirection.y * 6.0f);
				const ImVec2 Perpendicular(-NormalizedDirection.y * 3.0f, NormalizedDirection.x * 3.0f);
				DrawList->AddLine(Center, ArrowBase, PackedColor, 2.0f);
				DrawList->AddTriangleFilled(End,
					ImVec2(ArrowBase.x + Perpendicular.x, ArrowBase.y + Perpendicular.y),
					ImVec2(ArrowBase.x - Perpendicular.x, ArrowBase.y - Perpendicular.y), PackedColor);
				DrawList->AddText(ImVec2(End.x + NormalizedDirection.x * 3.0f - 3.0f,
					End.y + NormalizedDirection.y * 3.0f - 5.0f), PackedColor, Labels[Axis]);
			}
		}

		void DrawSceneViewportPanel()
		{
			m_ViewportImageHovered = false;
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport))
			{
				ResetViewportPanelInteraction();
				return;
			}

			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::SceneViewport))
			{
				if (!m_Project)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::FolderOpen,
						"Open a project to render a scene.");
				else if (!m_Scene)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Scene, "Open a scene to render it here.");
				else
				{
#ifdef PF_EDITOR_RENDERER_OPENGL
					m_EditorStyle.TextMuted(
						"The OpenGL fallback retains the editor shell; the scene viewport requires Vulkan.");
#else
					DrawViewportToolbar();
					const ImVec2 Available = ImGui::GetContentRegionAvail();
					if (Available.x <= 1.0f || Available.y <= 1.0f)
					{
						ResetViewportPanelInteraction();
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Scene,
							"Expand the panel to display the scene.");
					}
					else
					{
						if (auto Result = EnsureViewportTarget(Available); !Result)
							SetViewportTargetError("Could not resize the scene viewport: " + Result.error());
						else
							ClearViewportTargetError();
						if (m_ViewportTarget && m_ViewportTextureID != 0)
						{
							const float TargetAspect = static_cast<float>(m_ViewportWidth) /
								static_cast<float>(m_ViewportHeight);
							ImVec2 ImageSize = Available;
							if (ImageSize.x / ImageSize.y > TargetAspect)
								ImageSize.x = ImageSize.y * TargetAspect;
							else
								ImageSize.y = ImageSize.x / TargetAspect;
							const ImVec2 Cursor = ImGui::GetCursorScreenPos();
							ImGui::SetCursorScreenPos(ImVec2(
								Cursor.x + (Available.x - ImageSize.x) * 0.5f,
								Cursor.y + (Available.y - ImageSize.y) * 0.5f));
							ImGui::Image(
								ImTextureRef(static_cast<ImTextureID>(m_ViewportTextureID)),
								ImageSize);
							const bool ImageHovered = ImGui::IsItemHovered();
							const bool ImageClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
							const ImVec2 ImageMinimum = ImGui::GetItemRectMin();
							const ImVec2 ImageMaximum = ImGui::GetItemRectMax();
							ImGui::GetWindowDrawList()->AddRect(
								ImageMinimum,
								ImageMaximum,
								ImGui::GetColorU32(m_EditorStyle.GetColor(
									PulseForgeEditor::EditorColorToken::BorderStrong)));
							m_ViewportImageRect = {
								{ ImageMinimum.x, ImageMinimum.y },
								{ ImageMaximum.x, ImageMaximum.y } };

							const ImVec2 HelpMinimum(ImageMaximum.x - 34.0f, ImageMinimum.y + 8.0f);
							const ImVec2 HelpMaximum(HelpMinimum.x + 27.0f, HelpMinimum.y + 27.0f);
							const ImVec2 MousePosition = ImGui::GetIO().MousePos;
							const ImVec2 WidgetMinimum(ImageMaximum.x - 92.0f, ImageMinimum.y + 38.0f);
							const ImVec2 WidgetMaximum(ImageMaximum.x - 5.0f, ImageMinimum.y + 112.0f);
							const bool ShowEditorOverlays = !m_SceneRuntime && m_ShowEditorOverlays;
							const bool OverHelp = ShowEditorOverlays && MousePosition.x >= HelpMinimum.x && MousePosition.x <= HelpMaximum.x &&
								MousePosition.y >= HelpMinimum.y && MousePosition.y <= HelpMaximum.y;
							const bool OverOrientationWidget = ShowEditorOverlays && MousePosition.x >= WidgetMinimum.x &&
								MousePosition.x <= WidgetMaximum.x && MousePosition.y >= WidgetMinimum.y &&
								MousePosition.y <= WidgetMaximum.y;
							m_ViewportImageHovered = ImageHovered && !OverHelp && !OverOrientationWidget;

							if (ShowEditorOverlays)
							{
								const ImVec2 CursorAfterImage = ImGui::GetCursorScreenPos();
								ImGui::SetCursorScreenPos(HelpMinimum);
								(void)m_EditorStyle.ToolIconButton(PulseForgeEditor::EditorIcon::Info,
									"Help", "ViewportNavigationHelp",
									"Click the image, then WASD move; Q/E down/up; Shift faster\n"
									"Wheel: zoom; with right-mouse navigation: adjust fly speed\n"
									"Right mouse + drag: look\nAlt + left/middle/right drag: orbit/pan/dolly\n"
									"Viewport focus: 1/2/3/4 select/move/rotate/scale; otherwise Q/W/E/R",
									false, true, 27.0f);
								// Finish the cursor restore at the image edge; the normal post-image cursor includes extra spacing.
								ImGui::SetCursorScreenPos(ImVec2(CursorAfterImage.x, ImageMaximum.y));
								ImGui::Dummy(ImVec2(0.0f, 0.0f));
							}
							HandleViewportImage(ImageClicked && !OverHelp && !OverOrientationWidget);
							if (ShowEditorOverlays)
								DrawViewportOrientationWidget(ImGui::GetWindowDrawList(), ImageMinimum, ImageMaximum);
							if (m_EnvironmentLightingPending)
								ImGui::GetWindowDrawList()->AddText(ImVec2(ImageMinimum.x + 12.0f, ImageMinimum.y + 12.0f),
									ImGui::GetColorU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::TextMuted)),
									"Preparing environment lighting...");
						}
						else
						{
							CancelGizmoInteraction();
							m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
								m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::ViewportUnavailable);
							m_ViewportImageRect = {};
							ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error),
								"The scene viewport render target is unavailable.");
						}

						if (!m_ViewportTargetError.empty())
						{
							ImGui::Separator();
							ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error),
								"%s", m_ViewportTargetError.c_str());
						}
						if (!m_ViewportSceneError.empty())
						{
							ImGui::Separator();
							ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning),
								"%s", m_ViewportSceneError.c_str());
						}
					}
#endif
				}
			}
			else
				ResetViewportPanelInteraction();
			ImGui::End();
		}

#ifndef PF_EDITOR_RENDERER_OPENGL
		void DrawViewportToolbar()
		{
			if (!m_EditorStyle.BeginToolbar(
				"ViewportToolbar", PulseForgeEditor::EditorStyleMetrics::ViewportToolbarHeight))
			{
				m_EditorStyle.EndToolbar();
				return;
			}

			const PulseForgeEditor::EditorViewportToolbarLayout Layout =
				PulseForgeEditor::GetEditorViewportToolbarLayout(ImGui::GetContentRegionAvail().x);
			const bool CanEditViewport = m_Scene && !m_SceneRuntime;
			if (Layout == PulseForgeEditor::EditorViewportToolbarLayout::Overflow)
			{
				ImGui::BeginDisabled(!CanEditViewport);
				ImGui::SetNextItemWidth((std::max)(76.0f,
					ImGui::GetContentRegionAvail().x - PulseForgeEditor::EditorStyleMetrics::ViewportToolbarButtonSize - 5.0f));
				if (ImGui::BeginCombo("##ViewportToolOverflow", GetViewportToolName()))
				{
					DrawViewportToolMenuItems(CanEditViewport);
					ImGui::EndCombo();
				}
				ImGui::EndDisabled();
				ImGui::SameLine(0.0f, 4.0f);
				DrawViewportToolbarMoreButton();
				DrawViewportOptionsPopup(true, true);
			}
			else
			{
				DrawViewportToolButton(PulseForgeEditor::ViewportTool::Select,
					PulseForgeEditor::EditorIcon::Select, "Select", "ViewportSelectTool", "Select entities (1 / Q).");
				ImGui::SameLine(0.0f, 3.0f);
				DrawViewportToolButton(PulseForgeEditor::ViewportTool::Move,
					PulseForgeEditor::EditorIcon::Translate, "Move", "ViewportMoveTool", "Move entities (2 / W).");
				ImGui::SameLine(0.0f, 3.0f);
				DrawViewportToolButton(PulseForgeEditor::ViewportTool::Rotate,
					PulseForgeEditor::EditorIcon::Rotate, "Rotate", "ViewportRotateTool", "Rotate entities (3 / E).");
				ImGui::SameLine(0.0f, 3.0f);
				DrawViewportToolButton(PulseForgeEditor::ViewportTool::Scale,
					PulseForgeEditor::EditorIcon::Scale, "Scale", "ViewportScaleTool", "Scale entities (4 / R).");

				DrawViewportToolbarSeparator();
				DrawViewportCoordinateSpaceControl();
				DrawViewportToolbarSeparator();
				DrawViewportSnapToggle();

				if (Layout == PulseForgeEditor::EditorViewportToolbarLayout::Full)
				{
					ImGui::SameLine(0.0f, 5.0f);
					DrawViewportSnapIncrementControl(false);
					DrawViewportToolbarSeparator();
					DrawViewportExposureControl(false);
				}
				else
				{
					ImGui::SameLine(0.0f, 4.0f);
					DrawViewportToolbarMoreButton();
					DrawViewportOptionsPopup(false, false);
				}
			}

			m_EditorStyle.EndToolbar();
		}

		void DrawViewportToolButton(
			PulseForgeEditor::ViewportTool Tool,
			PulseForgeEditor::EditorIcon Icon,
			std::string_view Label,
			std::string_view StableID,
			std::string_view Tooltip)
		{
			if (m_EditorStyle.ToolIconButton(
				Icon, Label, StableID, Tooltip, m_ViewportTool == Tool, m_Scene && !m_SceneRuntime,
				PulseForgeEditor::EditorStyleMetrics::ViewportToolbarButtonSize))
				SetViewportTool(Tool);
		}

		const char* GetViewportToolName() const noexcept
		{
			switch (m_ViewportTool)
			{
			case PulseForgeEditor::ViewportTool::Select: return "Select";
			case PulseForgeEditor::ViewportTool::Move: return "Move";
			case PulseForgeEditor::ViewportTool::Rotate: return "Rotate";
			case PulseForgeEditor::ViewportTool::Scale: return "Scale";
			}
			return "Select";
		}

		void DrawViewportCoordinateSpaceControl()
		{
			if (ImGui::SmallButton(m_GizmoLocalSpace ? "Local##ViewportCoordinateSpace" : "World##ViewportCoordinateSpace"))
			{
				CancelGizmoInteraction();
				m_GizmoLocalSpace = !m_GizmoLocalSpace;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Toggle Local and World gizmo axes.");
		}

		void DrawViewportToolbarSeparator()
		{
			ImGui::SameLine(0.0f, PulseForgeEditor::EditorStyleMetrics::ViewportToolbarGroupSpacing);
			const ImVec2 Cursor = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(Cursor.x + 0.5f, Cursor.y + 3.0f),
				ImVec2(Cursor.x + 0.5f, Cursor.y + 23.0f),
				ImGui::GetColorU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::BorderStrong)));
			ImGui::Dummy(ImVec2(1.0f, 26.0f));
			ImGui::SameLine(0.0f, PulseForgeEditor::EditorStyleMetrics::ViewportToolbarGroupSpacing);
		}

		void DrawViewportToolbarMoreButton()
		{
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Settings, "More", "ViewportToolbarOptions",
				"More viewport and transform options", false,
				PulseForgeEditor::EditorStyleMetrics::ViewportToolbarButtonSize,
				PulseForgeEditor::EditorStyleMetrics::ViewportToolbarButtonSize))
				ImGui::OpenPopup("ViewportOptionsPopup");
		}

		void DrawViewportToolMenuItems(bool CanEditViewport)
		{
			const auto ToolItem = [this, CanEditViewport](
				PulseForgeEditor::EditorIcon Icon,
				const char* Label,
				const char* Shortcut,
				PulseForgeEditor::ViewportTool Tool)
			{
				if (m_EditorStyle.MenuItem(Icon, Label, Shortcut, m_ViewportTool == Tool, CanEditViewport))
					SetViewportTool(Tool);
			};
			ToolItem(PulseForgeEditor::EditorIcon::Select, "Select", "1", PulseForgeEditor::ViewportTool::Select);
			ToolItem(PulseForgeEditor::EditorIcon::Translate, "Move", "2", PulseForgeEditor::ViewportTool::Move);
			ToolItem(PulseForgeEditor::EditorIcon::Rotate, "Rotate", "3", PulseForgeEditor::ViewportTool::Rotate);
			ToolItem(PulseForgeEditor::EditorIcon::Scale, "Scale", "4", PulseForgeEditor::ViewportTool::Scale);
		}

		void DrawViewportOptionsPopup(bool IncludeCoordinateSpace, bool IncludeSnapToggle)
		{
			if (!ImGui::BeginPopup("ViewportOptionsPopup"))
				return;

			if (IncludeCoordinateSpace)
				DrawViewportCoordinateSpaceControl();
			if (IncludeCoordinateSpace && IncludeSnapToggle)
				ImGui::Separator();
			if (IncludeSnapToggle)
				DrawViewportSnapToggle();
			DrawViewportSnapIncrementControl(true);
			ImGui::Separator();
			DrawViewportExposureControl(true);
			ImGui::EndPopup();
		}

		void DrawViewportSnapToggle()
		{
			ImGui::Checkbox("Snap", &m_TransformSnapping);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Snap during gizmo drags. Hold Ctrl to temporarily enable snapping.");
		}

		void DrawViewportSnapIncrementControl(bool ShowLabel)
		{
			struct SnapPreset
			{
				const char* Label;
				float Value;
			};
			constexpr std::array<SnapPreset, 5> TranslationPresets{{
				{ "0.1 units", 0.1f }, { "0.5 units", 0.5f }, { "1 unit", 1.0f },
				{ "5 units", 5.0f }, { "10 units", 10.0f } }};
			constexpr std::array<SnapPreset, 5> RotationPresets{{
				{ "5 degrees", 5.0f }, { "10 degrees", 10.0f }, { "15 degrees", 15.0f },
				{ "45 degrees", 45.0f }, { "90 degrees", 90.0f } }};
			constexpr std::array<SnapPreset, 5> ScalePresets{{
				{ "0.05 scale", 0.05f }, { "0.1 scale", 0.1f }, { "0.25 scale", 0.25f },
				{ "0.5 scale", 0.5f }, { "1.0 scale", 1.0f } }};

			float* Increment = &m_TranslationSnapIncrement;
			const std::array<SnapPreset, 5>* Presets = &TranslationPresets;
			if (m_ViewportTool == PulseForgeEditor::ViewportTool::Rotate)
			{
				Increment = &m_RotationSnapDegrees;
				Presets = &RotationPresets;
			}
			else if (m_ViewportTool == PulseForgeEditor::ViewportTool::Scale)
			{
				Increment = &m_ScaleSnapIncrement;
				Presets = &ScalePresets;
			}

			const char* CurrentLabel = (*Presets)[0].Label;
			for (const SnapPreset& Preset : *Presets)
				if (std::abs(Preset.Value - *Increment) < 1.0e-5f)
					CurrentLabel = Preset.Label;
			ImGui::SetNextItemWidth(ShowLabel ? 156.0f : 112.0f);
			if (ImGui::BeginCombo(ShowLabel ? "Snap Increment" : "##TransformSnapIncrement", CurrentLabel))
			{
				for (const SnapPreset& Preset : *Presets)
				{
					const bool Selected = std::abs(Preset.Value - *Increment) < 1.0e-5f;
					if (ImGui::Selectable(Preset.Label, Selected))
						*Increment = Preset.Value;
					if (Selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		void DrawViewportExposureControl(bool ShowLabel)
		{
			if (!ShowLabel)
			{
				m_EditorStyle.TextMuted("Exposure");
				ImGui::SameLine(0.0f, 4.0f);
			}
			ImGui::SetNextItemWidth(ShowLabel ? 144.0f : 98.0f);
			(void)ImGui::DragFloat(
				ShowLabel ? "Exposure (EV)" : "##ViewportExposureEV",
				&m_ExposureEV,
				0.05f,
				-8.0f,
				8.0f,
				"%.2f EV",
				ImGuiSliderFlags_AlwaysClamp);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Adjust viewport exposure from -8 to +8 EV. This does not modify the scene.");
		}
#endif

		void SetViewportTool(PulseForgeEditor::ViewportTool Tool)
		{
			if (m_ViewportTool == Tool)
				return;
			CancelGizmoInteraction();
			m_ViewportTool = Tool;
			switch (Tool)
			{
			case PulseForgeEditor::ViewportTool::Move:
				m_GizmoOperation = PulseForgeEditor::TransformGizmoOperation::Translate;
				break;
			case PulseForgeEditor::ViewportTool::Rotate:
				m_GizmoOperation = PulseForgeEditor::TransformGizmoOperation::Rotate;
				break;
			case PulseForgeEditor::ViewportTool::Scale:
				m_GizmoOperation = PulseForgeEditor::TransformGizmoOperation::Scale;
				break;
			case PulseForgeEditor::ViewportTool::Select:
				break;
			}
		}

		void HandleViewportToolShortcuts()
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			return;
#else
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport))
				return;

			const ImGuiIO& IO = ImGui::GetIO();
			const PulseForgeEditor::ViewportShortcutContext ShortcutContext{
				.SceneAvailable = m_Scene != nullptr,
				.RuntimeActive = m_SceneRuntime != nullptr,
				.CameraNavigationActive = m_EditorCameraNavigationActive,
				.ViewportKeyboardFocused = m_ViewportKeyboardFocused,
				.RightMouseHeld = IO.MouseDown[ImGuiMouseButton_Right],
				.TextInputActive = IO.WantTextInput,
				.ActiveImGuiItem = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui->NavLayer == ImGuiNavLayer_Menu ||
					GImGui->NavWindowingTarget != nullptr,
				.ModifierHeld = IO.KeyCtrl || IO.KeyShift || IO.KeyAlt || IO.KeySuper
			};
			if (m_ViewportKeyboardFocused)
			{
				if (!PulseForgeEditor::CanHandleFocusedViewportToolShortcuts(ShortcutContext))
					return;
				constexpr std::array<std::pair<ImGuiKey, char>, 4> FocusedShortcuts = {{
					{ ImGuiKey_1, '1' },
					{ ImGuiKey_2, '2' },
					{ ImGuiKey_3, '3' },
					{ ImGuiKey_4, '4' }
				}};
				for (const auto& [Key, Character] : FocusedShortcuts)
					if (ImGui::IsKeyPressed(Key, false))
						if (const auto Tool = PulseForgeEditor::GetFocusedViewportToolForShortcut(Character))
							SetViewportTool(*Tool);
				return;
			}

			// With no viewport keyboard focus, Q/W/E/R retain their established editor-wide tool bindings.
			if (!PulseForgeEditor::CanHandleViewportToolShortcuts(ShortcutContext))
				return;

			constexpr std::array<std::pair<ImGuiKey, char>, 4> Shortcuts = {{
				{ ImGuiKey_Q, 'Q' },
				{ ImGuiKey_W, 'W' },
				{ ImGuiKey_E, 'E' },
				{ ImGuiKey_R, 'R' }
			}};
			for (const auto& [Key, Character] : Shortcuts)
				if (ImGui::IsKeyPressed(Key, false))
					if (const auto Tool = PulseForgeEditor::GetViewportToolForShortcut(Character))
						SetViewportTool(*Tool);
#endif
		}

		void HandleViewportImage(bool ImageClicked)
		{
			if (m_SceneRuntime || !m_Scene)
			{
				CancelGizmoInteraction();
				m_ViewportCameraGesture = ViewportCameraGesture::None;
				return;
			}
			if (ImageClicked && m_ViewportImageHovered)
				m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
					m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::RenderedImageClicked);

			ImGuiIO& IO = ImGui::GetIO();
			const PulseForgeEditor::ViewportWheelAction WheelAction =
				PulseForgeEditor::ResolveViewportWheelAction({
					.PointerOverViewportImage = m_ViewportImageHovered,
					.RightMouseNavigationActive = m_EditorCameraNavigationActive,
					.RightMouseHeld = IO.MouseDown[ImGuiMouseButton_Right],
					.RuntimeActive = m_SceneRuntime != nullptr,
					.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)
				});
			if (WheelAction == PulseForgeEditor::ViewportWheelAction::Zoom && IO.MouseWheel != 0.0f &&
				!IO.MouseDown[ImGuiMouseButton_Left] && !IO.MouseDown[ImGuiMouseButton_Middle] &&
				!ImGui::IsAnyItemActive())
				m_EditorCamera.Dolly(PulseForgeEditor::GetViewportWheelDollyDelta(IO.MouseWheel));
			if (!m_ShowEditorOverlays)
				return;

			if (m_SelectedEntity && !m_Scene->FindEntity(*m_SelectedEntity))
			{
				SelectSingleEntity(std::nullopt);
				CancelGizmoInteraction();
			}

			const glm::vec2 Mouse{ IO.MousePos.x, IO.MousePos.y };
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
				(m_ViewportCameraGesture != ViewportCameraGesture::None || m_GizmoDrag.has_value()))
			{
				m_ViewportCameraGesture = ViewportCameraGesture::None;
				if (m_GizmoDrag)
					CancelGizmoInteraction();
				return;
			}
			if (m_GizmoDrag)
			{
				UpdateGizmoDrag(Mouse);
				if (m_SelectedEntity)
				{
					if (const auto Selected = m_Scene->FindEntity(*m_SelectedEntity))
						DrawViewportGizmo(*Selected, std::nullopt);
				}
				if (PulseForgeEditor::GetViewportGizmoPointerOutcome({
					m_GizmoDrag.has_value(),
					IO.MouseDown[ImGuiMouseButton_Left],
					IO.MouseReleased[ImGuiMouseButton_Left]
				}, false) == PulseForgeEditor::ViewportGizmoPointerOutcome::Complete)
					CompleteGizmoInteraction();
				return;
			}
			if (m_ViewportCameraGesture != ViewportCameraGesture::None)
			{
				const int Button = m_ViewportCameraGesture == ViewportCameraGesture::Orbit
					? ImGuiMouseButton_Left
					: m_ViewportCameraGesture == ViewportCameraGesture::Pan
						? ImGuiMouseButton_Middle : ImGuiMouseButton_Right;
				if (IO.MouseReleased[Button] || !IO.MouseDown[Button] || !IO.KeyAlt)
				{
					m_ViewportCameraGesture = ViewportCameraGesture::None;
					return;
				}
				const ImVec2 Delta = IO.MouseDelta;
				if (m_ViewportCameraGesture == ViewportCameraGesture::Orbit)
					m_EditorCamera.Orbit(Delta.x, Delta.y);
				else if (m_ViewportCameraGesture == ViewportCameraGesture::Pan)
					m_EditorCamera.Pan(Delta.x, Delta.y,
						m_ViewportImageRect.Maximum.y - m_ViewportImageRect.Minimum.y);
				else
					m_EditorCamera.Dolly(-Delta.y * 0.08f);
				return;
			}

			std::optional<ViewportGizmoGeometry> Geometry;
			if (m_SelectedEntity && m_ViewportTool != PulseForgeEditor::ViewportTool::Select)
			{
				if (const auto Selected = m_Scene->FindEntity(*m_SelectedEntity))
					if (IsEntityEffectivelyVisible(*Selected))
						Geometry = BuildViewportGizmoGeometry(*Selected);
			}

			const std::optional<GizmoHandle> HoveredHandle = Geometry && m_ViewportImageHovered
				? FindGizmoHandle(*Geometry, Mouse)
				: std::nullopt;
			if (Geometry)
			{
				const auto Selected = m_Scene->FindEntity(*m_SelectedEntity);
				if (Selected)
					DrawViewportGizmo(*Selected, HoveredHandle);
			}

			const bool CameraGestureClicked = IO.KeyAlt &&
				(IO.MouseClicked[ImGuiMouseButton_Left] || IO.MouseClicked[ImGuiMouseButton_Middle] ||
					IO.MouseClicked[ImGuiMouseButton_Right]);
			if ((!ImageClicked && !CameraGestureClicked) || !m_ViewportImageHovered || m_EditorCameraNavigationActive)
				return;
			if (HoveredHandle && Geometry)
			{
				(void)BeginGizmoDrag(*HoveredHandle, *Geometry, Mouse);
				return;
			}
			if (IO.KeyAlt)
			{
				if (IO.MouseClicked[ImGuiMouseButton_Left])
					m_ViewportCameraGesture = ViewportCameraGesture::Orbit;
				else if (IO.MouseClicked[ImGuiMouseButton_Middle])
					m_ViewportCameraGesture = ViewportCameraGesture::Pan;
				else if (IO.MouseClicked[ImGuiMouseButton_Right])
					m_ViewportCameraGesture = ViewportCameraGesture::Dolly;
				if (m_ViewportCameraGesture != ViewportCameraGesture::None)
					return;
			}
			PickEntityAt(Mouse, IO.KeyCtrl, IO.KeyShift);
		}

		[[nodiscard]] std::optional<glm::vec2> ProjectToViewportImage(const glm::vec3& Position) const
		{
			if (!m_ViewportViewProjectionValid)
				return std::nullopt;
			return PulseForgeEditor::ProjectWorldToViewportImage(
				Position,
				m_ViewportViewProjection,
				m_ViewportImageRect);
		}

		[[nodiscard]] std::optional<ViewportGizmoGeometry> BuildViewportGizmoGeometry(
			const PulseForge::Entity& Entity) const
		{
			if (!m_ViewportViewProjectionValid)
				return std::nullopt;
			const auto Transform = Entity.GetTransform();
			const auto World = Entity.GetWorldMatrix();
			const auto Parent = Entity.GetParent();
			if (!Transform || !World || !Parent || !PulseForgeEditor::Detail::IsFinite(*World))
				return std::nullopt;

			glm::mat4 ParentWorld(1.0f);
			if (Parent->has_value())
			{
				const auto ParentTransform = Parent->value().GetWorldMatrix();
				if (!ParentTransform || !PulseForgeEditor::Detail::IsFinite(*ParentTransform))
					return std::nullopt;
				ParentWorld = *ParentTransform;
			}

			ViewportGizmoGeometry Geometry;
			Geometry.PivotWorld = glm::vec3((*World)[3]);
			if (const auto Roots = GetTopLevelSelectedEntities(); Roots && Roots->size() > 1)
			{
				glm::vec3 Minimum((std::numeric_limits<float>::max)());
				glm::vec3 Maximum(-(std::numeric_limits<float>::max)());
				bool HasPosition = false;
				for (const PulseForge::Entity& Root : *Roots)
					if (const auto RootWorld = Root.GetWorldMatrix(); RootWorld && PulseForgeEditor::Detail::IsFinite(*RootWorld))
					{
						const glm::vec3 Position((*RootWorld)[3]);
						for (glm::length_t Axis = 0; Axis < 3; ++Axis)
						{
							Minimum[Axis] = (std::min)(Minimum[Axis], Position[Axis]);
							Maximum[Axis] = (std::max)(Maximum[Axis], Position[Axis]);
						}
						HasPosition = true;
					}
				if (HasPosition)
					Geometry.PivotWorld = (Minimum + Maximum) * 0.5f;
			}
			const auto PivotScreen = ProjectToViewportImage(Geometry.PivotWorld);
			if (!PivotScreen)
				return std::nullopt;
			Geometry.PivotScreen = *PivotScreen;

			const float CameraDistance = glm::length(m_EditorCamera.GetPosition() - Geometry.PivotWorld);
			const float ImageHeight = m_ViewportImageRect.Maximum.y - m_ViewportImageRect.Minimum.y;
			if (!std::isfinite(CameraDistance) || CameraDistance < 1.0e-4f || ImageHeight <= 1.0f)
				return std::nullopt;
			Geometry.HandleLengthWorld = CameraDistance * 2.0f * std::tan(glm::radians(45.0f) * 0.5f) * 82.0f / ImageHeight;
			if (!std::isfinite(Geometry.HandleLengthWorld) || Geometry.HandleLengthWorld <= 1.0e-6f)
				return std::nullopt;

			for (uint32_t AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				const auto Axis = PulseForgeEditor::GetGizmoAxis(
					ParentWorld, Transform->Rotation, AxisIndex, m_GizmoLocalSpace);
				if (!Axis)
					continue;
				const auto Endpoint = ProjectToViewportImage(
					Geometry.PivotWorld + Axis->Direction * Geometry.HandleLengthWorld);
				if (!Endpoint)
					continue;
				Geometry.Axes[AxisIndex] = { Axis->Direction, Axis->WorldUnitsPerLocalUnit, *Endpoint, true };

				const glm::vec3 WorldAxis = Axis->Direction;
				const glm::vec3 CameraForward = m_EditorCamera.GetForward();
				glm::vec3 RingU = glm::cross(WorldAxis, CameraForward);
				if (glm::length(RingU) < 1.0e-4f)
				{
					const glm::vec3 Fallback = std::abs(WorldAxis.y) < 0.9f
						? glm::vec3(0.0f, 1.0f, 0.0f)
						: glm::vec3(1.0f, 0.0f, 0.0f);
					RingU = glm::cross(WorldAxis, Fallback);
				}
				const float RingULength = glm::length(RingU);
				if (std::isfinite(RingULength) && RingULength > 1.0e-5f)
				{
					RingU /= RingULength;
					const glm::vec3 RingV = glm::normalize(glm::cross(WorldAxis, RingU));
					bool RingValid = true;
					for (size_t PointIndex = 0; PointIndex < Geometry.RotationRings[AxisIndex].size(); ++PointIndex)
					{
						const float Angle = glm::two_pi<float>() * static_cast<float>(PointIndex) / 48.0f;
						const glm::vec3 Point = Geometry.PivotWorld + Geometry.HandleLengthWorld * 0.82f *
							(RingU * std::cos(Angle) + RingV * std::sin(Angle));
						const auto ScreenPoint = ProjectToViewportImage(Point);
						if (!ScreenPoint)
						{
							RingValid = false;
							break;
						}
						Geometry.RotationRings[AxisIndex][PointIndex] = *ScreenPoint;
					}
					Geometry.RotationRingValid[AxisIndex] = RingValid;
				}
			}
			Geometry.IsValid = true;
			return Geometry;
		}

		[[nodiscard]] static float DistanceSquaredToSegment(
			const glm::vec2& Point,
			const glm::vec2& Start,
			const glm::vec2& End)
		{
			const glm::vec2 Segment = End - Start;
			const float LengthSquared = glm::dot(Segment, Segment);
			const float Parameter = LengthSquared > 1.0e-6f
				? std::clamp(glm::dot(Point - Start, Segment) / LengthSquared, 0.0f, 1.0f)
				: 0.0f;
			const glm::vec2 Difference = Point - (Start + Segment * Parameter);
			return glm::dot(Difference, Difference);
		}

		[[nodiscard]] std::optional<GizmoHandle> FindGizmoHandle(
			const ViewportGizmoGeometry& Geometry,
			const glm::vec2& Mouse) const
		{
			if (!Geometry.IsValid || !PulseForgeEditor::Detail::IsFinite(Mouse))
				return std::nullopt;
			constexpr float HitRadiusSquared = 8.0f * 8.0f;
			float ClosestDistance = HitRadiusSquared;
			std::optional<GizmoHandle> Closest;
			if (m_GizmoOperation == PulseForgeEditor::TransformGizmoOperation::Rotate)
			{
				for (uint32_t Axis = 0; Axis < 3; ++Axis)
				{
					if (!Geometry.RotationRingValid[Axis])
						continue;
					const auto& Ring = Geometry.RotationRings[Axis];
					for (size_t Point = 0; Point + 1 < Ring.size(); ++Point)
					{
						const float Distance = DistanceSquaredToSegment(Mouse, Ring[Point], Ring[Point + 1]);
						if (Distance < ClosestDistance)
						{
							ClosestDistance = Distance;
							Closest = GizmoHandle{ m_GizmoOperation, Axis };
						}
					}
				}
				return Closest;
			}

			for (uint32_t Axis = 0; Axis < 3; ++Axis)
			{
				if (!Geometry.Axes[Axis].IsValid)
					continue;
				const glm::vec2 ScreenAxis = Geometry.Axes[Axis].Endpoint - Geometry.PivotScreen;
				if (glm::dot(ScreenAxis, ScreenAxis) < 14.0f * 14.0f)
					continue;
				const glm::vec2 Start = Geometry.PivotScreen +
					(Geometry.Axes[Axis].Endpoint - Geometry.PivotScreen) * 0.12f;
				const float Distance = DistanceSquaredToSegment(Mouse, Start, Geometry.Axes[Axis].Endpoint);
				if (Distance < ClosestDistance)
				{
					ClosestDistance = Distance;
					Closest = GizmoHandle{ m_GizmoOperation, Axis };
				}
			}
			return Closest;
		}

		void DrawViewportGizmo(const PulseForge::Entity& Entity, std::optional<GizmoHandle> Hovered)
		{
			const auto Geometry = BuildViewportGizmoGeometry(Entity);
			if (!Geometry)
				return;

			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			DrawList->PushClipRect(
				ImVec2(m_ViewportImageRect.Minimum.x, m_ViewportImageRect.Minimum.y),
				ImVec2(m_ViewportImageRect.Maximum.x, m_ViewportImageRect.Maximum.y),
				true);
			const std::array<ImU32, 3> AxisColors{
				ImGui::ColorConvertFloat4ToU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoAxisX)),
				ImGui::ColorConvertFloat4ToU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoAxisY)),
				ImGui::ColorConvertFloat4ToU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoAxisZ)) };
			const ImU32 HighlightColor = ImGui::ColorConvertFloat4ToU32(
				m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoHighlight));
			if (m_GizmoOperation == PulseForgeEditor::TransformGizmoOperation::Rotate)
			{
				for (uint32_t Axis = 0; Axis < 3; ++Axis)
				{
					if (!Geometry->RotationRingValid[Axis])
						continue;
					const bool IsHighlighted = Hovered && Hovered->Axis == Axis;
					const ImU32 Color = IsHighlighted ? HighlightColor : AxisColors[Axis];
					const auto& Ring = Geometry->RotationRings[Axis];
					for (size_t Point = 0; Point + 1 < Ring.size(); ++Point)
						DrawList->AddLine(
							ImVec2(Ring[Point].x, Ring[Point].y),
							ImVec2(Ring[Point + 1].x, Ring[Point + 1].y),
							Color,
							IsHighlighted ? 3.0f : 2.0f);
				}
			}
			else
			{
				for (uint32_t Axis = 0; Axis < 3; ++Axis)
				{
					if (!Geometry->Axes[Axis].IsValid)
						continue;
					const bool IsHighlighted = Hovered && Hovered->Axis == Axis;
					const ImU32 Color = IsHighlighted ? HighlightColor : AxisColors[Axis];
					const glm::vec2 End = Geometry->Axes[Axis].Endpoint;
					DrawList->AddLine(
						ImVec2(Geometry->PivotScreen.x, Geometry->PivotScreen.y),
						ImVec2(End.x, End.y),
						Color,
						IsHighlighted ? 4.0f : 3.0f);
					if (m_GizmoOperation == PulseForgeEditor::TransformGizmoOperation::Scale)
						DrawList->AddRectFilled(ImVec2(End.x - 5.0f, End.y - 5.0f), ImVec2(End.x + 5.0f, End.y + 5.0f), Color);
					else
						DrawList->AddCircleFilled(ImVec2(End.x, End.y), IsHighlighted ? 6.0f : 4.5f, Color);
				}
			}
			DrawList->AddCircleFilled(
				ImVec2(Geometry->PivotScreen.x, Geometry->PivotScreen.y),
				5.0f,
				ImGui::ColorConvertFloat4ToU32(
					m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Text)));
			DrawList->PopClipRect();
		}

		[[nodiscard]] std::optional<std::vector<GizmoEntityStart>> CaptureGizmoTargets(
			const std::vector<PulseForge::UUID>& Identifiers) const
		{
			if (!m_Scene || Identifiers.empty())
				return std::nullopt;
			std::vector<GizmoEntityStart> Targets;
			Targets.reserve(Identifiers.size());
			for (const PulseForge::UUID& Identifier : Identifiers)
			{
				const auto Entity = m_Scene->FindEntity(Identifier);
				if (!Entity)
					return std::nullopt;
				const auto Transform = Entity->GetTransform();
				const auto World = Entity->GetWorldMatrix();
				const auto Parent = Entity->GetParent();
				if (!Transform || !World || !Parent || !PulseForgeEditor::Detail::IsFinite(*World))
					return std::nullopt;
				glm::mat4 ParentWorld(1.0f);
				if (Parent->has_value())
				{
					const auto ParentMatrix = Parent->value().GetWorldMatrix();
					if (!ParentMatrix || !PulseForgeEditor::Detail::IsFinite(*ParentMatrix) ||
						std::abs(glm::determinant(*ParentMatrix)) < 1.0e-8f)
						return std::nullopt;
					ParentWorld = *ParentMatrix;
				}
				Targets.push_back({ Identifier, *Transform, *World, ParentWorld });
			}
			return Targets;
		}

		[[nodiscard]] static std::optional<PulseForge::TransformComponent> DecomposeLocalTransform(
			const glm::mat4& LocalMatrix)
		{
			if (!PulseForgeEditor::Detail::IsFinite(LocalMatrix))
				return std::nullopt;
			glm::vec3 Scale(1.0f);
			glm::quat Rotation(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Translation(0.0f);
			glm::vec3 Skew(0.0f);
			glm::vec4 Perspective(0.0f, 0.0f, 0.0f, 1.0f);
			if (!glm::decompose(LocalMatrix, Scale, Rotation, Translation, Skew, Perspective))
				return std::nullopt;
			const bool PerspectiveFinite = std::isfinite(Perspective.x) && std::isfinite(Perspective.y) &&
				std::isfinite(Perspective.z) && std::isfinite(Perspective.w);
			if (
				!PulseForgeEditor::Detail::IsFinite(Scale) || !PulseForgeEditor::Detail::IsFinite(Rotation) ||
				!PulseForgeEditor::Detail::IsFinite(Translation) || !PulseForgeEditor::Detail::IsFinite(Skew) ||
				!PerspectiveFinite || glm::length(Skew) > 1.0e-3f ||
				glm::length(glm::vec3(Perspective)) > 1.0e-3f || std::abs(Perspective.w - 1.0f) > 1.0e-3f ||
				std::abs(Scale.x) < 1.0e-5f || std::abs(Scale.y) < 1.0e-5f || std::abs(Scale.z) < 1.0e-5f)
				return std::nullopt;
			PulseForge::TransformComponent Transform;
			Transform.Translation = Translation;
			Transform.Rotation = glm::normalize(Rotation);
			Transform.Scale = Scale;
			return Transform;
		}

		bool BeginGizmoDrag(const GizmoHandle& Handle, const ViewportGizmoGeometry& Geometry, const glm::vec2& Mouse)
		{
			if (!m_Scene || !m_SelectedEntity || m_SceneRuntime || m_EditorCameraNavigationActive ||
				Handle.Axis >= Geometry.Axes.size() || !Geometry.Axes[Handle.Axis].IsValid || !m_ViewportViewProjectionValid)
				return false;
			const auto Entity = m_Scene->FindEntity(*m_SelectedEntity);
			if (!Entity)
				return false;
			const auto Transform = Entity->GetTransform();
			const auto Ray = PulseForgeEditor::MakeViewportRay(Mouse, m_ViewportImageRect, m_ViewportViewProjection);
			if (!Transform || !Ray)
				return false;

			const glm::vec3 WorldAxis = Geometry.Axes[Handle.Axis].WorldDirection;
			glm::vec3 PlaneNormal = WorldAxis;
			if (Handle.Operation != PulseForgeEditor::TransformGizmoOperation::Rotate)
			{
				const glm::vec3 ToCamera = m_EditorCamera.GetPosition() - Geometry.PivotWorld;
				PlaneNormal = ToCamera - WorldAxis * glm::dot(ToCamera, WorldAxis);
			}
			const float PlaneNormalLength = glm::length(PlaneNormal);
			if (!std::isfinite(PlaneNormalLength) || PlaneNormalLength < 1.0e-5f)
				return false;
			PlaneNormal /= PlaneNormalLength;
			const auto StartPoint = PulseForgeEditor::IntersectRayPlane(*Ray, Geometry.PivotWorld, PlaneNormal);
			if (!StartPoint)
				return false;
			auto Roots = GetTopLevelSelectedEntities();
			if (!Roots || Roots->empty())
				return false;
			std::vector<PulseForge::UUID> RootIdentifiers;
			RootIdentifiers.reserve(Roots->size());
			for (const PulseForge::Entity& Root : *Roots)
				RootIdentifiers.push_back(Root.GetUUID());
			auto Targets = CaptureGizmoTargets(RootIdentifiers);
			if (!Targets)
				return false;
			if (!BeginSceneEdit(Handle.Operation == PulseForgeEditor::TransformGizmoOperation::Translate
				? "Move Selection"
				: Handle.Operation == PulseForgeEditor::TransformGizmoOperation::Rotate
					? "Rotate Selection" : "Scale Selection"))
				return false;
			GizmoDrag Drag;
			Drag.Entity = *m_SelectedEntity;
			Drag.SceneIdentity = m_Scene.get();
			Drag.InitialTransform = *Transform;
			Drag.Targets = std::move(*Targets);
			Drag.SourceRoots = std::move(RootIdentifiers);
			Drag.Operation = Handle.Operation;
			Drag.Axis = Handle.Axis;
			Drag.PivotWorld = Geometry.PivotWorld;
			Drag.WorldAxis = WorldAxis;
			Drag.PlaneNormal = PlaneNormal;
			Drag.StartPoint = *StartPoint;
			Drag.StartMouse = Mouse;
			Drag.WorldUnitsPerLocalUnit = Geometry.Axes[Handle.Axis].WorldUnitsPerLocalUnit;
			Drag.DuplicateOnDrag = ImGui::GetIO().KeyAlt;
			m_GizmoDrag = std::move(Drag);
			return true;
		}

		void UpdateGizmoDrag(const glm::vec2& Mouse)
		{
			if (!m_GizmoDrag)
				return;
			GizmoDrag& Drag = *m_GizmoDrag;
			if (!m_Scene || m_Scene.get() != Drag.SceneIdentity || m_SceneRuntime || !m_SelectedEntity)
			{
				CancelGizmoInteraction();
				return;
			}

			const auto Ray = PulseForgeEditor::MakeViewportRay(
				Mouse, m_ViewportImageRect, m_ViewportViewProjection, true);
			if (!Ray)
				return;
			const auto CurrentPoint = PulseForgeEditor::IntersectRayPlane(*Ray, Drag.PivotWorld, Drag.PlaneNormal);
			if (!CurrentPoint)
				return;

			float Delta = 0.0f;
			float WorldDelta = 0.0f;
			if (Drag.Operation == PulseForgeEditor::TransformGizmoOperation::Rotate)
			{
				const glm::vec3 StartVector = glm::normalize(Drag.StartPoint - Drag.PivotWorld);
				const glm::vec3 CurrentVector = glm::normalize(*CurrentPoint - Drag.PivotWorld);
				Delta = std::atan2(
					glm::dot(Drag.WorldAxis, glm::cross(StartVector, CurrentVector)),
					glm::dot(StartVector, CurrentVector));
			}
			else
			{
				WorldDelta = glm::dot(*CurrentPoint - Drag.StartPoint, Drag.WorldAxis);
				if (!std::isfinite(Drag.WorldUnitsPerLocalUnit) || Drag.WorldUnitsPerLocalUnit < 1.0e-7f)
					return;
				if (Drag.Operation == PulseForgeEditor::TransformGizmoOperation::Translate &&
					(m_TransformSnapping || ImGui::GetIO().KeyCtrl))
				{
					const auto Snapped = PulseForgeEditor::SnapValueToIncrement(WorldDelta, m_TranslationSnapIncrement);
					if (!Snapped)
						return;
					WorldDelta = *Snapped;
				}
				Delta = WorldDelta / Drag.WorldUnitsPerLocalUnit;
			}
			if (!std::isfinite(Delta))
				return;
			if (m_TransformSnapping || ImGui::GetIO().KeyCtrl)
			{
				const float Increment = Drag.Operation == PulseForgeEditor::TransformGizmoOperation::Rotate
					? glm::radians(m_RotationSnapDegrees) : m_ScaleSnapIncrement;
				if (Drag.Operation != PulseForgeEditor::TransformGizmoOperation::Translate)
				{
					const auto Snapped = PulseForgeEditor::SnapValueToIncrement(Delta, Increment);
					if (!Snapped)
						return;
					Delta = *Snapped;
				}
			}

			const glm::vec2 MouseDelta = Mouse - Drag.StartMouse;
			if (Drag.DuplicateOnDrag && !ImGui::GetIO().KeyAlt)
				Drag.DuplicateOnDrag = false;
			if (Drag.DuplicateOnDrag && !Drag.HasDuplicated && glm::length(MouseDelta) >= 3.0f)
			{
				std::vector<PulseForge::Entity> Sources;
				Sources.reserve(Drag.SourceRoots.size());
				for (const PulseForge::UUID& Identifier : Drag.SourceRoots)
				{
					const auto Source = m_Scene->FindEntity(Identifier);
					if (!Source)
					{
						CancelGizmoInteraction();
						return;
					}
					Sources.push_back(*Source);
				}
				auto Duplicated = DuplicateEntityRootsRaw(Sources);
				if (!Duplicated)
				{
					SetError("Alt-drag duplication failed: " + Duplicated.error().Message);
					CancelGizmoInteraction();
					return;
				}
				SetEditorSelection(*Duplicated);
				auto DuplicatedTargets = CaptureGizmoTargets(*Duplicated);
				if (!DuplicatedTargets)
				{
					SetError("Could not initialize the duplicated selection for the gizmo drag.");
					CancelGizmoInteraction();
					return;
				}
				Drag.Targets = std::move(*DuplicatedTargets);
				Drag.Entity = *m_SelectedEntity;
				Drag.InitialTransform = Drag.Targets.back().LocalTransform;
				Drag.HasDuplicated = true;
				Drag.DuplicateOnDrag = false;
			}

			if (Drag.Targets.empty())
			{
				CancelGizmoInteraction();
				return;
			}

			if (Drag.Targets.size() == 1 && !Drag.HasDuplicated && m_GizmoLocalSpace)
			{
				const auto Entity = m_Scene->FindEntity(Drag.Targets.front().Identifier);
				const auto Updated = PulseForgeEditor::ApplyLocalGizmoDelta(
					Drag.InitialTransform, Drag.Operation, Drag.Axis, Delta);
				if (!Entity || !Updated || !TransformsDiffer(Drag.InitialTransform, *Updated))
					return;
				if (auto Result = Entity->SetTransform(*Updated); !Result)
				{
					SetError("Viewport transform manipulation failed: " + Result.error().Message);
					CancelGizmoInteraction();
					return;
				}
				m_SceneDirty = true;
				return;
			}

			glm::mat4 WorldOperation(1.0f);
			if (Drag.Operation == PulseForgeEditor::TransformGizmoOperation::Translate)
				WorldOperation = glm::translate(glm::mat4(1.0f), Drag.WorldAxis * WorldDelta);
			else if (Drag.Operation == PulseForgeEditor::TransformGizmoOperation::Rotate)
			{
				const glm::quat Rotation = glm::angleAxis(Delta, glm::normalize(Drag.WorldAxis));
				WorldOperation = glm::translate(glm::mat4(1.0f), Drag.PivotWorld) * glm::mat4_cast(Rotation) *
					glm::translate(glm::mat4(1.0f), -Drag.PivotWorld);
			}
			else if (Drag.Operation == PulseForgeEditor::TransformGizmoOperation::Scale)
			{
				const float Factor = 1.0f + Delta;
				if (!std::isfinite(Factor) || Factor <= 1.0e-4f)
					return;
				const glm::vec3 Scale = Drag.Axis == 0 ? glm::vec3(Factor, 1.0f, 1.0f)
					: Drag.Axis == 1 ? glm::vec3(1.0f, Factor, 1.0f) : glm::vec3(1.0f, 1.0f, Factor);
				WorldOperation = glm::translate(glm::mat4(1.0f), Drag.PivotWorld) * glm::scale(glm::mat4(1.0f), Scale) *
					glm::translate(glm::mat4(1.0f), -Drag.PivotWorld);
			}

			for (const GizmoEntityStart& Target : Drag.Targets)
			{
				const auto Entity = m_Scene->FindEntity(Target.Identifier);
				std::optional<PulseForge::TransformComponent> Updated;
				if (Drag.Operation == PulseForgeEditor::TransformGizmoOperation::Scale && m_GizmoLocalSpace)
				{
					const float Factor = 1.0f + Delta;
					PulseForge::TransformComponent Scaled = Target.LocalTransform;
					Scaled.Scale[Drag.Axis] *= Factor;
					const glm::vec3 Position(Target.WorldTransform[3]);
					const float AlongAxis = glm::dot(Position - Drag.PivotWorld, Drag.WorldAxis);
					const glm::vec3 ScaledPosition = Position + Drag.WorldAxis * AlongAxis * (Factor - 1.0f);
					const glm::vec3 LocalPosition(glm::inverse(Target.ParentWorldTransform) * glm::vec4(ScaledPosition, 1.0f));
					Scaled.Translation = LocalPosition;
					if (std::abs(Scaled.Scale[Drag.Axis]) >= 1.0e-4f &&
						PulseForgeEditor::Detail::IsFinite(Scaled.Translation) &&
						PulseForgeEditor::Detail::IsFinite(Scaled.Scale))
						Updated = Scaled;
				}
				else
				{
					const glm::mat4 LocalMatrix = glm::inverse(Target.ParentWorldTransform) *
						WorldOperation * Target.WorldTransform;
					Updated = DecomposeLocalTransform(LocalMatrix);
				}
				if (!Entity || !Updated)
				{
					SetError("The multi-object transform would require an unsupported shear or singular transform.");
					CancelGizmoInteraction();
					return;
				}
				if (auto Result = Entity->SetTransform(*Updated); !Result)
				{
					SetError("Viewport transform manipulation failed: " + Result.error().Message);
					CancelGizmoInteraction();
					return;
				}
			}
			m_SceneDirty = true;
		}

		[[nodiscard]] static bool TransformsDiffer(
			const PulseForge::TransformComponent& First,
			const PulseForge::TransformComponent& Second)
		{
			constexpr float Epsilon = 1.0e-6f;
			if (glm::length(First.Translation - Second.Translation) > Epsilon ||
				glm::length(First.Scale - Second.Scale) > Epsilon)
				return true;
			const glm::quat FirstRotation = glm::normalize(First.Rotation);
			const glm::quat SecondRotation = glm::normalize(Second.Rotation);
			return 1.0f - std::abs(glm::dot(FirstRotation, SecondRotation)) > Epsilon;
		}

		void CancelGizmoInteraction() noexcept
		{
			if (m_GizmoDrag)
				CancelSceneEdit();
			m_GizmoDrag.reset();
		}

		void CompleteReleasedGizmoInteractionBeforeShortcuts()
		{
			if (!m_GizmoDrag)
				return;

			const ImGuiIO& IO = ImGui::GetIO();
			const auto Outcome = PulseForgeEditor::GetViewportGizmoPointerOutcome({
				m_GizmoDrag.has_value(),
				IO.MouseDown[ImGuiMouseButton_Left],
				IO.MouseReleased[ImGuiMouseButton_Left]
			}, ImGui::IsKeyPressed(ImGuiKey_Escape, false));
			if (Outcome != PulseForgeEditor::ViewportGizmoPointerOutcome::Complete)
				return;

			// The viewport image normally consumes release later in this frame; finish first so
			// the history shortcut router and Edit menu observe the committed gesture immediately.
			UpdateGizmoDrag({ IO.MousePos.x, IO.MousePos.y });
			if (m_GizmoDrag)
				CompleteGizmoInteraction();
		}

		void CompleteGizmoInteraction()
		{
			if (m_GizmoDrag)
				FinishSceneEdit();
			m_GizmoDrag.reset();
		}

		[[nodiscard]] std::expected<const CpuPickingMesh*, std::string> GetCpuPickingMesh(
			const PulseForge::AssetID& Asset)
		{
			if (const auto Existing = m_PickingMeshCache.find(Asset); Existing != m_PickingMeshCache.end())
				return &Existing->second;
			if (const auto ExistingError = m_PickingMeshErrors.find(Asset); ExistingError != m_PickingMeshErrors.end())
				return std::unexpected(ExistingError->second);
			if (!m_Project)
				return std::unexpected("There is no open project for mesh picking");

			auto Imported = PulseForge::GltfMeshImporter::ImportStaticPrimitive(
				Asset, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
			if (!Imported)
			{
				const std::string Message = "Mesh " + Asset.ToString() + " could not be loaded for viewport picking: " +
					Imported.error().Message;
				m_PickingMeshErrors.emplace(Asset, Message);
				return std::unexpected(Message);
			}

			CpuPickingMesh Geometry;
			Geometry.Positions.reserve(Imported->Vertices.size());
			for (const PulseForge::GltfMeshVertex& Vertex : Imported->Vertices)
			{
				const glm::vec3 Position{ Vertex.Position[0], Vertex.Position[1], Vertex.Position[2] };
				if (!PulseForgeEditor::Detail::IsFinite(Position))
					return std::unexpected("The imported mesh contains a non-finite vertex position");
				Geometry.Positions.push_back(Position);
			}
			Geometry.Indices = Imported->Indices;
			auto [Inserted, WasInserted] = m_PickingMeshCache.emplace(Asset, std::move(Geometry));
			(void)WasInserted;
			return &Inserted->second;
		}

		void PickEntityAt(const glm::vec2& Mouse, bool Control, bool Shift)
		{
			if (!m_Scene || m_SceneRuntime || !m_ViewportViewProjectionValid)
			{
				SelectSingleEntity(std::nullopt);
				return;
			}
			const auto Ray = PulseForgeEditor::MakeViewportRay(Mouse, m_ViewportImageRect, m_ViewportViewProjection);
			if (!Ray)
				return;
			auto Snapshot = PulseForge::SceneRenderSnapshotBuilder::BuildForView(
				*m_Scene, m_ViewportViewProjection, m_EditorCamera.GetPosition());
			if (!Snapshot)
			{
				SetError("Viewport picking could not inspect the scene: " + Snapshot.error().Message);
				return;
			}
			std::erase_if(Snapshot->Meshes, [this](const PulseForge::SceneMeshInstance& Instance)
			{
				const auto Entity = m_Scene->FindEntity(Instance.Entity);
				return !Entity || !IsEntityEffectivelyVisible(*Entity);
			});

			std::vector<PulseForgeEditor::PickableMesh> Meshes;
			Meshes.reserve(Snapshot->Meshes.size());
			for (const PulseForge::SceneMeshInstance& Instance : Snapshot->Meshes)
			{
				const auto Geometry = GetCpuPickingMesh(Instance.MeshAsset);
				if (!Geometry)
				{
					SetWarning(Geometry.error());
					continue;
				}
				Meshes.push_back({
					Instance.Entity,
					Instance.WorldTransform,
					(*Geometry)->Positions,
					(*Geometry)->Indices });
			}
			const auto Hit = PulseForgeEditor::RaycastMeshes(*Ray, Meshes);
			if (Hit)
			{
				if (Control)
					ToggleEntitySelection(Hit->Entity);
				else if (Shift)
				{
					m_Selection.Add(Hit->Entity);
					m_SelectedEntity = m_Selection.GetPrimary();
				}
				else
					SelectSingleEntity(Hit->Entity);
			}
			else if (!Control && !Shift)
				SelectSingleEntity(std::nullopt);
		}

		void SnapSelectionToSurface()
		{
			if (!m_Project || !m_Scene || m_SceneRuntime || m_Selection.GetEntities().empty())
				return;
			auto Roots = GetTopLevelSelectedEntities();
			if (!Roots || Roots->empty())
				return;
			auto MovingSubtrees = CollectEntitySubtrees(*Roots);
			if (!MovingSubtrees)
			{
				SetError("Could not inspect the selected hierarchy for surface snapping: " + MovingSubtrees.error());
				return;
			}
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> ExcludedEntities;
			for (const PulseForge::Entity& Entity : *MovingSubtrees)
				ExcludedEntities.insert(Entity.GetUUID());

			std::vector<PulseForgeEditor::PickableMesh> CandidateMeshes;
			for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
			{
				if (ExcludedEntities.contains(Entity.GetUUID()) || !IsEntityEffectivelyVisible(Entity))
					continue;
				const auto MeshRenderer = Entity.GetMeshRenderer();
				if (!MeshRenderer || !MeshRenderer->has_value())
					continue;
				const auto World = Entity.GetWorldMatrix();
				if (!World || !PulseForgeEditor::Detail::IsFinite(*World))
					continue;
				auto Geometry = GetCpuPickingMesh(MeshRenderer->value().MeshAsset);
				if (!Geometry)
				{
					SetWarning(Geometry.error());
					continue;
				}
				CandidateMeshes.push_back({
					Entity.GetUUID(), *World, (*Geometry)->Positions, (*Geometry)->Indices });
			}
			if (CandidateMeshes.empty())
			{
				SetStatus("No visible mesh surfaces are available below the selection.");
				return;
			}

			std::vector<std::pair<PulseForge::UUID, PulseForge::TransformComponent>> UpdatedTransforms;
			constexpr size_t MaximumContactCandidates = 2048;
			constexpr size_t MaximumContactSamples = 128;
			for (const PulseForge::Entity& Root : *Roots)
			{
				auto Subtree = CollectEntitySubtrees(std::vector<PulseForge::Entity>{ Root });
				if (!Subtree)
				{
					SetError("Could not inspect a selected hierarchy for surface snapping: " + Subtree.error());
					return;
				}
				std::vector<glm::vec3> ContactPoints;
				for (const PulseForge::Entity& Entity : *Subtree)
				{
					if (!IsEntityEffectivelyVisible(Entity))
						continue;
					const auto MeshRenderer = Entity.GetMeshRenderer();
					if (!MeshRenderer || !MeshRenderer->has_value())
						continue;
					const auto World = Entity.GetWorldMatrix();
					if (!World || !PulseForgeEditor::Detail::IsFinite(*World))
						continue;
					auto Geometry = GetCpuPickingMesh(MeshRenderer->value().MeshAsset);
					if (!Geometry)
						continue;
					for (const glm::vec3& Vertex : (*Geometry)->Positions)
					{
						const glm::vec3 Position(*World * glm::vec4(Vertex, 1.0f));
						if (!PulseForgeEditor::Detail::IsFinite(Position))
							continue;
						if (ContactPoints.size() < MaximumContactCandidates)
							if (std::none_of(ContactPoints.begin(), ContactPoints.end(), [&Position](const glm::vec3& Existing)
							{
								const glm::vec3 Difference = Existing - Position;
								return glm::dot(Difference, Difference) < 1.0e-6f;
							}))
								ContactPoints.push_back(Position);
					}
				}
				if (ContactPoints.empty())
				{
					if (const auto World = Root.GetWorldMatrix(); World && PulseForgeEditor::Detail::IsFinite(*World))
						ContactPoints.push_back(glm::vec3((*World)[3]));
				}
				if (ContactPoints.size() > MaximumContactSamples)
				{
					std::vector<glm::vec3> Samples;
					const size_t Stride = (ContactPoints.size() + MaximumContactSamples - 1) / MaximumContactSamples;
					for (size_t Index = 0; Index < ContactPoints.size(); Index += Stride)
						Samples.push_back(ContactPoints[Index]);
					ContactPoints = std::move(Samples);
				}

				float BestDeltaY = -(std::numeric_limits<float>::infinity)();
				for (const glm::vec3& Point : ContactPoints)
				{
					constexpr float RayStartOffset = 0.005f;
					const auto Hit = PulseForgeEditor::RaycastSupportingSurface(
						{ Point + glm::vec3(0.0f, RayStartOffset, 0.0f), { 0.0f, -1.0f, 0.0f } },
						CandidateMeshes);
					if (!Hit)
						continue;
					const float DeltaY = Hit->Position.y - Point.y;
					if (DeltaY <= RayStartOffset)
						BestDeltaY = (std::max)(BestDeltaY, DeltaY);
				}
				if (!std::isfinite(BestDeltaY) || BestDeltaY >= -0.001f)
					continue;

				const auto Transform = Root.GetTransform();
				const auto World = Root.GetWorldMatrix();
				const auto Parent = Root.GetParent();
				if (!Transform || !World || !Parent)
					continue;
				glm::mat4 ParentWorld(1.0f);
				if (Parent->has_value())
				{
					const auto ParentTransform = Parent->value().GetWorldMatrix();
					if (!ParentTransform || !PulseForgeEditor::Detail::IsFinite(*ParentTransform) ||
						std::abs(glm::determinant(*ParentTransform)) < 1.0e-8f)
						continue;
					ParentWorld = *ParentTransform;
				}
				const glm::vec3 WorldPosition((*World)[3]);
				const glm::vec3 NewWorldPosition = WorldPosition + glm::vec3(0.0f, BestDeltaY, 0.0f);
				PulseForge::TransformComponent Updated = *Transform;
				Updated.Translation = glm::vec3(glm::inverse(ParentWorld) * glm::vec4(NewWorldPosition, 1.0f));
				if (!PulseForgeEditor::Detail::IsFinite(Updated.Translation))
					continue;
				UpdatedTransforms.emplace_back(Root.GetUUID(), Updated);
			}

			if (UpdatedTransforms.empty())
			{
				SetStatus("No supporting surface was found below the selection.");
				return;
			}
			const auto Recorded = RunSceneEdit("Snap Selection to Surface", [this, UpdatedTransforms]()
				-> std::expected<void, PulseForge::SceneError>
			{
				for (const auto& [Identifier, Transform] : UpdatedTransforms)
				{
					const auto Entity = m_Scene->FindEntity(Identifier);
					if (!Entity)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidEntity,
							"A selected entity disappeared during surface snapping" });
					if (auto Result = Entity->SetTransform(Transform); !Result)
						return std::unexpected(Result.error());
				}
				return {};
			});
			if (!Recorded)
				SetError("Surface snapping failed: " + Recorded.error());
			else
				SetStatus("Snapped " + std::to_string(UpdatedTransforms.size()) + " selected entities to supporting surfaces.");
		}

		void FocusSelection()
		{
			if (!m_Scene || m_SceneRuntime || m_Selection.GetEntities().empty())
				return;
			auto Roots = GetTopLevelSelectedEntities();
			if (!Roots || Roots->empty())
				return;
			auto Subtree = CollectEntitySubtrees(*Roots);
			if (!Subtree)
			{
				SetError("Could not frame the selection: " + Subtree.error());
				return;
			}
			glm::vec3 Minimum((std::numeric_limits<float>::max)());
			glm::vec3 Maximum(-(std::numeric_limits<float>::max)());
			bool HasBounds = false;
			for (const PulseForge::Entity& Entity : *Subtree)
			{
				if (!IsEntityEffectivelyVisible(Entity))
					continue;
				const auto World = Entity.GetWorldMatrix();
				if (!World || !PulseForgeEditor::Detail::IsFinite(*World))
					continue;
				const auto MeshRenderer = Entity.GetMeshRenderer();
				if (!MeshRenderer || !MeshRenderer->has_value())
					continue;
				auto Geometry = GetCpuPickingMesh(MeshRenderer->value().MeshAsset);
				if (!Geometry)
				{
					SetWarning(Geometry.error());
					continue;
				}
				for (const glm::vec3& Vertex : (*Geometry)->Positions)
				{
					const glm::vec3 Position(*World * glm::vec4(Vertex, 1.0f));
					if (!PulseForgeEditor::Detail::IsFinite(Position))
						continue;
					for (glm::length_t Axis = 0; Axis < 3; ++Axis)
					{
						Minimum[Axis] = (std::min)(Minimum[Axis], Position[Axis]);
						Maximum[Axis] = (std::max)(Maximum[Axis], Position[Axis]);
					}
					HasBounds = true;
				}
			}
			if (!HasBounds)
			{
				for (const PulseForge::Entity& Root : *Roots)
					if (const auto World = Root.GetWorldMatrix(); World && PulseForgeEditor::Detail::IsFinite(*World))
					{
						const glm::vec3 Position((*World)[3]);
						for (glm::length_t Axis = 0; Axis < 3; ++Axis)
						{
							Minimum[Axis] = (std::min)(Minimum[Axis], Position[Axis]);
							Maximum[Axis] = (std::max)(Maximum[Axis], Position[Axis]);
						}
						HasBounds = true;
					}
			}
			if (!HasBounds)
				return;
			const glm::vec3 Center = (Minimum + Maximum) * 0.5f;
			const float Radius = (std::max)(glm::length(Maximum - Minimum) * 0.5f, 0.35f);
			const float AspectRatio = m_ViewportHeight > 0
				? static_cast<float>(m_ViewportWidth) / static_cast<float>(m_ViewportHeight)
				: 1.0f;
			m_EditorCamera.Focus(Center, Radius, AspectRatio);
			SetStatus("Focused the selection.");
		}

		std::expected<void, std::string> EnsureViewportTarget(const ImVec2& LogicalSize)
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			(void)LogicalSize;
			return std::unexpected("The OpenGL fallback does not support the engine scene viewport");
#else
			const ImVec2 FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
			const double WidthInPixels = std::ceil(static_cast<double>(LogicalSize.x) *
				(FramebufferScale.x > 0.0f ? FramebufferScale.x : 1.0f));
			const double HeightInPixels = std::ceil(static_cast<double>(LogicalSize.y) *
				(FramebufferScale.y > 0.0f ? FramebufferScale.y : 1.0f));
			constexpr uint32_t ResolutionQuantum = 32;
			if (!std::isfinite(WidthInPixels) || !std::isfinite(HeightInPixels) ||
				WidthInPixels < 1.0 || HeightInPixels < 1.0 ||
				WidthInPixels > (std::numeric_limits<uint32_t>::max)() - ResolutionQuantum ||
				HeightInPixels > (std::numeric_limits<uint32_t>::max)() - ResolutionQuantum)
			{
				return std::unexpected("Viewport dimensions are outside the supported range");
			}

			const auto Quantize = [ResolutionQuantum](double Dimension)
			{
				return static_cast<uint32_t>(std::ceil(Dimension / ResolutionQuantum) * ResolutionQuantum);
			};
			const uint32_t Width = Quantize(WidthInPixels);
			const uint32_t Height = Quantize(HeightInPixels);
			if (m_ViewportTarget && m_ViewportWidth == Width && m_ViewportHeight == Height)
				return {};

			PulseForge::RenderTargetDesc Description;
			Description.Width = Width;
			Description.Height = Height;
			Description.ColorFormat = PulseForge::ColorTargetFormat::RGBA8_Srgb;
			Description.DebugName = "PulseForge editor scene viewport";
			auto Created = PulseForge::Application::Get().CreateRenderTarget(Description);
			if (!Created)
				return std::unexpected(Created.error().Message);

			if (!m_ImGuiRenderer)
				return std::unexpected("The Vulkan editor UI renderer is unavailable");
			const PulseForge::Texture* ColorTexture = (*Created)->GetColorTexture();
			if (!ColorTexture)
				return std::unexpected("The editor scene viewport requires a color attachment");
			auto TextureID = m_ImGuiRenderer->RegisterTexture(*ColorTexture);
			if (!TextureID)
				return std::unexpected(TextureID.error());

			if (m_ViewportTextureID != 0)
				m_ImGuiRenderer->UnregisterTexture(m_ViewportTextureID);
			CancelGizmoInteraction();
			m_ViewportTarget = std::move(*Created);
			m_ViewportTextureID = *TextureID;
			m_ViewportWidth = Width;
			m_ViewportHeight = Height;
			m_ViewportSceneReady = false;
			return {};
#endif
		}

		void PrepareViewportScene()
		{
			m_ViewportSceneReady = false;
			m_ViewportViewProjectionValid = false;
			m_EnvironmentLightingPending = false;
#ifdef PF_EDITOR_RENDERER_OPENGL
			return;
#else
			if (!m_Project || !m_Scene || m_ViewportWidth == 0 || m_ViewportHeight == 0)
				return;
			if (m_SceneRendererInitializationFailed)
				return;

			if (!m_SceneRenderer)
			{
				auto Created = PulseForge::SceneRenderer::Create(
					PulseForge::Application::Get(),
					*m_Project,
					std::filesystem::path(PF_EDITOR_SHADER_DIRECTORY));
				if (!Created)
				{
					m_SceneRendererInitializationFailed = true;
					SetViewportSceneError("Could not initialize the scene renderer: " + Created.error().Message);
					return;
				}
				m_SceneRenderer = std::move(*Created);
			}

			const PulseForge::Scene& ActiveScene = m_SceneRuntime && m_RuntimeScene ? *m_RuntimeScene : *m_Scene;
			const float AspectRatio = static_cast<float>(m_ViewportWidth) / static_cast<float>(m_ViewportHeight);
			std::expected<void, PulseForge::SceneRendererError> Prepared;
			if (m_SceneRuntime && m_RuntimeScene)
				Prepared = m_SceneRenderer->PrepareScene(ActiveScene, AspectRatio);
			else
			{
				auto ViewProjection = m_EditorCamera.GetViewProjection(AspectRatio);
				if (!ViewProjection)
				{
					SetViewportSceneError("Could not prepare the editor camera: " + ViewProjection.error());
					return;
				}
				m_ViewportViewProjection = *ViewProjection;
				auto Projection = m_EditorCamera.GetProjection(AspectRatio);
				if (!Projection)
				{
					SetViewportSceneError("Could not prepare the editor camera projection: " + Projection.error());
					return;
				}
				const std::vector<PulseForge::UUID> HiddenEntities = GetEffectivelyHiddenEntities();
				Prepared = m_SceneRenderer->PrepareSceneFiltered(
					ActiveScene, m_EditorCamera.GetView(), *Projection, m_EditorCamera.GetPosition(),
					0.1f, 1000.0f, HiddenEntities);
			}
			if (!Prepared)
			{
				SetViewportSceneError("Could not prepare the scene viewport: " + Prepared.error().Message);
				return;
			}
			ClearViewportSceneError();
			m_ViewportViewProjectionValid = !m_SceneRuntime && m_Scene != nullptr;
			m_ViewportSceneReady = true;
			m_EnvironmentLightingPending = m_SceneRenderer->IsEnvironmentLightingPending();
			if (m_ProjectOpenTimingActive && !m_LoggedFirstViewportPrepared)
			{
				m_LoggedFirstViewportPrepared = true;
				const double Milliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - m_ProjectOpenStartedAt).count();
				PF_INFO("Profile: Project open to first renderable viewport preparation took {:.2f} ms", Milliseconds);
			}
			if (m_ProjectOpenTimingActive && m_SceneRenderer->HasPreparedEnvironmentLighting() && !m_LoggedEnvironmentReady)
			{
				m_LoggedEnvironmentReady = true;
				const double Milliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - m_ProjectOpenStartedAt).count();
				PF_INFO("Profile: Project open to HDR environment-ready viewport preparation took {:.2f} ms", Milliseconds);
			}
#endif
		}

		void RenderViewportScene()
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			return;
#else
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport) || !m_ViewportTarget)
				return;

			if (m_SceneRenderer && m_ViewportSceneReady)
			{
				PulseForge::ToneMappingSettings ToneSettings;
				ToneSettings.ExposureEV = m_ExposureEV;
				if (!m_SceneRenderer->SetToneMappingSettings(ToneSettings))
				{
					SetViewportSceneError("The viewport exposure value is outside the supported range");
					return;
				}
				if (auto Rendered = m_SceneRenderer->RenderPreparedSceneToOutput(*m_ViewportTarget); !Rendered)
				{
					SetViewportSceneError("Scene viewport rendering failed: " + Rendered.error().Message);
					PulseForge::RenderTargetClearValue Clear;
					const auto Background = PulseForgeEditor::GetEditorColorValue(
						PulseForgeEditor::EditorColorToken::ViewportBackground);
					Clear.Color = { Background.Red, Background.Green, Background.Blue, Background.Alpha };
					if (const auto Begin = PulseForge::Application::Get().BeginRenderTarget(*m_ViewportTarget, Clear); Begin)
					{
						if (const auto End = PulseForge::Application::Get().EndRenderTarget(); !End)
							SetViewportSceneError("Could not clear the scene viewport after rendering failed: " + End.error().Message);
					}
					else
						SetViewportSceneError("Could not clear the scene viewport after rendering failed: " + Begin.error().Message);
				}
				else
				{
					ClearViewportSceneError();
					if (m_ProjectOpenTimingActive && !m_LoggedFirstViewportRendered)
					{
						m_LoggedFirstViewportRendered = true;
						const double Milliseconds = std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() - m_ProjectOpenStartedAt).count();
						PF_INFO("Profile: Project open to first rendered viewport frame took {:.2f} ms", Milliseconds);
					}
				}
				return;
			}

			PulseForge::RenderTargetClearValue Clear;
			const PulseForgeEditor::EditorColorValue ViewportBackground =
				PulseForgeEditor::GetEditorColorValue(PulseForgeEditor::EditorColorToken::ViewportBackground);
			Clear.Color = {
				ViewportBackground.Red,
				ViewportBackground.Green,
				ViewportBackground.Blue,
				ViewportBackground.Alpha
			};
			const auto Begin = PulseForge::Application::Get().BeginRenderTarget(*m_ViewportTarget, Clear);
			if (!Begin)
			{
				SetViewportSceneError("Could not clear the scene viewport: " + Begin.error().Message);
				return;
			}
			if (const auto End = PulseForge::Application::Get().EndRenderTarget(); !End)
				SetViewportSceneError("Could not finish clearing the scene viewport: " + End.error().Message);
#endif
		}

		void SetViewportTargetError(std::string Message)
		{
			if (m_ViewportTargetError == Message)
				return;
			m_ViewportTargetError = std::move(Message);
			PF_WARN("{}", m_ViewportTargetError);
		}

		void ClearViewportTargetError()
		{
			m_ViewportTargetError.clear();
		}

		void SetViewportSceneError(std::string Message)
		{
			if (m_ViewportSceneError == Message)
				return;
			m_ViewportSceneError = std::move(Message);
			PF_WARN("{}", m_ViewportSceneError);
		}

		void ClearViewportSceneError()
		{
			m_ViewportSceneError.clear();
		}

		void ResetViewportSceneRenderer() noexcept
		{
			CancelGizmoInteraction();
			ResetEditorViewportCamera();
			m_SceneRenderer.reset();
			m_ExposureEV = 0.0f;
			m_PickingMeshCache.clear();
			m_PickingMeshErrors.clear();
			m_ViewportSceneReady = false;
			m_SceneRendererInitializationFailed = false;
			ClearViewportSceneError();
		}

		bool BeginEditorCameraNavigation()
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			return false;
#else
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			ImGui::SetCurrentContext(m_Context);
			ImGuiIO& IO = ImGui::GetIO();
			if (IO.KeyAlt)
				return false;
			const bool Focused = Window && glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE;
			if (!m_ViewportMouseCapture.Begin(true, IsCursorOverViewportImage(), Focused,
				m_GizmoDrag.has_value(), m_SceneRuntime != nullptr))
				return false;

			m_PreviousCursorMode = glfwGetInputMode(Window, GLFW_CURSOR);
			glfwGetCursorPos(Window, &m_CursorRestoreX, &m_CursorRestoreY);
			m_CursorRestoreValid = std::isfinite(m_CursorRestoreX) && std::isfinite(m_CursorRestoreY);
			m_LastCursorX = static_cast<float>(m_CursorRestoreX);
			m_LastCursorY = static_cast<float>(m_CursorRestoreY);
			m_PreviousImGuiNoMouse = (IO.ConfigFlags & ImGuiConfigFlags_NoMouse) != 0;
			m_PreviousRawMouseMotion = false;
			m_RawMouseMotionChanged = glfwRawMouseMotionSupported() == GLFW_TRUE;
			if (m_RawMouseMotionChanged)
				m_PreviousRawMouseMotion = glfwGetInputMode(Window, GLFW_RAW_MOUSE_MOTION) == GLFW_TRUE;
			IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
			IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
			m_IgnoreFirstCursorDelta = true;
			m_EditorCameraNavigationActive = true;
			glfwSetInputMode(Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			if (m_RawMouseMotionChanged)
				glfwSetInputMode(Window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
			return true;
#endif
		}

		[[nodiscard]] bool IsCursorOverViewportImage() const
		{
			ImGui::SetCurrentContext(m_Context);
			const glm::vec2 Mouse{ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y };
			return PulseForgeEditor::Detail::IsFinite(Mouse) &&
				Mouse.x >= m_ViewportImageRect.Minimum.x && Mouse.x < m_ViewportImageRect.Maximum.x &&
				Mouse.y >= m_ViewportImageRect.Minimum.y && Mouse.y < m_ViewportImageRect.Maximum.y;
		}

		[[nodiscard]] bool IsNativeCursorOverViewportImage() const
		{
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			if (!Window)
				return false;
			double CursorX = 0.0;
			double CursorY = 0.0;
			int WindowX = 0;
			int WindowY = 0;
			glfwGetCursorPos(Window, &CursorX, &CursorY);
			glfwGetWindowPos(Window, &WindowX, &WindowY);
			const glm::vec2 Mouse{
				static_cast<float>(WindowX + CursorX),
				static_cast<float>(WindowY + CursorY)
			};
			return PulseForgeEditor::Detail::IsFinite(Mouse) &&
				Mouse.x >= m_ViewportImageRect.Minimum.x && Mouse.x < m_ViewportImageRect.Maximum.x &&
				Mouse.y >= m_ViewportImageRect.Minimum.y && Mouse.y < m_ViewportImageRect.Maximum.y;
		}

		void EndEditorCameraNavigation(bool RestoreCursorPosition = true) noexcept
		{
			if (!m_ViewportMouseCapture.OwnsMouse())
				return;

			m_ViewportMouseCapture.End();
			m_EditorCameraNavigationActive = false;
			m_IgnoreFirstCursorDelta = false;
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			if (Window)
			{
				if (m_RawMouseMotionChanged)
					glfwSetInputMode(Window, GLFW_RAW_MOUSE_MOTION, m_PreviousRawMouseMotion ? GLFW_TRUE : GLFW_FALSE);
				glfwSetInputMode(Window, GLFW_CURSOR, m_PreviousCursorMode);
				if (RestoreCursorPosition && m_CursorRestoreValid && glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE)
					glfwSetCursorPos(Window, m_CursorRestoreX, m_CursorRestoreY);
			}
			m_RawMouseMotionChanged = false;
			m_CursorRestoreValid = false;
			if (m_Context)
			{
				ImGui::SetCurrentContext(m_Context);
				ImGuiIO& IO = ImGui::GetIO();
				if (!m_PreviousImGuiNoMouse)
					IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
				IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
			}
		}

		void ResetEditorViewportCamera() noexcept
		{
			EndEditorCameraNavigation();
			m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
				m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::ViewportUnavailable);
			m_EditorCamera.Reset();
		}

		void UpdateEditorCamera(PulseForge::Timestep DeltaTime)
		{
			if (m_SceneRuntime || !m_Scene)
			{
				EndEditorCameraNavigation();
				m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
					m_ViewportKeyboardFocused, m_SceneRuntime
						? PulseForgeEditor::ViewportKeyboardFocusEvent::PlayModeStarted
						: PulseForgeEditor::ViewportKeyboardFocusEvent::ViewportUnavailable);
				return;
			}
			const PulseForge::Input& Input = PulseForge::Application::Get().GetInput();
			const bool RightMouseHeld = Input.IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
			if (m_EditorCameraNavigationActive && !RightMouseHeld)
			{
				EndEditorCameraNavigation();
			}
			if (!m_Context)
				return;

			ImGui::SetCurrentContext(m_Context);
			const ImGuiIO& IO = ImGui::GetIO();
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			const bool ApplicationFocused = Window && glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE;
			const PulseForgeEditor::ViewportCameraNavigationContext Context{
				.SceneAvailable = m_Scene != nullptr,
				.RuntimeActive = m_SceneRuntime != nullptr,
				.ViewportKeyboardFocused = m_ViewportKeyboardFocused,
				.RightMouseNavigationActive = m_EditorCameraNavigationActive && RightMouseHeld,
				.ApplicationFocused = ApplicationFocused,
				.TextInputActive = IO.WantTextInput,
				.ActiveImGuiItem = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui->NavLayer == ImGuiNavLayer_Menu ||
					GImGui->NavWindowingTarget != nullptr,
				.ModifierHeld = IO.KeyCtrl || IO.KeyAlt || IO.KeySuper,
				.GizmoInteractionActive = m_GizmoDrag.has_value(),
				.CameraGestureActive = m_ViewportCameraGesture != ViewportCameraGesture::None
			};
			if (PulseForgeEditor::CanNavigateViewportCamera(Context))
				m_EditorCamera.Move(Input, DeltaTime.GetSeconds());
		}

		void ResetViewportTarget() noexcept
		{
			CancelGizmoInteraction();
			m_ViewportImageRect = {};
			if (m_ImGuiRenderer && m_ViewportTextureID != 0)
				m_ImGuiRenderer->UnregisterTexture(m_ViewportTextureID);
			m_ViewportTextureID = 0;
			m_ViewportTarget.reset();
			m_ViewportWidth = 0;
			m_ViewportHeight = 0;
			m_ViewportSceneReady = false;
			ClearViewportTargetError();
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

		ImVec4 LogLevelColor(spdlog::level::level_enum Level) const
		{
			switch (Level)
			{
			case spdlog::level::warn:
				return m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning);
			case spdlog::level::err:
			case spdlog::level::critical:
				return m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error);
			default:
				return m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::TextMuted);
			}
		}

		bool RefreshConsoleSnapshot()
		{
			if (!m_ConsoleSink)
				return false;
			uint64_t CurrentRevision = m_ConsoleRevision;
			if (!m_ConsoleSink->CopyMessagesIfChanged(m_ConsoleRevision, m_ConsoleMessages, CurrentRevision))
				return false;
			m_ConsoleRevision = CurrentRevision;
			return true;
		}

		std::string FormatConsoleMessages() const
		{
			std::vector<PulseForgeEditor::ConsoleEntryView> Entries;
			Entries.reserve(m_ConsoleMessages.size());
			for (const EditorConsoleMessage& Message : m_ConsoleMessages)
				Entries.push_back({ LogLevelName(Message.Level), Message.Logger, Message.Text });
			return PulseForgeEditor::FormatConsoleHistory(Entries);
		}

		std::string FormatConsoleMessage(const EditorConsoleMessage& Message) const
		{
			return PulseForgeEditor::FormatConsoleEntry({ LogLevelName(Message.Level), Message.Logger, Message.Text });
		}

		void DrawConsolePanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Console))
				return;
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Console))
			{
				if (!m_ConsoleSink)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Warning,
						"The editor could not connect to the engine loggers.");
				else
				{
					// Refresh before toolbar actions so Copy All uses a coherent current sink snapshot.
					RefreshConsoleSnapshot();
					if (m_EditorStyle.BeginToolbar("ConsoleToolbar",
						PulseForgeEditor::EditorStyleMetrics::PanelToolbarHeight))
					{
						if (m_EditorStyle.SmallButton(PulseForgeEditor::EditorIcon::Clear, "Clear"))
						{
							m_ConsoleSink->Clear();
							RefreshConsoleSnapshot();
							m_ConsoleSelectableText.clear();
							m_ConsoleSelectableTextRevision = m_ConsoleRevision;
							m_ConsoleSelectableTextInitialized = true;
							m_ConsoleSelectableTextInteracting = false;
							m_ConsoleSelectableAutoScrollSuspended = false;
						}
						ImGui::SameLine();
						if (ImGui::Checkbox("Auto-scroll", &m_ConsoleAutoScroll) && m_ConsoleAutoScroll)
							m_ConsoleSelectableAutoScrollSuspended = false;
						ImGui::SameLine();
						ImGui::BeginDisabled(m_ConsoleMessages.empty());
						if (ImGui::Button("Copy All"))
						{
							const std::string Formatted = FormatConsoleMessages();
							if (!Formatted.empty())
								ImGui::SetClipboardText(Formatted.c_str());
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
						if (ImGui::Checkbox("Select text", &m_ConsoleSelectableView))
							m_ConsoleSelectableTextInteracting = false;
					}
					m_EditorStyle.EndToolbar();

					if (ImGui::BeginChild("ConsoleMessages", ImVec2(0.0f, 0.0f), true,
						ImGuiWindowFlags_HorizontalScrollbar))
					{
						[[maybe_unused]] const auto MonospaceFont = m_EditorStyle.PushMonospaceFont();
						if (m_ConsoleMessages.empty())
							m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Terminal,
								"No console messages yet.");
						else if (m_ConsoleSelectableView)
						{
							const bool TextViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
							if (TextViewHovered && ImGui::GetIO().MouseWheel != 0.0f)
								m_ConsoleSelectableAutoScrollSuspended = true;
							const bool CanReplaceText = !m_ConsoleSelectableTextInteracting;
							const bool TextNeedsRefresh = !m_ConsoleSelectableTextInitialized ||
								m_ConsoleSelectableTextRevision != m_ConsoleRevision;
							const bool TextChanged = CanReplaceText && TextNeedsRefresh;
							if (TextChanged)
							{
								m_ConsoleSelectableText = FormatConsoleMessages();
								m_ConsoleSelectableTextRevision = m_ConsoleRevision;
								m_ConsoleSelectableTextInitialized = true;
							}
							if (TextChanged && m_ConsoleAutoScroll && !m_ConsoleSelectableAutoScrollSuspended)
								ImGui::SetNextWindowScroll(ImVec2(-1.0f, FLT_MAX));

							const ImVec2 TextViewSize = ImGui::GetContentRegionAvail();
							ImGui::InputTextMultiline(
								"##ConsoleSelectableText",
								m_ConsoleSelectableText.data(),
								m_ConsoleSelectableText.size() + 1,
								TextViewSize,
								ImGuiInputTextFlags_ReadOnly);
							const bool TextViewActive = ImGui::IsItemActive();
							m_ConsoleSelectableTextInteracting = TextViewActive;
						}
						else
						{
							const bool WasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
							const float SourceWidth = std::clamp(
								ImGui::GetContentRegionAvail().x * 0.20f, 90.0f, 156.0f);
							if (ImGui::BeginTable("ConsoleMessageRows", 3,
								ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
								ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings))
							{
								ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 88.0f);
								ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, SourceWidth);
								ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 1.0f);
								ImGuiListClipper Clipper;
								Clipper.Begin(static_cast<int>(m_ConsoleMessages.size()));
								while (Clipper.Step())
								{
									for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
									{
										const EditorConsoleMessage& Message = m_ConsoleMessages[static_cast<size_t>(Index)];
										ImGui::PushID(static_cast<int>(Message.Sequence >> 32));
										ImGui::PushID(static_cast<int>(Message.Sequence & 0xffffffffu));
										ImGui::TableNextRow();
										ImGui::TableNextColumn();
										char SeverityLabel[16]{};
										std::snprintf(SeverityLabel, sizeof(SeverityLabel), "[%s]", LogLevelName(Message.Level));
										ImGui::PushStyleColor(ImGuiCol_Text, LogLevelColor(Message.Level));
										ImGui::Selectable(SeverityLabel, false,
											ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
											ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing()));
										ImGui::PopStyleColor();
										if (ImGui::BeginPopupContextItem("ConsoleEntryContextMenu"))
										{
											if (ImGui::MenuItem("Copy Message"))
												ImGui::SetClipboardText(Message.Text.c_str());
											if (ImGui::MenuItem("Copy Entry"))
											{
												const std::string Formatted = FormatConsoleMessage(Message);
												ImGui::SetClipboardText(Formatted.c_str());
											}
											ImGui::EndPopup();
										}
										ImGui::TableNextColumn();
										const float LoggerWidth = ImGui::GetContentRegionAvail().x;
										m_EditorStyle.TextMuted(Message.Logger);
										if (ImGui::IsItemHovered() && ImGui::CalcTextSize(Message.Logger.c_str()).x > LoggerWidth)
											ImGui::SetTooltip("%s", Message.Logger.c_str());
										ImGui::TableNextColumn();
										ImGui::TextUnformatted(
											Message.Text.data(), Message.Text.data() + Message.Text.size());
										if (ImGui::IsItemHovered())
											ImGui::SetTooltip("%s", Message.Text.c_str());
										ImGui::PopID();
										ImGui::PopID();
									}
								}
								ImGui::EndTable();
							}
							if (m_ConsoleAutoScroll && WasAtBottom)
								ImGui::SetScrollHereY(1.0f);
						}
					}
					ImGui::EndChild();
				}
			}
			ImGui::End();
		}

		void DrawHierarchyPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy))
				return;
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Hierarchy))
			{
				m_HierarchyVisibleOrderCurrent.clear();
				if (m_EditorStyle.BeginToolbar("HierarchyToolbar", 44.0f))
				{
					ImGui::BeginDisabled(!m_Scene);
					if (m_EditorStyle.IconButton(
						PulseForgeEditor::EditorIcon::Add, "Create", "CreateEntity", "Create a new entity", false, 38.0f, 34.0f))
						CreateEntityFromEditor();
					ImGui::SameLine();
					ImGui::BeginDisabled(!m_SelectedEntity);
					if (m_EditorStyle.IconButton(
						PulseForgeEditor::EditorIcon::Duplicate, "Duplicate", "DuplicateEntity", "Duplicate selected entity",
						false, 38.0f, 34.0f))
						DuplicateSelectedEntity();
					ImGui::SameLine();
					if (m_EditorStyle.IconButton(
						PulseForgeEditor::EditorIcon::Delete, "Delete", "DeleteEntity", "Delete selected entity", true,
						34.0f, 32.0f))
						DeleteSelectedEntity();
					ImGui::SameLine();
					if (m_EditorStyle.IconButton(
						PulseForgeEditor::EditorIcon::Prefab,
						"Prefab",
						"CreatePrefab",
						"Create a prefab from the selection",
						false,
						38.0f,
						34.0f))
						CreatePrefabFromSelectedEntity();
					ImGui::EndDisabled();
					ImGui::EndDisabled();
				}
				m_EditorStyle.EndToolbar();
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 7.0f));
				if (m_FocusHierarchySearchNextFrame)
				{
					ImGui::SetKeyboardFocusHere();
					m_FocusHierarchySearchNextFrame = false;
				}
				ImGui::InputTextWithHint("##HierarchySearch", "Search hierarchy...", &m_HierarchySearch);
				ImGui::PopStyleVar();
				ImGui::Separator();

				if (!m_Scene)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Scene, "No scene is open.");
				else if (m_Scene->GetEntityCount() == 0)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Entity, "This scene has no entities.");
				else
				{
					std::function<void()> HierarchyAction;
					const std::vector<PulseForge::Entity> Entities = m_Scene->GetEntities();
					std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> VisibleEntities;
					if (!m_HierarchySearch.empty())
					{
						VisibleEntities.reserve(Entities.size());
						for (const PulseForge::Entity& Candidate : Entities)
						{
							const auto CandidateTag = Candidate.GetTag();
							if (!CandidateTag || !PulseForgeEditor::ContainsCaseInsensitive(
								CandidateTag->Name, m_HierarchySearch))
								continue;

							PulseForge::Entity Ancestor = Candidate;
							while (Ancestor)
							{
								if (!VisibleEntities.insert(Ancestor.GetUUID()).second)
									break;
								const auto Parent = Ancestor.GetParent();
								if (!Parent || !Parent->has_value())
									break;
								Ancestor = **Parent;
							}
						}
					}
					const auto* Visible = m_HierarchySearch.empty() ? nullptr : &VisibleEntities;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 6.0f));
					for (const PulseForge::Entity& Entity : Entities)
					{
						const auto Parent = Entity.GetParent();
						if (Parent && !Parent->has_value() && (!Visible || Visible->contains(Entity.GetUUID())))
							DrawEntityTree(Entity, HierarchyAction, Visible);
					}
					ImGui::PopStyleVar();
					if (HierarchyAction)
						HierarchyAction();
					if (Visible && Visible->empty())
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Search, "No entities match this search.");
				}
				m_HierarchyVisibleOrder = std::move(m_HierarchyVisibleOrderCurrent);
			}
			ImGui::End();
		}

		void CreateEntityFromEditor()
		{
			if (!m_Scene)
				return;
			const auto Recorded = RunSceneEdit("Create Entity", [this]() -> std::expected<void, PulseForge::SceneError>
			{
				auto Created = m_Scene->CreateEntity();
				if (!Created)
					return std::unexpected(Created.error());
				SelectSingleEntity(Created->GetUUID());
				return {};
			});
			if (!Recorded)
				SetError("Entity creation failed: " + Recorded.error());
		}

		[[nodiscard]] std::expected<std::vector<PulseForge::UUID>, PulseForge::SceneError>
		DuplicateEntityRootsRaw(const std::vector<PulseForge::Entity>& Roots)
		{
			std::vector<PulseForge::UUID> DuplicatedRoots;
			std::function<std::expected<PulseForge::Entity, PulseForge::SceneError>(
				const PulseForge::Entity&, std::optional<PulseForge::UUID>)> DuplicateSubtree;
			DuplicateSubtree = [this, &DuplicateSubtree](
				const PulseForge::Entity& Source,
				std::optional<PulseForge::UUID> DuplicatedParent)
				-> std::expected<PulseForge::Entity, PulseForge::SceneError>
			{
				auto Duplicated = m_Scene->DuplicateEntity(Source);
				if (!Duplicated)
					return std::unexpected(Duplicated.error());
				auto Tag = Duplicated->GetTag();
				if (!Tag)
					return std::unexpected(Tag.error());
				Tag->Name = MakeUniqueEntityName(Tag->Name, Duplicated->GetUUID());
				if (auto Result = Duplicated->SetTag(*Tag); !Result)
					return std::unexpected(Result.error());
				if (DuplicatedParent)
				{
					const auto Parent = m_Scene->FindEntity(*DuplicatedParent);
					if (!Parent)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidEntity,
							"A duplicated parent entity could not be resolved" });
					if (auto Result = Duplicated->SetParent(*Parent); !Result)
						return std::unexpected(Result.error());
				}

				auto Children = Source.GetChildren();
				if (!Children)
					return std::unexpected(Children.error());
				for (const PulseForge::Entity& Child : *Children)
					if (auto Result = DuplicateSubtree(Child, Duplicated->GetUUID()); !Result)
						return std::unexpected(Result.error());
				return *Duplicated;
			};

			for (const PulseForge::Entity& Root : Roots)
			{
				auto Duplicated = DuplicateSubtree(Root, std::nullopt);
				if (!Duplicated)
					return std::unexpected(Duplicated.error());
				DuplicatedRoots.push_back(Duplicated->GetUUID());
			}
			return DuplicatedRoots;
		}

		void DuplicateSelectedEntity()
		{
			if (!m_Scene || m_Selection.GetEntities().empty())
				return;
			auto Roots = GetTopLevelSelectedEntities();
			if (!Roots)
			{
				SetError("Could not duplicate the selection: " + Roots.error());
				return;
			}
			std::vector<PulseForge::UUID> DuplicatedRoots;
			const auto Recorded = RunSceneEdit("Duplicate Entities", [this, Roots = std::move(*Roots), &DuplicatedRoots]()
				-> std::expected<void, PulseForge::SceneError>
			{
				auto Result = DuplicateEntityRootsRaw(Roots);
				if (!Result)
					return std::unexpected(Result.error());
				DuplicatedRoots = std::move(*Result);
				SetEditorSelection(DuplicatedRoots);
				return {};
			});
			if (!Recorded)
				SetError("Entity duplication failed: " + Recorded.error());
			else
				SetStatus("Duplicated " + std::to_string(DuplicatedRoots.size()) + " selected hierarchy root(s).");
		}

		void DeleteSelectedEntity()
		{
			if (!m_Scene || m_Selection.GetEntities().empty())
				return;
			auto Roots = GetTopLevelSelectedEntities();
			if (!Roots)
			{
				SetError("Could not delete the selection: " + Roots.error());
				return;
			}
			auto Subtree = CollectEntitySubtrees(*Roots);
			if (!Subtree)
			{
				SetError("Could not delete the selection: " + Subtree.error());
				return;
			}
			const size_t DeleteCount = Subtree->size();
			const auto Recorded = RunSceneEdit("Delete Entities", [this, Subtree = std::move(*Subtree)]()
				-> std::expected<void, PulseForge::SceneError>
			{
				for (auto Entity = Subtree.rbegin(); Entity != Subtree.rend(); ++Entity)
				{
					const auto Target = m_Scene->FindEntity(Entity->GetUUID());
					if (!Target)
						continue;
					m_EditorVisibility.Remove(Entity->GetUUID());
					if (auto Deleted = m_Scene->DestroyEntity(*Target); !Deleted)
						return std::unexpected(Deleted.error());
				}
				SelectSingleEntity(std::nullopt);
				return {};
			});
			if (!Recorded)
				SetError("Entity deletion failed: " + Recorded.error());
			else
				SetStatus("Deleted " + std::to_string(DeleteCount) + " entities.");
		}

		[[nodiscard]] bool CopySelectedEntities()
		{
			if (!m_Project || !m_Scene || m_Selection.GetEntities().empty())
				return false;
			auto Roots = GetTopLevelSelectedEntities();
			if (!Roots)
			{
				SetError("Could not copy the selection: " + Roots.error());
				return false;
			}
			auto Subtree = CollectEntitySubtrees(*Roots);
			if (!Subtree)
			{
				SetError("Could not copy the selection: " + Subtree.error());
				return false;
			}
			auto Serialized = PulseForge::SceneSerializer::Serialize(*m_Scene);
			if (!Serialized)
			{
				SetError("Could not serialize the selection for the clipboard: " + Serialized.error().Message);
				return false;
			}

			try
			{
				const EditorJson Document = EditorJson::parse(*Serialized);
				std::unordered_set<std::string> IncludedIdentifiers;
				for (const PulseForge::Entity& Entity : *Subtree)
					IncludedIdentifiers.insert(Entity.GetUUID().ToString());
				EntityClipboard Clipboard;
				Clipboard.Project = m_Project->GetDescription().ID;
				Clipboard.Scene = m_SceneAsset;
				for (const PulseForge::Entity& Root : *Roots)
					Clipboard.Roots.push_back(Root.GetUUID());
				for (const EditorJson& Record : Document.at("entities"))
					if (IncludedIdentifiers.contains(Record.at("uuid").get<std::string>()))
						Clipboard.Records.push_back(Record);
				if (Clipboard.Records.size() != Subtree->size())
				{
					SetError("The serialized scene did not contain every selected hierarchy entity.");
					return false;
				}
				m_EntityClipboard = std::move(Clipboard);
				SetStatus("Copied " + std::to_string(Subtree->size()) + " entities.");
				return true;
			}
			catch (const std::exception& Exception)
			{
				SetError(std::string("Could not encode the selection for the clipboard: ") + Exception.what());
				return false;
			}
		}

		void CutSelectedEntities()
		{
			if (m_Selection.GetEntities().empty())
				return;
			if (CopySelectedEntities())
				DeleteSelectedEntity();
		}

		void PasteEntities()
		{
			if (!m_Project || !m_Scene || !m_EntityClipboard)
				return;
			if (m_EntityClipboard->Project != m_Project->GetDescription().ID)
			{
				SetWarning("The clipboard belongs to another project; copy the entities again in this project.");
				return;
			}
			if (m_SceneRuntime)
				return;

			auto Serialized = PulseForge::SceneSerializer::Serialize(*m_Scene);
			if (!Serialized)
			{
				SetError("Could not prepare the active scene for paste: " + Serialized.error().Message);
				return;
			}
			try
			{
				EditorJson Document = EditorJson::parse(*Serialized);
				const bool SameScene = m_EntityClipboard->Scene == m_SceneAsset;
				std::unordered_map<std::string, PulseForge::UUID> RemappedIdentifiers;
				std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> ExistingIdentifiers;
				std::unordered_set<std::string> ExistingNames;
				for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
				{
					ExistingIdentifiers.insert(Entity.GetUUID());
					if (const auto Tag = Entity.GetTag())
						ExistingNames.insert(Tag->Name);
				}
				for (const EditorJson& Record : m_EntityClipboard->Records)
				{
					const std::string OldIdentifier = Record.at("uuid").get<std::string>();
					auto GeneratedIdentifier = PulseForge::UUID::Generate();
					if (!GeneratedIdentifier)
						throw std::runtime_error(GeneratedIdentifier.error().Message);
					PulseForge::UUID NewIdentifier = *GeneratedIdentifier;
					while (ExistingIdentifiers.contains(NewIdentifier))
					{
						GeneratedIdentifier = PulseForge::UUID::Generate();
						if (!GeneratedIdentifier)
							throw std::runtime_error(GeneratedIdentifier.error().Message);
						NewIdentifier = *GeneratedIdentifier;
					}
					ExistingIdentifiers.insert(NewIdentifier);
					RemappedIdentifiers.emplace(OldIdentifier, NewIdentifier);
				}

				std::vector<EditorJson> NewRecords = m_EntityClipboard->Records;
				std::vector<PulseForge::UUID> NewRoots;
				for (EditorJson& Record : NewRecords)
				{
					const std::string OldIdentifier = Record.at("uuid").get<std::string>();
					const auto NewIdentifier = RemappedIdentifiers.at(OldIdentifier);
					Record["uuid"] = NewIdentifier.ToString();
					const auto Tag = Record.find("tag");
					if (Tag != Record.end() && Tag->is_object() && Tag->contains("name"))
					{
						const std::string BaseName = Tag->at("name").get<std::string>() + " Copy";
						std::string Name = BaseName;
						for (uint32_t Suffix = 2; ExistingNames.contains(Name); ++Suffix)
							Name = BaseName + " " + std::to_string(Suffix);
						ExistingNames.insert(Name);
						(*Tag)["name"] = std::move(Name);
					}

					const auto Parent = Record.find("parent");
					if (Parent != Record.end() && Parent->is_string())
					{
						const std::string OldParent = Parent->get<std::string>();
						if (const auto InternalParent = RemappedIdentifiers.find(OldParent);
							InternalParent != RemappedIdentifiers.end())
							*Parent = InternalParent->second.ToString();
						else
						{
							const auto ParentIdentifier = PulseForge::UUID::Parse(OldParent);
							if (!SameScene || !ParentIdentifier || !m_Scene->FindEntity(*ParentIdentifier))
								throw std::runtime_error(
									"This hierarchy has an external parent that is unavailable in the destination scene.");
							*Parent = OldParent;
						}
					}

					if (const auto Camera = Record.find("camera"); Camera != Record.end() && Camera->is_object())
						(*Camera)["primary"] = false;
					if (const auto Listener = Record.find("audioListener");
						Listener != Record.end() && Listener->is_object())
						(*Listener)["primary"] = false;

					const auto CheckAsset = [this](const EditorJson& Object, std::string_view Field)
						-> std::optional<std::string>
					{
						const auto Value = Object.find(Field);
						if (Value == Object.end())
						return std::nullopt;
						if (!Value->is_string())
						return "A copied asset reference is malformed.";
						const auto Identifier = PulseForge::UUID::Parse(Value->get<std::string>());
						if (!Identifier || !m_Project->GetAssetRegistry().Find(*Identifier))
							return "A copied entity references an asset that is not available in this project.";
						return std::nullopt;
					};
					for (const auto& [ComponentName, Fields] : std::array{
						std::pair<std::string_view, std::array<std::string_view, 2>>{
							"meshRenderer", { "meshAsset", "materialAsset" }},
						std::pair<std::string_view, std::array<std::string_view, 2>>{
							"environmentLight", { "hdrImage", "" }},
						std::pair<std::string_view, std::array<std::string_view, 2>>{
							"audioSource", { "asset", "" }},
						std::pair<std::string_view, std::array<std::string_view, 2>>{
							"script", { "asset", "" } }})
					{
						const auto Component = Record.find(ComponentName);
						if (Component == Record.end() || !Component->is_object())
							continue;
						for (const std::string_view Field : Fields)
							if (!Field.empty())
								if (const auto Error = CheckAsset(*Component, Field))
									throw std::runtime_error(*Error);
					}

					if (std::find_if(m_EntityClipboard->Roots.begin(), m_EntityClipboard->Roots.end(),
						[&](const PulseForge::UUID& Root) { return Root.ToString() == OldIdentifier; }) !=
						m_EntityClipboard->Roots.end())
						NewRoots.push_back(NewIdentifier);
				}

				for (EditorJson& Record : NewRecords)
					Document["entities"].push_back(std::move(Record));
				const std::string MergedScene = Document.dump(2) + "\n";
				const size_t PastedCount = NewRecords.size();
				const auto Recorded = RunSceneEdit("Paste Entities", [this, MergedScene, NewRoots]()
				{
					auto Result = PulseForge::SceneSerializer::Deserialize(MergedScene, *m_Scene);
					if (!Result)
						return Result;
					SetEditorSelection(NewRoots);
					return Result;
				});
				if (!Recorded)
				{
					SetError("Paste failed: " + Recorded.error());
					return;
				}
				SetStatus("Pasted " + std::to_string(PastedCount) + " entities.");
			}
			catch (const std::exception& Exception)
			{
				SetError(std::string("Paste failed: ") + Exception.what());
			}
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

			if (ParentIdentifier && !m_Scene->FindEntity(*ParentIdentifier))
			{
				SetError("The selected parent entity no longer exists.");
				return;
			}

			const auto Recorded = RunSceneEdit("Reparent Entity", [this, Child = *Child, ParentIdentifier]()
				-> std::expected<void, PulseForge::SceneError>
			{
				if (ParentIdentifier)
				{
					const auto Parent = m_Scene->FindEntity(*ParentIdentifier);
					if (!Parent)
						return std::unexpected(PulseForge::SceneError{
							PulseForge::SceneErrorCode::InvalidEntity, "The selected parent entity no longer exists" });
					return Child.SetParent(*Parent);
				}
				return Child.ClearParent();
			});
			if (!Recorded)
				SetError("Entity reparenting failed: " + Recorded.error());
		}

		void DrawEntityTree(
			const PulseForge::Entity& Entity,
			std::function<void()>& DeferredHierarchyAction,
			const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash>* VisibleEntities = nullptr)
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
			const PulseForgeEditor::EditorIcon Icon = GetEntityIcon(Entity);
			const std::string Identifier = Entity.GetUUID().ToString();
			const bool IsSelected = IsEntitySelected(Entity.GetUUID());
			m_HierarchyVisibleOrderCurrent.push_back(Entity.GetUUID());
			ImGui::PushID(Identifier.c_str());
			ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanLabelWidth;
			if (!Children || Children->empty())
				Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			Flags |= ImGuiTreeNodeFlags_FramePadding;

			if (VisibleEntities && Children && !Children->empty())
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
			const bool IsEffectivelyVisible = IsEntityEffectivelyVisible(Entity);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, IsEffectivelyVisible ? 1.0f : 0.48f);
			const float RowStartX = ImGui::GetCursorPosX();
			const float ContentRightX = ImGui::GetWindowContentRegionMax().x;
			const float LabelStartX = RowStartX + ImGui::GetTreeNodeToLabelSpacing() + ImGui::GetFontSize() +
				ImGui::CalcTextSize(" ").x;
			const ImGuiStyle& Style = ImGui::GetStyle();
			const auto InitialRowLayout = PulseForgeEditor::ComputeEditorHierarchyRowLayout(
				RowStartX,
				ContentRightX,
				LabelStartX,
				RowStartX,
				RowStartX,
				Style.FramePadding.x);
			const float MaximumNameWidth = InitialRowLayout.MaximumNameWidth;
			std::string TruncatedNameStorage;
			std::string_view DisplayName = Tag->Name;
			if (Tag->Name.size() > 480 ||
				ImGui::CalcTextSize(Tag->Name.data(), Tag->Name.data() + Tag->Name.size()).x > MaximumNameWidth)
			{
				TruncatedNameStorage = FitHierarchyEntityName(Tag->Name, MaximumNameWidth);
				DisplayName = TruncatedNameStorage;
			}
			const bool NameTruncated = !TruncatedNameStorage.empty() || DisplayName.size() != Tag->Name.size();
			const ImVec2 RowScreenMinimum = ImGui::GetCursorScreenPos();
			const float DisplayNameWidth = ImGui::CalcTextSize(DisplayName.data(), DisplayName.data() + DisplayName.size()).x;
			const float ExpectedTreeItemHitRightX = LabelStartX + DisplayNameWidth + Style.FramePadding.x;
			const auto BackgroundRowLayout = PulseForgeEditor::ComputeEditorHierarchyRowLayout(
				RowStartX,
				ContentRightX,
				LabelStartX,
				ExpectedTreeItemHitRightX,
				RowStartX,
				Style.FramePadding.x);
			const ImVec2 RowScreenMaximum(
				RowScreenMinimum.x + BackgroundRowLayout.SelectionBackgroundWidth,
				RowScreenMinimum.y + PulseForgeEditor::EditorHierarchyEyeButtonHeight);
			const bool SelectionRegionHovered = ImGui::IsWindowHovered() &&
				ImGui::IsMouseHoveringRect(RowScreenMinimum, RowScreenMaximum);
			if (IsSelected || SelectionRegionHovered)
			{
				const ImGuiCol Background = IsSelected ? ImGuiCol_Header : ImGuiCol_HeaderHovered;
				ImGui::GetWindowDrawList()->AddRectFilled(
					RowScreenMinimum,
					RowScreenMaximum,
					ImGui::GetColorU32(ImGui::GetStyleColorVec4(Background)));
			}
			const float RowFramePaddingY = (std::max)(
				0.0f, (PulseForgeEditor::EditorHierarchyEyeButtonHeight - ImGui::GetFontSize()) * 0.5f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Style.FramePadding.x, RowFramePaddingY));
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			const bool IsOpen = m_EditorStyle.TreeNode(Icon, DisplayName, "Entity", Flags);
			ImGui::PopStyleColor(3);
			ImGui::PopStyleVar();
			const bool TreeItemClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
			const bool TreeItemRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
			const bool TreeItemHovered = ImGui::IsItemHovered();
			const ImVec2 TreeItemRectMin = ImGui::GetItemRectMin();
			const ImVec2 TreeItemRectMax = ImGui::GetItemRectMax();
			const float TreeItemHitRightX = TreeItemRectMax.x - ImGui::GetWindowPos().x;
			if (TreeItemRightClicked)
				ImGui::OpenPopup("Entity Actions");

			ImGui::SameLine(0.0f, 0.0f);
			const auto RowLayout = PulseForgeEditor::ComputeEditorHierarchyRowLayout(
				RowStartX,
				ContentRightX,
				LabelStartX,
				TreeItemHitRightX,
				ImGui::GetCursorPosX(),
				Style.FramePadding.x);
			bool GutterClicked = false;
			bool GutterRightClicked = false;
			bool GutterHovered = false;
			if (RowLayout.SelectionGutterWidth > 0.0f)
			{
				ImGui::SetCursorScreenPos(ImVec2(
					ImGui::GetWindowPos().x + RowLayout.SelectionGutterStartX, TreeItemRectMin.y));
				(void)ImGui::InvisibleButton(
					"##EntitySelectionGutter",
					ImVec2(RowLayout.SelectionGutterWidth, PulseForgeEditor::EditorHierarchyEyeButtonHeight),
					ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_NoNavFocus);
				GutterClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
				GutterRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
				GutterHovered = ImGui::IsItemHovered();
			}
			const bool RowClicked = TreeItemClicked || GutterClicked;
			if (NameTruncated && (TreeItemHovered || GutterHovered) &&
				ImGui::GetIO().MousePos.x < ImGui::GetWindowPos().x + RowLayout.EyeStartX)
				ImGui::SetTooltip("%s", Tag->Name.c_str());
			if (TreeItemRightClicked || GutterRightClicked)
				ImGui::OpenPopup("Entity Actions");

			ImGui::SameLine(RowLayout.EyeStartX);
			const bool LocallyVisible = m_EditorVisibility.IsLocallyVisible(Entity.GetUUID());
			if (m_EditorStyle.IconButton(
				LocallyVisible ? PulseForgeEditor::EditorIcon::Eye : PulseForgeEditor::EditorIcon::EyeSlash,
				LocallyVisible ? "Visible" : "Hidden",
				"EntityVisibility",
				LocallyVisible ? "Hide this entity in the editor viewport" : "Show this entity in the editor viewport",
				false,
				PulseForgeEditor::EditorHierarchyEyeButtonWidth,
				PulseForgeEditor::EditorHierarchyEyeButtonHeight))
				ToggleEntityVisibility(Entity.GetUUID());
			ImGui::PopStyleVar();
			if (RowClicked)
			{
				const ImGuiIO& IO = ImGui::GetIO();
				if (IO.KeyShift)
					SelectHierarchyRange(Entity.GetUUID(), IO.KeyCtrl);
				else if (IO.KeyCtrl)
					ToggleEntitySelection(Entity.GetUUID());
				else
					SelectSingleEntity(Entity.GetUUID());
			}
			if (ImGui::BeginPopup("Entity Actions"))
			{
				if (!IsEntitySelected(Entity.GetUUID()))
					SelectSingleEntity(Entity.GetUUID());
				const PulseForge::UUID ChildIdentifier = Entity.GetUUID();
				const bool HasParent = Parent && Parent->has_value();
				if (ImGui::MenuItem("Make Root", nullptr, false, HasParent))
					DeferredHierarchyAction = [this, ChildIdentifier] { ReparentEntity(ChildIdentifier, std::nullopt); };
				if (ImGui::MenuItem(LocallyVisible ? "Hide in Viewport" : "Show in Viewport"))
					DeferredHierarchyAction = [this, ChildIdentifier] { ToggleEntityVisibility(ChildIdentifier); };

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
					if (!VisibleEntities || VisibleEntities->contains(Child.GetUUID()))
						DrawEntityTree(Child, DeferredHierarchyAction, VisibleEntities);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		void DrawInspectorPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Inspector))
				return;
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Inspector))
			{
				if (PulseForgeEditor::ShouldFinishSceneEditForInspector(
					m_SceneHistory.IsActive(), m_InspectorHistoryItem, GImGui ? GImGui->ActiveId : 0))
					FinishSceneEdit();
				if (!m_Scene || !m_SelectedEntity)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Entity, "Select an entity in the Hierarchy.");
				else if (m_Selection.GetEntities().size() > 1)
				{
					ImGui::Text("%zu entities selected", m_Selection.GetEntities().size());
					ImGui::Spacing();
					m_EditorStyle.TextMuted("The Inspector shows the active entity. Multi-object property editing is not available yet.");
				}
				else if (const auto Entity = m_Scene->FindEntity(*m_SelectedEntity))
				{
					auto Tag = Entity->GetTag();
					if (Tag)
					{
						if (m_EditorStyle.BeginToolbar("InspectorEntityHeader", 74.0f))
						{
							m_EditorStyle.IconText(GetEntityIcon(*Entity), {});
							ImGui::SameLine();
							const float NameWidth = (std::max)(ImGui::GetContentRegionAvail().x, 1.0f);
							ImGui::SetNextItemWidth(NameWidth);
							bool NameChanged = false;
							{
								[[maybe_unused]] const auto EmphasisFont = m_EditorStyle.PushEmphasisFont();
								if (m_FocusEntityNameNextFrame)
								{
									ImGui::SetKeyboardFocusHere();
									m_FocusEntityNameNextFrame = false;
								}
								NameChanged = ImGui::InputText("##EntityName", &Tag->Name);
							}
							if (NameChanged)
							{
								const auto Updated = RunSceneEdit("Rename Entity", [Entity = *Entity, Tag = *Tag]
									{ return Entity.SetTag(Tag); });
								if (!Updated)
									SetError("Entity name update failed: " + Updated.error());
							}
							ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 29.0f);
							m_EditorStyle.TextMuted(Entity->GetUUID().ToString());
						}
						m_EditorStyle.EndToolbar();
						ImGui::Spacing();
					}

					if (const auto Transform = Entity->GetTransform())
					{
						ImGui::Spacing();
						if (m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Translate, "Transform", true))
						{
							m_EditorStyle.BeginComponentBody("TransformComponentBody");
							if (m_EditorStyle.BeginPropertyTable("TransformProperties"))
							{
								float Translation[3]{ Transform->Translation.x, Transform->Translation.y, Transform->Translation.z };
								const glm::vec3 RotationDegrees = glm::degrees(glm::eulerAngles(Transform->Rotation));
								float Rotation[3]{ RotationDegrees.x, RotationDegrees.y, RotationDegrees.z };
								float Scale[3]{ Transform->Scale.x, Transform->Scale.y, Transform->Scale.z };
								const bool TranslationChanged = m_EditorStyle.Vector3PropertyRow(
									"Position", Translation, 0.05f, "%.3f");
								const bool RotationChanged = m_EditorStyle.Vector3PropertyRow(
									"Rotation", Rotation, 0.5f, "%.1f");
								const bool ScaleChanged = m_EditorStyle.Vector3PropertyRow(
									"Scale", Scale, 0.05f, "%.3f");
								m_EditorStyle.EndPropertyTable();
								if (TranslationChanged || RotationChanged || ScaleChanged)
								{
									PulseForge::TransformComponent UpdatedTransform = *Transform;
									UpdatedTransform.Translation = { Translation[0], Translation[1], Translation[2] };
									UpdatedTransform.Rotation = glm::quat(glm::radians(
										glm::vec3(Rotation[0], Rotation[1], Rotation[2])));
									UpdatedTransform.Scale = { Scale[0], Scale[1], Scale[2] };
									const auto Updated = RunSceneEdit("Edit Transform", [Entity = *Entity, UpdatedTransform]
										{ return Entity.SetTransform(UpdatedTransform); });
									if (!Updated)
									SetError("Entity transform update failed: " + Updated.error());
								}
							}
							m_EditorStyle.EndComponentBody();
						}
					}
					ImGui::Separator();
					DrawExistingComponents(*Entity);
					DrawAddComponentControl(*Entity);
				}
				else
				{
					SelectSingleEntity(std::nullopt);
					m_EditorStyle.TextMuted("The selected entity no longer exists.");
				}
			}
			ImGui::End();
		}

		template<typename Action>
		void RecordComponentOperation(Action&& Operation, std::string_view Context)
		{
			const auto Result = RunSceneEdit(std::string(Context), std::forward<Action>(Operation));
			if (!Result)
			{
				SetError(std::string(Context) + ": " + Result.error());
				return;
			}
		}

		void SetCameraComponent(const PulseForge::Entity& Target, const PulseForge::CameraComponent& Camera)
		{
			if (auto Validation = Camera.Validate(); !Validation)
			{
				SetError("Camera update failed: " + Validation.error().Message);
				return;
			}
			RecordComponentOperation([this, Target, Camera]() -> std::expected<void, PulseForge::SceneError>
			{
				if (Camera.IsPrimary && m_Scene)
				{
					for (const PulseForge::Entity& Other : m_Scene->GetEntities())
					{
						if (Other.GetUUID() == Target.GetUUID())
							continue;
						auto Existing = Other.GetCamera();
						if (!Existing)
							return std::unexpected(Existing.error());
						if (!Existing->has_value() || !Existing->value().IsPrimary)
							continue;
						PulseForge::CameraComponent Updated = Existing->value();
						Updated.IsPrimary = false;
						if (auto Result = Other.SetCamera(Updated); !Result)
							return std::unexpected(Result.error());
					}
				}
				return Target.SetCamera(Camera);
			}, "Camera update failed");
		}

		void SetAudioListenerComponent(
			const PulseForge::Entity& Target,
			const PulseForge::AudioListenerComponent& Listener)
		{
			RecordComponentOperation([this, Target, Listener]() -> std::expected<void, PulseForge::SceneError>
			{
				if (Listener.IsPrimary && m_Scene)
				{
					for (const PulseForge::Entity& Other : m_Scene->GetEntities())
					{
						if (Other.GetUUID() == Target.GetUUID())
							continue;
						auto Existing = Other.GetAudioListener();
						if (!Existing)
							return std::unexpected(Existing.error());
						if (!Existing->has_value() || !Existing->value().IsPrimary)
							continue;
						PulseForge::AudioListenerComponent Updated = Existing->value();
						Updated.IsPrimary = false;
						if (auto Result = Other.SetAudioListener(Updated); !Result)
							return std::unexpected(Result.error());
					}
				}
				return Target.SetAudioListener(Listener);
				}, "Audio listener update failed");
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
			std::string PreviewPath;
			if (Current)
			{
				const auto Record = m_Project ? m_Project->GetAssetRegistry().Find(*Current) : std::nullopt;
				if (Record)
				{
					PreviewPath = PathToUtf8(Record->ProjectRelativePath);
					Preview = PathToUtf8(Record->ProjectRelativePath.filename());
				}
				else
					Preview = "<Missing asset: " + Current->ToString() + ">";
			}

			bool Changed = false;
			const bool IsOpen = ImGui::BeginCombo(Label, Preview.c_str());
			if (!PreviewPath.empty() && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", PreviewPath.c_str());
			if (!IsOpen)
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

		void DrawAddComponentControl(const PulseForge::Entity& Entity)
		{
			if (m_EditorStyle.FullWidthButton(PulseForgeEditor::EditorIcon::AddComponent, "Add Component"))
				ImGui::OpenPopup("Add Component");
			if (ImGui::BeginPopup("Add Component"))
			{
				const auto Camera = Entity.GetCamera();
				const auto DirectionalLight = Entity.GetDirectionalLight();
				const auto EnvironmentLight = Entity.GetEnvironmentLight();
				const auto PointLight = Entity.GetPointLight();
				const auto SpotLight = Entity.GetSpotLight();
				const auto Mesh = Entity.GetMeshRenderer();
				const auto Rigidbody = Entity.GetRigidbody();
				const auto Collider = Entity.GetBoxCollider();
				const auto AudioSource = Entity.GetAudioSource();
				const auto AudioListener = Entity.GetAudioListener();
				const auto Script = Entity.GetScript();
				if (!Camera || !DirectionalLight || !EnvironmentLight || !PointLight || !SpotLight || !Mesh || !Rigidbody || !Collider || !AudioSource || !AudioListener || !Script)
					SetError("Could not inspect entity components.");

				if (Camera && !Camera->has_value() &&
					m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::Camera, "Camera"))
				{
					PulseForge::CameraComponent Component;
					Component.IsPrimary = true;
					SetCameraComponent(Entity, Component);
				}
				if (DirectionalLight && !DirectionalLight->has_value() &&
					m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::DirectionalLight, "Directional Light"))
					RecordComponentOperation([Entity]
						{ return Entity.SetDirectionalLight(PulseForge::DirectionalLightComponent{}); },
						"Directional light component add failed");
				if (EnvironmentLight && !EnvironmentLight->has_value() && ImGui::BeginMenu("Environment Light"))
				{
					bool HasHdrImages = false;
					for (const PulseForge::AssetRecord& Asset : m_Assets)
					{
						if (!IsAssetExtension(Asset, { ".hdr" }))
							continue;
						HasHdrImages = true;
						const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
						if (ImGui::MenuItem(Path.c_str()))
						{
							PulseForge::EnvironmentLightComponent Component;
							Component.HdrImage = Asset.ID;
							RecordComponentOperation([Entity, Component] { return Entity.SetEnvironmentLight(Component); },
								"Environment light add failed");
							break;
						}
					}
					if (!HasHdrImages)
						ImGui::MenuItem("Import a Radiance HDR image first", nullptr, false, false);
					ImGui::EndMenu();
				}
				if (PointLight && !PointLight->has_value() && ImGui::MenuItem("Point Light"))
					RecordComponentOperation([Entity]
						{ return Entity.SetPointLight(PulseForge::PointLightComponent{}); }, "Point light component add failed");
				if (SpotLight && !SpotLight->has_value() && ImGui::MenuItem("Spot Light"))
					RecordComponentOperation([Entity]
						{ return Entity.SetSpotLight(PulseForge::SpotLightComponent{}); }, "Spot light component add failed");
				if (Rigidbody && !Rigidbody->has_value() && ImGui::MenuItem("Rigidbody"))
					RecordComponentOperation([Entity]
						{ return Entity.SetRigidbody(PulseForge::RigidbodyComponent{}); }, "Rigidbody component add failed");
				if (Collider && !Collider->has_value() && ImGui::MenuItem("Box Collider"))
					RecordComponentOperation([Entity]
						{ return Entity.SetBoxCollider(PulseForge::BoxColliderComponent{}); }, "Box collider add failed");
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
							RecordComponentOperation([Entity, Component] { return Entity.SetMeshRenderer(Component); },
								"Mesh renderer add failed");
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
							RecordComponentOperation([Entity, Component] { return Entity.SetAudioSource(Component); },
								"Audio source add failed");
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
							RecordComponentOperation([Entity, Component] { return Entity.SetScript(Component); },
								"Script component add failed");
							break;
						}
					}
					if (!HasScripts)
						ImGui::MenuItem("Import a Lua script first", nullptr, false, false);
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
		}

		void DrawExistingComponents(const PulseForge::Entity& Entity)
		{
			DrawCameraComponent(Entity);
			DrawDirectionalLightComponent(Entity);
			DrawPointLightComponent(Entity);
			DrawSpotLightComponent(Entity);
			DrawEnvironmentLightComponent(Entity);
			DrawMeshRendererComponent(Entity);
			DrawRigidbodyComponent(Entity);
			DrawBoxColliderComponent(Entity);
			DrawAudioSourceComponent(Entity);
			DrawAudioListenerComponent(Entity);
			DrawScriptComponent(Entity);
		}

		void DrawEnvironmentLightComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetEnvironmentLight();
			if (!Result || !Result->has_value())
				return;
			PulseForge::EnvironmentLightComponent Component = **Result;
			const bool Expanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Environment, "Environment Light");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveEnvironmentLight",
				"Remove Environment Light component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveEnvironmentLight(); },
					"Environment light removal failed");
				return;
			}
			if (!Expanded)
				return;
			m_EditorStyle.BeginComponentBody("EnvironmentLightComponentBody");
			bool Changed = false;
			std::optional<PulseForge::AssetID> HdrImage{ Component.HdrImage };
			if (m_EditorStyle.BeginPropertyTable("EnvironmentLightProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("HDR Image"))
					Changed |= DrawAssetSelector("##EnvironmentHDR", HdrImage, { ".hdr" }, false);
				if (m_EditorStyle.BeginPropertyRow("Intensity"))
					Changed |= ImGui::DragFloat("##EnvironmentIntensity", &Component.Intensity, 0.05f, 0.0f, 100000.0f, "%.3f");
				m_EditorStyle.EndPropertyTable();
			}
			if (HdrImage)
				Component.HdrImage = *HdrImage;
			if (Changed)
				RecordComponentOperation([Entity, Component] { return Entity.SetEnvironmentLight(Component); },
					"Environment light update failed");
			m_EditorStyle.EndComponentBody();
		}

		void DrawDirectionalLightComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetDirectionalLight();
			if (!Result || !Result->has_value())
				return;
			PulseForge::DirectionalLightComponent Component = **Result;
			const bool Expanded = m_EditorStyle.SectionHeader(
				PulseForgeEditor::EditorIcon::DirectionalLight, "Directional Light");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveDirectionalLight",
				"Remove Directional Light component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveDirectionalLight(); },
					"Directional light removal failed");
				return;
			}
			if (!Expanded)
				return;
			m_EditorStyle.BeginComponentBody("DirectionalLightComponentBody");
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("DirectionalLightProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Linear Color"))
					Changed |= ImGui::DragFloat3("##DirectionalLightLinearColor", &Component.Color.x, 0.01f, 0.0f, 100000.0f, "%.3f");
				if (m_EditorStyle.BeginPropertyRow("Intensity"))
					Changed |= ImGui::DragFloat("##DirectionalLightIntensity", &Component.Intensity, 0.05f, 0.0f, 100000.0f, "%.3f");
				if (m_EditorStyle.BeginPropertyRow("Cast Shadows"))
					Changed |= ImGui::Checkbox("##DirectionalLightCastShadows", &Component.CastShadows);
				if (Component.CastShadows)
				{
					if (m_EditorStyle.BeginPropertyRow("Shadow Distance"))
						Changed |= ImGui::DragFloat("##DirectionalLightShadowDistance", &Component.ShadowDistance, 0.25f, 0.1f, 5000.0f, "%.1f m");
					if (m_EditorStyle.BeginPropertyRow("Shadow Bias"))
						Changed |= ImGui::DragFloat("##DirectionalLightShadowBias", &Component.ShadowBias, 0.25f, 0.0f, 16.0f, "%.0f units");
					if (m_EditorStyle.BeginPropertyRow("Normal Bias"))
						Changed |= ImGui::DragFloat("##DirectionalLightShadowNormalBias", &Component.ShadowNormalBias, 0.0025f, 0.0f, 0.25f, "%.4f");
					if (m_EditorStyle.BeginPropertyRow("Softness"))
						Changed |= ImGui::DragFloat("##DirectionalLightShadowSoftness", &Component.ShadowSoftness, 0.05f, 0.0f, 8.0f, "%.2f texels");
				}
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed)
				RecordComponentOperation([Entity, Component] { return Entity.SetDirectionalLight(Component); },
					"Directional light update failed");
			m_EditorStyle.EndComponentBody();
		}

		void DrawPointLightComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetPointLight();
			if (!Result || !Result->has_value())
				return;
			PulseForge::PointLightComponent Component = **Result;
			const bool Expanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::PointLight, "Point Light");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemovePointLight",
				"Remove Point Light component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemovePointLight(); }, "Point light removal failed");
				return;
			}
			if (!Expanded)
				return;
			m_EditorStyle.BeginComponentBody("PointLightComponentBody");
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("PointLightProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Linear Color"))
					Changed |= ImGui::DragFloat3("##PointLightColor", &Component.Color.x, 0.01f, 0.0f, 1000.0f, "%.3f");
				if (m_EditorStyle.BeginPropertyRow("Intensity (unitless)"))
					Changed |= ImGui::DragFloat("##PointLightIntensity", &Component.Intensity, 0.1f, 0.0f, 1000000.0f, "%.2f");
				if (m_EditorStyle.BeginPropertyRow("Range"))
					Changed |= ImGui::DragFloat("##PointLightRange", &Component.Range, 0.05f, 0.01f, 10000.0f, "%.2f units");
				if (m_EditorStyle.BeginPropertyRow("Cast Shadows"))
					Changed |= ImGui::Checkbox("##PointLightCastShadows", &Component.CastShadows);
				if (Component.CastShadows)
				{
					if (m_EditorStyle.BeginPropertyRow("Shadow Bias (normalized)"))
						Changed |= ImGui::DragFloat("##PointLightShadowBias", &Component.ShadowBias, 0.0001f, 0.0f,
							PulseForge::MaxPointLightShadowBias, "%.4f");
					if (m_EditorStyle.BeginPropertyRow("Shadow Normal Bias"))
						Changed |= ImGui::DragFloat("##PointLightShadowNormalBias", &Component.ShadowNormalBias,
							0.001f, 0.0f, PulseForge::MaxPointLightShadowNormalBias, "%.3f units");
					if (m_EditorStyle.BeginPropertyRow("Shadow Softness (texels)"))
						Changed |= ImGui::DragFloat("##PointLightShadowSoftness", &Component.ShadowSoftness,
							0.05f, 0.0f, PulseForge::MaxPointLightShadowSoftness, "%.2f");
				}
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed)
				RecordComponentOperation([Entity, Component] { return Entity.SetPointLight(Component); },
					"Point light update failed");
			m_EditorStyle.EndComponentBody();
		}

		void DrawSpotLightComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetSpotLight();
			if (!Result || !Result->has_value())
				return;
			PulseForge::SpotLightComponent Component = **Result;
			const bool Expanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::SpotLight, "Spot Light");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveSpotLight",
				"Remove Spot Light component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveSpotLight(); }, "Spot light removal failed");
				return;
			}
			if (!Expanded)
				return;
			m_EditorStyle.BeginComponentBody("SpotLightComponentBody");
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("SpotLightProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Linear Color"))
					Changed |= ImGui::DragFloat3("##SpotLightColor", &Component.Color.x, 0.01f, 0.0f, 1000.0f, "%.3f");
				if (m_EditorStyle.BeginPropertyRow("Intensity (unitless)"))
					Changed |= ImGui::DragFloat("##SpotLightIntensity", &Component.Intensity, 0.1f, 0.0f, 1000000.0f, "%.2f");
				if (m_EditorStyle.BeginPropertyRow("Range"))
					Changed |= ImGui::DragFloat("##SpotLightRange", &Component.Range, 0.05f, 0.01f, 10000.0f, "%.2f units");
				if (m_EditorStyle.BeginPropertyRow("Inner cone half-angle"))
					Changed |= ImGui::DragFloat("##SpotLightInner", &Component.InnerConeAngleDegrees, 0.25f, 0.5f,
						Component.OuterConeAngleDegrees - 0.1f, "%.1f deg");
				if (m_EditorStyle.BeginPropertyRow("Outer cone half-angle"))
					Changed |= ImGui::DragFloat("##SpotLightOuter", &Component.OuterConeAngleDegrees, 0.25f,
						Component.InnerConeAngleDegrees + 0.1f, 89.5f, "%.1f deg");
				if (m_EditorStyle.BeginPropertyRow("Cast Shadows"))
					Changed |= ImGui::Checkbox("##SpotLightCastShadows", &Component.CastShadows);
				if (m_EditorStyle.BeginPropertyRow("Shadow Bias"))
					Changed |= ImGui::DragFloat("##SpotLightShadowBias", &Component.ShadowBias, 0.0001f,
						0.0f, 0.02f, "%.4f");
				if (m_EditorStyle.BeginPropertyRow("Shadow Normal Bias"))
					Changed |= ImGui::DragFloat("##SpotLightShadowNormalBias", &Component.ShadowNormalBias, 0.0025f,
						0.0f, 1.0f, "%.3f units");
				if (m_EditorStyle.BeginPropertyRow("Shadow Softness"))
					Changed |= ImGui::DragFloat("##SpotLightShadowSoftness", &Component.ShadowSoftness, 0.1f,
						0.0f, 4.0f, "%.1f texels");
				if (Component.CastShadows && Component.OuterConeAngleDegrees > 80.0f)
					ImGui::TextDisabled("Shadows support outer half-angles up to 80 degrees; illumination is unchanged.");
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed)
				RecordComponentOperation([Entity, Component] { return Entity.SetSpotLight(Component); },
					"Spot light update failed");
			m_EditorStyle.EndComponentBody();
		}

		void DrawCameraComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetCamera();
			if (!Result || !Result->has_value())
				return;

			PulseForge::CameraComponent Component = **Result;
			const bool IsExpanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Camera, "Camera");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveCamera", "Remove Camera component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveCamera(); }, "Camera component removal failed");
				return;
			}
			if (!IsExpanded)
				return;
			m_EditorStyle.BeginComponentBody("CameraComponentBody");

			float FieldOfViewDegrees = glm::degrees(Component.VerticalFieldOfViewRadians);
			const float MaxNearClip = (std::max)(0.002f, Component.FarClipPlane - 0.001f);
			const float MinFarClip = Component.NearClipPlane + 0.001f;
			bool FieldOfViewChanged = false;
			bool NearChanged = false;
			bool FarChanged = false;
			bool PrimaryChanged = false;
			if (m_EditorStyle.BeginPropertyTable("CameraProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Vertical FOV"))
					FieldOfViewChanged = ImGui::DragFloat("##CameraFov", &FieldOfViewDegrees, 0.1f, 1.0f, 179.0f, "%.1f deg");
				if (m_EditorStyle.BeginPropertyRow("Near Clip"))
					NearChanged = ImGui::DragFloat("##CameraNear", &Component.NearClipPlane, 0.01f, 0.001f, MaxNearClip, "%.3f");
				if (m_EditorStyle.BeginPropertyRow("Far Clip"))
					FarChanged = ImGui::DragFloat("##CameraFar", &Component.FarClipPlane, 1.0f, MinFarClip, 1000000.0f, "%.1f");
				if (m_EditorStyle.BeginPropertyRow("Primary"))
					PrimaryChanged = ImGui::Checkbox("##PrimaryCamera", &Component.IsPrimary);
				m_EditorStyle.EndPropertyTable();
			}
			if (FieldOfViewChanged || NearChanged || FarChanged || PrimaryChanged)
			{
				Component.VerticalFieldOfViewRadians = glm::radians(FieldOfViewDegrees);
				SetCameraComponent(Entity, Component);
			}
			m_EditorStyle.EndComponentBody();
		}

		void DrawMeshRendererComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetMeshRenderer();
			if (!Result || !Result->has_value())
				return;

			PulseForge::MeshRendererComponent Component = **Result;
			const bool IsExpanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Cube, "Mesh Renderer");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveMeshRenderer", "Remove Mesh Renderer component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveMeshRenderer(); },
					"Mesh renderer removal failed");
				return;
			}
			if (!IsExpanded)
				return;
			m_EditorStyle.BeginComponentBody("MeshRendererComponentBody");

			std::optional<PulseForge::AssetID> Mesh = Component.MeshAsset;
			std::optional<PulseForge::AssetID> Material = Component.MaterialAsset;
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("MeshRendererProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Mesh Asset"))
					Changed = DrawAssetSelector("##MeshAsset", Mesh, { ".gltf", ".glb" }, false);
				if (m_EditorStyle.BeginPropertyRow("Material"))
					Changed |= DrawAssetSelector("##MaterialAsset", Material, { ".material" }, true);
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed && Mesh)
			{
				Component.MeshAsset = *Mesh;
				Component.MaterialAsset = Material;
				RecordComponentOperation([Entity, Component] { return Entity.SetMeshRenderer(Component); },
					"Mesh renderer update failed");
			}
			m_EditorStyle.EndComponentBody();
		}

		void DrawRigidbodyComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetRigidbody();
			if (!Result || !Result->has_value())
				return;

			PulseForge::RigidbodyComponent Component = **Result;
			const bool IsExpanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Settings, "Rigidbody");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveRigidbody", "Remove Rigidbody component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveRigidbody(); }, "Rigidbody removal failed");
				return;
			}
			if (!IsExpanded)
				return;
			m_EditorStyle.BeginComponentBody("RigidbodyComponentBody");

			int MotionType = Component.MotionType == PulseForge::RigidbodyMotionType::Static ? 0 : 1;
			const char* MotionTypes[]{ "Static", "Dynamic" };
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("RigidbodyProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Motion Type") &&
					ImGui::Combo("##RigidbodyMotionType", &MotionType, MotionTypes, 2))
				{
					Component.MotionType = MotionType == 0
						? PulseForge::RigidbodyMotionType::Static
						: PulseForge::RigidbodyMotionType::Dynamic;
					Changed = true;
				}
				if (m_EditorStyle.BeginPropertyRow("Mass"))
					Changed |= ImGui::DragFloat("##RigidbodyMass", &Component.Mass, 0.05f, 0.001f, 1000000.0f, "%.2f");
				if (m_EditorStyle.BeginPropertyRow("Friction"))
					Changed |= ImGui::DragFloat("##RigidbodyFriction", &Component.Friction, 0.01f, 0.0f, 1.0f, "%.2f");
				if (m_EditorStyle.BeginPropertyRow("Restitution"))
					Changed |= ImGui::DragFloat("##RigidbodyRestitution", &Component.Restitution, 0.01f, 0.0f, 1.0f, "%.2f");
				if (m_EditorStyle.BeginPropertyRow("Allow Sleeping"))
					Changed |= ImGui::Checkbox("##RigidbodySleeping", &Component.AllowSleeping);
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed)
				RecordComponentOperation([Entity, Component] { return Entity.SetRigidbody(Component); },
					"Rigidbody update failed");
			m_EditorStyle.EndComponentBody();
		}

		void DrawBoxColliderComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetBoxCollider();
			if (!Result || !Result->has_value())
				return;

			PulseForge::BoxColliderComponent Component = **Result;
			const bool IsExpanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Cube, "Box Collider");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveBoxCollider", "Remove Box Collider component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveBoxCollider(); }, "Box collider removal failed");
				return;
			}
			if (!IsExpanded)
				return;
			m_EditorStyle.BeginComponentBody("BoxColliderComponentBody");

			float HalfExtents[3]{ Component.HalfExtents.x, Component.HalfExtents.y, Component.HalfExtents.z };
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("BoxColliderProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Half Extents"))
					Changed = ImGui::DragFloat3("##BoxColliderExtents", HalfExtents, 0.01f, 0.001f, 1000000.0f, "%.2f");
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed)
			{
				Component.HalfExtents = { HalfExtents[0], HalfExtents[1], HalfExtents[2] };
				RecordComponentOperation([Entity, Component] { return Entity.SetBoxCollider(Component); },
					"Box collider update failed");
			}
			m_EditorStyle.EndComponentBody();
		}

		void DrawAudioSourceComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetAudioSource();
			if (!Result || !Result->has_value())
				return;

			PulseForge::AudioSourceComponent Component = **Result;
			const bool IsExpanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::AudioSource, "Audio Source");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveAudioSource", "Remove Audio Source component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveAudioSource(); }, "Audio source removal failed");
				return;
			}
			if (!IsExpanded)
				return;
			m_EditorStyle.BeginComponentBody("AudioSourceComponentBody");

			std::optional<PulseForge::AssetID> Audio = Component.AudioAsset;
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("AudioSourceProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Audio Asset"))
					Changed = DrawAssetSelector("##AudioAsset", Audio, { ".wav", ".mp3", ".flac", ".ogg" }, false);
				if (m_EditorStyle.BeginPropertyRow("Volume"))
					Changed |= ImGui::DragFloat("##AudioVolume", &Component.Volume, 0.01f, 0.0f, 1.0f, "%.2f");
				if (m_EditorStyle.BeginPropertyRow("Looping"))
					Changed |= ImGui::Checkbox("##AudioLooping", &Component.Looping);
				if (m_EditorStyle.BeginPropertyRow("Play On Start"))
					Changed |= ImGui::Checkbox("##AudioPlayOnStart", &Component.PlayOnStart);
				if (m_EditorStyle.BeginPropertyRow("Spatialized"))
					Changed |= ImGui::Checkbox("##AudioSpatialized", &Component.Spatialized);
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed && Audio)
			{
				Component.AudioAsset = *Audio;
				RecordComponentOperation([Entity, Component] { return Entity.SetAudioSource(Component); },
					"Audio source update failed");
			}
			m_EditorStyle.EndComponentBody();
		}

		void DrawAudioListenerComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetAudioListener();
			if (!Result || !Result->has_value())
				return;

			PulseForge::AudioListenerComponent Component = **Result;
			const bool IsExpanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::AudioListener, "Audio Listener");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveAudioListener",
				"Remove Audio Listener component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveAudioListener(); },
					"Audio listener removal failed");
				return;
			}
			if (!IsExpanded)
				return;
			m_EditorStyle.BeginComponentBody("AudioListenerComponentBody");
			if (m_EditorStyle.BeginPropertyTable("AudioListenerProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Primary Listener") &&
					ImGui::Checkbox("##PrimaryAudioListener", &Component.IsPrimary))
					SetAudioListenerComponent(Entity, Component);
				m_EditorStyle.EndPropertyTable();
			}
			m_EditorStyle.EndComponentBody();
		}

		void DrawScriptComponent(const PulseForge::Entity& Entity)
		{
			const auto Result = Entity.GetScript();
			if (!Result || !Result->has_value())
				return;

			PulseForge::ScriptComponent Component = **Result;
			const bool IsExpanded = m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::Script, "Script");
			ImGui::SameLine();
			ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 31.0f));
			if (m_EditorStyle.IconButton(
				PulseForgeEditor::EditorIcon::Delete, "Remove", "RemoveScript", "Remove Script component", true))
			{
				RecordComponentOperation([Entity] { return Entity.RemoveScript(); }, "Script component removal failed");
				return;
			}
			if (!IsExpanded)
				return;
			m_EditorStyle.BeginComponentBody("ScriptComponentBody");

			std::optional<PulseForge::AssetID> Script = Component.ScriptAsset;
			bool Changed = false;
			if (m_EditorStyle.BeginPropertyTable("ScriptProperties"))
			{
				if (m_EditorStyle.BeginPropertyRow("Lua Script"))
					Changed = DrawAssetSelector("##ScriptAsset", Script, { ".lua" }, false);
				if (m_EditorStyle.BeginPropertyRow("Enabled"))
					Changed |= ImGui::Checkbox("##ScriptEnabled", &Component.Enabled);
				m_EditorStyle.EndPropertyTable();
			}
			if (Changed && Script)
			{
				Component.ScriptAsset = *Script;
				RecordComponentOperation([Entity, Component] { return Entity.SetScript(Component); },
					"Script component update failed");
			}
			m_EditorStyle.EndComponentBody();
		}

		void ResetContentBrowserNavigation()
		{
			m_CurrentContentFolder = "Assets";
			m_ContentSearch.clear();
			m_ContentGridView = true;
		}

		void RebuildContentFolderTree()
		{
			m_ContentFolderRoot = { "Assets", "Assets", {} };
			for (const PulseForge::AssetRecord& Asset : m_Assets)
			{
				const std::filesystem::path Normalized = Asset.ProjectRelativePath.lexically_normal();
				const std::filesystem::path ParentPath = Normalized.parent_path();
				auto Component = ParentPath.begin();
				if (Component == ParentPath.end() || *Component != "Assets")
					continue;
				++Component;
				std::filesystem::path CurrentPath = "Assets";
				ContentFolderNode* CurrentNode = &m_ContentFolderRoot;
				for (; Component != ParentPath.end(); ++Component)
				{
					CurrentPath /= *Component;
					auto Child = std::find_if(CurrentNode->Children.begin(), CurrentNode->Children.end(),
						[&Component](const ContentFolderNode& Candidate)
						{
							return Candidate.Name == PathToUtf8(*Component);
						});
					if (Child == CurrentNode->Children.end())
					{
						CurrentNode->Children.push_back({ PathToUtf8(*Component), CurrentPath, {} });
						Child = std::prev(CurrentNode->Children.end());
					}
					CurrentNode = &*Child;
				}
			}

			const auto SortChildren = [](auto&& Self, ContentFolderNode& Node) -> void
			{
				std::sort(Node.Children.begin(), Node.Children.end(), [](const ContentFolderNode& Left,
					const ContentFolderNode& Right)
				{
					return Left.Name < Right.Name;
				});
				for (ContentFolderNode& Child : Node.Children)
					Self(Self, Child);
			};
			SortChildren(SortChildren, m_ContentFolderRoot);
			if (!IsKnownContentFolder(m_CurrentContentFolder))
				m_CurrentContentFolder = "Assets";
		}

		bool IsKnownContentFolder(const std::filesystem::path& Folder) const
		{
			const auto ContainsFolder = [](auto&& Self, const ContentFolderNode& Node,
				const std::filesystem::path& Candidate) -> bool
			{
				if (Node.RelativePath == Candidate)
					return true;
				return std::any_of(Node.Children.begin(), Node.Children.end(), [&](const ContentFolderNode& Child)
				{
					return Self(Self, Child, Candidate);
				});
			};
			return ContainsFolder(ContainsFolder, m_ContentFolderRoot, Folder.lexically_normal());
		}

		const ContentFolderNode* FindContentFolderNode(
			const ContentFolderNode& Node,
			const std::filesystem::path& Folder) const
		{
			if (Node.RelativePath.lexically_normal() == Folder.lexically_normal())
				return &Node;
			for (const ContentFolderNode& Child : Node.Children)
				if (const ContentFolderNode* Found = FindContentFolderNode(Child, Folder))
					return Found;
			return nullptr;
		}

		void DrawContentFolderNode(
			const ContentFolderNode& Node,
			std::function<void()>& DeferredAction,
			bool IsRoot = false)
		{
			if (IsRoot)
				ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (Node.RelativePath == m_CurrentContentFolder)
				Flags |= ImGuiTreeNodeFlags_Selected;
			if (Node.Children.empty())
				Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			const std::string StableID = PathToUtf8(Node.RelativePath);
			const bool IsOpen = m_EditorStyle.TreeNode(
				Node.RelativePath == m_CurrentContentFolder
					? PulseForgeEditor::EditorIcon::FolderOpen
					: PulseForgeEditor::EditorIcon::Folder,
				Node.Name,
				StableID,
				Flags);
			if (ImGui::IsItemClicked())
				m_CurrentContentFolder = Node.RelativePath;
			BeginFolderDragSource(Node.RelativePath, Node.Name);
			HandleManagedFolderDropTarget(Node.RelativePath, DeferredAction);
			if (IsOpen && !Node.Children.empty())
			{
				for (const ContentFolderNode& Child : Node.Children)
					DrawContentFolderNode(Child, DeferredAction);
				ImGui::TreePop();
			}
		}

		void BeginFolderDragSource(const std::filesystem::path& Folder, std::string_view Name)
		{
			if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
				return;
			const std::string Payload = PathToUtf8(Folder);
			ImGui::SetDragDropPayload("PF_MANAGED_FOLDER_MOVE", Payload.c_str(), Payload.size() + 1);
			ImGui::Text("Move folder: %.*s", static_cast<int>(Name.size()), Name.data());
			ImGui::EndDragDropSource();
		}

		void HandleManagedFolderDropTarget(
			const std::filesystem::path& DestinationFolder,
			std::function<void()>& DeferredAction)
		{
			if (DeferredAction || !ImGui::BeginDragDropTarget())
				return;

			if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("PF_MANAGED_ASSET_MOVE"))
			{
				if (Payload->IsDelivery() && Payload->Data && Payload->DataSize > 1)
				{
					std::string_view IdentifierText(static_cast<const char*>(Payload->Data),
						static_cast<size_t>(Payload->DataSize));
					if (!IdentifierText.empty() && IdentifierText.back() == '\0')
						IdentifierText.remove_suffix(1);
					if (const auto Identifier = PulseForge::AssetID::Parse(IdentifierText))
					{
						const auto Record = m_Project
							? m_Project->GetAssetRegistry().Find(*Identifier) : std::nullopt;
						if (Record)
						{
							const std::filesystem::path Destination =
								DestinationFolder / Record->ProjectRelativePath.filename();
							DeferredAction = [this, Identifier = *Identifier, Destination]
							{
								QueueAfterSave([this, Identifier, Destination]
								{
									MoveAssetAt(Identifier, Destination);
								});
							};
						}
					}
				}
			}
			else if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("PF_MANAGED_FOLDER_MOVE"))
			{
				if (Payload->IsDelivery() && Payload->Data && Payload->DataSize > 1)
				{
					std::string_view SourceText(static_cast<const char*>(Payload->Data),
						static_cast<size_t>(Payload->DataSize));
					if (!SourceText.empty() && SourceText.back() == '\0')
						SourceText.remove_suffix(1);
					const std::string SourceString(SourceText);
					const std::filesystem::path Source = std::filesystem::u8path(SourceString);
					DeferredAction = [this, Source, DestinationFolder]
					{
						QueueAfterSave([this, Source, DestinationFolder]
						{
							MoveFolderAt(Source, DestinationFolder);
						});
					};
				}
			}
			ImGui::EndDragDropTarget();
		}

		void DrawContentBreadcrumbs()
		{
			std::vector<std::pair<std::filesystem::path, std::string>> Breadcrumbs;
			std::filesystem::path Breadcrumb = "Assets";
			for (auto Component = m_CurrentContentFolder.begin(); Component != m_CurrentContentFolder.end(); ++Component)
			{
				if (*Component == "Assets")
					continue;
				Breadcrumb /= *Component;
				Breadcrumbs.emplace_back(Breadcrumb, PathToUtf8(*Component));
			}

			if (ImGui::SmallButton("Assets##BreadcrumbRoot"))
				m_CurrentContentFolder = "Assets";
			if (Breadcrumbs.empty())
				return;

			ImGui::SameLine();
			m_EditorStyle.TextMuted("/");
			if (Breadcrumbs.size() > 1)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("...##BreadcrumbAncestors"))
					ImGui::OpenPopup("ContentBreadcrumbAncestors");
				if (ImGui::BeginPopup("ContentBreadcrumbAncestors"))
				{
					for (const auto& BreadcrumbEntry : Breadcrumbs)
					{
						const std::filesystem::path& Path = BreadcrumbEntry.first;
						const std::string FullPath = PathToUtf8(Path);
						if (ImGui::Selectable(FullPath.c_str(), Path == m_CurrentContentFolder))
							m_CurrentContentFolder = Path;
					}
					ImGui::EndPopup();
				}
				ImGui::SameLine();
				m_EditorStyle.TextMuted("/");
			}

			const std::string& CurrentFolderName = Breadcrumbs.back().second;
			const float LabelWidth = (std::max)(ImGui::GetContentRegionAvail().x -
				ImGui::GetStyle().FramePadding.x * 2.0f, 0.0f);
			const std::string VisibleLabel = FitEditorContextLabel(CurrentFolderName, LabelWidth);
			const std::string CurrentLabel = (VisibleLabel.empty() ? "..." : VisibleLabel) +
				"##BreadcrumbCurrent";
			if (ImGui::SmallButton(CurrentLabel.c_str()))
				m_CurrentContentFolder = Breadcrumbs.back().first;
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", PathToUtf8(m_CurrentContentFolder).c_str());
		}

		void HandleAssetBrowserItem(
			const PulseForge::AssetRecord& Asset,
			const AssetBrowserPresentation& Presentation,
			bool Activated,
			std::function<void()>& DeferredAction)
		{
			if (Activated)
			{
				m_SelectedAsset = Asset.ID;
				switch (PulseForgeEditor::ResolveAssetBrowserOpenAction(
					Presentation.Extension,
					Activated,
					ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
				{
					case PulseForgeEditor::AssetBrowserOpenAction::OpenScene:
						OpenScene(Asset.ID);
						break;
					case PulseForgeEditor::AssetBrowserOpenAction::OpenMaterialEditor:
					{
						const PulseForge::AssetID Identifier = Asset.ID;
						DeferredAction = [this, Identifier] { OpenMaterialEditor(Identifier); };
						break;
					}
					case PulseForgeEditor::AssetBrowserOpenAction::None:
						break;
				}
			}
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				const std::string Identifier = Asset.ID.ToString();
				ImGui::SetDragDropPayload("PF_MANAGED_ASSET_MOVE", Identifier.c_str(), Identifier.size() + 1);
				ImGui::Text("Move asset: %s", Presentation.Name.c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\nUUID: %s", Presentation.FullPath.c_str(), Presentation.Identifier.c_str());
			if (!ImGui::BeginPopupContextItem("Asset Actions"))
				return;

			m_SelectedAsset = Asset.ID;
			const PulseForge::AssetID Identifier = Asset.ID;
			if (Presentation.Extension == ".material" && m_EditorStyle.MenuItem(
				PulseForgeEditor::EditorIcon::Material, "Open in Material Editor"))
				DeferredAction = [this, Identifier] { OpenMaterialEditor(Identifier); };
			if (m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::Rename, "Move / Rename..."))
				DeferredAction = [this, Identifier] { MoveAsset(Identifier); };
			if (m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::Duplicate, "Duplicate..."))
				DeferredAction = [this, Identifier] { DuplicateAsset(Identifier); };
			if (Presentation.Extension == ".prefab" && m_EditorStyle.MenuItem(
				PulseForgeEditor::EditorIcon::Cube, "Instantiate in Scene", nullptr, false, m_Scene != nullptr))
				DeferredAction = [this, Identifier] { InstantiatePrefab(Identifier); };
			const bool IsStartupScene = m_Project->GetDescription().StartScene &&
				*m_Project->GetDescription().StartScene == Identifier;
			if (Presentation.Extension == ".scene" && ImGui::MenuItem(
				"Set as Startup Scene", nullptr, IsStartupScene, !IsStartupScene))
				DeferredAction = [this, Identifier] { SetStartupScene(Identifier); };
			if (m_EditorStyle.MenuItem(PulseForgeEditor::EditorIcon::Delete, "Delete..."))
				DeferredAction = [this, Identifier] { RequestDeleteAsset(Identifier); };
			ImGui::EndPopup();
		}

		void OpenMaterialEditor(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return;

			const auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			if (!Asset || NormalizedExtension(Asset->ProjectRelativePath) != ".material")
			{
				SetError("The selected material is no longer registered.");
				return;
			}

			m_OpenedMaterialAsset = Identifier;
			SetEditorPanelVisible(PulseForgeEditor::EditorPanel::MaterialEditor, true);
		}

		void DrawFolderList(
			const std::vector<ContentFolderNode>& Folders,
			std::function<void()>& DeferredAction)
		{
			if (Folders.empty())
				return;

			if (!ImGui::BeginTable("ManagedFolderList", 1,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings))
				return;
			ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			for (const ContentFolderNode& Folder : Folders)
			{
				const std::string StableID = PathToUtf8(Folder.RelativePath);
				ImGui::PushID(StableID.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const bool IsSelected = m_SelectedContentFolder &&
					m_SelectedContentFolder->lexically_normal() == Folder.RelativePath.lexically_normal();
				const bool Activated = m_EditorStyle.Selectable(PulseForgeEditor::EditorIcon::Folder,
					Folder.Name, IsSelected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns);
				if (Activated)
				{
					m_SelectedContentFolder = Folder.RelativePath;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						m_CurrentContentFolder = Folder.RelativePath;
				}
				BeginFolderDragSource(Folder.RelativePath, Folder.Name);
				HandleManagedFolderDropTarget(Folder.RelativePath, DeferredAction);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", StableID.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		void DrawFolderGrid(
			const std::vector<ContentFolderNode>& Folders,
			std::function<void()>& DeferredAction)
		{
			if (Folders.empty())
				return;

			const float AvailableWidth = ImGui::GetContentRegionAvail().x;
			const float DesiredTileWidth = 122.0f;
			const float Spacing = ImGui::GetStyle().ItemSpacing.x;
			const int ColumnCount = (std::max)(1, static_cast<int>((AvailableWidth + Spacing) /
				(DesiredTileWidth + Spacing)));
			if (!ImGui::BeginTable("ManagedFolderGrid", ColumnCount,
				ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings |
				ImGuiTableFlags_NoBordersInBody))
				return;

			constexpr float TileHeight = 96.0f;
			const int RowCount = static_cast<int>((Folders.size() + static_cast<size_t>(ColumnCount) - 1) /
				static_cast<size_t>(ColumnCount));
			ImGuiListClipper Clipper;
			Clipper.Begin(RowCount, TileHeight + Spacing);
			while (Clipper.Step())
			{
				for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
				{
					ImGui::TableNextRow(ImGuiTableRowFlags_None, TileHeight);
					for (int Column = 0; Column < ColumnCount; ++Column)
					{
						const size_t Index = static_cast<size_t>(Row) * static_cast<size_t>(ColumnCount) +
							static_cast<size_t>(Column);
						if (Index >= Folders.size())
							break;
						ImGui::TableSetColumnIndex(Column);
						const ContentFolderNode& Folder = Folders[Index];
						const std::string StableID = PathToUtf8(Folder.RelativePath);
						ImGui::PushID(StableID.c_str());
						const bool IsSelected = m_SelectedContentFolder &&
							m_SelectedContentFolder->lexically_normal() == Folder.RelativePath.lexically_normal();
						const bool Activated = m_EditorStyle.AssetTile(PulseForgeEditor::EditorIcon::Folder,
							Folder.Name, IsSelected,
							ImVec2((std::max)(ImGui::GetContentRegionAvail().x - 2.0f, 1.0f), TileHeight),
							ImGuiSelectableFlags_AllowDoubleClick);
						if (Activated)
						{
							m_SelectedContentFolder = Folder.RelativePath;
							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
								m_CurrentContentFolder = Folder.RelativePath;
						}
						BeginFolderDragSource(Folder.RelativePath, Folder.Name);
						HandleManagedFolderDropTarget(Folder.RelativePath, DeferredAction);
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", StableID.c_str());
						ImGui::PopID();
					}
				}
			}
			ImGui::EndTable();
		}

		void DrawAssetList(std::function<void()>& DeferredAction)
		{
			if (m_FilteredAssetIndices.empty())
				return;

			if (!ImGui::BeginTable("ManagedAssetList", 2,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings))
				return;
			ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f);
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(m_FilteredAssetIndices.size()));
			while (Clipper.Step())
			{
				for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
				{
					const size_t AssetIndex = m_FilteredAssetIndices[static_cast<size_t>(Row)];
					const PulseForge::AssetRecord& Asset = m_Assets[AssetIndex];
					const AssetBrowserPresentation& Presentation = m_AssetBrowserPresentations[AssetIndex];
					ImGui::PushID(Presentation.Identifier.c_str());
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					const bool IsSelected = m_SelectedAsset && *m_SelectedAsset == Asset.ID;
					const bool Activated = m_EditorStyle.Selectable(Presentation.Icon, Presentation.Name, IsSelected,
						ImGuiSelectableFlags_AllowDoubleClick);
					HandleAssetBrowserItem(Asset, Presentation, Activated, DeferredAction);
					ImGui::TableSetColumnIndex(1);
					m_EditorStyle.TextMuted(Presentation.Extension.empty()
						? "File" : std::string_view(Presentation.Extension).substr(1));
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}

		void DrawAssetGrid(std::function<void()>& DeferredAction)
		{
			if (m_FilteredAssetIndices.empty())
				return;

			const float AvailableWidth = ImGui::GetContentRegionAvail().x;
			const float DesiredTileWidth = 124.0f;
			const float Spacing = ImGui::GetStyle().ItemSpacing.x;
			const int ColumnCount = (std::max)(1, static_cast<int>((AvailableWidth + Spacing) /
				(DesiredTileWidth + Spacing)));
			const int RowCount = static_cast<int>((m_FilteredAssetIndices.size() +
				static_cast<size_t>(ColumnCount) - 1) / static_cast<size_t>(ColumnCount));
			if (!ImGui::BeginTable("ManagedAssetGrid", ColumnCount,
				ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings |
				ImGuiTableFlags_NoBordersInBody))
				return;

			constexpr float TileHeight = 102.0f;
			ImGuiListClipper Clipper;
			Clipper.Begin(RowCount, TileHeight + Spacing);
			while (Clipper.Step())
			{
				for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
				{
					ImGui::TableNextRow(ImGuiTableRowFlags_None, TileHeight);
					for (int Column = 0; Column < ColumnCount; ++Column)
					{
						const size_t AssetIndex = static_cast<size_t>(Row) * static_cast<size_t>(ColumnCount) +
							static_cast<size_t>(Column);
						if (AssetIndex >= m_FilteredAssetIndices.size())
							break;
						ImGui::TableSetColumnIndex(Column);
						const size_t SourceIndex = m_FilteredAssetIndices[AssetIndex];
						const PulseForge::AssetRecord& Asset = m_Assets[SourceIndex];
						const AssetBrowserPresentation& Presentation = m_AssetBrowserPresentations[SourceIndex];
						ImGui::PushID(Presentation.Identifier.c_str());
						const bool IsSelected = m_SelectedAsset && *m_SelectedAsset == Asset.ID;
						uint64_t PreviewID = 0;
						uint32_t PreviewWidth = 0;
						uint32_t PreviewHeight = 0;
						#ifndef PF_EDITOR_RENDERER_OPENGL
						const bool IsImagePreview = PulseForgeEditor::IsSupportedImagePreviewExtension(Asset.ProjectRelativePath);
						const bool IsMaterialPreview = Presentation.Extension == ".material";
						if (m_Project && (IsImagePreview || IsMaterialPreview))
						{
							const std::string ProjectID = m_Project->GetDescription().ID.ToString();
							const std::string AssetID = Asset.ID.ToString();
							if (!m_AssetPreviews.TouchPreview(ProjectID, AssetID))
							{
								auto SourcePath = PulseForge::AssetPathResolver::ResolveManagedSourcePath(
									m_Project->GetRootPath(), Asset.ProjectRelativePath);
								if (SourcePath)
								{
									if (IsMaterialPreview)
										m_AssetPreviews.RequestMaterialSwatch(ProjectID, AssetID, *SourcePath);
									else
										m_AssetPreviews.RequestImage(ProjectID, AssetID, *SourcePath);
								}
								else
									m_AssetPreviews.MarkUnavailable(ProjectID, AssetID);
							}
							if (const auto Preview = m_AssetPreviews.FindPreview(ProjectID, AssetID))
							{
								PreviewID = Preview->TextureID;
								PreviewWidth = Preview->Width;
								PreviewHeight = Preview->Height;
							}
						}
						#endif
						const bool Activated = m_EditorStyle.AssetTile(Presentation.Icon, Presentation.Name, IsSelected,
							ImVec2((std::max)(ImGui::GetContentRegionAvail().x - 2.0f, 1.0f), TileHeight),
							ImGuiSelectableFlags_AllowDoubleClick, PreviewID, PreviewWidth, PreviewHeight);
						HandleAssetBrowserItem(Asset, Presentation, Activated, DeferredAction);
						ImGui::PopID();
					}
				}
			}
			ImGui::EndTable();
		}

		void DrawSelectedAssetDetails()
		{
			if (!m_SelectedAsset || !m_Project)
				return;
			const auto Asset = m_Project->GetAssetRegistry().Find(*m_SelectedAsset);
			if (!Asset)
			{
				m_SelectedAsset.reset();
				return;
			}

			const std::string Name = PathToUtf8(Asset->ProjectRelativePath.filename());
			const std::string FullPath = PathToUtf8(Asset->ProjectRelativePath);
			const std::string Identifier = Asset->ID.ToString();
			const std::string Extension = NormalizedExtension(Asset->ProjectRelativePath);
			if (Extension == ".material")
				return;

			ImGui::Separator();
			{
				[[maybe_unused]] const auto Emphasis = m_EditorStyle.PushEmphasisFont();
				m_EditorStyle.IconText(PulseForgeEditor::GetAssetBrowserIcon(Asset->ProjectRelativePath), Name);
			}
			ImGui::PushStyleColor(ImGuiCol_Text,
				m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::TextMuted));
			ImGui::TextWrapped("%s", FullPath.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\nUUID: %s", FullPath.c_str(), Identifier.c_str());
			ImGui::PopStyleColor();

			if (Extension == ".scene")
			{
				const bool IsStartupScene = m_Project->GetDescription().StartScene &&
					*m_Project->GetDescription().StartScene == Asset->ID;
				if (IsStartupScene)
					m_EditorStyle.TextMuted("Project startup scene");
				else if (m_EditorStyle.SmallButton(PulseForgeEditor::EditorIcon::Settings, "Set as Startup Scene"))
					SetStartupScene(Asset->ID);
			}
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Rename, "Move", "SelectedAssetMove",
				"Move or rename selected asset"))
				MoveAsset(Asset->ID);
			ImGui::SameLine();
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Duplicate, "Duplicate",
				"SelectedAssetDuplicate", "Duplicate selected asset"))
				DuplicateAsset(Asset->ID);
			ImGui::SameLine();
			const bool IsProtectedScene = IsProtectedSceneAsset(Asset->ID);
			ImGui::BeginDisabled(IsProtectedScene);
			if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Delete, "Delete",
				"SelectedAssetDelete", "Delete selected asset", true))
				RequestDeleteAsset(Asset->ID);
			ImGui::EndDisabled();
			if (Extension == ".prefab")
			{
				ImGui::SameLine();
				ImGui::BeginDisabled(!m_Scene);
				if (m_EditorStyle.SmallButton(PulseForgeEditor::EditorIcon::Cube, "Instantiate"))
					InstantiatePrefab(Asset->ID);
				ImGui::EndDisabled();
			}
			if (IsProtectedScene)
				m_EditorStyle.TextMuted("Open and startup scenes cannot be deleted.");
		}

		std::string GetMaterialDraftKey(const PulseForge::AssetID& Asset) const
		{
			return PulseForgeEditor::MakeMaterialDraftKey(
				PathToUtf8(m_Project->GetRootPath().lexically_normal()), Asset.ToString());
		}

		void ForgetMaterialDraft(const PulseForge::AssetID& Asset)
		{
			if (!m_Project)
				return;
			const std::string Key = GetMaterialDraftKey(Asset);
			m_MaterialDrafts.erase(Key);
			m_DirtyMaterialDrafts.erase(Key);
			m_MaterialEditorErrors.erase(Key);
		}

		bool DrawMaterialAssetProperties(const PulseForge::AssetRecord& Asset)
		{
			const std::string Key = GetMaterialDraftKey(Asset.ID);
			if (const auto Error = m_MaterialEditorErrors.find(Key); Error != m_MaterialEditorErrors.end())
			{
				ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error),
					"Material could not be loaded: %s", Error->second.c_str());
				if (ImGui::SmallButton("Retry Material Load"))
					m_MaterialEditorErrors.erase(Error);
				return false;
			}

			auto Draft = m_MaterialDrafts.find(Key);
			if (Draft == m_MaterialDrafts.end())
			{
				auto Loaded = PulseForge::MaterialAssetService::Load(
					Asset.ID, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
				if (!Loaded)
				{
					m_MaterialEditorErrors.emplace(Key, Loaded.error().Message);
					ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error),
						"Material could not be loaded: %s", Loaded.error().Message.c_str());
					if (ImGui::SmallButton("Retry Material Load"))
						m_MaterialEditorErrors.erase(Key);
					return false;
				}
				Draft = m_MaterialDrafts.emplace(Key, std::move(*Loaded)).first;
			}

			PulseForge::MaterialAssetDesc& Material = Draft->second;
			bool Changed = false;
			bool UseTwoColumnLayout = PulseForgeEditor::GetMaterialPropertyLayout(
				ImGui::GetContentRegionAvail().x) == PulseForgeEditor::MaterialPropertyLayout::TwoColumn;
			bool PropertyTableOpen = false;
			if (UseTwoColumnLayout)
			{
				ImGui::PushStyleColor(ImGuiCol_TableBorderLight,
					m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Border));
				PropertyTableOpen = ImGui::BeginTable("MaterialAssetProperties", 2,
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings);
				if (PropertyTableOpen)
				{
					const float AvailableWidth = ImGui::GetContentRegionAvail().x;
					ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed,
						PulseForgeEditor::GetMaterialPropertyLabelWidth(AvailableWidth));
					ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				}
				else
				{
					ImGui::PopStyleColor();
					UseTwoColumnLayout = false;
				}
			}

			const auto BeginMaterialPropertyRow = [this, UseTwoColumnLayout](std::string_view Label)
			{
				if (UseTwoColumnLayout)
					return m_EditorStyle.BeginPropertyRow(Label);

				m_EditorStyle.TextMuted(Label);
				const float LabelWidth = ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x;
				if (LabelWidth > ImGui::GetContentRegionAvail().x && ImGui::IsItemHovered())
					ImGui::SetTooltip("%.*s", static_cast<int>(Label.size()), Label.data());
				return true;
			};
			const auto DrawMaterialPropertyRow = [UseTwoColumnLayout, &BeginMaterialPropertyRow](
				std::string_view Label,
				auto&& DrawValue)
			{
				if (!BeginMaterialPropertyRow(Label))
					return;
				DrawValue();
				if (!UseTwoColumnLayout)
					ImGui::Spacing();
			};

			if (!UseTwoColumnLayout || PropertyTableOpen)
			{
				DrawMaterialPropertyRow("Base Color Factor", [&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat4("##MaterialBaseColorFactor", &Material.BaseColorFactor.x,
						0.01f, 0.0f, 1.0f, "%.3f");
				});
				DrawMaterialPropertyRow("Alpha Mode", [&]
				{
					int AlphaMode = static_cast<int>(Material.AlphaMode);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::Combo("##MaterialAlphaMode", &AlphaMode, "Opaque\0Mask\0Blend\0"))
					{
						Material.AlphaMode = static_cast<PulseForge::MaterialAlphaMode>(AlphaMode);
						Changed = true;
					}
				});
				if (Material.AlphaMode == PulseForge::MaterialAlphaMode::Mask)
					DrawMaterialPropertyRow("Alpha Cutoff", [&]
					{
						ImGui::SetNextItemWidth(-FLT_MIN);
						Changed |= ImGui::DragFloat("##MaterialAlphaCutoff", &Material.AlphaCutoff,
							0.01f, 0.0f, 1.0f, "%.3f");
					});
				DrawMaterialPropertyRow("Double Sided", [&]
				{
					Changed |= ImGui::Checkbox("##MaterialDoubleSided", &Material.DoubleSided);
				});
				DrawMaterialPropertyRow("Base Color Texture", [&]
				{
					std::optional<PulseForge::AssetID> Texture = Material.BaseColorTexture.IsNil()
						? std::nullopt : std::optional<PulseForge::AssetID>(Material.BaseColorTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (DrawAssetSelector("##MaterialBaseColorTexture", Texture,
						{ ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.BaseColorTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
				DrawMaterialPropertyRow("Metallic / Roughness", [&]
				{
					const float AvailableWidth = ImGui::GetContentRegionAvail().x;
					const float ItemSpacing = ImGui::GetStyle().ItemSpacing.x;
					const PulseForgeEditor::MaterialValuePairLayout PairLayout =
						PulseForgeEditor::GetMaterialValuePairLayout(AvailableWidth, ItemSpacing);
					ImGui::SetNextItemWidth(PairLayout.FirstWidth);
					Changed |= ImGui::DragFloat("##MaterialMetallic", &Material.MetallicFactor,
						0.01f, 0.0f, 1.0f, "M %.3f");
					if (!PairLayout.Stacked)
						ImGui::SameLine(0.0f, ItemSpacing);
					ImGui::SetNextItemWidth(PairLayout.SecondWidth);
					Changed |= ImGui::DragFloat("##MaterialRoughness", &Material.RoughnessFactor,
						0.01f, 0.0f, 1.0f, "R %.3f");
				});
				DrawMaterialPropertyRow("Metallic-Roughness Map", [&]
				{
					std::optional<PulseForge::AssetID> Texture = Material.MetallicRoughnessTexture.IsNil()
						? std::nullopt : std::optional<PulseForge::AssetID>(Material.MetallicRoughnessTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (DrawAssetSelector("##MaterialMetallicRoughnessTexture", Texture,
						{ ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.MetallicRoughnessTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
				DrawMaterialPropertyRow("Normal Map", [&]
				{
					std::optional<PulseForge::AssetID> Texture = Material.NormalTexture.IsNil()
						? std::nullopt : std::optional<PulseForge::AssetID>(Material.NormalTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (DrawAssetSelector("##MaterialNormalTexture", Texture,
						{ ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.NormalTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
				DrawMaterialPropertyRow("Normal Scale", [&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat("##MaterialNormalScale", &Material.NormalScale,
						0.02f, 0.0f, 4.0f, "%.3f");
				});
				DrawMaterialPropertyRow("Occlusion Map", [&]
				{
					std::optional<PulseForge::AssetID> Texture = Material.OcclusionTexture.IsNil()
						? std::nullopt : std::optional<PulseForge::AssetID>(Material.OcclusionTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (DrawAssetSelector("##MaterialOcclusionTexture", Texture,
						{ ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.OcclusionTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
				DrawMaterialPropertyRow("Occlusion Strength", [&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat("##MaterialOcclusionStrength", &Material.OcclusionStrength,
						0.01f, 0.0f, 1.0f, "%.3f");
				});
				DrawMaterialPropertyRow("Emissive Texture", [&]
				{
					std::optional<PulseForge::AssetID> Texture = Material.EmissiveTexture.IsNil()
						? std::nullopt : std::optional<PulseForge::AssetID>(Material.EmissiveTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (DrawAssetSelector("##MaterialEmissiveTexture", Texture,
						{ ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.EmissiveTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
				DrawMaterialPropertyRow("Emissive Factor (Linear)", [&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat3("##MaterialEmissiveFactor", &Material.EmissiveFactor.x,
						0.05f, 0.0f, 1000.0f, "%.3f");
				});
			}
			if (PropertyTableOpen)
				m_EditorStyle.EndPropertyTable();

			ImGui::PushStyleColor(ImGuiCol_Text,
				m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::TextMuted));
			ImGui::TextWrapped("Base color and emissive maps use sRGB; normal, metallic-roughness and occlusion maps use linear data.");
			ImGui::TextWrapped("Mask clips below Alpha Cutoff and casts cutout shadows. Blend is sorted back-to-front and does not cast shadows.");
			ImGui::PopStyleColor();

			if (Changed)
				m_DirtyMaterialDrafts.insert(Key);
			return true;
		}

		void DrawMaterialEditorActions(const PulseForge::AssetRecord& Asset)
		{
			const std::string Key = GetMaterialDraftKey(Asset.ID);
			const bool HasDraft = m_MaterialDrafts.contains(Key);
			const bool IsDirty = m_DirtyMaterialDrafts.contains(Key);
			if (IsDirty)
				ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning),
					"Unsaved material draft");
			else
				m_EditorStyle.TextMuted("No unsaved changes");

			const float AvailableWidth = ImGui::GetContentRegionAvail().x;
			const bool StackButtons = AvailableWidth < 320.0f;
			const float ButtonWidth = StackButtons ? -1.0f : (AvailableWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
			ImGui::BeginDisabled(!HasDraft || !IsDirty);
			if (ImGui::Button("Save Material", ImVec2(ButtonWidth, 0.0f)))
			{
				auto Draft = m_MaterialDrafts.find(Key);
				if (Draft != m_MaterialDrafts.end())
				{
					auto Saved = PulseForge::MaterialAssetService::Save(
						Asset.ID, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), Draft->second);
					if (!Saved)
						SetError("Material save failed: " + Saved.error().Message);
					else
					{
						m_DirtyMaterialDrafts.erase(Key);
						m_MaterialEditorErrors.erase(Key);
						m_AssetPreviewAssetsPending = true;
						if (m_SceneRenderer)
							m_SceneRenderer->RefreshMaterialAssets();
						SetStatus("Saved material " + PathToUtf8(Asset.ProjectRelativePath.filename()) + ".");
					}
				}
			}
			if (StackButtons)
			{
				if (ImGui::Button("Revert Material", ImVec2(-1.0f, 0.0f)))
				{
					auto Loaded = PulseForge::MaterialAssetService::Load(
						Asset.ID, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
					if (!Loaded)
						SetError("Material reload failed: " + Loaded.error().Message);
					else
					{
						m_MaterialDrafts[Key] = std::move(*Loaded);
						m_DirtyMaterialDrafts.erase(Key);
					}
				}
			}
			else
			{
				ImGui::SameLine();
				if (ImGui::Button("Revert Material", ImVec2(ButtonWidth, 0.0f)))
				{
					auto Loaded = PulseForge::MaterialAssetService::Load(
						Asset.ID, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
					if (!Loaded)
						SetError("Material reload failed: " + Loaded.error().Message);
					else
					{
						m_MaterialDrafts[Key] = std::move(*Loaded);
						m_DirtyMaterialDrafts.erase(Key);
					}
				}
			}
			ImGui::EndDisabled();
		}

		void DrawMaterialEditorPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::MaterialEditor))
				return;
			if (m_FocusMaterialEditorNextFrame)
			{
				ImGui::SetNextWindowFocus();
				m_FocusMaterialEditorNextFrame = false;
			}
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::MaterialEditor))
			{
				if (!m_Project)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Material,
						"Open a project to edit a material.");
				else if (!m_OpenedMaterialAsset)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Material,
						"Open a material from the Content Browser.");
				else
				{
					const auto Asset = m_Project->GetAssetRegistry().Find(*m_OpenedMaterialAsset);
					if (!Asset || NormalizedExtension(Asset->ProjectRelativePath) != ".material")
					{
						m_OpenedMaterialAsset.reset();
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Warning,
							"The opened material is no longer registered. Select it again in the Content Browser.");
					}
					else
					{
						const std::string Name = PathToUtf8(Asset->ProjectRelativePath.filename());
						const std::string FullPath = PathToUtf8(Asset->ProjectRelativePath);
						const std::string Identifier = Asset->ID.ToString();
						m_EditorStyle.IconText(PulseForgeEditor::EditorIcon::Material, Name);
						if (m_DirtyMaterialDrafts.contains(GetMaterialDraftKey(Asset->ID)))
						{
							ImGui::SameLine();
							ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning), "Unsaved");
						}
						ImGui::TextWrapped("%s", FullPath.c_str());
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s\nUUID: %s", FullPath.c_str(), Identifier.c_str());
						ImGui::Separator();

						const bool StackActions = ImGui::GetContentRegionAvail().x < 320.0f;
						const float FooterHeight =
							ImGui::GetFrameHeightWithSpacing() * (StackActions ? 3.0f : 2.0f) + 8.0f;
						const float PropertiesHeight = (std::max)(
							ImGui::GetContentRegionAvail().y - FooterHeight, 0.0f);
						if (ImGui::BeginChild("MaterialEditorProperties", ImVec2(0.0f, PropertiesHeight),
							ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar))
							(void)DrawMaterialAssetProperties(*Asset);
						ImGui::EndChild();
						ImGui::Separator();
						DrawMaterialEditorActions(*Asset);
					}
				}
			}
			ImGui::End();
		}

		void DrawContentBrowserPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::ContentBrowser))
				return;
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::ContentBrowser))
			{
				if (!m_Project)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::FolderOpen,
						"Open a project to browse managed assets.");
				else
				{
					std::function<void()> DeferredAction;
					const ImVec2 Available = ImGui::GetContentRegionAvail();
					const bool ShowFolderTree = Available.x >= 520.0f;
					if (ShowFolderTree)
					{
						const float FolderWidth = std::clamp(Available.x * 0.24f, 145.0f, 205.0f);
						ImGui::BeginChild("ContentFolderTree", ImVec2(FolderWidth, 0.0f), ImGuiChildFlags_Borders);
						DrawContentFolderNode(m_ContentFolderRoot, DeferredAction, true);
						ImGui::EndChild();
						ImGui::SameLine(0.0f, 6.0f);
					}

					ImGui::BeginChild("ContentAssetArea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
					if (ShowFolderTree)
						DrawContentBreadcrumbs();
					else
					{
						const std::string CurrentFolder = PathToUtf8(m_CurrentContentFolder);
						if (ImGui::BeginCombo("##ContentFolder", CurrentFolder.c_str()))
						{
							const auto DrawFolderOptions = [this](auto&& Self, const ContentFolderNode& Node) -> void
							{
								const std::string Path = PathToUtf8(Node.RelativePath);
								if (ImGui::Selectable(Path.c_str(), Node.RelativePath == m_CurrentContentFolder))
									m_CurrentContentFolder = Node.RelativePath;
								for (const ContentFolderNode& Child : Node.Children)
									Self(Self, Child);
							};
							DrawFolderOptions(DrawFolderOptions, m_ContentFolderRoot);
							ImGui::EndCombo();
						}
					}

					if (m_EditorStyle.BeginToolbar("ContentBrowserToolbar",
						PulseForgeEditor::EditorStyleMetrics::PanelToolbarHeight))
					{
						const bool Compact = ImGui::GetWindowWidth() < 440.0f;
						if (Compact
							? m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Import, "Import", "ImportAsset",
								"Import asset into the project")
							: m_EditorStyle.SmallButton(PulseForgeEditor::EditorIcon::Import, "Import"))
							ImportAsset();
						ImGui::SameLine();
						if (Compact
							? m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Refresh, "Refresh", "RefreshAssets",
								"Refresh the managed asset registry")
							: m_EditorStyle.SmallButton(PulseForgeEditor::EditorIcon::Refresh, "Refresh"))
							RefreshAssets();
						ImGui::SameLine();
						const float SearchWidth = (std::max)(ImGui::GetContentRegionAvail().x - 83.0f, 52.0f);
						ImGui::SetNextItemWidth(SearchWidth);
						if (m_FocusContentSearchNextFrame)
						{
							ImGui::SetKeyboardFocusHere();
							m_FocusContentSearchNextFrame = false;
						}
						ImGui::InputTextWithHint("##AssetSearch", "Search assets...", &m_ContentSearch);
						ImGui::SameLine();
						if (m_EditorStyle.ToolIconButton(PulseForgeEditor::EditorIcon::Grid, "Grid", "AssetGridMode",
							"Grid view", m_ContentGridView))
							m_ContentGridView = true;
						ImGui::SameLine();
						if (m_EditorStyle.ToolIconButton(PulseForgeEditor::EditorIcon::List, "List", "AssetListMode",
							"List view", !m_ContentGridView))
							m_ContentGridView = false;
					}
					m_EditorStyle.EndToolbar();

					m_FilteredAssetIndices.clear();
					for (size_t Index = 0; Index < m_Assets.size(); ++Index)
					{
						if (!PulseForgeEditor::MatchesManagedAssetFolderSearch(
							m_Assets[Index].ProjectRelativePath, m_CurrentContentFolder, m_ContentSearch))
							continue;
						m_FilteredAssetIndices.push_back(Index);
					}

					const ContentFolderNode* CurrentFolderNode =
						FindContentFolderNode(m_ContentFolderRoot, m_CurrentContentFolder);
					const bool ShowFolderTiles = m_ContentSearch.empty() && CurrentFolderNode &&
						!CurrentFolderNode->Children.empty();
					const std::vector<ContentFolderNode>* VisibleFolders = ShowFolderTiles
						? &CurrentFolderNode->Children : nullptr;
					bool SelectedAssetVisible = false;
					if (m_SelectedAsset)
						SelectedAssetVisible = std::any_of(m_FilteredAssetIndices.begin(), m_FilteredAssetIndices.end(),
							[this](size_t Index) { return m_Assets[Index].ID == *m_SelectedAsset; });

					bool SelectedMaterial = false;
					if (SelectedAssetVisible && m_SelectedAsset)
					{
						const auto SelectedRecord = m_Project->GetAssetRegistry().Find(*m_SelectedAsset);
						SelectedMaterial = SelectedRecord &&
							NormalizedExtension(SelectedRecord->ProjectRelativePath) == ".material";
					}
					const PulseForgeEditor::ContentBrowserVerticalLayout VerticalLayout =
						PulseForgeEditor::GetContentBrowserVerticalLayout(
							ImGui::GetContentRegionAvail().y,
							SelectedAssetVisible,
							SelectedMaterial,
							m_ContentDetailsExpandedCompact);
					ImGui::BeginChild("ContentAssetItems", ImVec2(0.0f, VerticalLayout.AssetItemsHeight),
						ImGuiChildFlags_None);
					if (VisibleFolders && !VisibleFolders->empty())
					{
						if (m_ContentGridView)
							DrawFolderGrid(*VisibleFolders, DeferredAction);
						else
							DrawFolderList(*VisibleFolders, DeferredAction);
						if (!m_FilteredAssetIndices.empty())
							ImGui::Separator();
					}
					if (m_ContentGridView)
						DrawAssetGrid(DeferredAction);
					else
						DrawAssetList(DeferredAction);
					if ((!VisibleFolders || VisibleFolders->empty()) && m_FilteredAssetIndices.empty())
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Search,
							m_Assets.empty() ? "The project has no managed assets." :
							"No assets or folders match this location.");
					ImGui::EndChild();
					const bool HasDeferredAction = static_cast<bool>(DeferredAction);
					if (DeferredAction)
						DeferredAction();
					if (!HasDeferredAction && SelectedAssetVisible && VerticalLayout.SelectionSummaryHeight > 0.0f)
					{
						const auto SelectedRecord = m_Project->GetAssetRegistry().Find(*m_SelectedAsset);
						std::string SelectedName = SelectedRecord
							? PathToUtf8(SelectedRecord->ProjectRelativePath.filename()) : "Selected asset";
						if (SelectedMaterial && m_DirtyMaterialDrafts.contains(GetMaterialDraftKey(*m_SelectedAsset)))
							SelectedName += " — Unsaved draft";
						const float ActionWidth = SelectedMaterial ? 154.0f : 112.0f;
						const float SummaryWidth = (std::max)(
							ImGui::GetContentRegionAvail().x - ActionWidth - ImGui::GetStyle().ItemSpacing.x, 20.0f);
						const std::string Summary = FitEditorContextLabel(SelectedName,
							SummaryWidth);
						ImGui::TextUnformatted(Summary.c_str());
						if (ImGui::IsItemHovered() && SelectedRecord)
						{
							const std::string FullPath = PathToUtf8(SelectedRecord->ProjectRelativePath);
							const std::string Identifier = SelectedRecord->ID.ToString();
							ImGui::SetTooltip("%s\nUUID: %s", FullPath.c_str(), Identifier.c_str());
						}
						ImGui::SameLine();
						if (SelectedMaterial)
						{
							if (ImGui::Button("Open Material Editor", ImVec2(ActionWidth, 0.0f)))
								OpenMaterialEditor(*m_SelectedAsset);
						}
						else if (ImGui::SmallButton(m_ContentDetailsExpandedCompact ? "Hide Details" : "Details"))
							m_ContentDetailsExpandedCompact = !m_ContentDetailsExpandedCompact;
					}
					if (!HasDeferredAction && SelectedAssetVisible && VerticalLayout.DetailsHeight > 0.0f)
					{
						ImGui::BeginChild("SelectedAssetDetails", ImVec2(0.0f, VerticalLayout.DetailsHeight),
							ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
						DrawSelectedAssetDetails();
						ImGui::EndChild();
					}
					ImGui::EndChild();
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
			CancelGizmoInteraction();
			EndEditorCameraNavigation();
			m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
				m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::PlayModeStarted);
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
			SetStatus("Runtime started on an isolated scene copy. The viewport now displays the runtime scene.");
		}

		void StopRuntime() noexcept
		{
			CancelGizmoInteraction();
			EndEditorCameraNavigation();
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
			m_AssetPreviewProjectPending = true;
			m_AssetPreviewAssetsPending = true;
			m_PickingMeshCache.clear();
			m_PickingMeshErrors.clear();
			if (m_Project)
				m_Assets = m_Project->GetAssetRegistry().GetAssets();
			else
				m_Assets.clear();

			m_AssetBrowserPresentations.clear();
			m_AssetBrowserPresentations.reserve(m_Assets.size());
			for (const PulseForge::AssetRecord& Asset : m_Assets)
			{
				m_AssetBrowserPresentations.push_back({
					Asset.ID.ToString(),
					PathToUtf8(Asset.ProjectRelativePath.filename()),
					PathToUtf8(Asset.ProjectRelativePath),
					NormalizedExtension(Asset.ProjectRelativePath),
					PulseForgeEditor::GetAssetBrowserIcon(Asset.ProjectRelativePath) });
			}
			RebuildContentFolderTree();
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
			CancelGizmoInteraction();
			ResetEditorViewportCamera();
			StopRuntime();
			m_ViewportSceneReady = false;
			ClearViewportSceneError();
			m_Scene.reset();
			m_SceneAsset.reset();
			SelectSingleEntity(std::nullopt);
			m_SelectedAsset.reset();
			m_PendingDeleteAsset.reset();
			m_OpenDeleteAssetDialog = false;
			ResetSceneHistory();
		}

		void ShutdownImGui() noexcept
		{
			m_AssetPreviews.Shutdown();
			WriteAssetPreviewStressReport();
			if (m_ImGuiRenderer)
			{
				m_ImGuiRenderer->Shutdown();
				m_ImGuiRenderer.reset();
			}
			if (!m_Context)
				return;

			ImGui::SetCurrentContext(m_Context);
			if (auto Result = m_Layout.SaveNow(); !Result)
				ReportLayoutPersistenceError(Result.error());
			else
				m_LayoutPersistenceError.clear();
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (m_OpenGLBackendActive)
				ImGui_ImplOpenGL3_Shutdown();
			#endif
			if (m_GlfwBackendActive)
				ImGui_ImplGlfw_Shutdown();
			m_EditorStyle.Reset();
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
		PulseForgeEditor::EditorLayout m_Layout;
		PulseForgeEditor::EditorStyle m_EditorStyle;
		std::unique_ptr<PulseForgeEditor::EditorImGuiRenderer> m_ImGuiRenderer;
		PulseForgeEditor::EditorAssetPreviewService m_AssetPreviews;
		std::optional<std::filesystem::path> m_AssetPreviewStressManifest;
		std::string m_AssetPreviewStressBudget;
		std::vector<std::pair<std::string, std::filesystem::path>> m_AssetPreviewStressAssets;
		std::string m_AssetPreviewStressProject = "Phase5StressProjectA";
		std::filesystem::path m_AssetPreviewStressReportPath;
		size_t m_AssetPreviewStressBatch = 0;
		size_t m_AssetPreviewStressFrames = 0;
		size_t m_AssetPreviewStressDraws = 0;
		size_t m_AssetPreviewStressSubmissions = 0;
		size_t m_AssetPreviewStressShutdownPendingRequests = 0;
		bool m_AssetPreviewStressSwitchedProject = false;
		bool m_AssetPreviewStressFinished = false;
		bool m_AssetPreviewStressReadyThisFrame = false;
		bool m_AssetPreviewStressRenderFailedThisFrame = false;
		bool m_AssetPreviewStressFailed = false;
		bool m_AssetPreviewStressInvalidatedQueued = false;
		bool m_AssetPreviewStressRefreshed = false;
		size_t m_AssetPreviewStressPass = 0;
		bool m_AssetPreviewStressReportWritten = false;
		bool m_AssetPreviewProjectPending = false;
		bool m_AssetPreviewAssetsPending = false;
		bool m_GlfwBackendActive = false;
		bool m_OpenGLBackendActive = false;
		#ifdef PF_EDITOR_RENDERER_OPENGL
		std::filesystem::path m_OpenGLDiagnosticCapturePath;
		bool m_OpenGLDiagnosticCaptureWritten = false;
		bool m_OpenGLDiagnosticPlatformCaptureWritten = false;
		bool m_OpenGLDiagnosticContentBrowserSelected = false;
		uint32_t m_OpenGLDiagnosticFramesSeen = 0;
		#endif
		bool m_FileDialogActive = false;
		bool m_ViewportSceneReady = false;
		bool m_ViewportImageHovered = false;
		bool m_ViewportViewProjectionValid = false;
		bool m_EnvironmentLightingPending = false;
		bool m_ProjectOpenTimingActive = false;
		bool m_LoggedFirstViewportPrepared = false;
		bool m_LoggedFirstViewportRendered = false;
		bool m_LoggedEnvironmentReady = false;
		bool m_EditorCameraNavigationActive = false;
		bool m_ViewportKeyboardFocused = false;
		PulseForgeEditor::Detail::ViewportMouseCaptureState m_ViewportMouseCapture;
		bool m_IgnoreFirstCursorDelta = false;
		bool m_PreviousImGuiNoMouse = false;
		bool m_PreviousRawMouseMotion = false;
		bool m_RawMouseMotionChanged = false;
		bool m_CursorRestoreValid = false;
		bool m_SceneRendererInitializationFailed = false;
		bool m_SceneDirty = false;
		PulseForgeEditor::EditorCommandHistory m_SceneHistory;
		PulseForgeEditor::EditorSelection m_Selection;
		PulseForgeEditor::EditorVisibility m_EditorVisibility;
		std::vector<PulseForge::UUID> m_HierarchyVisibleOrder;
		std::vector<PulseForge::UUID> m_HierarchyVisibleOrderCurrent;
		uint32_t m_InspectorHistoryItem = 0;
		bool m_FocusEntityNameNextFrame = false;
		bool m_FocusHierarchySearchNextFrame = false;
		bool m_FocusContentSearchNextFrame = false;
		bool m_OpenUnsavedDialog = false;
		bool m_ConsoleAutoScroll = true;
		bool m_ConsoleSelectableView = false;
		bool m_ConsoleSelectableTextInteracting = false;
		bool m_ConsoleSelectableTextInitialized = false;
		bool m_ConsoleSelectableAutoScrollSuspended = false;
		bool m_ContentGridView = true;
		bool m_ContentDetailsExpandedCompact = false;
		bool m_InitialMaterialEditorDockPending = false;
		bool m_FocusMaterialEditorNextFrame = false;
		uint64_t m_ConsoleRevision = 0;
		uint64_t m_ConsoleSelectableTextRevision = 0;
		std::string m_ConsoleSelectableText;
		std::shared_ptr<EditorConsoleSink> m_ConsoleSink;
		std::vector<EditorConsoleMessage> m_ConsoleMessages;
		std::vector<std::shared_ptr<spdlog::logger>> m_ConsoleLoggers;
		std::function<void()> m_PendingAction;
		std::optional<std::filesystem::path> m_StartupProjectPath;
		std::optional<PulseForge::Project> m_Project;
		std::unique_ptr<PulseForge::Scene> m_Scene;
		std::optional<std::string> m_SavedSceneSnapshot;
		std::unique_ptr<PulseForge::Scene> m_RuntimeScene;
		std::unique_ptr<PulseForge::SceneRuntime> m_SceneRuntime;
		std::unique_ptr<PulseForge::SceneRenderer> m_SceneRenderer;
		PulseForge::RenderTargetHandle m_ViewportTarget;
		glm::mat4 m_ViewportViewProjection{ 1.0f };
		PulseForgeEditor::ViewportImageRect m_ViewportImageRect;
		std::optional<GizmoDrag> m_GizmoDrag;
		PulseForgeEditor::TransformGizmoOperation m_GizmoOperation =
			PulseForgeEditor::TransformGizmoOperation::Translate;
		PulseForgeEditor::ViewportTool m_ViewportTool = PulseForgeEditor::ViewportTool::Move;
		bool m_GizmoLocalSpace = true;
		bool m_TransformSnapping = false;
		float m_TranslationSnapIncrement = 1.0f;
		float m_RotationSnapDegrees = 15.0f;
		float m_ScaleSnapIncrement = 0.1f;
		ViewportCameraGesture m_ViewportCameraGesture = ViewportCameraGesture::None;
		bool m_ShowEditorOverlays = true;
		std::unordered_map<PulseForge::AssetID, CpuPickingMesh, PulseForge::UUIDHash> m_PickingMeshCache;
		std::unordered_map<PulseForge::AssetID, std::string, PulseForge::UUIDHash> m_PickingMeshErrors;
		uint64_t m_ViewportTextureID = 0;
		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;
		float m_ExposureEV = 0.0f;
		int m_PreviousCursorMode = GLFW_CURSOR_NORMAL;
		float m_LastCursorX = 0.0f;
		float m_LastCursorY = 0.0f;
		double m_CursorRestoreX = 0.0;
		double m_CursorRestoreY = 0.0;
		EditorViewportCamera m_EditorCamera;
		std::optional<PulseForge::AssetID> m_SceneAsset;
		std::optional<PulseForge::UUID> m_SelectedEntity;
		std::optional<EntityClipboard> m_EntityClipboard;
		std::optional<PulseForge::AssetID> m_SelectedAsset;
		std::optional<PulseForge::AssetID> m_OpenedMaterialAsset;
		std::optional<PulseForge::AssetID> m_PendingDeleteAsset;
		std::unordered_map<std::string, PulseForge::MaterialAssetDesc> m_MaterialDrafts;
		std::unordered_map<std::string, std::string> m_MaterialEditorErrors;
		std::unordered_set<std::string> m_DirtyMaterialDrafts;
		std::vector<PulseForge::AssetRecord> m_Assets;
		std::vector<AssetBrowserPresentation> m_AssetBrowserPresentations;
		std::vector<size_t> m_FilteredAssetIndices;
		ContentFolderNode m_ContentFolderRoot{ "Assets", "Assets", {} };
		std::filesystem::path m_CurrentContentFolder{ "Assets" };
		std::optional<std::filesystem::path> m_SelectedContentFolder;
		std::string m_ContentSearch;
		std::string m_HierarchySearch;
		std::string m_WindowTitle;
		std::string m_ViewportTargetError;
		std::string m_ViewportSceneError;
		std::string m_ImGuiRenderingError;
		std::string m_LayoutPersistenceError;
		std::chrono::steady_clock::time_point m_ProjectOpenStartedAt{};
		std::string m_StatusMessage = "Create or open a project to begin.";
		bool m_StatusIsError = false;
		bool m_StatusIsWarning = false;
		bool m_OpenDeleteAssetDialog = false;
	};

	class PulseForgeEditorApplication final : public PulseForge::Application
	{
	public:
		explicit PulseForgeEditorApplication(PulseForgeEditor::EditorCommandLineOptions Options)
#ifdef PF_EDITOR_RENDERER_OPENGL
			: Application(PulseForge::RendererAPI::OpenGL)
#else
			: Application(PulseForge::RendererAPI::Vulkan)
#endif
		{
			PushLayer(std::make_unique<EditorLayer>(std::move(Options)));
#ifdef PF_EDITOR_RENDERER_OPENGL
			PF_INFO("PulseForge editor started with the OpenGL ImGui backend");
#else
			PF_INFO("PulseForge editor started with the Vulkan/NVRHI ImGui backend");
#endif
		}
	};
}

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication()
{
	return PulseForgeEditor::CreateEditorApplication({});
}

std::unique_ptr<PulseForge::Application> PulseForgeEditor::CreateEditorApplication(
	EditorCommandLineOptions Options)
{
	return std::make_unique<PulseForgeEditorApplication>(std::move(Options));
}
