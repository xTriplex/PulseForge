#pragma once

#include "Scene/Components/TransformComponent.h"
#include "Scene/UUID.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

namespace PulseForgeEditor
{
	namespace Detail
	{
		[[nodiscard]] inline bool IsFinite(const glm::vec3& Value);
	}

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

	[[nodiscard]] inline std::optional<float> SnapValueToIncrement(float Value, float Increment)
	{
		if (!std::isfinite(Value) || !std::isfinite(Increment) || Increment <= 0.0f)
			return std::nullopt;
		const float Snapped = std::round(Value / Increment) * Increment;
		return std::isfinite(Snapped) ? std::optional<float>(Snapped) : std::nullopt;
	}

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
		return Context.SceneAvailable && !Context.RuntimeActive && !Context.CameraNavigationActive &&
			!Context.ViewportKeyboardFocused &&
			!Context.RightMouseHeld && !Context.TextInputActive && !Context.ActiveImGuiItem && !Context.PopupOpen &&
			!Context.MenuNavigationActive && !Context.ModifierHeld;
	}

	[[nodiscard]] constexpr bool CanHandleFocusedViewportToolShortcuts(
		const ViewportShortcutContext& Context) noexcept
	{
		return Context.SceneAvailable && !Context.RuntimeActive && !Context.CameraNavigationActive &&
			Context.ViewportKeyboardFocused && !Context.RightMouseHeld && !Context.TextInputActive &&
			!Context.ActiveImGuiItem && !Context.PopupOpen && !Context.MenuNavigationActive && !Context.ModifierHeld;
	}

	[[nodiscard]] constexpr std::optional<ViewportTool> GetViewportToolForShortcut(char Key) noexcept
	{
		switch (Key)
		{
		case 'q':
		case 'Q': return ViewportTool::Select;
		case 'w':
		case 'W': return ViewportTool::Move;
		case 'e':
		case 'E': return ViewportTool::Rotate;
		case 'r':
		case 'R': return ViewportTool::Scale;
		default: return std::nullopt;
		}
	}

	[[nodiscard]] constexpr std::optional<ViewportTool> GetFocusedViewportToolForShortcut(char Key) noexcept
	{
		switch (Key)
		{
		case '1': return ViewportTool::Select;
		case '2': return ViewportTool::Move;
		case '3': return ViewportTool::Rotate;
		case '4': return ViewportTool::Scale;
		default: return std::nullopt;
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

	[[nodiscard]] constexpr bool ResolveViewportKeyboardFocus(
		bool CurrentlyFocused,
		ViewportKeyboardFocusEvent Event) noexcept
	{
		switch (Event)
		{
		case ViewportKeyboardFocusEvent::RenderedImageClicked: return true;
		case ViewportKeyboardFocusEvent::OtherEditorPanelClicked:
		case ViewportKeyboardFocusEvent::ApplicationFocusLost:
		case ViewportKeyboardFocusEvent::ViewportUnavailable:
		case ViewportKeyboardFocusEvent::PlayModeStarted: return false;
		case ViewportKeyboardFocusEvent::NoChange: return CurrentlyFocused;
		default: return false;
		}
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

	[[nodiscard]] constexpr bool CanNavigateViewportCamera(
		const ViewportCameraNavigationContext& Context) noexcept
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

	[[nodiscard]] inline glm::vec3 CalculateViewportCameraMoveOffset(
		const glm::vec3& Forward,
		const glm::vec3& Right,
		const ViewportCameraMoveKeys& Keys,
		float MoveSpeed,
		double DeltaSeconds) noexcept
	{
		if (!Detail::IsFinite(Forward) || !Detail::IsFinite(Right) || !std::isfinite(MoveSpeed) || MoveSpeed <= 0.0f ||
			!std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.0)
			return glm::vec3(0.0f);

		glm::vec3 Direction(0.0f);
		if (Keys.Forward)
			Direction += Forward;
		if (Keys.Backward)
			Direction -= Forward;
		if (Keys.Right)
			Direction += Right;
		if (Keys.Left)
			Direction -= Right;
		if (Keys.Up)
			Direction.y += 1.0f;
		if (Keys.Down)
			Direction.y -= 1.0f;
		const float DirectionLength = glm::length(Direction);
		if (!std::isfinite(DirectionLength) || DirectionLength < 1.0e-6f)
			return glm::vec3(0.0f);

		const float Step = static_cast<float>((std::min)(DeltaSeconds, 0.1));
		const float Speed = MoveSpeed * (Keys.Fast ? 4.0f : 1.0f);
		const glm::vec3 Offset = Direction / DirectionLength * (Speed * Step);
		return Detail::IsFinite(Offset) ? Offset : glm::vec3(0.0f);
	}

	struct ViewportCameraDollyResult
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 FocusPivot{ 0.0f };
	};

	[[nodiscard]] inline std::optional<ViewportCameraDollyResult> CalculateViewportCameraDolly(
		const glm::vec3& Position,
		const glm::vec3& Forward,
		const std::optional<glm::vec3>& ExistingFocusPivot,
		float MoveSpeed,
		float Delta) noexcept
	{
		if (!Detail::IsFinite(Position) || !Detail::IsFinite(Forward) || !std::isfinite(MoveSpeed) || MoveSpeed <= 0.0f ||
			!std::isfinite(Delta))
			return std::nullopt;
		const float ForwardLength = glm::length(Forward);
		if (!std::isfinite(ForwardLength) || ForwardLength < 1.0e-6f)
			return std::nullopt;

		const glm::vec3 Direction = Forward / ForwardLength;
		glm::vec3 Pivot = ExistingFocusPivot.value_or(Position + Direction * (std::max)(MoveSpeed * 2.0f, 1.0f));
		if (!Detail::IsFinite(Pivot))
			return std::nullopt;

		const float CurrentDistance = glm::length(Position - Pivot);
		if (!std::isfinite(CurrentDistance))
			return std::nullopt;
		const float BoundedDelta = std::clamp(Delta, -48.0f, 48.0f);
		const float Distance = std::clamp(CurrentDistance * std::exp(-BoundedDelta * 0.12f), 0.15f, 100000.0f);
		const glm::vec3 UpdatedPosition = Pivot - Direction * Distance;
		if (!Detail::IsFinite(UpdatedPosition))
			return std::nullopt;
		return ViewportCameraDollyResult{ UpdatedPosition, Pivot };
	}

	[[nodiscard]] inline float GetViewportWheelDollyDelta(float WheelDelta) noexcept
	{
		return std::isfinite(WheelDelta) ? std::clamp(WheelDelta, -4.0f, 4.0f) * 1.5f : 0.0f;
	}

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

	[[nodiscard]] constexpr ViewportWheelAction ResolveViewportWheelAction(
		const ViewportWheelContext& Context) noexcept
	{
		if (Context.PopupOpen)
			return ViewportWheelAction::None;
		if (Context.RightMouseNavigationActive && Context.RightMouseHeld && !Context.RuntimeActive)
			return ViewportWheelAction::AdjustFlySpeed;
		if (Context.PointerOverViewportImage && !Context.RuntimeActive && !Context.RightMouseHeld)
			return ViewportWheelAction::Zoom;
		return Context.PointerOverViewportImage ? ViewportWheelAction::None : ViewportWheelAction::ScrollPanel;
	}

	namespace Detail
	{
		inline bool IsFinite(const glm::vec2& Value)
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		inline bool IsFinite(const glm::vec3& Value)
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		inline bool IsFinite(const glm::mat4& Value)
		{
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Value[Column][Row]))
						return false;
			return true;
		}

		inline bool IsFinite(const glm::quat& Value)
		{
			return std::isfinite(Value.w) && std::isfinite(Value.x) &&
				std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		inline std::optional<float> IntersectTriangle(
			const Ray& LocalRay,
			const glm::vec3& A,
			const glm::vec3& B,
			const glm::vec3& C)
		{
			const glm::vec3 Edge1 = B - A;
			const glm::vec3 Edge2 = C - A;
			const glm::vec3 Cross = glm::cross(LocalRay.Direction, Edge2);
			const float Determinant = glm::dot(Edge1, Cross);
			if (!std::isfinite(Determinant) || std::abs(Determinant) < 1.0e-7f)
				return std::nullopt;

			const float InverseDeterminant = 1.0f / Determinant;
			const glm::vec3 FromA = LocalRay.Origin - A;
			const float U = glm::dot(FromA, Cross) * InverseDeterminant;
			if (U < 0.0f || U > 1.0f)
				return std::nullopt;

			const glm::vec3 CrossFromA = glm::cross(FromA, Edge1);
			const float V = glm::dot(LocalRay.Direction, CrossFromA) * InverseDeterminant;
			if (V < 0.0f || U + V > 1.0f)
				return std::nullopt;

			const float Distance = glm::dot(Edge2, CrossFromA) * InverseDeterminant;
			return std::isfinite(Distance) && Distance > 0.0f
				? std::optional<float>{ Distance }
				: std::nullopt;
		}
	}

	[[nodiscard]] inline std::optional<std::array<ViewportOrientationAxis, 3>> ProjectViewportOrientationAxes(
		const glm::vec3& CameraForward)
	{
		if (!Detail::IsFinite(CameraForward))
			return std::nullopt;

		const float ForwardLength = glm::length(CameraForward);
		if (!std::isfinite(ForwardLength) || ForwardLength < 1.0e-6f)
			return std::nullopt;
		const glm::vec3 Forward = CameraForward / ForwardLength;
		const glm::vec3 RightUnnormalized = glm::cross(Forward, glm::vec3(0.0f, 1.0f, 0.0f));
		const float RightLength = glm::length(RightUnnormalized);
		if (!std::isfinite(RightLength) || RightLength < 1.0e-6f)
			return std::nullopt;
		const glm::vec3 Right = RightUnnormalized / RightLength;
		const glm::vec3 Up = glm::normalize(glm::cross(Right, Forward));
		constexpr std::array<glm::vec3, 3> WorldAxes = {
			glm::vec3(1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f)
		};

		std::array<ViewportOrientationAxis, 3> Result{};
		for (size_t Index = 0; Index < WorldAxes.size(); ++Index)
		{
			Result[Index].ScreenDirection = {
				glm::dot(WorldAxes[Index], Right),
				-glm::dot(WorldAxes[Index], Up)
			};
			Result[Index].Depth = glm::dot(WorldAxes[Index], Forward);
		}
		return Result;
	}

	// NVRHI's Vulkan backend uses a negative-height viewport, so NDC +Y maps to the top of the image.
	[[nodiscard]] inline std::optional<glm::vec2> ViewportImageToNdc(
		const glm::vec2& ImagePosition,
		const ViewportImageRect& ImageRect)
	{
		if (!Detail::IsFinite(ImagePosition) || !Detail::IsFinite(ImageRect.Minimum) ||
			!Detail::IsFinite(ImageRect.Maximum))
			return std::nullopt;

		const glm::vec2 ImageSize = ImageRect.Maximum - ImageRect.Minimum;
		if (ImageSize.x <= 0.0f || ImageSize.y <= 0.0f)
			return std::nullopt;

		const glm::vec2 Unit = (ImagePosition - ImageRect.Minimum) / ImageSize;
		const glm::vec2 Ndc{ Unit.x * 2.0f - 1.0f, 1.0f - Unit.y * 2.0f };
		return Detail::IsFinite(Ndc) ? std::optional<glm::vec2>{ Ndc } : std::nullopt;
	}

	[[nodiscard]] inline std::optional<glm::vec2> NdcToViewportImage(
		const glm::vec2& Ndc,
		const ViewportImageRect& ImageRect)
	{
		if (!Detail::IsFinite(Ndc) || !Detail::IsFinite(ImageRect.Minimum) ||
			!Detail::IsFinite(ImageRect.Maximum))
			return std::nullopt;

		const glm::vec2 ImageSize = ImageRect.Maximum - ImageRect.Minimum;
		if (ImageSize.x <= 0.0f || ImageSize.y <= 0.0f)
			return std::nullopt;

		const glm::vec2 Unit{ Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f };
		const glm::vec2 ImagePosition = ImageRect.Minimum + Unit * ImageSize;
		return Detail::IsFinite(ImagePosition) ? std::optional<glm::vec2>{ ImagePosition } : std::nullopt;
	}

	[[nodiscard]] inline std::optional<glm::vec2> ProjectWorldToViewportImage(
		const glm::vec3& WorldPosition,
		const glm::mat4& ViewProjection,
		const ViewportImageRect& ImageRect)
	{
		if (!Detail::IsFinite(WorldPosition) || !Detail::IsFinite(ViewProjection))
			return std::nullopt;

		const glm::vec4 Clip = ViewProjection * glm::vec4(WorldPosition, 1.0f);
		if (!std::isfinite(Clip.w) || Clip.w <= 1.0e-7f)
			return std::nullopt;

		const glm::vec3 Ndc = glm::vec3(Clip) / Clip.w;
		if (!Detail::IsFinite(Ndc) || Ndc.z < 0.0f || Ndc.z > 1.0f)
			return std::nullopt;
		return NdcToViewportImage(glm::vec2(Ndc), ImageRect);
	}

	[[nodiscard]] inline std::optional<Ray> MakeViewportRay(
		const glm::vec2& ScreenPosition,
		const ViewportImageRect& ImageRect,
		const glm::mat4& ViewProjection,
		bool AllowOutsideImage = false)
	{
		if (!Detail::IsFinite(ScreenPosition) || !Detail::IsFinite(ImageRect.Minimum) ||
			!Detail::IsFinite(ImageRect.Maximum) || !Detail::IsFinite(ViewProjection))
			return std::nullopt;

		if (!AllowOutsideImage && (ScreenPosition.x < ImageRect.Minimum.x || ScreenPosition.x >= ImageRect.Maximum.x ||
			ScreenPosition.y < ImageRect.Minimum.y || ScreenPosition.y >= ImageRect.Maximum.y))
			return std::nullopt;
		const auto Ndc = ViewportImageToNdc(ScreenPosition, ImageRect);
		if (!Ndc)
			return std::nullopt;

		const float Determinant = glm::determinant(ViewProjection);
		if (!std::isfinite(Determinant) || std::abs(Determinant) < std::numeric_limits<float>::min())
			return std::nullopt;

		const glm::mat4 InverseViewProjection = glm::inverse(ViewProjection);
		if (!Detail::IsFinite(InverseViewProjection))
			return std::nullopt;

		const glm::vec4 NearHomogeneous = InverseViewProjection * glm::vec4(*Ndc, 0.0f, 1.0f);
		const glm::vec4 FarHomogeneous = InverseViewProjection * glm::vec4(*Ndc, 1.0f, 1.0f);
		if (!std::isfinite(NearHomogeneous.w) || !std::isfinite(FarHomogeneous.w) ||
			std::abs(NearHomogeneous.w) < 1.0e-8f || std::abs(FarHomogeneous.w) < 1.0e-8f)
			return std::nullopt;

		const glm::vec3 NearPoint = glm::vec3(NearHomogeneous) / NearHomogeneous.w;
		const glm::vec3 FarPoint = glm::vec3(FarHomogeneous) / FarHomogeneous.w;
		const glm::vec3 Direction = FarPoint - NearPoint;
		const float DirectionLength = glm::length(Direction);
		if (!Detail::IsFinite(NearPoint) || !Detail::IsFinite(FarPoint) ||
			!std::isfinite(DirectionLength) || DirectionLength < 1.0e-7f)
			return std::nullopt;

		return Ray{ NearPoint, Direction / DirectionLength, DirectionLength };
	}

	[[nodiscard]] inline std::optional<glm::vec3> IntersectRayPlane(
		const Ray& InputRay,
		const glm::vec3& PointOnPlane,
		const glm::vec3& PlaneNormal)
	{
		if (!Detail::IsFinite(InputRay.Origin) || !Detail::IsFinite(InputRay.Direction) ||
			!Detail::IsFinite(PointOnPlane) || !Detail::IsFinite(PlaneNormal))
			return std::nullopt;

		const float Denominator = glm::dot(InputRay.Direction, PlaneNormal);
		if (!std::isfinite(Denominator) || std::abs(Denominator) < 1.0e-6f)
			return std::nullopt;
		const float Distance = glm::dot(PointOnPlane - InputRay.Origin, PlaneNormal) / Denominator;
		if (!std::isfinite(Distance) || Distance < 0.0f ||
			(std::isfinite(InputRay.MaxDistance) && Distance > InputRay.MaxDistance))
			return std::nullopt;

		const glm::vec3 Intersection = InputRay.Origin + InputRay.Direction * Distance;
		return Detail::IsFinite(Intersection) ? std::optional<glm::vec3>{ Intersection } : std::nullopt;
	}

	// Mesh positions and indices are borrowed for this synchronous query. The direction is normalized internally.
	[[nodiscard]] inline std::optional<PickHit> RaycastMeshes(
		const Ray& WorldRay,
		std::span<const PickableMesh> Meshes)
	{
		if (!Detail::IsFinite(WorldRay.Origin) || !Detail::IsFinite(WorldRay.Direction) ||
			std::isnan(WorldRay.MaxDistance) || WorldRay.MaxDistance <= 0.0f)
			return std::nullopt;
		const float DirectionLength = glm::length(WorldRay.Direction);
		if (!std::isfinite(DirectionLength) || DirectionLength < 1.0e-7f)
			return std::nullopt;
		const Ray NormalizedWorldRay{ WorldRay.Origin, WorldRay.Direction / DirectionLength, WorldRay.MaxDistance };

		std::optional<PickHit> Closest;
		for (const PickableMesh& Mesh : Meshes)
		{
			if (Mesh.Entity.IsNil() || Mesh.Positions.size() < 3 || !Detail::IsFinite(Mesh.WorldTransform))
				continue;

			const float Determinant = glm::determinant(Mesh.WorldTransform);
			if (!std::isfinite(Determinant) || std::abs(Determinant) < std::numeric_limits<float>::min())
				continue;
			const glm::mat4 InverseWorld = glm::inverse(Mesh.WorldTransform);
			if (!Detail::IsFinite(InverseWorld))
				continue;

			const glm::vec4 LocalOrigin = InverseWorld * glm::vec4(NormalizedWorldRay.Origin, 1.0f);
			const glm::vec4 LocalDirection = InverseWorld * glm::vec4(NormalizedWorldRay.Direction, 0.0f);
			if (!std::isfinite(LocalOrigin.w) || std::abs(LocalOrigin.w) < 1.0e-8f)
				continue;
			const Ray LocalRay{
				glm::vec3(LocalOrigin) / LocalOrigin.w,
				glm::vec3(LocalDirection) };
			if (!Detail::IsFinite(LocalRay.Origin) || !Detail::IsFinite(LocalRay.Direction))
				continue;

			const size_t TriangleIndexCount = Mesh.Indices.empty()
				? Mesh.Positions.size()
				: Mesh.Indices.size();
			for (size_t Index = 0; Index + 2 < TriangleIndexCount; Index += 3)
			{
				const uint32_t A = Mesh.Indices.empty() ? static_cast<uint32_t>(Index) : Mesh.Indices[Index];
				const uint32_t B = Mesh.Indices.empty() ? static_cast<uint32_t>(Index + 1) : Mesh.Indices[Index + 1];
				const uint32_t C = Mesh.Indices.empty() ? static_cast<uint32_t>(Index + 2) : Mesh.Indices[Index + 2];
				if (A >= Mesh.Positions.size() || B >= Mesh.Positions.size() || C >= Mesh.Positions.size())
					continue;

				const auto Distance = Detail::IntersectTriangle(
					LocalRay, Mesh.Positions[A], Mesh.Positions[B], Mesh.Positions[C]);
				if (Distance && *Distance <= NormalizedWorldRay.MaxDistance &&
					(!Closest || *Distance < Closest->Distance))
					Closest = PickHit{ Mesh.Entity, *Distance };
			}
		}
		return Closest;
	}

	[[nodiscard]] inline std::optional<SupportingSurfaceHit> RaycastSupportingSurface(
		const Ray& WorldRay,
		std::span<const PickableMesh> Meshes,
		float MinimumUpDot = 0.5f)
	{
		if (!Detail::IsFinite(WorldRay.Origin) || !Detail::IsFinite(WorldRay.Direction) ||
			std::isnan(WorldRay.MaxDistance) || WorldRay.MaxDistance <= 0.0f ||
			!std::isfinite(MinimumUpDot) || MinimumUpDot < 0.0f || MinimumUpDot > 1.0f)
			return std::nullopt;
		const float DirectionLength = glm::length(WorldRay.Direction);
		if (!std::isfinite(DirectionLength) || DirectionLength < 1.0e-7f)
			return std::nullopt;
		const Ray NormalizedRay{ WorldRay.Origin, WorldRay.Direction / DirectionLength, WorldRay.MaxDistance };
		std::optional<SupportingSurfaceHit> Closest;
		for (const PickableMesh& Mesh : Meshes)
		{
			if (Mesh.Entity.IsNil() || Mesh.Positions.size() < 3 || !Detail::IsFinite(Mesh.WorldTransform))
				continue;
			const size_t TriangleCount = Mesh.Indices.empty() ? Mesh.Positions.size() : Mesh.Indices.size();
			for (size_t Index = 0; Index + 2 < TriangleCount; Index += 3)
			{
				const uint32_t A = Mesh.Indices.empty() ? static_cast<uint32_t>(Index) : Mesh.Indices[Index];
				const uint32_t B = Mesh.Indices.empty() ? static_cast<uint32_t>(Index + 1) : Mesh.Indices[Index + 1];
				const uint32_t C = Mesh.Indices.empty() ? static_cast<uint32_t>(Index + 2) : Mesh.Indices[Index + 2];
				if (A >= Mesh.Positions.size() || B >= Mesh.Positions.size() || C >= Mesh.Positions.size())
					continue;
				const glm::vec4 WorldA = Mesh.WorldTransform * glm::vec4(Mesh.Positions[A], 1.0f);
				const glm::vec4 WorldB = Mesh.WorldTransform * glm::vec4(Mesh.Positions[B], 1.0f);
				const glm::vec4 WorldC = Mesh.WorldTransform * glm::vec4(Mesh.Positions[C], 1.0f);
				if (!std::isfinite(WorldA.w) || !std::isfinite(WorldB.w) || !std::isfinite(WorldC.w) ||
					std::abs(WorldA.w) < 1.0e-7f || std::abs(WorldB.w) < 1.0e-7f || std::abs(WorldC.w) < 1.0e-7f)
					continue;
				const glm::vec3 PositionA = glm::vec3(WorldA) / WorldA.w;
				const glm::vec3 PositionB = glm::vec3(WorldB) / WorldB.w;
				const glm::vec3 PositionC = glm::vec3(WorldC) / WorldC.w;
				if (!Detail::IsFinite(PositionA) || !Detail::IsFinite(PositionB) || !Detail::IsFinite(PositionC))
					continue;
				glm::vec3 Normal = glm::cross(PositionB - PositionA, PositionC - PositionA);
				const float NormalLength = glm::length(Normal);
				if (!std::isfinite(NormalLength) || NormalLength < 1.0e-7f)
					continue;
				Normal /= NormalLength;
				if (Normal.y < 0.0f)
					Normal = -Normal;
				if (Normal.y < MinimumUpDot)
					continue;
				const auto Distance = Detail::IntersectTriangle(NormalizedRay, PositionA, PositionB, PositionC);
				if (!Distance || *Distance > NormalizedRay.MaxDistance || (Closest && *Distance >= Closest->Distance))
					continue;
				Closest = SupportingSurfaceHit{
					Mesh.Entity,
					*Distance,
					NormalizedRay.Origin + NormalizedRay.Direction * *Distance,
					Normal };
			}
		}
		return Closest;
	}

	[[nodiscard]] inline std::optional<WorldGizmoAxis> GetWorldGizmoAxis(
		const glm::mat4& ParentWorldTransform,
		const glm::quat& LocalRotation,
		uint32_t AxisIndex)
	{
		if (AxisIndex >= 3 || !Detail::IsFinite(ParentWorldTransform) || !Detail::IsFinite(LocalRotation))
			return std::nullopt;
		const float RotationLength = glm::length(LocalRotation);
		if (!std::isfinite(RotationLength) || RotationLength < 1.0e-7f)
			return std::nullopt;

		const glm::vec3 Basis(AxisIndex == 0, AxisIndex == 1, AxisIndex == 2);
		const glm::vec3 ParentLocalAxis = glm::normalize(LocalRotation) * Basis;
		const glm::vec3 WorldVector = glm::mat3(ParentWorldTransform) * ParentLocalAxis;
		const float WorldLength = glm::length(WorldVector);
		if (!Detail::IsFinite(WorldVector) || !std::isfinite(WorldLength) || WorldLength < 1.0e-7f)
			return std::nullopt;
		return WorldGizmoAxis{ WorldVector / WorldLength, WorldLength };
	}

	[[nodiscard]] inline std::optional<WorldGizmoAxis> GetGizmoAxis(
		const glm::mat4& ParentWorldTransform,
		const glm::quat& LocalRotation,
		uint32_t AxisIndex,
		bool LocalSpace)
	{
		if (LocalSpace)
			return GetWorldGizmoAxis(ParentWorldTransform, LocalRotation, AxisIndex);
		if (AxisIndex >= 3)
			return std::nullopt;
		const glm::vec3 Direction(AxisIndex == 0, AxisIndex == 1, AxisIndex == 2);
		return WorldGizmoAxis{ Direction, 1.0f };
	}

	[[nodiscard]] inline std::optional<float> WorldDeltaToLocalAxisDelta(
		float WorldDelta,
		const glm::mat4& ParentWorldTransform,
		const glm::quat& LocalRotation,
		uint32_t AxisIndex)
	{
		if (!std::isfinite(WorldDelta))
			return std::nullopt;
		const auto Axis = GetWorldGizmoAxis(ParentWorldTransform, LocalRotation, AxisIndex);
		if (!Axis)
			return std::nullopt;
		const float Delta = WorldDelta / Axis->WorldUnitsPerLocalUnit;
		return std::isfinite(Delta) ? std::optional<float>{ Delta } : std::nullopt;
	}

	[[nodiscard]] inline std::optional<PulseForge::TransformComponent> ApplyLocalGizmoDelta(
		const PulseForge::TransformComponent& InitialTransform,
		TransformGizmoOperation Operation,
		uint32_t AxisIndex,
		float Delta)
	{
		if (AxisIndex >= 3 || !std::isfinite(Delta) || !Detail::IsFinite(InitialTransform.Translation) ||
			!Detail::IsFinite(InitialTransform.Scale) || !Detail::IsFinite(InitialTransform.Rotation))
			return std::nullopt;
		const float RotationLength = glm::length(InitialTransform.Rotation);
		if (!std::isfinite(RotationLength) || RotationLength < 1.0e-7f)
			return std::nullopt;

		const glm::vec3 Basis(AxisIndex == 0, AxisIndex == 1, AxisIndex == 2);
		const glm::quat Rotation = glm::normalize(InitialTransform.Rotation);
		PulseForge::TransformComponent Updated = InitialTransform;
		switch (Operation)
		{
		case TransformGizmoOperation::Translate:
			Updated.Translation += Rotation * Basis * Delta;
			break;
		case TransformGizmoOperation::Rotate:
			Updated.Rotation = glm::normalize(Rotation * glm::angleAxis(Delta, Basis));
			break;
		case TransformGizmoOperation::Scale:
			Updated.Scale[AxisIndex] += Delta;
			if (std::abs(Updated.Scale[AxisIndex]) < 1.0e-4f)
				return std::nullopt;
			break;
		default:
			return std::nullopt;
		}

		return Detail::IsFinite(Updated.Translation) && Detail::IsFinite(Updated.Scale) &&
			Detail::IsFinite(Updated.Rotation)
			? std::optional<PulseForge::TransformComponent>{ Updated }
			: std::nullopt;
	}
}
