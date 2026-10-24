#pragma once

#include "Input/GameplayInput.h"
#include "Scene/Components/TransformComponent.h"
#include "Scene/UUID.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace PulseForgeEditor
{
	namespace Detail
	{
		bool IsFinite(const glm::vec2& Value);
		[[nodiscard]] bool IsFinite(const glm::vec3& Value);
		bool IsFinite(const glm::mat4& Value);
		bool IsFinite(const glm::quat& Value);
	} // namespace Detail

	struct ViewportImageRect
	{
		glm::vec2 Minimum{ 0.0f };
		glm::vec2 Maximum{ 0.0f };
	};

	struct Ray
	{
		glm::vec3 Origin{ 0.0f };
		glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
		float MaxDistance = std::numeric_limits<float>::infinity();
	};

	struct PickableMesh
	{
		PulseForge::UUID Entity;
		glm::mat4 WorldTransform{ 1.0f };
		std::span<const glm::vec3> Positions;
		std::span<const uint32_t> Indices;
	};

	struct PickHit
	{
		PulseForge::UUID Entity;
		float Distance = 0.0f;
	};

	struct SupportingSurfaceHit
	{
		PulseForge::UUID Entity;
		float Distance = 0.0f;
		glm::vec3 Position{ 0.0f };
		glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
	};

	struct WorldGizmoAxis
	{
		glm::vec3 Direction{ 0.0f };
		float WorldUnitsPerLocalUnit = 0.0f;
	};

	struct ViewportOrientationAxis
	{
		glm::vec2 ScreenDirection{ 0.0f };
		float Depth = 0.0f;
	};

	[[nodiscard]] std::optional<float> SnapValueToIncrement(float Value, float Increment);

	enum class TransformGizmoOperation : uint8_t
	{
		Translate,
		Rotate,
		Scale
	};

	enum class ViewportTool : uint8_t
	{
		Select,
		Move,
		Rotate,
		Scale
	};

	struct ViewportShortcutContext
	{
		bool SceneAvailable = false;
		bool RuntimeActive = false;
		bool CameraNavigationActive = false;
		bool ViewportKeyboardFocused = false;
		bool RightMouseHeld = false;
		bool TextInputActive = false;
		bool ActiveImGuiItem = false;
		bool PopupOpen = false;
		bool MenuNavigationActive = false;
		bool ModifierHeld = false;
	};

	[[nodiscard]] constexpr bool CanHandleViewportToolShortcuts(const ViewportShortcutContext& Context) noexcept
	{
		return Context.SceneAvailable && !Context.RuntimeActive && !Context.CameraNavigationActive && !Context.ViewportKeyboardFocused &&
			   !Context.RightMouseHeld && !Context.TextInputActive && !Context.ActiveImGuiItem && !Context.PopupOpen &&
			   !Context.MenuNavigationActive && !Context.ModifierHeld;
	}

	[[nodiscard]] constexpr bool CanHandleFocusedViewportToolShortcuts(const ViewportShortcutContext& Context) noexcept
	{
		return Context.SceneAvailable && !Context.RuntimeActive && !Context.CameraNavigationActive && Context.ViewportKeyboardFocused &&
			   !Context.RightMouseHeld && !Context.TextInputActive && !Context.ActiveImGuiItem && !Context.PopupOpen &&
			   !Context.MenuNavigationActive && !Context.ModifierHeld;
	}

	[[nodiscard]] constexpr std::optional<ViewportTool> GetViewportToolForShortcut(char Key) noexcept
	{
		switch (Key)
		{
		case 'q':
		case 'Q':
			return ViewportTool::Select;
		case 'w':
		case 'W':
			return ViewportTool::Move;
		case 'e':
		case 'E':
			return ViewportTool::Rotate;
		case 'r':
		case 'R':
			return ViewportTool::Scale;
		default:
			return std::nullopt;
		}
	}

	[[nodiscard]] constexpr std::optional<ViewportTool> GetFocusedViewportToolForShortcut(char Key) noexcept
	{
		switch (Key)
		{
		case '1':
			return ViewportTool::Select;
		case '2':
			return ViewportTool::Move;
		case '3':
			return ViewportTool::Rotate;
		case '4':
			return ViewportTool::Scale;
		default:
			return std::nullopt;
		}
	}

	enum class ViewportKeyboardFocusEvent : uint8_t
	{
		NoChange,
		RenderedImageClicked,
		OtherEditorPanelClicked,
		ApplicationFocusLost,
		ViewportUnavailable,
		PlayModeStarted
	};

	[[nodiscard]] constexpr bool ResolveViewportKeyboardFocus(bool CurrentlyFocused, ViewportKeyboardFocusEvent Event) noexcept
	{
		switch (Event)
		{
		case ViewportKeyboardFocusEvent::RenderedImageClicked:
			return true;
		case ViewportKeyboardFocusEvent::OtherEditorPanelClicked:
		case ViewportKeyboardFocusEvent::ApplicationFocusLost:
		case ViewportKeyboardFocusEvent::ViewportUnavailable:
		case ViewportKeyboardFocusEvent::PlayModeStarted:
			return false;
		case ViewportKeyboardFocusEvent::NoChange:
			return CurrentlyFocused;
		default:
			return false;
		}
	}

	// Play mode disables editor-camera navigation, but ordinary runtime updates preserve viewport ownership.
	[[nodiscard]] constexpr bool MaintainViewportKeyboardFocusDuringUpdate(
		bool CurrentlyFocused, bool SceneAvailable, bool RuntimeActive) noexcept
	{
		return SceneAvailable || RuntimeActive ? CurrentlyFocused : false;
	}

	struct EditorGameplayInputRoutingRequest
	{
		PulseForge::InputWindowID ViewportWindow = 0;
		bool ViewportAvailable = false;
		bool NativeOwnerFocused = false;
		bool ImGuiOwnerFocused = false;
		bool RuntimeActive = false;
		bool ViewportKeyboardFocused = false;
		bool TextOrModalInput = false;
		bool ViewportNavigation = false;
	};

	[[nodiscard]] constexpr PulseForge::InputRoutingState BuildEditorGameplayInputRouting(
		const EditorGameplayInputRoutingRequest& Request) noexcept
	{
		PulseForge::InputRoutingState Routing;
		if (Request.ViewportWindow == 0 || !Request.NativeOwnerFocused || !Request.ImGuiOwnerFocused) return Routing;

		Routing.FocusedWindow = Request.ViewportWindow;
		const auto AddContext = [&Routing](PulseForge::InputContext Context, PulseForge::InputDeviceMask Consumes, bool Active)
		{
			Routing.Contexts[Routing.ContextCount++] = {
				.Context = Context, .Consumes = Consumes, .OwnerWindow = Routing.FocusedWindow, .Active = Active
			};
		};
		const auto KeyboardAndMouse = PulseForge::InputDeviceMask::Keyboard | PulseForge::InputDeviceMask::Mouse;
		const bool GameplayActive = Request.RuntimeActive && Request.ViewportAvailable && Request.ViewportKeyboardFocused &&
									!Request.TextOrModalInput && !Request.ViewportNavigation;
		const bool ShortcutsActive = !Request.TextOrModalInput && !Request.ViewportNavigation;
		AddContext(PulseForge::InputContext::Gameplay, PulseForge::InputDeviceMask::None, GameplayActive);
		AddContext(PulseForge::InputContext::EditorShortcuts, PulseForge::InputDeviceMask::None, ShortcutsActive);
		AddContext(PulseForge::InputContext::ViewportNavigation, KeyboardAndMouse, Request.ViewportNavigation);
		AddContext(PulseForge::InputContext::TextEntry, PulseForge::InputDeviceMask::All, Request.TextOrModalInput);
		return Routing;
	}

	struct ViewportCameraNavigationContext
	{
		bool SceneAvailable = false;
		bool RuntimeActive = false;
		bool ViewportKeyboardFocused = false;
		bool RightMouseNavigationActive = false;
		bool ApplicationFocused = false;
		bool TextInputActive = false;
		bool ActiveImGuiItem = false;
		bool PopupOpen = false;
		bool MenuNavigationActive = false;
		bool ModifierHeld = false;
		bool GizmoInteractionActive = false;
		bool CameraGestureActive = false;
	};

	[[nodiscard]] constexpr bool CanNavigateViewportCamera(const ViewportCameraNavigationContext& Context) noexcept
	{
		return Context.SceneAvailable && !Context.RuntimeActive && Context.ApplicationFocused &&
			   (Context.ViewportKeyboardFocused || Context.RightMouseNavigationActive) && !Context.TextInputActive &&
			   !Context.ActiveImGuiItem && !Context.PopupOpen && !Context.MenuNavigationActive && !Context.ModifierHeld &&
			   !Context.GizmoInteractionActive && !Context.CameraGestureActive;
	}

	struct ViewportCameraMoveKeys
	{
		bool Forward = false;
		bool Backward = false;
		bool Left = false;
		bool Right = false;
		bool Down = false;
		bool Up = false;
		bool Fast = false;
	};

	[[nodiscard]] glm::vec3 CalculateViewportCameraMoveOffset(const glm::vec3& Forward, const glm::vec3& Right,
		const ViewportCameraMoveKeys& Keys, float MoveSpeed, double DeltaSeconds) noexcept;

	struct ViewportCameraDollyResult
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 FocusPivot{ 0.0f };
	};

	[[nodiscard]] std::optional<ViewportCameraDollyResult> CalculateViewportCameraDolly(const glm::vec3& Position, const glm::vec3& Forward,
		const std::optional<glm::vec3>& ExistingFocusPivot, float MoveSpeed, float Delta) noexcept;

	[[nodiscard]] float GetViewportWheelDollyDelta(float WheelDelta) noexcept;

	enum class ViewportWheelAction : uint8_t
	{
		None,
		ScrollPanel,
		AdjustFlySpeed,
		Zoom
	};

	struct ViewportWheelContext
	{
		bool PointerOverViewportImage = false;
		bool RightMouseNavigationActive = false;
		bool RightMouseHeld = false;
		bool RuntimeActive = false;
		bool PopupOpen = false;
	};

	[[nodiscard]] constexpr ViewportWheelAction ResolveViewportWheelAction(const ViewportWheelContext& Context) noexcept
	{
		if (Context.PopupOpen) return ViewportWheelAction::None;
		if (Context.RightMouseNavigationActive && Context.RightMouseHeld && !Context.RuntimeActive)
			return ViewportWheelAction::AdjustFlySpeed;
		if (Context.PointerOverViewportImage && !Context.RuntimeActive && !Context.RightMouseHeld) return ViewportWheelAction::Zoom;
		return Context.PointerOverViewportImage ? ViewportWheelAction::None : ViewportWheelAction::ScrollPanel;
	}

	[[nodiscard]] std::optional<std::array<ViewportOrientationAxis, 3>> ProjectViewportOrientationAxes(const glm::vec3& CameraForward);

	// NVRHI's Vulkan backend uses a negative-height viewport, so NDC +Y maps to the top of the image.
	[[nodiscard]] std::optional<glm::vec2> ViewportImageToNdc(const glm::vec2& ImagePosition, const ViewportImageRect& ImageRect);

	[[nodiscard]] std::optional<glm::vec2> NdcToViewportImage(const glm::vec2& Ndc, const ViewportImageRect& ImageRect);

	[[nodiscard]] std::optional<glm::vec2> ProjectWorldToViewportImage(
		const glm::vec3& WorldPosition, const glm::mat4& ViewProjection, const ViewportImageRect& ImageRect);

	[[nodiscard]] std::optional<Ray> MakeViewportRay(const glm::vec2& ScreenPosition, const ViewportImageRect& ImageRect,
		const glm::mat4& ViewProjection, bool AllowOutsideImage = false);

	[[nodiscard]] std::optional<glm::vec3> IntersectRayPlane(
		const Ray& InputRay, const glm::vec3& PointOnPlane, const glm::vec3& PlaneNormal);

	// Mesh positions and indices are borrowed for this synchronous query. The direction is normalized internally.
	[[nodiscard]] std::optional<PickHit> RaycastMeshes(const Ray& WorldRay, std::span<const PickableMesh> Meshes);

	[[nodiscard]] std::optional<SupportingSurfaceHit> RaycastSupportingSurface(
		const Ray& WorldRay, std::span<const PickableMesh> Meshes, float MinimumUpDot = 0.5f);

	[[nodiscard]] std::optional<WorldGizmoAxis> GetWorldGizmoAxis(
		const glm::mat4& ParentWorldTransform, const glm::quat& LocalRotation, uint32_t AxisIndex);

	[[nodiscard]] std::optional<WorldGizmoAxis> GetGizmoAxis(
		const glm::mat4& ParentWorldTransform, const glm::quat& LocalRotation, uint32_t AxisIndex, bool LocalSpace);

	[[nodiscard]] std::optional<float> WorldDeltaToLocalAxisDelta(
		float WorldDelta, const glm::mat4& ParentWorldTransform, const glm::quat& LocalRotation, uint32_t AxisIndex);

	[[nodiscard]] std::optional<PulseForge::TransformComponent> ApplyLocalGizmoDelta(
		const PulseForge::TransformComponent& InitialTransform, TransformGizmoOperation Operation, uint32_t AxisIndex, float Delta);
} // namespace PulseForgeEditor
