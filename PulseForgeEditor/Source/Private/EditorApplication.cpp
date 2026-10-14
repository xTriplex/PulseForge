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
#include "Assets/AssetMetadata.h"
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
#include "Editor/EditorDeferredOperationQueue.h"
#include "EditorConsolePanel.h"
#include "EditorWorkspacePresentation.h"
#include "Editor/EditorLayout.h"
#include "EditorViewportCamera.h"
#include "EditorMaterialDraftStore.h"
#include "Editor/EditorContentBrowserPolicy.h"
#include "Editor/EditorContentBrowserState.h"
#include "Editor/EditorAssetWorkflowState.h"
#include "Editor/EditorSceneEditController.h"
#include "Editor/EditorAssetDestinationPolicy.h"
#include "Editor/EditorAssetSelectorPolicy.h"
#include "Editor/EditorProjectOpening.h"
#include "EditorApplicationFactory.h"
#include "Editor/EditorInteractionPolicy.h"
#include "Editor/EditorEntityIconPolicy.h"
#include "Editor/EditorHierarchyRowPolicy.h"
#include "Editor/EditorGlfwViewportPolicy.h"
#include "Editor/EditorMaterialEditorPolicy.h"
#include "Editor/EditorPlatformViewportPolicy.h"
#include "Editor/EditorMaterialPropertyLayoutPolicy.h"
#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorDocumentCloseBatch.h"
#include "Editor/EditorNativeWindowBoundsPolicy.h"
#include "Editor/EditorDocumentHostPolicy.h"
#include "Editor/EditorDocumentTabPolicy.h"
#include "Editor/EditorDocumentWorkspace.h"
#include "Editor/EditorSnapshotHistory.h"
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

#include <algorithm>
#include <array>
#include <fstream>
#include <charconv>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
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

	GLFWmonitor* FindMonitorForBounds(float X, float Y, float Width, float Height)
	{
		int MonitorCount = 0;
		GLFWmonitor** Monitors = glfwGetMonitors(&MonitorCount);
		GLFWmonitor* BestMonitor = nullptr;
		double BestArea = 0.0;
		for (int Index = 0; Monitors && Index < MonitorCount; ++Index)
		{
			int MonitorX = 0;
			int MonitorY = 0;
			glfwGetMonitorPos(Monitors[Index], &MonitorX, &MonitorY);
			const GLFWvidmode* Mode = glfwGetVideoMode(Monitors[Index]);
			if (!Mode)
				continue;
			const double IntersectionWidth = (std::max)(0.0,
				(std::min)(static_cast<double>(X + Width), static_cast<double>(MonitorX + Mode->width)) -
				(std::max)(static_cast<double>(X), static_cast<double>(MonitorX)));
			const double IntersectionHeight = (std::max)(0.0,
				(std::min)(static_cast<double>(Y + Height), static_cast<double>(MonitorY + Mode->height)) -
				(std::max)(static_cast<double>(Y), static_cast<double>(MonitorY)));
			const double Area = IntersectionWidth * IntersectionHeight;
			if (Area > BestArea)
			{
				BestArea = Area;
				BestMonitor = Monitors[Index];
			}
		}
		return BestArea >= 100.0 * 60.0 ? BestMonitor : nullptr;
	}

	struct ResolvedNativeWindowBounds
	{
		ImVec2 Position;
		ImVec2 Size;
	};

	ResolvedNativeWindowBounds ResolveRestoredNativeWindowBounds(
		const PulseForgeEditor::EditorDocumentHost::WindowBounds& Saved)
	{
		GLFWmonitor* Monitor = nullptr;
		int MonitorCount = 0;
		GLFWmonitor** Monitors = glfwGetMonitors(&MonitorCount);
		if (!Saved.MonitorName.empty())
			for (int Index = 0; Monitors && Index < MonitorCount; ++Index)
				if (Saved.MonitorName == (glfwGetMonitorName(Monitors[Index]) ? glfwGetMonitorName(Monitors[Index]) : ""))
				{
					Monitor = Monitors[Index];
					break;
				}
		if (!Monitor)
			Monitor = FindMonitorForBounds(Saved.X, Saved.Y, Saved.Width, Saved.Height);
		float X = Saved.X;
		float Y = Saved.Y;
		float Width = Saved.Width;
		float Height = Saved.Height;
		if (!Monitor)
		{
			const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			X = MainViewport->WorkPos.x + 80.0f;
			Y = MainViewport->WorkPos.y + 80.0f;
			Width = (std::min)(Width, (std::max)(320.0f, MainViewport->WorkSize.x - 120.0f));
			Height = (std::min)(Height, (std::max)(240.0f, MainViewport->WorkSize.y - 120.0f));
			Monitor = FindMonitorForBounds(MainViewport->Pos.x, MainViewport->Pos.y,
				MainViewport->Size.x, MainViewport->Size.y);
			if (!Monitor)
				Monitor = glfwGetPrimaryMonitor();
		}
		if (Monitor)
		{
			int WorkX = 0;
			int WorkY = 0;
			int WorkWidth = 0;
			int WorkHeight = 0;
			glfwGetMonitorWorkarea(Monitor, &WorkX, &WorkY, &WorkWidth, &WorkHeight);
			if (WorkWidth <= 0 || WorkHeight <= 0)
			{
				int MonitorX = 0;
				int MonitorY = 0;
				glfwGetMonitorPos(Monitor, &MonitorX, &MonitorY);
				const GLFWvidmode* Mode = glfwGetVideoMode(Monitor);
				if (Mode)
				{
					WorkX = MonitorX;
					WorkY = MonitorY;
					WorkWidth = Mode->width;
					WorkHeight = Mode->height;
				}
			}
			float ScaleX = 1.0f;
			float ScaleY = 1.0f;
			glfwGetMonitorContentScale(Monitor, &ScaleX, &ScaleY);
			auto PlacementBounds = Saved;
			PlacementBounds.X = X;
			PlacementBounds.Y = Y;
			PlacementBounds.Width = Width;
			PlacementBounds.Height = Height;
			const auto Resolved = PulseForgeEditor::ResolveNativeWindowRect(PlacementBounds,
				{ static_cast<float>(WorkX), static_cast<float>(WorkY),
					static_cast<float>(WorkWidth), static_cast<float>(WorkHeight) },
				(std::max)(ScaleX, ScaleY));
			return { ImVec2(Resolved.X, Resolved.Y), ImVec2(Resolved.Width, Resolved.Height) };
		}
		auto FallbackBounds = Saved;
		FallbackBounds.X = X;
		FallbackBounds.Y = Y;
		FallbackBounds.Width = Width;
		FallbackBounds.Height = Height;
		const auto Resolved = PulseForgeEditor::ResolveNativeWindowRect(FallbackBounds, {}, Saved.DpiScale);
		return { ImVec2(Resolved.X, Resolved.Y), ImVec2(Resolved.Width, Resolved.Height) };
	}

	PulseForgeEditor::EditorDocumentHost::WindowBounds MakeDocumentDropWindowBounds(ImVec2 MousePosition)
	{
		PulseForgeEditor::EditorDocumentHost::WindowBounds Bounds;
		Bounds.X = MousePosition.x - 24.0f;
		Bounds.Y = MousePosition.y - 12.0f;
		Bounds.Width = 560.0f;
		Bounds.Height = 760.0f;
		Bounds.DpiScale = 1.0f;
		Bounds.HasValue = true;
		if (GLFWmonitor* Monitor = FindMonitorForBounds(Bounds.X, Bounds.Y, Bounds.Width, Bounds.Height))
		{
			if (const char* MonitorName = glfwGetMonitorName(Monitor))
				Bounds.MonitorName = MonitorName;
			float ScaleX = 1.0f;
			float ScaleY = 1.0f;
			glfwGetMonitorContentScale(Monitor, &ScaleX, &ScaleY);
			Bounds.DpiScale = (std::max)(ScaleX, ScaleY);
		}
		return Bounds;
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

	std::expected<std::optional<std::string>, std::string> ReadDocumentWorkspaceFile(
		const std::filesystem::path& Path)
	{
		std::error_code Error;
		if (!std::filesystem::exists(Path, Error))
		{
			if (Error)
				return std::unexpected("Could not inspect document workspace: " + Error.message());
			return std::optional<std::string>{};
		}
		const uintmax_t Size = std::filesystem::file_size(Path, Error);
		if (Error || Size > 2u * 1024u * 1024u)
			return std::unexpected(Error ? "Could not inspect document workspace: " + Error.message()
				: "Document workspace exceeds the 2 MiB safety limit");
		std::ifstream Input(Path, std::ios::binary);
		if (!Input)
			return std::unexpected("Could not open the document workspace for reading");
		std::string Contents(static_cast<size_t>(Size), '\0');
		if (!Contents.empty())
			Input.read(Contents.data(), static_cast<std::streamsize>(Contents.size()));
		if (!Input && !Input.eof())
			return std::unexpected("Could not read the complete document workspace");
		return std::optional<std::string>(std::move(Contents));
	}

	std::expected<void, std::string> WriteDocumentWorkspaceFileAtomically(
		const std::filesystem::path& Path,
		std::string_view Contents)
	{
		if (Path.empty())
			return std::unexpected("The document workspace path is unavailable");
		std::filesystem::path TemporaryPath = Path;
		TemporaryPath += L".tmp";
		{
			std::ofstream Output(TemporaryPath, std::ios::binary | std::ios::trunc);
			if (!Output)
				return std::unexpected("Could not create a temporary document workspace file");
			Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			Output.flush();
			if (!Output)
			{
				Output.close();
				std::error_code RemoveError;
				std::filesystem::remove(TemporaryPath, RemoveError);
				return std::unexpected("Could not write the document workspace");
			}
		}
		if (!MoveFileExW(TemporaryPath.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD ErrorCode = GetLastError();
			std::error_code RemoveError;
			std::filesystem::remove(TemporaryPath, RemoveError);
			return std::unexpected("Could not atomically replace document workspace (Windows error " +
				std::to_string(ErrorCode) + ")");
		}
		return {};
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

	class EditorLayer final : public PulseForge::Layer
	{
		struct DocumentHostTestState final : PulseForgeEditor::EditorDocumentEditorState
		{
			int Value = 0;
			bool FailNextSave = false;
			bool FailClose = false;
			std::vector<int> Undo;
			std::vector<int> Redo;
		};

		struct PlatformViewportRendererData
		{
			uint64_t TargetID = 0;
			bool FrameActive = false;
		};

		struct CpuPickingMesh
		{
			std::vector<glm::vec3> Positions;
			std::vector<uint32_t> Indices;
		};

		using ContentFolderNode = PulseForgeEditor::EditorContentFolderNode;
		using ProjectAssetDestinationCreateKind = PulseForgeEditor::ProjectAssetDestinationCreateKind;

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

		struct PendingDocumentTransfer
		{
			PulseForgeEditor::EditorDocumentIdentity Identity;
			std::string DestinationHostID;
			size_t DestinationIndex = 0;
			bool CreateNativeHost = false;
			std::optional<PulseForgeEditor::EditorDocumentHost::WindowBounds> NativeHostBounds;
		};

		struct DocumentTabDropTarget
		{
			std::string HostID;
			ImGuiID ViewportID = 0;
			ImVec2 Minimum{ 0.0f, 0.0f };
			ImVec2 Maximum{ 0.0f, 0.0f };
			size_t BeforeIndex = 0;
			size_t AfterIndex = 0;
		};

		struct DocumentTabTestPointerTarget
		{
			const char* Name = nullptr;
			ImVec2 Position{ 0.0f, 0.0f };
			ImGuiID ViewportID = 0;
			bool Available = false;
		};

		enum class DocumentTabTearOffTestPhase : uint8_t
		{
			Disabled,
			WaitingForTab,
			PressQueued,
			DragQueued,
			ReleaseQueued,
			Complete,
			Failed
		};

		enum class DocumentTabReattachTestPhase : uint8_t
		{
			Disabled,
			WaitingForTab,
			InvalidDropPressQueued,
			InvalidDropPositionQueued,
			InvalidDropReleaseQueued,
			ValidDropPressQueued,
			ValidDropPositionQueued,
			ValidDropReleaseQueued,
			Complete,
			Failed
		};

		enum class DocumentTabReorderTestPhase : uint8_t
		{
			Disabled,
			WaitingForTabs,
			ForwardPressQueued,
			ForwardPositionQueued,
			ForwardReleaseQueued,
			WaitingForReverse,
			ReversePressQueued,
			ReversePositionQueued,
			ReverseReleaseQueued,
			Complete,
			Failed
		};

		enum class DocumentTabGroupTestPhase : uint8_t
		{
			Disabled,
			WaitingForTabs,
			PressQueued,
			PositionQueued,
			ReleaseQueued,
			Complete,
			Failed
		};

		enum class DocumentWorkspaceSwitchTestPhase : uint8_t
		{
			Disabled,
			WaitingForFirstFixture,
			WaitingForLevel,
			WaitingForFirstFixtureAgain,
			WaitingForSecondFixture,
			WaitingForNativeTransfer,
			WaitingForMainTransfer,
			Complete,
			Failed
		};

		static constexpr size_t DocumentTabTestDropPointsPerPanel = 5;
		static constexpr size_t DocumentTabRoundTripCycleCount = 3;
		static constexpr std::array<const char*, 5> DocumentTabTestInvalidDropPanelNames = {
			"Hierarchy", "Inspector", "Scene Viewport", "Content Browser", "Console"
		};
		static constexpr size_t DocumentTabTestConsolePanelIndex =
			DocumentTabTestInvalidDropPanelNames.size() - 1;
		static constexpr std::array<const char*,
			DocumentTabTestInvalidDropPanelNames.size() * DocumentTabTestDropPointsPerPanel>
			DocumentTabTestInvalidDropTargetNames = {
				"Hierarchy body", "Hierarchy top edge", "Hierarchy left edge", "Hierarchy right edge", "Hierarchy bottom edge",
				"Inspector body", "Inspector top edge", "Inspector left edge", "Inspector right edge", "Inspector bottom edge",
				"Scene Viewport body", "Scene Viewport top edge", "Scene Viewport left edge",
				"Scene Viewport right edge", "Scene Viewport bottom edge",
				"Content Browser body", "Content Browser top edge", "Content Browser left edge",
				"Content Browser right edge", "Content Browser bottom edge",
				"Console body", "Console top edge", "Console left edge", "Console right edge", "Console bottom edge"
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
			  m_AssetPreviewStressManifest(std::move(Options.AssetPreviewStressManifest)),
			  m_AssetPreviewStressBudget(std::move(Options.AssetPreviewStressBudget)),
			  m_StartupProjectPath(std::move(Options.ProjectFile)),
			  m_ProjectOpenRenderTestFile(std::move(Options.ProjectOpenRenderTestFile)),
			  m_DocumentHostTestFixture(Options.EnableDocumentHostTestFixture),
			  m_DocumentHostResetLayoutTest(Options.EnableDocumentHostResetLayoutTest),
			  m_DocumentHostTabTearOffTest(Options.EnableDocumentHostTabTearOffTest),
			  m_DocumentHostTabReattachTest(Options.EnableDocumentHostTabReattachTest),
			  m_DocumentHostTabReorderTest(Options.EnableDocumentHostTabReorderTest),
			  m_DocumentWorkspaceSwitchTest(Options.EnableDocumentWorkspaceSwitchTest),
			  m_DocumentHostPresentationFailureTest(Options.EnableDocumentHostPresentationFailureTest),
			  m_DocumentHostAcquireOutOfDateTest(Options.EnableDocumentHostAcquireOutOfDateTest),
			  m_DocumentHostImageAllocationFailureTest(Options.EnableDocumentHostImageAllocationFailureTest)
		{
			#ifdef _DEBUG
			if (m_DocumentHostTestFixture)
				RegisterDocumentHostTestEditor();
			#endif
		}

		static EditorLayer* GetPlatformCallbackLayer() noexcept
		{
			ImGuiContext* Context = ImGui::GetCurrentContext();
			return Context ? static_cast<EditorLayer*>(ImGui::GetIO().UserData) : nullptr;
		}

		static void OnRendererCreateViewport(ImGuiViewport* Viewport)
		{
			EditorLayer* Layer = GetPlatformCallbackLayer();
			if (!Layer || !Viewport || !Viewport->PlatformHandle)
				return;
			if (Layer->ShouldInjectDocumentPresentationFailure(Viewport))
			{
				Layer->HandlePlatformPresentationFailure(Viewport, "Debug fixture injected a presentation failure");
				return;
			}
			bool InjectAcquireOutOfDate = false;
#if defined(PF_DEBUG)
			InjectAcquireOutOfDate = Layer->ShouldInjectDocumentAcquireOutOfDate(Viewport);
			if (Layer->ShouldInjectDocumentImageAllocationFailure(Viewport) &&
				!PulseForge::Application::Get().InjectNextPlatformImageAllocationFailureForTesting())
			{
				Layer->HandlePlatformPresentationFailure(
					Viewport, "The active renderer does not support the image-allocation failure fixture");
				return;
			}
#endif
			auto Target = PulseForge::Application::Get().CreatePlatformPresentationTarget(Viewport->PlatformHandle);
			if (!Target)
			{
				if (Layer)
					Layer->HandlePlatformPresentationFailure(Viewport, Target.error());
				else
					PF_ERROR("Could not create native editor presentation target: {}", Target.error());
				return;
			}
			try
			{
				Viewport->RendererUserData = new PlatformViewportRendererData{ *Target, false };
#if defined(PF_DEBUG)
				if (InjectAcquireOutOfDate &&
					!PulseForge::Application::Get().InjectNextPlatformAcquireOutOfDateForTesting(*Target))
				{
					PF_ERROR("The active renderer could not arm the native-host acquire out-of-date fixture");
					auto* RendererData = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
					Viewport->RendererUserData = nullptr;
					delete RendererData;
					(void)PulseForge::Application::Get().DestroyPlatformPresentationTarget(*Target);
					Layer->HandlePlatformPresentationFailure(
						Viewport, "The active renderer could not arm the acquire out-of-date fixture");
					return;
				}
#endif
			}
			catch (const std::exception& Exception)
			{
				(void)PulseForge::Application::Get().DestroyPlatformPresentationTarget(*Target);
				PF_ERROR("Could not allocate native editor renderer state: {}", Exception.what());
			}
		}

		static void OnRendererDestroyViewport(ImGuiViewport* Viewport)
		{
			if (!Viewport || !Viewport->RendererUserData)
				return;
			auto* Data = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
			if (Data->FrameActive)
				PulseForge::Application::Get().EndPlatformPresentationTarget(Data->TargetID);
			const bool Retired = PulseForge::Application::Get().DestroyPlatformPresentationTarget(Data->TargetID);
			if (!Retired)
				PF_ERROR("Native Vulkan presentation retirement is deferred until renderer shutdown.");
			if (EditorLayer* Layer = GetPlatformCallbackLayer(); Layer && Layer->m_ImGuiRenderer)
				if (Retired)
					Layer->m_ImGuiRenderer->RetirePresentationTarget(Data->TargetID);
			delete Data;
			Viewport->RendererUserData = nullptr;
		}

		static void OnRendererRenderViewport(ImGuiViewport* Viewport, void*)
		{
			EditorLayer* Layer = GetPlatformCallbackLayer();
			if (!Layer || !Layer->m_ImGuiRenderer || !Viewport || !Viewport->RendererUserData)
				return;
			auto* Data = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
			if (!PulseForge::Application::Get().BeginPlatformPresentationTarget(Data->TargetID))
				return;
			Data->FrameActive = true;
			if (auto Result = Layer->m_ImGuiRenderer->RenderDrawData(Viewport->DrawData, Data->TargetID); !Result)
				PF_ERROR("Native editor window ImGui rendering failed: {}", Result.error());
		}

		static void OnRendererSwapViewport(ImGuiViewport* Viewport, void*)
		{
			if (!Viewport || !Viewport->RendererUserData)
				return;
			auto* Data = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
			if (!Data->FrameActive)
				return;
			PulseForge::Application::Get().EndPlatformPresentationTarget(Data->TargetID);
			Data->FrameActive = false;
		}

		~EditorLayer() override
		{
			EndEditorCameraNavigation();
			CancelGizmoInteraction();
			StopRuntime();
			m_ConsolePanel.Detach();
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
			m_PreviousImGuiUserData = IO.UserData;
			IO.UserData = this;
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
			m_Documents.SetOpeningPreference(static_cast<PulseForgeEditor::EditorDocumentOpenPreference>(
				m_Layout.GetDocumentOpeningPreference()));

			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			RequestDarkNativeCaption(Window);
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!ImGui_ImplGlfw_InitForOpenGL(Window, true))
			#else
			if (!ImGui_ImplGlfw_InitForVulkan(Window, true))
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
			IO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
			ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
			PlatformIO.Renderer_CreateWindow = &OnRendererCreateViewport;
			PlatformIO.Renderer_DestroyWindow = &OnRendererDestroyViewport;
			PlatformIO.Renderer_RenderWindow = &OnRendererRenderViewport;
			PlatformIO.Renderer_SwapBuffers = &OnRendererSwapViewport;
			IO.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
			#endif
			m_ConsolePanel.Attach();

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
			m_DeferredEditorOperations.Clear();
			EndEditorCameraNavigation();
			CancelGizmoInteraction();
			StopRuntime();
			m_ConsolePanel.Detach();
			ResetViewportSceneRenderer();
			ResetViewportTarget();
			m_Scene.reset();
			ShutdownImGui();
			ShutdownFileDialog();
			m_Project.reset();
		}

		void OnUpdate(PulseForge::Timestep DeltaTime) override
		{
			(void)m_DeferredEditorOperations.ExecuteNext();
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (m_Context)
			{
				ImGui::SetCurrentContext(m_Context);
				const ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
				const uint32_t SecondaryViewportCount = PlatformIO.Viewports.Size > 1
					? static_cast<uint32_t>(PlatformIO.Viewports.Size - 1) : 0;
				PulseForge::Application::Get().SetExternalPlatformPresentationTargetCount(SecondaryViewportCount);
			}
			#endif
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
				RequestEditorApplicationClose();
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
					RequestEditorApplicationClose();
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
				if (Index == 0 || !Viewport)
					continue;
				const PulseForgeEditor::EditorGlfwViewportHandles Handles{
					.GlfwWindow = Viewport->PlatformHandle,
					.NativeWindow = Viewport->PlatformHandleRaw
				};
				GLFWwindow* PlatformWindow = static_cast<GLFWwindow*>(
					PulseForgeEditor::GetGlfwContextWindow(Handles));
				if (!PlatformWindow)
					continue;
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
			QueueDocumentHostTabTearOffTestInput();
			QueueDocumentHostTabReattachTestInput();
			QueueDocumentTabReorderTestInput();
			QueueDocumentTabGroupTestInput();
			ImGui::NewFrame();
			m_DocumentTabDropTargets.clear();
			for (DocumentTabTestPointerTarget& Target : m_DocumentHostTabReattachInvalidDropTargets)
				Target.Available = false;
			m_DocumentHostTabReorderTestFirstTabCenterValid = false;
			m_DocumentHostTabReorderTestSecondBeforePositionValid = false;
			m_DocumentHostTabReorderTestSecondAfterPositionValid = false;
			m_DocumentHostTabGroupSourceCenterValid = false;
			m_DocumentHostTabGroupTargetCenterValid = false;
			FinalizePendingMaterialEdits();
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
			if (m_ProjectOpenRenderTestFile && !m_ProjectOpenRenderTestRequested)
			{
				m_ProjectOpenRenderTestRequested = true;
				const std::filesystem::path ProjectFile = *m_ProjectOpenRenderTestFile;
				PF_INFO("Project-open render-stage fixture queued a project load from the editor frame.");
				QueueAfterSave([this, ProjectFile]
				{
					PF_INFO("Project-open render-stage fixture executing the queued project load.");
					if (auto Result = OpenProjectAt(ProjectFile); !Result)
						PF_ERROR("Project-open render-stage fixture failed: {}", Result.error());
				});
			}
			DrawEditorToolbar();
			DrawMainDocumentStrip();
			DrawStatusBar();
			const auto* MainDocumentHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
			const bool MainLevelWorkspaceActive = !MainDocumentHost || !MainDocumentHost->ActiveDocument ||
				MainDocumentHost->ActiveDocument->Kind == PulseForgeEditor::EditorDocumentKind::Level;
			if (m_Layout.SubmitDockspace(!MainLevelWorkspaceActive))
			{
				CancelViewportInteractionForLayoutChange();
				ValidateDocumentHostResetLayoutTest();
			}
			DrawWorkspacePanels();
			AdvanceDocumentWorkspaceSwitchTest();
			ProcessReleasedDocumentTabDrag();
			ProcessPendingDocumentTabClose();
			ProcessPendingDocumentTransfer();
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!m_OpenGLDiagnosticCapturePath.empty())
			{
				if (const char* Folder = std::getenv("PULSEFORGE_EDITOR_OPENGL_CONTENT_FOLDER");
					Folder && *Folder)
				{
					const std::filesystem::path RequestedFolder = std::filesystem::u8path(Folder);
					m_ContentBrowser.SetCurrentFolder(RequestedFolder);
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
			if (PulseForge::Application::Get().IsMainFrameActive())
				RenderViewportScene();
			UpdateWindowTitle();
			if (PulseForgeEditor::ShouldFinishSceneEditForInspector(
				m_SceneEditController.IsActive(), m_SceneEditController.InspectorHistoryItem(), GImGui ? GImGui->ActiveId : 0))
				FinishSceneEdit();

			ImGui::Render();
			if (auto Result = m_Layout.SaveIfRequested(); !Result)
				ReportLayoutPersistenceError(Result.error());
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (PulseForge::Application::Get().IsMainFrameActive())
			{
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
				CaptureOpenGLFrameForValidation();
			}
			#else
			if (PulseForge::Application::Get().IsMainFrameActive() && m_ImGuiRenderer)
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
			#endif
			SaveDocumentWorkspaceIfNeeded();
		}

		void OnRenderPlatformWindows() override
		{
			ImGui::SetCurrentContext(m_Context);
			if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				if (PulseForgeEditor::ShouldProcessNativeDocumentHostCloseRequests(
					m_ApplicationShutdownRequested))
					HandleNativePlatformCloseRequests();
				#ifdef PF_EDITOR_RENDERER_OPENGL
				GLFWwindow* MainContext = glfwGetCurrentContext();
				#endif
				ImGui::UpdatePlatformWindows();
				ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
				for (int Index = 1; Index < PlatformIO.Viewports.Size; ++Index)
				{
					ImGuiViewport* Viewport = PlatformIO.Viewports[Index];
					if (!Viewport || (Viewport->Flags & ImGuiViewportFlags_IsMinimized))
						continue;

					PulseForgeEditor::RenderPlatformViewportSequentially(
						[&]
						{
							if (PlatformIO.Platform_RenderWindow)
								PlatformIO.Platform_RenderWindow(Viewport, nullptr);
						},
						[&]
						{
							if (PlatformIO.Renderer_RenderWindow)
								PlatformIO.Renderer_RenderWindow(Viewport, nullptr);
						},
						[&]
						{
							if (PlatformIO.Platform_SwapBuffers)
								PlatformIO.Platform_SwapBuffers(Viewport, nullptr);
						},
						[&]
						{
							if (PlatformIO.Renderer_SwapBuffers)
								PlatformIO.Renderer_SwapBuffers(Viewport, nullptr);
						});
				}
				#ifdef PF_EDITOR_RENDERER_OPENGL
				CaptureOpenGLPlatformFramesForValidation(MainContext);
				glfwMakeContextCurrent(MainContext);
				#endif
			}
			#ifndef PF_EDITOR_RENDERER_OPENGL
			if (m_AssetPreviewStressManifest)
				AdvanceAssetPreviewStressFrame();
			#endif
		}

		void OnEvent(PulseForge::Event& Event) override
		{
			const bool NavigationOwnedMouseBeforeEvent = m_ViewportMouseCapture.OwnsMouse();
			bool BeganNavigationForEvent = false;
			if (Event.GetEventType() == PulseForge::EEventType::WindowClose)
			{
				QueueAfterSave([this] { RequestEditorApplicationClose(); });
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
		void RequestEditorApplicationClose()
		{
			if (m_ApplicationShutdownRequested)
				return;
			std::vector<PulseForgeEditor::EditorDocumentIdentity> Documents;
			for (const auto& [Identity, Session] : m_Documents.GetSessions())
				if (Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level)
					Documents.push_back(Identity);
			if (GetDirtyAssetDocuments().empty() && CloseDocumentBatchClean(Documents))
			{
				CompleteEditorApplicationClose();
				return;
			}
			m_PendingDocumentClose.reset();
			m_PendingDocumentHostClose.reset();
			m_PendingDocumentCloseBatch = std::move(Documents);
			m_PendingCloseAllDocuments = true;
			m_DocumentCloseDialogHostID = std::string(PulseForgeEditor::EditorDocumentManager::MainHostID);
			m_PendingDocumentCloseAction = [this] { CompleteEditorApplicationClose(); };
			m_OpenDocumentCloseDialog = true;
			if (m_DocumentCloseError.empty())
				m_DocumentCloseError = "Resolve open documents before closing PulseForge.";
		}

		void CompleteEditorApplicationClose()
		{
			m_ApplicationShutdownRequested = true;
			PulseForge::Application::Get().RequestClose();
		}

		[[nodiscard]] std::expected<void, std::string> RestoreSceneState(std::string_view State)
		{
			if (!m_Scene || m_SceneRuntime)
				return std::unexpected("Scene history is available only for the authored scene in Edit mode");
			auto Restored = m_SceneEditController.Restore(State, *m_Scene, m_SceneWorkspace);
			if (!Restored)
				return std::unexpected(Restored.error());
			m_GizmoDrag.reset();
			m_SceneEditController.InspectorHistoryItem() = 0;
			UpdateSceneDirty();
			return {};
		}
		[[nodiscard]] const std::optional<PulseForge::UUID>& SelectedEntity() const noexcept
		{
			return m_SceneWorkspace.Selection().GetPrimary();
		}

		void SetEditorSelection(std::vector<PulseForge::UUID> Identifiers,
			std::optional<PulseForge::UUID> Primary = std::nullopt)
		{
			m_SceneWorkspace.Selection().Set(std::move(Identifiers), Primary);
		}

		void SelectSingleEntity(std::optional<PulseForge::UUID> Identifier)
		{
			if (Identifier)
			{
				m_SceneWorkspace.Selection().SelectSingle(*Identifier);
			}
			else
			{
				m_SceneWorkspace.Selection().Clear();
			}
		}

		[[nodiscard]] bool IsEntitySelected(const PulseForge::UUID& Identifier) const noexcept
		{
			return m_SceneWorkspace.Selection().Contains(Identifier);
		}

		[[nodiscard]] bool IsEntityEffectivelyVisible(const PulseForge::Entity& Entity) const
		{
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Visited;
			PulseForge::Entity Current = Entity;
			while (Current)
			{
				if (!Visited.insert(Current.GetUUID()).second ||
					!m_SceneWorkspace.Visibility().IsLocallyVisible(Current.GetUUID()))
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
			m_SceneWorkspace.Selection().Toggle(Identifier);
		}

		void SelectHierarchyRange(const PulseForge::UUID& Identifier, bool AddToSelection)
		{
			(void)m_SceneWorkspace.Selection().SelectRange(
				m_SceneWorkspace.VisibleHierarchyOrder(), Identifier, AddToSelection);
		}

		[[nodiscard]] std::expected<std::vector<PulseForge::Entity>, std::string> GetTopLevelSelectedEntities() const
		{
			if (!m_Scene)
				return std::unexpected("There is no active scene");

			const std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> Selected(
				m_SceneWorkspace.Selection().GetEntities().begin(), m_SceneWorkspace.Selection().GetEntities().end());
			std::vector<PulseForge::Entity> Roots;
			for (const PulseForge::UUID& Identifier : m_SceneWorkspace.Selection().GetEntities())
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
			const bool MakeVisible = !m_SceneWorkspace.Visibility().IsLocallyVisible(Identifier);
			const auto Recorded = RunSceneEdit(MakeVisible ? "Show Entity" : "Hide Entity", [this, Identifier, MakeVisible]()
				-> std::expected<void, PulseForge::SceneError>
			{
				m_SceneWorkspace.Visibility().SetVisible(Identifier, MakeVisible);
				return {};
			});
			if (!Recorded)
				SetError("Could not update editor visibility: " + Recorded.error());
		}

		void HideSelectedEntities()
		{
			if (!m_Scene || m_SceneRuntime || m_SceneWorkspace.Selection().GetEntities().empty())
				return;
			const std::vector<PulseForge::UUID> Identifiers = m_SceneWorkspace.Selection().GetEntities();
			const auto Recorded = RunSceneEdit("Hide Selected", [this, Identifiers]()
				-> std::expected<void, PulseForge::SceneError>
			{
				(void)m_SceneWorkspace.Visibility().Hide(Identifiers);
				return {};
			});
			if (!Recorded)
				SetError("Could not hide the selection: " + Recorded.error());
			else
				SetStatus("Hid " + std::to_string(Identifiers.size()) + " selected entities in the editor viewport.");
		}

		void ShowAllEntities()
		{
			if (!m_Scene || m_SceneRuntime || m_SceneWorkspace.Visibility().GetHiddenEntities().empty())
				return;
			const size_t RestoredCount = m_SceneWorkspace.Visibility().GetHiddenEntities().size();
			const auto Recorded = RunSceneEdit("Show All Entities", [this]() -> std::expected<void, PulseForge::SceneError>
			{
				m_SceneWorkspace.Visibility().ShowAll();
				return {};
			});
			if (!Recorded)
				SetError("Could not restore editor visibility: " + Recorded.error());
			else
				SetStatus("Restored visibility for " + std::to_string(RestoredCount) + " hidden entities.");
		}

		void IsolateSelectedEntities()
		{
			if (!m_Scene || m_SceneRuntime || m_SceneWorkspace.Selection().GetEntities().empty())
				return;
			std::unordered_set<PulseForge::UUID, PulseForge::UUIDHash> KeepVisible;
			for (const PulseForge::UUID& Identifier : m_SceneWorkspace.Selection().GetEntities())
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
				if (!m_SceneWorkspace.Visibility().Isolate(Keep, All))
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
			if (!m_Scene || m_SceneRuntime || !m_SceneWorkspace.Visibility().GetIsolationBaseline())
				return;
			const auto Recorded = RunSceneEdit("Restore Isolated Visibility", [this]()
				-> std::expected<void, PulseForge::SceneError>
			{
				(void)m_SceneWorkspace.Visibility().RestoreIsolation();
				return {};
			});
			if (!Recorded)
				SetError("Could not restore the prior visibility state: " + Recorded.error());
			else
				SetStatus("Restored visibility before isolation.");
		}

		void UpdateSceneDirty()
		{
			m_SceneEditController.UpdateDirty(m_Scene.get());
		}

		void ResetSceneHistory()
		{
			m_SceneEditController.Reset(m_Scene.get());
			m_SceneWorkspace.Visibility().ShowAll();
		}

		void CancelSceneEdit()
		{
			const auto Result = m_SceneEditController.Cancel(
				m_Scene.get(), m_SceneWorkspace,
				[this](std::string_view State) { return RestoreSceneState(State); });
			if (!Result)
				SetError("Scene edit rollback failed: " + Result.error());
		}

		bool BeginSceneEdit(std::string Description)
		{
			if (auto Begun = m_SceneEditController.Begin(
				m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, std::move(Description),
				[this](std::string_view State) { return RestoreSceneState(State); }); !Begun)
			{
				SetError("Could not begin the scene edit: " + Begun.error());
				return false;
			}
			return true;
		}

		void MarkSceneSaved()
		{
			m_SceneEditController.MarkSaved(m_Scene.get());
		}

		void FinishSceneEdit()
		{
			if (auto Result = m_SceneEditController.Finish(
				m_Scene.get(), m_SceneWorkspace,
				[this](std::string_view State) { return RestoreSceneState(State); }); !Result)
				SetError(Result.error());
		}

		template<typename Action>
		[[nodiscard]] std::expected<void, std::string> RunSceneEdit(std::string Description, Action&& Operation)
		{
			const uint32_t ActiveItem = GImGui ? GImGui->ActiveId : 0;
			return m_SceneEditController.Run(
				m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, ActiveItem,
				[] { return GImGui ? GImGui->ActiveId : 0; }, std::move(Description), std::forward<Action>(Operation),
				[this](std::string_view State) { return RestoreSceneState(State); });
		}

		void UndoSceneEdit()
		{
			if (!m_Scene || m_SceneRuntime)
				return;
			const auto Result = m_SceneEditController.Undo(
				m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr,
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
			const auto Result = m_SceneEditController.Redo(
				m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr,
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
			const PulseForge::UUID CurrentProject = m_Project
				? m_Project->GetDescription().ID : PulseForge::UUID{};
			const auto CommandTarget = PulseForgeEditor::ResolveEditorDocumentCommandTarget(
				GetFocusedDocumentIdentity(), CurrentProject);
			const bool RegisteredDocumentFocused = GetFocusedRegisteredDocument() != nullptr;
			const bool MaterialDetailsFocused = IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails);
			const bool MaterialDocumentFocused = GetFocusedMaterialAsset().has_value();
			const bool UnsupportedDocumentFocused =
				(!RegisteredDocumentFocused &&
					CommandTarget == PulseForgeEditor::EditorDocumentCommandTarget::Unsupported) ||
				(MaterialDetailsFocused && !MaterialDocumentFocused);
			if (!PulseForgeEditor::CanRouteEditorShortcut({
				.SceneAvailable = RegisteredDocumentFocused || MaterialDocumentFocused ||
					(!UnsupportedDocumentFocused && m_Scene != nullptr),
				.RuntimeActive = !RegisteredDocumentFocused && !MaterialDocumentFocused && m_SceneRuntime != nullptr,
				.CameraNavigationActive = !RegisteredDocumentFocused && !MaterialDocumentFocused && m_EditorCameraNavigationActive,
				.GizmoInteractionActive = !RegisteredDocumentFocused && !MaterialDocumentFocused && m_GizmoDrag.has_value(),
				.TextInputActive = IO.WantTextInput,
				.ImGuiItemActive = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui && GImGui->NavLayer == ImGuiNavLayer_Menu,
				.WindowNavigationActive = GImGui && GImGui->NavWindowingTarget != nullptr
			}))
				return;

			if (!IO.KeyCtrl || IO.KeyAlt || IO.KeySuper)
				return;
			if (RegisteredDocumentFocused)
			{
				auto* Session = GetFocusedRegisteredDocument();
				if (Session && (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
					(ImGui::IsKeyPressed(ImGuiKey_Z, false) && IO.KeyShift)))
					(void)m_DocumentEditors.DispatchFocusedCommand(
						*Session, PulseForgeEditor::EditorDocumentCommand::Redo);
				else if (Session && ImGui::IsKeyPressed(ImGuiKey_Z, false))
					(void)m_DocumentEditors.DispatchFocusedCommand(
						*Session, PulseForgeEditor::EditorDocumentCommand::Undo);
				return;
			}
			if (MaterialDocumentFocused)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
					(ImGui::IsKeyPressed(ImGuiKey_Z, false) && IO.KeyShift))
					(void)ApplyMaterialHistoryStep(true);
				else if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
					(void)ApplyMaterialHistoryStep(false);
				return;
			}
			if (UnsupportedDocumentFocused)
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
			if (Panel == PulseForgeEditor::EditorPanel::MaterialDetails)
				return Root && Root->ID == ImHashStr("Material Editor");
			return Root && std::string_view(Root->Name) ==
				PulseForgeEditor::EditorPanelDescriptors[static_cast<size_t>(Panel)].WindowName;
		}

		[[nodiscard]] const PulseForgeEditor::EditorDocumentIdentity* GetFocusedDocumentIdentity() const noexcept
		{
			const auto* Host = m_Documents.GetFocusedHost();
			return Host && Host->ActiveDocument ? &*Host->ActiveDocument : nullptr;
		}

		[[nodiscard]] PulseForgeEditor::EditorDocumentSession* GetFocusedRegisteredDocument() noexcept
		{
			const auto* Identity = GetFocusedDocumentIdentity();
			if (!Identity || !m_DocumentEditors.IsRegistered(Identity->Kind))
				return nullptr;
			auto* Session = m_Documents.Find(*Identity);
			return Session && Session->EditorState ? Session : nullptr;
		}

		[[nodiscard]] bool IsFocusedMaterialDocument() const noexcept
		{
			return GetFocusedMaterialAsset().has_value();
		}

		[[nodiscard]] std::optional<PulseForge::AssetID> GetFocusedMaterialAsset() const noexcept
		{
			if (IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails))
				return m_MaterialDetailsAsset;
			return std::nullopt;
		}

		void FocusDocumentHostIfChanged(std::string_view HostID)
		{
			const auto* Current = m_Documents.GetFocusedHost();
			if ((!Current || Current->ID != HostID) && m_Documents.FocusHost(HostID))
				MarkDocumentWorkspaceDirty();
		}

		bool SaveFocusedDocument()
		{
			if (IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails))
				return m_MaterialDetailsAsset && SaveMaterialDraft(*m_MaterialDetailsAsset);
			if (auto* Session = GetFocusedRegisteredDocument())
				return m_DocumentEditors.Save(*Session);
			if (const auto Material = GetFocusedMaterialAsset())
				return SaveMaterialDraft(*Material);
			const auto* Identity = GetFocusedDocumentIdentity();
			const PulseForge::UUID CurrentProject = m_Project
				? m_Project->GetDescription().ID : PulseForge::UUID{};
			switch (PulseForgeEditor::ResolveEditorDocumentCommandTarget(Identity, CurrentProject))
			{
				case PulseForgeEditor::EditorDocumentCommandTarget::Material:
					return SaveMaterialDraft(Identity->Asset);
				case PulseForgeEditor::EditorDocumentCommandTarget::Unsupported:
				{
					SetWarning("Saving this document type is not supported yet.");
					return false;
				}
				case PulseForgeEditor::EditorDocumentCommandTarget::Level:
					break;
			}
			return SaveScene();
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
			return CanRouteEditorInputShortcut(m_Scene != nullptr);
		}

		[[nodiscard]] bool CanRouteEditorInputShortcut(bool CommandTargetAvailable) const
		{
			const ImGuiIO& IO = ImGui::GetIO();
			return PulseForgeEditor::CanRouteEditorShortcut({
				.SceneAvailable = CommandTargetAvailable,
				.RuntimeActive = m_SceneRuntime != nullptr,
				.CameraNavigationActive = m_EditorCameraNavigationActive,
				.GizmoInteractionActive = m_GizmoDrag.has_value(),
				.TextInputActive = IO.WantTextInput,
				.ImGuiItemActive = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui && GImGui->NavLayer == ImGuiNavLayer_Menu,
				.WindowNavigationActive = GImGui && GImGui->NavWindowingTarget != nullptr
			});
		}

		void HandleSceneEditingShortcuts()
		{
			const ImGuiIO& IO = ImGui::GetIO();
			const bool CanFocusSearch = m_Project && CanRouteEditorInputShortcut(true) &&
				!IO.KeyAlt && !IO.KeySuper;
			if (CanFocusSearch && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
			{
				FocusActivePanelSearch();
				return;
			}
			if (GetFocusedRegisteredDocument())
			{
				if (!IO.WantTextInput && !ImGui::IsAnyItemActive() &&
					!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
					GImGui->NavLayer != ImGuiNavLayer_Menu && GImGui->NavWindowingTarget == nullptr &&
					IO.KeyCtrl && !IO.KeyAlt && !IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false))
				{
					if (IO.KeyShift)
						SetWarning("Save As is not available for this document type.");
					else
						(void)SaveFocusedDocument();
				}
				return;
			}
			if (IsFocusedMaterialDocument())
			{
				if (!IO.WantTextInput && !ImGui::IsAnyItemActive() &&
					!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
					GImGui->NavLayer != ImGuiNavLayer_Menu && GImGui->NavWindowingTarget == nullptr &&
					IO.KeyCtrl && !IO.KeyAlt && !IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false))
				{
					if (IO.KeyShift)
						SetWarning("Save As is available for scenes; material documents use Save Material.");
					else
						(void)SaveFocusedDocument();
				}
				return;
			}
			if (IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails))
				return;
			const PulseForge::UUID CurrentProject = m_Project
				? m_Project->GetDescription().ID : PulseForge::UUID{};
			if (PulseForgeEditor::ResolveEditorDocumentCommandTarget(
				GetFocusedDocumentIdentity(), CurrentProject) ==
				PulseForgeEditor::EditorDocumentCommandTarget::Unsupported)
				return;
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
						if (m_ContentBrowser.SelectedAsset())
							DuplicateAsset(*m_ContentBrowser.SelectedAsset());
					}
					else
						DuplicateSelectedEntity();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
				{
					if (Shift)
						CreateScene(true);
					else
						(void)SaveFocusedDocument();
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
						if (m_ContentBrowser.SelectedAsset())
							RequestDeleteAsset(*m_ContentBrowser.SelectedAsset());
					}
					else
						DeleteSelectedEntity();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && ContentBrowser && m_ContentBrowser.SelectedAsset())
					MoveAsset(*m_ContentBrowser.SelectedAsset());
				else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && SelectedEntity() && !ContentBrowser)
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

		bool ClearCurrentProjectDocumentState(std::function<void()> OnDocumentsClosed = {})
		{
			if (m_Project)
			{
				SaveDocumentWorkspaceNow();
				std::vector<PulseForgeEditor::EditorDocumentIdentity> ProjectDocuments;
				for (const auto& [Identity, Session] : m_Documents.GetSessions())
					if (Identity.Project == m_Project->GetDescription().ID &&
						Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level)
						ProjectDocuments.push_back(Identity);
				if (!CloseDocumentBatchClean(ProjectDocuments))
				{
					m_PendingDocumentClose.reset();
					m_PendingDocumentHostClose.reset();
					m_PendingDocumentCloseBatch = std::move(ProjectDocuments);
					m_DocumentCloseDialogHostID = std::string(PulseForgeEditor::EditorDocumentManager::MainHostID);
					m_PendingDocumentCloseAction = std::move(OnDocumentsClosed);
					m_OpenDocumentCloseDialog = true;
					return false;
				}
				(void)m_Documents.ClearProject(m_Project->GetDescription().ID);
				ClearMaterialDraftsForProject();
			}
			m_LevelDocumentIdentity.reset();
			m_MaterialDetailsAsset.reset();
			m_InitialMaterialDetailsDockPending = false;
			m_ForceMaterialDetailsDock = false;
			m_FocusMaterialDetailsNextFrame = false;
			m_DocumentWorkspacePath.clear();
			m_DocumentWorkspaceLastContents.clear();
			m_DocumentWorkspaceImmediateDirty = false;
			m_DocumentWorkspaceBoundsDirty = false;
			m_RestoredHostBoundsPending.clear();
			m_AssetPreviewProjectPending = true;
			return true;
		}

		void RegisterLevelDocument()
		{
			if (!m_Project)
				return;
			m_LevelDocumentIdentity = PulseForgeEditor::EditorDocumentIdentity{
				m_Project->GetDescription().ID, PulseForge::UUID{}, PulseForgeEditor::EditorDocumentKind::Level
			};
			auto LevelDocument = m_Documents.Open(*m_LevelDocumentIdentity, {}, "Level Editor",
				PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip);
			if (!LevelDocument)
				SetWarning("The Level Editor document session could not be registered: " + LevelDocument.error());
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
				if (!ClearCurrentProjectDocumentState([this, ProjectFile] { (void)OpenProjectAt(ProjectFile); }))
					return;
				StopRuntime();
				ResetViewportSceneRenderer();
				m_Project.emplace(std::move(*Created));
				RegisterLevelDocument();
				ClearScene();
				ResetContentBrowserNavigation();
				UpdateAssetList();
				RestoreDocumentWorkspace();
				SetError("Project was created, but its initial scene could not be created: " + MainScene.error().Message);
				return;
			}

			const PulseForge::AssetRecord MainSceneRecord = MainScene
				? *MainScene
				: *MainScene.error().CommittedAsset;
			if (!ClearCurrentProjectDocumentState([this, ProjectFile] { (void)OpenProjectAt(ProjectFile); }))
				return;
			StopRuntime();
			ResetViewportSceneRenderer();
			m_Project.emplace(std::move(*Created));
			RegisterLevelDocument();
			ResetContentBrowserNavigation();
			m_Scene = std::move(NewScene);
			m_SceneAsset = MainSceneRecord.ID;
			SelectSingleEntity(std::nullopt);
			m_ContentBrowser.SelectedAsset() = MainSceneRecord.ID;
			m_AssetWorkflow.DismissDelete();
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
			RestoreDocumentWorkspace();

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
			if (!ClearCurrentProjectDocumentState([this, ProjectFile] { (void)OpenProjectAt(ProjectFile); }))
				return std::unexpected("The current project has a document session that could not close");
			StopRuntime();
			ResetViewportSceneRenderer();
			m_Project.emplace(std::move(Opened->Project));
			RegisterLevelDocument();
			ResetContentBrowserNavigation();
			m_Scene = std::move(Opened->StartupScene);
			m_SceneAsset = Opened->StartupSceneAsset;
			SelectSingleEntity(std::nullopt);
			m_ContentBrowser.SelectedAsset() = Opened->StartupSceneAsset;
			m_AssetWorkflow.DismissDelete();
			ResetSceneHistory();
			UpdateAssetList();
			RestoreDocumentWorkspace();
			InitializeDocumentHostTestFixture();
			if (Opened->StartupSceneWarning.empty())
				SetStatus("Opened project " + ProjectName + ".");
			else
				SetWarning(std::move(Opened->StartupSceneWarning));
			return {};
		}

		[[nodiscard]] static constexpr std::array<std::string_view, 2> GetDocumentHostTestFixtureAssetIDs() noexcept
		{
			return {
				"d3110000-0000-4000-8000-000000000001",
				"d3110000-0000-4000-8000-000000000002"
			};
		}

		[[nodiscard]] PulseForgeEditor::EditorDocumentEditorContext MakeDocumentEditorContext(
			const PulseForgeEditor::EditorDocumentIdentity& Identity,
			std::filesystem::path ProjectRelativePath,
			std::string DisplayName) const
		{
			PulseForgeEditor::EditorDocumentEditorContext Context;
			Context.Identity = Identity;
			Context.ProjectRelativePath = std::move(ProjectRelativePath);
			Context.DisplayName = std::move(DisplayName);
			if (m_Project && m_Project->GetDescription().ID == Identity.Project)
			{
				Context.ProjectRoot = m_Project->GetRootPath();
				Context.Registry = &m_Project->GetAssetRegistry();
			}
			return Context;
		}

		void RegisterDocumentHostTestEditor()
		{
			PulseForgeEditor::EditorDocumentEditorAdapter Adapter;
			Adapter.Kind = PulseForgeEditor::EditorDocumentKind::Texture;
			Adapter.IsEligible = [this](const auto& Context)
			{
				return IsDocumentHostTestFixtureIdentity(Context.Identity);
			};
			Adapter.LoadSession = [this](const auto& Context)
				-> std::expected<std::unique_ptr<PulseForgeEditor::EditorDocumentEditorState>, std::string>
			{
				if (!IsDocumentHostTestFixtureIdentity(Context.Identity))
					return std::unexpected("The identity is not a document-host test fixture");
				auto State = std::make_unique<DocumentHostTestState>();
				if (const auto Saved = m_DocumentHostTestSavedValues.find(Context.Identity.Asset);
					Saved != m_DocumentHostTestSavedValues.end())
					State->Value = Saved->second;
				return std::unique_ptr<PulseForgeEditor::EditorDocumentEditorState>(std::move(State));
			};
			Adapter.DrawWorkspace = [this](auto& Session)
			{
				auto* State = dynamic_cast<DocumentHostTestState*>(Session.EditorState.get());
				if (!State)
					return;
				(void)m_EditorStyle.SectionHeader(PulseForgeEditor::EditorIcon::File,
					"Registered Document Workspace Fixture");
				m_EditorStyle.TextMuted("This independent workspace is not part of the Level Editor panels.");
				ImGui::Text("Asset %s", Session.Identity.Asset.ToString().c_str());
				int Candidate = State->Value;
				if (ImGui::InputInt("Editable value", &Candidate))
				{
					State->Undo.push_back(State->Value);
					State->Redo.clear();
					State->Value = Candidate;
					(void)m_Documents.MarkEdited(Session.Identity);
				}
				if (ImGui::Button("Save fixture"))
					(void)m_DocumentEditors.Save(Session);
				ImGui::SameLine();
				if (ImGui::Button("Revert fixture"))
					(void)m_DocumentEditors.Discard(Session);
				ImGui::SameLine();
				if (ImGui::Button("Fail next save"))
					State->FailNextSave = true;
				ImGui::SameLine();
				if (ImGui::Button(State->FailClose ? "Allow close" : "Fail close"))
					State->FailClose = !State->FailClose;
				ImGui::Text("Undo %zu  |  Redo %zu", State->Undo.size(), State->Redo.size());
				ImGui::TextUnformatted(Session.IsDirty() ? "Unsaved fixture changes" : "Fixture saved");
			};
			Adapter.Save = [this](auto& Session, uint64_t)
			{
				auto* State = dynamic_cast<DocumentHostTestState*>(Session.EditorState.get());
				if (!State || State->FailNextSave)
				{
					if (State)
						State->FailNextSave = false;
					SetError("The document-host fixture save was rejected.");
					return false;
				}
				m_DocumentHostTestSavedValues[Session.Identity.Asset] = State->Value;
				return true;
			};
			Adapter.Discard = [this](auto& Session)
			{
				auto* State = dynamic_cast<DocumentHostTestState*>(Session.EditorState.get());
				if (!State)
					return false;
				if (const auto Saved = m_DocumentHostTestSavedValues.find(Session.Identity.Asset);
					Saved != m_DocumentHostTestSavedValues.end())
					State->Value = Saved->second;
				else
					State->Value = 0;
				State->Undo.clear();
				State->Redo.clear();
				return true;
			};
			Adapter.FocusedCommand = [](auto& Session, PulseForgeEditor::EditorDocumentCommand Command)
			{
				auto* State = dynamic_cast<DocumentHostTestState*>(Session.EditorState.get());
				if (!State)
					return false;
				auto& Source = Command == PulseForgeEditor::EditorDocumentCommand::Undo
					? State->Undo : State->Redo;
				auto& Destination = Command == PulseForgeEditor::EditorDocumentCommand::Undo
					? State->Redo : State->Undo;
				if (Source.empty())
					return false;
				Destination.push_back(State->Value);
				State->Value = Source.back();
				Source.pop_back();
				++Session.Revision;
				return true;
			};
			Adapter.AssetLocationChanged = [](auto&, const auto&, std::string_view) { return true; };
			Adapter.PrepareClose = [](const auto& Session) -> std::expected<void, std::string>
			{
				const auto* State = dynamic_cast<const DocumentHostTestState*>(Session.EditorState.get());
				if (State && State->FailClose)
					return std::unexpected("The Debug fixture is configured to reject close.");
				return {};
			};
			Adapter.Close = [](auto&) {};
			if (auto Result = m_DocumentEditors.Register(std::move(Adapter)); !Result)
				PF_ERROR("Could not register the debug document-host fixture editor: {}", Result.error());
		}

		[[nodiscard]] bool IsDocumentHostTestFixtureIdentity(
			const PulseForgeEditor::EditorDocumentIdentity& Identity) const
		{
			#ifndef _DEBUG
			(void)Identity;
			return false;
			#else
			if (!m_DocumentHostTestFixture || !m_Project ||
				Identity.Project != m_Project->GetDescription().ID ||
				Identity.Kind != PulseForgeEditor::EditorDocumentKind::Texture)
				return false;

			for (const std::string_view FixtureID : GetDocumentHostTestFixtureAssetIDs())
				if (const auto Parsed = PulseForge::UUID::Parse(FixtureID); Parsed && *Parsed == Identity.Asset)
					return true;
			return false;
			#endif
		}

		void InitializeDocumentHostTestFixture()
		{
			#ifndef _DEBUG
			return;
			#else
			if (!m_DocumentHostTestFixture || !m_Project)
				return;

			// These synthetic Texture-kind identities exercise registered document dispatch in Debug.
			const auto FixtureAssetIDs = GetDocumentHostTestFixtureAssetIDs();
			const auto Preference = m_Documents.GetOpeningPreference();
			std::unordered_set<std::string> FixtureHosts;
			m_DocumentHostFixtureSessions.clear();
			size_t OpenedCount = 0;
			size_t RestoredCount = 0;
			for (size_t Index = 0; Index < FixtureAssetIDs.size(); ++Index)
			{
				const auto AssetID = PulseForge::UUID::Parse(FixtureAssetIDs[Index]);
				if (!AssetID)
					continue;
				const PulseForgeEditor::EditorDocumentIdentity Identity{
					m_Project->GetDescription().ID, *AssetID, PulseForgeEditor::EditorDocumentKind::Texture };
				const std::string DisplayName = "Host Test Fixture " + std::to_string(Index + 1);
				std::string HostID;
				if (const auto* Existing = m_Documents.Find(Identity))
				{
					HostID = Existing->HostID;
					++RestoredCount;
				}
				else
				{
					auto Opened = m_Documents.Open(Identity, {}, DisplayName, Preference);
					if (!Opened)
					{
						PF_WARN("Could not open development document-host fixture {}: {}", DisplayName, Opened.error());
						continue;
					}
					HostID = Opened->HostID;
				}
				auto* Session = m_Documents.Find(Identity);
				const auto Context = MakeDocumentEditorContext(Identity, {}, DisplayName);
				if (!Session || !m_DocumentEditors.Load(*Session, Context))
				{
					PF_WARN("Could not load development document-host fixture {}", DisplayName);
					(void)m_Documents.Close(Identity, PulseForgeEditor::EditorDocumentCloseChoice::Discard, {}, {});
					continue;
				}
				++OpenedCount;
				FixtureHosts.insert(HostID);
				m_DocumentHostFixtureSessions.emplace_back(Identity, HostID);
				if ((m_DocumentHostResetLayoutTest || (m_DocumentHostPresentationFailureTest && Index == 0)) &&
					!m_Documents.MarkEdited(Identity))
					PF_WARN("Could not mark development fixture {} dirty for its document-host test.", DisplayName);
			}
			if (m_DocumentHostTabReorderTest)
			{
				bool Prepared = m_DocumentHostFixtureSessions.size() >= 2;
				for (const auto& [Identity, OriginalHostID] : m_DocumentHostFixtureSessions)
				{
					(void)OriginalHostID;
					const auto* Session = m_Documents.Find(Identity);
					const auto* MainHost = m_Documents.FindHost(
						PulseForgeEditor::EditorDocumentManager::MainHostID);
					if (!Session || !MainHost)
					{
						Prepared = false;
						break;
					}
					const std::string SourceHostID = Session->HostID;
					const auto Transfer = m_Documents.Transfer(
						Identity,
						PulseForgeEditor::EditorDocumentManager::MainHostID,
						MainHost->Documents.size());
					if (!Transfer)
					{
						Prepared = false;
						PF_ERROR("Document tab reorder fixture could not place its synthetic session in the main strip: {}",
							Transfer.error());
						break;
					}
					if (Transfer->EmptiedSourceHost)
					{
						m_RestoredHostBoundsPending.erase(*Transfer->EmptiedSourceHost);
						(void)m_Documents.RemoveEmptyNativeHost(SourceHostID);
					}
				}
				if (Prepared)
				{
					m_DocumentHostTabReorderTestFirst = m_DocumentHostFixtureSessions[0].first;
					m_DocumentHostTabReorderTestSecond = m_DocumentHostFixtureSessions[1].first;
					Prepared = m_Documents.Activate(m_DocumentHostTabReorderTestFirst);
				}
				if (Prepared)
				{
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::WaitingForTabs;
					PF_INFO("Document tab reorder fixture placed two synthetic sessions in the main strip.");
				}
				else
				{
					m_DocumentHostTabReorderTest = false;
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
					PF_ERROR("Document tab reorder fixture could not prepare two sessions in the main strip.");
				}
			}
			if (m_DocumentWorkspaceSwitchTest)
				PrepareDocumentWorkspaceSwitchTest();
			MarkDocumentWorkspaceDirty();
			PF_WARN(
				"Development document-host fixture enabled with opening preference {}: {} sessions restored from saved "
				"host state, {} total across {} hosts; "
				"fixture sessions use a Debug-only registered workspace adapter.",
				static_cast<uint32_t>(Preference), RestoredCount, OpenedCount, FixtureHosts.size());
			if (m_DocumentHostTabTearOffTest)
			{
				if (m_DocumentHostFixtureSessions.empty())
				{
					m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
					PF_ERROR("Document tab tear-off fixture could not find a synthetic session.");
				}
				else
				{
					m_DocumentHostTabTearOffTestIdentity = m_DocumentHostFixtureSessions.front().first;
					if (const auto* Session = m_Documents.Find(m_DocumentHostTabTearOffTestIdentity);
						Session && Session->HostID != PulseForgeEditor::EditorDocumentManager::MainHostID)
					{
						const auto* MainHost = m_Documents.FindHost(
							PulseForgeEditor::EditorDocumentManager::MainHostID);
						const size_t DestinationIndex = MainHost ? MainHost->Documents.size() : 0;
						const std::string SourceHostID = Session->HostID;
						const auto Transfer = m_Documents.Transfer(
							m_DocumentHostTabTearOffTestIdentity,
							PulseForgeEditor::EditorDocumentManager::MainHostID,
							DestinationIndex);
						if (!Transfer)
						{
							m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
							PF_ERROR("Document tab tear-off fixture could not prepare its source tab: {}", Transfer.error());
						}
						else
						{
							if (Transfer->EmptiedSourceHost)
								(void)m_Documents.RemoveEmptyNativeHost(SourceHostID);
							m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::WaitingForTab;
						}
					}
					else
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::WaitingForTab;
				}
			}
			if (m_DocumentHostTabReattachTest)
			{
				if (m_DocumentHostFixtureSessions.size() < 2)
				{
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
					m_DocumentHostTabReattachTest = false;
					PF_ERROR("Document tab reattachment UI fixture failed: it requires two synthetic sessions.");
				}
				else
				{
					if (m_DocumentHostFixtureSessions.size() >= 2)
					{
						const auto TargetIdentity = m_DocumentHostFixtureSessions[1].first;
						const auto* TargetSession = m_Documents.Find(TargetIdentity);
						const auto* TargetHost = TargetSession ? m_Documents.FindHost(TargetSession->HostID) : nullptr;
						const auto* SourceSession = m_Documents.Find(m_DocumentHostFixtureSessions.front().first);
						if (!TargetSession || !TargetHost || !SourceSession)
						{
							m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
							m_DocumentHostTabReattachTest = false;
							PF_ERROR(
								"Document tab reattachment UI fixture failed: it could not resolve two synthetic sessions and "
								"their hosts.");
						}
						else if (TargetHost->Role != PulseForgeEditor::EditorDocumentHostRole::Native ||
							TargetHost->ID == SourceSession->HostID)
						{
							auto NativeHost = m_Documents.CreateNativeHost(TargetIdentity.Kind);
							if (!NativeHost)
							{
								m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
								m_DocumentHostTabReattachTest = false;
								PF_ERROR(
									"Document tab reattachment UI fixture failed to create a separate grouping host: {}",
									NativeHost.error());
							}
							else
							{
								const std::string PreviousHostID = TargetSession->HostID;
								const auto Transfer = m_Documents.Transfer(TargetIdentity, *NativeHost, 0);
								if (!Transfer)
								{
									(void)m_Documents.RemoveEmptyNativeHost(*NativeHost);
									m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
									m_DocumentHostTabReattachTest = false;
									PF_ERROR(
										"Document tab reattachment UI fixture failed to prepare a separate grouping host: {}",
										Transfer.error());
								}
								else
								{
									if (Transfer->EmptiedSourceHost)
									{
										m_RestoredHostBoundsPending.erase(*Transfer->EmptiedSourceHost);
										(void)m_Documents.RemoveEmptyNativeHost(PreviousHostID);
									}
									m_DocumentHostFixtureSessions[1].second = *NativeHost;
									m_DocumentHostFocusRequests.insert(*NativeHost);
									MarkDocumentWorkspaceDirty();
								}
							}
						}
					}
					if (!m_DocumentHostTabReattachTest)
						return;
					m_DocumentHostTabReattachTestIdentity = m_DocumentHostFixtureSessions.front().first;
					const auto* Session = m_Documents.Find(m_DocumentHostTabReattachTestIdentity);
					const auto* Host = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
					if (!Session || !Host)
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
						m_DocumentHostTabReattachTest = false;
						PF_ERROR(
							"Document tab reattachment UI fixture failed to resolve its synthetic session host.");
					}
					else if (Host->Role == PulseForgeEditor::EditorDocumentHostRole::Main)
					{
						auto NativeHost = m_Documents.CreateNativeHost(m_DocumentHostTabReattachTestIdentity.Kind);
						if (!NativeHost)
						{
							m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
							m_DocumentHostTabReattachTest = false;
							PF_ERROR("Document tab reattachment UI fixture failed to create a native source host: {}",
								NativeHost.error());
						}
						else
						{
							m_DocumentHostTabReattachTestSourceHostID = *NativeHost;
							m_DocumentHostTabReattachExpectSourceHostRetired = true;
							const auto Transfer = m_Documents.Transfer(
								m_DocumentHostTabReattachTestIdentity, *NativeHost, 0);
							if (!Transfer)
							{
								(void)m_Documents.RemoveEmptyNativeHost(*NativeHost);
								m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
								m_DocumentHostTabReattachTest = false;
								PF_ERROR(
									"Document tab reattachment UI fixture failed to prepare its native source host: {}",
									Transfer.error());
							}
							else
							{
								MarkDocumentWorkspaceDirty();
								m_DocumentHostFocusRequests.insert(*NativeHost);
								m_DocumentHostFixtureSessions.front().second = *NativeHost;
								m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::WaitingForTab;
							}
						}
					}
					else if (Host->Role == PulseForgeEditor::EditorDocumentHostRole::Native)
					{
						m_DocumentHostTabReattachTestSourceHostID = Host->ID;
						m_DocumentHostTabReattachExpectSourceHostRetired = Host->Documents.size() == 1;
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::WaitingForTab;
					}
					else
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
						m_DocumentHostTabReattachTest = false;
						PF_ERROR("Document tab reattachment UI fixture failed: the source has an unsupported host role.");
					}
				}
			}
			if (m_DocumentHostResetLayoutTest && OpenedCount == FixtureAssetIDs.size())
			{
				m_DocumentHostResetLayoutExpectedHostCount = m_Documents.GetHosts().size();
				m_DocumentHostResetLayoutTestPending = true;
				m_Layout.RequestReset();
			}
			#endif
		}

		void PrepareDocumentWorkspaceSwitchTest()
		{
#ifndef _DEBUG
			m_DocumentWorkspaceSwitchTest = false;
			m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::Failed;
#else
			const auto Fail = [this](std::string_view Message)
			{
				m_DocumentWorkspaceSwitchTest = false;
				m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::Failed;
				PF_ERROR("Document workspace switch fixture failed during setup: {}", Message);
			};
			if (m_DocumentHostFixtureSessions.size() < 2 || !m_Project)
			{
				Fail("two registered fixture sessions and an open project are required");
				return;
			}

			for (const auto& [Identity, OriginalHostID] : m_DocumentHostFixtureSessions)
			{
				(void)OriginalHostID;
				const auto* Session = m_Documents.Find(Identity);
				const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				if (!Session || !MainHost)
				{
					Fail("a fixture session or the main document host is unavailable");
					return;
				}
				const std::string SourceHostID = Session->HostID;
				const auto Transfer = m_Documents.Transfer(
					Identity, PulseForgeEditor::EditorDocumentManager::MainHostID, MainHost->Documents.size());
				if (!Transfer)
				{
					Fail(Transfer.error());
					return;
				}
				if (Transfer->EmptiedSourceHost)
				{
					m_RestoredHostBoundsPending.erase(*Transfer->EmptiedSourceHost);
					(void)m_Documents.RemoveEmptyNativeHost(SourceHostID);
				}
			}

			m_DocumentWorkspaceSwitchLevelIdentity = {};
			for (const auto& [Identity, Session] : m_Documents.GetSessions())
				if (Identity.Project == m_Project->GetDescription().ID &&
					Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
				{
					m_DocumentWorkspaceSwitchLevelIdentity = Identity;
					break;
				}
			if (m_DocumentWorkspaceSwitchLevelIdentity.Project.IsNil())
			{
				Fail("the Level Editor session is missing");
				return;
			}

			m_DocumentWorkspaceSwitchFirstIdentity = m_DocumentHostFixtureSessions[0].first;
			m_DocumentWorkspaceSwitchSecondIdentity = m_DocumentHostFixtureSessions[1].first;
			const auto InitializeModel = [this](const auto& Identity, int Value)
			{
				auto* Session = m_Documents.Find(Identity);
				auto* State = Session ? dynamic_cast<DocumentHostTestState*>(Session->EditorState.get()) : nullptr;
				if (!Session || !State)
					return false;
				State->Value = Value;
				State->Undo.push_back(0);
				if (!m_Documents.MarkEdited(Identity) || !m_DocumentEditors.Save(*Session))
					return false;
				return true;
			};
			if (!InitializeModel(m_DocumentWorkspaceSwitchFirstIdentity, 41) ||
				!InitializeModel(m_DocumentWorkspaceSwitchSecondIdentity, 82))
			{
				Fail("the fixture editor could not establish independent saved models and history");
				return;
			}

			const auto* FirstSession = m_Documents.Find(m_DocumentWorkspaceSwitchFirstIdentity);
			const auto* SecondSession = m_Documents.Find(m_DocumentWorkspaceSwitchSecondIdentity);
			m_DocumentWorkspaceSwitchFirstSession = FirstSession;
			m_DocumentWorkspaceSwitchFirstState = FirstSession ? FirstSession->EditorState.get() : nullptr;
			m_DocumentWorkspaceSwitchSecondState = SecondSession ? SecondSession->EditorState.get() : nullptr;
			if (!FirstSession || !SecondSession ||
				!m_Documents.Activate(m_DocumentWorkspaceSwitchFirstIdentity))
			{
				Fail("the first fixture could not be activated in the main document strip");
				return;
			}

			const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			if (!MainViewport)
			{
				Fail("the main ImGui viewport is unavailable");
				return;
			}
			char HostWindowName[32]{};
			ImFormatString(HostWindowName, IM_ARRAYSIZE(HostWindowName), "WindowOverViewport_%08X", MainViewport->ID);
			m_DocumentWorkspaceSwitchMainDockspaceID = ImHashStr("DockSpace", 0, ImHashStr(HostWindowName));
			m_DocumentWorkspaceSwitchScene = m_Scene.get();
			m_DocumentWorkspaceSwitchRuntimeScene = m_RuntimeScene.get();
			m_DocumentWorkspaceSwitchSelectedEntity = SelectedEntity();
			m_DocumentWorkspaceSwitchCameraPosition = m_EditorCamera.GetPosition();
			m_DocumentWorkspaceSwitchSceneViewportVisible =
				m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport);
			m_DocumentWorkspaceSwitchHierarchyVisible =
				m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy);
			m_DocumentWorkspaceSwitchInspectorVisible =
				m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Inspector);
			m_DocumentWorkspaceSwitchContentBrowserVisible =
				m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::ContentBrowser);
			m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForFirstFixture;
			PF_INFO("Document workspace switch fixture opened two independent registered sessions in the main document strip.");
#endif
		}

		void AdvanceDocumentWorkspaceSwitchTest()
		{
			if (!m_DocumentWorkspaceSwitchTest ||
				m_DocumentWorkspaceSwitchTestPhase == DocumentWorkspaceSwitchTestPhase::Disabled)
				return;
#ifndef _DEBUG
			m_DocumentWorkspaceSwitchTest = false;
#else
			const auto Fail = [this](std::string_view Message)
			{
				m_DocumentWorkspaceSwitchTest = false;
				m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::Failed;
				PF_ERROR("Document workspace switch fixture failed: {}", Message);
			};
			const auto MainHostActive = [this]() -> std::optional<PulseForgeEditor::EditorDocumentIdentity>
			{
				const auto* Host = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				return Host ? Host->ActiveDocument : std::nullopt;
			};
			const auto HasWorkspaceDrawn = [this](const auto& Identity, std::string_view HostID)
			{
				return m_WorkspacePresentation.WasWorkspaceDrawn(Identity, HostID);
			};
			const auto ViewportInteractionReleased = [this]
			{
				return !m_EditorCameraNavigationActive && !m_ViewportKeyboardFocused &&
					!m_ViewportMouseCapture.OwnsMouse() && m_ViewportCameraGesture == ViewportCameraGesture::None &&
					!m_GizmoDrag.has_value();
			};
			const auto MainLayoutRetained = [this]
			{
				return m_DocumentWorkspaceSwitchMainDockspaceID != 0 &&
					ImGui::DockBuilderGetNode(m_DocumentWorkspaceSwitchMainDockspaceID) != nullptr;
			};
			const auto FixtureStatePreserved = [this](const auto& Identity, const auto* ExpectedState, int ExpectedValue)
			{
				const auto* Session = m_Documents.Find(Identity);
				const auto* State = Session
					? dynamic_cast<const DocumentHostTestState*>(Session->EditorState.get()) : nullptr;
				return Session && ExpectedState && Session->EditorState.get() == ExpectedState && State &&
					State->Value == ExpectedValue && State->Undo.size() == 1 && State->Undo.front() == 0 &&
					State->Redo.empty() && !Session->IsDirty();
			};
			const auto Activate = [this, &Fail](const auto& Identity, std::string_view Name)
			{
				if (!m_Documents.Activate(Identity))
				{
					Fail(Name);
					return false;
				}
			return true;
			};

			switch (m_DocumentWorkspaceSwitchTestPhase)
			{
				case DocumentWorkspaceSwitchTestPhase::WaitingForFirstFixture:
					if (MainHostActive() == m_DocumentWorkspaceSwitchFirstIdentity &&
						HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity,
							PulseForgeEditor::EditorDocumentManager::MainHostID))
					{
						if (!MainLayoutRetained())
						{
							Fail("the Level Editor dockspace was lost while the fixture workspace was active");
						break;
						}
						if (!Activate(m_DocumentWorkspaceSwitchLevelIdentity, "the Level Editor could not be activated"))
							break;
						m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForLevel;
					}
					break;
				case DocumentWorkspaceSwitchTestPhase::WaitingForLevel:
					if (MainHostActive() == m_DocumentWorkspaceSwitchLevelIdentity &&
						m_WorkspacePresentation.IsLevelWorkspaceActive())
					{
						if (!ViewportInteractionReleased() || !MainLayoutRetained())
						{
							Fail("viewport interaction or the Level Editor layout was not preserved on return");
						break;
						}
						if (!Activate(m_DocumentWorkspaceSwitchFirstIdentity, "the first fixture could not be reactivated"))
							break;
						m_DocumentWorkspaceSwitchTestPhase =
							DocumentWorkspaceSwitchTestPhase::WaitingForFirstFixtureAgain;
					}
					break;
				case DocumentWorkspaceSwitchTestPhase::WaitingForFirstFixtureAgain:
					if (MainHostActive() == m_DocumentWorkspaceSwitchFirstIdentity &&
						HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity,
							PulseForgeEditor::EditorDocumentManager::MainHostID))
					{
						if (!FixtureStatePreserved(m_DocumentWorkspaceSwitchFirstIdentity,
							m_DocumentWorkspaceSwitchFirstState, 41) || !ViewportInteractionReleased())
						{
							Fail("the first session's identity, value, history, or input isolation changed");
						break;
						}
						if (!Activate(m_DocumentWorkspaceSwitchSecondIdentity, "the second fixture could not be activated"))
							break;
						m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForSecondFixture;
					}
					break;
				case DocumentWorkspaceSwitchTestPhase::WaitingForSecondFixture:
					if (MainHostActive() == m_DocumentWorkspaceSwitchSecondIdentity &&
						HasWorkspaceDrawn(m_DocumentWorkspaceSwitchSecondIdentity,
							PulseForgeEditor::EditorDocumentManager::MainHostID))
					{
						if (!FixtureStatePreserved(m_DocumentWorkspaceSwitchSecondIdentity,
							m_DocumentWorkspaceSwitchSecondState, 82) || !ViewportInteractionReleased())
						{
							Fail("the second session did not retain independent state and history");
						break;
						}
						if (!Activate(m_DocumentWorkspaceSwitchFirstIdentity, "the first fixture could not be activated before transfer"))
							break;
						m_DocumentWorkspaceSwitchTestPhase =
							DocumentWorkspaceSwitchTestPhase::WaitingForNativeTransfer;
					}
					break;
				case DocumentWorkspaceSwitchTestPhase::WaitingForNativeTransfer:
					if (MainHostActive() == m_DocumentWorkspaceSwitchFirstIdentity &&
						HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity,
							PulseForgeEditor::EditorDocumentManager::MainHostID))
					{
						const auto NativeHost = m_Documents.CreateNativeHost(
							PulseForgeEditor::EditorDocumentKind::Texture);
						if (!NativeHost)
						{
							Fail(NativeHost.error());
						break;
						}
						const auto Transfer = m_Documents.Transfer(m_DocumentWorkspaceSwitchFirstIdentity, *NativeHost, 0);
						if (!Transfer || !m_Documents.Activate(m_DocumentWorkspaceSwitchLevelIdentity))
						{
							(void)m_Documents.RemoveEmptyNativeHost(*NativeHost);
							Fail("the first fixture could not transfer to a native host while preserving the Level Editor");
							break;
						}
						m_DocumentWorkspaceSwitchNativeHostID = *NativeHost;
						MarkDocumentWorkspaceDirty(true);
						m_DocumentWorkspaceSwitchTestPhase =
							DocumentWorkspaceSwitchTestPhase::WaitingForMainTransfer;
					}
					break;
				case DocumentWorkspaceSwitchTestPhase::WaitingForMainTransfer:
				{
					const auto* Session = m_Documents.Find(m_DocumentWorkspaceSwitchFirstIdentity);
					if (Session && Session->HostID == m_DocumentWorkspaceSwitchNativeHostID &&
						HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity,
							m_DocumentWorkspaceSwitchNativeHostID))
					{
						if (MainHostActive() != m_DocumentWorkspaceSwitchLevelIdentity ||
							!m_WorkspacePresentation.IsLevelWorkspaceActive() || !ViewportInteractionReleased())
						{
							Fail("the Level Editor did not remain available while the native fixture workspace was active");
							break;
						}
						const auto Transfer = m_Documents.Transfer(m_DocumentWorkspaceSwitchFirstIdentity,
							PulseForgeEditor::EditorDocumentManager::MainHostID, 1);
						if (!Transfer || !Transfer->EmptiedSourceHost)
						{
							Fail("the native fixture host did not reattach cleanly to the main document strip");
							break;
						}
						(void)m_Documents.RemoveEmptyNativeHost(*Transfer->EmptiedSourceHost);
						if (!Activate(m_DocumentWorkspaceSwitchFirstIdentity,
							"the reattached fixture could not be activated"))
							break;
						MarkDocumentWorkspaceDirty(true);
						m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForMainTransfer;
					}
					else if (MainHostActive() == m_DocumentWorkspaceSwitchLevelIdentity &&
						m_WorkspacePresentation.IsLevelWorkspaceActive() &&
						!ViewportInteractionReleased())
						Fail("viewport navigation or gizmo state remained captured after the workspace transfer");
					break;
				}
				case DocumentWorkspaceSwitchTestPhase::Complete:
				case DocumentWorkspaceSwitchTestPhase::Failed:
					break;
				case DocumentWorkspaceSwitchTestPhase::Disabled:
					break;
			}
			if (m_DocumentWorkspaceSwitchTestPhase == DocumentWorkspaceSwitchTestPhase::WaitingForMainTransfer &&
				MainHostActive() == m_DocumentWorkspaceSwitchFirstIdentity &&
				m_Documents.Find(m_DocumentWorkspaceSwitchFirstIdentity)->HostID ==
					PulseForgeEditor::EditorDocumentManager::MainHostID &&
				HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity,
					PulseForgeEditor::EditorDocumentManager::MainHostID))
			{
				if (m_Documents.Find(m_DocumentWorkspaceSwitchFirstIdentity) != m_DocumentWorkspaceSwitchFirstSession ||
					!FixtureStatePreserved(m_DocumentWorkspaceSwitchFirstIdentity,
						m_DocumentWorkspaceSwitchFirstState, 41) || !ViewportInteractionReleased() ||
					!MainLayoutRetained() || m_Scene.get() != m_DocumentWorkspaceSwitchScene ||
					m_RuntimeScene.get() != m_DocumentWorkspaceSwitchRuntimeScene ||
					SelectedEntity() != m_DocumentWorkspaceSwitchSelectedEntity ||
					glm::length(m_EditorCamera.GetPosition() - m_DocumentWorkspaceSwitchCameraPosition) > 0.0001f ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport) !=
						m_DocumentWorkspaceSwitchSceneViewportVisible ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy) !=
						m_DocumentWorkspaceSwitchHierarchyVisible ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Inspector) !=
						m_DocumentWorkspaceSwitchInspectorVisible ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::ContentBrowser) !=
						m_DocumentWorkspaceSwitchContentBrowserVisible)
				{
					Fail("document transfer changed workspace state, scene state, camera, selection, or panel visibility");
					return;
				}
				m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::Complete;
				m_DocumentWorkspaceSwitchTest = false;
				PF_INFO("Document workspace switch fixture passed: Level -> fixture 1 -> Level -> fixture 1 -> fixture 2, then native/main transfer preserved session, model history, viewport state, and the hidden Level layout");
			}
#endif
		}

		void QueueDocumentHostTabTearOffTestInput()
		{
			if (!m_DocumentHostTabTearOffTest)
				return;
#ifndef _DEBUG
			PF_ERROR("The document tab tear-off UI fixture is available only in Debug builds.");
			m_DocumentHostTabTearOffTest = false;
#else
			ImGuiIO& IO = ImGui::GetIO();
			if (++m_DocumentHostTabTearOffTestFrameCount > 600)
			{
				m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
				m_DocumentHostTabTearOffTest = false;
				if (m_DocumentHostTabRoundTripTearOffPending)
					PF_ERROR("Document tab native-window round-trip fixture timed out before tearing off the reattached session.");
				else
					PF_ERROR("Document tab tear-off UI fixture timed out before creating its native host.");
				m_DocumentHostTabRoundTripTearOffPending = false;
				return;
			}
			switch (m_DocumentHostTabTearOffTestPhase)
			{
				case DocumentTabTearOffTestPhase::WaitingForTab:
					if (m_DocumentHostTabTearOffTestTabCenterValid)
					{
						m_DocumentHostTabTearOffTestStart = m_DocumentHostTabTearOffTestTabCenter;
						const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
						if (!MainViewport || MainViewport->Size.x <= 0.0f || MainViewport->Size.y <= 0.0f)
						{
							m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
							PF_ERROR("Document tab tear-off UI fixture has no visible main viewport.");
							break;
						}
						m_DocumentHostTabTearOffTestDrop = m_DocumentHostTabTearOffFromNativeHost
							? ImVec2(m_DocumentHostTabTearOffTestStart.x + IO.MouseDragThreshold + 24.0f,
								m_DocumentHostTabTearOffTestStart.y)
							: ImVec2(MainViewport->Pos.x + MainViewport->Size.x + 24.0f,
								MainViewport->Pos.y + MainViewport->Size.y * 0.5f);
						const float DeltaX = m_DocumentHostTabTearOffTestDrop.x - m_DocumentHostTabTearOffTestStart.x;
						const float DeltaY = m_DocumentHostTabTearOffTestDrop.y - m_DocumentHostTabTearOffTestStart.y;
						if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
						{
							m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
							PF_ERROR("Document tab tear-off UI fixture could not place a drop point past the drag threshold.");
							break;
						}
						IO.AddMousePosEvent(m_DocumentHostTabTearOffTestStart.x, m_DocumentHostTabTearOffTestStart.y);
						IO.AddMouseViewportEvent(m_DocumentHostTabTearOffTestTabViewportID);
						IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::PressQueued;
					}
					break;
				case DocumentTabTearOffTestPhase::PressQueued:
					IO.AddMousePosEvent(m_DocumentHostTabTearOffTestDrop.x, m_DocumentHostTabTearOffTestDrop.y);
					if (m_DocumentHostTabTearOffFromNativeHost)
						IO.AddMouseViewportEvent(0);
					m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::DragQueued;
					break;
				case DocumentTabTearOffTestPhase::DragQueued:
					IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
					IO.AddMouseViewportEvent(0);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
					m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::ReleaseQueued;
					break;
				case DocumentTabTearOffTestPhase::ReleaseQueued:
				{
					const auto* Session = m_Documents.Find(m_DocumentHostTabTearOffTestIdentity);
					const auto* Host = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
					const auto* MainHost = m_Documents.FindHost(
						PulseForgeEditor::EditorDocumentManager::MainHostID);
					const bool LevelRetained = MainHost && std::any_of(
						MainHost->Documents.begin(), MainHost->Documents.end(), [](const auto& Identity)
						{
							return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level;
						});
					const std::string_view ExpectedPreviousHostID = m_DocumentHostTabTearOffFromNativeHost
						? std::string_view(m_DocumentHostTabTearOffSourceHostID)
						: PulseForgeEditor::EditorDocumentManager::MainHostID;
					const bool EnteredDifferentNativeHost = Session && Host &&
						Host->Role == PulseForgeEditor::EditorDocumentHostRole::Native &&
						Session->HostID != ExpectedPreviousHostID;
					const auto* PreviousNativeHost = m_DocumentHostTabTearOffFromNativeHost
						? m_Documents.FindHost(m_DocumentHostTabTearOffSourceHostID) : nullptr;
					const bool PreviousNativeGroupIntact = !m_DocumentHostTabTearOffFromNativeHost ||
						(PreviousNativeHost && PreviousNativeHost->Role ==
							PulseForgeEditor::EditorDocumentHostRole::Native &&
						PreviousNativeHost->Documents.size() == 1 &&
						PreviousNativeHost->Documents.front() == m_DocumentHostTabGroupTargetIdentity);
					if (EnteredDifferentNativeHost && PreviousNativeGroupIntact && LevelRetained)
					{
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Complete;
						if (m_DocumentHostTabTearOffFromNativeHost)
							PF_INFO("Document tab native-group tear-off UI fixture passed: the tab entered a new native host and the original group remained intact.");
						else if (m_DocumentHostTabRoundTripTearOffPending)
						{
							m_DocumentHostTabRoundTripTearOffPending = false;
							++m_DocumentHostTabRoundTripCount;
							PF_INFO(
								"Document tab native-window round-trip {}/{} passed: the same session was dragged into the main "
								"strip and back into a native host, while Level Editor remained in the main host.",
								m_DocumentHostTabRoundTripCount, DocumentTabRoundTripCycleCount);
							const bool HasGroupTarget = m_DocumentHostFixtureSessions.size() >= 2;
							const auto* GroupTargetSession = HasGroupTarget
								? m_Documents.Find(m_DocumentHostFixtureSessions[1].first) : nullptr;
							const auto* GroupTargetHost = GroupTargetSession
								? m_Documents.FindHost(GroupTargetSession->HostID) : nullptr;
							if (GroupTargetSession && GroupTargetHost &&
								GroupTargetHost->Role == PulseForgeEditor::EditorDocumentHostRole::Native &&
								GroupTargetHost->ID != Host->ID &&
								GroupTargetSession->Identity.Kind == Session->Identity.Kind)
							{
								m_DocumentHostTabGroupTargetIdentity = GroupTargetSession->Identity;
								m_DocumentHostTabGroupTestFrameCount = 0;
								m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::WaitingForTabs;
								m_DocumentHostTabGroupTest = true;
								PF_INFO("Document tab group fixture is moving the reattached session into a second native host.");
							}
							else
							{
								m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::Failed;
								PF_ERROR("Document tab group fixture could not find a second compatible native host.");
							}
						}
						else
						{
							PF_INFO(
								"Document tab tear-off UI fixture passed: the captured tab drag created a native host and kept "
								"Level Editor in the main host.");
						}
						m_DocumentHostTabTearOffFromNativeHost = false;
						m_DocumentHostTabTearOffSourceHostID.clear();
					}
					else
					{
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
						if (m_DocumentHostTabTearOffFromNativeHost)
							PF_ERROR("Document tab native-group tear-off fixture failed: the tab did not enter a separate native host or the original group changed.");
						else if (m_DocumentHostTabRoundTripTearOffPending)
							PF_ERROR("Document tab native-window round-trip fixture failed: the reattached session did not return to a native host or Level Editor left the main host.");
						else
							PF_ERROR("Document tab tear-off UI fixture failed: the released tab did not enter a native host or Level Editor left the main host.");
						m_DocumentHostTabRoundTripTearOffPending = false;
						m_DocumentHostTabTearOffFromNativeHost = false;
						m_DocumentHostTabTearOffSourceHostID.clear();
					}
					m_DocumentHostTabTearOffTest = false;
					break;
				}
				case DocumentTabTearOffTestPhase::Disabled:
				case DocumentTabTearOffTestPhase::Complete:
				case DocumentTabTearOffTestPhase::Failed:
					break;
			}
#endif
		}

		void QueueDocumentHostTabReattachTestInput()
		{
			if (!m_DocumentHostTabReattachTest)
				return;
#ifndef _DEBUG
			PF_ERROR("The document tab reattachment UI fixture is available only in Debug builds.");
			m_DocumentHostTabReattachTest = false;
#else
			ImGuiIO& IO = ImGui::GetIO();
			if (++m_DocumentHostTabReattachTestFrameCount > 600)
			{
				m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
				m_DocumentHostTabReattachTest = false;
				PF_ERROR("Document tab reattachment UI fixture timed out before validating all drop targets.");
				return;
			}
			switch (m_DocumentHostTabReattachTestPhase)
			{
				case DocumentTabReattachTestPhase::WaitingForTab:
					if (!m_DocumentHostTabReattachTestTabCenterValid)
						break;
					if (m_DocumentHostTabReattachInvalidDropIndex <
						m_DocumentHostTabReattachInvalidDropTargets.size())
					{
						DocumentTabTestPointerTarget& Target = m_DocumentHostTabReattachInvalidDropTargets[
							m_DocumentHostTabReattachInvalidDropIndex];
						if (!Target.Available)
							break;
						const float DeltaX = Target.Position.x - m_DocumentHostTabReattachTestTabCenter.x;
						const float DeltaY = Target.Position.y - m_DocumentHostTabReattachTestTabCenter.y;
						if (DeltaX * DeltaX + DeltaY * DeltaY <=
							IO.MouseDragThreshold * IO.MouseDragThreshold)
						{
							m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
							PF_ERROR("Document tab reattachment fixture could not place a drag over {} past the threshold.",
								Target.Name);
							break;
						}
						IO.AddMousePosEvent(
							m_DocumentHostTabReattachTestTabCenter.x,
							m_DocumentHostTabReattachTestTabCenter.y);
						IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestTabViewportID);
						IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
						m_DocumentHostTabReattachTestPhase =
							DocumentTabReattachTestPhase::InvalidDropPressQueued;
					}
					else if (m_DocumentHostTabReattachTestMainDropPositionValid)
					{
						const float DeltaX = m_DocumentHostTabReattachTestMainDropPosition.x -
							m_DocumentHostTabReattachTestTabCenter.x;
						const float DeltaY = m_DocumentHostTabReattachTestMainDropPosition.y -
							m_DocumentHostTabReattachTestTabCenter.y;
						if (DeltaX * DeltaX + DeltaY * DeltaY <=
							IO.MouseDragThreshold * IO.MouseDragThreshold)
						{
							m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
							PF_ERROR("Document tab reattachment fixture could not place a valid strip drop past the drag threshold.");
							break;
						}
						IO.AddMousePosEvent(
							m_DocumentHostTabReattachTestTabCenter.x,
							m_DocumentHostTabReattachTestTabCenter.y);
						IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestTabViewportID);
						IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
						m_DocumentHostTabReattachTestPhase =
							DocumentTabReattachTestPhase::ValidDropPressQueued;
						PF_INFO("Document tab reattachment fixture is returning the native tab to the main strip.");
					}
					break;
				case DocumentTabReattachTestPhase::InvalidDropPressQueued:
				{
					const auto& Target = m_DocumentHostTabReattachInvalidDropTargets[
						m_DocumentHostTabReattachInvalidDropIndex];
					IO.AddMousePosEvent(Target.Position.x, Target.Position.y);
					IO.AddMouseViewportEvent(Target.ViewportID);
					m_DocumentHostTabReattachTestPhase =
						DocumentTabReattachTestPhase::InvalidDropPositionQueued;
					break;
				}
				case DocumentTabReattachTestPhase::InvalidDropPositionQueued:
				{
					const auto& Target = m_DocumentHostTabReattachInvalidDropTargets[
						m_DocumentHostTabReattachInvalidDropIndex];
					IO.AddMousePosEvent(Target.Position.x, Target.Position.y);
					IO.AddMouseViewportEvent(Target.ViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
					m_DocumentHostTabReattachTestPhase =
						DocumentTabReattachTestPhase::InvalidDropReleaseQueued;
					break;
				}
				case DocumentTabReattachTestPhase::InvalidDropReleaseQueued:
				{
					const auto& Target = m_DocumentHostTabReattachInvalidDropTargets[
						m_DocumentHostTabReattachInvalidDropIndex];
					const auto* Session = m_Documents.Find(m_DocumentHostTabReattachTestIdentity);
					const bool StillInSourceHost = Session &&
						Session->HostID == m_DocumentHostTabReattachTestSourceHostID;
					const bool NoRegisteredStripTarget =
						!FindDocumentTabDropTarget(Target.Position, Target.ViewportID);
					if (!StillInSourceHost || !NoRegisteredStripTarget)
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
						PF_ERROR("Document tab drag over {} changed document membership or matched a registered strip.",
							Target.Name);
						break;
					}
					PF_INFO("Document tab drag over {} was rejected without changing host membership.", Target.Name);
					++m_DocumentHostTabReattachInvalidDropIndex;
					m_DocumentHostTabReattachTestTabCenterValid = false;
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::WaitingForTab;
					break;
				}
				case DocumentTabReattachTestPhase::ValidDropPressQueued:
					IO.AddMousePosEvent(
						m_DocumentHostTabReattachTestMainDropPosition.x,
						m_DocumentHostTabReattachTestMainDropPosition.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestMainViewportID);
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::ValidDropPositionQueued;
					break;
				case DocumentTabReattachTestPhase::ValidDropPositionQueued:
					IO.AddMousePosEvent(
						m_DocumentHostTabReattachTestMainDropPosition.x,
						m_DocumentHostTabReattachTestMainDropPosition.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestMainViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::ValidDropReleaseQueued;
					break;
				case DocumentTabReattachTestPhase::ValidDropReleaseQueued:
				{
					const auto* Session = m_Documents.Find(m_DocumentHostTabReattachTestIdentity);
					const auto* Host = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
					const auto* MainHost = m_Documents.FindHost(
						PulseForgeEditor::EditorDocumentManager::MainHostID);
					const bool LevelRetained = MainHost && std::any_of(
						MainHost->Documents.begin(), MainHost->Documents.end(), [](const auto& Identity)
						{
							return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level;
						});
					const bool SourceHostRetired =
						m_Documents.FindHost(m_DocumentHostTabReattachTestSourceHostID) == nullptr;
					const bool SourceHostStateMatches = m_DocumentHostTabReattachExpectSourceHostRetired
						? SourceHostRetired : !SourceHostRetired;
					if (Session && Host && Host->Role == PulseForgeEditor::EditorDocumentHostRole::Main &&
						SourceHostStateMatches && LevelRetained)
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Complete;
						PF_INFO(
							"Document tab reattachment UI fixture passed: {} non-strip drops were rejected, then the tab "
							"returned to the main strip; source-host retirement matched its remaining membership. Testing "
							"whether it can be dragged back into a native host (round trip {}/{}).",
							m_DocumentHostTabReattachInvalidDropIndex,
							m_DocumentHostTabRoundTripCount + 1,
							DocumentTabRoundTripCycleCount);
						m_DocumentHostTabTearOffTestIdentity = m_DocumentHostTabReattachTestIdentity;
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::WaitingForTab;
						m_DocumentHostTabTearOffTest = true;
						m_DocumentHostTabRoundTripTearOffPending = true;
					}
					else
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
						PF_ERROR("Document tab reattachment UI fixture failed: the native session did not return to the main strip.");
					}
					m_DocumentHostTabReattachTest = false;
					break;
				}
				case DocumentTabReattachTestPhase::Disabled:
				case DocumentTabReattachTestPhase::Complete:
				case DocumentTabReattachTestPhase::Failed:
					break;
			}
#endif
		}

		void QueueDocumentTabReorderTestInput()
		{
			if (!m_DocumentHostTabReorderTest)
				return;
#ifndef _DEBUG
			PF_ERROR("The document tab reorder UI fixture is available only in Debug builds.");
			m_DocumentHostTabReorderTest = false;
#else
			ImGuiIO& IO = ImGui::GetIO();
			if (++m_DocumentHostTabReorderTestFrameCount > 600)
			{
				m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
				m_DocumentHostTabReorderTest = false;
				PF_ERROR("Document tab reorder UI fixture timed out before restoring the original tab order.");
				return;
			}

			const auto HasExpectedOrder = [this](bool Reversed)
			{
				const auto* MainHost = m_Documents.FindHost(
					PulseForgeEditor::EditorDocumentManager::MainHostID);
				const auto* FirstSession = m_Documents.Find(m_DocumentHostTabReorderTestFirst);
				const auto* SecondSession = m_Documents.Find(m_DocumentHostTabReorderTestSecond);
				if (!MainHost || !FirstSession || !SecondSession ||
					FirstSession->HostID != MainHost->ID || SecondSession->HostID != MainHost->ID ||
					MainHost->ActiveDocument != m_DocumentHostTabReorderTestFirst)
					return false;
				const auto First = std::find(
					MainHost->Documents.begin(), MainHost->Documents.end(), m_DocumentHostTabReorderTestFirst);
				const auto Second = std::find(
					MainHost->Documents.begin(), MainHost->Documents.end(), m_DocumentHostTabReorderTestSecond);
				const bool HasLevel = std::any_of(
					MainHost->Documents.begin(), MainHost->Documents.end(), [](const auto& Identity)
					{
						return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level;
					});
				return HasLevel && First != MainHost->Documents.end() && Second != MainHost->Documents.end() &&
					(Reversed ? Second < First : First < Second);
			};
			const auto QueuePointer = [&IO](ImVec2 Position, ImGuiID ViewportID)
			{
				IO.AddMousePosEvent(Position.x, Position.y);
				IO.AddMouseViewportEvent(ViewportID);
			};

			switch (m_DocumentHostTabReorderTestPhase)
			{
				case DocumentTabReorderTestPhase::WaitingForTabs:
					if (!m_DocumentHostTabReorderTestFirstTabCenterValid ||
						!m_DocumentHostTabReorderTestSecondAfterPositionValid)
						break;
					QueuePointer(m_DocumentHostTabReorderTestFirstTabCenter,
						m_DocumentHostTabReorderTestFirstTabViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ForwardPressQueued;
					break;
				case DocumentTabReorderTestPhase::ForwardPressQueued:
				{
					const float DeltaX = m_DocumentHostTabReorderTestSecondAfterPosition.x -
						m_DocumentHostTabReorderTestFirstTabCenter.x;
					const float DeltaY = m_DocumentHostTabReorderTestSecondAfterPosition.y -
						m_DocumentHostTabReorderTestFirstTabCenter.y;
					if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
					{
						m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
						m_DocumentHostTabReorderTest = false;
						PF_ERROR("Document tab reorder fixture could not drag the first tab past the threshold.");
						break;
					}
					QueuePointer(m_DocumentHostTabReorderTestSecondAfterPosition,
						m_DocumentHostTabReorderTestFirstTabViewportID);
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ForwardPositionQueued;
					break;
				}
				case DocumentTabReorderTestPhase::ForwardPositionQueued:
					QueuePointer(m_DocumentHostTabReorderTestSecondAfterPosition,
						m_DocumentHostTabReorderTestFirstTabViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ForwardReleaseQueued;
					break;
				case DocumentTabReorderTestPhase::ForwardReleaseQueued:
					if (!HasExpectedOrder(true))
					{
						m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
						m_DocumentHostTabReorderTest = false;
						PF_ERROR("Document tab reorder fixture did not move the first session after its neighbor.");
						break;
					}
					PF_INFO("Document tab reorder fixture moved the synthetic session after its neighbor.");
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::WaitingForReverse;
					break;
				case DocumentTabReorderTestPhase::WaitingForReverse:
					if (!m_DocumentHostTabReorderTestFirstTabCenterValid ||
						!m_DocumentHostTabReorderTestSecondBeforePositionValid)
						break;
					QueuePointer(m_DocumentHostTabReorderTestFirstTabCenter,
						m_DocumentHostTabReorderTestFirstTabViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ReversePressQueued;
					break;
				case DocumentTabReorderTestPhase::ReversePressQueued:
				{
					const float DeltaX = m_DocumentHostTabReorderTestSecondBeforePosition.x -
						m_DocumentHostTabReorderTestFirstTabCenter.x;
					const float DeltaY = m_DocumentHostTabReorderTestSecondBeforePosition.y -
						m_DocumentHostTabReorderTestFirstTabCenter.y;
					if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
					{
						m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
						m_DocumentHostTabReorderTest = false;
						PF_ERROR("Document tab reorder fixture could not drag the first tab back past the threshold.");
						break;
					}
					QueuePointer(m_DocumentHostTabReorderTestSecondBeforePosition,
						m_DocumentHostTabReorderTestFirstTabViewportID);
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ReversePositionQueued;
					break;
				}
				case DocumentTabReorderTestPhase::ReversePositionQueued:
					QueuePointer(m_DocumentHostTabReorderTestSecondBeforePosition,
						m_DocumentHostTabReorderTestFirstTabViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ReverseReleaseQueued;
					break;
				case DocumentTabReorderTestPhase::ReverseReleaseQueued:
					if (!HasExpectedOrder(false))
					{
						m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
						m_DocumentHostTabReorderTest = false;
						PF_ERROR("Document tab reorder fixture did not restore the original session order.");
						break;
					}
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Complete;
					m_DocumentHostTabReorderTest = false;
					PF_INFO("Document tab reorder UI fixture passed: the same synthetic session moved after and before its neighbor, preserving the active session and Level Editor tab.");
					break;
				case DocumentTabReorderTestPhase::Disabled:
				case DocumentTabReorderTestPhase::Complete:
				case DocumentTabReorderTestPhase::Failed:
					break;
			}
#endif
		}

		void QueueDocumentTabGroupTestInput()
		{
			if (!m_DocumentHostTabGroupTest)
				return;
#ifndef _DEBUG
			PF_ERROR("The document tab group UI fixture is available only in Debug builds.");
			m_DocumentHostTabGroupTest = false;
#else
			ImGuiIO& IO = ImGui::GetIO();
			const auto Fail = [this](const char* Message)
			{
				m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::Failed;
				m_DocumentHostTabGroupTest = false;
				PF_ERROR("Document tab group UI fixture failed: {}", Message);
			};
			if (++m_DocumentHostTabGroupTestFrameCount > 600)
			{
				Fail("timed out before the native document tabs were grouped.");
				return;
			}

			switch (m_DocumentHostTabGroupTestPhase)
			{
				case DocumentTabGroupTestPhase::WaitingForTabs:
				{
					if (!m_DocumentHostTabGroupSourceCenterValid || !m_DocumentHostTabGroupTargetCenterValid)
						break;
					const auto* SourceSession = m_Documents.Find(m_DocumentHostTabTearOffTestIdentity);
					const auto* TargetSession = m_Documents.Find(m_DocumentHostTabGroupTargetIdentity);
					const auto* SourceHost = SourceSession ? m_Documents.FindHost(SourceSession->HostID) : nullptr;
					const auto* TargetHost = TargetSession ? m_Documents.FindHost(TargetSession->HostID) : nullptr;
					if (!SourceSession || !TargetSession || !SourceHost || !TargetHost ||
						SourceHost->Role != PulseForgeEditor::EditorDocumentHostRole::Native ||
						TargetHost->Role != PulseForgeEditor::EditorDocumentHostRole::Native ||
						SourceHost->ID == TargetHost->ID ||
						SourceSession->Identity.Kind != TargetSession->Identity.Kind)
					{
						Fail("source and destination are not separate compatible native hosts.");
						break;
					}
					const float DeltaX = m_DocumentHostTabGroupTargetCenter.x - m_DocumentHostTabGroupSourceCenter.x;
					const float DeltaY = m_DocumentHostTabGroupTargetCenter.y - m_DocumentHostTabGroupSourceCenter.y;
					if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
					{
						Fail("source and destination tabs are too close to create a drag.");
						break;
					}
					m_DocumentHostTabGroupSourceHostID = SourceHost->ID;
					m_DocumentHostTabGroupTargetHostID = TargetHost->ID;
					IO.AddMousePosEvent(
						m_DocumentHostTabGroupSourceCenter.x,
						m_DocumentHostTabGroupSourceCenter.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabGroupSourceViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
					m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::PressQueued;
					break;
				}
				case DocumentTabGroupTestPhase::PressQueued:
					IO.AddMousePosEvent(
						m_DocumentHostTabGroupTargetCenter.x,
						m_DocumentHostTabGroupTargetCenter.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabGroupTargetViewportID);
					m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::PositionQueued;
					break;
				case DocumentTabGroupTestPhase::PositionQueued:
					IO.AddMousePosEvent(
						m_DocumentHostTabGroupTargetCenter.x,
						m_DocumentHostTabGroupTargetCenter.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabGroupTargetViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
					m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::ReleaseQueued;
					break;
				case DocumentTabGroupTestPhase::ReleaseQueued:
				{
					const auto* SourceSession = m_Documents.Find(m_DocumentHostTabTearOffTestIdentity);
					const auto* TargetSession = m_Documents.Find(m_DocumentHostTabGroupTargetIdentity);
					const auto* TargetHost = m_Documents.FindHost(m_DocumentHostTabGroupTargetHostID);
					const auto* MainHost = m_Documents.FindHost(
						PulseForgeEditor::EditorDocumentManager::MainHostID);
					const bool BothSessionsGrouped = TargetHost && SourceSession && TargetSession &&
						SourceSession->HostID == TargetHost->ID && TargetSession->HostID == TargetHost->ID &&
						TargetHost->Documents.size() == 2 && TargetHost->ActiveDocument == SourceSession->Identity;
					const bool SourceHostRetired =
						m_Documents.FindHost(m_DocumentHostTabGroupSourceHostID) == nullptr;
					const bool LevelRetained = MainHost && std::any_of(
						MainHost->Documents.begin(), MainHost->Documents.end(), [](const auto& Identity)
						{
							return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level;
						});
					if (!BothSessionsGrouped || !SourceHostRetired || !LevelRetained)
					{
						Fail("the same session did not join the target native tab stack or the empty host was not retired.");
						break;
					}
					m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::Complete;
					m_DocumentHostTabGroupTest = false;
					++m_DocumentHostTabGroupCycleCount;
					PF_INFO(
						"Document tab group UI fixture passed for round trip {}: the same session joined an existing native "
						"tab stack and the empty source host was retired.",
						m_DocumentHostTabGroupCycleCount);
					if (m_DocumentHostTabGroupCycleCount < DocumentTabRoundTripCycleCount)
					{
						m_DocumentHostTabReattachTestIdentity = m_DocumentHostTabTearOffTestIdentity;
						m_DocumentHostTabReattachTestSourceHostID = TargetHost->ID;
						m_DocumentHostTabReattachExpectSourceHostRetired = false;
						m_DocumentHostTabReattachInvalidDropIndex = 0;
						m_DocumentHostTabReattachTestFrameCount = 0;
						m_DocumentHostTabReattachTestTabCenterValid = false;
						m_DocumentHostTabReattachTestMainDropPositionValid = false;
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::WaitingForTab;
						m_DocumentHostTabReattachTest = true;
						PF_INFO("Starting the next bounded native/main transfer cycle with the same document identity.");
					}
					else
						PF_INFO(
							"Document tab repeated-transfer UI fixture passed: the same session completed {} native/main "
							"round trips and grouping operations.",
							m_DocumentHostTabGroupCycleCount);
					if (m_DocumentHostTabGroupCycleCount == DocumentTabRoundTripCycleCount)
					{
						m_DocumentHostTabTearOffFromNativeHost = true;
						m_DocumentHostTabTearOffSourceHostID = TargetHost->ID;
						m_DocumentHostTabTearOffTestFrameCount = 0;
						m_DocumentHostTabTearOffTestTabCenterValid = false;
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::WaitingForTab;
						m_DocumentHostTabTearOffTest = true;
						PF_INFO("Document tab native-group tear-off fixture is detaching the same session outside all editor windows.");
					}
					break;
				}
				case DocumentTabGroupTestPhase::Disabled:
				case DocumentTabGroupTestPhase::Complete:
				case DocumentTabGroupTestPhase::Failed:
					break;
			}
#endif
		}

		void ValidateDocumentHostResetLayoutTest()
		{
			if (!m_DocumentHostResetLayoutTestPending)
				return;
			m_DocumentHostResetLayoutTestPending = false;

			bool SessionsPreserved = m_Documents.GetHosts().size() == m_DocumentHostResetLayoutExpectedHostCount;
			for (const auto& [Identity, ExpectedHostID] : m_DocumentHostFixtureSessions)
			{
				const auto* Session = m_Documents.Find(Identity);
				SessionsPreserved = SessionsPreserved && Session && Session->HostID == ExpectedHostID &&
					Session->IsDirty() && Session->Revision == Session->SavedRevision + 1;
			}

			if (SessionsPreserved)
				PF_INFO("Document host reset-layout test passed: {} dirty fixture sessions retained their hosts.",
					m_DocumentHostFixtureSessions.size());
			else
				PF_ERROR("Document host reset-layout test failed: dirty fixture sessions or host membership changed.");

			// These synthetic sessions must not trigger a dirty-close dialog during test cleanup.
			for (const auto& [Identity, ExpectedHostID] : m_DocumentHostFixtureSessions)
			{
				(void)ExpectedHostID;
				if (const auto* Session = m_Documents.Find(Identity))
					(void)m_Documents.MarkSaved(Identity, Session->Revision);
			}
		}

		void MarkDocumentWorkspaceDirty(bool BoundsOnly = false)
		{
			if (!m_Project || m_DocumentWorkspacePath.empty())
				return;
			if (BoundsOnly)
			{
				m_DocumentWorkspaceBoundsDirty = true;
				m_DocumentWorkspaceSaveAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
			}
			else
				m_DocumentWorkspaceImmediateDirty = true;
		}

		void RestoreDocumentWorkspace()
		{
			if (!m_Project)
				return;
			m_DocumentWorkspacePath = m_Layout.GetDocumentWorkspacePath(
				m_Project->GetDescription().ID.ToString());
			m_DocumentWorkspaceLastContents.clear();
			auto File = ReadDocumentWorkspaceFile(m_DocumentWorkspacePath);
			if (!File)
			{
				PF_WARN("Could not read document workspace: {}", File.error());
				m_DocumentWorkspaceImmediateDirty = false;
				m_DocumentWorkspaceBoundsDirty = false;
				return;
			}
			if (!*File)
			{
				m_DocumentWorkspaceImmediateDirty = true;
				return;
			}

			const std::string Contents = **File;
			auto Workspace = PulseForgeEditor::EditorDocumentWorkspaceCodec::Deserialize(
				Contents, m_Project->GetDescription().ID);
			if (!Workspace)
			{
				std::filesystem::path RecoveryPath = m_DocumentWorkspacePath;
				RecoveryPath += L".recovery";
				std::error_code CopyError;
				if (!std::filesystem::exists(RecoveryPath, CopyError) && !CopyError)
					(void)std::filesystem::copy_file(m_DocumentWorkspacePath, RecoveryPath, CopyError);
				PF_WARN("Document workspace was rejected and will be rebuilt from registered documents: {}",
					Workspace.error());
				m_DocumentWorkspaceImmediateDirty = true;
				return;
			}

			m_DocumentWorkspaceLastContents = Contents;
			bool SkippedDocument = false;
			if (Workspace->MaterialDetailsAsset)
			{
				const auto Asset = m_Project->GetAssetRegistry().Find(*Workspace->MaterialDetailsAsset);
				if (Asset && NormalizedExtension(Asset->ProjectRelativePath) == ".material")
				{
					m_MaterialDetailsAsset = *Workspace->MaterialDetailsAsset;
					(void)m_Layout.SetPanelVisible(PulseForgeEditor::EditorPanel::MaterialDetails, true);
					m_InitialMaterialDetailsDockPending = true;
					m_FocusMaterialDetailsNextFrame = true;
				}
				else
					SkippedDocument = true;
			}
			bool RecoveredMaterialDetails = false;
			std::optional<PulseForge::AssetID> FocusedActiveMaterial;
			for (const auto& Host : Workspace->Hosts)
				if (Host.ID == Workspace->FocusedHostID && Host.ActiveDocument &&
					Host.ActiveDocument->Kind == PulseForgeEditor::EditorDocumentKind::Material)
					FocusedActiveMaterial = Host.ActiveDocument->Asset;
			for (const PulseForge::AssetID& Candidate :
				PulseForgeEditor::GetMaterialDetailsMigrationCandidates(*Workspace))
			{
				const auto Asset = m_Project->GetAssetRegistry().Find(Candidate);
				if (!Asset || NormalizedExtension(Asset->ProjectRelativePath) != ".material")
					continue;
				if (FocusedActiveMaterial == Candidate || !m_MaterialDetailsAsset)
					m_MaterialDetailsAsset = Candidate;
				SkippedDocument = true;
				RecoveredMaterialDetails = true;
			}
			std::unordered_set<std::string> RestoredHosts;
			for (const auto& SavedHost : Workspace->Hosts)
			{
				if (SavedHost.Role != PulseForgeEditor::EditorDocumentHostRole::Native)
					continue;
				auto Created = m_Documents.CreateNativeHost(SavedHost.ID, SavedHost.PlacementKind);
				if (!Created)
				{
					PF_WARN("Could not restore document host {}: {}", SavedHost.ID, Created.error());
					continue;
				}
				RestoredHosts.insert(SavedHost.ID);
				if (SavedHost.Bounds.HasValue)
				{
					(void)m_Documents.SetHostBounds(SavedHost.ID, SavedHost.Bounds);
					m_RestoredHostBoundsPending.insert(SavedHost.ID);
				}
			}

			for (const auto& SavedHost : Workspace->Hosts)
			{
				const bool IsNativeHost = SavedHost.Role == PulseForgeEditor::EditorDocumentHostRole::Native;
				if (IsNativeHost && !RestoredHosts.contains(SavedHost.ID))
				{
					SkippedDocument |= !SavedHost.Documents.empty();
					continue;
				}
				for (const auto& Identity : SavedHost.Documents)
				{
					if (Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
					{
						if (!m_Documents.Find(Identity))
							SkippedDocument = true;
						continue;
					}
					std::filesystem::path RelativePath;
					std::string DisplayName;
					if (IsDocumentHostTestFixtureIdentity(Identity))
					{
						const auto FixtureIDs = GetDocumentHostTestFixtureAssetIDs();
						const std::string AssetIDText = Identity.Asset.ToString();
						const auto FixtureID = std::find(FixtureIDs.begin(), FixtureIDs.end(), AssetIDText);
						if (FixtureID == FixtureIDs.end())
						{
							SkippedDocument = true;
							continue;
						}
						const size_t FixtureIndex = static_cast<size_t>(std::distance(FixtureIDs.begin(), FixtureID));
						DisplayName = "Host Test Fixture " + std::to_string(FixtureIndex + 1);
					}
					else if (const auto Asset = m_Project->GetAssetRegistry().Find(Identity.Asset))
					{
						RelativePath = Asset->ProjectRelativePath;
						DisplayName = PathToUtf8(RelativePath.filename());
					}
					else
					{
						SkippedDocument = true;
						continue;
					}

					auto Context = MakeDocumentEditorContext(Identity, RelativePath, DisplayName);
					if (!m_DocumentEditors.CanOpen(Context))
					{
						SkippedDocument = true;
						continue;
					}
					auto Opened = m_Documents.Open(Identity, RelativePath, DisplayName,
						PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip);
					if (!Opened)
					{
						SkippedDocument = true;
						PF_WARN("Could not restore registered document {}: {}", DisplayName, Opened.error());
						continue;
					}
					auto* Session = m_Documents.Find(Identity);
					if (!Session || !m_DocumentEditors.Load(*Session, Context))
					{
						(void)m_Documents.Close(Identity,
							PulseForgeEditor::EditorDocumentCloseChoice::Discard, {}, {});
						SkippedDocument = true;
						PF_WARN("Could not load registered document session {}", DisplayName);
						continue;
					}
					const auto SavedDocument = std::find(SavedHost.Documents.begin(), SavedHost.Documents.end(), Identity);
					const size_t DestinationIndex = static_cast<size_t>(
						std::distance(SavedHost.Documents.begin(), SavedDocument));
					if (!m_Documents.Transfer(Identity, SavedHost.ID, DestinationIndex, {},
						PulseForgeEditor::EditorDocumentTransferCause::WorkspaceRestore))
					{
						(void)m_Documents.Close(Identity,
							PulseForgeEditor::EditorDocumentCloseChoice::Discard, {}, {});
						SkippedDocument = true;
						PF_WARN("Could not restore registered document placement for {}", DisplayName);
					}
				}
			}
			if (RecoveredMaterialDetails)
			{
				(void)m_Layout.SetPanelVisible(PulseForgeEditor::EditorPanel::MaterialDetails, true);
				m_InitialMaterialDetailsDockPending = true;
				m_ForceMaterialDetailsDock = true;
				m_FocusMaterialDetailsNextFrame = true;
				PF_INFO("Recovered saved material properties as the Material Details utility panel");
			}

			for (const auto& SavedHost : Workspace->Hosts)
			{
				const auto* Host = m_Documents.FindHost(SavedHost.ID);
				if (Host && SavedHost.ActiveDocument && m_Documents.Find(*SavedHost.ActiveDocument))
					(void)m_Documents.Activate(*SavedHost.ActiveDocument);
				if (Host && Host->Role == PulseForgeEditor::EditorDocumentHostRole::Native && Host->Documents.empty())
					(void)m_Documents.RemoveEmptyNativeHost(Host->ID);
			}
		for (const auto& [Kind, SavedPlacement] : Workspace->RememberedPlacements)
		{
			PulseForgeEditor::EditorDocumentPlacement Placement = SavedPlacement;
			const auto CompatibleHost = std::find_if(
				Workspace->Hosts.begin(), Workspace->Hosts.end(),
				[Kind, &RestoredHosts](const auto& Candidate)
				{
					return Candidate.Role == PulseForgeEditor::EditorDocumentHostRole::Native &&
						Candidate.PlacementKind == Kind && RestoredHosts.contains(Candidate.ID) &&
						!Candidate.Documents.empty();
				});
			if (CompatibleHost != Workspace->Hosts.end())
				Placement.LiveHostID = CompatibleHost->ID;
			(void)m_Documents.SetRememberedPlacement(
				Workspace->Project, Kind, Placement.Role, Placement.Bounds, Placement.LiveHostID);
		}
			if (!m_Documents.FocusHost(Workspace->FocusedHostID))
				FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
			m_DocumentWorkspaceImmediateDirty = SkippedDocument;
			m_DocumentWorkspaceBoundsDirty = false;
		}

		void SaveDocumentWorkspaceIfNeeded()
		{
			if (!m_Project || m_DocumentWorkspacePath.empty() ||
				(!m_DocumentWorkspaceImmediateDirty && !m_DocumentWorkspaceBoundsDirty))
				return;
			if (!m_DocumentWorkspaceImmediateDirty &&
				std::chrono::steady_clock::now() < m_DocumentWorkspaceSaveAt)
				return;

			PulseForgeEditor::EditorDocumentWorkspaceState Workspace;
			Workspace.Project = m_Project->GetDescription().ID;
			Workspace.MaterialDetailsAsset = m_MaterialDetailsAsset;
			if (const auto* FocusedHost = m_Documents.GetFocusedHost())
				Workspace.FocusedHostID = FocusedHost->ID;
			for (const auto& Host : m_Documents.GetHosts())
				if (Host.Role == PulseForgeEditor::EditorDocumentHostRole::Main || !Host.Documents.empty())
					Workspace.Hosts.push_back(Host);
			Workspace.RememberedPlacements = m_Documents.GetRememberedPlacements(Workspace.Project);
			std::sort(Workspace.RememberedPlacements.begin(), Workspace.RememberedPlacements.end(),
				[](const auto& Left, const auto& Right)
				{
					return static_cast<uint8_t>(Left.first) < static_cast<uint8_t>(Right.first);
				});
			auto Serialized = PulseForgeEditor::EditorDocumentWorkspaceCodec::Serialize(Workspace);
			if (!Serialized)
			{
				PF_WARN("Could not serialize editor document workspace: {}", Serialized.error());
				return;
			}
			if (*Serialized != m_DocumentWorkspaceLastContents)
			{
				auto Written = WriteDocumentWorkspaceFileAtomically(m_DocumentWorkspacePath, *Serialized);
				if (!Written)
				{
					PF_WARN("Could not persist editor document workspace: {}", Written.error());
					return;
				}
				m_DocumentWorkspaceLastContents = std::move(*Serialized);
			}
			m_DocumentWorkspaceImmediateDirty = false;
			m_DocumentWorkspaceBoundsDirty = false;
		}

		void SaveDocumentWorkspaceNow()
		{
			if (!m_Project || m_DocumentWorkspacePath.empty())
				return;
			m_DocumentWorkspaceImmediateDirty = true;
			SaveDocumentWorkspaceIfNeeded();
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
			m_ContentBrowser.SelectedAsset() = Imported->ID;
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
			OpenAssetDestinationDialog(Identifier, false);
		}

		void OpenAssetDestinationDialog(const PulseForge::AssetID& Identifier, bool Duplicate)
		{
			if (!m_Project)
				return;
			const auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			if (!Asset)
			{
				SetError("The selected asset is no longer registered.");
				return;
			}
			m_AssetWorkflow.BeginAssetDestination(*Asset, Duplicate);
		}

		void OpenSceneDestinationDialog(bool SaveAs)
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

			std::optional<PulseForge::AssetRecord> Source;
			if (SaveAs && m_SceneAsset)
				Source = m_Project->GetAssetRegistry().Find(*m_SceneAsset);
			m_AssetWorkflow.BeginSceneDestination(SaveAs, Source);
		}

		void OpenPrefabDestinationDialog(const PulseForge::UUID& Root)
		{
			if (!m_Project || !m_Scene || !m_Scene->FindEntity(Root))
			{
				SetError("Open a project, scene, and valid entity before creating a prefab.");
				return;
			}

			m_AssetWorkflow.BeginPrefabDestination(Root);
		}

		void DrawAssetDestinationBreadcrumbs()
		{
			const std::filesystem::path CurrentFolder =
				std::filesystem::u8path(m_AssetWorkflow.DestinationFolder()).lexically_normal();
			const std::vector<std::filesystem::path> Breadcrumbs =
				PulseForgeEditor::BuildAssetDestinationBreadcrumbs(CurrentFolder);
			if (Breadcrumbs.empty())
				return;

			if (ImGui::SmallButton("Assets##AssetDestinationBreadcrumbRoot"))
			{
				m_AssetWorkflow.DestinationFolder() = "Assets";
				m_AssetWorkflow.ValidationError().clear();
			}
			if (Breadcrumbs.size() == 1)
				return;
			if (Breadcrumbs.size() > 1)
			{
				ImGui::SameLine();
				m_EditorStyle.TextMuted("/");
				ImGui::SameLine();
				if (ImGui::SmallButton("...##AssetDestinationBreadcrumbAncestors"))
					ImGui::OpenPopup("AssetDestinationBreadcrumbAncestors");
				if (ImGui::BeginPopup("AssetDestinationBreadcrumbAncestors"))
				{
					for (size_t Index = 0; Index + 1 < Breadcrumbs.size(); ++Index)
					{
						const auto& Path = Breadcrumbs[Index];
						const std::string PathText = PathToUtf8(Path);
						if (ImGui::Selectable(PathText.c_str(), Path == CurrentFolder))
						{
							m_AssetWorkflow.DestinationFolder() = PathText;
							m_AssetWorkflow.ValidationError().clear();
						}
					}
					ImGui::EndPopup();
				}
				ImGui::SameLine();
				m_EditorStyle.TextMuted("/");
				ImGui::SameLine();
			}

			const std::string CurrentLabel = PathToUtf8(Breadcrumbs.back().filename());
			const float LabelWidth = (std::max)(ImGui::GetContentRegionAvail().x -
				ImGui::GetStyle().FramePadding.x * 2.0f, 0.0f);
			ImGui::TextUnformatted(FitEditorContextLabel(CurrentLabel, LabelWidth).c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", PathToUtf8(CurrentFolder).c_str());
		}

		void CollectContentFolders(
			const ContentFolderNode& Node,
			std::vector<std::filesystem::path>& Folders) const
		{
			Folders.push_back(Node.RelativePath);
			for (const ContentFolderNode& Child : Node.Children)
				CollectContentFolders(Child, Folders);
		}

		void DrawAssetDestinationDialog()
		{
			if (m_AssetWorkflow.TakeDestinationDialogRequest())
				ImGui::OpenPopup("Project Asset Destination");
			bool PopupOpen = true;
			if (!ImGui::BeginPopupModal("Project Asset Destination", &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
				return;

			const auto Asset = m_Project && m_AssetWorkflow.DestinationAsset()
				? m_Project->GetAssetRegistry().Find(*m_AssetWorkflow.DestinationAsset())
				: std::optional<PulseForge::AssetRecord>{};
			const bool IsProjectAssetCreation =
				m_AssetWorkflow.CreateKind() != ProjectAssetDestinationCreateKind::None;
			if (!m_Project || (!IsProjectAssetCreation && !Asset))
				ImGui::TextUnformatted(m_Project
					? "The asset is no longer registered."
					: "The project is no longer open.");
			else
			{
				const std::filesystem::path SourcePath = Asset
					? Asset->ProjectRelativePath
					: m_AssetWorkflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab
						? std::filesystem::path("Assets/NewPrefab.prefab")
						: std::filesystem::path("Assets/NewScene.scene");
				std::string Operation;
				if (Asset)
					Operation = m_AssetWorkflow.IsDuplicate() ? "Duplicate" : "Move / Rename";
				else if (m_AssetWorkflow.CreateKind() == ProjectAssetDestinationCreateKind::SaveSceneAs)
					Operation = "Save Scene As";
				else if (m_AssetWorkflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab)
					Operation = "Create Prefab";
				else
					Operation = "Create Scene";
				ImGui::Text("%s%s%s", Operation.c_str(), Asset ? " " : "",
					Asset ? PathToUtf8(Asset->ProjectRelativePath.filename()).c_str() : "");
				ImGui::TextDisabled("Choose a project folder and use the required asset extension.");
				DrawAssetDestinationBreadcrumbs();
				ImGui::SetNextItemWidth(420.0f);
				ImGui::InputTextWithHint("##AssetDestinationSearch", "Search project folders...",
					m_AssetWorkflow.SearchBuffer().data(), m_AssetWorkflow.SearchBuffer().size());

				std::vector<std::filesystem::path> Folders;
				CollectContentFolders(m_ContentBrowser.FolderRoot(), Folders);
				std::string Search = m_AssetWorkflow.SearchBuffer().data();
				std::transform(Search.begin(), Search.end(), Search.begin(), [](unsigned char Character)
				{
					return static_cast<char>(std::tolower(Character));
				});
				if (ImGui::BeginChild("AssetDestinationFolderResults", ImVec2(420.0f, 180.0f), ImGuiChildFlags_Borders))
				{
					bool AnyFolder = false;
					for (const auto& Folder : Folders)
					{
						const std::string FolderText = PathToUtf8(Folder);
						std::string Searchable = FolderText;
						std::transform(Searchable.begin(), Searchable.end(), Searchable.begin(), [](unsigned char Character)
						{
							return static_cast<char>(std::tolower(Character));
						});
						if (!Search.empty() && Searchable.find(Search) == std::string::npos)
							continue;
						AnyFolder = true;
						if (ImGui::Selectable(FolderText.c_str(), m_AssetWorkflow.DestinationFolder() == FolderText))
						{
							m_AssetWorkflow.DestinationFolder() = FolderText;
							m_AssetWorkflow.ValidationError().clear();
						}
					}
					if (!AnyFolder)
						ImGui::TextDisabled("No matching project folders.");
				}
				ImGui::EndChild();
				ImGui::TextWrapped("Destination: %s", m_AssetWorkflow.DestinationFolder().c_str());
				ImGui::SetNextItemWidth(420.0f);
				const bool EnterPressed = ImGui::InputText("Asset filename", m_AssetWorkflow.FilenameBuffer().data(),
					m_AssetWorkflow.FilenameBuffer().size(), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::TextDisabled("Extension: %s", SourcePath.extension().string().c_str());
				if (!m_AssetWorkflow.ValidationError().empty())
					ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error),
						"%s", m_AssetWorkflow.ValidationError().c_str());

				const char* SubmitLabel = m_AssetWorkflow.IsDuplicate() ? "Duplicate" :
					m_AssetWorkflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab ? "Create Prefab" :
					m_AssetWorkflow.CreateKind() == ProjectAssetDestinationCreateKind::SaveSceneAs ? "Save Scene As" :
					m_AssetWorkflow.CreateKind() == ProjectAssetDestinationCreateKind::NewScene ? "Create Scene" : "Move";
				if (ImGui::Button(SubmitLabel, ImVec2(132.0f, 0.0f)) ||
					EnterPressed)
				{
					const std::string FilenameText = m_AssetWorkflow.FilenameBuffer().data();
					const auto FinalFilename = PulseForgeEditor::ResolveAssetDestinationFilename(
						FilenameText, SourcePath);
					if (!FinalFilename)
					{
						switch (FinalFilename.error())
						{
							case PulseForgeEditor::AssetDestinationNameError::InvalidFilename:
								m_AssetWorkflow.ValidationError() = "Enter a single valid filename without a folder path.";
								break;
							case PulseForgeEditor::AssetDestinationNameError::ExtensionMismatch:
								m_AssetWorkflow.ValidationError() = "The destination must keep the original asset extension.";
								break;
							case PulseForgeEditor::AssetDestinationNameError::ReservedSidecarExtension:
								m_AssetWorkflow.ValidationError() = "The .meta extension is reserved for asset sidecars.";
								break;
						}
					}
					else if (!m_Project || (!IsProjectAssetCreation && !m_AssetWorkflow.DestinationAsset()) || !IsKnownContentFolder(
						std::filesystem::u8path(m_AssetWorkflow.DestinationFolder())))
						m_AssetWorkflow.ValidationError() = "Choose a folder that belongs to the open project.";
					else
					{
						const std::filesystem::path Target =
							std::filesystem::u8path(m_AssetWorkflow.DestinationFolder()) / *FinalFilename;
						const std::filesystem::path AbsoluteTarget = m_Project->GetRootPath() / Target;
						const std::filesystem::path AbsoluteSidecar =
							PulseForge::AssetMetadataSerializer::GetSidecarPath(AbsoluteTarget);
						const std::filesystem::path AbsoluteSource = Asset
							? m_Project->GetRootPath() / Asset->ProjectRelativePath
							: std::filesystem::path{};
						const auto Inspect = [](const std::filesystem::path& Path, bool& Exists, std::error_code& Error)
						{
							const auto Status = std::filesystem::symlink_status(Path, Error);
							if (Error == std::errc::no_such_file_or_directory)
							{
								Error.clear();
								Exists = false;
							}
							else
								Exists = !Error && Status.type() != std::filesystem::file_type::not_found;
						};
						std::error_code DestinationError;
						std::error_code SidecarError;
						bool DestinationExists = false;
						bool SidecarExists = false;
						Inspect(AbsoluteTarget, DestinationExists, DestinationError);
						Inspect(AbsoluteSidecar, SidecarExists, SidecarError);
						bool SameAsset = false;
						bool SameSidecar = false;
						std::error_code EquivalentError;
						if (!m_AssetWorkflow.IsDuplicate() && Asset && DestinationExists)
							SameAsset = std::filesystem::equivalent(AbsoluteSource, AbsoluteTarget, EquivalentError);
						EquivalentError.clear();
						if (!m_AssetWorkflow.IsDuplicate() && Asset && SidecarExists)
							SameSidecar = std::filesystem::equivalent(
								PulseForge::AssetMetadataSerializer::GetSidecarPath(AbsoluteSource), AbsoluteSidecar, EquivalentError);
						const bool InspectionFailed = static_cast<bool>(DestinationError) ||
							static_cast<bool>(SidecarError) || static_cast<bool>(EquivalentError);
						const std::string SourceSpelling = Asset
							? PathToUtf8(Asset->ProjectRelativePath.lexically_normal()) : std::string{};
						const std::string TargetSpelling = PathToUtf8(Target.lexically_normal());
						const auto Conflict = PulseForgeEditor::ResolveAssetDestinationConflict(
							m_AssetWorkflow.IsDuplicate() || IsProjectAssetCreation,
							DestinationExists, SidecarExists, SameAsset,
							Asset && SourceSpelling == TargetSpelling, InspectionFailed, SameSidecar);
						switch (Conflict)
						{
							case PulseForgeEditor::AssetDestinationConflict::SamePath:
								m_AssetWorkflow.ValidationError() = "The selected folder and filename are already the source asset location.";
								break;
							case PulseForgeEditor::AssetDestinationConflict::ExistingDestination:
								m_AssetWorkflow.ValidationError() = "An asset or metadata sidecar already exists at that destination.";
								break;
							case PulseForgeEditor::AssetDestinationConflict::CaseOnlyRename:
								// The asset operation stages the asset and its sidecar to support Windows case-only renames.
								[[fallthrough]];
							case PulseForgeEditor::AssetDestinationConflict::None:
							{
								const std::optional<PulseForge::AssetID> Identifier = m_AssetWorkflow.DestinationAsset();
								const bool Duplicate = m_AssetWorkflow.IsDuplicate();
								const ProjectAssetDestinationCreateKind CreateKind = m_AssetWorkflow.CreateKind();
								const std::optional<PulseForge::UUID> PrefabRoot = m_AssetWorkflow.PrefabRoot();
								m_AssetWorkflow.DismissDestination();
								ImGui::CloseCurrentPopup();
								if (Identifier)
								{
									if (Duplicate)
										QueueAfterSave([this, ID = *Identifier, Target] { DuplicateAssetAt(ID, Target); });
									else
										QueueAfterSave([this, ID = *Identifier, Target] { MoveAssetAt(ID, Target); });
								}
								else if (CreateKind == ProjectAssetDestinationCreateKind::NewScene ||
									CreateKind == ProjectAssetDestinationCreateKind::SaveSceneAs)
									QueueAfterSave([this, Target, CreateKind]
									{
										const std::filesystem::path AbsoluteTarget = m_Project
											? m_Project->GetRootPath() / Target : Target;
										CreateSceneAt(AbsoluteTarget,
											CreateKind == ProjectAssetDestinationCreateKind::SaveSceneAs);
									});
								else if (CreateKind == ProjectAssetDestinationCreateKind::Prefab && PrefabRoot)
									QueueAfterSave([this, Root = *PrefabRoot, Target] { CreatePrefabAt(Root, Target); });
								break;
							}
							case PulseForgeEditor::AssetDestinationConflict::InspectionFailed:
								m_AssetWorkflow.ValidationError() = "Could not inspect the destination. Check project folder permissions and try again.";
								break;
						}
					}
				}
			}
			ImGui::SameLine();
			const bool EscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
			if (ImGui::Button("Cancel", ImVec2(112.0f, 0.0f)) || EscapePressed)
			{
				m_AssetWorkflow.DismissDestination();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			if (!PopupOpen)
				m_AssetWorkflow.DismissDestination();
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
			m_ContentBrowser.SelectedAsset() = Moved->ID;
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

			m_ContentBrowser.RebaseFoldersAfterMove(Source, *Moved);

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

			OpenAssetDestinationDialog(Identifier, true);
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
			m_ContentBrowser.SelectedAsset() = Duplicated->ID;
			SetStatus(
				"Duplicated " + PathToUtf8(Duplicated->ProjectRelativePath) +
				" (new UUID " + Duplicated->ID.ToString() + ").");
		}

		void CreatePrefabFromSelectedEntity()
		{
			if (!m_Project || !m_Scene || !SelectedEntity())
			{
				SetError("Open a project and select an entity before creating a prefab.");
				return;
			}

			OpenPrefabDestinationDialog(*SelectedEntity());
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
					m_ContentBrowser.SelectedAsset() = Created.error().CommittedAsset->ID;
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
			m_ContentBrowser.SelectedAsset() = Created->ID;
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
			m_AssetWorkflow.BeginDelete(Identifier);
		}

		void DrawDeleteAssetDialog()
		{
			if (m_AssetWorkflow.TakeDeleteDialogRequest())
				ImGui::OpenPopup("Delete Managed Asset");

			bool PopupOpen = true;
			if (ImGui::BeginPopupModal("Delete Managed Asset", &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(
					PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingX,
					PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingY));
				const auto Asset = m_Project && m_AssetWorkflow.PendingDeleteAsset()
					? m_Project->GetAssetRegistry().Find(*m_AssetWorkflow.PendingDeleteAsset())
					: std::optional<PulseForge::AssetRecord>{};
				if (Asset)
					ImGui::TextWrapped("Permanently delete %s and its sidecar metadata?", PathToUtf8(Asset->ProjectRelativePath).c_str());
				else
					ImGui::TextUnformatted("The selected asset is no longer registered.");
				ImGui::TextUnformatted("This cannot be undone and may leave UUID references unresolved.");

				if (Asset && m_EditorStyle.DangerButton(PulseForgeEditor::EditorIcon::Delete, "Delete"))
				{
					const PulseForge::AssetID Identifier = Asset->ID;
					m_AssetWorkflow.DismissDelete();
					ImGui::CloseCurrentPopup();
					QueueAfterSave([this, Identifier] { DeleteAssetAt(Identifier); });
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
				{
					m_AssetWorkflow.DismissDelete();
					ImGui::CloseCurrentPopup();
				}
				ImGui::PopStyleVar();
				ImGui::EndPopup();
			}
			if (!PopupOpen)
			{
				m_AssetWorkflow.DismissDelete();
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
			const auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			const bool DeletedMaterial = Asset && NormalizedExtension(Asset->ProjectRelativePath) == ".material";
			std::vector<PulseForgeEditor::EditorDocumentIdentity> DeletedDocuments;
			for (const auto& [Identity, Session] : m_Documents.GetSessions())
			{
				(void)Session;
				if (Identity.Project == m_Project->GetDescription().ID && Identity.Asset == Identifier)
					DeletedDocuments.push_back(Identity);
			}
			auto PreparedClose = PrepareDocumentCloseBatch(DeletedDocuments);
			if (!PreparedClose)
			{
				m_DocumentCloseError = PreparedClose.error();
				m_PendingDocumentClose.reset();
				m_PendingDocumentHostClose.reset();
				m_PendingDocumentCloseBatch = DeletedDocuments;
				m_DocumentCloseDialogHostID = std::string(PulseForgeEditor::EditorDocumentManager::MainHostID);
				m_PendingDocumentCloseAction = [this, Identifier] { DeleteAssetAt(Identifier); };
				m_OpenDocumentCloseDialog = true;
				SetError(m_DocumentCloseError);
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
			if (!CommitPreparedDocumentCloseBatch(std::move(*PreparedClose)))
				return;
			if (DeletedMaterial)
				ForgetMaterialDraft(Identifier);
			if (m_ContentBrowser.SelectedAsset() && *m_ContentBrowser.SelectedAsset() == Identifier)
				m_ContentBrowser.SelectedAsset().reset();
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

			OpenSceneDestinationDialog(SaveAs);
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
			m_ContentBrowser.SelectedAsset() = Identifier;
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

			if (!m_SceneEditController.IsDirty())
			{
				QueueAfterDocuments(std::move(Action));
				return;
			}

			m_PendingAction = [this, Action = std::move(Action)]() mutable
			{
				QueueAfterDocuments(std::move(Action));
			};
			m_OpenUnsavedDialog = true;
		}

		bool DiscardSceneChanges()
		{
			if (!m_SceneEditController.IsDirty())
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
			if (!ClearCurrentProjectDocumentState([this] { CloseProjectNow(); }))
				return;
			ClearScene();
			ResetViewportSceneRenderer();
			m_Project.reset();
			m_ContentBrowser.ClearProject();
			m_AssetBrowserPresentations.clear();
			SetStatus("Project closed.");
		}

		void DrawMainMenu()
		{
			if (!ImGui::BeginMainMenuBar())
				return;
			if (ImGui::GetWindowViewport() && ImGui::GetMainViewport() &&
				ImGui::GetWindowViewport()->ID == ImGui::GetMainViewport()->ID &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
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
					QueueAfterSave([this] { RequestEditorApplicationClose(); });
				ImGui::EndMenu();
			}

			const bool MaterialDocumentFocused = IsFocusedMaterialDocument();
			const bool RegisteredDocumentFocused = GetFocusedRegisteredDocument() != nullptr;
			const bool SceneDocumentFocused = !IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails) &&
				(!GetFocusedDocumentIdentity() ||
				GetFocusedDocumentIdentity()->Kind == PulseForgeEditor::EditorDocumentKind::Level);
			if (ImGui::BeginMenu("Edit", m_Scene != nullptr || MaterialDocumentFocused || RegisteredDocumentFocused))
			{
				const auto UndoCommand = m_SceneEditController.GetUndoCommand();
				const auto RedoCommand = m_SceneEditController.GetRedoCommand();
				const bool CanEditScene = SceneDocumentFocused && m_Scene != nullptr && m_SceneRuntime == nullptr;
				bool CanUndoDocument = CanEditScene && m_SceneEditController.CanUndo();
				bool CanRedoDocument = CanEditScene && m_SceneEditController.CanRedo();
				if (RegisteredDocumentFocused)
					CanUndoDocument = CanRedoDocument = true;
				if (MaterialDocumentFocused)
				{
					const auto Material = GetFocusedMaterialAsset();
					if (Material)
					{
						const std::string Key = GetMaterialDraftKey(*Material);
						CanUndoDocument = m_MaterialDraftStore.CanUndo(Key);
						CanRedoDocument = m_MaterialDraftStore.CanRedo(Key);
					}
				}
				const bool HasSelection = !m_SceneWorkspace.Selection().GetEntities().empty();
				const std::string UndoLabel = RegisteredDocumentFocused
					? "Undo Document"
					: MaterialDocumentFocused
					? "Undo Material Edit"
					: UndoCommand
					? "Undo " + std::string(UndoCommand->Description)
					: "Undo";
				const std::string RedoLabel = RegisteredDocumentFocused
					? "Redo Document"
					: MaterialDocumentFocused
					? "Redo Material Edit"
					: RedoCommand
					? "Redo " + std::string(RedoCommand->Description)
					: "Redo";
				if (ImGui::MenuItem(UndoLabel.c_str(), "Ctrl+Z", false, CanUndoDocument))
				{
					if (RegisteredDocumentFocused)
					{
						if (auto* Session = GetFocusedRegisteredDocument())
							(void)m_DocumentEditors.DispatchFocusedCommand(
								*Session, PulseForgeEditor::EditorDocumentCommand::Undo);
					}
					else if (MaterialDocumentFocused)
						(void)ApplyMaterialHistoryStep(false);
					else
						UndoSceneEdit();
				}
				if (ImGui::MenuItem(RedoLabel.c_str(), "Ctrl+Y / Ctrl+Shift+Z", false, CanRedoDocument))
				{
					if (RegisteredDocumentFocused)
					{
						if (auto* Session = GetFocusedRegisteredDocument())
							(void)m_DocumentEditors.DispatchFocusedCommand(
								*Session, PulseForgeEditor::EditorDocumentCommand::Redo);
					}
					else if (MaterialDocumentFocused)
						(void)ApplyMaterialHistoryStep(true);
					else
						RedoSceneEdit();
				}
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
				if (ImGui::MenuItem("Rename", "F2", false, CanEditScene && SelectedEntity().has_value()))
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
					CanEditScene && !m_SceneWorkspace.Visibility().GetHiddenEntities().empty()))
					ShowAllEntities();
				if (ImGui::MenuItem("Isolate Selection", "Shift+H", false, CanEditScene && HasSelection))
					IsolateSelectedEntities();
				if (ImGui::MenuItem("Restore Isolation Visibility", nullptr, false,
					CanEditScene && m_SceneWorkspace.Visibility().GetIsolationBaseline().has_value()))
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
					for (const PulseForge::AssetRecord& Asset : m_ContentBrowser.Assets())
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
					const char* PanelLabel = Descriptor.Panel == PulseForgeEditor::EditorPanel::MaterialDetails
						? "Material Details" : Descriptor.WindowName.data();
					if (ImGui::MenuItem(PanelLabel, nullptr, IsVisible))
						SetEditorPanelVisible(Descriptor.Panel, !IsVisible);
				}
				ImGui::Separator();
				if (ImGui::BeginMenu("New asset documents open in"))
				{
					auto Preference = m_Documents.GetOpeningPreference();
					if (ImGui::MenuItem("New native window", nullptr,
						Preference == PulseForgeEditor::EditorDocumentOpenPreference::NewNativeWindow))
					{
						m_Documents.SetOpeningPreference(PulseForgeEditor::EditorDocumentOpenPreference::NewNativeWindow);
						(void)m_Layout.SetDocumentOpeningPreference(0);
					}
					if (ImGui::MenuItem("Main document strip", nullptr,
						Preference == PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip))
					{
						m_Documents.SetOpeningPreference(PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip);
						(void)m_Layout.SetDocumentOpeningPreference(1);
					}
					if (ImGui::MenuItem("Remembered placement", nullptr,
						Preference == PulseForgeEditor::EditorDocumentOpenPreference::RememberedPlacement))
					{
						m_Documents.SetOpeningPreference(PulseForgeEditor::EditorDocumentOpenPreference::RememberedPlacement);
						(void)m_Layout.SetDocumentOpeningPreference(2);
					}
					ImGui::EndMenu();
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
					if (m_SceneEditController.IsDirty())
						SceneName.push_back('*');
					const std::string SceneLabel = FitEditorContextLabel(SceneName, SceneWidth);
					ImGui::TextUnformatted(SceneLabel.c_str());
					if (SceneLabel != SceneName && ImGui::IsItemHovered())
						ImGui::SetTooltip("Scene: %s%s", SceneName.c_str(), m_SceneEditController.IsDirty() ? " (unsaved changes)" : "");
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
				const auto* FocusedDocument = GetFocusedDocumentIdentity();
				const bool MaterialCanSave = IsFocusedMaterialDocument() && FocusedDocument &&
					m_MaterialDraftStore.IsDirty(GetMaterialDraftKey(FocusedDocument->Asset));
				const bool RegisteredDocumentFocused = GetFocusedRegisteredDocument() != nullptr;
				ImGui::BeginDisabled(RegisteredDocumentFocused ? false : IsFocusedMaterialDocument() ? !MaterialCanSave :
					(!m_Scene || !m_SceneAsset || m_SceneRuntime));
				if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Save, "Save", "GlobalSaveScene",
					RegisteredDocumentFocused ? "Save the focused document" : IsFocusedMaterialDocument()
						? "Save Material Details" : "Save the current scene",
					false, SaveWidth, 40.0f))
					(void)SaveFocusedDocument();
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
					if (m_SceneEditController.IsDirty())
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
				const bool ShowDirtyState = m_Scene && m_SceneEditController.IsDirty();
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

		std::string GetDocumentPayload(const PulseForgeEditor::EditorDocumentIdentity& Identity) const
		{
			return Identity.Project.ToString() + "|" + Identity.Asset.ToString() + "|" +
				std::to_string(static_cast<uint32_t>(Identity.Kind));
		}

		bool SaveMaterialDraft(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
			{
				SetError("Cannot save a material document without an open project.");
				return false;
			}
			auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			if (!Asset || NormalizedExtension(Asset->ProjectRelativePath) != ".material")
			{
				SetError("The material document's asset is no longer registered.");
				return false;
			}
			const std::string Key = GetMaterialDraftKey(Identifier);
			PulseForge::MaterialAssetDesc* const Draft = m_MaterialDraftStore.FindDraft(Key);
			if (!Draft || !m_MaterialDraftStore.IsDirty(Key))
				return true;

			const auto Saved = PulseForge::MaterialAssetService::Save(
				Identifier, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), *Draft);
			if (!Saved)
			{
				SetError("Material save failed: " + Saved.error().Message);
				return false;
			}
			m_MaterialDraftStore.MarkSaved(Key);
			const PulseForgeEditor::EditorDocumentIdentity Identity{
				m_Project->GetDescription().ID, Identifier, PulseForgeEditor::EditorDocumentKind::Material
			};
			if (const auto* Session = m_Documents.Find(Identity))
				(void)m_Documents.MarkSaved(Identity, Session->Revision);
			m_AssetPreviewAssetsPending = true;
			if (m_SceneRenderer)
				m_SceneRenderer->RefreshMaterialAssets();
			SetStatus("Saved material " + PathToUtf8(Asset->ProjectRelativePath.filename()) + ".");
			return true;
		}

		bool ReloadMaterialDraft(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return false;
			auto Loaded = PulseForge::MaterialAssetService::Load(
				Identifier, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
			if (!Loaded)
			{
				SetError("Material reload failed: " + Loaded.error().Message);
				return false;
			}
			const std::string Key = GetMaterialDraftKey(Identifier);
			m_MaterialDraftStore.ReplaceDraft(Key, std::move(*Loaded));
			const PulseForgeEditor::EditorDocumentIdentity Identity{
				m_Project->GetDescription().ID, Identifier, PulseForgeEditor::EditorDocumentKind::Material
			};
			if (auto* Session = m_Documents.Find(Identity))
				Session->Revision = Session->SavedRevision;
			return true;
		}

		std::vector<PulseForgeEditor::EditorDocumentIdentity> GetDirtyAssetDocuments() const
		{
			std::vector<PulseForgeEditor::EditorDocumentIdentity> Dirty;
			for (const auto& [Identity, Session] : m_Documents.GetSessions())
				if (Session.IsDirty())
					Dirty.push_back(Identity);
			if (m_Project)
			{
				const PulseForge::UUID ProjectID = m_Project->GetDescription().ID;
				for (const std::string& Key : m_MaterialDraftStore.GetDirtyKeys())
				{
					const size_t Separator = Key.rfind(':');
					if (Separator == std::string::npos)
						continue;
					const auto Asset = PulseForge::UUID::Parse(std::string_view(Key).substr(Separator + 1));
					if (!Asset)
						continue;
					const PulseForgeEditor::EditorDocumentIdentity Identity{
						ProjectID, *Asset, PulseForgeEditor::EditorDocumentKind::Material };
					if (std::find(Dirty.begin(), Dirty.end(), Identity) == Dirty.end())
						Dirty.push_back(Identity);
				}
			}
			return Dirty;
		}

		std::expected<PulseForgeEditor::EditorDocumentCloseBatch, std::string> PrepareDocumentCloseBatch(
			const std::vector<PulseForgeEditor::EditorDocumentIdentity>& Documents) const
		{
			return PulseForgeEditor::EditorDocumentCloseBatch::Prepare(m_Documents, m_DocumentEditors, Documents);
		}

		bool CommitPreparedDocumentCloseBatch(PulseForgeEditor::EditorDocumentCloseBatch&& Prepared)
		{
			if (auto Result = std::move(Prepared).Commit(m_Documents, m_DocumentEditors); !Result)
			{
				m_DocumentCloseError = Result.error();
				SetError(m_DocumentCloseError);
				return false;
			}
			MarkDocumentWorkspaceDirty();
			return true;
		}

		bool CloseDocumentBatchClean(const std::vector<PulseForgeEditor::EditorDocumentIdentity>& Documents)
		{
			auto Prepared = PrepareDocumentCloseBatch(Documents);
			if (!Prepared)
			{
				m_DocumentCloseError = Prepared.error();
				SetError(m_DocumentCloseError);
				return false;
			}
			return CommitPreparedDocumentCloseBatch(std::move(*Prepared));
		}

		void RequestDocumentClose(const PulseForgeEditor::EditorDocumentIdentity& Identity)
		{
			const auto* Session = m_Documents.Find(Identity);
			if (!Session || Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
				return;
			if (!Session->IsDirty())
			{
				if (CloseDocumentBatchClean({ Identity }))
					return;
				m_PendingDocumentClose = Identity;
				m_PendingDocumentHostClose.reset();
				m_PendingDocumentCloseBatch = { Identity };
				m_DocumentCloseDialogHostID = Session->HostID;
				m_PendingDocumentCloseAction = {};
				m_OpenDocumentCloseDialog = true;
				return;
			}
			m_DocumentCloseError.clear();
			m_PendingDocumentClose = Identity;
			m_PendingDocumentHostClose.reset();
			m_PendingDocumentCloseBatch = { Identity };
			m_DocumentCloseDialogHostID = Session->HostID;
			m_PendingDocumentCloseAction = {};
			m_OpenDocumentCloseDialog = true;
		}

		void RequestDocumentHostClose(std::string_view HostID)
		{
			const auto* Host = m_Documents.FindHost(HostID);
			if (!Host || Host->Role != PulseForgeEditor::EditorDocumentHostRole::Native)
				return;
			const auto Documents = Host->Documents;
			const auto DirtyDocuments = PulseForgeEditor::FilterDirtyDocumentIdentities(
				Documents, [this](const auto& Identity)
			{
				const auto* Session = m_Documents.Find(Identity);
				return Session && Session->IsDirty();
			});
			if (DirtyDocuments.empty())
			{
				if (CloseDocumentBatchClean(Documents))
					return;
				m_PendingDocumentHostClose = std::string(HostID);
				m_PendingDocumentCloseBatch = Documents;
				m_DocumentCloseDialogHostID = std::string(HostID);
				m_PendingDocumentCloseAction = {};
				m_OpenDocumentCloseDialog = true;
				return;
			}
			m_DocumentCloseError.clear();
			m_PendingDocumentClose.reset();
			m_PendingDocumentHostClose = std::string(HostID);
			m_PendingDocumentCloseBatch = Documents;
			m_DocumentCloseDialogHostID = std::string(HostID);
			m_PendingDocumentCloseAction = {};
			m_OpenDocumentCloseDialog = true;
		}

		void QueueAfterDocuments(std::function<void()> Action)
		{
			if (!Action)
				return;
			if (GetDirtyAssetDocuments().empty())
			{
				m_DeferredEditorOperations.Enqueue(std::move(Action));
				return;
			}
			m_PendingDocumentClose.reset();
			m_PendingDocumentHostClose.reset();
			m_PendingDocumentCloseBatch.clear();
			m_PendingCloseAllDocuments = false;
			m_DocumentCloseError.clear();
			m_DocumentCloseDialogHostID = std::string(PulseForgeEditor::EditorDocumentManager::MainHostID);
			m_PendingDocumentCloseAction = std::move(Action);
			m_OpenDocumentCloseDialog = true;
		}

		bool ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice Choice)
		{
			if (Choice != PulseForgeEditor::EditorDocumentCloseChoice::Save &&
				Choice != PulseForgeEditor::EditorDocumentCloseChoice::Discard)
				return false;

			std::vector<PulseForgeEditor::EditorDocumentIdentity> Documents;
			const bool ClosingDocuments = m_PendingDocumentClose.has_value() ||
				m_PendingDocumentHostClose.has_value() || !m_PendingDocumentCloseBatch.empty() ||
				m_PendingCloseAllDocuments;
			if (!m_PendingDocumentCloseBatch.empty())
				Documents = m_PendingDocumentCloseBatch;
			else if (m_PendingDocumentClose)
				Documents.push_back(*m_PendingDocumentClose);
			else if (m_PendingDocumentHostClose)
			{
				if (const auto* Host = m_Documents.FindHost(*m_PendingDocumentHostClose))
					Documents = Host->Documents;
			}
			else
				Documents = GetDirtyAssetDocuments();
			if (m_PendingCloseAllDocuments)
				for (const auto& Identity : GetDirtyAssetDocuments())
					if (std::find(Documents.begin(), Documents.end(), Identity) == Documents.end())
						Documents.push_back(Identity);

			std::vector<PulseForgeEditor::EditorDocumentIdentity> CloseableDocuments;
			if (ClosingDocuments)
				for (const auto& Identity : Documents)
					if (m_Documents.Find(Identity))
						CloseableDocuments.push_back(Identity);

			auto ResolveDirtyChanges = [this, &Documents, Choice]() -> std::expected<void, std::string>
			{
				if (Choice == PulseForgeEditor::EditorDocumentCloseChoice::Save)
				{
					for (const auto& Identity : Documents)
					{
						auto* Session = m_Documents.Find(Identity);
						const bool DirtyMaterial = Identity.Kind == PulseForgeEditor::EditorDocumentKind::Material &&
							m_MaterialDraftStore.IsDirty(GetMaterialDraftKey(Identity.Asset));
						if (Session && Session->IsDirty())
						{
							if (m_DocumentEditors.IsRegistered(Identity.Kind) && Session->EditorState)
							{
								if (!m_DocumentEditors.Save(*Session) || Session->IsDirty())
									return std::unexpected("The document could not be saved; it remains open.");
							}
							else if (Identity.Kind == PulseForgeEditor::EditorDocumentKind::Material &&
								!SaveMaterialDraft(Identity.Asset))
								return std::unexpected("The material could not be saved; its draft remains open.");
							else if (Identity.Kind != PulseForgeEditor::EditorDocumentKind::Material)
								return std::unexpected("This document type does not provide a save operation.");
						}
						if (DirtyMaterial && !SaveMaterialDraft(Identity.Asset))
							return std::unexpected("The material could not be saved; its draft remains open.");
					}
				}
				else if (Choice == PulseForgeEditor::EditorDocumentCloseChoice::Discard)
				{
					std::vector<std::pair<PulseForgeEditor::EditorDocumentIdentity, PulseForge::MaterialAssetDesc>> Reloaded;
					for (const auto& Identity : Documents)
					{
						auto* Session = m_Documents.Find(Identity);
						const bool DirtyMaterial = Identity.Kind == PulseForgeEditor::EditorDocumentKind::Material &&
							m_MaterialDraftStore.IsDirty(GetMaterialDraftKey(Identity.Asset));
						if ((!Session || !Session->IsDirty()) && !DirtyMaterial)
							continue;
						if (Session && Session->IsDirty() && m_DocumentEditors.IsRegistered(Identity.Kind) &&
							Session->EditorState)
						{
							if (!m_DocumentEditors.Discard(*Session))
								return std::unexpected(
									"The document editor could not restore its saved state; unsaved edits were preserved.");
							continue;
						}
						if (Identity.Kind != PulseForgeEditor::EditorDocumentKind::Material || !m_Project)
							return std::unexpected("Discard is unavailable for this document type.");
						auto Loaded = PulseForge::MaterialAssetService::Load(
							Identity.Asset, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
						if (!Loaded)
							return std::unexpected(
								"Material reload failed; unsaved draft was preserved: " + Loaded.error().Message);
						Reloaded.emplace_back(Identity, std::move(*Loaded));
					}
					for (auto& [Identity, Material] : Reloaded)
					{
						const std::string Key = GetMaterialDraftKey(Identity.Asset);
						m_MaterialDraftStore.ReplaceDraft(Key, std::move(Material));
						if (auto* Session = m_Documents.Find(Identity))
							Session->Revision = Session->SavedRevision;
					}
				}
				return {};
			};

			bool CloseContinuationQueued = false;
			if (ClosingDocuments)
			{
				auto CloseResult = PulseForgeEditor::ResolveDocumentCloseBatchWithChanges(
					m_Documents, m_DocumentEditors, CloseableDocuments,
					Choice == PulseForgeEditor::EditorDocumentCloseChoice::Discard,
					ResolveDirtyChanges, m_PendingDocumentCloseAction,
					[this](std::function<void()> Continuation)
					{
						m_DeferredEditorOperations.Enqueue(std::move(Continuation));
					});
				if (!CloseResult)
				{
					m_DocumentCloseError = CloseResult.error();
					SetError(m_DocumentCloseError);
					if (m_DocumentCloseDialogHostID.empty())
					{
						if (m_PendingDocumentHostClose)
							m_DocumentCloseDialogHostID = *m_PendingDocumentHostClose;
						else if (!Documents.empty())
							if (const auto* Session = m_Documents.Find(Documents.front()))
								m_DocumentCloseDialogHostID = Session->HostID;
						if (m_DocumentCloseDialogHostID.empty())
							m_DocumentCloseDialogHostID =
								std::string(PulseForgeEditor::EditorDocumentManager::MainHostID);
					}
					return false;
				}
				CloseContinuationQueued = true;
				MarkDocumentWorkspaceDirty();
			}
			else if (auto Resolved = ResolveDirtyChanges(); !Resolved)
			{
				m_DocumentCloseError = Resolved.error();
				SetError(m_DocumentCloseError);
				return false;
			}
			m_PendingDocumentClose.reset();
			m_PendingDocumentHostClose.reset();
			m_PendingDocumentCloseBatch.clear();
			m_PendingCloseAllDocuments = false;
			m_DocumentCloseError.clear();
			m_DocumentCloseDialogHostID.clear();
			m_OpenDocumentCloseDialog = false;
			ImGui::CloseCurrentPopup();
			if (!CloseContinuationQueued)
				(void)PulseForgeEditor::QueueDocumentCloseContinuation(true, m_PendingDocumentCloseAction,
					[this](std::function<void()> Continuation)
					{
						m_DeferredEditorOperations.Enqueue(std::move(Continuation));
					});
			return true;
		}

		void DrawDocumentCloseDialog(std::string_view HostID)
		{
			if (HostID != m_DocumentCloseDialogHostID)
				return;

			const std::string PopupName = "Unsaved Asset Documents###PFDocumentClose_" + std::string(HostID);
			if (m_OpenDocumentCloseDialog)
			{
				ImGui::OpenPopup(PopupName.c_str());
				m_OpenDocumentCloseDialog = false;
			}
			bool PopupOpen = true;
			const bool PopupVisible = ImGui::BeginPopupModal(
				PopupName.c_str(), &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize);
			if (!PopupVisible)
				return;
			std::vector<PulseForgeEditor::EditorDocumentIdentity> DirtyDocuments;
			if (m_PendingCloseAllDocuments)
				DirtyDocuments = GetDirtyAssetDocuments();
			else if (m_PendingDocumentClose)
				DirtyDocuments.push_back(*m_PendingDocumentClose);
			else if (!m_PendingDocumentCloseBatch.empty())
				DirtyDocuments = PulseForgeEditor::FilterDirtyDocumentIdentities(
					m_PendingDocumentCloseBatch, [this](const auto& Identity)
					{
						const auto* Session = m_Documents.Find(Identity);
						return Session && Session->IsDirty();
					});
			else if (m_PendingDocumentHostClose)
			{
				if (const auto* Host = m_Documents.FindHost(*m_PendingDocumentHostClose))
					DirtyDocuments = PulseForgeEditor::FilterDirtyDocumentIdentities(
						Host->Documents, [this](const auto& Identity)
						{
							const auto* Session = m_Documents.Find(Identity);
							return Session && Session->IsDirty();
						});
			}
			else
				DirtyDocuments = GetDirtyAssetDocuments();
			ImGui::TextWrapped("%zu unsaved asset draft%s have changes.", DirtyDocuments.size(),
				DirtyDocuments.size() == 1 ? "" : "s");
			ImGui::TextUnformatted(DirtyDocuments.empty()
				? "The close request is still pending. Retry it or cancel."
				: "Save or discard these drafts before continuing?");
			if (!m_DocumentCloseError.empty())
				ImGui::TextWrapped("%s", m_DocumentCloseError.c_str());
			if (DirtyDocuments.empty() && (!m_PendingDocumentCloseBatch.empty() ||
				m_PendingDocumentClose || m_PendingDocumentHostClose || m_PendingCloseAllDocuments))
			{
				if (ImGui::Button("Retry Close", ImVec2(112.0f, 0.0f)))
					(void)ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice::Save);
				ImGui::SameLine();
			}
			if (ImGui::Button("Save", ImVec2(112.0f, 0.0f)))
				(void)ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice::Save);
			ImGui::SameLine();
			if (m_EditorStyle.DangerButton(PulseForgeEditor::EditorIcon::Delete, "Discard"))
				(void)ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice::Discard);
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(112.0f, 0.0f)))
			{
				m_PendingDocumentClose.reset();
				m_PendingDocumentHostClose.reset();
				m_PendingDocumentCloseBatch.clear();
				m_PendingCloseAllDocuments = false;
				m_DocumentCloseDialogHostID.clear();
				m_PendingDocumentCloseAction = {};
				m_DocumentCloseError.clear();
				m_OpenDocumentCloseDialog = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			if (!PopupOpen)
			{
				m_PendingDocumentClose.reset();
				m_PendingDocumentHostClose.reset();
				m_PendingDocumentCloseBatch.clear();
				m_PendingCloseAllDocuments = false;
				m_DocumentCloseDialogHostID.clear();
				m_PendingDocumentCloseAction = {};
				m_DocumentCloseError.clear();
				m_OpenDocumentCloseDialog = false;
			}
		}

		void DrawUnregisteredAssetDocument(const PulseForgeEditor::EditorDocumentIdentity& Identity)
		{
			(void)Identity;
			m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Warning,
				"No full asset editor is registered for this document type. Open property details from the Content Browser.");
		}

		void DrawNativeDocumentHosts()
		{
			std::vector<std::string> HostIDs;
			for (const auto& Host : m_Documents.GetHosts())
				if (Host.Role == PulseForgeEditor::EditorDocumentHostRole::Native && !Host.Documents.empty())
					HostIDs.push_back(Host.ID);
			for (const std::string& HostID : HostIDs)
			{
				const auto* Host = m_Documents.FindHost(HostID);
				if (!Host || !Host->ActiveDocument)
					continue;
				const auto* ActiveSession = m_Documents.Find(*Host->ActiveDocument);
				if (!ActiveSession)
					continue;
				std::string WindowName = ActiveSession->DisplayName + "###PFDocumentHost_" + HostID;
				if (m_DocumentHostFocusRequests.erase(HostID) != 0)
					ImGui::SetNextWindowFocus();
				const auto SavedBounds = Host->Bounds;
				const bool RestoreBounds = SavedBounds.HasValue && m_RestoredHostBoundsPending.contains(HostID);
				if (RestoreBounds)
				{
					const ResolvedNativeWindowBounds Bounds = ResolveRestoredNativeWindowBounds(SavedBounds);
					ImGui::SetNextWindowPos(Bounds.Position, ImGuiCond_Always);
					ImGui::SetNextWindowSize(Bounds.Size, ImGuiCond_Always);
				}
				else if (ImGuiViewport* MainViewport = ImGui::GetMainViewport())
				{
					ImGui::SetNextWindowPos(ImVec2(MainViewport->Pos.x + 80.0f,
						MainViewport->Pos.y + 84.0f), ImGuiCond_FirstUseEver);
					ImGui::SetNextWindowSize(ImVec2(560.0f, 760.0f), ImGuiCond_FirstUseEver);
				}
				ImGuiWindowClass DocumentWindowClass;
				DocumentWindowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
				ImGui::SetNextWindowClass(&DocumentWindowClass);
				bool IsOpen = true;
				const bool Visible = ImGui::Begin(WindowName.c_str(), &IsOpen, ImGuiWindowFlags_NoDocking);
				if (RestoreBounds)
				{
					m_RestoredHostBoundsPending.erase(HostID);
					if (SavedBounds.Maximized && ImGui::GetWindowViewport()->PlatformHandle)
						glfwMaximizeWindow(static_cast<GLFWwindow*>(ImGui::GetWindowViewport()->PlatformHandle));
					ImGui::MarkIniSettingsDirty();
				}
				if (!IsOpen)
					RequestDocumentHostClose(HostID);
				if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				{
					if (m_ViewportKeyboardFocused || m_ViewportMouseCapture.OwnsMouse() ||
						m_ViewportCameraGesture != ViewportCameraGesture::None)
						ResetViewportPanelInteraction();
					FocusDocumentHostIfChanged(HostID);
				}
				if (auto* CurrentHost = m_Documents.FindHost(HostID))
				{
					const ImVec2 Position = ImGui::GetWindowPos();
					const ImVec2 Size = ImGui::GetWindowSize();
					PulseForgeEditor::EditorDocumentHost::WindowBounds Bounds;
					Bounds.X = Position.x;
					Bounds.Y = Position.y;
					Bounds.Width = Size.x;
					Bounds.Height = Size.y;
					Bounds.HasValue = true;
					ImGuiViewport* WindowViewport = ImGui::GetWindowViewport();
					if (WindowViewport)
						Bounds.DpiScale = WindowViewport->DpiScale;
					if (GLFWwindow* NativeWindow = WindowViewport
						? static_cast<GLFWwindow*>(WindowViewport->PlatformHandle) : nullptr)
					{
						Bounds.Maximized = glfwGetWindowAttrib(NativeWindow, GLFW_MAXIMIZED) == GLFW_TRUE;
						if (GLFWmonitor* Monitor = FindMonitorForBounds(
							Bounds.X, Bounds.Y, Bounds.Width, Bounds.Height))
							if (const char* MonitorName = glfwGetMonitorName(Monitor))
								Bounds.MonitorName = MonitorName;
					}
					if (m_Documents.SetHostBounds(HostID, std::move(Bounds)))
						MarkDocumentWorkspaceDirty(true);
				}
				std::optional<PulseForgeEditor::EditorDocumentIdentity> RegisteredWorkspaceToDraw;
				if (Visible && IsOpen)
				{
					DrawDocumentTabStrip(HostID);
					ImGui::Separator();
					const auto* CurrentHost = m_Documents.FindHost(HostID);
					if (CurrentHost && CurrentHost->ActiveDocument)
					{
						auto* Session = m_Documents.Find(*CurrentHost->ActiveDocument);
						if (Session && m_DocumentEditors.IsRegistered(Session->Identity.Kind) && Session->EditorState)
						{
							m_WorkspacePresentation.DrawWorkspaceDockspace(*Session);
							RegisteredWorkspaceToDraw = Session->Identity;
						}
						else
							DrawUnregisteredAssetDocument(*CurrentHost->ActiveDocument);
					}
				}
				DrawDocumentCloseDialog(HostID);
				ImGui::End();
				if (Visible && IsOpen && RegisteredWorkspaceToDraw)
					if (auto* Session = m_Documents.Find(*RegisteredWorkspaceToDraw))
						m_WorkspacePresentation.DrawWorkspaceContent(*Session, m_DocumentEditors,
							[](void* Context, const auto& Identity)
							{
								static_cast<EditorLayer*>(Context)->DrawUnregisteredAssetDocument(Identity);
							}, this);
			}
		}

		bool ShouldInjectDocumentPresentationFailure(ImGuiViewport* Viewport)
		{
#ifndef _DEBUG
			(void)Viewport;
			return false;
#else
			if (!m_DocumentHostTestFixture || !m_DocumentHostPresentationFailureTest || !Viewport)
				return false;
			for (const auto& Host : m_Documents.GetHosts())
			{
				if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty())
					continue;
				const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
				if (!Session)
					continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				if (!Window || Window->Viewport != Viewport)
					continue;
				m_DocumentHostPresentationFailureTest = false;
				m_DocumentHostPresentationFailureInjected = true;
				PF_WARN("Injecting Debug-only native document presentation failure for host {}", Host.ID);
				return true;
			}
			return false;
#endif
		}

		bool ShouldInjectDocumentImageAllocationFailure(ImGuiViewport* Viewport)
		{
#ifndef _DEBUG
			(void)Viewport;
			return false;
#else
			if (!m_DocumentHostTestFixture || !m_DocumentHostImageAllocationFailureTest || !Viewport ||
				PulseForge::Application::Get().GetRendererAPI() != PulseForge::RendererAPI::Vulkan)
				return false;
			for (const auto& Host : m_Documents.GetHosts())
			{
				if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty())
					continue;
				const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
				if (!Session)
					continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				if (!Window || Window->Viewport != Viewport)
					continue;
				m_DocumentHostImageAllocationFailureTest = false;
				m_DocumentHostPresentationFailureInjected = true;
				bool AllSessionsMarkedDirty = true;
				for (const auto& Identity : Host.Documents)
					AllSessionsMarkedDirty = m_Documents.MarkEdited(Identity) && AllSessionsMarkedDirty;
				if (!AllSessionsMarkedDirty)
				{
					m_DocumentHostPresentationFailureInjected = false;
					PF_ERROR("Could not mark every synthetic document dirty for the allocation-failure test.");
					return false;
				}
				PF_WARN("Injecting Debug-only native swapchain image/depth allocation failure for host {}", Host.ID);
				return true;
			}
			return false;
#endif
		}

		bool ShouldInjectDocumentAcquireOutOfDate(ImGuiViewport* Viewport)
		{
#ifndef _DEBUG
			(void)Viewport;
			return false;
#else
			if (!m_DocumentHostTestFixture || !m_DocumentHostAcquireOutOfDateTest || !Viewport ||
				PulseForge::Application::Get().GetRendererAPI() != PulseForge::RendererAPI::Vulkan)
				return false;
			for (const auto& Host : m_Documents.GetHosts())
			{
				if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty())
					continue;
				const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
				if (!Session)
					continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				if (!Window || Window->Viewport != Viewport)
					continue;
				m_DocumentHostAcquireOutOfDateTest = false;
				PF_WARN("Scheduling a Debug-only out-of-date acquire for native document host {}", Host.ID);
				return true;
			}
			return false;
#endif
		}

		void CompleteDocumentPresentationFailureTest(
			std::string_view SourceHostID,
			const std::vector<PulseForgeEditor::EditorDocumentIdentity>& OriginalDocuments,
			size_t MovedCount,
			bool SourceHostRetired)
		{
			if (!m_DocumentHostPresentationFailureInjected)
				return;
			const auto* MainHost = m_Documents.FindHost(
				PulseForgeEditor::EditorDocumentManager::MainHostID);
			bool Recovered = SourceHostRetired && !m_Documents.FindHost(SourceHostID) && MainHost &&
				MovedCount == OriginalDocuments.size();
			for (const auto& Identity : OriginalDocuments)
			{
				const auto* Session = m_Documents.Find(Identity);
				Recovered = Recovered && Session &&
					Session->HostID == PulseForgeEditor::EditorDocumentManager::MainHostID && Session->IsDirty() &&
					MainHost && std::find(MainHost->Documents.begin(), MainHost->Documents.end(), Identity) !=
						MainHost->Documents.end();
			}
			Recovered = Recovered && MainHost && std::any_of(
				MainHost->Documents.begin(), MainHost->Documents.end(), [](const auto& Identity)
				{
					return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level;
				});
			if (Recovered)
				PF_INFO("Document presentation-failure fixture passed: {} dirty sessions were recovered in the main strip and Level Editor remained available.",
					OriginalDocuments.size());
			else
				PF_ERROR("Document presentation-failure fixture failed: dirty sessions, source-host retirement, or Level Editor retention was incorrect.");

			// The fixture creates synthetic sessions; mark them clean so automated shutdown
			// can validate teardown without opening a user-facing dirty-close prompt.
			for (const auto& Identity : OriginalDocuments)
				if (IsDocumentHostTestFixtureIdentity(Identity))
					if (const auto* Session = m_Documents.Find(Identity))
						(void)m_Documents.MarkSaved(Identity, Session->Revision);
			m_DocumentHostPresentationFailureInjected = false;
		}

		void HandlePlatformPresentationFailure(ImGuiViewport* Viewport, std::string_view Error)
		{
			if (Viewport)
			{
				for (const auto& Host : m_Documents.GetHosts())
				{
					if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty())
						continue;
					const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
					if (!Session)
						continue;
					const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
					ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
					if (!Window || Window->Viewport != Viewport)
						continue;

					PF_ERROR("Could not create native editor presentation target: {}", Error);
					const std::string FailedHostID = Host.ID;
					const std::vector<PulseForgeEditor::EditorDocumentIdentity> FailedHostDocuments = Host.Documents;
					auto Moved = m_Documents.RecoverAllToMain(FailedHostID);
					if (!Moved)
					{
						PF_ERROR("Could not recover all documents from failed native host {}: {}", FailedHostID, Moved.error());
						CompleteDocumentPresentationFailureTest(FailedHostID, FailedHostDocuments, 0, false);
						SetWarning("Native document presentation failed; its open sessions were preserved, but could not be moved to the main document strip.");
						Viewport->PlatformRequestClose = false;
						return;
					}
					if (m_Documents.FindHost(FailedHostID) && !m_Documents.RemoveEmptyNativeHost(FailedHostID))
					{
						PF_ERROR("Native host {} was emptied but could not be retired after presentation failure", FailedHostID);
						CompleteDocumentPresentationFailureTest(FailedHostID, FailedHostDocuments, *Moved, false);
						SetWarning("Native document presentation failed; the documents were recovered, but the empty host could not be retired.");
						Viewport->PlatformRequestClose = false;
						MarkDocumentWorkspaceDirty();
						return;
					}
					CompleteDocumentPresentationFailureTest(FailedHostID, FailedHostDocuments, *Moved,
						!m_Documents.FindHost(FailedHostID));
					MarkDocumentWorkspaceDirty();
					SetWarning("Native document window creation failed; its open documents were retained in the main document strip.");
					Viewport->PlatformRequestClose = true;
					return;
				}
				Viewport->PlatformRequestClose = true;
			}
			if (m_DocumentHostPresentationFailureInjected)
			{
				PF_ERROR("Document presentation-failure fixture could not match its viewport to an open native host.");
				m_DocumentHostPresentationFailureInjected = false;
			}
			PF_ERROR("Could not create native editor presentation target: {}", Error);
		}

		void HandleNativePlatformCloseRequests()
		{
			if (!m_Context || !(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
				return;
			ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			std::vector<std::string> HostIDs;
			for (const auto& Host : m_Documents.GetHosts())
				if (Host.Role == PulseForgeEditor::EditorDocumentHostRole::Native && !Host.Documents.empty())
					HostIDs.push_back(Host.ID);
			for (const std::string& HostID : HostIDs)
			{
				const auto* Host = m_Documents.FindHost(HostID);
				if (!Host || Host->Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host->Documents.empty())
					continue;
				const auto* Session = Host->ActiveDocument ? m_Documents.Find(*Host->ActiveDocument) : nullptr;
				if (!Session)
					continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + HostID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				ImGuiViewport* Viewport = Window ? Window->Viewport : nullptr;
				if (!Viewport || Viewport == MainViewport || !Viewport->PlatformRequestClose)
					continue;
				Viewport->PlatformRequestClose = false;
				RequestDocumentHostClose(HostID);
			}
		}

		void QueueDocumentTabClose(const PulseForgeEditor::EditorDocumentIdentity& Identity)
		{
			if (!m_PendingDocumentTabClose)
				m_PendingDocumentTabClose = Identity;
		}

		void ProcessPendingDocumentTabClose()
		{
			if (!m_PendingDocumentTabClose)
				return;

			const PulseForgeEditor::EditorDocumentIdentity Identity = *m_PendingDocumentTabClose;
			m_PendingDocumentTabClose.reset();
			RequestDocumentClose(Identity);
		}

		void QueueDocumentTransfer(
			const PulseForgeEditor::EditorDocumentIdentity& Identity,
			std::string DestinationHostID,
			size_t Index,
			bool CreateNativeHost = false,
			std::optional<PulseForgeEditor::EditorDocumentHost::WindowBounds> NativeHostBounds = std::nullopt)
		{
			if (m_PendingDocumentTransfer)
				return;
			m_PendingDocumentTransfer = PendingDocumentTransfer{
				Identity, std::move(DestinationHostID), Index, CreateNativeHost, std::move(NativeHostBounds) };
		}

		void ProcessReleasedDocumentTabDrag()
		{
			const ImGuiIO& IO = ImGui::GetIO();
			const auto CurrentPosition = PulseForgeEditor::IsUsableDocumentTabPointerPosition(
				IO.MousePos.x, IO.MousePos.y)
				? std::optional<PulseForgeEditor::DocumentTabPointerPosition>(
					PulseForgeEditor::DocumentTabPointerPosition{ IO.MousePos.x, IO.MousePos.y })
				: std::nullopt;
			if (!IO.MouseReleased[ImGuiMouseButton_Left])
			{
				if (m_ActiveDocumentTabDrag && IO.MouseDown[ImGuiMouseButton_Left] && CurrentPosition)
					m_ActiveDocumentTabDragLastPosition = CurrentPosition;
				if (!IO.MouseDown[ImGuiMouseButton_Left] && !ImGui::GetDragDropPayload())
				{
					m_ActiveDocumentTabDrag.reset();
					m_ActiveDocumentTabDragStart.reset();
					m_ActiveDocumentTabDragLastPosition.reset();
				}
				return;
			}

			if (!m_ActiveDocumentTabDrag)
				return;
			// The captured identity remains authoritative when a cross-viewport release clears ImGui's payload.
			const PulseForgeEditor::EditorDocumentIdentity Identity = *m_ActiveDocumentTabDrag;
			m_ActiveDocumentTabDrag.reset();
			const auto LastValidPosition = m_ActiveDocumentTabDragLastPosition;
			const auto ReleasePosition = PulseForgeEditor::ResolveDocumentTabReleasePosition(
				IO.MousePos.x, IO.MousePos.y, LastValidPosition);
			const ImVec2 DragStart = m_ActiveDocumentTabDragStart.value_or(
				ReleasePosition ? ImVec2(ReleasePosition->X, ReleasePosition->Y) : ImVec2(-FLT_MAX, -FLT_MAX));
			m_ActiveDocumentTabDragStart.reset();
			m_ActiveDocumentTabDragLastPosition.reset();
			if (!ReleasePosition || !PulseForgeEditor::IsUsableDocumentTabPointerPosition(DragStart.x, DragStart.y))
				return;
			const ImVec2 MousePosition(ReleasePosition->X, ReleasePosition->Y);
			const auto StartPosition = PulseForgeEditor::DocumentTabPointerPosition{ DragStart.x, DragStart.y };
			const float DragDistanceSquared = PulseForgeEditor::CalculateDocumentTabDragDistanceSquared(
				StartPosition, CurrentPosition, LastValidPosition,
				IO.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left]);

			const bool TransferQueued = m_PendingDocumentTransfer &&
				m_PendingDocumentTransfer->Identity == Identity;
			const auto* Session = m_Documents.Find(Identity);
			const auto* SourceHost = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
			const auto DropTarget = CurrentPosition
				? FindDocumentTabDropTarget(MousePosition, IO.MouseHoveredViewport)
				: std::nullopt;
			const bool SourceIsMainHost = SourceHost &&
				SourceHost->Role == PulseForgeEditor::EditorDocumentHostRole::Main;
			const bool SourceIsNativeHost = SourceHost &&
				SourceHost->Role == PulseForgeEditor::EditorDocumentHostRole::Native;
			const bool CanCreateNativeHost = PulseForgeEditor::CanDetachDocumentTabToNativeHost(
				SourceIsMainHost, SourceIsNativeHost, IO.MouseHoveredViewport == 0);
			const PulseForgeEditor::DocumentTabReleaseAction Action =
				PulseForgeEditor::ResolveDocumentTabReleaseAction(
					true,
					true,
					TransferQueued,
					DragDistanceSquared,
					IO.MouseDragThreshold,
					CanCreateNativeHost,
					Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level,
					DropTarget.has_value());
			if (Action == PulseForgeEditor::DocumentTabReleaseAction::TransferToStrip && DropTarget)
			{
				QueueDocumentTransfer(Identity, DropTarget->first, DropTarget->second);
				return;
			}
			if (Action != PulseForgeEditor::DocumentTabReleaseAction::CreateNativeHost || !Session)
				return;

			QueueDocumentTransfer(Identity, {}, 0, true, MakeDocumentDropWindowBounds(MousePosition));
		}

		[[nodiscard]] std::optional<std::pair<std::string, size_t>> FindDocumentTabDropTarget(
			const ImVec2& Position,
			ImGuiID HoveredViewportID) const
		{
			std::optional<std::pair<std::string, size_t>> Result;
			for (const DocumentTabDropTarget& Target : m_DocumentTabDropTargets)
			{
				if (HoveredViewportID != 0 && Target.ViewportID != HoveredViewportID)
					continue;
				if (Position.x < Target.Minimum.x || Position.x >= Target.Maximum.x ||
					Position.y < Target.Minimum.y || Position.y >= Target.Maximum.y)
					continue;

				if (Result)
					return std::nullopt;
				const float Midpoint = (Target.Minimum.x + Target.Maximum.x) * 0.5f;
				const size_t Index = Target.BeforeIndex != Target.AfterIndex && Position.x >= Midpoint
					? Target.AfterIndex : Target.BeforeIndex;
				Result.emplace(Target.HostID, Index);
			}
			return Result;
		}

		void ProcessPendingDocumentTransfer()
		{
			if (!m_PendingDocumentTransfer)
				return;

			PendingDocumentTransfer Pending = std::move(*m_PendingDocumentTransfer);
			m_PendingDocumentTransfer.reset();
			const auto* Session = m_Documents.Find(Pending.Identity);
			if (!Session)
				return;

			std::string DestinationHostID = std::move(Pending.DestinationHostID);
			bool CreatedHost = false;
			if (Pending.CreateNativeHost)
			{
				const auto* Source = m_Documents.FindHost(Session->HostID);
				if (!Source || Pending.Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
				{
					SetWarning("Only open asset documents can be detached to a new native window.");
					return;
				}

				auto Host = m_Documents.CreateNativeHost(Pending.Identity.Kind);
				if (!Host)
				{
					SetWarning("Could not create a native document host: " + Host.error());
					return;
				}
				DestinationHostID = std::move(*Host);
				CreatedHost = true;
				if (Pending.NativeHostBounds)
				{
					(void)m_Documents.SetHostBounds(DestinationHostID, std::move(*Pending.NativeHostBounds));
					m_RestoredHostBoundsPending.insert(DestinationHostID);
				}
			}

			const auto Moved = m_Documents.Transfer(Pending.Identity, DestinationHostID, Pending.DestinationIndex,
				[](const auto&, const auto&) { return true; });
			if (!Moved)
			{
				if (CreatedHost)
				{
					m_RestoredHostBoundsPending.erase(DestinationHostID);
					(void)m_Documents.RemoveEmptyNativeHost(DestinationHostID);
				}
				SetWarning("Document transfer was rejected: " + Moved.error());
				return;
			}

			if (Moved->Changed)
				MarkDocumentWorkspaceDirty();
			if (Moved->EmptiedSourceHost)
			{
				m_RestoredHostBoundsPending.erase(*Moved->EmptiedSourceHost);
				(void)m_Documents.RemoveEmptyNativeHost(*Moved->EmptiedSourceHost);
			}
			m_DocumentHostFocusRequests.insert(std::move(DestinationHostID));
		}

		void TryTransferDocumentPayload(const ImGuiPayload* Payload, std::string_view DestinationHostID, size_t Index)
		{
			if (!Payload || !Payload->IsDelivery() || !Payload->Data || Payload->DataSize <= 1)
				return;
			const std::string_view Encoded(static_cast<const char*>(Payload->Data),
				static_cast<size_t>(Payload->DataSize - 1));
			std::optional<PulseForgeEditor::EditorDocumentIdentity> Identity;
			for (const auto& [Candidate, Session] : m_Documents.GetSessions())
			{
				(void)Session;
				if (GetDocumentPayload(Candidate) == Encoded)
				{
					Identity = Candidate;
					break;
				}
			}
			if (!Identity)
				return;
			QueueDocumentTransfer(*Identity, std::string(DestinationHostID), Index);
		}

		void DrawDocumentTabStrip(std::string_view HostID)
		{
			const auto* Host = m_Documents.FindHost(HostID);
			if (!Host)
				return;
			const std::vector<PulseForgeEditor::EditorDocumentIdentity> Documents = Host->Documents;
			for (size_t Index = 0; Index < Documents.size(); ++Index)
			{
				const auto* Session = m_Documents.Find(Documents[Index]);
				if (!Session)
					continue;
				ImGui::PushID(GetDocumentPayload(Session->Identity).c_str());
				std::string Label = Session->Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level
					? "Level Editor" : Session->DisplayName;
				if (Session->IsDirty())
					Label += " *";
				const bool HasCloseButton = Session->Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level;
				constexpr float TabHeight = 25.0f;
				constexpr float LeftPadding = 10.0f;
				constexpr float LabelToCloseGap = 1.0f;
				constexpr float CloseRegionWidth = 26.0f;
				constexpr float RightPadding = 2.0f;
				const ImVec2 LabelSize = ImGui::CalcTextSize(Label.c_str());
				const float TabWidth = LeftPadding + LabelSize.x +
					(HasCloseButton ? LabelToCloseGap + CloseRegionWidth + RightPadding : LeftPadding);
				const bool Active = Host->ActiveDocument == Session->Identity;
				const bool Pressed = ImGui::Selectable("##DocumentTab", Active, ImGuiSelectableFlags_None,
					ImVec2(TabWidth, TabHeight));
				const ImVec2 TabMinimum = ImGui::GetItemRectMin();
				const ImVec2 TabMaximum = ImGui::GetItemRectMax();
				const ImGuiViewport* TabViewport = ImGui::GetWindowViewport();
				const ImGuiID TabViewportID = TabViewport ? TabViewport->ID : 0;
				m_DocumentTabDropTargets.push_back(DocumentTabDropTarget{
					std::string(HostID), TabViewportID, TabMinimum, TabMaximum, Index, Index + 1 });
				if (m_DocumentHostTabTearOffTest &&
					Session->Identity == m_DocumentHostTabTearOffTestIdentity)
				{
					m_DocumentHostTabTearOffTestTabCenter = ImVec2(
						(TabMinimum.x + TabMaximum.x) * 0.5f,
						(TabMinimum.y + TabMaximum.y) * 0.5f);
					m_DocumentHostTabTearOffTestTabViewportID = TabViewportID;
					m_DocumentHostTabTearOffTestTabCenterValid = true;
				}
				if (m_DocumentHostTabReattachTest &&
					Session->Identity == m_DocumentHostTabReattachTestIdentity)
				{
					m_DocumentHostTabReattachTestTabCenter = ImVec2(
						(TabMinimum.x + TabMaximum.x) * 0.5f,
						(TabMinimum.y + TabMaximum.y) * 0.5f);
					m_DocumentHostTabReattachTestTabViewportID = TabViewportID;
					m_DocumentHostTabReattachTestTabCenterValid = true;
				}
				if (m_DocumentHostTabReorderTest)
				{
					if (Session->Identity == m_DocumentHostTabReorderTestFirst)
					{
						m_DocumentHostTabReorderTestFirstTabCenter = ImVec2(
							(TabMinimum.x + TabMaximum.x) * 0.5f,
							(TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabReorderTestFirstTabViewportID = TabViewportID;
						m_DocumentHostTabReorderTestFirstTabCenterValid = true;
					}
					else if (Session->Identity == m_DocumentHostTabReorderTestSecond)
					{
						const float TabWidth = TabMaximum.x - TabMinimum.x;
						m_DocumentHostTabReorderTestSecondBeforePosition = ImVec2(
							TabMinimum.x + TabWidth * 0.35f,
							(TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabReorderTestSecondAfterPosition = ImVec2(
							TabMinimum.x + TabWidth * 0.65f,
							(TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabReorderTestSecondBeforePositionValid = true;
						m_DocumentHostTabReorderTestSecondAfterPositionValid = true;
					}
				}
				if (m_DocumentHostTabGroupTest)
				{
					if (Session->Identity == m_DocumentHostTabTearOffTestIdentity)
					{
						m_DocumentHostTabGroupSourceCenter = ImVec2(
							(TabMinimum.x + TabMaximum.x) * 0.5f,
							(TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabGroupSourceViewportID = TabViewportID;
						m_DocumentHostTabGroupSourceCenterValid = true;
					}
					else if (Session->Identity == m_DocumentHostTabGroupTargetIdentity)
					{
						m_DocumentHostTabGroupTargetCenter = ImVec2(
							(TabMinimum.x + TabMaximum.x) * 0.5f,
							(TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabGroupTargetViewportID = TabViewportID;
						m_DocumentHostTabGroupTargetCenterValid = true;
					}
				}
				if (m_DocumentHostTabReattachTest &&
					HostID == PulseForgeEditor::EditorDocumentManager::MainHostID &&
					Session->Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
				{
					m_DocumentHostTabReattachTestMainDropPosition = ImVec2(
						TabMinimum.x + (TabMaximum.x - TabMinimum.x) * 0.75f,
						(TabMinimum.y + TabMaximum.y) * 0.5f);
					m_DocumentHostTabReattachTestMainViewportID = TabViewportID;
					m_DocumentHostTabReattachTestMainDropPositionValid = true;
				}
				const bool TabHovered = ImGui::IsItemHovered();
				const ImVec2 MousePosition = ImGui::GetIO().MousePos;
				const float CloseRegionMinimumX = TabMinimum.x + LeftPadding + LabelSize.x + LabelToCloseGap;
				const ImVec2 CloseButtonMinimum(CloseRegionMinimumX, TabMinimum.y + 1.0f);
				const ImVec2 CloseButtonMaximum(CloseRegionMinimumX + CloseRegionWidth, TabMaximum.y - 1.0f);
				const bool CloseHovered = HasCloseButton && TabHovered &&
					MousePosition.x >= CloseButtonMinimum.x && MousePosition.x < CloseButtonMaximum.x &&
					MousePosition.y >= CloseButtonMinimum.y && MousePosition.y < CloseButtonMaximum.y;
				if (Session->Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level &&
					PulseForgeEditor::ShouldCaptureDocumentTabDrag(
						TabHovered, CloseHovered, ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left]))
				{
					m_ActiveDocumentTabDrag = Session->Identity;
					m_ActiveDocumentTabDragStart = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
					if (PulseForgeEditor::IsUsableDocumentTabPointerPosition(MousePosition.x, MousePosition.y))
						m_ActiveDocumentTabDragLastPosition = PulseForgeEditor::DocumentTabPointerPosition{
							MousePosition.x, MousePosition.y };
				}
				if (Pressed && CloseHovered)
					QueueDocumentTabClose(Session->Identity);
				else if (Pressed)
				{
					(void)m_Documents.Activate(Session->Identity);
					MarkDocumentWorkspaceDirty();
					m_DocumentHostFocusRequests.insert(std::string(HostID));
				}

				ImDrawList* DrawList = ImGui::GetWindowDrawList();
				const ImVec2 LabelPosition(
					TabMinimum.x + LeftPadding,
					TabMinimum.y + (TabHeight - LabelSize.y) * 0.5f);
				DrawList->AddText(LabelPosition, ImGui::GetColorU32(ImGuiCol_Text), Label.c_str());
				if (HasCloseButton)
				{
					const ImVec2 CloseCenter(CloseRegionMinimumX + CloseRegionWidth * 0.5f,
						TabMinimum.y + TabHeight * 0.5f);
					constexpr float CloseHalfExtent = 8.5f;
					const ImU32 CloseColor = ImGui::GetColorU32(
						CloseHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
					DrawList->AddLine(
						ImVec2(CloseCenter.x - CloseHalfExtent, CloseCenter.y - CloseHalfExtent),
						ImVec2(CloseCenter.x + CloseHalfExtent, CloseCenter.y + CloseHalfExtent), CloseColor, 2.0f);
					DrawList->AddLine(
						ImVec2(CloseCenter.x + CloseHalfExtent, CloseCenter.y - CloseHalfExtent),
						ImVec2(CloseCenter.x - CloseHalfExtent, CloseCenter.y + CloseHalfExtent), CloseColor, 2.0f);
					if (CloseHovered)
						ImGui::SetTooltip("Close %s", Session->DisplayName.c_str());
				}

				if (ImGui::BeginPopupContextItem("DocumentTabContext"))
				{
					if (const auto* CurrentHost = m_Documents.FindHost(HostID))
					{
						if (CurrentHost->Role == PulseForgeEditor::EditorDocumentHostRole::Main)
						{
							if (Session->Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level &&
								ImGui::MenuItem("Move to New Native Window"))
								QueueDocumentTransfer(Session->Identity, {}, 0, true);
						}
						else if (ImGui::MenuItem("Move to Main Document Strip"))
							QueueDocumentTransfer(Session->Identity,
								std::string(PulseForgeEditor::EditorDocumentManager::MainHostID), 0);

						if (CurrentHost->Role == PulseForgeEditor::EditorDocumentHostRole::Native &&
							ImGui::BeginMenu("Move to Existing Document Window"))
						{
							for (const auto& CandidateHost : m_Documents.GetHosts())
							{
								if (CandidateHost.Role != PulseForgeEditor::EditorDocumentHostRole::Native ||
									CandidateHost.ID == HostID ||
									CandidateHost.PlacementKind != Session->Identity.Kind)
									continue;
								const bool SameProject = std::all_of(
									CandidateHost.Documents.begin(), CandidateHost.Documents.end(),
									[&Session](const auto& CandidateIdentity)
									{
										return CandidateIdentity.Project == Session->Identity.Project;
									});
								if (!SameProject)
									continue;
								const auto* ActiveSession = CandidateHost.ActiveDocument
									? m_Documents.Find(*CandidateHost.ActiveDocument) : nullptr;
								std::string ItemLabel = ActiveSession ? ActiveSession->DisplayName : CandidateHost.ID;
								ItemLabel += "##" + CandidateHost.ID;
								if (ImGui::MenuItem(ItemLabel.c_str()))
									QueueDocumentTransfer(Session->Identity, CandidateHost.ID,
										CandidateHost.Documents.size());
							}
							ImGui::EndMenu();
						}
					}
					ImGui::EndPopup();
				}
				if (Session->Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level &&
					ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
				{
					m_ActiveDocumentTabDrag = Session->Identity;
					// Preserve the original press coordinate as the drag source starts. If
					// item activation was not observed, using the release position as the
					// fallback would produce a zero-length drag and suppress tear-off.
					const ImVec2 PressPosition = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
					if (std::isfinite(PressPosition.x) && std::isfinite(PressPosition.y) &&
						std::abs(PressPosition.x) < 100000.0f && std::abs(PressPosition.y) < 100000.0f)
						m_ActiveDocumentTabDragStart = PressPosition;
					if (PulseForgeEditor::IsUsableDocumentTabPointerPosition(MousePosition.x, MousePosition.y))
						m_ActiveDocumentTabDragLastPosition = PulseForgeEditor::DocumentTabPointerPosition{
							MousePosition.x, MousePosition.y };
					const std::string Payload = GetDocumentPayload(Session->Identity);
					ImGui::SetDragDropPayload("PF_EDITOR_DOCUMENT", Payload.c_str(), Payload.size() + 1);
					ImGui::TextUnformatted(Label.c_str());
					ImGui::EndDragDropSource();
				}
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("PF_EDITOR_DOCUMENT"))
						TryTransferDocumentPayload(Payload, HostID, Index);
					ImGui::EndDragDropTarget();
				}
				if (TabHovered && !Session->ProjectRelativePath.empty())
					ImGui::SetTooltip("%s", PathToUtf8(Session->ProjectRelativePath).c_str());
				ImGui::SameLine(0.0f, 4.0f);
				ImGui::PopID();
			}

			const float DropWidth = ImGui::GetContentRegionAvail().x;
			if (DropWidth > 24.0f)
			{
				ImGui::InvisibleButton("DocumentStripDropTarget", ImVec2(DropWidth, 25.0f));
				const ImVec2 DropMinimum = ImGui::GetItemRectMin();
				const ImVec2 DropMaximum = ImGui::GetItemRectMax();
				const ImGuiViewport* DropViewport = ImGui::GetWindowViewport();
				m_DocumentTabDropTargets.push_back(DocumentTabDropTarget{
					std::string(HostID), DropViewport ? DropViewport->ID : 0,
					DropMinimum, DropMaximum, Documents.size(), Documents.size() });
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("PF_EDITOR_DOCUMENT"))
						TryTransferDocumentPayload(Payload, HostID, Documents.size());
					ImGui::EndDragDropTarget();
				}
			}
		}

		void DrawMainDocumentStrip()
		{
			if (!BeginEditorChromeBar("##PulseForgeDocumentStrip", ImGuiDir_Up, 34.0f,
				PulseForgeEditor::EditorColorToken::PanelRaised, m_EditorStyle))
				return;
			if (ImGui::GetWindowViewport() && ImGui::GetMainViewport() &&
				ImGui::GetWindowViewport()->ID == ImGui::GetMainViewport()->ID &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
			if (const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				MainHost && !MainHost->Documents.empty())
				DrawDocumentTabStrip(MainHost->ID);
			else
				m_EditorStyle.IconText(PulseForgeEditor::EditorIcon::Scene, "Level Editor");
			EndEditorChromeBar();
		}

		void DrawWorkspacePanels()
		{
		const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
		const std::optional<PulseForgeEditor::EditorDocumentIdentity> MainActive =
			MainHost ? MainHost->ActiveDocument : std::nullopt;
		if (m_WorkspacePresentation.BeginMainFrame(MainActive))
		{
			ResetViewportPanelInteraction();
		}
		const bool LevelWorkspaceActive = m_WorkspacePresentation.IsLevelWorkspaceActive();

			DrawUnsavedChangesDialog();
			DrawDocumentCloseDialog(PulseForgeEditor::EditorDocumentManager::MainHostID);
			if (!LevelWorkspaceActive && MainActive)
			{
				ResetViewportPanelInteraction();
				ImGuiViewport* Viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(Viewport->WorkPos, ImGuiCond_Always);
				ImGui::SetNextWindowSize(Viewport->WorkSize, ImGuiCond_Always);
				bool DrawDocumentWorkspaceContent = false;
				if (ImGui::Begin("##PFMainDocumentWorkspace", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoDocking))
					if (auto* Session = m_Documents.Find(*MainActive))
					{
						if (m_DocumentEditors.IsRegistered(Session->Identity.Kind) && Session->EditorState)
						{
							m_WorkspacePresentation.DrawWorkspaceDockspace(*Session);
							DrawDocumentWorkspaceContent = true;
						}
						else
							DrawUnregisteredAssetDocument(Session->Identity);
					}
				ImGui::End();
				if (DrawDocumentWorkspaceContent)
					if (auto* Session = m_Documents.Find(*MainActive))
						m_WorkspacePresentation.DrawWorkspaceContent(*Session, m_DocumentEditors,
							[](void* Context, const auto& Identity)
							{
								static_cast<EditorLayer*>(Context)->DrawUnregisteredAssetDocument(Identity);
							}, this);
			}
			if (LevelWorkspaceActive)
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
							m_EditorStyle.TextMuted(m_SceneEditController.IsDirty() ? "Unsaved" : "Saved");
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
			DrawSceneViewportPanel();
			DrawHierarchyPanel();
			DrawInspectorPanel();
			#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!m_OpenGLDiagnosticCapturePath.empty() && !m_OpenGLDiagnosticContentBrowserSelected)
				ImGui::SetNextWindowFocus();
			#endif
			DrawContentBrowserPanel();
			if (m_InitialMaterialDetailsDockPending)
			{
				if (m_ForceMaterialDetailsDock)
					(void)m_Layout.DockPanelBeside(PulseForgeEditor::EditorPanel::MaterialDetails,
						PulseForgeEditor::EditorPanel::Inspector);
				else
					(void)m_Layout.DockPanelBesideIfUnplaced(PulseForgeEditor::EditorPanel::MaterialDetails,
						PulseForgeEditor::EditorPanel::Inspector);
				m_InitialMaterialDetailsDockPending = false;
				m_ForceMaterialDetailsDock = false;
			}
			DrawMaterialDetailsPanel();
			DrawDeleteAssetDialog();
			DrawAssetDestinationDialog();
			DrawConsolePanel();
			}
			DrawNativeDocumentHosts();
			m_WorkspacePresentation.KeepInactiveWorkspacesAlive(m_Documents, m_DocumentEditors);
		}

		void CaptureDocumentTabTestInvalidDropTarget(PulseForgeEditor::EditorPanel Panel)
		{
			if (!m_DocumentHostTabReattachTest)
				return;

			size_t PanelIndex = m_DocumentHostTabReattachInvalidDropTargets.size();
			switch (Panel)
			{
				case PulseForgeEditor::EditorPanel::Hierarchy: PanelIndex = 0; break;
				case PulseForgeEditor::EditorPanel::Inspector: PanelIndex = 1; break;
				case PulseForgeEditor::EditorPanel::SceneViewport: PanelIndex = 2; break;
				case PulseForgeEditor::EditorPanel::ContentBrowser: PanelIndex = 3; break;
				case PulseForgeEditor::EditorPanel::Console: PanelIndex = 4; break;
				case PulseForgeEditor::EditorPanel::Scene:
				case PulseForgeEditor::EditorPanel::MaterialDetails:
				case PulseForgeEditor::EditorPanel::Count:
					return;
			}
			if (PanelIndex >= DocumentTabTestInvalidDropPanelNames.size())
				return;

			ImGuiViewport* Viewport = ImGui::GetWindowViewport();
			const ImVec2 Position = ImGui::GetWindowPos();
			const ImVec2 Size = ImGui::GetWindowSize();
			if (!Viewport || Size.x < 100.0f || Size.y < 100.0f)
				return;

			const ImVec2 RegionMinimum = ImGui::GetWindowContentRegionMin();
			const ImVec2 RegionMaximum = ImGui::GetWindowContentRegionMax();
			const float MinimumX = Position.x + RegionMinimum.x;
			const float MinimumY = Position.y + RegionMinimum.y;
			const float MaximumX = Position.x + RegionMaximum.x;
			const float MaximumY = Position.y + RegionMaximum.y;
			if (MaximumX - MinimumX < 12.0f || MaximumY - MinimumY < 12.0f)
				return;

			const float CenterX = (MinimumX + MaximumX) * 0.5f;
			const float CenterY = (MinimumY + MaximumY) * 0.5f;
			constexpr float EdgeInset = 3.0f;
			const std::array<ImVec2, DocumentTabTestDropPointsPerPanel> Positions = {
				ImVec2(CenterX, CenterY),
				ImVec2(CenterX, MinimumY + EdgeInset),
				ImVec2(MinimumX + EdgeInset, CenterY),
				ImVec2(MaximumX - EdgeInset, CenterY),
				ImVec2(CenterX, MaximumY - EdgeInset)
			};
			const size_t FirstTargetIndex = PanelIndex * DocumentTabTestDropPointsPerPanel;
			for (size_t PointIndex = 0; PointIndex < Positions.size(); ++PointIndex)
			{
				auto& Target = m_DocumentHostTabReattachInvalidDropTargets[FirstTargetIndex + PointIndex];
				Target.Name = DocumentTabTestInvalidDropTargetNames[FirstTargetIndex + PointIndex];
				Target.Position = Positions[PointIndex];
				Target.ViewportID = Viewport->ID;
				Target.Available = true;
			}
		}

		bool BeginEditorPanel(PulseForgeEditor::EditorPanel Panel, std::string_view DisplayTitle = {})
		{
			bool* IsOpen = m_Layout.GetPanelVisibility(Panel);
			if (!IsOpen || !*IsOpen)
				return false;

			const bool WasOpen = *IsOpen;
			const auto& Descriptor = PulseForgeEditor::EditorPanelDescriptors[static_cast<size_t>(Panel)];
			const std::string WindowName = DisplayTitle.empty()
				? std::string(Descriptor.WindowName)
				: std::string(DisplayTitle) + "###" + std::string(Descriptor.WindowName);
			const bool ContentsVisible = ImGui::Begin(WindowName.c_str(), IsOpen);
			if (ContentsVisible)
				CaptureDocumentTabTestInvalidDropTarget(Panel);
			if (ImGui::GetWindowViewport() && ImGui::GetMainViewport() &&
				ImGui::GetWindowViewport()->ID == ImGui::GetMainViewport()->ID &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
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
			if (Panel == PulseForgeEditor::EditorPanel::MaterialDetails && Visible)
			{
				m_InitialMaterialDetailsDockPending = true;
				m_FocusMaterialDetailsNextFrame = true;
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

			if (SelectedEntity() && !m_Scene->FindEntity(*SelectedEntity()))
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
				if (SelectedEntity())
				{
					if (const auto Selected = m_Scene->FindEntity(*SelectedEntity()))
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
			if (SelectedEntity() && m_ViewportTool != PulseForgeEditor::ViewportTool::Select)
			{
				if (const auto Selected = m_Scene->FindEntity(*SelectedEntity()))
					if (IsEntityEffectivelyVisible(*Selected))
						Geometry = BuildViewportGizmoGeometry(*Selected);
			}

			const std::optional<GizmoHandle> HoveredHandle = Geometry && m_ViewportImageHovered
				? FindGizmoHandle(*Geometry, Mouse)
				: std::nullopt;
			if (Geometry)
			{
				const auto Selected = m_Scene->FindEntity(*SelectedEntity());
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
			if (!m_Scene || !SelectedEntity() || m_SceneRuntime || m_EditorCameraNavigationActive ||
				Handle.Axis >= Geometry.Axes.size() || !Geometry.Axes[Handle.Axis].IsValid || !m_ViewportViewProjectionValid)
				return false;
			const auto Entity = m_Scene->FindEntity(*SelectedEntity());
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
			Drag.Entity = *SelectedEntity();
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
			if (!m_Scene || m_Scene.get() != Drag.SceneIdentity || m_SceneRuntime || !SelectedEntity())
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
				Drag.Entity = *SelectedEntity();
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
				m_SceneEditController.MarkDirty();
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
			m_SceneEditController.MarkDirty();
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
					m_SceneWorkspace.Selection().Add(Hit->Entity);
				}
				else
					SelectSingleEntity(Hit->Entity);
			}
			else if (!Control && !Shift)
				SelectSingleEntity(std::nullopt);
		}

		void SnapSelectionToSurface()
		{
			if (!m_Project || !m_Scene || m_SceneRuntime || m_SceneWorkspace.Selection().GetEntities().empty())
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
			if (!m_Scene || m_SceneRuntime || m_SceneWorkspace.Selection().GetEntities().empty())
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
			{
				const PulseForgeEditor::ViewportCameraMoveKeys Keys{
					.Forward = Input.IsKeyPressed(GLFW_KEY_W),
					.Backward = Input.IsKeyPressed(GLFW_KEY_S),
					.Left = Input.IsKeyPressed(GLFW_KEY_A),
					.Right = Input.IsKeyPressed(GLFW_KEY_D),
					.Down = Input.IsKeyPressed(GLFW_KEY_Q),
					.Up = Input.IsKeyPressed(GLFW_KEY_E),
					.Fast = Input.IsKeyPressed(GLFW_KEY_LEFT_SHIFT) || Input.IsKeyPressed(GLFW_KEY_RIGHT_SHIFT)
				};
				m_EditorCamera.Move(Keys, DeltaTime.GetSeconds());
			}
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

		void DrawConsolePanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Console))
				return;
			if (m_DocumentHostTabReattachTest &&
				m_DocumentHostTabReattachInvalidDropIndex >=
					(DocumentTabTestDropPointsPerPanel * DocumentTabTestConsolePanelIndex) &&
				m_DocumentHostTabReattachInvalidDropIndex <
					(DocumentTabTestDropPointsPerPanel * DocumentTabTestInvalidDropPanelNames.size()))
				ImGui::SetNextWindowFocus();
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Console))
				m_ConsolePanel.DrawContents(m_EditorStyle);
			ImGui::End();
		}
		void DrawHierarchyPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy))
				return;
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Hierarchy))
			{
				m_SceneWorkspace.BeginHierarchyFrame();
				if (m_EditorStyle.BeginToolbar("HierarchyToolbar", 44.0f))
				{
					ImGui::BeginDisabled(!m_Scene);
					if (m_EditorStyle.IconButton(
						PulseForgeEditor::EditorIcon::Add, "Create", "CreateEntity", "Create a new entity", false, 38.0f, 34.0f))
						CreateEntityFromEditor();
					ImGui::SameLine();
					ImGui::BeginDisabled(!SelectedEntity());
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
				m_SceneWorkspace.CompleteHierarchyFrame();
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
			if (!m_Scene || m_SceneWorkspace.Selection().GetEntities().empty())
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
			if (!m_Scene || m_SceneWorkspace.Selection().GetEntities().empty())
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
					m_SceneWorkspace.Visibility().Remove(Entity->GetUUID());
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
			if (!m_Project || !m_Scene || m_SceneWorkspace.Selection().GetEntities().empty())
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
			if (m_SceneWorkspace.Selection().GetEntities().empty())
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
			m_SceneWorkspace.RecordVisibleHierarchyEntity(Entity.GetUUID());
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
			const bool LocallyVisible = m_SceneWorkspace.Visibility().IsLocallyVisible(Entity.GetUUID());
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

				const std::optional<PulseForge::UUID> ParentIdentifier = SelectedEntity();
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
					m_SceneEditController.IsActive(), m_SceneEditController.InspectorHistoryItem(), GImGui ? GImGui->ActiveId : 0))
					FinishSceneEdit();
				if (!m_Scene || !SelectedEntity())
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Entity, "Select an entity in the Hierarchy.");
				else if (m_SceneWorkspace.Selection().GetEntities().size() > 1)
				{
					ImGui::Text("%zu entities selected", m_SceneWorkspace.Selection().GetEntities().size());
					ImGui::Spacing();
					m_EditorStyle.TextMuted("The Inspector shows the active entity. Multi-object property editing is not available yet.");
				}
				else if (const auto Entity = m_Scene->FindEntity(*SelectedEntity()))
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

			const bool Activated = ImGui::IsItemActivated();
			if (Activated || m_AssetSelectorSearchOwner != Label)
			{
				m_AssetSelectorSearch.fill('\0');
				m_AssetSelectorSearchOwner = Label;
			}
			if (Activated)
				ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##AssetSelectorSearch", "Search compatible assets by name or folder...",
				m_AssetSelectorSearch.data(), m_AssetSelectorSearch.size());

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
			bool HasVisibleCandidates = false;
			if (m_Project)
			{
				for (const PulseForge::AssetRecord& Asset : m_ContentBrowser.Assets())
				{
					if (!IsAssetExtension(Asset, Extensions))
						continue;
					HasCandidates = true;
					const std::string Identifier = Asset.ID.ToString();
					const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
					if (!PulseForgeEditor::AssetSelectorMatchesSearch(Path, m_AssetSelectorSearch.data()))
						continue;
					HasVisibleCandidates = true;
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
			else if (!HasVisibleCandidates)
				ImGui::TextDisabled("No compatible assets match this search.");

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
					for (const PulseForge::AssetRecord& Asset : m_ContentBrowser.Assets())
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
					for (const PulseForge::AssetRecord& Asset : m_ContentBrowser.Assets())
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
					for (const PulseForge::AssetRecord& Asset : m_ContentBrowser.Assets())
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
					for (const PulseForge::AssetRecord& Asset : m_ContentBrowser.Assets())
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
			m_ContentBrowser.ResetNavigation();
		}

		bool IsKnownContentFolder(const std::filesystem::path& Folder) const
		{
			return m_ContentBrowser.IsKnownFolder(Folder);
		}

		void DrawContentFolderNode(
			const ContentFolderNode& Node,
			std::function<void()>& DeferredAction,
			bool IsRoot = false)
		{
			if (IsRoot)
				ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (Node.RelativePath == m_ContentBrowser.CurrentFolder())
				Flags |= ImGuiTreeNodeFlags_Selected;
			if (Node.Children.empty())
				Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			const std::string StableID = PathToUtf8(Node.RelativePath);
			const bool IsOpen = m_EditorStyle.TreeNode(
				Node.RelativePath == m_ContentBrowser.CurrentFolder()
					? PulseForgeEditor::EditorIcon::FolderOpen
					: PulseForgeEditor::EditorIcon::Folder,
				Node.Name,
				StableID,
				Flags);
			if (ImGui::IsItemClicked())
				m_ContentBrowser.SetCurrentFolder(Node.RelativePath);
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
			for (auto Component = m_ContentBrowser.CurrentFolder().begin(); Component != m_ContentBrowser.CurrentFolder().end(); ++Component)
			{
				if (*Component == "Assets")
					continue;
				Breadcrumb /= *Component;
				Breadcrumbs.emplace_back(Breadcrumb, PathToUtf8(*Component));
			}

			if (ImGui::SmallButton("Assets##BreadcrumbRoot"))
				m_ContentBrowser.SetCurrentFolder("Assets");
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
						if (ImGui::Selectable(FullPath.c_str(), Path == m_ContentBrowser.CurrentFolder()))
							m_ContentBrowser.SetCurrentFolder(Path);
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
				m_ContentBrowser.SetCurrentFolder(Breadcrumbs.back().first);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", PathToUtf8(m_ContentBrowser.CurrentFolder()).c_str());
		}

		void HandleAssetBrowserItem(
			const PulseForge::AssetRecord& Asset,
			const AssetBrowserPresentation& Presentation,
			bool Activated,
			std::function<void()>& DeferredAction)
		{
			if (Activated)
			{
				m_ContentBrowser.SelectedAsset() = Asset.ID;
				switch (PulseForgeEditor::ResolveAssetBrowserOpenAction(
					Presentation.Extension,
					Activated,
					ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
				{
					case PulseForgeEditor::AssetBrowserOpenAction::OpenScene:
						OpenScene(Asset.ID);
						break;
					case PulseForgeEditor::AssetBrowserOpenAction::ShowMaterialDetails:
					{
						const PulseForge::AssetID Identifier = Asset.ID;
						DeferredAction = [this, Identifier] { ShowMaterialDetails(Identifier); };
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

			m_ContentBrowser.SelectedAsset() = Asset.ID;
			const PulseForge::AssetID Identifier = Asset.ID;
			if (Presentation.Extension == ".material" && m_EditorStyle.MenuItem(
				PulseForgeEditor::EditorIcon::Material, "Show Material Details"))
				DeferredAction = [this, Identifier] { ShowMaterialDetails(Identifier); };
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

		void ShowMaterialDetails(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project)
				return;

			const auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			if (!Asset || NormalizedExtension(Asset->ProjectRelativePath) != ".material")
			{
				SetError("The selected material is no longer registered.");
				return;
			}

			m_MaterialDetailsAsset = Identifier;
			MarkDocumentWorkspaceDirty();
			m_ContentBrowser.SelectedAsset() = Identifier;
			SetEditorPanelVisible(PulseForgeEditor::EditorPanel::MaterialDetails, true);
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
				const bool IsSelected = m_ContentBrowser.SelectedFolder() &&
					m_ContentBrowser.SelectedFolder()->lexically_normal() == Folder.RelativePath.lexically_normal();
				const bool Activated = m_EditorStyle.Selectable(PulseForgeEditor::EditorIcon::Folder,
					Folder.Name, IsSelected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns);
				if (Activated)
				{
					m_ContentBrowser.SelectedFolder() = Folder.RelativePath;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						m_ContentBrowser.SetCurrentFolder(Folder.RelativePath);
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
						const bool IsSelected = m_ContentBrowser.SelectedFolder() &&
							m_ContentBrowser.SelectedFolder()->lexically_normal() == Folder.RelativePath.lexically_normal();
						const bool Activated = m_EditorStyle.AssetTile(PulseForgeEditor::EditorIcon::Folder,
							Folder.Name, IsSelected,
							ImVec2((std::max)(ImGui::GetContentRegionAvail().x - 2.0f, 1.0f), TileHeight),
							ImGuiSelectableFlags_AllowDoubleClick);
						if (Activated)
						{
							m_ContentBrowser.SelectedFolder() = Folder.RelativePath;
							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
								m_ContentBrowser.SetCurrentFolder(Folder.RelativePath);
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
			if (m_ContentBrowser.FilteredAssetIndices().empty())
				return;

			if (!ImGui::BeginTable("ManagedAssetList", 2,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings))
				return;
			ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f);
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(m_ContentBrowser.FilteredAssetIndices().size()));
			while (Clipper.Step())
			{
				for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
				{
					const size_t AssetIndex = m_ContentBrowser.FilteredAssetIndices()[static_cast<size_t>(Row)];
					const PulseForge::AssetRecord& Asset = m_ContentBrowser.Assets()[AssetIndex];
					const AssetBrowserPresentation& Presentation = m_AssetBrowserPresentations[AssetIndex];
					ImGui::PushID(Presentation.Identifier.c_str());
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					const bool IsSelected = m_ContentBrowser.SelectedAsset() && *m_ContentBrowser.SelectedAsset() == Asset.ID;
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
			if (m_ContentBrowser.FilteredAssetIndices().empty())
				return;

			const float AvailableWidth = ImGui::GetContentRegionAvail().x;
			const float DesiredTileWidth = 124.0f;
			const float Spacing = ImGui::GetStyle().ItemSpacing.x;
			const int ColumnCount = (std::max)(1, static_cast<int>((AvailableWidth + Spacing) /
				(DesiredTileWidth + Spacing)));
			const int RowCount = static_cast<int>((m_ContentBrowser.FilteredAssetIndices().size() +
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
						if (AssetIndex >= m_ContentBrowser.FilteredAssetIndices().size())
							break;
						ImGui::TableSetColumnIndex(Column);
						const size_t SourceIndex = m_ContentBrowser.FilteredAssetIndices()[AssetIndex];
						const PulseForge::AssetRecord& Asset = m_ContentBrowser.Assets()[SourceIndex];
						const AssetBrowserPresentation& Presentation = m_AssetBrowserPresentations[SourceIndex];
						ImGui::PushID(Presentation.Identifier.c_str());
						const bool IsSelected = m_ContentBrowser.SelectedAsset() && *m_ContentBrowser.SelectedAsset() == Asset.ID;
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
			if (!m_ContentBrowser.SelectedAsset() || !m_Project)
				return;
			const auto Asset = m_Project->GetAssetRegistry().Find(*m_ContentBrowser.SelectedAsset());
			if (!Asset)
			{
				m_ContentBrowser.SelectedAsset().reset();
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

		void ClearMaterialDraftsForProject()
		{
			if (!m_Project)
				return;
			const std::string Prefix = PathToUtf8(m_Project->GetRootPath().lexically_normal()) + ":";
			m_MaterialDraftStore.ClearProject(Prefix);
		}

		void FinalizePendingMaterialEdits()
		{
			m_MaterialDraftStore.FinalizePendingEdits(ImGui::IsAnyItemActive());
		}

		bool ApplyMaterialHistoryStep(bool Redo)
		{
			const auto Asset = GetFocusedMaterialAsset();
			if (!Asset || !m_Project)
				return false;
			const std::string Key = GetMaterialDraftKey(*Asset);
			const auto Changed = m_MaterialDraftStore.ApplyHistoryStep(Key, Redo);
			if (!Changed)
				return false;
			if (!*Changed)
				return true;

			const PulseForgeEditor::EditorDocumentIdentity Identity{
				m_Project->GetDescription().ID, *Asset, PulseForgeEditor::EditorDocumentKind::Material
			};
			if (m_Documents.Find(Identity))
			{
				if (!m_Documents.MarkEdited(Identity))
					return false;
				if (!m_MaterialDraftStore.IsDirty(Key))
					if (auto* Session = m_Documents.Find(Identity))
						Session->SavedRevision = Session->Revision;
			}
			SetStatus(Redo ? "Redid material edit." : "Undid material edit.");
			return true;
		}

		void ForgetMaterialDraft(const PulseForge::AssetID& Asset)
		{
			if (m_Project)
				m_MaterialDraftStore.Forget(GetMaterialDraftKey(Asset));
		}
		bool DrawMaterialAssetProperties(const PulseForge::AssetRecord& Asset)
		{
			const std::string Key = GetMaterialDraftKey(Asset.ID);
			if (const std::string* Error = m_MaterialDraftStore.FindError(Key))
			{
				ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error),
					"Material could not be loaded: %s", Error->c_str());
				if (ImGui::SmallButton("Retry Material Load"))
					m_MaterialDraftStore.ClearError(Key);
				return false;
			}

			PulseForge::MaterialAssetDesc* Draft = m_MaterialDraftStore.FindDraft(Key);
			if (!Draft)
			{
				auto Loaded = PulseForge::MaterialAssetService::Load(
					Asset.ID, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
				if (!Loaded)
				{
					m_MaterialDraftStore.SetError(Key, Loaded.error().Message);
					ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error),
						"Material could not be loaded: %s", Loaded.error().Message.c_str());
					if (ImGui::SmallButton("Retry Material Load"))
						m_MaterialDraftStore.ClearError(Key);
					return false;
				}
				Draft = &m_MaterialDraftStore.PutDraft(Key, std::move(*Loaded));
			}

			const PulseForge::MaterialAssetDesc MaterialBeforeEdit = *Draft;
			PulseForge::MaterialAssetDesc& Material = *Draft;
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
			{
				const bool Added = m_MaterialDraftStore.BeginEdit(Key, MaterialBeforeEdit);
				if (Added && m_Project)
				{
					const PulseForgeEditor::EditorDocumentIdentity Identity{
						m_Project->GetDescription().ID, Asset.ID, PulseForgeEditor::EditorDocumentKind::Material };
					if (m_Documents.Find(Identity))
						(void)m_Documents.MarkEdited(Identity);
				}
			}
			return true;
		}

		void DrawMaterialEditorActions(const PulseForge::AssetRecord& Asset)
		{
			const std::string Key = GetMaterialDraftKey(Asset.ID);
			const bool HasDraft = m_MaterialDraftStore.ContainsDraft(Key);
			const bool IsDirty = m_MaterialDraftStore.IsDirty(Key);
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
				(void)SaveMaterialDraft(Asset.ID);
			if (StackButtons)
			{
				if (ImGui::Button("Revert Material", ImVec2(-1.0f, 0.0f)))
					(void)ReloadMaterialDraft(Asset.ID);
			}
			else
			{
				ImGui::SameLine();
				if (ImGui::Button("Revert Material", ImVec2(ButtonWidth, 0.0f)))
					(void)ReloadMaterialDraft(Asset.ID);
			}
			ImGui::EndDisabled();
		}

		void DrawMaterialDetailsPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::MaterialDetails))
				return;
			const bool FocusMaterialDetails = m_FocusMaterialDetailsNextFrame;
			if (FocusMaterialDetails)
				ImGui::SetNextWindowFocus();
			const bool ContentsVisible = BeginEditorPanel(PulseForgeEditor::EditorPanel::MaterialDetails,
				"Material Details###Material Editor");
			if (FocusMaterialDetails)
			{
				const bool Activated = m_Layout.ActivatePanel(PulseForgeEditor::EditorPanel::MaterialDetails);
				const ImGuiWindow* Window = ImGui::GetCurrentWindow();
				const ImGuiDockNode* Node = Window->DockNode;
				const bool IsActive = Node && Node->TabBar
					? Node->SelectedTabId == Window->TabId
					: ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				m_FocusMaterialDetailsNextFrame = !Activated || !IsActive;
			}
			if (ContentsVisible)
			{
				if (!m_Project)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Material,
						"Open a project to inspect a material.");
				else if (!m_MaterialDetailsAsset)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Material,
						"Show Material Details from a material in the Content Browser.");
				else
				{
					const auto Asset = m_Project->GetAssetRegistry().Find(*m_MaterialDetailsAsset);
					if (!Asset || NormalizedExtension(Asset->ProjectRelativePath) != ".material")
					{
						m_MaterialDetailsAsset.reset();
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Warning,
							"The material is no longer registered. Refresh the Content Browser and select it again.");
					}
					else
					{
						const std::string Name = PathToUtf8(Asset->ProjectRelativePath.filename());
						const std::string FullPath = PathToUtf8(Asset->ProjectRelativePath);
						const std::string Identifier = Asset->ID.ToString();
						m_EditorStyle.IconText(PulseForgeEditor::EditorIcon::Material, Name);
						if (m_MaterialDraftStore.IsDirty(GetMaterialDraftKey(Asset->ID)))
						{
							ImGui::SameLine();
							ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning),
								"Unsaved");
						}
						ImGui::TextWrapped("%s", FullPath.c_str());
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s\nUUID: %s", FullPath.c_str(), Identifier.c_str());
						ImGui::Separator();
						const bool StackActions = ImGui::GetContentRegionAvail().x < 320.0f;
						const float FooterHeight = ImGui::GetFrameHeightWithSpacing() *
							(StackActions ? 3.0f : 2.0f) + 8.0f;
						const float PropertiesHeight = (std::max)(
							ImGui::GetContentRegionAvail().y - FooterHeight, 0.0f);
						if (ImGui::BeginChild("MaterialDetailsProperties", ImVec2(0.0f, PropertiesHeight),
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
			// The default layout stacks Content Browser and Console; select each while its drop targets are tested.
			if (m_DocumentHostTabReattachTest &&
				m_DocumentHostTabReattachInvalidDropIndex <
					(DocumentTabTestDropPointsPerPanel * DocumentTabTestConsolePanelIndex))
				ImGui::SetNextWindowFocus();
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
						DrawContentFolderNode(m_ContentBrowser.FolderRoot(), DeferredAction, true);
						ImGui::EndChild();
						ImGui::SameLine(0.0f, 6.0f);
					}

					ImGui::BeginChild("ContentAssetArea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
					if (ShowFolderTree)
						DrawContentBreadcrumbs();
					else
					{
						const std::string CurrentFolder = PathToUtf8(m_ContentBrowser.CurrentFolder());
						if (ImGui::BeginCombo("##ContentFolder", CurrentFolder.c_str()))
						{
							const auto DrawFolderOptions = [this](auto&& Self, const ContentFolderNode& Node) -> void
							{
								const std::string Path = PathToUtf8(Node.RelativePath);
								if (ImGui::Selectable(Path.c_str(), Node.RelativePath == m_ContentBrowser.CurrentFolder()))
									m_ContentBrowser.SetCurrentFolder(Node.RelativePath);
								for (const ContentFolderNode& Child : Node.Children)
									Self(Self, Child);
							};
							DrawFolderOptions(DrawFolderOptions, m_ContentBrowser.FolderRoot());
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
						ImGui::InputTextWithHint("##AssetSearch", "Search assets...", &m_ContentBrowser.SearchText());
						ImGui::SameLine();
						if (m_EditorStyle.ToolIconButton(PulseForgeEditor::EditorIcon::Grid, "Grid", "AssetGridMode",
							"Grid view", m_ContentBrowser.GridView()))
							m_ContentBrowser.GridView() = true;
						ImGui::SameLine();
						if (m_EditorStyle.ToolIconButton(PulseForgeEditor::EditorIcon::List, "List", "AssetListMode",
							"List view", !m_ContentBrowser.GridView()))
							m_ContentBrowser.GridView() = false;
					}
					m_EditorStyle.EndToolbar();

					m_ContentBrowser.RebuildFilteredAssetIndices();

					const ContentFolderNode* CurrentFolderNode =
						m_ContentBrowser.FindFolder(m_ContentBrowser.CurrentFolder());
					const bool ShowFolderTiles = m_ContentBrowser.SearchText().empty() && CurrentFolderNode &&
						!CurrentFolderNode->Children.empty();
					const std::vector<ContentFolderNode>* VisibleFolders = ShowFolderTiles
						? &CurrentFolderNode->Children : nullptr;
					bool SelectedAssetVisible = false;
					if (m_ContentBrowser.SelectedAsset())
						SelectedAssetVisible = std::any_of(m_ContentBrowser.FilteredAssetIndices().begin(), m_ContentBrowser.FilteredAssetIndices().end(),
							[this](size_t Index) { return m_ContentBrowser.Assets()[Index].ID == *m_ContentBrowser.SelectedAsset(); });

					bool SelectedMaterial = false;
					if (SelectedAssetVisible && m_ContentBrowser.SelectedAsset())
					{
						const auto SelectedRecord = m_Project->GetAssetRegistry().Find(*m_ContentBrowser.SelectedAsset());
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
						if (m_ContentBrowser.GridView())
							DrawFolderGrid(*VisibleFolders, DeferredAction);
						else
							DrawFolderList(*VisibleFolders, DeferredAction);
						if (!m_ContentBrowser.FilteredAssetIndices().empty())
							ImGui::Separator();
					}
					if (m_ContentBrowser.GridView())
						DrawAssetGrid(DeferredAction);
					else
						DrawAssetList(DeferredAction);
					if ((!VisibleFolders || VisibleFolders->empty()) && m_ContentBrowser.FilteredAssetIndices().empty())
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Search,
							m_ContentBrowser.Assets().empty() ? "The project has no managed assets." :
							"No assets or folders match this location.");
					ImGui::EndChild();
					const bool HasDeferredAction = static_cast<bool>(DeferredAction);
					if (DeferredAction)
						DeferredAction();
					if (!HasDeferredAction && SelectedAssetVisible && VerticalLayout.SelectionSummaryHeight > 0.0f)
					{
						const auto SelectedRecord = m_Project->GetAssetRegistry().Find(*m_ContentBrowser.SelectedAsset());
						std::string SelectedName = SelectedRecord
							? PathToUtf8(SelectedRecord->ProjectRelativePath.filename()) : "Selected asset";
						if (SelectedMaterial && m_MaterialDraftStore.IsDirty(GetMaterialDraftKey(*m_ContentBrowser.SelectedAsset())))
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
							if (ImGui::Button("Open Material Details", ImVec2(ActionWidth, 0.0f)))
								ShowMaterialDetails(*m_ContentBrowser.SelectedAsset());
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
			m_ContentBrowser.SetAssets(m_Project
				? m_Project->GetAssetRegistry().GetAssets()
				: std::vector<PulseForge::AssetRecord>{});
			SynchronizeOpenDocumentAssetLocations();

			m_AssetBrowserPresentations.clear();
			m_AssetBrowserPresentations.reserve(m_ContentBrowser.Assets().size());
			for (const PulseForge::AssetRecord& Asset : m_ContentBrowser.Assets())
			{
				m_AssetBrowserPresentations.push_back({
					Asset.ID.ToString(),
					PathToUtf8(Asset.ProjectRelativePath.filename()),
					PathToUtf8(Asset.ProjectRelativePath),
					NormalizedExtension(Asset.ProjectRelativePath),
					PulseForgeEditor::GetAssetBrowserIcon(Asset.ProjectRelativePath) });
			}
		}

		void SynchronizeOpenDocumentAssetLocations()
		{
			if (!m_Project)
				return;

			const PulseForge::UUID ProjectID = m_Project->GetDescription().ID;
			for (const auto& [Identity, ExistingSession] : m_Documents.GetSessions())
			{
				(void)ExistingSession;
				if (Identity.Project != ProjectID || Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
					continue;

				const auto Asset = m_Project->GetAssetRegistry().Find(Identity.Asset);
				if (!Asset)
					continue;

				const std::string DisplayName = PathToUtf8(Asset->ProjectRelativePath.filename());
				auto* Session = m_Documents.Find(Identity);
				if (!Session || (Session->ProjectRelativePath == Asset->ProjectRelativePath &&
					Session->DisplayName == DisplayName))
					continue;
				if (m_DocumentEditors.IsRegistered(Identity.Kind) && Session->EditorState)
				{
					if (!m_DocumentEditors.UpdateAssetLocation(
						*Session, Asset->ProjectRelativePath, DisplayName))
					{
						SetWarning("The document editor rejected its moved asset location; the session now follows the registered asset path.");
						(void)m_Documents.UpdateAssetLocation(
							Identity, Asset->ProjectRelativePath, DisplayName);
					}
				}
				else
					(void)m_Documents.UpdateAssetLocation(
						Identity, Asset->ProjectRelativePath, DisplayName);
			}
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
			m_ContentBrowser.SelectedAsset().reset();
			m_AssetWorkflow.DismissDelete();
			ResetSceneHistory();
		}

		void ShutdownImGui() noexcept
		{
			if (!m_Context)
				return;

			ImGui::SetCurrentContext(m_Context);
			if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
				ImGui::DestroyPlatformWindows();
			if (m_Project)
			{
				m_DocumentWorkspaceImmediateDirty = true;
				SaveDocumentWorkspaceIfNeeded();
			}
			#ifndef PF_EDITOR_RENDERER_OPENGL
			ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
			PlatformIO.Renderer_CreateWindow = nullptr;
			PlatformIO.Renderer_DestroyWindow = nullptr;
			PlatformIO.Renderer_RenderWindow = nullptr;
			PlatformIO.Renderer_SwapBuffers = nullptr;
			ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
			#endif
			m_AssetPreviews.Shutdown();
			WriteAssetPreviewStressReport();
			if (m_ImGuiRenderer)
			{
				m_ImGuiRenderer->Shutdown();
				m_ImGuiRenderer.reset();
			}
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
			ImGui::GetIO().UserData = m_PreviousImGuiUserData;
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
		void* m_PreviousImGuiUserData = nullptr;
		PulseForgeEditor::EditorLayout m_Layout;
		PulseForgeEditor::EditorDocumentManager m_Documents;
		PulseForgeEditor::EditorDocumentEditorRegistry m_DocumentEditors;
		PulseForgeEditor::EditorWorkspacePresentation m_WorkspacePresentation;
		PulseForgeEditor::EditorDeferredOperationQueue m_DeferredEditorOperations;
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
		PulseForgeEditor::EditorSceneEditController m_SceneEditController;
		PulseForgeEditor::EditorSceneWorkspaceState m_SceneWorkspace;
		bool m_FocusEntityNameNextFrame = false;
		bool m_FocusHierarchySearchNextFrame = false;
		bool m_FocusContentSearchNextFrame = false;
		bool m_OpenUnsavedDialog = false;
		bool m_OpenDocumentCloseDialog = false;
		bool m_ApplicationShutdownRequested = false;
		bool m_PendingCloseAllDocuments = false;
		PulseForgeEditor::EditorAssetWorkflowState m_AssetWorkflow;
		PulseForgeEditor::EditorConsolePanel m_ConsolePanel;
		PulseForgeEditor::EditorContentBrowserState m_ContentBrowser;
		bool m_ContentDetailsExpandedCompact = false;
		bool m_InitialMaterialDetailsDockPending = false;
		bool m_ForceMaterialDetailsDock = false;
		bool m_FocusMaterialDetailsNextFrame = false;
		std::function<void()> m_PendingAction;
		std::function<void()> m_PendingDocumentCloseAction;
		std::array<char, 256> m_AssetSelectorSearch{};
		std::string m_AssetSelectorSearchOwner;
		std::optional<PulseForgeEditor::EditorDocumentIdentity> m_LevelDocumentIdentity;
		std::optional<PulseForge::AssetID> m_MaterialDetailsAsset;
		std::optional<PulseForgeEditor::EditorDocumentIdentity> m_PendingDocumentClose;
		std::optional<PulseForgeEditor::EditorDocumentIdentity> m_PendingDocumentTabClose;
		std::optional<std::string> m_PendingDocumentHostClose;
		std::vector<PulseForgeEditor::EditorDocumentIdentity> m_PendingDocumentCloseBatch;
		std::string m_DocumentCloseError;
		std::optional<PendingDocumentTransfer> m_PendingDocumentTransfer;
		std::optional<PulseForgeEditor::EditorDocumentIdentity> m_ActiveDocumentTabDrag;
		std::optional<ImVec2> m_ActiveDocumentTabDragStart;
		std::optional<PulseForgeEditor::DocumentTabPointerPosition> m_ActiveDocumentTabDragLastPosition;
		std::vector<DocumentTabDropTarget> m_DocumentTabDropTargets;
		std::string m_DocumentCloseDialogHostID;
		std::unordered_set<std::string> m_DocumentHostFocusRequests;
		std::optional<std::filesystem::path> m_StartupProjectPath;
		std::optional<std::filesystem::path> m_ProjectOpenRenderTestFile;
		bool m_ProjectOpenRenderTestRequested = false;
		bool m_DocumentHostTestFixture = false;
		bool m_DocumentHostResetLayoutTest = false;
		bool m_DocumentHostTabTearOffTest = false;
		bool m_DocumentHostTabTearOffFromNativeHost = false;
		bool m_DocumentHostTabReattachTest = false;
		size_t m_DocumentHostTabTearOffTestFrameCount = 0;
		bool m_DocumentHostTabRoundTripTearOffPending = false;
		size_t m_DocumentHostTabRoundTripCount = 0;
		bool m_DocumentHostTabReorderTest = false;
		bool m_DocumentWorkspaceSwitchTest = false;
		bool m_DocumentHostTabGroupTest = false;
		size_t m_DocumentHostTabGroupTestFrameCount = 0;
		size_t m_DocumentHostTabGroupCycleCount = 0;
		bool m_DocumentHostPresentationFailureTest = false;
		bool m_DocumentHostAcquireOutOfDateTest = false;
		bool m_DocumentHostImageAllocationFailureTest = false;
		bool m_DocumentHostPresentationFailureInjected = false;
		bool m_DocumentHostResetLayoutTestPending = false;
		size_t m_DocumentHostResetLayoutExpectedHostCount = 0;
		std::vector<std::pair<PulseForgeEditor::EditorDocumentIdentity, std::string>> m_DocumentHostFixtureSessions;
		std::unordered_map<PulseForge::AssetID, int, PulseForge::UUIDHash> m_DocumentHostTestSavedValues;
		DocumentTabTearOffTestPhase m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Disabled;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentHostTabTearOffTestIdentity;
		ImVec2 m_DocumentHostTabTearOffTestTabCenter{ 0.0f, 0.0f };
		ImVec2 m_DocumentHostTabTearOffTestStart{ 0.0f, 0.0f };
		ImVec2 m_DocumentHostTabTearOffTestDrop{ 0.0f, 0.0f };
		ImGuiID m_DocumentHostTabTearOffTestTabViewportID = 0;
		std::string m_DocumentHostTabTearOffSourceHostID;
		bool m_DocumentHostTabTearOffTestTabCenterValid = false;
		DocumentTabReattachTestPhase m_DocumentHostTabReattachTestPhase =
			DocumentTabReattachTestPhase::Disabled;
		DocumentTabReorderTestPhase m_DocumentHostTabReorderTestPhase =
			DocumentTabReorderTestPhase::Disabled;
		DocumentTabGroupTestPhase m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::Disabled;
		DocumentWorkspaceSwitchTestPhase m_DocumentWorkspaceSwitchTestPhase =
			DocumentWorkspaceSwitchTestPhase::Disabled;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentWorkspaceSwitchLevelIdentity;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentWorkspaceSwitchFirstIdentity;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentWorkspaceSwitchSecondIdentity;
		const PulseForgeEditor::EditorDocumentSession* m_DocumentWorkspaceSwitchFirstSession = nullptr;
		const PulseForgeEditor::EditorDocumentEditorState* m_DocumentWorkspaceSwitchFirstState = nullptr;
		const PulseForgeEditor::EditorDocumentEditorState* m_DocumentWorkspaceSwitchSecondState = nullptr;
		const PulseForge::Scene* m_DocumentWorkspaceSwitchScene = nullptr;
		const PulseForge::Scene* m_DocumentWorkspaceSwitchRuntimeScene = nullptr;
		std::optional<PulseForge::UUID> m_DocumentWorkspaceSwitchSelectedEntity;
		glm::vec3 m_DocumentWorkspaceSwitchCameraPosition{ 0.0f };
		bool m_DocumentWorkspaceSwitchSceneViewportVisible = false;
		bool m_DocumentWorkspaceSwitchHierarchyVisible = false;
		bool m_DocumentWorkspaceSwitchInspectorVisible = false;
		bool m_DocumentWorkspaceSwitchContentBrowserVisible = false;
		std::string m_DocumentWorkspaceSwitchNativeHostID;
		ImGuiID m_DocumentWorkspaceSwitchMainDockspaceID = 0;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentHostTabReattachTestIdentity;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentHostTabReorderTestFirst;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentHostTabReorderTestSecond;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentHostTabGroupTargetIdentity;
		std::string m_DocumentHostTabReattachTestSourceHostID;
		std::string m_DocumentHostTabGroupSourceHostID;
		std::string m_DocumentHostTabGroupTargetHostID;
		bool m_DocumentHostTabReattachExpectSourceHostRetired = true;
		ImVec2 m_DocumentHostTabReattachTestTabCenter{ 0.0f, 0.0f };
		ImGuiID m_DocumentHostTabReattachTestTabViewportID = 0;
		bool m_DocumentHostTabReattachTestTabCenterValid = false;
		std::array<DocumentTabTestPointerTarget, DocumentTabTestInvalidDropTargetNames.size()>
			m_DocumentHostTabReattachInvalidDropTargets{};
		size_t m_DocumentHostTabReattachInvalidDropIndex = 0;
		size_t m_DocumentHostTabReattachTestFrameCount = 0;
		ImVec2 m_DocumentHostTabReattachTestMainDropPosition{ 0.0f, 0.0f };
		ImGuiID m_DocumentHostTabReattachTestMainViewportID = 0;
		bool m_DocumentHostTabReattachTestMainDropPositionValid = false;
		ImVec2 m_DocumentHostTabReorderTestFirstTabCenter{ 0.0f, 0.0f };
		ImGuiID m_DocumentHostTabReorderTestFirstTabViewportID = 0;
		bool m_DocumentHostTabReorderTestFirstTabCenterValid = false;
		ImVec2 m_DocumentHostTabReorderTestSecondBeforePosition{ 0.0f, 0.0f };
		ImVec2 m_DocumentHostTabReorderTestSecondAfterPosition{ 0.0f, 0.0f };
		bool m_DocumentHostTabReorderTestSecondBeforePositionValid = false;
		bool m_DocumentHostTabReorderTestSecondAfterPositionValid = false;
		size_t m_DocumentHostTabReorderTestFrameCount = 0;
		ImVec2 m_DocumentHostTabGroupSourceCenter{ 0.0f, 0.0f };
		ImVec2 m_DocumentHostTabGroupTargetCenter{ 0.0f, 0.0f };
		ImGuiID m_DocumentHostTabGroupSourceViewportID = 0;
		ImGuiID m_DocumentHostTabGroupTargetViewportID = 0;
		bool m_DocumentHostTabGroupSourceCenterValid = false;
		bool m_DocumentHostTabGroupTargetCenterValid = false;
		std::optional<PulseForge::Project> m_Project;
		std::unique_ptr<PulseForge::Scene> m_Scene;
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
		PulseForgeEditor::EditorViewportCamera m_EditorCamera;
		std::optional<PulseForge::AssetID> m_SceneAsset;
		std::optional<EntityClipboard> m_EntityClipboard;
		PulseForgeEditor::EditorMaterialDraftStore m_MaterialDraftStore;
		std::vector<AssetBrowserPresentation> m_AssetBrowserPresentations;
		std::string m_HierarchySearch;
		std::string m_WindowTitle;
		std::string m_ViewportTargetError;
		std::string m_ViewportSceneError;
		std::string m_ImGuiRenderingError;
		std::string m_LayoutPersistenceError;
		std::filesystem::path m_DocumentWorkspacePath;
		std::string m_DocumentWorkspaceLastContents;
		std::unordered_set<std::string> m_RestoredHostBoundsPending;
		std::chrono::steady_clock::time_point m_DocumentWorkspaceSaveAt{};
		bool m_DocumentWorkspaceImmediateDirty = false;
		bool m_DocumentWorkspaceBoundsDirty = false;
		std::chrono::steady_clock::time_point m_ProjectOpenStartedAt{};
		std::string m_StatusMessage = "Create or open a project to begin.";
		bool m_StatusIsError = false;
		bool m_StatusIsWarning = false;
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
