#ifdef PF_EDITOR_RENDERER_OPENGL
#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#endif

#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Core/PlatformServices.h"
#include "Core/ScopedProfileTimer.h"
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
#include "Assets/AssetTypes.h"
#include "Assets/SceneAssetService.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Runtime/SceneRuntime.h"
#include "Window/Window.h"
#include "Editor/EditorImGuiRenderer.h"
#include "Editor/EditorAssetPreviewService.h"
#include "Editor/EditorAssetPreviewStressHarness.h"
#include "Editor/EditorAssetPreviewPolicy.h"
#include "Editor/EditorAssetDestinationDialog.h"
#include "Editor/EditorMeshPickingCache.h"
#include "Editor/EditorDeferredOperationQueue.h"
#include "Editor/EditorConsolePanel.h"
#include "Editor/EditorStatusPanel.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorContentBrowserPanel.h"
#include "Editor/EditorHierarchyPanel.h"
#include "Editor/EditorInspectorPanel.h"
#include "Editor/EditorMaterialDetailsPanel.h"
#include "Editor/EditorWorkspacePresentation.h"
#include "Editor/EditorLayout.h"
#include "Editor/EditorMainMenuPanel.h"
#include "Editor/EditorViewportCamera.h"
#include "Editor/EditorViewportSceneRenderView.h"
#include "Editor/EditorViewportToolbar.h"
#include "Editor/EditorViewportGizmoInteraction.h"
#include "Editor/EditorMaterialDraftController.h"
#include "Editor/EditorContentBrowserPolicy.h"
#include "Editor/EditorContentBrowserState.h"
#include "Editor/EditorAssetWorkflowState.h"
#include "Editor/EditorSceneEditController.h"
#include "Editor/EditorSceneEntityCommands.h"
#include "Editor/EditorSceneViewportInteraction.h"
#include "Editor/EditorSceneVisibilityCommands.h"
#include "Editor/EditorAssetDestinationPolicy.h"
#include "Editor/EditorAssetSelectorPolicy.h"
#include "Editor/EditorProjectOpening.h"
#include "Editor/EditorApplicationFactory.h"
#include "Editor/EditorPlatformServices.h"
#include "Editor/EditorInteractionPolicy.h"
#include "Editor/EditorGlfwViewportPolicy.h"
#include "Editor/EditorMaterialEditorPolicy.h"
#include "Editor/EditorPlatformViewportPolicy.h"
#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorDocumentHostTestEditor.h"
#include "Editor/EditorDocumentCloseBatch.h"
#include "Editor/EditorNativeWindowBoundsPolicy.h"
#include "Editor/EditorDocumentHostPolicy.h"
#include "Editor/EditorDocumentTabPolicy.h"
#include "Editor/EditorDocumentTabInteraction.h"
#include "Editor/EditorDocumentWorkspace.h"
#include "Editor/EditorDocumentWorkspaceStore.h"
#include "Editor/EditorDocumentCloseRequest.h"
#include "Editor/EditorSnapshotHistory.h"
#include "Editor/EditorSceneTransitionQueue.h"
#include "Editor/EditorSceneHierarchyOperations.h"
#include "Editor/EditorStyle.h"
#include "Editor/EditorWorkspaceView.h"
#include "Editor/EditorViewportMouseCapture.h"
#include "Editor/ViewportMath.h"
#include "Editor/ViewportGizmoGeometry.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <misc/cpp/imgui_stdlib.h>
#include <backends/imgui_impl_glfw.h>
#include <imgui_internal.h>
#ifdef PF_EDITOR_RENDERER_OPENGL
#include <backends/imgui_impl_opengl3.h>
#endif
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

	GLFWmonitor* FindMonitorForBounds(float X, float Y, float Width, float Height)
	{
		int MonitorCount = 0;
		GLFWmonitor** Monitors = glfwGetMonitors(&MonitorCount);
		GLFWmonitor* BestMonitor = nullptr;
		double BestArea = 0.0;
		for (int Index = 0; Monitors && Index < MonitorCount; ++Index)
		{
			int WorkX = 0;
			int WorkY = 0;
			int WorkWidth = 0;
			int WorkHeight = 0;
			glfwGetMonitorWorkarea(Monitors[Index], &WorkX, &WorkY, &WorkWidth, &WorkHeight);
			const double Area = PulseForgeEditor::NativeWindowWorkAreaIntersectionArea(
				X, Y, Width, Height, { static_cast<float>(WorkX), static_cast<float>(WorkY),
					static_cast<float>(WorkWidth), static_cast<float>(WorkHeight) });
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

	ResolvedNativeWindowBounds ResolveRestoredNativeWindowBounds(const PulseForgeEditor::EditorDocumentHost::WindowBounds& Saved)
	{
		if (!PulseForgeEditor::EditorPlatformServices::CanPositionNativeWindows())
		{
			const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			const float Width = (std::min)(Saved.Width, (std::max)(320.0f, MainViewport->WorkSize.x - 48.0f));
			const float Height = (std::min)(Saved.Height, (std::max)(240.0f, MainViewport->WorkSize.y - 48.0f));
			return { ImVec2(0.0f, 0.0f), ImVec2(Width, Height) };
		}

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
		if (!Monitor) Monitor = FindMonitorForBounds(Saved.X, Saved.Y, Saved.Width, Saved.Height);
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
			Monitor = FindMonitorForBounds(MainViewport->Pos.x, MainViewport->Pos.y, MainViewport->Size.x, MainViewport->Size.y);
			if (!Monitor) Monitor = glfwGetPrimaryMonitor();
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
#if !defined(__APPLE__)
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
#endif
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
				{ static_cast<float>(WorkX), static_cast<float>(WorkY), static_cast<float>(WorkWidth), static_cast<float>(WorkHeight) },
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
		if (!PulseForgeEditor::EditorPlatformServices::CanPositionNativeWindows()) return Bounds;
		Bounds.HasValue = true;
		if (GLFWmonitor* Monitor = FindMonitorForBounds(Bounds.X, Bounds.Y, Bounds.Width, Bounds.Height))
		{
			if (const char* MonitorName = glfwGetMonitorName(Monitor)) Bounds.MonitorName = MonitorName;
			float ScaleX = 1.0f;
			float ScaleY = 1.0f;
			glfwGetMonitorContentScale(Monitor, &ScaleX, &ScaleY);
			Bounds.DpiScale = (std::max)(ScaleX, ScaleY);
		}
		return Bounds;
	}

	bool BeginEditorChromeBar(const char* ID, ImGuiDir Direction, float Height, PulseForgeEditor::EditorColorToken Background,
		const PulseForgeEditor::EditorStyle& Style)
	{
		ImGui::PushStyleColor(ImGuiCol_WindowBg, Style.GetColor(Background));
		ImGui::PushStyleColor(ImGuiCol_Border, Style.GetColor(PulseForgeEditor::EditorColorToken::Border));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		const bool Visible = ImGui::BeginViewportSideBar(ID, ImGui::GetMainViewport(), Direction, Height,
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

	bool HasReservedMetadataExtension(const std::filesystem::path& Path)
	{
		return PulseForge::IsMetadataSidecarPath(Path);
	}

	std::string FitEditorContextLabel(std::string_view Label, float AvailableWidth)
	{
		constexpr std::string_view Ellipsis = "\xe2\x80\xa6";
		if (ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x <= AvailableWidth) return std::string(Label);

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

	class EditorLayer final : public PulseForge::Layer,
							  public PulseForgeEditor::EditorHierarchyPanelActionSink,
							  public PulseForgeEditor::EditorInspectorPanelActions,
							  public PulseForgeEditor::EditorSceneEntityCommandActions,
							  public PulseForgeEditor::EditorMaterialDetailsActionSink,
							  public PulseForgeEditor::EditorContentBrowserPanelActions
	{
		struct PlatformViewportRendererData
		{
			uint64_t TargetID = 0;
			bool FrameActive = false;
		};

		using ContentFolderNode = PulseForgeEditor::EditorContentFolderNode;
		using AssetBrowserPresentation = PulseForgeEditor::EditorContentBrowserAssetPresentation;
		using ProjectAssetDestinationCreateKind = PulseForgeEditor::ProjectAssetDestinationCreateKind;

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
		static constexpr std::array<const char*, 5> DocumentTabTestInvalidDropPanelNames = { "Hierarchy", "Inspector", "Scene Viewport",
			"Content Browser", "Console" };
		static constexpr size_t DocumentTabTestConsolePanelIndex = DocumentTabTestInvalidDropPanelNames.size() - 1;
		static constexpr std::array<const char*, DocumentTabTestInvalidDropPanelNames.size() * DocumentTabTestDropPointsPerPanel>
			DocumentTabTestInvalidDropTargetNames = { "Hierarchy body", "Hierarchy top edge", "Hierarchy left edge", "Hierarchy right edge",
				"Hierarchy bottom edge", "Inspector body", "Inspector top edge", "Inspector left edge", "Inspector right edge",
				"Inspector bottom edge", "Scene Viewport body", "Scene Viewport top edge", "Scene Viewport left edge",
				"Scene Viewport right edge", "Scene Viewport bottom edge", "Content Browser body", "Content Browser top edge",
				"Content Browser left edge", "Content Browser right edge", "Content Browser bottom edge", "Console body",
				"Console top edge", "Console left edge", "Console right edge", "Console bottom edge" };

		using GizmoHandle = PulseForgeEditor::ViewportGizmoHandle;
		using ViewportGizmoGeometry = PulseForgeEditor::ViewportGizmoGeometry;

		enum class ViewportCameraGesture : uint8_t
		{
			None,
			Orbit,
			Pan,
			Dolly
		};

	public:
		explicit EditorLayer(PulseForgeEditor::EditorCommandLineOptions Options)
			: Layer("Editor"),
			  m_AssetPreviewStress(std::move(Options.AssetPreviewStressManifest), std::move(Options.AssetPreviewStressBudget)),
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
#if defined(_DEBUG) || defined(PF_DEBUG)
			if (m_DocumentHostTestFixture)
			{
				m_DocumentHostTestEditor = std::make_unique<PulseForgeEditor::Detail::EditorDocumentHostTestEditor>(
					m_Documents, m_DocumentEditors, m_EditorStyle, [this](std::string Message) { SetError(std::move(Message)); },
					[this] { return m_Project ? m_Project->GetDescription().ID : PulseForge::UUID{}; });
				if (auto Result = m_DocumentHostTestEditor->Register(); !Result)
					PF_ERROR("Could not register the debug document-host fixture editor: {}", Result.error());
			}
#endif
		}

		void HandleHierarchyPanelAction(const PulseForgeEditor::EditorHierarchyPanelAction& Action) override
		{
			using PulseForgeEditor::EditorHierarchyPanelActionKind;
			switch (Action.Kind)
			{
			case EditorHierarchyPanelActionKind::CreateEntity:
				CreateEntityFromEditor();
				break;
			case EditorHierarchyPanelActionKind::DuplicateSelection:
				DuplicateSelectedEntity();
				break;
			case EditorHierarchyPanelActionKind::DeleteSelection:
				DeleteSelectedEntity();
				break;
			case EditorHierarchyPanelActionKind::CreatePrefab:
				CreatePrefabFromSelectedEntity();
				break;
			case EditorHierarchyPanelActionKind::ToggleVisibility:
				ToggleEntityVisibility(Action.Entity);
				break;
			case EditorHierarchyPanelActionKind::MakeRoot:
				ReparentEntity(Action.Entity, std::nullopt);
				break;
			case EditorHierarchyPanelActionKind::ParentUnderSelection:
				ReparentEntity(Action.Entity, Action.Parent);
				break;
			case EditorHierarchyPanelActionKind::ReportError:
				SetError(Action.Message);
				break;
			}
		}

		void FinishInspectorSceneEdit() override { FinishSceneEdit(); }

		[[nodiscard]] std::expected<void, std::string> RunInspectorSceneEdit(
			std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			return RunSceneEdit(std::move(Description), std::move(Operation));
		}

		void ReportInspectorError(std::string Message) override { SetError(std::move(Message)); }

		[[nodiscard]] std::expected<void, std::string> RunEditorSceneEdit(
			std::string Description, std::function<std::expected<void, PulseForge::SceneError>()> Operation) override
		{
			return RunSceneEdit(std::move(Description), std::move(Operation));
		}

		void ReportEditorSceneCommandError(std::string Message) override { SetError(std::move(Message)); }

		void SetEditorSceneCommandStatus(std::string Message) override { SetStatus(std::move(Message)); }

		void ReportEditorSceneCommandWarning(std::string Message) override { SetWarning(std::move(Message)); }

		void SetEditorSceneSelection(std::vector<PulseForge::UUID> Identifiers) override { SetEditorSelection(std::move(Identifiers)); }

		void SelectSingleEditorEntity(std::optional<PulseForge::UUID> Identifier) override { SelectSingleEntity(Identifier); }

		void HandleContentBrowserToolbarAction(PulseForgeEditor::EditorContentBrowserToolbarAction Action) override
		{
			switch (Action)
			{
			case PulseForgeEditor::EditorContentBrowserToolbarAction::Import:
				ImportAsset();
				break;
			case PulseForgeEditor::EditorContentBrowserToolbarAction::Refresh:
				RefreshAssets();
				break;
			}
		}

		void HandleContentBrowserAssetAction(
			PulseForgeEditor::EditorContentBrowserAssetAction Action, const PulseForge::AssetID& Identifier) override
		{
			using PulseForgeEditor::EditorContentBrowserAssetAction;
			switch (Action)
			{
			case EditorContentBrowserAssetAction::OpenScene:
				OpenScene(Identifier);
				break;
			case EditorContentBrowserAssetAction::ShowMaterialDetails:
				ShowMaterialDetails(Identifier);
				break;
			case EditorContentBrowserAssetAction::Move:
				MoveAsset(Identifier);
				break;
			case EditorContentBrowserAssetAction::Duplicate:
				DuplicateAsset(Identifier);
				break;
			case EditorContentBrowserAssetAction::InstantiatePrefab:
				InstantiatePrefab(Identifier);
				break;
			case EditorContentBrowserAssetAction::SetStartupScene:
				SetStartupScene(Identifier);
				break;
			case EditorContentBrowserAssetAction::Delete:
				RequestDeleteAsset(Identifier);
				break;
			}
		}

		bool IsContentBrowserMaterialDraftDirty(const PulseForge::AssetID& Identifier) const override
		{
			return m_Project && m_MaterialDrafts.IsDirty(*m_Project, Identifier);
		}

		bool SaveMaterialFromDetails(const PulseForge::AssetID& Asset) override { return SaveMaterialDraft(Asset); }

		bool RevertMaterialFromDetails(const PulseForge::AssetID& Asset) override { return ReloadMaterialDraft(Asset); }

		void MaterialDetailsDraftEdited(const PulseForge::UUID& ProjectID, const PulseForge::AssetID& Asset) override
		{
			const PulseForgeEditor::EditorDocumentIdentity Identity{ ProjectID, Asset, PulseForgeEditor::EditorDocumentKind::Material };
			if (m_Documents.Find(Identity)) (void)m_Documents.MarkEdited(Identity);
		}

		void QueueAssetMoveAfterSave(const PulseForge::AssetID& Asset, const std::filesystem::path& Destination) override
		{
			QueueAfterSave([this, Asset, Destination] { MoveAssetAt(Asset, Destination); });
		}

		void QueueFolderMoveAfterSave(const std::filesystem::path& Source, const std::filesystem::path& Destination) override
		{
			QueueAfterSave([this, Source, Destination] { MoveFolderAt(Source, Destination); });
		}

		static EditorLayer* GetPlatformCallbackLayer() noexcept
		{
			ImGuiContext* Context = ImGui::GetCurrentContext();
			return Context ? static_cast<EditorLayer*>(ImGui::GetIO().UserData) : nullptr;
		}

		static void OnRendererCreateViewport(ImGuiViewport* Viewport)
		{
			EditorLayer* Layer = GetPlatformCallbackLayer();
			if (!Layer || !Viewport || !Viewport->PlatformHandle) return;
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
				if (Layer) Layer->HandlePlatformPresentationFailure(Viewport, Target.error());
				else PF_ERROR("Could not create native editor presentation target: {}", Target.error());
				return;
			}
			try
			{
				Viewport->RendererUserData = new PlatformViewportRendererData{ *Target, false };
#if defined(PF_DEBUG)
				if (InjectAcquireOutOfDate && !PulseForge::Application::Get().InjectNextPlatformAcquireOutOfDateForTesting(*Target))
				{
					PF_ERROR("The active renderer could not arm the native-host acquire out-of-date fixture");
					auto* RendererData = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
					Viewport->RendererUserData = nullptr;
					delete RendererData;
					(void)PulseForge::Application::Get().DestroyPlatformPresentationTarget(*Target);
					Layer->HandlePlatformPresentationFailure(Viewport, "The active renderer could not arm the acquire out-of-date fixture");
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
			if (!Viewport || !Viewport->RendererUserData) return;
			auto* Data = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
			if (Data->FrameActive) PulseForge::Application::Get().EndPlatformPresentationTarget(Data->TargetID);
			const bool Retired = PulseForge::Application::Get().DestroyPlatformPresentationTarget(Data->TargetID);
			if (!Retired) PF_ERROR("Native Vulkan presentation retirement is deferred until renderer shutdown.");
			if (EditorLayer* Layer = GetPlatformCallbackLayer(); Layer && Layer->m_ImGuiRenderer)
				if (Retired) Layer->m_ImGuiRenderer->RetirePresentationTarget(Data->TargetID);
			delete Data;
			Viewport->RendererUserData = nullptr;
		}

		static void OnRendererRenderViewport(ImGuiViewport* Viewport, void*)
		{
			EditorLayer* Layer = GetPlatformCallbackLayer();
			if (!Layer || !Layer->m_ImGuiRenderer || !Viewport || !Viewport->RendererUserData) return;
			auto* Data = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
			if (!PulseForge::Application::Get().BeginPlatformPresentationTarget(Data->TargetID)) return;
			Data->FrameActive = true;
			if (auto Result = Layer->m_ImGuiRenderer->RenderDrawData(Viewport->DrawData, Data->TargetID); !Result)
				PF_ERROR("Native editor window ImGui rendering failed: {}", Result.error());
		}

		static void OnRendererSwapViewport(ImGuiViewport* Viewport, void*)
		{
			if (!Viewport || !Viewport->RendererUserData) return;
			auto* Data = static_cast<PlatformViewportRendererData*>(Viewport->RendererUserData);
			if (!Data->FrameActive) return;
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
			if (!m_Context) throw std::runtime_error("Could not create the editor ImGui context");

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
			m_EditorStyle.ApplyTheme(GetEditorUiColorEncoding(PulseForge::Application::Get().GetOutputColorEncoding()));
			if (auto Result = m_Layout.Initialize(); !Result) ReportLayoutPersistenceError(Result.error());
			m_Documents.SetOpeningPreference(
				static_cast<PulseForgeEditor::EditorDocumentOpenPreference>(m_Layout.GetDocumentOpeningPreference()));

			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			PulseForgeEditor::EditorPlatformServices::ApplyMainWindowAppearance(Window);
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
			auto ResourceDirectory = PulseForge::Platform::GetResourceDirectory();
			if (!ResourceDirectory)
			{
				ShutdownImGui();
				throw std::runtime_error(
					"Could not resolve the editor resource directory for shader loading: " + ResourceDirectory.error().Message);
			}
			m_EditorShaderDirectory = *ResourceDirectory / PF_EDITOR_SHADER_DIRECTORY;
			if (auto Result = m_ImGuiRenderer->Initialize(Runtime, m_EditorShaderDirectory); !Result)
			{
				const std::string Message = "Could not initialize the Vulkan editor UI renderer: " + Result.error();
				ShutdownImGui();
				throw std::runtime_error(Message);
			}
			const PulseForgeEditor::AssetPreviewServiceLimits PreviewLimits = m_AssetPreviewStress.GetServiceLimits();
			m_AssetPreviews.Initialize(Runtime, *m_ImGuiRenderer, PreviewLimits);
			if (m_AssetPreviewStress.IsEnabled()) m_AssetPreviewStress.Initialize(m_AssetPreviews);
			IO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
			ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
			PlatformIO.Renderer_CreateWindow = &OnRendererCreateViewport;
			PlatformIO.Renderer_DestroyWindow = &OnRendererDestroyViewport;
			PlatformIO.Renderer_RenderWindow = &OnRendererRenderViewport;
			PlatformIO.Renderer_SwapBuffers = &OnRendererSwapViewport;
			IO.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
#endif
			m_ConsolePanel.Attach();

			if (auto Result = PulseForgeEditor::EditorPlatformServices::InitializeNativeFileDialogs(); Result)
			{
				m_FileDialogActive = true;
				PF_INFO("Native file dialogs initialized");
			}
			else SetError("Could not initialize native file dialogs: " + Result.error());

			if (m_StartupProjectPath)
			{
				auto Opened = OpenProjectAt(*m_StartupProjectPath);
				if (!Opened) throw std::runtime_error("Startup project load failed: " + Opened.error());
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
			m_ConsolePanel.CapturePendingLogs();
#ifdef PF_EDITOR_RENDERER_OPENGL
			if (m_Context)
			{
				ImGui::SetCurrentContext(m_Context);
				const ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
				const uint32_t SecondaryViewportCount =
					PlatformIO.Viewports.Size > 1 ? static_cast<uint32_t>(PlatformIO.Viewports.Size - 1) : 0;
				PulseForge::Application::Get().SetExternalPlatformPresentationTargetCount(SecondaryViewportCount);
			}
#endif
			if (m_AssetPreviewProjectPending)
			{
				if (!m_AssetPreviewStress.IsEnabled())
					m_AssetPreviews.SetProject(m_Project ? m_Project->GetDescription().ID.ToString() : std::string{});
				m_AssetPreviewProjectPending = false;
			}
			if (m_AssetPreviewAssetsPending)
			{
				m_AssetPreviews.InvalidateAll();
				m_AssetPreviewAssetsPending = false;
			}
			if (m_RendererAssetCachesPending || m_MaterialAssetBindingsPending)
			{
				if (m_SceneRuntime)
				{
					if (!m_AssetRefreshDeferredForRuntime) SetWarning("Renderer asset changes are deferred until Play mode stops.");
					m_AssetRefreshDeferredForRuntime = true;
				}
				else
				{
					if (m_RendererAssetCachesPending)
					{
						m_ViewportRenderView.RefreshProjectAssets();
						m_MeshPickingCache.Clear();
					}
					else if (m_MaterialAssetBindingsPending) m_ViewportRenderView.RefreshMaterialAssets();
					m_RendererAssetCachesPending = false;
					m_MaterialAssetBindingsPending = false;
					m_AssetRefreshDeferredForRuntime = false;
				}
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

#ifdef PF_EDITOR_RENDERER_OPENGL
		void CaptureOpenGLFrameForValidation() noexcept
		{
			if (m_OpenGLDiagnosticCapturePath.empty() || m_OpenGLDiagnosticCaptureWritten || m_OpenGLDiagnosticFramesSeen < 3) return;
			m_OpenGLDiagnosticCaptureWritten = true;
			CaptureOpenGLFramebuffer(m_OpenGLDiagnosticCapturePath, GL_BACK);
		}

		void CaptureOpenGLPlatformFramesForValidation(GLFWwindow* MainContext) noexcept
		{
			if (m_OpenGLDiagnosticCapturePath.empty() || m_OpenGLDiagnosticPlatformCaptureWritten || m_OpenGLDiagnosticFramesSeen < 3)
				return;
			m_OpenGLDiagnosticPlatformCaptureWritten = true;
			ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
			for (int Index = 0; Index < PlatformIO.Viewports.Size; ++Index)
			{
				ImGuiViewport* Viewport = PlatformIO.Viewports[Index];
				const int VertexCount = Viewport && Viewport->DrawData ? Viewport->DrawData->TotalVtxCount : 0;
				PF_INFO("OpenGL platform viewport {}: id 0x{:08X}, {}x{}, ImGui vertices {}", Index, Viewport ? Viewport->ID : 0,
					Viewport ? Viewport->Size.x : 0.0f, Viewport ? Viewport->Size.y : 0.0f, VertexCount);
				if (Index == 0 || !Viewport) continue;
				const PulseForgeEditor::EditorGlfwViewportHandles Handles{ .GlfwWindow = Viewport->PlatformHandle,
					.NativeWindow = Viewport->PlatformHandleRaw };
				GLFWwindow* PlatformWindow = static_cast<GLFWwindow*>(PulseForgeEditor::GetGlfwContextWindow(Handles));
				if (!PlatformWindow) continue;
				glfwMakeContextCurrent(PlatformWindow);
				std::filesystem::path OutputPath = m_OpenGLDiagnosticCapturePath;
				OutputPath.replace_filename(
					OutputPath.stem().string() + "_viewport_" + std::to_string(Viewport->ID) + OutputPath.extension().string());
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
			Header[0] = 'B';
			Header[1] = 'M';
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
			if (!Output) PF_ERROR("Could not write OpenGL validation framebuffer capture to {}", OutputPath.string());
			else PF_INFO("OpenGL validation framebuffer captured to {}: {}x{}", OutputPath.string(), Viewport[2], Viewport[3]);
		}
#endif

		void OnRender() override
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			if (!m_OpenGLDiagnosticCapturePath.empty()) ++m_OpenGLDiagnosticFramesSeen;
#endif
			m_AssetPreviewStress.BeginFrame();
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
			m_DocumentTabInteraction.BeginFrame();
			for (DocumentTabTestPointerTarget& Target : m_DocumentHostTabReattachInvalidDropTargets)
				Target.Available = false;
			m_DocumentHostTabReorderTestFirstTabCenterValid = false;
			m_DocumentHostTabReorderTestSecondBeforePositionValid = false;
			m_DocumentHostTabReorderTestSecondAfterPositionValid = false;
			m_DocumentHostTabGroupSourceCenterValid = false;
			m_DocumentHostTabGroupTargetCenterValid = false;
			FinalizePendingMaterialEdits();
			m_AssetPreviews.BeginRequestFrame();
			if (m_AssetPreviewStress.IsEnabled()) m_AssetPreviewStress.Draw(m_AssetPreviews);
			m_EditorStyle.ApplyTheme(GetEditorUiColorEncoding(PulseForge::Application::Get().GetOutputColorEncoding()));
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
				QueueAfterSave(
					[this, ProjectFile]
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
				if (const char* Folder = std::getenv("PULSEFORGE_EDITOR_OPENGL_CONTENT_FOLDER"); Folder && *Folder)
				{
					const std::filesystem::path RequestedFolder = std::filesystem::u8path(Folder);
					m_ContentBrowser.SetCurrentFolder(RequestedFolder);
				}
				if (ImGuiWindow* ContentBrowser = ImGui::FindWindowByName("Content Browser"); ContentBrowser)
				{
					ImGui::SetWindowFocus("Content Browser");
					if (ContentBrowser->DockNode) ContentBrowser->DockNode->SelectedTabId = ContentBrowser->TabId;
					ImGui::MarkIniSettingsDirty();
					if (!m_OpenGLDiagnosticContentBrowserSelected)
					{
						PF_INFO("OpenGL validation selected Content Browser: window 0x{:08X}, tab 0x{:08X}, node 0x{:08X}",
							ContentBrowser->ID, ContentBrowser->TabId, ContentBrowser->DockNode ? ContentBrowser->DockNode->ID : 0);
						m_OpenGLDiagnosticContentBrowserSelected = true;
					}
				}
			}
#endif
			if (PulseForge::Application::Get().IsMainFrameActive()) RenderViewportScene();
			UpdateWindowTitle();
			if (PulseForgeEditor::ShouldFinishSceneEditForInspector(
					m_SceneEditController.IsActive(), m_SceneEditController.InspectorHistoryItem(), GImGui ? GImGui->ActiveId : 0))
				FinishSceneEdit();

			ImGui::Render();
			if (auto Result = m_Layout.SaveIfRequested(); !Result) ReportLayoutPersistenceError(Result.error());
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
					if (m_AssetPreviewStress.IsEnabled()) m_AssetPreviewStress.MarkRenderFailure();
					if (m_ImGuiRenderingError != Result.error())
					{
						m_ImGuiRenderingError = Result.error();
						PF_ERROR("Editor UI rendering failed: {}", m_ImGuiRenderingError);
					}
				}
				else m_ImGuiRenderingError.clear();
			}
#endif
			SaveDocumentWorkspaceIfNeeded();
		}

		void OnRenderPlatformWindows() override
		{
			ImGui::SetCurrentContext(m_Context);
			if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				if (PulseForgeEditor::ShouldProcessNativeDocumentHostCloseRequests(m_ApplicationShutdownRequested))
					HandleNativePlatformCloseRequests();
#ifdef PF_EDITOR_RENDERER_OPENGL
				GLFWwindow* MainContext = glfwGetCurrentContext();
#endif
				ImGui::UpdatePlatformWindows();
				ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
				for (int Index = 1; Index < PlatformIO.Viewports.Size; ++Index)
				{
					ImGuiViewport* Viewport = PlatformIO.Viewports[Index];
					if (!Viewport || (Viewport->Flags & ImGuiViewportFlags_IsMinimized)) continue;

					PulseForgeEditor::RenderPlatformViewportSequentially(
						[&]
						{
							if (PlatformIO.Platform_RenderWindow) PlatformIO.Platform_RenderWindow(Viewport, nullptr);
						},
						[&]
						{
							if (PlatformIO.Renderer_RenderWindow) PlatformIO.Renderer_RenderWindow(Viewport, nullptr);
						},
						[&]
						{
							if (PlatformIO.Platform_SwapBuffers) PlatformIO.Platform_SwapBuffers(Viewport, nullptr);
						},
						[&]
						{
							if (PlatformIO.Renderer_SwapBuffers) PlatformIO.Renderer_SwapBuffers(Viewport, nullptr);
						});
				}
#ifdef PF_EDITOR_RENDERER_OPENGL
				CaptureOpenGLPlatformFramesForValidation(MainContext);
				glfwMakeContextCurrent(MainContext);
#endif
			}
#ifndef PF_EDITOR_RENDERER_OPENGL
			if (m_AssetPreviewStress.IsEnabled() && m_AssetPreviewStress.AdvanceFrame(m_AssetPreviews)) RequestEditorApplicationClose();
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
				if (Mouse.GetMouseButton() == GLFW_MOUSE_BUTTON_RIGHT) BeganNavigationForEvent = BeginEditorCameraNavigation();
			}
			else if (Event.GetEventType() == PulseForge::EEventType::MouseButtonReleased)
			{
				const auto& Mouse = static_cast<PulseForge::MouseButtonReleasedEvent&>(Event);
				if (Mouse.GetMouseButton() == GLFW_MOUSE_BUTTON_RIGHT && m_ViewportMouseCapture.OwnsMouse()) EndEditorCameraNavigation();
			}
			else if (Event.GetEventType() == PulseForge::EEventType::MouseScrolled)
			{
				if (m_Context) ImGui::SetCurrentContext(m_Context);
				const bool PopupOpen = m_Context && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
				const PulseForgeEditor::ViewportWheelAction WheelAction =
					PulseForgeEditor::ResolveViewportWheelAction({ .RightMouseNavigationActive = m_ViewportMouseCapture.OwnsMouse(),
						.RightMouseHeld = PulseForge::Application::Get().GetInput().IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT),
						.RuntimeActive = m_SceneRuntime != nullptr,
						.PopupOpen = PopupOpen });
				if (WheelAction == PulseForgeEditor::ViewportWheelAction::AdjustFlySpeed)
				{
					const auto& Scroll = static_cast<PulseForge::MouseScrolledEvent&>(Event);
					m_EditorCamera.AdjustMoveSpeed(Scroll.GetYOffset());
				}
			}
			else if (Event.GetEventType() == PulseForge::EEventType::MouseMoved && m_ViewportMouseCapture.OwnsMouse())
			{
				const auto& Mouse = static_cast<PulseForge::MouseMovedEvent&>(Event);
				if (const auto Delta = m_ViewportMouseCapture.UpdateCursorPosition(Mouse.GetX(), Mouse.GetY()))
					m_EditorCamera.Rotate(Delta->first, Delta->second);
			}

			ImGui::SetCurrentContext(m_Context);
			const ImGuiIO& IO = ImGui::GetIO();
			Event.bHandled |=
				Event.IsInCategory(PulseForge::EventCategoryMouse) &&
				(NavigationOwnedMouseBeforeEvent || BeganNavigationForEvent || m_ViewportMouseCapture.OwnsMouse() || IO.WantCaptureMouse);
			Event.bHandled |= Event.IsInCategory(PulseForge::EventCategoryKeyboard) && IO.WantCaptureKeyboard;
		}

	private:
		void RequestEditorApplicationClose()
		{
			if (m_ApplicationShutdownRequested || m_ApplicationClosePending) return;
			if (!SaveDocumentWorkspaceNow()) return;
			m_ApplicationClosePending = true;
			std::vector<PulseForgeEditor::EditorDocumentIdentity> Documents;
			for (const auto& [Identity, Session] : m_Documents.GetSessions())
				if (Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level) Documents.push_back(Identity);
			std::string CloseError;
			if (GetDirtyAssetDocuments().empty())
			{
				if (CloseDocumentBatchClean(Documents))
				{
					CompleteEditorApplicationClose();
					return;
				}
				CloseError = m_DocumentCloseRequest.GetError();
			}
			m_DocumentCloseRequest.BeginCloseAllDocuments(std::move(Documents),
				std::string(PulseForgeEditor::EditorDocumentManager::MainHostID), [this] { CompleteEditorApplicationClose(); });
			if (!CloseError.empty()) m_DocumentCloseRequest.SetError(CloseError);
			else
			{
				m_DocumentCloseRequest.SetError("Resolve open documents before closing PulseForge.");
				SetError(m_DocumentCloseRequest.GetError());
			}
		}

		void CompleteEditorApplicationClose()
		{
			m_ApplicationClosePending = false;
			m_ApplicationShutdownRequested = true;
			PulseForge::Application::Get().RequestClose();
		}

		void CancelEditorApplicationClose()
		{
			if (!m_ApplicationClosePending) return;
			m_ApplicationClosePending = false;
			MarkDocumentWorkspaceDirty();
		}

		[[nodiscard]] std::expected<void, std::string> RestoreSceneState(std::string_view State)
		{
			if (!m_Scene || m_SceneRuntime) return std::unexpected("Scene history is available only for the authored scene in Edit mode");
			auto Restored = m_SceneEditController.Restore(State, *m_Scene, m_SceneWorkspace);
			if (!Restored) return std::unexpected(Restored.error());
			m_ViewportGizmoInteraction.DiscardActiveState();
			m_SceneEditController.InspectorHistoryItem() = 0;
			UpdateSceneDirty();
			return {};
		}
		[[nodiscard]] const std::optional<PulseForge::UUID>& SelectedEntity() const noexcept
		{
			return m_SceneWorkspace.Selection().GetPrimary();
		}

		void SetEditorSelection(std::vector<PulseForge::UUID> Identifiers, std::optional<PulseForge::UUID> Primary = std::nullopt)
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

		[[nodiscard]] bool IsEntityEffectivelyVisible(const PulseForge::Entity& Entity) const
		{
			return PulseForgeEditor::EditorSceneVisibilityCommands::IsEntityEffectivelyVisible(Entity, m_SceneWorkspace.Visibility());
		}

		[[nodiscard]] std::vector<PulseForge::UUID> GetEffectivelyHiddenEntities() const
		{
			return PulseForgeEditor::EditorSceneVisibilityCommands::GetEffectivelyHiddenEntities(
				m_Scene.get(), m_SceneWorkspace.Visibility());
		}

		[[nodiscard]] std::expected<std::vector<PulseForge::Entity>, std::string> GetTopLevelSelectedEntities() const
		{
			if (!m_Scene) return std::unexpected("There is no active scene");
			return PulseForgeEditor::EditorSceneHierarchyOperations::GetTopLevelSelection(
				*m_Scene, m_SceneWorkspace.Selection().GetEntities());
		}

		[[nodiscard]] std::string MakeUniqueEntityName(
			std::string BaseName, std::optional<PulseForge::UUID> IgnoredEntity = std::nullopt) const
		{
			if (!m_Scene) return BaseName;
			return PulseForgeEditor::EditorSceneHierarchyOperations::MakeUniqueName(*m_Scene, std::move(BaseName), IgnoredEntity);
		}

		void ToggleEntityVisibility(const PulseForge::UUID& Identifier)
		{
			m_SceneVisibilityCommands.ToggleEntityVisibility(m_Scene.get(), m_SceneWorkspace, Identifier, m_SceneRuntime != nullptr, *this);
		}

		void HideSelectedEntities()
		{
			m_SceneVisibilityCommands.HideSelectedEntities(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, *this);
		}

		void ShowAllEntities()
		{
			m_SceneVisibilityCommands.ShowAllEntities(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, *this);
		}

		void IsolateSelectedEntities()
		{
			m_SceneVisibilityCommands.IsolateSelectedEntities(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, *this);
		}

		void RestoreIsolatedVisibility()
		{
			m_SceneVisibilityCommands.RestoreIsolatedVisibility(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, *this);
		}

		void UpdateSceneDirty() { m_SceneEditController.UpdateDirty(m_Scene.get()); }

		void ResetSceneHistory()
		{
			m_SceneEditController.Reset(m_Scene.get());
			m_SceneWorkspace.Visibility().ShowAll();
		}

		void CancelSceneEdit()
		{
			const auto Result = m_SceneEditController.Cancel(
				m_Scene.get(), m_SceneWorkspace, [this](std::string_view State) { return RestoreSceneState(State); });
			if (!Result) SetError("Scene edit rollback failed: " + Result.error());
		}

		bool BeginSceneEdit(std::string Description)
		{
			if (auto Begun = m_SceneEditController.Begin(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, std::move(Description),
					[this](std::string_view State) { return RestoreSceneState(State); });
				!Begun)
			{
				SetError("Could not begin the scene edit: " + Begun.error());
				return false;
			}
			return true;
		}

		void MarkSceneSaved() { m_SceneEditController.MarkSaved(m_Scene.get()); }

		bool FinishSceneEdit()
		{
			const auto Restore = [this](std::string_view State) { return RestoreSceneState(State); };
			std::expected<void, std::string> Result;
			if (m_ViewportGizmoInteraction.IsActive())
				Result = m_ViewportGizmoInteraction.Complete(m_Scene.get(), m_SceneWorkspace, m_SceneEditController, Restore);
			else Result = m_SceneEditController.Finish(m_Scene.get(), m_SceneWorkspace, Restore);
			if (!Result)
			{
				SetError(Result.error());
				return false;
			}
			return true;
		}

		template <typename Action> [[nodiscard]] std::expected<void, std::string> RunSceneEdit(std::string Description, Action&& Operation)
		{
			const uint32_t ActiveItem = GImGui ? GImGui->ActiveId : 0;
			return m_SceneEditController.Run(
				m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, ActiveItem, [] { return GImGui ? GImGui->ActiveId : 0; },
				std::move(Description), std::forward<Action>(Operation),
				[this](std::string_view State) { return RestoreSceneState(State); });
		}

		void UndoSceneEdit()
		{
			if (!m_Scene || m_SceneRuntime) return;
			const auto Result = m_SceneEditController.Undo(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr,
				[this](std::string_view State) { return RestoreSceneState(State); });
			if (!Result)
			{
				SetError("Undo failed: " + Result.error());
				return;
			}
			if (*Result) SetStatus("Undid the last scene edit.");
			UpdateSceneDirty();
		}

		void RedoSceneEdit()
		{
			if (!m_Scene || m_SceneRuntime) return;
			const auto Result = m_SceneEditController.Redo(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr,
				[this](std::string_view State) { return RestoreSceneState(State); });
			if (!Result)
			{
				SetError("Redo failed: " + Result.error());
				return;
			}
			if (*Result) SetStatus("Redid the last scene edit.");
			UpdateSceneDirty();
		}

		void HandleEditorHistoryShortcuts()
		{
			const ImGuiIO& IO = ImGui::GetIO();
			const PulseForge::UUID CurrentProject = m_Project ? m_Project->GetDescription().ID : PulseForge::UUID{};
			const auto CommandTarget = PulseForgeEditor::ResolveEditorDocumentCommandTarget(GetFocusedDocumentIdentity(), CurrentProject);
			const bool RegisteredDocumentFocused = GetFocusedRegisteredDocument() != nullptr;
			const bool MaterialDetailsFocused = IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails);
			const bool MaterialDocumentFocused = GetFocusedMaterialAsset().has_value();
			const bool UnsupportedDocumentFocused =
				(!RegisteredDocumentFocused && CommandTarget == PulseForgeEditor::EditorDocumentCommandTarget::Unsupported) ||
				(MaterialDetailsFocused && !MaterialDocumentFocused);
			if (!PulseForgeEditor::CanRouteEditorShortcut({ .SceneAvailable = RegisteredDocumentFocused || MaterialDocumentFocused ||
																			  (!UnsupportedDocumentFocused && m_Scene != nullptr),
					.RuntimeActive = !RegisteredDocumentFocused && !MaterialDocumentFocused && m_SceneRuntime != nullptr,
					.CameraNavigationActive = !RegisteredDocumentFocused && !MaterialDocumentFocused && m_ViewportMouseCapture.OwnsMouse(),
					.GizmoInteractionActive =
						!RegisteredDocumentFocused && !MaterialDocumentFocused && m_ViewportGizmoInteraction.IsActive(),
					.TextInputActive = IO.WantTextInput,
					.ImGuiItemActive = ImGui::IsAnyItemActive(),
					.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
					.MenuNavigationActive = GImGui && GImGui->NavLayer == ImGuiNavLayer_Menu,
					.WindowNavigationActive = GImGui && GImGui->NavWindowingTarget != nullptr }))
				return;

			if (!IO.KeyCtrl || IO.KeyAlt || IO.KeySuper) return;
			if (RegisteredDocumentFocused)
			{
				auto* Session = GetFocusedRegisteredDocument();
				if (Session && (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (ImGui::IsKeyPressed(ImGuiKey_Z, false) && IO.KeyShift)))
					(void)m_DocumentEditors.DispatchFocusedCommand(*Session, PulseForgeEditor::EditorDocumentCommand::Redo);
				else if (Session && ImGui::IsKeyPressed(ImGuiKey_Z, false))
					(void)m_DocumentEditors.DispatchFocusedCommand(*Session, PulseForgeEditor::EditorDocumentCommand::Undo);
				return;
			}
			if (MaterialDocumentFocused)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (ImGui::IsKeyPressed(ImGuiKey_Z, false) && IO.KeyShift))
					(void)ApplyMaterialHistoryStep(true);
				else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) (void)ApplyMaterialHistoryStep(false);
				return;
			}
			if (UnsupportedDocumentFocused) return;
			if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) RedoSceneEdit();
			else if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && IO.KeyShift) RedoSceneEdit();
			else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) UndoSceneEdit();
		}

		[[nodiscard]] bool IsContentBrowserFocused() const noexcept
		{
			if (!GImGui || !GImGui->NavWindow) return false;
			const ImGuiWindow* Root = GImGui->NavWindow->RootWindow;
			return Root && std::string_view(Root->Name) ==
							   PulseForgeEditor::EditorPanelDescriptors[static_cast<size_t>(PulseForgeEditor::EditorPanel::ContentBrowser)]
								   .WindowName;
		}

		[[nodiscard]] bool IsEditorPanelFocused(PulseForgeEditor::EditorPanel Panel) const noexcept
		{
			if (!GImGui || !GImGui->NavWindow) return false;
			const ImGuiWindow* Root = GImGui->NavWindow->RootWindow;
			if (Panel == PulseForgeEditor::EditorPanel::MaterialDetails) return Root && Root->ID == ImHashStr("Material Editor");
			return Root && std::string_view(Root->Name) == PulseForgeEditor::EditorPanelDescriptors[static_cast<size_t>(Panel)].WindowName;
		}

		[[nodiscard]] const PulseForgeEditor::EditorDocumentIdentity* GetFocusedDocumentIdentity() const noexcept
		{
			const auto* Host = m_Documents.GetFocusedHost();
			return Host && Host->ActiveDocument ? &*Host->ActiveDocument : nullptr;
		}

		[[nodiscard]] PulseForgeEditor::EditorDocumentSession* GetFocusedRegisteredDocument() noexcept
		{
			const auto* Identity = GetFocusedDocumentIdentity();
			if (!Identity || !m_DocumentEditors.IsRegistered(Identity->Kind)) return nullptr;
			auto* Session = m_Documents.Find(*Identity);
			return Session && Session->EditorState ? Session : nullptr;
		}

		[[nodiscard]] bool IsFocusedMaterialDocument() const noexcept { return GetFocusedMaterialAsset().has_value(); }

		[[nodiscard]] std::optional<PulseForge::AssetID> GetFocusedMaterialAsset() const noexcept
		{
			if (IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails)) return m_MaterialDetailsAsset;
			return std::nullopt;
		}

		void FocusDocumentHostIfChanged(std::string_view HostID)
		{
			const auto* Current = m_Documents.GetFocusedHost();
			if ((!Current || Current->ID != HostID) && m_Documents.FocusHost(HostID)) MarkDocumentWorkspaceDirty();
		}

		bool SaveFocusedDocument()
		{
			if (IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails))
				return m_MaterialDetailsAsset && SaveMaterialDraft(*m_MaterialDetailsAsset);
			if (auto* Session = GetFocusedRegisteredDocument()) return m_DocumentEditors.Save(*Session);
			if (const auto Material = GetFocusedMaterialAsset()) return SaveMaterialDraft(*Material);
			const auto* Identity = GetFocusedDocumentIdentity();
			const PulseForge::UUID CurrentProject = m_Project ? m_Project->GetDescription().ID : PulseForge::UUID{};
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
				m_ContentBrowserPanel.FocusSearch();
			}
			else
			{
				SetEditorPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy, true);
				m_HierarchyPanel.FocusSearchNextFrame();
			}
		}

		[[nodiscard]] bool CanRouteEditorShortcut() const { return CanRouteEditorInputShortcut(m_Scene != nullptr); }

		[[nodiscard]] bool CanRouteEditorInputShortcut(bool CommandTargetAvailable) const
		{
			const ImGuiIO& IO = ImGui::GetIO();
			return PulseForgeEditor::CanRouteEditorShortcut({ .SceneAvailable = CommandTargetAvailable,
				.RuntimeActive = m_SceneRuntime != nullptr,
				.CameraNavigationActive = m_ViewportMouseCapture.OwnsMouse(),
				.GizmoInteractionActive = m_ViewportGizmoInteraction.IsActive(),
				.TextInputActive = IO.WantTextInput,
				.ImGuiItemActive = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui && GImGui->NavLayer == ImGuiNavLayer_Menu,
				.WindowNavigationActive = GImGui && GImGui->NavWindowingTarget != nullptr });
		}

		void HandleSceneEditingShortcuts()
		{
			const ImGuiIO& IO = ImGui::GetIO();
			const bool CanFocusSearch = m_Project && CanRouteEditorInputShortcut(true) && !IO.KeyAlt && !IO.KeySuper;
			if (CanFocusSearch && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
			{
				FocusActivePanelSearch();
				return;
			}
			if (GetFocusedRegisteredDocument())
			{
				if (!IO.WantTextInput && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
					GImGui->NavLayer != ImGuiNavLayer_Menu && GImGui->NavWindowingTarget == nullptr && IO.KeyCtrl && !IO.KeyAlt &&
					!IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false))
				{
					if (IO.KeyShift) SetWarning("Save As is not available for this document type.");
					else (void)SaveFocusedDocument();
				}
				return;
			}
			if (IsFocusedMaterialDocument())
			{
				if (!IO.WantTextInput && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
					GImGui->NavLayer != ImGuiNavLayer_Menu && GImGui->NavWindowingTarget == nullptr && IO.KeyCtrl && !IO.KeyAlt &&
					!IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false))
				{
					if (IO.KeyShift) SetWarning("Save As is available for scenes; material documents use Save Material.");
					else (void)SaveFocusedDocument();
				}
				return;
			}
			if (IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails)) return;
			const PulseForge::UUID CurrentProject = m_Project ? m_Project->GetDescription().ID : PulseForge::UUID{};
			if (PulseForgeEditor::ResolveEditorDocumentCommandTarget(GetFocusedDocumentIdentity(), CurrentProject) ==
				PulseForgeEditor::EditorDocumentCommandTarget::Unsupported)
				return;
			if (!CanRouteEditorShortcut()) return;
			const bool HasUnsupportedModifier = IO.KeyAlt || IO.KeySuper;
			const bool Control = IO.KeyCtrl && !HasUnsupportedModifier;
			const bool Shift = IO.KeyShift;
			const bool ContentBrowser = IsContentBrowserFocused();

			if (Control)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !Shift && !ContentBrowser) ShowAllEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !ContentBrowser)
				{
					std::vector<PulseForge::UUID> Identifiers;
					for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
						Identifiers.push_back(Entity.GetUUID());
					SetEditorSelection(std::move(Identifiers));
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !ContentBrowser) (void)CopySelectedEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_X, false) && !ContentBrowser) CutSelectedEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_V, false) && !ContentBrowser) PasteEntities();
				else if (ImGui::IsKeyPressed(ImGuiKey_D, false))
				{
					if (ContentBrowser)
					{
						if (m_ContentBrowser.SelectedAsset()) DuplicateAsset(*m_ContentBrowser.SelectedAsset());
					}
					else DuplicateSelectedEntity();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
				{
					if (Shift) CreateScene(true);
					else (void)SaveFocusedDocument();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_N, false) && Shift) CreateEntityFromEditor();
			}
			else if (!HasUnsupportedModifier)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
				{
					if (ContentBrowser)
					{
						if (m_ContentBrowser.SelectedAsset()) RequestDeleteAsset(*m_ContentBrowser.SelectedAsset());
					}
					else DeleteSelectedEntity();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && ContentBrowser && m_ContentBrowser.SelectedAsset())
					MoveAsset(*m_ContentBrowser.SelectedAsset());
				else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && SelectedEntity() && !ContentBrowser)
				{
					m_InspectorPanel.FocusEntityNameNextFrame();
					SetEditorPanelVisible(PulseForgeEditor::EditorPanel::Inspector, true);
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FocusSelection();
				else if (ImGui::IsKeyPressed(ImGuiKey_End, false) && !ContentBrowser) SnapSelectionToSurface();
				else if (ImGui::IsKeyPressed(ImGuiKey_H, false))
				{
					if (Shift) IsolateSelectedEntities();
					else HideSelectedEntities();
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_G, false))
				{
					m_ShowEditorOverlays = !m_ShowEditorOverlays;
					SetStatus(m_ShowEditorOverlays ? "Editor viewport overlays enabled." : "Game View enabled.");
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) SelectSingleEntity(std::nullopt);
			}
		}

		[[nodiscard]] GLFWwindow* GetDialogParentWindow() const
		{
			return static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
		}

		std::optional<std::filesystem::path> ShowOpenDialog(
			const PulseForgeEditor::EditorFileDialogFilter* Filter, const std::filesystem::path& DefaultPath = {})
		{
			if (!m_FileDialogActive)
			{
				SetError("Native file dialogs are unavailable. See the Console for initialization diagnostics.");
				return std::nullopt;
			}

			auto Result = PulseForgeEditor::EditorPlatformServices::ShowOpenFileDialog(GetDialogParentWindow(), Filter, DefaultPath);
			if (!Result)
			{
				SetError("Could not open the file dialog: " + Result.error());
				return std::nullopt;
			}
			return std::move(*Result);
		}

		std::optional<std::filesystem::path> ShowSaveDialog(
			const PulseForgeEditor::EditorFileDialogFilter* Filter, const std::filesystem::path& DefaultPath, const char* DefaultName)
		{
			if (!m_FileDialogActive)
			{
				SetError("Native file dialogs are unavailable. See the Console for initialization diagnostics.");
				return std::nullopt;
			}

			auto Result = PulseForgeEditor::EditorPlatformServices::ShowSaveFileDialog(
				GetDialogParentWindow(), Filter, DefaultPath, DefaultName ? std::string_view(DefaultName) : std::string_view{});
			if (!Result)
			{
				SetError("Could not open the save dialog: " + Result.error());
				return std::nullopt;
			}
			return std::move(*Result);
		}

		void CreateProject()
		{
			static constexpr PulseForgeEditor::EditorFileDialogFilter Filter{ "PulseForge Project", "pfproj" };
			auto ProjectFile = ShowSaveDialog(&Filter, {}, "NewProject.pfproj");
			if (!ProjectFile) return;
			if (ProjectFile->extension().empty()) *ProjectFile += ".pfproj";
			const std::filesystem::path ChosenPath = *ProjectFile;
			QueueAfterSave([this, ChosenPath] { CreateProjectAt(ChosenPath); });
		}

		bool ClearCurrentProjectDocumentState(std::function<void()> OnDocumentsClosed = {})
		{
			if (!FinishSceneEdit()) return false;
			if (m_Project)
			{
				if (!SaveDocumentWorkspaceNow()) return false;
				std::vector<PulseForgeEditor::EditorDocumentIdentity> ProjectDocuments;
				for (const auto& [Identity, Session] : m_Documents.GetSessions())
					if (Identity.Project == m_Project->GetDescription().ID && Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level)
						ProjectDocuments.push_back(Identity);
				if (!CloseDocumentBatchClean(ProjectDocuments))
				{
					const std::string CloseError = m_DocumentCloseRequest.GetError();
					m_DocumentCloseRequest.BeginCloseDocuments(std::move(ProjectDocuments),
						std::string(PulseForgeEditor::EditorDocumentManager::MainHostID), std::move(OnDocumentsClosed));
					m_DocumentCloseRequest.SetError(CloseError);
					return false;
				}
				if (auto Cleared = m_Documents.ClearProject(m_Project->GetDescription().ID); !Cleared)
				{
					SetError("Could not clear project document state: " + Cleared.error());
					return false;
				}
				ClearMaterialDraftsForProject();
			}
			m_LevelDocumentIdentity.reset();
			m_MaterialDetailsAsset.reset();
			m_InitialMaterialDetailsDockPending = false;
			m_ForceMaterialDetailsDock = false;
			m_FocusMaterialDetailsNextFrame = false;
			m_DocumentWorkspaceStore.Clear();
			m_RestoredHostBoundsPending.clear();
			m_AssetPreviewProjectPending = true;
			return true;
		}

		void RegisterLevelDocument()
		{
			if (!m_Project) return;
			m_LevelDocumentIdentity = PulseForgeEditor::EditorDocumentIdentity{ m_Project->GetDescription().ID, PulseForge::UUID{},
				PulseForgeEditor::EditorDocumentKind::Level };
			auto LevelDocument = m_Documents.Open(
				*m_LevelDocumentIdentity, {}, "Level Editor", PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip);
			if (!LevelDocument) SetWarning("The Level Editor document session could not be registered: " + LevelDocument.error());
		}

		bool ActivatePreparedProject(PulseForgeEditor::PreparedEditorProject&& Prepared, bool ClearSceneWhenMissing)
		{
			if (!FinishSceneEdit()) return false;
			auto StartupScene = std::move(Prepared.StartupScene);
			const auto StartupSceneAsset = Prepared.StartupSceneAsset;
			StopRuntime();
			ResetViewportSceneRenderer();
			m_Project.emplace(std::move(Prepared.Project));
			RegisterLevelDocument();
			ResetContentBrowserNavigation();
			if (ClearSceneWhenMissing && !StartupScene) ClearScene();
			else
			{
				m_Scene = std::move(StartupScene);
				m_SceneAsset = StartupSceneAsset;
				SelectSingleEntity(std::nullopt);
				m_ContentBrowser.SelectedAsset() = StartupSceneAsset;
				m_AssetWorkflow.DismissDelete();
				ResetSceneHistory();
			}
			UpdateAssetList();
			RestoreDocumentWorkspace();
			return true;
		}

		void CreateProjectAt(const std::filesystem::path& ProjectFile)
		{
			auto Created = PulseForgeEditor::PrepareEditorProjectCreation(ProjectFile);
			if (!Created)
			{
				SetError(Created.error().Message);
				return;
			}

			const std::string ProjectName = Created->Project.GetDescription().Name;
			if (!ClearCurrentProjectDocumentState([this, ProjectFile] { (void)OpenProjectAt(ProjectFile); })) return;

			if (auto StartSceneWarning = PulseForgeEditor::CommitPreparedProjectSettings(*Created))
				Created->StartupSceneWarning = std::move(*StartSceneWarning);
			std::string SceneError = std::move(Created->StartupSceneError);
			std::string SceneWarning = std::move(Created->StartupSceneWarning);
			if (!ActivatePreparedProject(std::move(*Created), true)) return;
			if (!SceneError.empty()) SetError(std::move(SceneError));
			else if (!SceneWarning.empty()) SetWarning(std::move(SceneWarning));
			else SetStatus("Created project " + ProjectName + " with Assets/Main.scene.");
		}

		void OpenProject()
		{
			static constexpr PulseForgeEditor::EditorFileDialogFilter Filter{ "PulseForge Project", "pfproj" };
			auto ProjectFile = ShowOpenDialog(&Filter);
			if (!ProjectFile) return;
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
			m_ViewportRenderView.ClearEnvironmentLightingPending();
			PulseForge::Detail::ScopedProfileTimer OpenTimer("Editor project open + startup scene load");
			auto Opened = PulseForgeEditor::PrepareEditorProjectOpen(ProjectFile);
			if (!Opened)
			{
				SetError(Opened.error().Message);
				return std::unexpected(Opened.error().Message);
			}

			const std::string ProjectName = Opened->Project.GetDescription().Name;
			if (!ClearCurrentProjectDocumentState([this, ProjectFile] { (void)OpenProjectAt(ProjectFile); }))
				return std::unexpected("The current project has a document session that could not close");
			std::string StartupSceneWarning = std::move(Opened->StartupSceneWarning);
			if (!ActivatePreparedProject(std::move(*Opened), false))
				return std::unexpected("The current scene edit could not be completed before project activation");
			InitializeDocumentHostTestFixture();
			if (StartupSceneWarning.empty()) SetStatus("Opened project " + ProjectName + ".");
			else SetWarning(std::move(StartupSceneWarning));
			return {};
		}

		[[nodiscard]] PulseForgeEditor::EditorDocumentEditorContext MakeDocumentEditorContext(
			const PulseForgeEditor::EditorDocumentIdentity& Identity, std::filesystem::path ProjectRelativePath,
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

		[[nodiscard]] bool IsDocumentHostTestFixtureIdentity(const PulseForgeEditor::EditorDocumentIdentity& Identity) const
		{
#if !defined(_DEBUG) && !defined(PF_DEBUG)
			(void)Identity;
			return false;
#else
			return m_DocumentHostTestFixture && m_DocumentHostTestEditor && m_DocumentHostTestEditor->IsFixtureIdentity(Identity);
#endif
		}

		void InitializeDocumentHostTestFixture()
		{
#if !defined(_DEBUG) && !defined(PF_DEBUG)
			return;
#else
			if (!m_DocumentHostTestFixture || !m_Project) return;

			// These synthetic Texture-kind identities exercise registered document dispatch in Debug.
			const auto FixtureAssetIDs = PulseForgeEditor::Detail::EditorDocumentHostTestEditor::FixtureAssetIDs();
			const auto Preference = m_Documents.GetOpeningPreference();
			std::unordered_set<std::string> FixtureHosts;
			m_DocumentHostFixtureSessions.clear();
			size_t OpenedCount = 0;
			size_t RestoredCount = 0;
			for (size_t Index = 0; Index < FixtureAssetIDs.size(); ++Index)
			{
				const auto AssetID = PulseForge::UUID::Parse(FixtureAssetIDs[Index]);
				if (!AssetID) continue;
				const PulseForgeEditor::EditorDocumentIdentity Identity{ m_Project->GetDescription().ID, *AssetID,
					PulseForgeEditor::EditorDocumentKind::Texture };
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
					const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
					if (!Session || !MainHost)
					{
						Prepared = false;
						break;
					}
					const std::string SourceHostID = Session->HostID;
					const auto Transfer =
						m_Documents.Transfer(Identity, PulseForgeEditor::EditorDocumentManager::MainHostID, MainHost->Documents.size());
					if (!Transfer)
					{
						Prepared = false;
						PF_ERROR(
							"Document tab reorder fixture could not place its synthetic session in the main strip: {}", Transfer.error());
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
			if (m_DocumentWorkspaceSwitchTest) PrepareDocumentWorkspaceSwitchTest();
			MarkDocumentWorkspaceDirty();
			PF_WARN("Development document-host fixture enabled with opening preference {}: {} sessions restored from saved "
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
						const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
						const size_t DestinationIndex = MainHost ? MainHost->Documents.size() : 0;
						const std::string SourceHostID = Session->HostID;
						const auto Transfer = m_Documents.Transfer(
							m_DocumentHostTabTearOffTestIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID, DestinationIndex);
						if (!Transfer)
						{
							m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
							PF_ERROR("Document tab tear-off fixture could not prepare its source tab: {}", Transfer.error());
						}
						else
						{
							if (Transfer->EmptiedSourceHost) (void)m_Documents.RemoveEmptyNativeHost(SourceHostID);
							m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::WaitingForTab;
						}
					}
					else m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::WaitingForTab;
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
							PF_ERROR("Document tab reattachment UI fixture failed: it could not resolve two synthetic sessions and "
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
								PF_ERROR("Document tab reattachment UI fixture failed to create a separate grouping host: {}",
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
									PF_ERROR("Document tab reattachment UI fixture failed to prepare a separate grouping host: {}",
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
					if (!m_DocumentHostTabReattachTest) return;
					m_DocumentHostTabReattachTestIdentity = m_DocumentHostFixtureSessions.front().first;
					const auto* Session = m_Documents.Find(m_DocumentHostTabReattachTestIdentity);
					const auto* Host = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
					if (!Session || !Host)
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
						m_DocumentHostTabReattachTest = false;
						PF_ERROR("Document tab reattachment UI fixture failed to resolve its synthetic session host.");
					}
					else if (Host->Role == PulseForgeEditor::EditorDocumentHostRole::Main)
					{
						auto NativeHost = m_Documents.CreateNativeHost(m_DocumentHostTabReattachTestIdentity.Kind);
						if (!NativeHost)
						{
							m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
							m_DocumentHostTabReattachTest = false;
							PF_ERROR("Document tab reattachment UI fixture failed to create a native source host: {}", NativeHost.error());
						}
						else
						{
							m_DocumentHostTabReattachTestSourceHostID = *NativeHost;
							m_DocumentHostTabReattachExpectSourceHostRetired = true;
							const auto Transfer = m_Documents.Transfer(m_DocumentHostTabReattachTestIdentity, *NativeHost, 0);
							if (!Transfer)
							{
								(void)m_Documents.RemoveEmptyNativeHost(*NativeHost);
								m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
								m_DocumentHostTabReattachTest = false;
								PF_ERROR(
									"Document tab reattachment UI fixture failed to prepare its native source host: {}", Transfer.error());
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
#if !defined(_DEBUG) && !defined(PF_DEBUG)
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
				const auto Transfer =
					m_Documents.Transfer(Identity, PulseForgeEditor::EditorDocumentManager::MainHostID, MainHost->Documents.size());
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
				if (Identity.Project == m_Project->GetDescription().ID && Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
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
				return Session && m_DocumentHostTestEditor && m_DocumentHostTestEditor->InitializeSavedModel(*Session, Value);
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
			if (!FirstSession || !SecondSession || !m_Documents.Activate(m_DocumentWorkspaceSwitchFirstIdentity))
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
			m_DocumentWorkspaceSwitchSceneViewportVisible = m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport);
			m_DocumentWorkspaceSwitchHierarchyVisible = m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy);
			m_DocumentWorkspaceSwitchInspectorVisible = m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Inspector);
			m_DocumentWorkspaceSwitchContentBrowserVisible = m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::ContentBrowser);
			m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForFirstFixture;
			PF_INFO("Document workspace switch fixture opened two independent registered sessions in the main document strip.");
#endif
		}

		void AdvanceDocumentWorkspaceSwitchTest()
		{
			if (!m_DocumentWorkspaceSwitchTest || m_DocumentWorkspaceSwitchTestPhase == DocumentWorkspaceSwitchTestPhase::Disabled) return;
#if !defined(_DEBUG) && !defined(PF_DEBUG)
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
			{ return m_WorkspacePresentation.WasWorkspaceDrawn(Identity, HostID); };
			const auto ViewportInteractionReleased = [this]
			{
				return !m_ViewportMouseCapture.OwnsMouse() && !m_ViewportKeyboardFocused &&
					   m_ViewportCameraGesture == ViewportCameraGesture::None && !m_ViewportGizmoInteraction.IsActive();
			};
			const auto MainLayoutRetained = [this]
			{
				return m_DocumentWorkspaceSwitchMainDockspaceID != 0 &&
					   ImGui::DockBuilderGetNode(m_DocumentWorkspaceSwitchMainDockspaceID) != nullptr;
			};
			const auto FixtureStatePreserved = [this](const auto& Identity, const auto* ExpectedState, int ExpectedValue)
			{
				const auto* Session = m_Documents.Find(Identity);
				return Session && m_DocumentHostTestEditor &&
					   m_DocumentHostTestEditor->MatchesSavedModel(*Session, ExpectedState, ExpectedValue);
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
					HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID))
				{
					if (!MainLayoutRetained())
					{
						Fail("the Level Editor dockspace was lost while the fixture workspace was active");
						break;
					}
					if (!Activate(m_DocumentWorkspaceSwitchLevelIdentity, "the Level Editor could not be activated")) break;
					m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForLevel;
				}
				break;
			case DocumentWorkspaceSwitchTestPhase::WaitingForLevel:
				if (MainHostActive() == m_DocumentWorkspaceSwitchLevelIdentity && m_WorkspacePresentation.IsLevelWorkspaceActive())
				{
					if (!ViewportInteractionReleased() || !MainLayoutRetained())
					{
						Fail("viewport interaction or the Level Editor layout was not preserved on return");
						break;
					}
					if (!Activate(m_DocumentWorkspaceSwitchFirstIdentity, "the first fixture could not be reactivated")) break;
					m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForFirstFixtureAgain;
				}
				break;
			case DocumentWorkspaceSwitchTestPhase::WaitingForFirstFixtureAgain:
				if (MainHostActive() == m_DocumentWorkspaceSwitchFirstIdentity &&
					HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID))
				{
					if (!FixtureStatePreserved(m_DocumentWorkspaceSwitchFirstIdentity, m_DocumentWorkspaceSwitchFirstState, 41) ||
						!ViewportInteractionReleased())
					{
						Fail("the first session's identity, value, history, or input isolation changed");
						break;
					}
					if (!Activate(m_DocumentWorkspaceSwitchSecondIdentity, "the second fixture could not be activated")) break;
					m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForSecondFixture;
				}
				break;
			case DocumentWorkspaceSwitchTestPhase::WaitingForSecondFixture:
				if (MainHostActive() == m_DocumentWorkspaceSwitchSecondIdentity &&
					HasWorkspaceDrawn(m_DocumentWorkspaceSwitchSecondIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID))
				{
					if (!FixtureStatePreserved(m_DocumentWorkspaceSwitchSecondIdentity, m_DocumentWorkspaceSwitchSecondState, 82) ||
						!ViewportInteractionReleased())
					{
						Fail("the second session did not retain independent state and history");
						break;
					}
					if (!Activate(m_DocumentWorkspaceSwitchFirstIdentity, "the first fixture could not be activated before transfer"))
						break;
					m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForNativeTransfer;
				}
				break;
			case DocumentWorkspaceSwitchTestPhase::WaitingForNativeTransfer:
				if (MainHostActive() == m_DocumentWorkspaceSwitchFirstIdentity &&
					HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID))
				{
					const auto NativeHost = m_Documents.CreateNativeHost(PulseForgeEditor::EditorDocumentKind::Texture);
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
					m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForMainTransfer;
				}
				break;
			case DocumentWorkspaceSwitchTestPhase::WaitingForMainTransfer:
			{
				const auto* Session = m_Documents.Find(m_DocumentWorkspaceSwitchFirstIdentity);
				if (Session && Session->HostID == m_DocumentWorkspaceSwitchNativeHostID &&
					HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity, m_DocumentWorkspaceSwitchNativeHostID))
				{
					if (MainHostActive() != m_DocumentWorkspaceSwitchLevelIdentity || !m_WorkspacePresentation.IsLevelWorkspaceActive() ||
						!ViewportInteractionReleased())
					{
						Fail("the Level Editor did not remain available while the native fixture workspace was active");
						break;
					}
					const auto Transfer = m_Documents.Transfer(
						m_DocumentWorkspaceSwitchFirstIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID, 1);
					if (!Transfer || !Transfer->EmptiedSourceHost)
					{
						Fail("the native fixture host did not reattach cleanly to the main document strip");
						break;
					}
					(void)m_Documents.RemoveEmptyNativeHost(*Transfer->EmptiedSourceHost);
					if (!Activate(m_DocumentWorkspaceSwitchFirstIdentity, "the reattached fixture could not be activated")) break;
					MarkDocumentWorkspaceDirty(true);
					m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::WaitingForMainTransfer;
				}
				else if (MainHostActive() == m_DocumentWorkspaceSwitchLevelIdentity && m_WorkspacePresentation.IsLevelWorkspaceActive() &&
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
				m_Documents.Find(m_DocumentWorkspaceSwitchFirstIdentity)->HostID == PulseForgeEditor::EditorDocumentManager::MainHostID &&
				HasWorkspaceDrawn(m_DocumentWorkspaceSwitchFirstIdentity, PulseForgeEditor::EditorDocumentManager::MainHostID))
			{
				if (m_Documents.Find(m_DocumentWorkspaceSwitchFirstIdentity) != m_DocumentWorkspaceSwitchFirstSession ||
					!FixtureStatePreserved(m_DocumentWorkspaceSwitchFirstIdentity, m_DocumentWorkspaceSwitchFirstState, 41) ||
					!ViewportInteractionReleased() || !MainLayoutRetained() || m_Scene.get() != m_DocumentWorkspaceSwitchScene ||
					m_RuntimeScene.get() != m_DocumentWorkspaceSwitchRuntimeScene ||
					SelectedEntity() != m_DocumentWorkspaceSwitchSelectedEntity ||
					glm::length(m_EditorCamera.GetPosition() - m_DocumentWorkspaceSwitchCameraPosition) > 0.0001f ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport) !=
						m_DocumentWorkspaceSwitchSceneViewportVisible ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy) != m_DocumentWorkspaceSwitchHierarchyVisible ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Inspector) != m_DocumentWorkspaceSwitchInspectorVisible ||
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::ContentBrowser) !=
						m_DocumentWorkspaceSwitchContentBrowserVisible)
				{
					Fail("document transfer changed workspace state, scene state, camera, selection, or panel visibility");
					return;
				}
				m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::Complete;
				m_DocumentWorkspaceSwitchTest = false;
				PF_INFO("Document workspace switch fixture passed: Level -> fixture 1 -> Level -> fixture 1 -> fixture 2, then native/main "
						"transfer preserved session, model history, viewport state, and the hidden Level layout");
			}
#endif
		}

		void QueueDocumentHostTabTearOffTestInput()
		{
			if (!m_DocumentHostTabTearOffTest) return;
#if !defined(_DEBUG) && !defined(PF_DEBUG)
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
				else if (m_DocumentHostTabTearOffPointerPrepared)
					PF_ERROR("Document tab tear-off UI fixture timed out waiting for the focused source window to become hovered.");
				else PF_ERROR("Document tab tear-off UI fixture timed out before creating its native host.");
				m_DocumentHostTabRoundTripTearOffPending = false;
				return;
			}
			switch (m_DocumentHostTabTearOffTestPhase)
			{
			case DocumentTabTearOffTestPhase::WaitingForTab:
				if (m_DocumentHostTabTearOffTestTabCenterValid)
				{
					ImGuiViewport* SourceViewport = ImGui::FindViewportByID(m_DocumentHostTabTearOffTestTabViewportID);
					GLFWwindow* SourceWindow = SourceViewport ? static_cast<GLFWwindow*>(SourceViewport->PlatformHandle) : nullptr;
					if (!SourceWindow)
					{
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
						m_DocumentHostTabTearOffTest = false;
						PF_ERROR("Document tab tear-off UI fixture could not find the source platform window.");
						break;
					}
					if (!m_DocumentHostTabTearOffPointerPrepared)
					{
						glfwFocusWindow(SourceWindow);
						glfwSetCursorPos(SourceWindow, m_DocumentHostTabTearOffTestTabCenter.x - SourceViewport->Pos.x,
							m_DocumentHostTabTearOffTestTabCenter.y - SourceViewport->Pos.y);
						m_DocumentHostTabTearOffPointerPrepared = true;
						break;
					}
					if (glfwGetWindowAttrib(SourceWindow, GLFW_HOVERED) != GLFW_TRUE) break;
					m_DocumentHostTabTearOffTestStart = m_DocumentHostTabTearOffTestTabCenter;
					const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
					if (!MainViewport || MainViewport->Size.x <= 0.0f || MainViewport->Size.y <= 0.0f)
					{
						m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
						PF_ERROR("Document tab tear-off UI fixture has no visible main viewport.");
						break;
					}
					m_DocumentHostTabTearOffTestDrop =
						m_DocumentHostTabTearOffFromNativeHost
							? ImVec2(
								  m_DocumentHostTabTearOffTestStart.x + IO.MouseDragThreshold + 24.0f, m_DocumentHostTabTearOffTestStart.y)
							: ImVec2(MainViewport->Pos.x + MainViewport->Size.x * 0.5f, MainViewport->Pos.y + MainViewport->Size.y * 0.75f);
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
				if (m_DocumentHostTabTearOffFromNativeHost) IO.AddMouseViewportEvent(0);
				m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::DragQueued;
				break;
			case DocumentTabTearOffTestPhase::DragQueued:
				if (m_DocumentHostTabTearOffFromNativeHost)
				{
					IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
					IO.AddMouseViewportEvent(0);
				}
				else
				{
					IO.AddMousePosEvent(m_DocumentHostTabTearOffTestDrop.x, m_DocumentHostTabTearOffTestDrop.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabTearOffTestTabViewportID);
				}
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::ReleaseQueued;
				break;
			case DocumentTabTearOffTestPhase::ReleaseQueued:
			{
				const auto* Session = m_Documents.Find(m_DocumentHostTabTearOffTestIdentity);
				const auto* Host = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
				const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				const bool LevelRetained =
					MainHost && std::any_of(MainHost->Documents.begin(), MainHost->Documents.end(),
									[](const auto& Identity) { return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level; });
				const std::string_view ExpectedPreviousHostID = m_DocumentHostTabTearOffFromNativeHost
																	? std::string_view(m_DocumentHostTabTearOffSourceHostID)
																	: PulseForgeEditor::EditorDocumentManager::MainHostID;
				const bool EnteredDifferentNativeHost = Session && Host && Host->Role == PulseForgeEditor::EditorDocumentHostRole::Native &&
														Session->HostID != ExpectedPreviousHostID;
				const auto* PreviousNativeHost =
					m_DocumentHostTabTearOffFromNativeHost ? m_Documents.FindHost(m_DocumentHostTabTearOffSourceHostID) : nullptr;
				const bool PreviousNativeGroupIntact =
					!m_DocumentHostTabTearOffFromNativeHost ||
					(PreviousNativeHost && PreviousNativeHost->Role == PulseForgeEditor::EditorDocumentHostRole::Native &&
						PreviousNativeHost->Documents.size() == 1 &&
						PreviousNativeHost->Documents.front() == m_DocumentHostTabGroupTargetIdentity);
				if (EnteredDifferentNativeHost && PreviousNativeGroupIntact && LevelRetained)
				{
					m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Complete;
					if (m_DocumentHostTabTearOffFromNativeHost)
						PF_INFO("Document tab native-group tear-off UI fixture passed: the tab entered a new native host and the original "
								"group remained intact.");
					else if (m_DocumentHostTabRoundTripTearOffPending)
					{
						m_DocumentHostTabRoundTripTearOffPending = false;
						++m_DocumentHostTabRoundTripCount;
						PF_INFO("Document tab native-window round-trip {}/{} passed: the same session was dragged into the main "
								"strip and back into a native host, while Level Editor remained in the main host.",
							m_DocumentHostTabRoundTripCount, DocumentTabRoundTripCycleCount);
						const bool HasGroupTarget = m_DocumentHostFixtureSessions.size() >= 2;
						const auto* GroupTargetSession =
							HasGroupTarget ? m_Documents.Find(m_DocumentHostFixtureSessions[1].first) : nullptr;
						const auto* GroupTargetHost = GroupTargetSession ? m_Documents.FindHost(GroupTargetSession->HostID) : nullptr;
						if (GroupTargetSession && GroupTargetHost &&
							GroupTargetHost->Role == PulseForgeEditor::EditorDocumentHostRole::Native && GroupTargetHost->ID != Host->ID &&
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
						PF_INFO("Document tab tear-off UI fixture passed: releasing the captured asset tab in the editor body created a native "
								"host and kept Level Editor in the main host.");
					}
					m_DocumentHostTabTearOffFromNativeHost = false;
					m_DocumentHostTabTearOffSourceHostID.clear();
				}
				else
				{
					m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Failed;
					if (m_DocumentHostTabTearOffFromNativeHost)
						PF_ERROR("Document tab native-group tear-off fixture failed: the tab did not enter a separate native host or the "
								 "original group changed.");
					else if (m_DocumentHostTabRoundTripTearOffPending)
						PF_ERROR("Document tab native-window round-trip fixture failed: the reattached session did not return to a native "
								 "host or Level Editor left the main host.");
					else
						PF_ERROR("Document tab tear-off UI fixture failed: the released tab did not enter a native host or Level Editor "
								 "left the main host.");
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
			if (!m_DocumentHostTabReattachTest) return;
#if !defined(_DEBUG) && !defined(PF_DEBUG)
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
				if (!m_DocumentHostTabReattachTestTabCenterValid) break;
				if (m_DocumentHostTabReattachInvalidDropIndex < m_DocumentHostTabReattachInvalidDropTargets.size())
				{
					DocumentTabTestPointerTarget& Target =
						m_DocumentHostTabReattachInvalidDropTargets[m_DocumentHostTabReattachInvalidDropIndex];
					if (!Target.Available) break;
					const float DeltaX = Target.Position.x - m_DocumentHostTabReattachTestTabCenter.x;
					const float DeltaY = Target.Position.y - m_DocumentHostTabReattachTestTabCenter.y;
					if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
						PF_ERROR("Document tab reattachment fixture could not place a drag over {} past the threshold.", Target.Name);
						break;
					}
					IO.AddMousePosEvent(m_DocumentHostTabReattachTestTabCenter.x, m_DocumentHostTabReattachTestTabCenter.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestTabViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::InvalidDropPressQueued;
				}
				else if (m_DocumentHostTabReattachTestMainDropPositionValid)
				{
					const float DeltaX = m_DocumentHostTabReattachTestMainDropPosition.x - m_DocumentHostTabReattachTestTabCenter.x;
					const float DeltaY = m_DocumentHostTabReattachTestMainDropPosition.y - m_DocumentHostTabReattachTestTabCenter.y;
					if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
					{
						m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
						PF_ERROR("Document tab reattachment fixture could not place a valid strip drop past the drag threshold.");
						break;
					}
					IO.AddMousePosEvent(m_DocumentHostTabReattachTestTabCenter.x, m_DocumentHostTabReattachTestTabCenter.y);
					IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestTabViewportID);
					IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::ValidDropPressQueued;
					PF_INFO("Document tab reattachment fixture is returning the native tab to the main strip.");
				}
				break;
			case DocumentTabReattachTestPhase::InvalidDropPressQueued:
			{
				const auto& Target = m_DocumentHostTabReattachInvalidDropTargets[m_DocumentHostTabReattachInvalidDropIndex];
				IO.AddMousePosEvent(Target.Position.x, Target.Position.y);
				IO.AddMouseViewportEvent(Target.ViewportID);
				m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::InvalidDropPositionQueued;
				break;
			}
			case DocumentTabReattachTestPhase::InvalidDropPositionQueued:
			{
				const auto& Target = m_DocumentHostTabReattachInvalidDropTargets[m_DocumentHostTabReattachInvalidDropIndex];
				IO.AddMousePosEvent(Target.Position.x, Target.Position.y);
				IO.AddMouseViewportEvent(Target.ViewportID);
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::InvalidDropReleaseQueued;
				break;
			}
			case DocumentTabReattachTestPhase::InvalidDropReleaseQueued:
			{
				const auto& Target = m_DocumentHostTabReattachInvalidDropTargets[m_DocumentHostTabReattachInvalidDropIndex];
				const auto* Session = m_Documents.Find(m_DocumentHostTabReattachTestIdentity);
				const bool StillInSourceHost = Session && Session->HostID == m_DocumentHostTabReattachTestSourceHostID;
				const bool NoRegisteredStripTarget = !FindDocumentTabDropTarget(Target.Position, Target.ViewportID);
				if (!StillInSourceHost || !NoRegisteredStripTarget)
				{
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Failed;
					PF_ERROR("Document tab drag over {} changed document membership or matched a registered strip.", Target.Name);
					break;
				}
				PF_INFO("Document tab drag over {} was rejected without changing host membership.", Target.Name);
				++m_DocumentHostTabReattachInvalidDropIndex;
				m_DocumentHostTabReattachTestTabCenterValid = false;
				m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::WaitingForTab;
				break;
			}
			case DocumentTabReattachTestPhase::ValidDropPressQueued:
				IO.AddMousePosEvent(m_DocumentHostTabReattachTestMainDropPosition.x, m_DocumentHostTabReattachTestMainDropPosition.y);
				IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestMainViewportID);
				m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::ValidDropPositionQueued;
				break;
			case DocumentTabReattachTestPhase::ValidDropPositionQueued:
				IO.AddMousePosEvent(m_DocumentHostTabReattachTestMainDropPosition.x, m_DocumentHostTabReattachTestMainDropPosition.y);
				IO.AddMouseViewportEvent(m_DocumentHostTabReattachTestMainViewportID);
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::ValidDropReleaseQueued;
				break;
			case DocumentTabReattachTestPhase::ValidDropReleaseQueued:
			{
				const auto* Session = m_Documents.Find(m_DocumentHostTabReattachTestIdentity);
				const auto* Host = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
				const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				const bool LevelRetained =
					MainHost && std::any_of(MainHost->Documents.begin(), MainHost->Documents.end(),
									[](const auto& Identity) { return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level; });
				const bool SourceHostRetired = m_Documents.FindHost(m_DocumentHostTabReattachTestSourceHostID) == nullptr;
				const bool SourceHostStateMatches =
					m_DocumentHostTabReattachExpectSourceHostRetired ? SourceHostRetired : !SourceHostRetired;
				if (Session && Host && Host->Role == PulseForgeEditor::EditorDocumentHostRole::Main && SourceHostStateMatches &&
					LevelRetained)
				{
					m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Complete;
					PF_INFO("Document tab reattachment UI fixture passed: {} non-strip drops were rejected, then the tab "
							"returned to the main strip; source-host retirement matched its remaining membership. Testing "
							"whether it can be dragged back into a native host (round trip {}/{}).",
						m_DocumentHostTabReattachInvalidDropIndex, m_DocumentHostTabRoundTripCount + 1, DocumentTabRoundTripCycleCount);
					m_DocumentHostTabTearOffTestIdentity = m_DocumentHostTabReattachTestIdentity;
					m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::WaitingForTab;
					m_DocumentHostTabTearOffPointerPrepared = false;
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
			if (!m_DocumentHostTabReorderTest) return;
#if !defined(_DEBUG) && !defined(PF_DEBUG)
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
				const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				const auto* FirstSession = m_Documents.Find(m_DocumentHostTabReorderTestFirst);
				const auto* SecondSession = m_Documents.Find(m_DocumentHostTabReorderTestSecond);
				if (!MainHost || !FirstSession || !SecondSession || FirstSession->HostID != MainHost->ID ||
					SecondSession->HostID != MainHost->ID || MainHost->ActiveDocument != m_DocumentHostTabReorderTestFirst)
					return false;
				const auto First = std::find(MainHost->Documents.begin(), MainHost->Documents.end(), m_DocumentHostTabReorderTestFirst);
				const auto Second = std::find(MainHost->Documents.begin(), MainHost->Documents.end(), m_DocumentHostTabReorderTestSecond);
				const bool HasLevel = std::any_of(MainHost->Documents.begin(), MainHost->Documents.end(),
					[](const auto& Identity) { return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level; });
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
				if (!m_DocumentHostTabReorderTestFirstTabCenterValid || !m_DocumentHostTabReorderTestSecondAfterPositionValid) break;
				QueuePointer(m_DocumentHostTabReorderTestFirstTabCenter, m_DocumentHostTabReorderTestFirstTabViewportID);
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ForwardPressQueued;
				break;
			case DocumentTabReorderTestPhase::ForwardPressQueued:
			{
				const float DeltaX = m_DocumentHostTabReorderTestSecondAfterPosition.x - m_DocumentHostTabReorderTestFirstTabCenter.x;
				const float DeltaY = m_DocumentHostTabReorderTestSecondAfterPosition.y - m_DocumentHostTabReorderTestFirstTabCenter.y;
				if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
				{
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
					m_DocumentHostTabReorderTest = false;
					PF_ERROR("Document tab reorder fixture could not drag the first tab past the threshold.");
					break;
				}
				QueuePointer(m_DocumentHostTabReorderTestSecondAfterPosition, m_DocumentHostTabReorderTestFirstTabViewportID);
				m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ForwardPositionQueued;
				break;
			}
			case DocumentTabReorderTestPhase::ForwardPositionQueued:
				QueuePointer(m_DocumentHostTabReorderTestSecondAfterPosition, m_DocumentHostTabReorderTestFirstTabViewportID);
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
				if (!m_DocumentHostTabReorderTestFirstTabCenterValid || !m_DocumentHostTabReorderTestSecondBeforePositionValid) break;
				QueuePointer(m_DocumentHostTabReorderTestFirstTabCenter, m_DocumentHostTabReorderTestFirstTabViewportID);
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ReversePressQueued;
				break;
			case DocumentTabReorderTestPhase::ReversePressQueued:
			{
				const float DeltaX = m_DocumentHostTabReorderTestSecondBeforePosition.x - m_DocumentHostTabReorderTestFirstTabCenter.x;
				const float DeltaY = m_DocumentHostTabReorderTestSecondBeforePosition.y - m_DocumentHostTabReorderTestFirstTabCenter.y;
				if (DeltaX * DeltaX + DeltaY * DeltaY <= IO.MouseDragThreshold * IO.MouseDragThreshold)
				{
					m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Failed;
					m_DocumentHostTabReorderTest = false;
					PF_ERROR("Document tab reorder fixture could not drag the first tab back past the threshold.");
					break;
				}
				QueuePointer(m_DocumentHostTabReorderTestSecondBeforePosition, m_DocumentHostTabReorderTestFirstTabViewportID);
				m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::ReversePositionQueued;
				break;
			}
			case DocumentTabReorderTestPhase::ReversePositionQueued:
				QueuePointer(m_DocumentHostTabReorderTestSecondBeforePosition, m_DocumentHostTabReorderTestFirstTabViewportID);
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
				PF_INFO("Document tab reorder UI fixture passed: the same synthetic session moved after and before its neighbor, "
						"preserving the active session and Level Editor tab.");
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
			if (!m_DocumentHostTabGroupTest) return;
#if !defined(_DEBUG) && !defined(PF_DEBUG)
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
				if (!m_DocumentHostTabGroupSourceCenterValid || !m_DocumentHostTabGroupTargetCenterValid) break;
				const auto* SourceSession = m_Documents.Find(m_DocumentHostTabTearOffTestIdentity);
				const auto* TargetSession = m_Documents.Find(m_DocumentHostTabGroupTargetIdentity);
				const auto* SourceHost = SourceSession ? m_Documents.FindHost(SourceSession->HostID) : nullptr;
				const auto* TargetHost = TargetSession ? m_Documents.FindHost(TargetSession->HostID) : nullptr;
				if (!SourceSession || !TargetSession || !SourceHost || !TargetHost ||
					SourceHost->Role != PulseForgeEditor::EditorDocumentHostRole::Native ||
					TargetHost->Role != PulseForgeEditor::EditorDocumentHostRole::Native || SourceHost->ID == TargetHost->ID ||
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
				IO.AddMousePosEvent(m_DocumentHostTabGroupSourceCenter.x, m_DocumentHostTabGroupSourceCenter.y);
				IO.AddMouseViewportEvent(m_DocumentHostTabGroupSourceViewportID);
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::PressQueued;
				break;
			}
			case DocumentTabGroupTestPhase::PressQueued:
				IO.AddMousePosEvent(m_DocumentHostTabGroupTargetCenter.x, m_DocumentHostTabGroupTargetCenter.y);
				IO.AddMouseViewportEvent(m_DocumentHostTabGroupTargetViewportID);
				m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::PositionQueued;
				break;
			case DocumentTabGroupTestPhase::PositionQueued:
				IO.AddMousePosEvent(m_DocumentHostTabGroupTargetCenter.x, m_DocumentHostTabGroupTargetCenter.y);
				IO.AddMouseViewportEvent(m_DocumentHostTabGroupTargetViewportID);
				IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::ReleaseQueued;
				break;
			case DocumentTabGroupTestPhase::ReleaseQueued:
			{
				const auto* SourceSession = m_Documents.Find(m_DocumentHostTabTearOffTestIdentity);
				const auto* TargetSession = m_Documents.Find(m_DocumentHostTabGroupTargetIdentity);
				const auto* TargetHost = m_Documents.FindHost(m_DocumentHostTabGroupTargetHostID);
				const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				const bool BothSessionsGrouped = TargetHost && SourceSession && TargetSession && SourceSession->HostID == TargetHost->ID &&
												 TargetSession->HostID == TargetHost->ID && TargetHost->Documents.size() == 2 &&
												 TargetHost->ActiveDocument == SourceSession->Identity;
				const bool SourceHostRetired = m_Documents.FindHost(m_DocumentHostTabGroupSourceHostID) == nullptr;
				const bool LevelRetained =
					MainHost && std::any_of(MainHost->Documents.begin(), MainHost->Documents.end(),
									[](const auto& Identity) { return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level; });
				if (!BothSessionsGrouped || !SourceHostRetired || !LevelRetained)
				{
					Fail("the same session did not join the target native tab stack or the empty host was not retired.");
					break;
				}
				m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::Complete;
				m_DocumentHostTabGroupTest = false;
				++m_DocumentHostTabGroupCycleCount;
				PF_INFO("Document tab group UI fixture passed for round trip {}: the same session joined an existing native "
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
					PF_INFO("Document tab repeated-transfer UI fixture passed: the same session completed {} native/main "
							"round trips and grouping operations.",
						m_DocumentHostTabGroupCycleCount);
				if (m_DocumentHostTabGroupCycleCount == DocumentTabRoundTripCycleCount)
				{
					m_DocumentHostTabTearOffFromNativeHost = true;
					m_DocumentHostTabTearOffSourceHostID = TargetHost->ID;
					m_DocumentHostTabTearOffTestFrameCount = 0;
					m_DocumentHostTabTearOffTestTabCenterValid = false;
					m_DocumentHostTabTearOffPointerPrepared = false;
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
			if (!m_DocumentHostResetLayoutTestPending) return;
			m_DocumentHostResetLayoutTestPending = false;

			bool SessionsPreserved = m_Documents.GetHosts().size() == m_DocumentHostResetLayoutExpectedHostCount;
			for (const auto& [Identity, ExpectedHostID] : m_DocumentHostFixtureSessions)
			{
				const auto* Session = m_Documents.Find(Identity);
				SessionsPreserved = SessionsPreserved && Session && Session->HostID == ExpectedHostID && Session->IsDirty() &&
									Session->Revision == Session->SavedRevision + 1;
			}

			if (SessionsPreserved)
				PF_INFO("Document host reset-layout test passed: {} dirty fixture sessions retained their hosts.",
					m_DocumentHostFixtureSessions.size());
			else PF_ERROR("Document host reset-layout test failed: dirty fixture sessions or host membership changed.");

			// These synthetic sessions must not trigger a dirty-close dialog during test cleanup.
			for (const auto& [Identity, ExpectedHostID] : m_DocumentHostFixtureSessions)
			{
				(void)ExpectedHostID;
				if (const auto* Session = m_Documents.Find(Identity)) (void)m_Documents.MarkSaved(Identity, Session->Revision);
			}
		}

		void MarkDocumentWorkspaceDirty(bool BoundsOnly = false)
		{
			if (m_ApplicationClosePending || m_ApplicationShutdownRequested || !m_Project || !m_DocumentWorkspaceStore.HasPath()) return;
			m_DocumentWorkspaceStore.MarkDirty(BoundsOnly);
		}

		void RestoreDocumentWorkspace()
		{
			if (!m_Project) return;
			m_DocumentWorkspaceStore.Configure(m_Layout.GetDocumentWorkspacePath(m_Project->GetDescription().ID.ToString()));
			auto Loaded = m_DocumentWorkspaceStore.Load(m_Project->GetDescription().ID);
			if (!Loaded)
			{
				PF_WARN("Could not read document workspace: {}", Loaded.error());
				return;
			}
			if (Loaded->Result == PulseForgeEditor::EditorDocumentWorkspaceLoadResult::Status::Missing) return;
			if (Loaded->Result == PulseForgeEditor::EditorDocumentWorkspaceLoadResult::Status::Rejected)
			{
				const std::string RecoveryStatus =
					Loaded->RecoveryError.empty()
						? "preserved at " + (Loaded->RecoveryPath ? Loaded->RecoveryPath->string() : "unknown path")
						: "failed; automatic replacement is disabled: " + Loaded->RecoveryError;
				PF_WARN("Document workspace rejected: {}. Recovery {}", Loaded->Error, RecoveryStatus);
				return;
			}
			auto Workspace = std::move(Loaded->Workspace);
			if (!Workspace)
			{
				PF_WARN("Document workspace load completed without state; rebuilding from registered documents.");
				return;
			}
			bool SkippedDocument = false;
			if (Workspace->MaterialDetailsAsset)
			{
				const auto Asset = m_Project->GetAssetRegistry().Find(*Workspace->MaterialDetailsAsset);
				if (Asset && PulseForge::HasAssetTypeExtension(Asset->ProjectRelativePath, PulseForge::AssetFileType::Material))
				{
					m_MaterialDetailsAsset = *Workspace->MaterialDetailsAsset;
					(void)m_Layout.SetPanelVisible(PulseForgeEditor::EditorPanel::MaterialDetails, true);
					m_InitialMaterialDetailsDockPending = true;
					m_FocusMaterialDetailsNextFrame = true;
				}
				else SkippedDocument = true;
			}
			bool RecoveredMaterialDetails = false;
			std::optional<PulseForge::AssetID> FocusedActiveMaterial;
			for (const auto& Host : Workspace->Hosts)
				if (Host.ID == Workspace->FocusedHostID && Host.ActiveDocument &&
					Host.ActiveDocument->Kind == PulseForgeEditor::EditorDocumentKind::Material)
					FocusedActiveMaterial = Host.ActiveDocument->Asset;
			for (const PulseForge::AssetID& Candidate : PulseForgeEditor::GetMaterialDetailsMigrationCandidates(*Workspace))
			{
				const auto Asset = m_Project->GetAssetRegistry().Find(Candidate);
				if (!Asset || !PulseForge::HasAssetTypeExtension(Asset->ProjectRelativePath, PulseForge::AssetFileType::Material)) continue;
				if (FocusedActiveMaterial == Candidate || !m_MaterialDetailsAsset) m_MaterialDetailsAsset = Candidate;
				SkippedDocument = true;
				RecoveredMaterialDetails = true;
			}
			std::unordered_set<std::string> RestoredHosts;
			for (const auto& SavedHost : Workspace->Hosts)
			{
				if (SavedHost.Role != PulseForgeEditor::EditorDocumentHostRole::Native) continue;
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
						if (!m_Documents.Find(Identity)) SkippedDocument = true;
						continue;
					}
					std::filesystem::path RelativePath;
					std::string DisplayName;
					if (IsDocumentHostTestFixtureIdentity(Identity))
					{
						const auto FixtureIndex = PulseForgeEditor::Detail::EditorDocumentHostTestEditor::FindFixtureIndex(Identity.Asset);
						if (!FixtureIndex)
						{
							SkippedDocument = true;
							continue;
						}
						DisplayName = "Host Test Fixture " + std::to_string(*FixtureIndex + 1);
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
					auto Opened = m_Documents.Open(
						Identity, RelativePath, DisplayName, PulseForgeEditor::EditorDocumentOpenPreference::MainDocumentStrip);
					if (!Opened)
					{
						SkippedDocument = true;
						PF_WARN("Could not restore registered document {}: {}", DisplayName, Opened.error());
						continue;
					}
					auto* Session = m_Documents.Find(Identity);
					if (!Session || !m_DocumentEditors.Load(*Session, Context))
					{
						(void)m_Documents.Close(Identity, PulseForgeEditor::EditorDocumentCloseChoice::Discard, {}, {});
						SkippedDocument = true;
						PF_WARN("Could not load registered document session {}", DisplayName);
						continue;
					}
					const auto SavedDocument = std::find(SavedHost.Documents.begin(), SavedHost.Documents.end(), Identity);
					const size_t DestinationIndex = static_cast<size_t>(std::distance(SavedHost.Documents.begin(), SavedDocument));
					if (!m_Documents.Transfer(
							Identity, SavedHost.ID, DestinationIndex, {}, PulseForgeEditor::EditorDocumentTransferCause::WorkspaceRestore))
					{
						(void)m_Documents.Close(Identity, PulseForgeEditor::EditorDocumentCloseChoice::Discard, {}, {});
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
				const auto CompatibleHost = std::find_if(Workspace->Hosts.begin(), Workspace->Hosts.end(),
					[Kind, &RestoredHosts](const auto& Candidate)
					{
						return Candidate.Role == PulseForgeEditor::EditorDocumentHostRole::Native && Candidate.PlacementKind == Kind &&
							   RestoredHosts.contains(Candidate.ID) && !Candidate.Documents.empty();
					});
				if (CompatibleHost != Workspace->Hosts.end()) Placement.LiveHostID = CompatibleHost->ID;
				(void)m_Documents.SetRememberedPlacement(Workspace->Project, Kind, Placement.Role, Placement.Bounds, Placement.LiveHostID);
			}
			if (!m_Documents.FocusHost(Workspace->FocusedHostID))
				FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
			if (SkippedDocument) m_DocumentWorkspaceStore.MarkDirty(false);
		}

		PulseForgeEditor::EditorDocumentWorkspaceState CaptureDocumentWorkspace() const
		{
			PulseForgeEditor::EditorDocumentWorkspaceState Workspace;
			Workspace.Project = m_Project->GetDescription().ID;
			Workspace.MaterialDetailsAsset = m_MaterialDetailsAsset;
			if (const auto* FocusedHost = m_Documents.GetFocusedHost()) Workspace.FocusedHostID = FocusedHost->ID;
			for (const auto& Host : m_Documents.GetHosts())
				if (Host.Role == PulseForgeEditor::EditorDocumentHostRole::Main || !Host.Documents.empty()) Workspace.Hosts.push_back(Host);
			Workspace.RememberedPlacements = m_Documents.GetRememberedPlacements(Workspace.Project);
			std::sort(Workspace.RememberedPlacements.begin(), Workspace.RememberedPlacements.end(),
				[](const auto& Left, const auto& Right) { return static_cast<uint8_t>(Left.first) < static_cast<uint8_t>(Right.first); });
			return Workspace;
		}

		void SaveDocumentWorkspaceIfNeeded()
		{
			if (m_ApplicationClosePending || m_ApplicationShutdownRequested || !m_Project || !m_DocumentWorkspaceStore.HasPath() ||
				!m_DocumentWorkspaceStore.IsSaveDue())
				return;
			if (auto Saved = m_DocumentWorkspaceStore.SaveIfDue(CaptureDocumentWorkspace()); !Saved)
				PF_WARN("Could not persist editor document workspace: {}", Saved.error());
		}

		bool SaveDocumentWorkspaceNow()
		{
			if (!m_Project || !m_DocumentWorkspaceStore.HasPath()) return true;
			if (auto Saved = m_DocumentWorkspaceStore.SaveNow(CaptureDocumentWorkspace()); !Saved)
			{
				SetError("Could not persist the editor document workspace before closing: " + Saved.error());
				return false;
			}
			return true;
		}

		void ImportAsset()
		{
			if (!m_Project)
			{
				SetError("Create or open a project before importing assets.");
				return;
			}

			auto SourceFile = ShowOpenDialog(nullptr, m_Project->GetRootPath());
			if (!SourceFile) return;
			if (HasReservedMetadataExtension(*SourceFile))
			{
				SetError("Asset sidecar metadata files cannot be imported as source assets.");
				return;
			}

			const std::string DefaultName = PathToUtf8(SourceFile->filename());
			auto DestinationFile = ShowSaveDialog(nullptr, m_Project->GetRootPath() / "Assets", DefaultName.c_str());
			if (!DestinationFile) return;
			if (DestinationFile->extension().empty()) *DestinationFile += SourceFile->extension();
			if (HasReservedMetadataExtension(*DestinationFile))
			{
				SetError("Asset destinations cannot use the reserved .meta extension.");
				return;
			}

			const std::filesystem::path SourcePath = *SourceFile;
			const std::filesystem::path DestinationPath = *DestinationFile;
			QueueAfterSave([this, SourcePath, DestinationPath] { ImportAssetAt(SourcePath, DestinationPath); });
		}

		void ImportAssetAt(const std::filesystem::path& SourceFile, const std::filesystem::path& DestinationFile)
		{
			if (!m_Project)
			{
				SetError("Create or open a project before importing assets.");
				return;
			}

			const auto RelativeDestination = GetProjectRelativePath(DestinationFile, "asset import");
			if (!RelativeDestination) return;

			auto Imported = PulseForge::AssetOperations::ImportFile(
				m_Project->GetAssetRegistry(), m_Project->GetRootPath(), SourceFile, *RelativeDestination);
			if (!Imported)
			{
				std::string Message = "Asset import failed: " + Imported.error().Message;
				if (!Imported.error().Path.empty()) Message += " Path: " + PathToUtf8(Imported.error().Path) + ".";
				if (Imported.error().RecoveryPath) Message += " Recovery data: " + PathToUtf8(*Imported.error().RecoveryPath) + ".";
				if (Imported.error().CommittedAsset)
					Message += " The asset was committed with UUID " + Imported.error().CommittedAsset->ID.ToString() + ".";

				const auto RegistryRefresh = m_Project->GetAssetRegistry().Rebuild(m_Project->GetRootPath());
				if (RegistryRefresh) UpdateAssetList(true);
				else
				{
					Message += " Registry recovery also reported:";
					for (const PulseForge::AssetRegistryIssue& Issue : RegistryRefresh.error().Issues)
						Message += " " + Issue.Message;
				}
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList(true);
			m_ContentBrowser.SelectedAsset() = Imported->ID;
			SetStatus("Imported " + PathToUtf8(Imported->ProjectRelativePath) + " (UUID " + Imported->ID.ToString() + ").");
		}

		void MoveAsset(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project) return;
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
			if (!m_Project) return;
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
			if (SaveAs && m_SceneAsset) Source = m_Project->GetAssetRegistry().Find(*m_SceneAsset);
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

		void DrawAssetDestinationDialog()
		{
			const auto Operation = PulseForgeEditor::EditorAssetDestinationDialog::Draw(
				m_EditorStyle, m_AssetWorkflow, m_Project ? &*m_Project : nullptr, m_ContentBrowser);
			if (!Operation || !m_Project) return;

			const std::filesystem::path Target = Operation->ProjectRelativeDestination;
			switch (Operation->Kind)
			{
			case PulseForgeEditor::EditorAssetDestinationOperationKind::MoveAsset:
				if (Operation->Asset) QueueAfterSave([this, ID = *Operation->Asset, Target] { MoveAssetAt(ID, Target); });
				break;
			case PulseForgeEditor::EditorAssetDestinationOperationKind::DuplicateAsset:
				if (Operation->Asset) QueueAfterSave([this, ID = *Operation->Asset, Target] { DuplicateAssetAt(ID, Target); });
				break;
			case PulseForgeEditor::EditorAssetDestinationOperationKind::CreateScene:
			case PulseForgeEditor::EditorAssetDestinationOperationKind::SaveSceneAs:
			{
				const bool SaveAs = Operation->Kind == PulseForgeEditor::EditorAssetDestinationOperationKind::SaveSceneAs;
				QueueAfterSave(
					[this, Target, SaveAs]
					{
						const std::filesystem::path AbsoluteTarget = m_Project ? m_Project->GetRootPath() / Target : Target;
						CreateSceneAt(AbsoluteTarget, SaveAs);
					});
				break;
			}
			case PulseForgeEditor::EditorAssetDestinationOperationKind::CreatePrefab:
				if (Operation->PrefabRoot) QueueAfterSave([this, Root = *Operation->PrefabRoot, Target] { CreatePrefabAt(Root, Target); });
				break;
			}
		}

		void MoveAssetAt(const PulseForge::AssetID& Identifier, const std::filesystem::path& Destination)
		{
			if (!m_Project) return;
			auto Moved =
				PulseForge::AssetOperations::Move(m_Project->GetAssetRegistry(), m_Project->GetRootPath(), Identifier, Destination);
			if (!Moved)
			{
				std::string Message = DescribeAssetOperationError("Asset move failed", Moved.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList(true);
			m_ContentBrowser.SelectedAsset() = Moved->ID;
			SetStatus("Moved " + PathToUtf8(Moved->ProjectRelativePath) + ".");
		}

		void MoveFolderAt(const std::filesystem::path& Source, const std::filesystem::path& DestinationParent)
		{
			if (!m_Project) return;
			auto Moved =
				PulseForge::AssetOperations::MoveFolder(m_Project->GetAssetRegistry(), m_Project->GetRootPath(), Source, DestinationParent);
			if (!Moved)
			{
				std::string Message = DescribeAssetOperationError("Folder move failed", Moved.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			m_ContentBrowser.RebaseFoldersAfterMove(Source, *Moved);

			UpdateAssetList(true);
			SetStatus("Moved folder to " + PathToUtf8(*Moved) + ". Asset identities were preserved.");
		}

		void DuplicateAsset(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project) return;
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

		void DuplicateAssetAt(const PulseForge::AssetID& Identifier, const std::filesystem::path& Destination)
		{
			if (!m_Project) return;
			auto Duplicated =
				PulseForge::AssetOperations::Duplicate(m_Project->GetAssetRegistry(), m_Project->GetRootPath(), Identifier, Destination);
			if (!Duplicated)
			{
				std::string Message = DescribeAssetOperationError("Asset duplication failed", Duplicated.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList(true);
			m_ContentBrowser.SelectedAsset() = Duplicated->ID;
			SetStatus("Duplicated " + PathToUtf8(Duplicated->ProjectRelativePath) + " (new UUID " + Duplicated->ID.ToString() + ").");
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

		void CreatePrefabAt(const PulseForge::UUID& RootIdentifier, const std::filesystem::path& Destination)
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
				m_Project->GetAssetRegistry(), m_Project->GetRootPath(), Destination, *m_Scene, *Root);
			if (!Created)
			{
				std::string Message = "Prefab creation failed: " + Created.error().Message;
				if (Created.error().RecoveryPath) Message += " Recovery data: " + PathToUtf8(*Created.error().RecoveryPath) + ".";
				if (Created.error().CommittedAsset)
				{
					UpdateAssetList(true);
					m_ContentBrowser.SelectedAsset() = Created.error().CommittedAsset->ID;
					SetWarning("Prefab was created with UUID " + Created.error().CommittedAsset->ID.ToString() +
							   ", but temporary cleanup failed: " + Message);
					return;
				}

				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList(true);
			m_ContentBrowser.SelectedAsset() = Created->ID;
			SetStatus("Created prefab " + PathToUtf8(Created->ProjectRelativePath) + " (UUID " + Created->ID.ToString() + ").");
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
			const auto Recorded = RunSceneEdit("Instantiate Prefab",
				[this, Identifier, &InstantiatedIdentifier] -> std::expected<void, PulseForge::PrefabAssetError>
				{
					auto Instantiated = PulseForge::PrefabAssetService::Instantiate(
						Identifier, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), *m_Scene);
					if (!Instantiated) return std::unexpected(Instantiated.error());
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
			if (m_AssetWorkflow.TakeDeleteDialogRequest()) ImGui::OpenPopup("Delete Managed Asset");

			bool PopupOpen = true;
			if (ImGui::BeginPopupModal("Delete Managed Asset", &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingX,
																   PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingY));
				const auto Asset = m_Project && m_AssetWorkflow.PendingDeleteAsset()
									   ? m_Project->GetAssetRegistry().Find(*m_AssetWorkflow.PendingDeleteAsset())
									   : std::optional<PulseForge::AssetRecord>{};
				if (Asset)
					ImGui::TextWrapped("Permanently delete %s and its sidecar metadata?", PathToUtf8(Asset->ProjectRelativePath).c_str());
				else ImGui::TextUnformatted("The selected asset is no longer registered.");
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
			if (!m_Project) return;
			if (IsProtectedSceneAsset(Identifier))
			{
				SetError("The open or configured startup scene cannot be deleted. Choose another startup scene first.");
				return;
			}
			const auto Asset = m_Project->GetAssetRegistry().Find(Identifier);
			const bool DeletedMaterial =
				Asset && PulseForge::HasAssetTypeExtension(Asset->ProjectRelativePath, PulseForge::AssetFileType::Material);
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
				m_DocumentCloseRequest.BeginCloseDocuments(DeletedDocuments,
					std::string(PulseForgeEditor::EditorDocumentManager::MainHostID), [this, Identifier] { DeleteAssetAt(Identifier); });
				m_DocumentCloseRequest.SetError(PreparedClose.error());
				SetError(m_DocumentCloseRequest.GetError());
				return;
			}

			auto Deleted = PulseForge::AssetOperations::Delete(m_Project->GetAssetRegistry(), m_Project->GetRootPath(), Identifier);
			if (!Deleted)
			{
				std::string Message = DescribeAssetOperationError("Asset deletion failed", Deleted.error());
				RefreshAssetsAfterOperationFailure(Message);
				SetError(std::move(Message));
				return;
			}

			UpdateAssetList(true);
			if (!CommitPreparedDocumentCloseBatch(std::move(*PreparedClose))) return;
			if (DeletedMaterial) ForgetMaterialDraft(Identifier);
			if (m_ContentBrowser.SelectedAsset() && *m_ContentBrowser.SelectedAsset() == Identifier)
				m_ContentBrowser.SelectedAsset().reset();
			SetStatus("Deleted asset " + Identifier.ToString() + ".");
		}

		bool IsProtectedSceneAsset(const PulseForge::AssetID& Identifier) const override
		{
			return (m_SceneAsset && *m_SceneAsset == Identifier) ||
				   (m_Project && m_Project->GetDescription().StartScene && *m_Project->GetDescription().StartScene == Identifier);
		}

		[[nodiscard]] std::optional<std::filesystem::path> GetProjectRelativePath(
			const std::filesystem::path& AbsolutePath, std::string_view Operation)
		{
			if (!m_Project) return std::nullopt;

			std::error_code Error;
			const std::filesystem::path Root = std::filesystem::absolute(m_Project->GetRootPath(), Error).lexically_normal();
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

		static std::string DescribeAssetOperationError(std::string_view Operation, const PulseForge::AssetOperationError& Error)
		{
			std::string Message = std::string(Operation) + ": " + Error.Message;
			if (!Error.Path.empty()) Message += " Path: " + PathToUtf8(Error.Path) + ".";
			if (Error.RecoveryPath) Message += " Recovery data: " + PathToUtf8(*Error.RecoveryPath) + ".";
			if (Error.CommittedAsset) Message += " The asset was committed with UUID " + Error.CommittedAsset->ID.ToString() + ".";
			return Message;
		}

		void RefreshAssetsAfterOperationFailure(std::string& Message)
		{
			if (!m_Project) return;
			const auto Rebuild = m_Project->GetAssetRegistry().Rebuild(m_Project->GetRootPath());
			if (Rebuild) UpdateAssetList(true);
			else
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
			if (!FinishSceneEdit()) return;
			std::unique_ptr<PulseForge::Scene> NewScene;
			if (!SaveAs) NewScene = std::make_unique<PulseForge::Scene>();
			const PulseForge::Scene& Source = SaveAs ? *m_Scene : *NewScene;
			auto Created =
				PulseForge::SceneAssetService::Create(m_Project->GetAssetRegistry(), m_Project->GetRootPath(), RelativePath, Source);
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
			if (SaveAs) MarkSceneSaved();
			else ResetSceneHistory();
			UpdateAssetList();
			if (auto StartScene = m_Project->SetStartScene(Record.ID); !StartScene)
			{
				std::string Message = "Scene was created, but the project startup scene could not be updated: ";
				Message += StartScene.error().Message;
				if (!Created) Message += " Temporary cleanup also needs attention: " + Created.error().Message;
				if (!Created && Created.error().RecoveryPath) Message += " Recovery data: " + PathToUtf8(*Created.error().RecoveryPath);
				SetWarning(std::move(Message));
			}
			else if (!Created)
			{
				std::string Message = "Scene was created, but temporary cleanup needs attention: ";
				Message += Created.error().Message;
				if (Created.error().RecoveryPath) Message += " Recovery data: " + PathToUtf8(*Created.error().RecoveryPath);
				SetWarning(std::move(Message));
			}
			else SetStatus("Created scene " + PathToUtf8(Record.ProjectRelativePath) + ".");
		}

		void OpenScene(const PulseForge::AssetID& Identifier)
		{
			QueueAfterSave([this, Identifier] { OpenSceneNow(Identifier); });
		}

		void OpenSceneNow(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project) return;
			auto Loaded = std::make_unique<PulseForge::Scene>();
			const auto Result =
				PulseForge::SceneAssetService::Load(Identifier, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), *Loaded);
			if (!Result)
			{
				SetError("Scene could not be opened: " + Result.error().Message);
				return;
			}
			if (!FinishSceneEdit()) return;

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
			if (!m_Project) return;
			if (auto Result = m_Project->SetStartScene(Identifier); !Result)
			{
				SetError("Could not set the project startup scene: " + Result.error().Message);
				return;
			}
			SetStatus("Updated the project startup scene to " + Identifier.ToString() + ".");
		}

		bool SaveScene()
		{
			if (!FinishSceneEdit()) return false;
			if (!m_Project || !m_Scene || !m_SceneAsset)
			{
				SetError("There is no managed scene asset to save.");
				return false;
			}

			const auto Result =
				PulseForge::SceneAssetService::Save(*m_SceneAsset, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), *m_Scene);
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
			if (!FinishSceneEdit()) return;
			if (m_SceneTransitionQueue.HasPendingDecision())
			{
				SetWarning("Resolve the current unsaved-scene prompt before starting another editor operation.");
				return;
			}

			auto Request = m_SceneTransitionQueue.Request(
				m_SceneEditController.IsDirty(), [this, Action = std::move(Action)]() mutable { QueueAfterDocuments(std::move(Action)); });
			if (Request.Status == PulseForgeEditor::EditorSceneTransitionQueue::RequestStatus::Rejected)
			{
				SetWarning("The editor could not queue the requested scene transition.");
				return;
			}
			if (Request.Status == PulseForgeEditor::EditorSceneTransitionQueue::RequestStatus::AwaitingDecision) m_OpenUnsavedDialog = true;
			else if (Request.ReadyAction) Request.ReadyAction();
		}

		bool DiscardSceneChanges()
		{
			if (!FinishSceneEdit()) return false;
			if (!m_SceneEditController.IsDirty()) return true;
			if (!m_Project || !m_Scene || !m_SceneAsset)
			{
				SetError("Cannot discard changes because the current scene has no managed source asset to reload.");
				return false;
			}

			auto ReloadedScene = std::make_unique<PulseForge::Scene>();
			const auto Result =
				PulseForge::SceneAssetService::Load(*m_SceneAsset, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), *ReloadedScene);
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

		void ExecutePendingAction(const std::function<bool()>& ResolveSceneChanges)
		{
			auto Action = m_SceneTransitionQueue.Resolve(ResolveSceneChanges);
			if (!Action) return;
			m_OpenUnsavedDialog = false;
			ImGui::CloseCurrentPopup();
			(*Action)();
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
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingX,
																   PulseForgeEditor::EditorStyleMetrics::DialogItemSpacingY));
				ImGui::TextUnformatted("The current scene has unsaved changes.");
				ImGui::TextUnformatted("Save before continuing?");
				if (ImGui::Button("Save", ImVec2(120.0f, 0.0f)))
				{
					ExecutePendingAction([this] { return SaveScene(); });
				}
				ImGui::SameLine();
				if (m_EditorStyle.DangerButton(PulseForgeEditor::EditorIcon::Delete, "Discard"))
				{
					ExecutePendingAction([this] { return DiscardSceneChanges(); });
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
				{
					m_SceneTransitionQueue.Cancel();
					m_OpenUnsavedDialog = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::PopStyleVar();
				ImGui::EndPopup();
			}
			if (!PopupOpen)
			{
				m_SceneTransitionQueue.Cancel();
				m_OpenUnsavedDialog = false;
			}
		}

		void CloseProjectNow()
		{
			if (!ClearCurrentProjectDocumentState([this] { CloseProjectNow(); })) return;
			ClearScene();
			ResetViewportSceneRenderer();
			m_Project.reset();
			m_ContentBrowser.ClearProject();
			SetStatus("Project closed.");
		}

		PulseForgeEditor::EditorMainMenuState BuildMainMenuState()
		{
			PulseForgeEditor::EditorMainMenuState State;
			State.ProjectOpen = m_Project.has_value();
			State.Assets = m_ContentBrowser.Assets();
			State.ActiveSceneAsset = m_SceneAsset;
			State.OpeningPreference = m_Documents.GetOpeningPreference();
			for (const auto& Descriptor : PulseForgeEditor::EditorPanelDescriptors)
				State.PanelVisibility[static_cast<size_t>(Descriptor.Panel)] = m_Layout.IsPanelVisible(Descriptor.Panel);

			const auto* FocusedIdentityPointer = GetFocusedDocumentIdentity();
			const std::optional<PulseForgeEditor::EditorDocumentIdentity> FocusedIdentity =
				FocusedIdentityPointer ? std::optional<PulseForgeEditor::EditorDocumentIdentity>(*FocusedIdentityPointer) : std::nullopt;
			const PulseForge::UUID CurrentProject = m_Project ? m_Project->GetDescription().ID : PulseForge::UUID{};
			const bool LevelDocumentAvailable = m_LevelDocumentIdentity && m_Documents.Find(*m_LevelDocumentIdentity);
			auto* FocusedRegisteredSession = GetFocusedRegisteredDocument();
			const std::optional<PulseForgeEditor::EditorDocumentIdentity> RegisteredDocument =
				FocusedRegisteredSession ? std::optional<PulseForgeEditor::EditorDocumentIdentity>(FocusedRegisteredSession->Identity)
										 : std::nullopt;
			State.LevelDocumentTarget = PulseForgeEditor::CaptureEditorCommandContext(
				std::nullopt, CurrentProject, false, std::nullopt, std::nullopt, m_LevelDocumentIdentity, LevelDocumentAvailable);
			State.FocusedCommandTarget = PulseForgeEditor::CaptureEditorCommandContext(FocusedIdentity, CurrentProject,
				IsEditorPanelFocused(PulseForgeEditor::EditorPanel::MaterialDetails), m_MaterialDetailsAsset, RegisteredDocument,
				m_LevelDocumentIdentity, LevelDocumentAvailable);
			const auto FocusedCommandKind = State.FocusedCommandTarget.Kind;
			const bool MaterialDocumentFocused = FocusedCommandKind == PulseForgeEditor::EditorCommandContextKind::MaterialDetails;
			const bool RegisteredDocumentFocused = FocusedCommandKind == PulseForgeEditor::EditorCommandContextKind::RegisteredDocument;
			const bool SceneDocumentFocused = FocusedCommandKind == PulseForgeEditor::EditorCommandContextKind::Level;
			State.EditMenuEnabled = FocusedCommandKind != PulseForgeEditor::EditorCommandContextKind::Unavailable;
			State.CanEditScene = SceneDocumentFocused && m_Scene != nullptr && m_SceneRuntime == nullptr;

			const auto UndoCommand = m_SceneEditController.GetUndoCommand();
			const auto RedoCommand = m_SceneEditController.GetRedoCommand();
			State.CanUndo = State.CanEditScene && m_SceneEditController.CanUndo();
			State.CanRedo = State.CanEditScene && m_SceneEditController.CanRedo();
			if (RegisteredDocumentFocused) State.CanUndo = State.CanRedo = true;
			if (MaterialDocumentFocused)
			{
				if (State.FocusedCommandTarget.MaterialAsset)
				{
					State.CanUndo = m_Project && m_MaterialDrafts.CanUndo(*m_Project, *State.FocusedCommandTarget.MaterialAsset);
					State.CanRedo = m_Project && m_MaterialDrafts.CanRedo(*m_Project, *State.FocusedCommandTarget.MaterialAsset);
				}
			}
			State.UndoLabel = RegisteredDocumentFocused				? "Undo Document"
							  : MaterialDocumentFocused				? "Undo Material Edit"
							  : SceneDocumentFocused && UndoCommand ? "Undo " + std::string(UndoCommand->Description)
																	: "Undo";
			State.RedoLabel = RegisteredDocumentFocused				? "Redo Document"
							  : MaterialDocumentFocused				? "Redo Material Edit"
							  : SceneDocumentFocused && RedoCommand ? "Redo " + std::string(RedoCommand->Description)
																	: "Redo";

			State.HasSelection = !m_SceneWorkspace.Selection().GetEntities().empty();
			State.CanPaste = State.CanEditScene && m_Project && m_SceneEntityCommands.CanPaste(m_Project->GetDescription().ID);
			State.HasSceneEntities = m_Scene && m_Scene->GetEntityCount() > 0;
			State.CanRename = SelectedEntity().has_value();
			State.CanFocusSelection = State.HasSelection;
			State.CanSnapSelection = State.HasSelection;
			State.CanHideSelection = State.HasSelection;
			State.CanShowAll = !m_SceneWorkspace.Visibility().GetHiddenEntities().empty();
			State.CanIsolateSelection = State.HasSelection;
			State.CanRestoreIsolation = m_SceneWorkspace.Visibility().GetIsolationBaseline().has_value();
			State.CanSaveScene =
				State.LevelDocumentTarget.Kind == PulseForgeEditor::EditorCommandContextKind::Level && m_Scene && m_SceneAsset;
			State.CanSaveSceneAs =
				State.LevelDocumentTarget.Kind == PulseForgeEditor::EditorCommandContextKind::Level && m_Scene != nullptr;
			return State;
		}

		[[nodiscard]] std::optional<PulseForgeEditor::EditorCommandContext> ResolveMainMenuCommandTarget(
			const PulseForgeEditor::EditorCommandContext& Target) const
		{
			const PulseForge::UUID CurrentProject = m_Project ? m_Project->GetDescription().ID : PulseForge::UUID{};
			const bool LevelAvailable = m_LevelDocumentIdentity && m_Documents.Find(*m_LevelDocumentIdentity);
			const auto* Session = Target.Document ? m_Documents.Find(*Target.Document) : nullptr;
			const bool RegisteredAvailable = Session && Session->EditorState && m_DocumentEditors.IsRegistered(Session->Identity.Kind) &&
											 Session->Identity.Project == CurrentProject;
			const bool MaterialAvailable =
				m_Project && Target.MaterialAsset && m_MaterialDrafts.ContainsDraft(*m_Project, *Target.MaterialAsset);
			return PulseForgeEditor::ResolveEditorCommandContext(
				Target, CurrentProject, m_LevelDocumentIdentity, LevelAvailable, RegisteredAvailable, MaterialAvailable);
		}

		void DrawMainMenu()
		{
			const auto State = BuildMainMenuState();
			const auto Result = m_MainMenuPanel.Draw(State, m_EditorStyle);
			if (Result.Action) ExecuteMainMenuAction(*Result.Action);
			if (Result.FocusMainDocumentHost) FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
		}

		void ExecuteMainMenuAction(const PulseForgeEditor::EditorMainMenuAction& Action)
		{
			using PulseForgeEditor::EditorMainMenuActionKind;
			const auto IsLevelSceneAction = [&Action]
			{
				switch (Action.Kind)
				{
				case EditorMainMenuActionKind::Cut:
				case EditorMainMenuActionKind::Copy:
				case EditorMainMenuActionKind::Paste:
				case EditorMainMenuActionKind::Duplicate:
				case EditorMainMenuActionKind::Delete:
				case EditorMainMenuActionKind::SelectAll:
				case EditorMainMenuActionKind::Rename:
				case EditorMainMenuActionKind::FocusSelection:
				case EditorMainMenuActionKind::SnapSelection:
				case EditorMainMenuActionKind::HideSelection:
				case EditorMainMenuActionKind::ShowAll:
				case EditorMainMenuActionKind::IsolateSelection:
				case EditorMainMenuActionKind::RestoreIsolation:
				case EditorMainMenuActionKind::NewScene:
				case EditorMainMenuActionKind::SaveScene:
				case EditorMainMenuActionKind::SaveSceneAs:
				case EditorMainMenuActionKind::OpenScene:
					return true;
				default:
					return false;
				}
			};
			const auto ResolvedTarget = ResolveMainMenuCommandTarget(Action.CommandTarget);
			if (IsLevelSceneAction() && (Action.CommandTarget.Kind != PulseForgeEditor::EditorCommandContextKind::Level || !ResolvedTarget))
			{
				SetWarning("The Level Editor command target is no longer available.");
				return;
			}
			switch (Action.Kind)
			{
			case EditorMainMenuActionKind::NewProject:
				CreateProject();
				break;
			case EditorMainMenuActionKind::OpenProject:
				OpenProject();
				break;
			case EditorMainMenuActionKind::CloseProject:
				QueueAfterSave([this] { CloseProjectNow(); });
				break;
			case EditorMainMenuActionKind::Exit:
				QueueAfterSave([this] { RequestEditorApplicationClose(); });
				break;
			case EditorMainMenuActionKind::Undo:
			case EditorMainMenuActionKind::Redo:
			{
				const bool Redo = Action.Kind == EditorMainMenuActionKind::Redo;
				if (!ResolvedTarget)
				{
					SetWarning("The document targeted by the menu command is no longer available.");
					break;
				}
				switch (ResolvedTarget->Kind)
				{
				case PulseForgeEditor::EditorCommandContextKind::RegisteredDocument:
				{
					auto* Session = ResolvedTarget->Document ? m_Documents.Find(*ResolvedTarget->Document) : nullptr;
					if (!Session ||
						!m_DocumentEditors.DispatchFocusedCommand(
							*Session, Redo ? PulseForgeEditor::EditorDocumentCommand::Redo : PulseForgeEditor::EditorDocumentCommand::Undo))
						SetWarning("The targeted editor no longer accepts this history command.");
					break;
				}
				case PulseForgeEditor::EditorCommandContextKind::MaterialDetails:
					if (!ResolvedTarget->MaterialAsset || !ApplyMaterialHistoryStep(*ResolvedTarget->MaterialAsset, Redo))
						SetWarning("Material Details no longer has the requested history step.");
					break;
				case PulseForgeEditor::EditorCommandContextKind::Level:
					if (Redo) RedoSceneEdit();
					else UndoSceneEdit();
					break;
				case PulseForgeEditor::EditorCommandContextKind::Unavailable:
					break;
				}
				break;
			}
			case EditorMainMenuActionKind::Cut:
				CutSelectedEntities();
				break;
			case EditorMainMenuActionKind::Copy:
				(void)CopySelectedEntities();
				break;
			case EditorMainMenuActionKind::Paste:
				PasteEntities();
				break;
			case EditorMainMenuActionKind::Duplicate:
				DuplicateSelectedEntity();
				break;
			case EditorMainMenuActionKind::Delete:
				DeleteSelectedEntity();
				break;
			case EditorMainMenuActionKind::SelectAll:
			{
				std::vector<PulseForge::UUID> Identifiers;
				if (m_Scene)
					for (const PulseForge::Entity& Entity : m_Scene->GetEntities())
						Identifiers.push_back(Entity.GetUUID());
				SetEditorSelection(std::move(Identifiers));
				break;
			}
			case EditorMainMenuActionKind::Rename:
				m_InspectorPanel.FocusEntityNameNextFrame();
				SetEditorPanelVisible(PulseForgeEditor::EditorPanel::Inspector, true);
				break;
			case EditorMainMenuActionKind::FocusSelection:
				FocusSelection();
				break;
			case EditorMainMenuActionKind::SnapSelection:
				SnapSelectionToSurface();
				break;
			case EditorMainMenuActionKind::HideSelection:
				HideSelectedEntities();
				break;
			case EditorMainMenuActionKind::ShowAll:
				ShowAllEntities();
				break;
			case EditorMainMenuActionKind::IsolateSelection:
				IsolateSelectedEntities();
				break;
			case EditorMainMenuActionKind::RestoreIsolation:
				RestoreIsolatedVisibility();
				break;
			case EditorMainMenuActionKind::NewScene:
				CreateScene(false);
				break;
			case EditorMainMenuActionKind::SaveScene:
				(void)SaveScene();
				break;
			case EditorMainMenuActionKind::SaveSceneAs:
				CreateScene(true);
				break;
			case EditorMainMenuActionKind::OpenScene:
				if (Action.Asset) OpenScene(*Action.Asset);
				break;
			case EditorMainMenuActionKind::SetPanelVisible:
				(void)SetEditorPanelVisible(Action.Panel, Action.PanelVisible);
				break;
			case EditorMainMenuActionKind::SetOpeningPreference:
				m_Documents.SetOpeningPreference(Action.OpeningPreference);
				(void)m_Layout.SetDocumentOpeningPreference(static_cast<uint32_t>(Action.OpeningPreference));
				break;
			case EditorMainMenuActionKind::ResetLayout:
				m_Layout.RequestReset();
				break;
			}
		}

		void DrawEditorToolbar()
		{
			constexpr float Height = 60.0f;
			if (!BeginEditorChromeBar(
					"##PulseForgeGlobalToolbar", ImGuiDir_Up, Height, PulseForgeEditor::EditorColorToken::PanelRaised, m_EditorStyle))
				return;

			const float ContentWidth = ImGui::GetContentRegionAvail().x;
			if (ImGui::BeginTable("GlobalToolbarLayout", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
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
					if (ProjectLabel != ProjectName && ImGui::IsItemHovered()) ImGui::SetTooltip("Project: %s", ProjectName.c_str());
					ImGui::SameLine();
					m_EditorStyle.TextMuted("/");
					ImGui::SameLine();
					std::string SceneName = "No scene";
					if (m_SceneAsset)
						if (const auto Record = m_Project->GetAssetRegistry().Find(*m_SceneAsset))
							SceneName = PathToUtf8(Record->ProjectRelativePath.filename());
					if (m_SceneEditController.IsDirty()) SceneName.push_back('*');
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
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - RuntimeWidth) * 0.5f));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24.0f, 8.0f));
				if (!m_SceneRuntime)
				{
					ImGui::BeginDisabled(!m_Project || !m_Scene);
					if (m_EditorStyle.AccentButton(PulseForgeEditor::EditorIcon::Play, "Play")) StartRuntime();
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
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, ImGui::GetContentRegionAvail().x - SaveWidth));
				const auto* FocusedDocument = GetFocusedDocumentIdentity();
				const bool MaterialCanSave = IsFocusedMaterialDocument() && FocusedDocument && m_Project &&
											 m_MaterialDrafts.IsDirty(*m_Project, FocusedDocument->Asset);
				const bool RegisteredDocumentFocused = GetFocusedRegisteredDocument() != nullptr;
				ImGui::BeginDisabled(RegisteredDocumentFocused	   ? false
									 : IsFocusedMaterialDocument() ? !MaterialCanSave
																   : (!m_Scene || !m_SceneAsset || m_SceneRuntime));
				if (m_EditorStyle.IconButton(PulseForgeEditor::EditorIcon::Save, "Save", "GlobalSaveScene",
						RegisteredDocumentFocused	  ? "Save the focused document"
						: IsFocusedMaterialDocument() ? "Save Material Details"
													  : "Save the current scene",
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
					if (m_SceneEditController.IsDirty()) Title.push_back('*');
					Title += " - PulseForge Editor";
				}
			}
			else if (m_Project) Title = m_Project->GetDescription().Name + " - PulseForge Editor";

			if (Title == m_WindowTitle) return;
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			if (Window) glfwSetWindowTitle(Window, Title.c_str());
			m_WindowTitle = std::move(Title);
		}

		void DrawStatusBar()
		{
			if (!BeginEditorChromeBar(
					"##PulseForgeStatusBar", ImGuiDir_Down, 28.0f, PulseForgeEditor::EditorColorToken::MenuBar, m_EditorStyle))
				return;

			m_StatusPanel.DrawStatusBarContents(m_EditorStyle, m_Scene != nullptr, m_Scene && m_SceneEditController.IsDirty(),
				m_SceneRuntime != nullptr,
				PulseForge::Application::Get().GetRendererAPI() == PulseForge::RendererAPI::Vulkan ? "Vulkan" : "OpenGL");
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
			const bool WasDirty = m_MaterialDrafts.IsDirty(*m_Project, Identifier);
			auto SavedPath = m_MaterialDrafts.Save(*m_Project, Identifier, m_Documents);
			if (!SavedPath)
			{
				SetError("Material save failed: " + SavedPath.error());
				return false;
			}
			if (WasDirty)
			{
				m_AssetPreviewAssetsPending = true;
				m_MaterialAssetBindingsPending = true;
			}
			SetStatus(WasDirty ? "Saved material " + PathToUtf8(SavedPath->filename()) + "." : "Material has no unsaved changes.");
			return true;
		}

		bool ReloadMaterialDraft(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project) return false;
			if (auto Reloaded = m_MaterialDrafts.Reload(*m_Project, Identifier, m_Documents); !Reloaded)
			{
				SetError("Material reload failed: " + Reloaded.error());
				return false;
			}
			return true;
		}

		std::vector<PulseForgeEditor::EditorDocumentIdentity> GetDirtyAssetDocuments() const
		{
			std::vector<PulseForgeEditor::EditorDocumentIdentity> Dirty;
			for (const auto& [Identity, Session] : m_Documents.GetSessions())
				if (Session.IsDirty()) Dirty.push_back(Identity);
			if (m_Project)
			{
				const PulseForge::UUID ProjectID = m_Project->GetDescription().ID;
				for (const PulseForge::AssetID& Asset : m_MaterialDrafts.GetDirtyAssets(*m_Project))
				{
					const PulseForgeEditor::EditorDocumentIdentity Identity{ ProjectID, Asset,
						PulseForgeEditor::EditorDocumentKind::Material };
					if (std::find(Dirty.begin(), Dirty.end(), Identity) == Dirty.end()) Dirty.push_back(Identity);
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
				m_DocumentCloseRequest.SetError(Result.error());
				SetError(m_DocumentCloseRequest.GetError());
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
				m_DocumentCloseRequest.SetError(Prepared.error());
				SetError(m_DocumentCloseRequest.GetError());
				return false;
			}
			return CommitPreparedDocumentCloseBatch(std::move(*Prepared));
		}

		void RequestDocumentClose(const PulseForgeEditor::EditorDocumentIdentity& Identity)
		{
			const auto* Session = m_Documents.Find(Identity);
			if (!Session || Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level) return;
			if (!Session->IsDirty())
			{
				if (CloseDocumentBatchClean({ Identity })) return;
				const std::string CloseError = m_DocumentCloseRequest.GetError();
				m_DocumentCloseRequest.BeginCloseDocuments({ Identity }, Session->HostID);
				m_DocumentCloseRequest.SetError(CloseError);
				return;
			}
			m_DocumentCloseRequest.BeginCloseDocuments({ Identity }, Session->HostID);
		}

		void RequestDocumentHostClose(std::string_view HostID)
		{
			const auto* Host = m_Documents.FindHost(HostID);
			if (!Host || Host->Role != PulseForgeEditor::EditorDocumentHostRole::Native) return;
			const auto Documents = Host->Documents;
			const auto DirtyDocuments = PulseForgeEditor::FilterDirtyDocumentIdentities(Documents,
				[this](const auto& Identity)
				{
					const auto* Session = m_Documents.Find(Identity);
					return Session && Session->IsDirty();
				});
			if (DirtyDocuments.empty())
			{
				if (CloseDocumentBatchClean(Documents)) return;
				const std::string CloseError = m_DocumentCloseRequest.GetError();
				m_DocumentCloseRequest.BeginCloseDocuments(Documents, std::string(HostID));
				m_DocumentCloseRequest.SetError(CloseError);
				return;
			}
			m_DocumentCloseRequest.BeginCloseDocuments(Documents, std::string(HostID));
		}

		void QueueAfterDocuments(std::function<void()> Action)
		{
			if (!Action) return;
			if (GetDirtyAssetDocuments().empty())
			{
				m_DeferredEditorOperations.Enqueue(std::move(Action));
				return;
			}
			m_DocumentCloseRequest.BeginResolveChanges(std::string(PulseForgeEditor::EditorDocumentManager::MainHostID), std::move(Action));
		}

		bool ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice Choice)
		{
			auto ResolveDirtyChanges = [this](std::span<const PulseForgeEditor::EditorDocumentIdentity> Documents,
										   PulseForgeEditor::EditorDocumentCloseChoice Choice) -> std::expected<void, std::string>
			{
				if (Choice == PulseForgeEditor::EditorDocumentCloseChoice::Save)
				{
					for (const auto& Identity : Documents)
					{
						auto* Session = m_Documents.Find(Identity);
						const bool DirtyMaterial = Identity.Kind == PulseForgeEditor::EditorDocumentKind::Material && m_Project &&
												   m_MaterialDrafts.IsDirty(*m_Project, Identity.Asset);
						if (Session && Session->IsDirty())
						{
							if (m_DocumentEditors.IsRegistered(Identity.Kind) && Session->EditorState)
							{
								if (!m_DocumentEditors.Save(*Session) || Session->IsDirty())
									return std::unexpected("The document could not be saved; it remains open.");
							}
							else if (Identity.Kind == PulseForgeEditor::EditorDocumentKind::Material && !SaveMaterialDraft(Identity.Asset))
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
						const bool DirtyMaterial = Identity.Kind == PulseForgeEditor::EditorDocumentKind::Material && m_Project &&
												   m_MaterialDrafts.IsDirty(*m_Project, Identity.Asset);
						if ((!Session || !Session->IsDirty()) && !DirtyMaterial) continue;
						if (Session && Session->IsDirty() && m_DocumentEditors.IsRegistered(Identity.Kind) && Session->EditorState)
						{
							if (!m_DocumentEditors.Discard(*Session))
								return std::unexpected(
									"The document editor could not restore its saved state; unsaved edits were preserved.");
							continue;
						}
						if (Identity.Kind != PulseForgeEditor::EditorDocumentKind::Material || !m_Project)
							return std::unexpected("Discard is unavailable for this document type.");
						auto Loaded =
							PulseForge::MaterialAssetService::Load(Identity.Asset, m_Project->GetRootPath(), m_Project->GetAssetRegistry());
						if (!Loaded)
							return std::unexpected("Material reload failed; unsaved draft was preserved: " + Loaded.error().Message);
						Reloaded.emplace_back(Identity, std::move(*Loaded));
					}
					for (auto& [Identity, Material] : Reloaded)
					{
						m_MaterialDrafts.ReplaceDraft(*m_Project, Identity.Asset, std::move(Material));
						if (auto* Session = m_Documents.Find(Identity)) Session->Revision = Session->SavedRevision;
					}
				}
				return {};
			};
			const std::vector<PulseForgeEditor::EditorDocumentIdentity> CurrentDirtyDocuments = GetDirtyAssetDocuments();
			auto Resolved =
				m_DocumentCloseRequest.Resolve(m_Documents, m_DocumentEditors, Choice, CurrentDirtyDocuments, ResolveDirtyChanges,
					[this](std::function<void()> Continuation) { m_DeferredEditorOperations.Enqueue(std::move(Continuation)); });
			if (!Resolved)
			{
				SetError(Resolved.error());
				m_DocumentCloseRequest.SetDialogHostIfEmpty(std::string(PulseForgeEditor::EditorDocumentManager::MainHostID));
				return false;
			}
			if (Resolved->DocumentsClosed) MarkDocumentWorkspaceDirty();
			ImGui::CloseCurrentPopup();
			return true;
		}

		void DrawDocumentCloseDialog(std::string_view HostID)
		{
			if (HostID != m_DocumentCloseRequest.GetDialogHostID()) return;

			const std::string PopupName = "Unsaved Asset Documents###PFDocumentClose_" + std::string(HostID);
			if (m_DocumentCloseRequest.ConsumeDialogOpenRequest()) ImGui::OpenPopup(PopupName.c_str());
			bool PopupOpen = true;
			const bool PopupVisible = ImGui::BeginPopupModal(PopupName.c_str(), &PopupOpen, ImGuiWindowFlags_AlwaysAutoResize);
			if (!PopupVisible) return;
			const std::vector<PulseForgeEditor::EditorDocumentIdentity> CurrentDirtyDocuments = GetDirtyAssetDocuments();
			const auto DirtyDocuments = m_DocumentCloseRequest.GetDirtyPromptDocuments(CurrentDirtyDocuments,
				[this](const auto& Identity)
				{
					const auto* Session = m_Documents.Find(Identity);
					return Session && Session->IsDirty();
				});
			ImGui::TextWrapped("%zu unsaved asset draft%s have changes.", DirtyDocuments.size(), DirtyDocuments.size() == 1 ? "" : "s");
			ImGui::TextUnformatted(DirtyDocuments.empty() ? "The close request is still pending. Retry it or cancel."
														  : "Save or discard these drafts before continuing?");
			if (!m_DocumentCloseRequest.GetError().empty()) ImGui::TextWrapped("%s", m_DocumentCloseRequest.GetError().c_str());
			if (m_DocumentCloseRequest.ShouldOfferCloseRetry(DirtyDocuments.empty()))
			{
				if (ImGui::Button("Retry Close", ImVec2(112.0f, 0.0f)))
					(void)ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice::Save);
				ImGui::SameLine();
			}
			if (ImGui::Button("Save", ImVec2(112.0f, 0.0f))) (void)ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice::Save);
			ImGui::SameLine();
			if (m_EditorStyle.DangerButton(PulseForgeEditor::EditorIcon::Delete, "Discard"))
				(void)ResolveDocumentClose(PulseForgeEditor::EditorDocumentCloseChoice::Discard);
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(112.0f, 0.0f)))
			{
				m_DocumentCloseRequest.Cancel();
				CancelEditorApplicationClose();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			if (!PopupOpen)
			{
				m_DocumentCloseRequest.Cancel();
				CancelEditorApplicationClose();
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
				if (Host.Role == PulseForgeEditor::EditorDocumentHostRole::Native && !Host.Documents.empty()) HostIDs.push_back(Host.ID);
			for (const std::string& HostID : HostIDs)
			{
				const auto* Host = m_Documents.FindHost(HostID);
				if (!Host || !Host->ActiveDocument) continue;
				const auto* ActiveSession = m_Documents.Find(*Host->ActiveDocument);
				if (!ActiveSession) continue;
				std::string WindowName = ActiveSession->DisplayName + "###PFDocumentHost_" + HostID;
				if (m_DocumentHostFocusRequests.erase(HostID) != 0) ImGui::SetNextWindowFocus();
				const auto SavedBounds = Host->Bounds;
				const bool RestoreBounds = SavedBounds.HasValue && m_RestoredHostBoundsPending.contains(HostID);
				const bool CanPositionWindow = PulseForgeEditor::EditorPlatformServices::CanPositionNativeWindows();
				if (RestoreBounds)
				{
					const ResolvedNativeWindowBounds Bounds = ResolveRestoredNativeWindowBounds(SavedBounds);
					if (CanPositionWindow) ImGui::SetNextWindowPos(Bounds.Position, ImGuiCond_Always);
					else if (!m_WindowPositionRestoreWarningLogged)
					{
						PF_WARN(
							"The current window system controls native document-window placement; restoring size without saved position");
						m_WindowPositionRestoreWarningLogged = true;
					}
					ImGui::SetNextWindowSize(Bounds.Size, ImGuiCond_Always);
				}
				else if (ImGuiViewport* MainViewport = ImGui::GetMainViewport())
				{
					if (CanPositionWindow)
						ImGui::SetNextWindowPos(ImVec2(MainViewport->Pos.x + 80.0f, MainViewport->Pos.y + 84.0f), ImGuiCond_FirstUseEver);
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
				if (!IsOpen) RequestDocumentHostClose(HostID);
				if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				{
					if (m_ViewportKeyboardFocused || m_ViewportMouseCapture.OwnsMouse() ||
						m_ViewportCameraGesture != ViewportCameraGesture::None)
						ResetViewportPanelInteraction();
					FocusDocumentHostIfChanged(HostID);
				}
				if (auto* CurrentHost = m_Documents.FindHost(HostID))
				{
					const ImVec2 Size = ImGui::GetWindowSize();
					PulseForgeEditor::EditorDocumentHost::WindowBounds Bounds = CurrentHost->Bounds;
					Bounds.Width = Size.x;
					Bounds.Height = Size.y;
					Bounds.HasValue = true;
					ImGuiViewport* WindowViewport = ImGui::GetWindowViewport();
					if (WindowViewport) Bounds.DpiScale = WindowViewport->DpiScale;
					if (GLFWwindow* NativeWindow = WindowViewport ? static_cast<GLFWwindow*>(WindowViewport->PlatformHandle) : nullptr)
					{
						Bounds.Maximized = glfwGetWindowAttrib(NativeWindow, GLFW_MAXIMIZED) == GLFW_TRUE;
						if (CanPositionWindow)
						{
							const ImVec2 Position = ImGui::GetWindowPos();
							Bounds.X = Position.x;
							Bounds.Y = Position.y;
							Bounds.MonitorName.clear();
							if (GLFWmonitor* Monitor = FindMonitorForBounds(Bounds.X, Bounds.Y, Bounds.Width, Bounds.Height))
								if (const char* MonitorName = glfwGetMonitorName(Monitor)) Bounds.MonitorName = MonitorName;
						}
					}
					if (m_Documents.SetHostBounds(HostID, std::move(Bounds))) MarkDocumentWorkspaceDirty(true);
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
						else DrawUnregisteredAssetDocument(*CurrentHost->ActiveDocument);
					}
				}
				DrawDocumentCloseDialog(HostID);
				ImGui::End();
				if (Visible && IsOpen && RegisteredWorkspaceToDraw)
					if (auto* Session = m_Documents.Find(*RegisteredWorkspaceToDraw))
						m_WorkspacePresentation.DrawWorkspaceContent(
							*Session, m_DocumentEditors, [](void* Context, const auto& Identity)
							{ static_cast<EditorLayer*>(Context)->DrawUnregisteredAssetDocument(Identity); }, this);
			}
		}

		bool ShouldInjectDocumentPresentationFailure(ImGuiViewport* Viewport)
		{
#if !defined(_DEBUG) && !defined(PF_DEBUG)
			(void)Viewport;
			return false;
#else
			if (!m_DocumentHostTestFixture || !m_DocumentHostPresentationFailureTest || !Viewport) return false;
			for (const auto& Host : m_Documents.GetHosts())
			{
				if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty()) continue;
				const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
				if (!Session) continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				if (!Window || Window->Viewport != Viewport) continue;
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
#if !defined(_DEBUG) && !defined(PF_DEBUG)
			(void)Viewport;
			return false;
#else
			if (!m_DocumentHostTestFixture || !m_DocumentHostImageAllocationFailureTest || !Viewport ||
				PulseForge::Application::Get().GetRendererAPI() != PulseForge::RendererAPI::Vulkan)
				return false;
			for (const auto& Host : m_Documents.GetHosts())
			{
				if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty()) continue;
				const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
				if (!Session) continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				if (!Window || Window->Viewport != Viewport) continue;
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
#if !defined(_DEBUG) && !defined(PF_DEBUG)
			(void)Viewport;
			return false;
#else
			if (!m_DocumentHostTestFixture || !m_DocumentHostAcquireOutOfDateTest || !Viewport ||
				PulseForge::Application::Get().GetRendererAPI() != PulseForge::RendererAPI::Vulkan)
				return false;
			for (const auto& Host : m_Documents.GetHosts())
			{
				if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty()) continue;
				const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
				if (!Session) continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				if (!Window || Window->Viewport != Viewport) continue;
				m_DocumentHostAcquireOutOfDateTest = false;
				PF_WARN("Scheduling a Debug-only out-of-date acquire for native document host {}", Host.ID);
				return true;
			}
			return false;
#endif
		}

		void CompleteDocumentPresentationFailureTest(std::string_view SourceHostID,
			const std::vector<PulseForgeEditor::EditorDocumentIdentity>& OriginalDocuments, size_t MovedCount, bool SourceHostRetired)
		{
			if (!m_DocumentHostPresentationFailureInjected) return;
			const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
			bool Recovered = SourceHostRetired && !m_Documents.FindHost(SourceHostID) && MainHost && MovedCount == OriginalDocuments.size();
			for (const auto& Identity : OriginalDocuments)
			{
				const auto* Session = m_Documents.Find(Identity);
				Recovered = Recovered && Session && Session->HostID == PulseForgeEditor::EditorDocumentManager::MainHostID &&
							Session->IsDirty() && MainHost &&
							std::find(MainHost->Documents.begin(), MainHost->Documents.end(), Identity) != MainHost->Documents.end();
			}
			Recovered = Recovered && MainHost &&
						std::any_of(MainHost->Documents.begin(), MainHost->Documents.end(),
							[](const auto& Identity) { return Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level; });
			if (Recovered)
				PF_INFO("Document presentation-failure fixture passed: {} dirty sessions were recovered in the main strip and Level Editor "
						"remained available.",
					OriginalDocuments.size());
			else
				PF_ERROR("Document presentation-failure fixture failed: dirty sessions, source-host retirement, or Level Editor retention "
						 "was incorrect.");

			// The fixture creates synthetic sessions; mark them clean so automated shutdown
			// can validate teardown without opening a user-facing dirty-close prompt.
			for (const auto& Identity : OriginalDocuments)
				if (IsDocumentHostTestFixtureIdentity(Identity))
					if (const auto* Session = m_Documents.Find(Identity)) (void)m_Documents.MarkSaved(Identity, Session->Revision);
			m_DocumentHostPresentationFailureInjected = false;
		}

		void HandlePlatformPresentationFailure(ImGuiViewport* Viewport, std::string_view Error)
		{
			if (Viewport)
			{
				for (const auto& Host : m_Documents.GetHosts())
				{
					if (Host.Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host.Documents.empty()) continue;
					const auto* Session = Host.ActiveDocument ? m_Documents.Find(*Host.ActiveDocument) : nullptr;
					if (!Session) continue;
					const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + Host.ID;
					ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
					if (!Window || Window->Viewport != Viewport) continue;

					PF_ERROR("Could not create native editor presentation target: {}", Error);
					const std::string FailedHostID = Host.ID;
					const std::vector<PulseForgeEditor::EditorDocumentIdentity> FailedHostDocuments = Host.Documents;
					auto Moved = m_Documents.RecoverAllToMain(FailedHostID);
					if (!Moved)
					{
						PF_ERROR("Could not recover all documents from failed native host {}: {}", FailedHostID, Moved.error());
						CompleteDocumentPresentationFailureTest(FailedHostID, FailedHostDocuments, 0, false);
						SetWarning("Native document presentation failed; its open sessions were preserved, but could not be moved to the "
								   "main document strip.");
						Viewport->PlatformRequestClose = false;
						return;
					}
					if (m_Documents.FindHost(FailedHostID) && !m_Documents.RemoveEmptyNativeHost(FailedHostID))
					{
						PF_ERROR("Native host {} was emptied but could not be retired after presentation failure", FailedHostID);
						CompleteDocumentPresentationFailureTest(FailedHostID, FailedHostDocuments, *Moved, false);
						SetWarning(
							"Native document presentation failed; the documents were recovered, but the empty host could not be retired.");
						Viewport->PlatformRequestClose = false;
						MarkDocumentWorkspaceDirty();
						return;
					}
					CompleteDocumentPresentationFailureTest(FailedHostID, FailedHostDocuments, *Moved, !m_Documents.FindHost(FailedHostID));
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
			if (!m_Context || !(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) return;
			ImGuiViewport* MainViewport = ImGui::GetMainViewport();
			std::vector<std::string> HostIDs;
			for (const auto& Host : m_Documents.GetHosts())
				if (Host.Role == PulseForgeEditor::EditorDocumentHostRole::Native && !Host.Documents.empty()) HostIDs.push_back(Host.ID);
			for (const std::string& HostID : HostIDs)
			{
				const auto* Host = m_Documents.FindHost(HostID);
				if (!Host || Host->Role != PulseForgeEditor::EditorDocumentHostRole::Native || Host->Documents.empty()) continue;
				const auto* Session = Host->ActiveDocument ? m_Documents.Find(*Host->ActiveDocument) : nullptr;
				if (!Session) continue;
				const std::string WindowName = Session->DisplayName + "###PFDocumentHost_" + HostID;
				ImGuiWindow* Window = ImGui::FindWindowByName(WindowName.c_str());
				ImGuiViewport* Viewport = Window ? Window->Viewport : nullptr;
				if (!Viewport || Viewport == MainViewport || !Viewport->PlatformRequestClose) continue;
				Viewport->PlatformRequestClose = false;
				RequestDocumentHostClose(HostID);
			}
		}

		void QueueDocumentTabClose(const PulseForgeEditor::EditorDocumentIdentity& Identity)
		{
			if (!m_PendingDocumentTabClose) m_PendingDocumentTabClose = Identity;
		}

		void ProcessPendingDocumentTabClose()
		{
			if (!m_PendingDocumentTabClose) return;

			const PulseForgeEditor::EditorDocumentIdentity Identity = *m_PendingDocumentTabClose;
			m_PendingDocumentTabClose.reset();
			RequestDocumentClose(Identity);
		}

		void QueueDocumentTransfer(const PulseForgeEditor::EditorDocumentIdentity& Identity, std::string DestinationHostID, size_t Index,
			bool CreateNativeHost = false,
			std::optional<PulseForgeEditor::EditorDocumentHost::WindowBounds> NativeHostBounds = std::nullopt)
		{
			(void)m_DocumentTabInteraction.QueueTransfer(PulseForgeEditor::EditorDocumentTransferRequest{
				Identity, std::move(DestinationHostID), Index, CreateNativeHost, std::move(NativeHostBounds) });
		}

		void ProcessReleasedDocumentTabDrag()
		{
			const ImGuiIO& IO = ImGui::GetIO();
			const auto ActiveIdentity = m_DocumentTabInteraction.ActiveDragIdentity();
			const auto* Session = ActiveIdentity ? m_Documents.Find(*ActiveIdentity) : nullptr;
			const auto* SourceHost = Session ? m_Documents.FindHost(Session->HostID) : nullptr;
			const PulseForgeEditor::EditorDocumentTabDragFrame Frame{ .LeftButtonReleased = IO.MouseReleased[ImGuiMouseButton_Left],
				.LeftButtonDown = IO.MouseDown[ImGuiMouseButton_Left],
				.ImGuiPayloadActive = ImGui::GetDragDropPayload() != nullptr,
				.PointerPosition = { IO.MousePos.x, IO.MousePos.y },
				.HoveredViewportID = IO.MouseHoveredViewport,
				.MaximumObservedDistanceSquared = IO.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left],
				.DragThreshold = IO.MouseDragThreshold,
				.SourceIsMainHost = SourceHost && SourceHost->Role == PulseForgeEditor::EditorDocumentHostRole::Main,
				.SourceIsNativeHost = SourceHost && SourceHost->Role == PulseForgeEditor::EditorDocumentHostRole::Native };
			const auto Released = m_DocumentTabInteraction.UpdateDrag(Frame);
			if (!Released) return;

			// The captured identity stays authoritative when a cross-viewport release clears ImGui's payload.
			if (Released->Action == PulseForgeEditor::DocumentTabReleaseAction::TransferToStrip && Released->DropLocation)
			{
				QueueDocumentTransfer(Released->Identity, Released->DropLocation->HostID, Released->DropLocation->Index);
				return;
			}
			if (Released->Action != PulseForgeEditor::DocumentTabReleaseAction::CreateNativeHost || !Session) return;

			QueueDocumentTransfer(Released->Identity, {}, 0, true,
				MakeDocumentDropWindowBounds(ImVec2(Released->ReleasePosition.X, Released->ReleasePosition.Y)));
		}

		[[nodiscard]] std::optional<std::pair<std::string, size_t>> FindDocumentTabDropTarget(
			const ImVec2& Position, ImGuiID HoveredViewportID) const
		{
			const auto Result =
				m_DocumentTabInteraction.FindDropTarget({ Position.x, Position.y }, static_cast<uint32_t>(HoveredViewportID));
			if (!Result) return std::nullopt;
			return std::pair{ Result->HostID, Result->Index };
		}

		void ProcessPendingDocumentTransfer()
		{
			auto Request = m_DocumentTabInteraction.TakePendingTransfer();
			if (!Request) return;

			PulseForgeEditor::EditorDocumentTransferRequest Pending = std::move(*Request);
			const auto* Session = m_Documents.Find(Pending.Identity);
			if (!Session) return;

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

			const auto Moved = m_Documents.Transfer(
				Pending.Identity, DestinationHostID, Pending.DestinationIndex, [](const auto&, const auto&) { return true; });
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

			if (Moved->Changed) MarkDocumentWorkspaceDirty();
			if (Moved->EmptiedSourceHost)
			{
				m_RestoredHostBoundsPending.erase(*Moved->EmptiedSourceHost);
				(void)m_Documents.RemoveEmptyNativeHost(*Moved->EmptiedSourceHost);
			}
			m_DocumentHostFocusRequests.insert(std::move(DestinationHostID));
		}

		void TryTransferDocumentPayload(const ImGuiPayload* Payload, std::string_view DestinationHostID, size_t Index)
		{
			if (!Payload || !Payload->IsDelivery() || !Payload->Data || Payload->DataSize <= 1) return;
			const std::string_view Encoded(static_cast<const char*>(Payload->Data), static_cast<size_t>(Payload->DataSize - 1));
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
			if (!Identity) return;
			QueueDocumentTransfer(*Identity, std::string(DestinationHostID), Index);
		}

		void DrawDocumentTabStrip(std::string_view HostID)
		{
			const auto* Host = m_Documents.FindHost(HostID);
			if (!Host) return;
			const std::vector<PulseForgeEditor::EditorDocumentIdentity> Documents = Host->Documents;
			for (size_t Index = 0; Index < Documents.size(); ++Index)
			{
				const auto* Session = m_Documents.Find(Documents[Index]);
				if (!Session) continue;
				ImGui::PushID(GetDocumentPayload(Session->Identity).c_str());
				std::string Label =
					Session->Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level ? "Level Editor" : Session->DisplayName;
				if (Session->IsDirty()) Label += " *";
				const bool HasCloseButton = Session->Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level;
				constexpr float TabHeight = 25.0f;
				constexpr float LeftPadding = 10.0f;
				constexpr float LabelToCloseGap = 1.0f;
				constexpr float CloseRegionWidth = 26.0f;
				constexpr float RightPadding = 2.0f;
				const ImVec2 LabelSize = ImGui::CalcTextSize(Label.c_str());
				const float TabWidth =
					LeftPadding + LabelSize.x + (HasCloseButton ? LabelToCloseGap + CloseRegionWidth + RightPadding : LeftPadding);
				const bool Active = Host->ActiveDocument == Session->Identity;
				const bool Pressed = ImGui::Selectable("##DocumentTab", Active, ImGuiSelectableFlags_None, ImVec2(TabWidth, TabHeight));
				const ImVec2 TabMinimum = ImGui::GetItemRectMin();
				const ImVec2 TabMaximum = ImGui::GetItemRectMax();
				const ImGuiViewport* TabViewport = ImGui::GetWindowViewport();
				const ImGuiID TabViewportID = TabViewport ? TabViewport->ID : 0;
				m_DocumentTabInteraction.RegisterDropTarget({ std::string(HostID), static_cast<uint32_t>(TabViewportID),
					{ TabMinimum.x, TabMinimum.y }, { TabMaximum.x, TabMaximum.y }, Index, Index + 1 });
				if (m_DocumentHostTabTearOffTest && Session->Identity == m_DocumentHostTabTearOffTestIdentity)
				{
					m_DocumentHostTabTearOffTestTabCenter =
						ImVec2((TabMinimum.x + TabMaximum.x) * 0.5f, (TabMinimum.y + TabMaximum.y) * 0.5f);
					m_DocumentHostTabTearOffTestTabViewportID = TabViewportID;
					m_DocumentHostTabTearOffTestTabCenterValid = true;
				}
				if (m_DocumentHostTabReattachTest && Session->Identity == m_DocumentHostTabReattachTestIdentity)
				{
					m_DocumentHostTabReattachTestTabCenter =
						ImVec2((TabMinimum.x + TabMaximum.x) * 0.5f, (TabMinimum.y + TabMaximum.y) * 0.5f);
					m_DocumentHostTabReattachTestTabViewportID = TabViewportID;
					m_DocumentHostTabReattachTestTabCenterValid = true;
				}
				if (m_DocumentHostTabReorderTest)
				{
					if (Session->Identity == m_DocumentHostTabReorderTestFirst)
					{
						m_DocumentHostTabReorderTestFirstTabCenter =
							ImVec2((TabMinimum.x + TabMaximum.x) * 0.5f, (TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabReorderTestFirstTabViewportID = TabViewportID;
						m_DocumentHostTabReorderTestFirstTabCenterValid = true;
					}
					else if (Session->Identity == m_DocumentHostTabReorderTestSecond)
					{
						const float TabWidth = TabMaximum.x - TabMinimum.x;
						m_DocumentHostTabReorderTestSecondBeforePosition =
							ImVec2(TabMinimum.x + TabWidth * 0.35f, (TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabReorderTestSecondAfterPosition =
							ImVec2(TabMinimum.x + TabWidth * 0.65f, (TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabReorderTestSecondBeforePositionValid = true;
						m_DocumentHostTabReorderTestSecondAfterPositionValid = true;
					}
				}
				if (m_DocumentHostTabGroupTest)
				{
					if (Session->Identity == m_DocumentHostTabTearOffTestIdentity)
					{
						m_DocumentHostTabGroupSourceCenter =
							ImVec2((TabMinimum.x + TabMaximum.x) * 0.5f, (TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabGroupSourceViewportID = TabViewportID;
						m_DocumentHostTabGroupSourceCenterValid = true;
					}
					else if (Session->Identity == m_DocumentHostTabGroupTargetIdentity)
					{
						m_DocumentHostTabGroupTargetCenter =
							ImVec2((TabMinimum.x + TabMaximum.x) * 0.5f, (TabMinimum.y + TabMaximum.y) * 0.5f);
						m_DocumentHostTabGroupTargetViewportID = TabViewportID;
						m_DocumentHostTabGroupTargetCenterValid = true;
					}
				}
				if (m_DocumentHostTabReattachTest && HostID == PulseForgeEditor::EditorDocumentManager::MainHostID &&
					Session->Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level)
				{
					m_DocumentHostTabReattachTestMainDropPosition =
						ImVec2(TabMinimum.x + (TabMaximum.x - TabMinimum.x) * 0.75f, (TabMinimum.y + TabMaximum.y) * 0.5f);
					m_DocumentHostTabReattachTestMainViewportID = TabViewportID;
					m_DocumentHostTabReattachTestMainDropPositionValid = true;
				}
				const bool TabHovered = ImGui::IsItemHovered();
				const ImVec2 MousePosition = ImGui::GetIO().MousePos;
				const float CloseRegionMinimumX = TabMinimum.x + LeftPadding + LabelSize.x + LabelToCloseGap;
				const ImVec2 CloseButtonMinimum(CloseRegionMinimumX, TabMinimum.y + 1.0f);
				const ImVec2 CloseButtonMaximum(CloseRegionMinimumX + CloseRegionWidth, TabMaximum.y - 1.0f);
				const bool CloseHovered = HasCloseButton && TabHovered && MousePosition.x >= CloseButtonMinimum.x &&
										  MousePosition.x < CloseButtonMaximum.x && MousePosition.y >= CloseButtonMinimum.y &&
										  MousePosition.y < CloseButtonMaximum.y;
				if (Session->Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level &&
					PulseForgeEditor::ShouldCaptureDocumentTabDrag(
						TabHovered, CloseHovered, ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left]))
				{
					const ImVec2 DragStart = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
					m_DocumentTabInteraction.BeginDrag(Session->Identity,
						PulseForgeEditor::DocumentTabPointerPosition{ DragStart.x, DragStart.y }, { MousePosition.x, MousePosition.y });
				}
				if (Pressed && CloseHovered) QueueDocumentTabClose(Session->Identity);
				else if (Pressed)
				{
					(void)m_Documents.Activate(Session->Identity);
					MarkDocumentWorkspaceDirty();
					m_DocumentHostFocusRequests.insert(std::string(HostID));
				}

				ImDrawList* DrawList = ImGui::GetWindowDrawList();
				const ImVec2 LabelPosition(TabMinimum.x + LeftPadding, TabMinimum.y + (TabHeight - LabelSize.y) * 0.5f);
				DrawList->AddText(LabelPosition, ImGui::GetColorU32(ImGuiCol_Text), Label.c_str());
				if (HasCloseButton)
				{
					const ImVec2 CloseCenter(CloseRegionMinimumX + CloseRegionWidth * 0.5f, TabMinimum.y + TabHeight * 0.5f);
					constexpr float CloseHalfExtent = 8.5f;
					const ImU32 CloseColor = ImGui::GetColorU32(CloseHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
					DrawList->AddLine(ImVec2(CloseCenter.x - CloseHalfExtent, CloseCenter.y - CloseHalfExtent),
						ImVec2(CloseCenter.x + CloseHalfExtent, CloseCenter.y + CloseHalfExtent), CloseColor, 2.0f);
					DrawList->AddLine(ImVec2(CloseCenter.x + CloseHalfExtent, CloseCenter.y - CloseHalfExtent),
						ImVec2(CloseCenter.x - CloseHalfExtent, CloseCenter.y + CloseHalfExtent), CloseColor, 2.0f);
					if (CloseHovered) ImGui::SetTooltip("Close %s", Session->DisplayName.c_str());
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
							QueueDocumentTransfer(Session->Identity, std::string(PulseForgeEditor::EditorDocumentManager::MainHostID), 0);

						if (CurrentHost->Role == PulseForgeEditor::EditorDocumentHostRole::Native &&
							ImGui::BeginMenu("Move to Existing Document Window"))
						{
							for (const auto& CandidateHost : m_Documents.GetHosts())
							{
								if (CandidateHost.Role != PulseForgeEditor::EditorDocumentHostRole::Native || CandidateHost.ID == HostID ||
									CandidateHost.PlacementKind != Session->Identity.Kind)
									continue;
								const bool SameProject = std::all_of(CandidateHost.Documents.begin(), CandidateHost.Documents.end(),
									[&Session](const auto& CandidateIdentity)
									{ return CandidateIdentity.Project == Session->Identity.Project; });
								if (!SameProject) continue;
								const auto* ActiveSession =
									CandidateHost.ActiveDocument ? m_Documents.Find(*CandidateHost.ActiveDocument) : nullptr;
								std::string ItemLabel = ActiveSession ? ActiveSession->DisplayName : CandidateHost.ID;
								ItemLabel += "##" + CandidateHost.ID;
								if (ImGui::MenuItem(ItemLabel.c_str()))
									QueueDocumentTransfer(Session->Identity, CandidateHost.ID, CandidateHost.Documents.size());
							}
							ImGui::EndMenu();
						}
					}
					ImGui::EndPopup();
				}
				if (Session->Identity.Kind != PulseForgeEditor::EditorDocumentKind::Level &&
					ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
				{
					// Preserve the original press coordinate as the drag source starts. If
					// item activation was not observed, using the release position as the
					// fallback would produce a zero-length drag and suppress tear-off.
					const ImVec2 PressPosition = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
					std::optional<PulseForgeEditor::DocumentTabPointerPosition> DragStart;
					if (PulseForgeEditor::IsUsableDocumentTabPointerPosition(PressPosition.x, PressPosition.y))
						DragStart = PulseForgeEditor::DocumentTabPointerPosition{ PressPosition.x, PressPosition.y };
					m_DocumentTabInteraction.BeginDrag(Session->Identity, DragStart, { MousePosition.x, MousePosition.y });
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
				m_DocumentTabInteraction.RegisterDropTarget(
					{ std::string(HostID), DropViewport ? static_cast<uint32_t>(DropViewport->ID) : 0, { DropMinimum.x, DropMinimum.y },
						{ DropMaximum.x, DropMaximum.y }, Documents.size(), Documents.size() });
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
			if (!BeginEditorChromeBar(
					"##PulseForgeDocumentStrip", ImGuiDir_Up, 34.0f, PulseForgeEditor::EditorColorToken::PanelRaised, m_EditorStyle))
				return;
			if (ImGui::GetWindowViewport() && ImGui::GetMainViewport() && ImGui::GetWindowViewport()->ID == ImGui::GetMainViewport()->ID &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
			if (const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
				MainHost && !MainHost->Documents.empty())
				DrawDocumentTabStrip(MainHost->ID);
			else m_EditorStyle.IconText(PulseForgeEditor::EditorIcon::Scene, "Level Editor");
			EndEditorChromeBar();
		}

		void DrawWorkspacePanels()
		{
			const auto* MainHost = m_Documents.FindHost(PulseForgeEditor::EditorDocumentManager::MainHostID);
			const std::optional<PulseForgeEditor::EditorDocumentIdentity> MainActive = MainHost ? MainHost->ActiveDocument : std::nullopt;
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
						else DrawUnregisteredAssetDocument(Session->Identity);
					}
				ImGui::End();
				if (DrawDocumentWorkspaceContent)
					if (auto* Session = m_Documents.Find(*MainActive))
						m_WorkspacePresentation.DrawWorkspaceContent(
							*Session, m_DocumentEditors, [](void* Context, const auto& Identity)
							{ static_cast<EditorLayer*>(Context)->DrawUnregisteredAssetDocument(Identity); }, this);
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
								const std::string SceneName =
									Record ? PathToUtf8(Record->ProjectRelativePath) : std::string("<unregistered>");
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
								m_EditorStyle.EmptyState(
									PulseForgeEditor::EditorIcon::Scene, "No scene is open. Create or open one from the Scene menu.");
						}
						else
							m_EditorStyle.EmptyState(
								PulseForgeEditor::EditorIcon::FolderOpen, "Create or open a project from the File menu.");

						ImGui::Separator();
						m_StatusPanel.DrawMessage(m_EditorStyle, PulseForgeEditor::EditorStatusMessagePresentation::Wrapped);
					}
					ImGui::End();
				}
				DrawSceneViewportPanel();
				DrawHierarchyPanel();
				DrawInspectorPanel();
#ifdef PF_EDITOR_RENDERER_OPENGL
				if (!m_OpenGLDiagnosticCapturePath.empty() && !m_OpenGLDiagnosticContentBrowserSelected) ImGui::SetNextWindowFocus();
#endif
				DrawContentBrowserPanel();
				if (m_InitialMaterialDetailsDockPending)
				{
					if (m_ForceMaterialDetailsDock)
						(void)m_Layout.DockPanelBeside(
							PulseForgeEditor::EditorPanel::MaterialDetails, PulseForgeEditor::EditorPanel::Inspector);
					else
						(void)m_Layout.DockPanelBesideIfUnplaced(
							PulseForgeEditor::EditorPanel::MaterialDetails, PulseForgeEditor::EditorPanel::Inspector);
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
			if (!m_DocumentHostTabReattachTest) return;

			size_t PanelIndex = m_DocumentHostTabReattachInvalidDropTargets.size();
			switch (Panel)
			{
			case PulseForgeEditor::EditorPanel::Hierarchy:
				PanelIndex = 0;
				break;
			case PulseForgeEditor::EditorPanel::Inspector:
				PanelIndex = 1;
				break;
			case PulseForgeEditor::EditorPanel::SceneViewport:
				PanelIndex = 2;
				break;
			case PulseForgeEditor::EditorPanel::ContentBrowser:
				PanelIndex = 3;
				break;
			case PulseForgeEditor::EditorPanel::Console:
				PanelIndex = 4;
				break;
			case PulseForgeEditor::EditorPanel::Scene:
			case PulseForgeEditor::EditorPanel::MaterialDetails:
			case PulseForgeEditor::EditorPanel::Count:
				return;
			}
			if (PanelIndex >= DocumentTabTestInvalidDropPanelNames.size()) return;

			ImGuiViewport* Viewport = ImGui::GetWindowViewport();
			const ImVec2 Position = ImGui::GetWindowPos();
			const ImVec2 Size = ImGui::GetWindowSize();
			if (!Viewport || Size.x < 100.0f || Size.y < 100.0f) return;

			const ImVec2 RegionMinimum = ImGui::GetWindowContentRegionMin();
			const ImVec2 RegionMaximum = ImGui::GetWindowContentRegionMax();
			const float MinimumX = Position.x + RegionMinimum.x;
			const float MinimumY = Position.y + RegionMinimum.y;
			const float MaximumX = Position.x + RegionMaximum.x;
			const float MaximumY = Position.y + RegionMaximum.y;
			if (MaximumX - MinimumX < 12.0f || MaximumY - MinimumY < 12.0f) return;

			const float CenterX = (MinimumX + MaximumX) * 0.5f;
			const float CenterY = (MinimumY + MaximumY) * 0.5f;
			constexpr float EdgeInset = 3.0f;
			const std::array<ImVec2, DocumentTabTestDropPointsPerPanel> Positions = { ImVec2(CenterX, CenterY),
				ImVec2(CenterX, MinimumY + EdgeInset), ImVec2(MinimumX + EdgeInset, CenterY), ImVec2(MaximumX - EdgeInset, CenterY),
				ImVec2(CenterX, MaximumY - EdgeInset) };
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
			if (!IsOpen || !*IsOpen) return false;

			const bool WasOpen = *IsOpen;
			const auto& Descriptor = PulseForgeEditor::EditorPanelDescriptors[static_cast<size_t>(Panel)];
			const std::string WindowName = DisplayTitle.empty() ? std::string(Descriptor.WindowName)
																: std::string(DisplayTitle) + "###" + std::string(Descriptor.WindowName);
			const bool ContentsVisible = ImGui::Begin(WindowName.c_str(), IsOpen);
			if (ContentsVisible) CaptureDocumentTabTestInvalidDropTarget(Panel);
			if (ImGui::GetWindowViewport() && ImGui::GetMainViewport() && ImGui::GetWindowViewport()->ID == ImGui::GetMainViewport()->ID &&
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
				FocusDocumentHostIfChanged(PulseForgeEditor::EditorDocumentManager::MainHostID);
			if (Panel != PulseForgeEditor::EditorPanel::SceneViewport && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
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
			if (m_Layout.SetPanelVisible(Panel, Visible)) HandleEditorPanelVisibilityChange(Panel);
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

		void CancelViewportInteractionForLayoutChange() { ResetViewportPanelInteraction(); }

		void ReportLayoutPersistenceError(const std::string& Message)
		{
			if (m_LayoutPersistenceError == Message) return;
			m_LayoutPersistenceError = Message;
			PF_WARN("Editor workspace persistence failed: {}", Message);
		}

		void DrawViewportOrientationWidget(ImDrawList* DrawList, ImVec2 ImageMinimum, ImVec2 ImageMaximum) const
		{
			const auto Axes = PulseForgeEditor::ProjectViewportOrientationAxes(m_EditorCamera.GetForward());
			if (!Axes) return;

			const ImVec2 Center(ImageMaximum.x - 51.0f, ImageMinimum.y + 78.0f);
			constexpr float AxisLength = 25.0f;
			DrawList->AddCircle(
				Center, 29.0f, ImGui::GetColorU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Border)), 24, 1.0f);

			constexpr std::array<const char*, 3> Labels = { "X", "Y", "Z" };
			constexpr std::array<PulseForgeEditor::EditorColorToken, 3> Tokens = { PulseForgeEditor::EditorColorToken::GizmoAxisX,
				PulseForgeEditor::EditorColorToken::GizmoAxisY, PulseForgeEditor::EditorColorToken::GizmoAxisZ };
			std::array<size_t, 3> DrawOrder = { 0, 1, 2 };
			std::sort(
				DrawOrder.begin(), DrawOrder.end(), [&](size_t Left, size_t Right) { return (*Axes)[Left].Depth > (*Axes)[Right].Depth; });

			for (const size_t Axis : DrawOrder)
			{
				const glm::vec2 Direction = (*Axes)[Axis].ScreenDirection;
				const float DirectionLength = glm::length(Direction);
				ImVec4 Color = m_EditorStyle.GetColor(Tokens[Axis]);
				if ((*Axes)[Axis].Depth > 0.2f) Color.w *= 0.55f;
				const ImU32 PackedColor = ImGui::GetColorU32(Color);
				if (DirectionLength < 0.16f)
				{
					DrawList->AddCircleFilled(Center, 3.0f, PackedColor, 12);
					DrawList->AddText(ImVec2(Center.x + 5.0f, Center.y + 2.0f), PackedColor, Labels[Axis]);
					continue;
				}

				const glm::vec2 NormalizedDirection = Direction / DirectionLength;
				const ImVec2 End(Center.x + NormalizedDirection.x * AxisLength, Center.y + NormalizedDirection.y * AxisLength);
				const ImVec2 ArrowBase(End.x - NormalizedDirection.x * 6.0f, End.y - NormalizedDirection.y * 6.0f);
				const ImVec2 Perpendicular(-NormalizedDirection.y * 3.0f, NormalizedDirection.x * 3.0f);
				DrawList->AddLine(Center, ArrowBase, PackedColor, 2.0f);
				DrawList->AddTriangleFilled(End, ImVec2(ArrowBase.x + Perpendicular.x, ArrowBase.y + Perpendicular.y),
					ImVec2(ArrowBase.x - Perpendicular.x, ArrowBase.y - Perpendicular.y), PackedColor);
				DrawList->AddText(ImVec2(End.x + NormalizedDirection.x * 3.0f - 3.0f, End.y + NormalizedDirection.y * 3.0f - 5.0f),
					PackedColor, Labels[Axis]);
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
				if (!m_Project) m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::FolderOpen, "Open a project to render a scene.");
				else if (!m_Scene) m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Scene, "Open a scene to render it here.");
				else
				{
#ifdef PF_EDITOR_RENDERER_OPENGL
					m_EditorStyle.TextMuted("The OpenGL fallback retains the editor shell; the scene viewport requires Vulkan.");
#else
					DrawViewportToolbar();
					const ImVec2 Available = ImGui::GetContentRegionAvail();
					if (Available.x <= 1.0f || Available.y <= 1.0f)
					{
						ResetViewportPanelInteraction();
						m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::Scene, "Expand the panel to display the scene.");
					}
					else
					{
						(void)EnsureViewportTarget(Available);
						if (m_ViewportRenderView.HasTarget() && m_ViewportRenderView.GetTextureID() != 0)
						{
							const float TargetAspect =
								static_cast<float>(m_ViewportRenderView.GetWidth()) / static_cast<float>(m_ViewportRenderView.GetHeight());
							ImVec2 ImageSize = Available;
							if (ImageSize.x / ImageSize.y > TargetAspect) ImageSize.x = ImageSize.y * TargetAspect;
							else ImageSize.y = ImageSize.x / TargetAspect;
							const ImVec2 Cursor = ImGui::GetCursorScreenPos();
							ImGui::SetCursorScreenPos(
								ImVec2(Cursor.x + (Available.x - ImageSize.x) * 0.5f, Cursor.y + (Available.y - ImageSize.y) * 0.5f));
							ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_ViewportRenderView.GetTextureID())), ImageSize);
							const bool ImageHovered = ImGui::IsItemHovered();
							const bool ImageClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
							const ImVec2 ImageMinimum = ImGui::GetItemRectMin();
							const ImVec2 ImageMaximum = ImGui::GetItemRectMax();
							ImGui::GetWindowDrawList()->AddRect(ImageMinimum, ImageMaximum,
								ImGui::GetColorU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::BorderStrong)));
							m_ViewportImageRect = { { ImageMinimum.x, ImageMinimum.y }, { ImageMaximum.x, ImageMaximum.y } };

							const ImVec2 HelpMinimum(ImageMaximum.x - 34.0f, ImageMinimum.y + 8.0f);
							const ImVec2 HelpMaximum(HelpMinimum.x + 27.0f, HelpMinimum.y + 27.0f);
							const ImVec2 MousePosition = ImGui::GetIO().MousePos;
							const ImVec2 WidgetMinimum(ImageMaximum.x - 92.0f, ImageMinimum.y + 38.0f);
							const ImVec2 WidgetMaximum(ImageMaximum.x - 5.0f, ImageMinimum.y + 112.0f);
							const bool ShowEditorOverlays = !m_SceneRuntime && m_ShowEditorOverlays;
							const bool OverHelp = ShowEditorOverlays && MousePosition.x >= HelpMinimum.x &&
												  MousePosition.x <= HelpMaximum.x && MousePosition.y >= HelpMinimum.y &&
												  MousePosition.y <= HelpMaximum.y;
							const bool OverOrientationWidget = ShowEditorOverlays && MousePosition.x >= WidgetMinimum.x &&
															   MousePosition.x <= WidgetMaximum.x && MousePosition.y >= WidgetMinimum.y &&
															   MousePosition.y <= WidgetMaximum.y;
							m_ViewportImageHovered = ImageHovered && !OverHelp && !OverOrientationWidget;

							if (ShowEditorOverlays)
							{
								const ImVec2 CursorAfterImage = ImGui::GetCursorScreenPos();
								ImGui::SetCursorScreenPos(HelpMinimum);
								(void)m_EditorStyle.ToolIconButton(PulseForgeEditor::EditorIcon::Info, "Help", "ViewportNavigationHelp",
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
							if (ShowEditorOverlays) DrawViewportOrientationWidget(ImGui::GetWindowDrawList(), ImageMinimum, ImageMaximum);
							if (m_ViewportRenderView.IsEnvironmentLightingPending())
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

						if (!m_ViewportRenderView.GetTargetError().empty())
						{
							ImGui::Separator();
							ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Error), "%s",
								m_ViewportRenderView.GetTargetError().c_str());
						}
						if (!m_ViewportRenderView.GetSceneError().empty())
						{
							ImGui::Separator();
							ImGui::TextColored(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Warning), "%s",
								m_ViewportRenderView.GetSceneError().c_str());
						}
					}
#endif
				}
			}
			else ResetViewportPanelInteraction();
			ImGui::End();
		}

#ifndef PF_EDITOR_RENDERER_OPENGL
		void DrawViewportToolbar()
		{
			const PulseForgeEditor::EditorViewportToolbarActions Actions =
				m_ViewportToolbar.Draw(m_EditorStyle, m_ViewportTools, m_Scene && !m_SceneRuntime);
			if (Actions.SelectedTool) SetViewportTool(*Actions.SelectedTool);
			if (Actions.ToggleCoordinateSpace)
			{
				CancelGizmoInteraction();
				m_ViewportTools.ToggleCoordinateSpace();
			}
		}
#endif

		void SetViewportTool(PulseForgeEditor::ViewportTool Tool)
		{
			if (m_ViewportTools.GetTool() == Tool) return;
			CancelGizmoInteraction();
			(void)m_ViewportTools.SetTool(Tool);
		}

		void HandleViewportToolShortcuts()
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			return;
#else
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport)) return;

			const ImGuiIO& IO = ImGui::GetIO();
			const PulseForgeEditor::ViewportShortcutContext ShortcutContext{ .SceneAvailable = m_Scene != nullptr,
				.RuntimeActive = m_SceneRuntime != nullptr,
				.CameraNavigationActive = m_ViewportMouseCapture.OwnsMouse(),
				.ViewportKeyboardFocused = m_ViewportKeyboardFocused,
				.RightMouseHeld = IO.MouseDown[ImGuiMouseButton_Right],
				.TextInputActive = IO.WantTextInput,
				.ActiveImGuiItem = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui->NavLayer == ImGuiNavLayer_Menu || GImGui->NavWindowingTarget != nullptr,
				.ModifierHeld = IO.KeyCtrl || IO.KeyShift || IO.KeyAlt || IO.KeySuper };
			if (m_ViewportKeyboardFocused)
			{
				if (!PulseForgeEditor::CanHandleFocusedViewportToolShortcuts(ShortcutContext)) return;
				constexpr std::array<std::pair<ImGuiKey, char>, 4> FocusedShortcuts = { { { ImGuiKey_1, '1' }, { ImGuiKey_2, '2' },
					{ ImGuiKey_3, '3' }, { ImGuiKey_4, '4' } } };
				for (const auto& [Key, Character] : FocusedShortcuts)
					if (ImGui::IsKeyPressed(Key, false))
						if (const auto Tool = PulseForgeEditor::GetFocusedViewportToolForShortcut(Character)) SetViewportTool(*Tool);
				return;
			}

			// With no viewport keyboard focus, Q/W/E/R retain their established editor-wide tool bindings.
			if (!PulseForgeEditor::CanHandleViewportToolShortcuts(ShortcutContext)) return;

			constexpr std::array<std::pair<ImGuiKey, char>, 4> Shortcuts = { { { ImGuiKey_Q, 'Q' }, { ImGuiKey_W, 'W' },
				{ ImGuiKey_E, 'E' }, { ImGuiKey_R, 'R' } } };
			for (const auto& [Key, Character] : Shortcuts)
				if (ImGui::IsKeyPressed(Key, false))
					if (const auto Tool = PulseForgeEditor::GetViewportToolForShortcut(Character)) SetViewportTool(*Tool);
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
				PulseForgeEditor::ResolveViewportWheelAction({ .PointerOverViewportImage = m_ViewportImageHovered,
					.RightMouseNavigationActive = m_ViewportMouseCapture.OwnsMouse(),
					.RightMouseHeld = IO.MouseDown[ImGuiMouseButton_Right],
					.RuntimeActive = m_SceneRuntime != nullptr,
					.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) });
			if (WheelAction == PulseForgeEditor::ViewportWheelAction::Zoom && IO.MouseWheel != 0.0f &&
				!IO.MouseDown[ImGuiMouseButton_Left] && !IO.MouseDown[ImGuiMouseButton_Middle] && !ImGui::IsAnyItemActive())
				m_EditorCamera.Dolly(PulseForgeEditor::GetViewportWheelDollyDelta(IO.MouseWheel));
			if (!m_ShowEditorOverlays) return;

			if (SelectedEntity() && !m_Scene->FindEntity(*SelectedEntity()))
			{
				SelectSingleEntity(std::nullopt);
				CancelGizmoInteraction();
			}

			const glm::vec2 Mouse{ IO.MousePos.x, IO.MousePos.y };
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
				(m_ViewportCameraGesture != ViewportCameraGesture::None || m_ViewportGizmoInteraction.IsActive()))
			{
				m_ViewportCameraGesture = ViewportCameraGesture::None;
				if (m_ViewportGizmoInteraction.IsActive()) CancelGizmoInteraction();
				return;
			}
			if (m_ViewportGizmoInteraction.IsActive())
			{
				UpdateGizmoDrag(Mouse);
				if (SelectedEntity())
				{
					if (const auto Selected = m_Scene->FindEntity(*SelectedEntity()))
						if (const auto Geometry = BuildViewportGizmoGeometry(*Selected)) DrawViewportGizmo(*Geometry, std::nullopt);
				}
				if (PulseForgeEditor::GetViewportGizmoPointerOutcome(
						{ m_ViewportGizmoInteraction.IsActive(), IO.MouseDown[ImGuiMouseButton_Left],
							IO.MouseReleased[ImGuiMouseButton_Left] },
						false) == PulseForgeEditor::ViewportGizmoPointerOutcome::Complete)
					CompleteGizmoInteraction();
				return;
			}
			if (m_ViewportCameraGesture != ViewportCameraGesture::None)
			{
				const int Button = m_ViewportCameraGesture == ViewportCameraGesture::Orbit ? ImGuiMouseButton_Left
								   : m_ViewportCameraGesture == ViewportCameraGesture::Pan ? ImGuiMouseButton_Middle
																						   : ImGuiMouseButton_Right;
				if (IO.MouseReleased[Button] || !IO.MouseDown[Button] || !IO.KeyAlt)
				{
					m_ViewportCameraGesture = ViewportCameraGesture::None;
					return;
				}
				const ImVec2 Delta = IO.MouseDelta;
				if (m_ViewportCameraGesture == ViewportCameraGesture::Orbit) m_EditorCamera.Orbit(Delta.x, Delta.y);
				else if (m_ViewportCameraGesture == ViewportCameraGesture::Pan)
					m_EditorCamera.Pan(Delta.x, Delta.y, m_ViewportImageRect.Maximum.y - m_ViewportImageRect.Minimum.y);
				else m_EditorCamera.Dolly(-Delta.y * 0.08f);
				return;
			}

			std::optional<ViewportGizmoGeometry> Geometry;
			if (SelectedEntity() && m_ViewportTools.GetTool() != PulseForgeEditor::ViewportTool::Select)
			{
				if (const auto Selected = m_Scene->FindEntity(*SelectedEntity()))
					if (IsEntityEffectivelyVisible(*Selected)) Geometry = BuildViewportGizmoGeometry(*Selected);
			}

			const std::optional<GizmoHandle> HoveredHandle =
				Geometry && m_ViewportImageHovered
					? PulseForgeEditor::FindViewportGizmoHandle(*Geometry, m_ViewportTools.GetOperation(), Mouse)
					: std::nullopt;
			if (Geometry)
			{
				const auto Selected = m_Scene->FindEntity(*SelectedEntity());
				if (Selected) DrawViewportGizmo(*Geometry, HoveredHandle);
			}

			const bool CameraGestureClicked =
				IO.KeyAlt && (IO.MouseClicked[ImGuiMouseButton_Left] || IO.MouseClicked[ImGuiMouseButton_Middle] ||
								 IO.MouseClicked[ImGuiMouseButton_Right]);
			if ((!ImageClicked && !CameraGestureClicked) || !m_ViewportImageHovered || m_ViewportMouseCapture.OwnsMouse()) return;
			if (HoveredHandle && Geometry)
			{
				(void)BeginGizmoDrag(*HoveredHandle, *Geometry, Mouse);
				return;
			}
			if (IO.KeyAlt)
			{
				if (IO.MouseClicked[ImGuiMouseButton_Left]) m_ViewportCameraGesture = ViewportCameraGesture::Orbit;
				else if (IO.MouseClicked[ImGuiMouseButton_Middle]) m_ViewportCameraGesture = ViewportCameraGesture::Pan;
				else if (IO.MouseClicked[ImGuiMouseButton_Right]) m_ViewportCameraGesture = ViewportCameraGesture::Dolly;
				if (m_ViewportCameraGesture != ViewportCameraGesture::None) return;
			}
			PickEntityAt(Mouse, IO.KeyCtrl, IO.KeyShift);
		}

		[[nodiscard]] std::optional<ViewportGizmoGeometry> BuildViewportGizmoGeometry(const PulseForge::Entity& Entity) const
		{
			if (!m_ViewportRenderView.IsViewProjectionValid()) return std::nullopt;
			const auto Transform = Entity.GetTransform();
			const auto World = Entity.GetWorldMatrix();
			const auto Parent = Entity.GetParent();
			if (!Transform || !World || !Parent || !PulseForgeEditor::Detail::IsFinite(*World)) return std::nullopt;

			glm::mat4 ParentWorld(1.0f);
			if (Parent->has_value())
			{
				const auto ParentTransform = Parent->value().GetWorldMatrix();
				if (!ParentTransform || !PulseForgeEditor::Detail::IsFinite(*ParentTransform)) return std::nullopt;
				ParentWorld = *ParentTransform;
			}

			std::vector<glm::vec3> SelectedRootPositions;
			const auto Roots = GetTopLevelSelectedEntities();
			const bool HasMultipleSelectedRoots = Roots && Roots->size() > 1;
			if (HasMultipleSelectedRoots)
			{
				SelectedRootPositions.reserve(Roots->size());
				for (const PulseForge::Entity& Root : *Roots)
					if (const auto RootWorld = Root.GetWorldMatrix(); RootWorld && PulseForgeEditor::Detail::IsFinite(*RootWorld))
						SelectedRootPositions.emplace_back((*RootWorld)[3]);
			}

			PulseForgeEditor::ViewportGizmoGeometryInput Input;
			Input.ViewProjection = m_ViewportRenderView.GetViewProjection();
			Input.CameraPosition = m_EditorCamera.GetPosition();
			Input.CameraForward = m_EditorCamera.GetForward();
			Input.ParentWorldTransform = ParentWorld;
			Input.LocalRotation = Transform->Rotation;
			Input.EntityWorldPosition = glm::vec3((*World)[3]);
			Input.SelectedRootWorldPositions = SelectedRootPositions;
			Input.ImageRect = { m_ViewportImageRect.Minimum, m_ViewportImageRect.Maximum };
			Input.LocalSpace = m_ViewportTools.GetValues().LocalSpace;
			Input.HasMultipleSelectedRoots = HasMultipleSelectedRoots;
			return PulseForgeEditor::BuildViewportGizmoGeometry(Input);
		}
		void DrawViewportGizmo(const ViewportGizmoGeometry& Geometry, std::optional<GizmoHandle> Hovered)
		{
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			DrawList->PushClipRect(ImVec2(m_ViewportImageRect.Minimum.x, m_ViewportImageRect.Minimum.y),
				ImVec2(m_ViewportImageRect.Maximum.x, m_ViewportImageRect.Maximum.y), true);
			const std::array<ImU32, 3> AxisColors{ ImGui::ColorConvertFloat4ToU32(
													   m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoAxisX)),
				ImGui::ColorConvertFloat4ToU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoAxisY)),
				ImGui::ColorConvertFloat4ToU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoAxisZ)) };
			const ImU32 HighlightColor =
				ImGui::ColorConvertFloat4ToU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::GizmoHighlight));
			if (m_ViewportTools.GetOperation() == PulseForgeEditor::TransformGizmoOperation::Rotate)
			{
				for (uint32_t Axis = 0; Axis < 3; ++Axis)
				{
					if (!Geometry.RotationRingValid[Axis]) continue;
					const bool IsHighlighted = Hovered && Hovered->Axis == Axis;
					const ImU32 Color = IsHighlighted ? HighlightColor : AxisColors[Axis];
					const auto& Ring = Geometry.RotationRings[Axis];
					for (size_t Point = 0; Point + 1 < Ring.size(); ++Point)
						DrawList->AddLine(ImVec2(Ring[Point].x, Ring[Point].y), ImVec2(Ring[Point + 1].x, Ring[Point + 1].y), Color,
							IsHighlighted ? 3.0f : 2.0f);
				}
			}
			else
			{
				for (uint32_t Axis = 0; Axis < 3; ++Axis)
				{
					if (!Geometry.Axes[Axis].IsValid) continue;
					const bool IsHighlighted = Hovered && Hovered->Axis == Axis;
					const ImU32 Color = IsHighlighted ? HighlightColor : AxisColors[Axis];
					const glm::vec2 End = Geometry.Axes[Axis].Endpoint;
					DrawList->AddLine(
						ImVec2(Geometry.PivotScreen.x, Geometry.PivotScreen.y), ImVec2(End.x, End.y), Color, IsHighlighted ? 4.0f : 3.0f);
					if (m_ViewportTools.GetOperation() == PulseForgeEditor::TransformGizmoOperation::Scale)
						DrawList->AddRectFilled(ImVec2(End.x - 5.0f, End.y - 5.0f), ImVec2(End.x + 5.0f, End.y + 5.0f), Color);
					else DrawList->AddCircleFilled(ImVec2(End.x, End.y), IsHighlighted ? 6.0f : 4.5f, Color);
				}
			}
			DrawList->AddCircleFilled(ImVec2(Geometry.PivotScreen.x, Geometry.PivotScreen.y), 5.0f,
				ImGui::ColorConvertFloat4ToU32(m_EditorStyle.GetColor(PulseForgeEditor::EditorColorToken::Text)));
			DrawList->PopClipRect();
		}

		bool BeginGizmoDrag(const GizmoHandle& Handle, const ViewportGizmoGeometry& Geometry, const glm::vec2& Mouse)
		{
			if (!m_Scene) return false;
			PulseForgeEditor::EditorViewportGizmoBeginInput Input;
			Input.Handle = Handle;
			Input.Geometry = Geometry;
			Input.MousePosition = Mouse;
			Input.ImageRect = m_ViewportImageRect;
			Input.ViewProjection = m_ViewportRenderView.GetViewProjection();
			Input.CameraPosition = m_EditorCamera.GetPosition();
			Input.ViewProjectionValid = m_ViewportRenderView.IsViewProjectionValid();
			Input.RuntimeActive = m_SceneRuntime != nullptr;
			Input.CameraNavigationActive = m_ViewportMouseCapture.OwnsMouse();
			Input.DuplicateOnDrag = ImGui::GetIO().KeyAlt;
			auto Started = m_ViewportGizmoInteraction.Begin(*m_Scene, m_SceneWorkspace, m_SceneEditController, Input,
				[this](std::string_view State) { return RestoreSceneState(State); });
			if (!Started)
			{
				SetError("Could not begin viewport gizmo interaction: " + Started.error());
				return false;
			}
			return *Started;
		}

		void UpdateGizmoDrag(const glm::vec2& Mouse)
		{
			PulseForgeEditor::EditorViewportGizmoUpdateInput Input;
			Input.MousePosition = Mouse;
			Input.ImageRect = m_ViewportImageRect;
			Input.ViewProjection = m_ViewportRenderView.GetViewProjection();
			Input.ToolValues = m_ViewportTools.GetValues();
			Input.RuntimeActive = m_SceneRuntime != nullptr;
			Input.ControlDown = ImGui::GetIO().KeyCtrl;
			Input.AltDown = ImGui::GetIO().KeyAlt;
			if (auto Updated = m_ViewportGizmoInteraction.Update(m_Scene.get(), m_SceneWorkspace, m_SceneEditController, Input,
					[this](std::string_view State) { return RestoreSceneState(State); });
				!Updated)
				SetError(Updated.error());
		}

		bool CancelGizmoInteraction()
		{
			if (auto Cancelled = m_ViewportGizmoInteraction.Cancel(m_Scene.get(), m_SceneWorkspace, m_SceneEditController,
					[this](std::string_view State) { return RestoreSceneState(State); });
				!Cancelled)
			{
				SetError("Scene edit rollback failed: " + Cancelled.error());
				return false;
			}
			return true;
		}

		void CompleteReleasedGizmoInteractionBeforeShortcuts()
		{
			if (!m_ViewportGizmoInteraction.IsActive()) return;

			const ImGuiIO& IO = ImGui::GetIO();
			const auto Outcome = PulseForgeEditor::GetViewportGizmoPointerOutcome(
				{ m_ViewportGizmoInteraction.IsActive(), IO.MouseDown[ImGuiMouseButton_Left], IO.MouseReleased[ImGuiMouseButton_Left] },
				ImGui::IsKeyPressed(ImGuiKey_Escape, false));
			if (Outcome != PulseForgeEditor::ViewportGizmoPointerOutcome::Complete) return;

			// The viewport image normally consumes release later in this frame; finish first so
			// the history shortcut router and Edit menu observe the committed gesture immediately.
			UpdateGizmoDrag({ IO.MousePos.x, IO.MousePos.y });
			if (m_ViewportGizmoInteraction.IsActive()) CompleteGizmoInteraction();
		}

		void CompleteGizmoInteraction()
		{
			if (auto Completed = m_ViewportGizmoInteraction.Complete(m_Scene.get(), m_SceneWorkspace, m_SceneEditController,
					[this](std::string_view State) { return RestoreSceneState(State); });
				!Completed)
				SetError(Completed.error());
		}
		[[nodiscard]] std::expected<const PulseForgeEditor::EditorMeshPickingGeometry*, std::string> GetCpuPickingMesh(
			const PulseForge::AssetID& Asset)
		{
			if (!m_Project) return std::unexpected("There is no open project for mesh picking");
			return m_MeshPickingCache.Get(*m_Project, Asset);
		}

		void PickEntityAt(const glm::vec2& Mouse, bool Control, bool Shift)
		{
			const auto ResolveMesh =
				[this](const PulseForge::AssetID& Asset) -> std::expected<const PulseForgeEditor::EditorMeshPickingGeometry*, std::string>
			{ return GetCpuPickingMesh(Asset); };
			auto Result = m_ViewportInteraction.PickEntityAt(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr,
				m_ViewportRenderView.IsViewProjectionValid(), m_ViewportImageRect, m_ViewportRenderView.GetViewProjection(),
				m_EditorCamera.GetPosition(), Mouse, Control, Shift, ResolveMesh);
			if (!Result)
			{
				SetError(Result.error());
				return;
			}
			for (std::string& Warning : *Result)
				SetWarning(std::move(Warning));
		}

		void SnapSelectionToSurface()
		{
			if (!m_Project) return;
			const PulseForgeEditor::EditorMeshGeometryResolver ResolveMesh = [this](const PulseForge::AssetID& Asset)
			{ return m_MeshPickingCache.Get(*m_Project, Asset); };
			m_SceneEntityCommands.SnapSelectionToSurface(m_Scene.get(), m_SceneWorkspace, m_SceneRuntime != nullptr, ResolveMesh, *this);
		}

		void FocusSelection()
		{
			if (!m_Scene || m_SceneRuntime) return;
			const auto ResolveMesh =
				[this](const PulseForge::AssetID& Asset) -> std::expected<const PulseForgeEditor::EditorMeshPickingGeometry*, std::string>
			{ return GetCpuPickingMesh(Asset); };
			auto Result = m_ViewportInteraction.FindSelectionFocus(*m_Scene, m_SceneWorkspace, ResolveMesh);
			if (!Result)
			{
				SetError(Result.error());
				return;
			}
			for (std::string& Warning : Result->Warnings)
				SetWarning(std::move(Warning));
			if (!Result->Focus) return;
			const float AspectRatio = m_ViewportRenderView.GetHeight() > 0 ? static_cast<float>(m_ViewportRenderView.GetWidth()) /
																				 static_cast<float>(m_ViewportRenderView.GetHeight())
																		   : 1.0f;
			m_EditorCamera.Focus(Result->Focus->Center, Result->Focus->Radius, AspectRatio);
			SetStatus("Focused the selection.");
		}

		std::expected<void, std::string> EnsureViewportTarget(const ImVec2& LogicalSize)
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			(void)LogicalSize;
			return std::unexpected("The OpenGL fallback does not support the engine scene viewport");
#else
			const ImVec2 FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
			if (!m_ImGuiRenderer) return std::unexpected("The Vulkan editor UI renderer is unavailable");
			auto Result = m_ViewportRenderView.EnsureTarget(
				LogicalSize.x, LogicalSize.y, FramebufferScale.x, FramebufferScale.y, PulseForge::Application::Get(), *m_ImGuiRenderer);
			if (!Result) return std::unexpected(Result.error());
			if (*Result) CancelGizmoInteraction();
			return {};
#endif
		}

		void PrepareViewportScene()
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			return;
#else
			std::vector<PulseForge::UUID> HiddenEntities;
			if (!m_SceneRuntime) HiddenEntities = GetEffectivelyHiddenEntities();
			m_ViewportRenderView.PrepareScene(PulseForge::Application::Get(), m_Project ? &*m_Project : nullptr, m_Scene.get(),
				m_RuntimeScene.get(), m_SceneRuntime != nullptr, m_EditorCamera, HiddenEntities, m_EditorShaderDirectory);
			if (m_ProjectOpenTimingActive && !m_LoggedFirstViewportPrepared && m_ViewportRenderView.IsSceneReady())
			{
				m_LoggedFirstViewportPrepared = true;
				const double Milliseconds =
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_ProjectOpenStartedAt).count();
				PF_INFO("Profile: Project open to first renderable viewport preparation took {:.2f} ms", Milliseconds);
			}
			if (m_ProjectOpenTimingActive && m_ViewportRenderView.IsSceneReady() && m_ViewportRenderView.HasPreparedEnvironmentLighting() &&
				!m_LoggedEnvironmentReady)
			{
				m_LoggedEnvironmentReady = true;
				const double Milliseconds =
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_ProjectOpenStartedAt).count();
				PF_INFO("Profile: Project open to HDR environment-ready viewport preparation took {:.2f} ms", Milliseconds);
			}
#endif
		}

		void RenderViewportScene()
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			return;
#else
			PulseForge::RenderTargetClearValue Clear;
			const auto Background = PulseForgeEditor::GetEditorColorValue(PulseForgeEditor::EditorColorToken::ViewportBackground);
			Clear.Color = { Background.Red, Background.Green, Background.Blue, Background.Alpha };
			if (m_ViewportRenderView.Render(PulseForge::Application::Get(),
					m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::SceneViewport), m_ViewportTools.GetValues().ExposureEV, Clear) &&
				m_ProjectOpenTimingActive && !m_LoggedFirstViewportRendered)
			{
				m_LoggedFirstViewportRendered = true;
				const double Milliseconds =
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_ProjectOpenStartedAt).count();
				PF_INFO("Profile: Project open to first rendered viewport frame took {:.2f} ms", Milliseconds);
				if (m_ProjectOpenRenderTestFile)
				{
					PF_INFO("PULSEFORGE_EDITOR_PROJECT_RENDER_SMOKE=PASS; requesting orderly shutdown");
					RequestEditorApplicationClose();
				}
			}
#endif
		}

		void ResetViewportSceneRenderer() noexcept
		{
			CancelGizmoInteraction();
			ResetEditorViewportCamera();
			m_ViewportRenderView.ResetScene();
			m_RendererAssetCachesPending = false;
			m_MaterialAssetBindingsPending = false;
			m_AssetRefreshDeferredForRuntime = false;
			m_ViewportTools.ResetExposure();
			m_MeshPickingCache.Clear();
		}
		bool BeginEditorCameraNavigation()
		{
#ifdef PF_EDITOR_RENDERER_OPENGL
			return false;
#else
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			return m_ViewportMouseCapture.Begin(
				Window, m_Context, true, IsCursorOverViewportImage(), m_ViewportGizmoInteraction.IsActive(), m_SceneRuntime != nullptr);
#endif
		}

		[[nodiscard]] bool IsCursorOverViewportImage() const
		{
			ImGui::SetCurrentContext(m_Context);
			const glm::vec2 Mouse{ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y };
			return PulseForgeEditor::Detail::IsFinite(Mouse) && Mouse.x >= m_ViewportImageRect.Minimum.x &&
				   Mouse.x < m_ViewportImageRect.Maximum.x && Mouse.y >= m_ViewportImageRect.Minimum.y &&
				   Mouse.y < m_ViewportImageRect.Maximum.y;
		}

		[[nodiscard]] bool IsNativeCursorOverViewportImage() const
		{
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			if (!Window) return false;
			double CursorX = 0.0;
			double CursorY = 0.0;
			int WindowX = 0;
			int WindowY = 0;
			glfwGetCursorPos(Window, &CursorX, &CursorY);
			glfwGetWindowPos(Window, &WindowX, &WindowY);
			const glm::vec2 Mouse{ static_cast<float>(WindowX + CursorX), static_cast<float>(WindowY + CursorY) };
			return PulseForgeEditor::Detail::IsFinite(Mouse) && Mouse.x >= m_ViewportImageRect.Minimum.x &&
				   Mouse.x < m_ViewportImageRect.Maximum.x && Mouse.y >= m_ViewportImageRect.Minimum.y &&
				   Mouse.y < m_ViewportImageRect.Maximum.y;
		}

		void EndEditorCameraNavigation(bool RestoreCursorPosition = true) noexcept
		{
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			m_ViewportMouseCapture.End(Window, m_Context, RestoreCursorPosition);
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
					m_ViewportKeyboardFocused, m_SceneRuntime ? PulseForgeEditor::ViewportKeyboardFocusEvent::PlayModeStarted
															  : PulseForgeEditor::ViewportKeyboardFocusEvent::ViewportUnavailable);
				return;
			}
			const PulseForge::Input& Input = PulseForge::Application::Get().GetInput();
			const bool RightMouseHeld = Input.IsMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT);
			if (m_ViewportMouseCapture.OwnsMouse() && !RightMouseHeld)
			{
				EndEditorCameraNavigation();
			}
			if (!m_Context) return;

			ImGui::SetCurrentContext(m_Context);
			const ImGuiIO& IO = ImGui::GetIO();
			auto* Window = static_cast<GLFWwindow*>(PulseForge::Application::Get().GetWindow().GetNativeWindow());
			const bool ApplicationFocused = Window && glfwGetWindowAttrib(Window, GLFW_FOCUSED) == GLFW_TRUE;
			const PulseForgeEditor::ViewportCameraNavigationContext Context{ .SceneAvailable = m_Scene != nullptr,
				.RuntimeActive = m_SceneRuntime != nullptr,
				.ViewportKeyboardFocused = m_ViewportKeyboardFocused,
				.RightMouseNavigationActive = m_ViewportMouseCapture.OwnsMouse() && RightMouseHeld,
				.ApplicationFocused = ApplicationFocused,
				.TextInputActive = IO.WantTextInput,
				.ActiveImGuiItem = ImGui::IsAnyItemActive(),
				.PopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup),
				.MenuNavigationActive = GImGui->NavLayer == ImGuiNavLayer_Menu || GImGui->NavWindowingTarget != nullptr,
				.ModifierHeld = IO.KeyCtrl || IO.KeyAlt || IO.KeySuper,
				.GizmoInteractionActive = m_ViewportGizmoInteraction.IsActive(),
				.CameraGestureActive = m_ViewportCameraGesture != ViewportCameraGesture::None };
			if (PulseForgeEditor::CanNavigateViewportCamera(Context))
			{
				const PulseForgeEditor::ViewportCameraMoveKeys Keys{ .Forward = Input.IsKeyPressed(GLFW_KEY_W),
					.Backward = Input.IsKeyPressed(GLFW_KEY_S),
					.Left = Input.IsKeyPressed(GLFW_KEY_A),
					.Right = Input.IsKeyPressed(GLFW_KEY_D),
					.Down = Input.IsKeyPressed(GLFW_KEY_Q),
					.Up = Input.IsKeyPressed(GLFW_KEY_E),
					.Fast = Input.IsKeyPressed(GLFW_KEY_LEFT_SHIFT) || Input.IsKeyPressed(GLFW_KEY_RIGHT_SHIFT) };
				m_EditorCamera.Move(Keys, DeltaTime.GetSeconds());
			}
		}

		void ResetViewportTarget() noexcept
		{
			CancelGizmoInteraction();
			m_ViewportImageRect = {};
			m_ViewportRenderView.ResetTarget();
		}

		void DrawConsolePanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Console)) return;
			if (m_DocumentHostTabReattachTest &&
				m_DocumentHostTabReattachInvalidDropIndex >= (DocumentTabTestDropPointsPerPanel * DocumentTabTestConsolePanelIndex) &&
				m_DocumentHostTabReattachInvalidDropIndex <
					(DocumentTabTestDropPointsPerPanel * DocumentTabTestInvalidDropPanelNames.size()))
				ImGui::SetNextWindowFocus();
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Console)) m_ConsolePanel.DrawContents(m_EditorStyle);
			ImGui::End();
		}
		void DrawHierarchyPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Hierarchy)) return;
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Hierarchy))
				m_HierarchyPanel.DrawContents(m_Scene.get(), m_SceneWorkspace, m_EditorStyle, *this);
			ImGui::End();
		}
		void CreateEntityFromEditor() { m_SceneEntityCommands.CreateEntity(m_Scene.get(), *this); }

		[[nodiscard]] std::expected<std::vector<PulseForge::UUID>, PulseForge::SceneError> DuplicateEntityRootsRaw(
			const std::vector<PulseForge::Entity>& Roots)
		{
			if (!m_Scene)
				return std::unexpected(PulseForge::SceneError{ PulseForge::SceneErrorCode::InvalidEntity, "There is no active scene" });
			return PulseForgeEditor::EditorSceneHierarchyOperations::DuplicateSubtrees(*m_Scene, Roots);
		}

		void DuplicateSelectedEntity() { m_SceneEntityCommands.DuplicateSelectedEntities(m_Scene.get(), m_SceneWorkspace, *this); }

		void DeleteSelectedEntity() { m_SceneEntityCommands.DeleteSelectedEntities(m_Scene.get(), m_SceneWorkspace, *this); }

		[[nodiscard]] bool CopySelectedEntities()
		{
			if (!m_Project) return false;
			return m_SceneEntityCommands.CopySelectedEntities(
				m_Scene.get(), m_Project->GetDescription().ID, m_SceneAsset, m_SceneWorkspace, *this);
		}

		void CutSelectedEntities()
		{
			if (!m_Project) return;
			m_SceneEntityCommands.CutSelectedEntities(m_Scene.get(), m_Project->GetDescription().ID, m_SceneAsset, m_SceneWorkspace, *this);
		}

		void PasteEntities()
		{
			if (!m_Project) return;
			m_SceneEntityCommands.PasteEntities(
				m_Scene.get(), m_Project->GetDescription().ID, m_SceneAsset, m_SceneWorkspace, m_SceneRuntime != nullptr,
				[this](const PulseForge::UUID& Identifier) { return m_Project->GetAssetRegistry().Find(Identifier).has_value(); }, *this);
		}

		void ReparentEntity(const PulseForge::UUID& ChildIdentifier, std::optional<PulseForge::UUID> ParentIdentifier)
		{
			m_SceneEntityCommands.ReparentEntity(m_Scene.get(), ChildIdentifier, ParentIdentifier, *this);
		}

		void DrawInspectorPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::Inspector)) return;
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::Inspector))
			{
				const bool FinishCurrentEdit = PulseForgeEditor::ShouldFinishSceneEditForInspector(
					m_SceneEditController.IsActive(), m_SceneEditController.InspectorHistoryItem(), GImGui ? GImGui->ActiveId : 0);
				m_InspectorPanel.DrawContents(m_Scene.get(), m_SceneWorkspace, m_EditorStyle, m_AssetSelector, m_ContentBrowser.Assets(),
					m_Project.has_value(), FinishCurrentEdit, *this);
			}
			ImGui::End();
		}

		void ResetContentBrowserNavigation() { m_ContentBrowser.ResetNavigation(); }

		void ShowMaterialDetails(const PulseForge::AssetID& Identifier)
		{
			if (!m_Project) return;

			m_MaterialDetailsAsset = Identifier;
			MarkDocumentWorkspaceDirty();
			m_ContentBrowser.SelectedAsset() = Identifier;
			SetEditorPanelVisible(PulseForgeEditor::EditorPanel::MaterialDetails, true);
		}

		void ClearMaterialDraftsForProject()
		{
			if (!m_Project) return;
			m_MaterialDrafts.ClearProject(*m_Project);
		}

		void FinalizePendingMaterialEdits() { m_MaterialDrafts.FinalizePendingEdits(ImGui::IsAnyItemActive()); }

		bool ApplyMaterialHistoryStep(bool Redo)
		{
			const auto Asset = GetFocusedMaterialAsset();
			return Asset && ApplyMaterialHistoryStep(*Asset, Redo);
		}

		bool ApplyMaterialHistoryStep(const PulseForge::AssetID& Asset, bool Redo)
		{
			if (!m_Project || !m_MaterialDrafts.ContainsDraft(*m_Project, Asset)) return false;
			const auto Changed = m_MaterialDrafts.ApplyHistoryStep(*m_Project, Asset, Redo, m_Documents);
			if (!Changed) return false;
			if (!*Changed) return true;
			SetStatus(Redo ? "Redid material edit." : "Undid material edit.");
			return true;
		}

		void ForgetMaterialDraft(const PulseForge::AssetID& Asset)
		{
			if (m_Project) m_MaterialDrafts.Forget(*m_Project, Asset);
		}
		void DrawMaterialDetailsPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::MaterialDetails)) return;
			const bool FocusMaterialDetails = m_FocusMaterialDetailsNextFrame;
			if (FocusMaterialDetails) ImGui::SetNextWindowFocus();
			const bool ContentsVisible =
				BeginEditorPanel(PulseForgeEditor::EditorPanel::MaterialDetails, "Material Details###Material Editor");
			if (FocusMaterialDetails)
			{
				const bool Activated = m_Layout.ActivatePanel(PulseForgeEditor::EditorPanel::MaterialDetails);
				const ImGuiWindow* Window = ImGui::GetCurrentWindow();
				const ImGuiDockNode* Node = Window->DockNode;
				const bool IsActive = Node && Node->TabBar ? Node->SelectedTabId == Window->TabId
														   : ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				m_FocusMaterialDetailsNextFrame = !Activated || !IsActive;
			}
			if (ContentsVisible)
				m_MaterialDetailsPanel.DrawContents(m_Project ? &*m_Project : nullptr, m_MaterialDetailsAsset, m_ContentBrowser.Assets(),
					m_MaterialDrafts, m_AssetSelector, m_EditorStyle, *this);
			ImGui::End();
		}
		void DrawContentBrowserPanel()
		{
			if (!m_Layout.IsPanelVisible(PulseForgeEditor::EditorPanel::ContentBrowser)) return;
			// The default layout stacks Content Browser and Console; select each while its drop targets are tested.
			if (m_DocumentHostTabReattachTest &&
				m_DocumentHostTabReattachInvalidDropIndex < (DocumentTabTestDropPointsPerPanel * DocumentTabTestConsolePanelIndex))
				ImGui::SetNextWindowFocus();
			if (BeginEditorPanel(PulseForgeEditor::EditorPanel::ContentBrowser))
			{
				if (!m_Project)
					m_EditorStyle.EmptyState(PulseForgeEditor::EditorIcon::FolderOpen, "Open a project to browse managed assets.");
				else
					m_ContentBrowserPanel.DrawContents(
						m_ContentBrowser, *m_Project, m_Scene != nullptr, m_AssetPreviews, m_EditorStyle, *this);
			}
			ImGui::End();
		}
		void SetStatus(std::string Message) { m_StatusPanel.SetStatus(std::move(Message)); }

		void StartRuntime()
		{
			if (!m_Project || !m_Scene)
			{
				SetError("Open a project and scene before starting the runtime.");
				return;
			}
			if (!CancelGizmoInteraction()) return;
			EndEditorCameraNavigation();
			m_ViewportKeyboardFocused = PulseForgeEditor::ResolveViewportKeyboardFocus(
				m_ViewportKeyboardFocused, PulseForgeEditor::ViewportKeyboardFocusEvent::PlayModeStarted);

			auto CandidateScene = PulseForge::SceneSerializer::Clone(*m_Scene);
			if (!CandidateScene)
			{
				SetError("Could not copy the authored scene for runtime: " + CandidateScene.error().Message);
				return;
			}

			PulseForge::SceneRuntimeServices Services;
			Services.InputState = &PulseForge::Application::Get().GetInput();
			auto CandidateRuntime = std::make_unique<PulseForge::SceneRuntime>(*m_Project, PulseForge::SceneRuntimeDesc{}, Services);
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
			if (m_SceneRuntime) m_SceneRuntime->Stop();
			m_SceneRuntime.reset();
			m_RuntimeScene.reset();
		}

		void RefreshAssets()
		{
			if (!m_Project) return;
			if (m_SceneRuntime)
			{
				SetWarning("Stop Play mode before refreshing project assets.");
				return;
			}

			const PulseForge::UUID ProjectID = m_Project->GetDescription().ID;
			m_DeferredEditorOperations.Enqueue(
				[this, ProjectID]
				{
					if (!m_Project || m_Project->GetDescription().ID != ProjectID) return;
					if (m_SceneRuntime)
					{
						SetWarning("Stop Play mode before refreshing project assets.");
						return;
					}

					const auto Rebuild = m_Project->GetAssetRegistry().Rebuild(m_Project->GetRootPath());
					if (!Rebuild)
					{
						std::string Message = "Asset registry refresh failed:";
						if (Rebuild.error().Issues.empty()) Message += " unknown registry error";
						for (const PulseForge::AssetRegistryIssue& Issue : Rebuild.error().Issues)
							Message += "\n - " + Issue.Message;
						SetError(std::move(Message));
						return;
					}

					UpdateAssetList(true);
					SetStatus("Asset registry refreshed.");
				});
		}

		void UpdateAssetList(bool RendererAssetsChanged = false)
		{
			m_AssetPreviewProjectPending = true;
			m_AssetPreviewAssetsPending = true;
			m_RendererAssetCachesPending = m_RendererAssetCachesPending || RendererAssetsChanged;
			if (!RendererAssetsChanged) m_MeshPickingCache.Clear();
			m_ContentBrowser.SetAssets(m_Project ? m_Project->GetAssetRegistry().GetAssets() : std::vector<PulseForge::AssetRecord>{});
			SynchronizeOpenDocumentAssetLocations();
		}

		void SynchronizeOpenDocumentAssetLocations()
		{
			if (!m_Project) return;

			const PulseForge::UUID ProjectID = m_Project->GetDescription().ID;
			for (const auto& [Identity, ExistingSession] : m_Documents.GetSessions())
			{
				(void)ExistingSession;
				if (Identity.Project != ProjectID || Identity.Kind == PulseForgeEditor::EditorDocumentKind::Level) continue;

				const auto Asset = m_Project->GetAssetRegistry().Find(Identity.Asset);
				if (!Asset) continue;

				const std::string DisplayName = PathToUtf8(Asset->ProjectRelativePath.filename());
				auto* Session = m_Documents.Find(Identity);
				if (!Session || (Session->ProjectRelativePath == Asset->ProjectRelativePath && Session->DisplayName == DisplayName))
					continue;
				if (m_DocumentEditors.IsRegistered(Identity.Kind) && Session->EditorState)
				{
					if (!m_DocumentEditors.UpdateAssetLocation(*Session, Asset->ProjectRelativePath, DisplayName))
					{
						SetWarning(
							"The document editor rejected its moved asset location; the session now follows the registered asset path.");
						(void)m_Documents.UpdateAssetLocation(Identity, Asset->ProjectRelativePath, DisplayName);
					}
				}
				else (void)m_Documents.UpdateAssetLocation(Identity, Asset->ProjectRelativePath, DisplayName);
			}
		}

		void SetWarning(std::string Message) { m_StatusPanel.SetWarning(std::move(Message)); }

		void SetError(std::string Message) { m_StatusPanel.SetError(std::move(Message)); }

		void ClearScene() noexcept
		{
			CancelGizmoInteraction();
			ResetEditorViewportCamera();
			StopRuntime();
			m_ViewportRenderView.InvalidatePreparedScene();
			m_Scene.reset();
			m_SceneAsset.reset();
			SelectSingleEntity(std::nullopt);
			m_ContentBrowser.SelectedAsset().reset();
			m_AssetWorkflow.DismissDelete();
			ResetSceneHistory();
		}

		void ShutdownImGui() noexcept
		{
			if (!m_Context) return;

			ImGui::SetCurrentContext(m_Context);
			m_ViewportRenderView.ResetTarget();
			if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ImGui::DestroyPlatformWindows();
			if (m_Project)
			{
				m_DocumentWorkspaceStore.MarkDirty(false);
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
			m_AssetPreviewStress.WriteReport(m_AssetPreviews, m_ImGuiRenderer ? m_ImGuiRenderer->GetRegisteredTextureCount() : 0);
			if (m_ImGuiRenderer)
			{
				m_ImGuiRenderer->Shutdown();
				m_ImGuiRenderer.reset();
			}
			if (auto Result = m_Layout.SaveNow(); !Result) ReportLayoutPersistenceError(Result.error());
			else m_LayoutPersistenceError.clear();
#ifdef PF_EDITOR_RENDERER_OPENGL
			if (m_OpenGLBackendActive) ImGui_ImplOpenGL3_Shutdown();
#endif
			if (m_GlfwBackendActive) ImGui_ImplGlfw_Shutdown();
			ImGui::GetIO().UserData = m_PreviousImGuiUserData;
			m_EditorStyle.Reset();
			ImGui::DestroyContext(m_Context);
			m_Context = nullptr;
			m_OpenGLBackendActive = false;
			m_GlfwBackendActive = false;
		}

		void ShutdownFileDialog() noexcept
		{
			if (m_FileDialogActive) PulseForgeEditor::EditorPlatformServices::ShutdownNativeFileDialogs();
			m_FileDialogActive = false;
		}

		ImGuiContext* m_Context = nullptr;
		void* m_PreviousImGuiUserData = nullptr;
		PulseForgeEditor::EditorLayout m_Layout;
		PulseForgeEditor::EditorDocumentManager m_Documents;
		PulseForgeEditor::EditorDocumentWorkspaceStore m_DocumentWorkspaceStore;
		PulseForgeEditor::EditorDocumentEditorRegistry m_DocumentEditors;
		PulseForgeEditor::EditorWorkspacePresentation m_WorkspacePresentation;
		PulseForgeEditor::EditorDeferredOperationQueue m_DeferredEditorOperations;
		PulseForgeEditor::EditorStyle m_EditorStyle;
		std::unique_ptr<PulseForgeEditor::EditorImGuiRenderer> m_ImGuiRenderer;
		PulseForgeEditor::EditorAssetPreviewService m_AssetPreviews;
		PulseForgeEditor::EditorAssetPreviewStressHarness m_AssetPreviewStress;
		bool m_AssetPreviewProjectPending = false;
		bool m_AssetPreviewAssetsPending = false;
		bool m_RendererAssetCachesPending = false;
		bool m_MaterialAssetBindingsPending = false;
		bool m_AssetRefreshDeferredForRuntime = false;
		bool m_GlfwBackendActive = false;
		bool m_OpenGLBackendActive = false;
		std::filesystem::path m_EditorShaderDirectory;
#ifdef PF_EDITOR_RENDERER_OPENGL
		std::filesystem::path m_OpenGLDiagnosticCapturePath;
		bool m_OpenGLDiagnosticCaptureWritten = false;
		bool m_OpenGLDiagnosticPlatformCaptureWritten = false;
		bool m_OpenGLDiagnosticContentBrowserSelected = false;
		uint32_t m_OpenGLDiagnosticFramesSeen = 0;
#endif
		bool m_FileDialogActive = false;
		bool m_WindowPositionRestoreWarningLogged = false;
		bool m_ViewportImageHovered = false;
		bool m_ProjectOpenTimingActive = false;
		bool m_LoggedFirstViewportPrepared = false;
		bool m_LoggedFirstViewportRendered = false;
		bool m_LoggedEnvironmentReady = false;
		bool m_ViewportKeyboardFocused = false;
		PulseForgeEditor::Detail::EditorViewportMouseCapture m_ViewportMouseCapture;
		PulseForgeEditor::EditorSceneEditController m_SceneEditController;
		PulseForgeEditor::EditorSceneWorkspaceState m_SceneWorkspace;
		PulseForgeEditor::EditorSceneVisibilityCommands m_SceneVisibilityCommands;
		bool m_OpenUnsavedDialog = false;
		bool m_ApplicationClosePending = false;
		bool m_ApplicationShutdownRequested = false;
		PulseForgeEditor::EditorDocumentCloseRequest m_DocumentCloseRequest;
		PulseForgeEditor::EditorAssetWorkflowState m_AssetWorkflow;
		PulseForgeEditor::EditorConsolePanel m_ConsolePanel;
		PulseForgeEditor::EditorStatusPanel m_StatusPanel;
		PulseForgeEditor::EditorHierarchyPanel m_HierarchyPanel;
		PulseForgeEditor::EditorMainMenuPanel m_MainMenuPanel;
		PulseForgeEditor::EditorInspectorPanel m_InspectorPanel;
		PulseForgeEditor::EditorAssetSelector m_AssetSelector;
		PulseForgeEditor::EditorContentBrowserPanel m_ContentBrowserPanel;
		PulseForgeEditor::EditorMaterialDetailsPanel m_MaterialDetailsPanel;
		PulseForgeEditor::EditorContentBrowserState m_ContentBrowser;
		bool m_InitialMaterialDetailsDockPending = false;
		bool m_ForceMaterialDetailsDock = false;
		bool m_FocusMaterialDetailsNextFrame = false;
		PulseForgeEditor::EditorSceneTransitionQueue m_SceneTransitionQueue;
		std::optional<PulseForgeEditor::EditorDocumentIdentity> m_LevelDocumentIdentity;
		std::optional<PulseForge::AssetID> m_MaterialDetailsAsset;
		std::optional<PulseForgeEditor::EditorDocumentIdentity> m_PendingDocumentTabClose;
		PulseForgeEditor::EditorDocumentTabInteraction m_DocumentTabInteraction;
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
		std::unique_ptr<PulseForgeEditor::Detail::EditorDocumentHostTestEditor> m_DocumentHostTestEditor;
		DocumentTabTearOffTestPhase m_DocumentHostTabTearOffTestPhase = DocumentTabTearOffTestPhase::Disabled;
		PulseForgeEditor::EditorDocumentIdentity m_DocumentHostTabTearOffTestIdentity;
		ImVec2 m_DocumentHostTabTearOffTestTabCenter{ 0.0f, 0.0f };
		ImVec2 m_DocumentHostTabTearOffTestStart{ 0.0f, 0.0f };
		ImVec2 m_DocumentHostTabTearOffTestDrop{ 0.0f, 0.0f };
		ImGuiID m_DocumentHostTabTearOffTestTabViewportID = 0;
		std::string m_DocumentHostTabTearOffSourceHostID;
		bool m_DocumentHostTabTearOffTestTabCenterValid = false;
		bool m_DocumentHostTabTearOffPointerPrepared = false;
		DocumentTabReattachTestPhase m_DocumentHostTabReattachTestPhase = DocumentTabReattachTestPhase::Disabled;
		DocumentTabReorderTestPhase m_DocumentHostTabReorderTestPhase = DocumentTabReorderTestPhase::Disabled;
		DocumentTabGroupTestPhase m_DocumentHostTabGroupTestPhase = DocumentTabGroupTestPhase::Disabled;
		DocumentWorkspaceSwitchTestPhase m_DocumentWorkspaceSwitchTestPhase = DocumentWorkspaceSwitchTestPhase::Disabled;
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
		PulseForgeEditor::EditorViewportSceneRenderView m_ViewportRenderView;
		PulseForgeEditor::EditorSceneViewportInteraction m_ViewportInteraction;
		std::unique_ptr<PulseForge::Scene> m_Scene;
		std::unique_ptr<PulseForge::Scene> m_RuntimeScene;
		std::unique_ptr<PulseForge::SceneRuntime> m_SceneRuntime;
		PulseForgeEditor::ViewportImageRect m_ViewportImageRect;
		PulseForgeEditor::EditorViewportGizmoInteraction m_ViewportGizmoInteraction;
		PulseForgeEditor::EditorViewportToolState m_ViewportTools;
		PulseForgeEditor::EditorViewportToolbar m_ViewportToolbar;
		ViewportCameraGesture m_ViewportCameraGesture = ViewportCameraGesture::None;
		bool m_ShowEditorOverlays = true;
		PulseForgeEditor::EditorMeshPickingCache m_MeshPickingCache;
		PulseForgeEditor::EditorViewportCamera m_EditorCamera;
		std::optional<PulseForge::AssetID> m_SceneAsset;
		PulseForgeEditor::EditorSceneEntityCommands m_SceneEntityCommands;
		PulseForgeEditor::EditorMaterialDraftController m_MaterialDrafts;
		std::string m_WindowTitle;
		std::string m_ImGuiRenderingError;
		std::string m_LayoutPersistenceError;
		std::unordered_set<std::string> m_RestoredHostBoundsPending;
		std::chrono::steady_clock::time_point m_ProjectOpenStartedAt{};
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
} // namespace

std::unique_ptr<PulseForge::Application> PulseForge::CreateApplication()
{
	return PulseForgeEditor::CreateEditorApplication({});
}

std::unique_ptr<PulseForge::Application> PulseForgeEditor::CreateEditorApplication(EditorCommandLineOptions Options)
{
	return std::make_unique<PulseForgeEditorApplication>(std::move(Options));
}
