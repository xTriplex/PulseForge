#include "EditorConsoleHistory.h"
#include "Editor/EditorContentBrowserState.h"
#include "Editor/EditorAssetWorkflowState.h"
#include "Editor/EditorSceneHistoryState.h"
#include "Editor/EditorSceneWorkspaceState.h"
#include "EditorMaterialDraftStore.h"
#include "EditorViewportCamera.h"
#include "TestEditorSupport.h"

namespace
{
	void TestEditorViewportMath(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		PF_CHECK(Tests, GetViewportToolForShortcut('q') == ViewportTool::Select);
		PF_CHECK(Tests, GetViewportToolForShortcut('Q') == ViewportTool::Select);
		PF_CHECK(Tests, GetViewportToolForShortcut('w') == ViewportTool::Move);
		PF_CHECK(Tests, GetViewportToolForShortcut('W') == ViewportTool::Move);
		PF_CHECK(Tests, GetViewportToolForShortcut('e') == ViewportTool::Rotate);
		PF_CHECK(Tests, GetViewportToolForShortcut('E') == ViewportTool::Rotate);
		PF_CHECK(Tests, GetViewportToolForShortcut('r') == ViewportTool::Scale);
		PF_CHECK(Tests, GetViewportToolForShortcut('R') == ViewportTool::Scale);
		PF_CHECK(Tests, !GetViewportToolForShortcut('x'));
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('1') == ViewportTool::Select);
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('2') == ViewportTool::Move);
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('3') == ViewportTool::Rotate);
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('4') == ViewportTool::Scale);
		PF_CHECK(Tests, !GetFocusedViewportToolForShortcut('w'));
		const ViewportShortcutContext Ready{ .SceneAvailable = true };
		PF_CHECK(Tests, CanHandleViewportToolShortcuts(Ready));
		const ViewportShortcutContext Focused{ .SceneAvailable = true, .ViewportKeyboardFocused = true };
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts(Focused));
		PF_CHECK(Tests, CanHandleFocusedViewportToolShortcuts(Focused));
		PF_CHECK(Tests, ResolveViewportKeyboardFocus(false,
			ViewportKeyboardFocusEvent::RenderedImageClicked));
		PF_CHECK(Tests, ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::NoChange));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::OtherEditorPanelClicked));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::ApplicationFocusLost));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::ViewportUnavailable));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::PlayModeStarted));
		PF_CHECK(Tests, !CanHandleFocusedViewportToolShortcuts({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanHandleFocusedViewportToolShortcuts({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ModifierHeld = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .RuntimeActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .CameraNavigationActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .RightMouseHeld = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .ActiveImGuiItem = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .PopupOpen = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .MenuNavigationActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .ModifierHeld = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({}));

		const ViewportCameraNavigationContext KeyboardNavigation{
			.SceneAvailable = true,
			.ViewportKeyboardFocused = true,
			.ApplicationFocused = true
		};
		PF_CHECK(Tests, CanNavigateViewportCamera(KeyboardNavigation));
		PF_CHECK(Tests, !CanNavigateViewportCamera({ .SceneAvailable = true, .ApplicationFocused = true }));
		PF_CHECK(Tests, CanNavigateViewportCamera({
			.SceneAvailable = true, .RightMouseNavigationActive = true, .ApplicationFocused = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .RuntimeActive = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .ActiveImGuiItem = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .PopupOpen = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .MenuNavigationActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .ModifierHeld = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .GizmoInteractionActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .CameraGestureActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = false }));

		const glm::vec3 CameraForward{ 0.0f, 0.0f, -1.0f };
		const glm::vec3 CameraRight{ 1.0f, 0.0f, 0.0f };
		const auto ForwardStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true }, 5.0f, 0.5);
		const auto BackwardStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Backward = true }, 5.0f, 0.1);
		const auto RightStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Right = true }, 5.0f, 0.1);
		const auto LeftStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Left = true }, 5.0f, 0.1);
		const auto UpStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Up = true }, 5.0f, 0.1);
		const auto DownStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Down = true }, 5.0f, 0.1);
		const auto DiagonalStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true, .Right = true }, 5.0f, 0.1);
		const auto FastStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true, .Fast = true }, 5.0f, 0.1);
		PF_CHECK(Tests, glm::length(ForwardStep - glm::vec3(0.0f, 0.0f, -0.5f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(BackwardStep - glm::vec3(0.0f, 0.0f, 0.5f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(RightStep - glm::vec3(0.5f, 0.0f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(LeftStep - glm::vec3(-0.5f, 0.0f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(UpStep - glm::vec3(0.0f, 0.5f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(DownStep - glm::vec3(0.0f, -0.5f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, std::abs(glm::length(DiagonalStep) - glm::length(RightStep)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(FastStep - ForwardStep * 4.0f) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true }, 5.0f, 0.0)) == 0.0f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, {}, 5.0f, 0.1)) == 0.0f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true }, 5.0f, 2.0) - ForwardStep) < 1.0e-5f);

		const float WheelDollyDelta = GetViewportWheelDollyDelta(1.0f);
		const auto ZoomWithoutPivot = CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, WheelDollyDelta);
		const auto ZoomOutWithoutPivot = CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, GetViewportWheelDollyDelta(-1.0f));
		const auto ZoomWithPivot = CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, glm::vec3(0.0f, 0.0f, -10.0f), 5.0f, WheelDollyDelta);
		PF_CHECK(Tests, ZoomWithoutPivot && ZoomWithoutPivot->Position.z < 0.0f);
		PF_CHECK(Tests, ZoomWithoutPivot && glm::length(ZoomWithoutPivot->FocusPivot - glm::vec3(0, 0, -10)) < 1.0e-5f);
		PF_CHECK(Tests, ZoomOutWithoutPivot && ZoomOutWithoutPivot->Position.z > 0.0f);
		PF_CHECK(Tests, ZoomWithPivot && ZoomWithPivot->Position.z < 0.0f);
		PF_CHECK(Tests, ZoomWithPivot && glm::length(ZoomWithPivot->FocusPivot - glm::vec3(0, 0, -10)) < 1.0e-5f);
		glm::vec3 RepeatedZoomPosition{ 0.0f };
		std::optional<glm::vec3> RepeatedZoomPivot;
		for (size_t Step = 0; Step < 100; ++Step)
		{
			const auto Updated = CalculateViewportCameraDolly(
				RepeatedZoomPosition, CameraForward, RepeatedZoomPivot, 5.0f, WheelDollyDelta);
			if (!Updated)
				break;
			RepeatedZoomPosition = Updated->Position;
			RepeatedZoomPivot = Updated->FocusPivot;
		}
		PF_CHECK(Tests, PulseForgeEditor::Detail::IsFinite(RepeatedZoomPosition) && RepeatedZoomPivot &&
			glm::length(RepeatedZoomPosition - *RepeatedZoomPivot) >= 0.149f);
		PF_CHECK(Tests, GetViewportWheelDollyDelta(100.0f) == 6.0f);
		PF_CHECK(Tests, GetViewportWheelDollyDelta(std::numeric_limits<float>::infinity()) == 0.0f);
		PF_CHECK(Tests, !CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, std::numeric_limits<float>::quiet_NaN()));

		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true }) == ViewportWheelAction::Zoom);
		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .RightMouseHeld = true }) ==
			ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({
			.PointerOverViewportImage = true, .RightMouseNavigationActive = true, .RightMouseHeld = true }) ==
			ViewportWheelAction::AdjustFlySpeed);
		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .RuntimeActive = true }) ==
			ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .PopupOpen = true }) ==
			ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({}) == ViewportWheelAction::ScrollPanel);
		PF_CHECK(Tests, FindEditorIcon(EditorIcon::Translate)->Codepoint !=
			FindEditorIcon(EditorIcon::Scale)->Codepoint);

		const ViewportImageRect ImageRect{ { 100.0f, 50.0f }, { 300.0f, 150.0f } };
		const auto TopLeft = NdcToViewportImage({ -1.0f, 1.0f }, ImageRect);
		const auto BottomRight = NdcToViewportImage({ 1.0f, -1.0f }, ImageRect);
		const auto ImageCenter = NdcToViewportImage({ 0.0f, 0.0f }, ImageRect);
		PF_CHECK(Tests, TopLeft && glm::length(*TopLeft - ImageRect.Minimum) < 1.0e-5f);
		PF_CHECK(Tests, BottomRight && glm::length(*BottomRight - ImageRect.Maximum) < 1.0e-5f);
		PF_CHECK(Tests, ImageCenter && glm::length(*ImageCenter - glm::vec2(200.0f, 100.0f)) < 1.0e-5f);
		const auto TopLeftNdc = ViewportImageToNdc(ImageRect.Minimum, ImageRect);
		const auto BottomRightNdc = ViewportImageToNdc(ImageRect.Maximum, ImageRect);
		PF_CHECK(Tests, TopLeftNdc && glm::length(*TopLeftNdc - glm::vec2(-1.0f, 1.0f)) < 1.0e-5f);
		PF_CHECK(Tests, BottomRightNdc && glm::length(*BottomRightNdc - glm::vec2(1.0f, -1.0f)) < 1.0e-5f);
		PF_CHECK(Tests, !NdcToViewportImage({ 0.0f, 0.0f }, {}));
		PF_CHECK(Tests, !ViewportImageToNdc({ 0.0f, 0.0f }, {}));
		const auto KnownProjectedPoint = ProjectWorldToViewportImage(
			{ 0.25f, -0.5f, 0.5f }, glm::mat4(1.0f), ImageRect);
		PF_CHECK(Tests, KnownProjectedPoint &&
			glm::length(*KnownProjectedPoint - glm::vec2(225.0f, 125.0f)) < 1.0e-5f);

		CameraComponent Camera;
		const auto Projection = Camera.GetProjectionMatrix(2.0f);
		PF_CHECK(Tests, Projection.has_value());
		if (!Projection)
			return;
		const glm::mat4 View = glm::lookAtRH(
			glm::vec3(0.0f, 0.0f, 5.0f),
			glm::vec3(0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 ViewProjection = *Projection * View;
		const auto ProjectedAbove = ProjectWorldToViewportImage({ 0.0f, 1.0f, 0.0f }, ViewProjection, ImageRect);
		const auto ProjectedBelow = ProjectWorldToViewportImage({ 0.0f, -1.0f, 0.0f }, ViewProjection, ImageRect);
		PF_CHECK(Tests, ProjectedAbove && ProjectedAbove->y < 100.0f);
		PF_CHECK(Tests, ProjectedBelow && ProjectedBelow->y > 100.0f);
		const ViewportImageRect DpiScaledRect{ { 200.0f, 100.0f }, { 600.0f, 300.0f } };
		const auto LogicalCenterNdc = ViewportImageToNdc({ 200.0f, 100.0f }, ImageRect);
		const auto DpiCenterNdc = ViewportImageToNdc({ 400.0f, 200.0f }, DpiScaledRect);
		PF_CHECK(Tests, LogicalCenterNdc && DpiCenterNdc &&
			glm::length(*LogicalCenterNdc - *DpiCenterNdc) < 1.0e-5f);

		const auto CenterRay = MakeViewportRay({ 200.0f, 100.0f }, ImageRect, ViewProjection);
		PF_CHECK(Tests, CenterRay.has_value());
		if (CenterRay)
		{
			PF_CHECK(Tests, glm::length(CenterRay->Direction - glm::vec3(0.0f, 0.0f, -1.0f)) < 1.0e-4f);
			PF_CHECK(Tests, CenterRay->Origin.z < 5.0f && CenterRay->Origin.z > 4.8f);
		}
		const auto RightRay = MakeViewportRay({ 250.0f, 100.0f }, ImageRect, ViewProjection);
		PF_CHECK(Tests, RightRay && RightRay->Direction.x > 0.0f);
		const auto TopImageRay = MakeViewportRay({ 200.0f, 75.0f }, ImageRect, ViewProjection);
		PF_CHECK(Tests, TopImageRay && TopImageRay->Direction.y > 0.0f);
		PF_CHECK(Tests, !MakeViewportRay({ 300.0f, 100.0f }, ImageRect, ViewProjection));
		PF_CHECK(Tests, !MakeViewportRay({ 99.0f, 100.0f }, ImageRect, ViewProjection));
		PF_CHECK(Tests, !MakeViewportRay({ 200.0f, 100.0f }, {}, ViewProjection));
		PF_CHECK(Tests, !MakeViewportRay({ 200.0f, 100.0f }, ImageRect, glm::mat4(0.0f)));
		const glm::vec2 Outside{ 350.0f, 100.0f };
		PF_CHECK(Tests, MakeViewportRay(Outside, ImageRect, ViewProjection, true).has_value());

		const std::array<ViewportImageRect, 2> ResizedImageRects{
			ViewportImageRect{ { 13.0f, 27.0f }, { 613.0f, 327.0f } },
			ViewportImageRect{ { 41.0f, 19.0f }, { 341.0f, 619.0f } }
		};
		for (const ViewportImageRect& TestRect : ResizedImageRects)
		{
			const float AspectRatio = (TestRect.Maximum.x - TestRect.Minimum.x) /
				(TestRect.Maximum.y - TestRect.Minimum.y);
			const auto TestProjection = Camera.GetProjectionMatrix(AspectRatio);
			PF_CHECK(Tests, TestProjection.has_value());
			if (!TestProjection)
				continue;

			const std::array<glm::vec3, 3> CameraPositions{
				glm::vec3(0.0f, 0.0f, 5.0f),
				glm::vec3(2.0f, 1.0f, 5.0f),
				glm::vec3(-3.0f, 2.0f, 4.0f)
			};
			const std::array<glm::vec3, 3> CameraTargets{
				glm::vec3(0.4f, -0.2f, 0.0f),
				glm::vec3(-0.5f, 0.3f, -0.5f),
				glm::vec3(0.8f, -0.4f, -1.0f)
			};
			for (size_t CameraIndex = 0; CameraIndex < CameraPositions.size(); ++CameraIndex)
			{
				const glm::mat4 CameraView = glm::lookAtRH(
					CameraPositions[CameraIndex],
					CameraTargets[CameraIndex],
					glm::vec3(0.0f, 1.0f, 0.0f));
				const glm::mat4 TestViewProjection = *TestProjection * CameraView;
				const glm::vec3 WorldPoint = CameraTargets[CameraIndex] + glm::vec3(0.15f, 0.1f, 0.0f);
				const auto Projected = ProjectWorldToViewportImage(WorldPoint, TestViewProjection, TestRect);
				PF_CHECK(Tests, Projected.has_value());
				if (!Projected)
					continue;

				const auto ThroughPoint = MakeViewportRay(*Projected, TestRect, TestViewProjection);
				PF_CHECK(Tests, ThroughPoint.has_value());
				if (ThroughPoint)
				{
					const glm::vec3 ToPoint = WorldPoint - ThroughPoint->Origin;
					const float AlongRay = glm::dot(ToPoint, ThroughPoint->Direction);
					const glm::vec3 ClosestPoint = ThroughPoint->Origin + ThroughPoint->Direction * AlongRay;
					PF_CHECK(Tests, AlongRay > 0.0f && AlongRay <= ThroughPoint->MaxDistance + 1.0e-3f);
					PF_CHECK(Tests, glm::length(ClosestPoint - WorldPoint) < 1.0e-3f);
				}
			}
		}

		const std::array<glm::vec3, 3> Triangle{
			glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
		const std::array<uint32_t, 3> Indices{ 0, 1, 2 };
		const UUID FarEntity{ 1, 1 };
		const UUID NearEntity{ 2, 2 };
		const glm::mat4 FarTransform = glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, -2.0f });
		const glm::mat4 NearTransform = glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, 1.0f });
		const std::array<PickableMesh, 2> Overlapping{
			PickableMesh{ FarEntity, FarTransform, Triangle, Indices },
			PickableMesh{ NearEntity, NearTransform, Triangle, Indices } };
		const Ray WorldRay{ { 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, -1.0f } };
		const auto Nearest = RaycastMeshes(WorldRay, Overlapping);
		PF_CHECK(Tests, Nearest && Nearest->Entity == NearEntity);
		PF_CHECK(Tests, Nearest && std::abs(Nearest->Distance - 4.0f) < 1.0e-4f);
		const std::array<PickableMesh, 1> NoHit{
			PickableMesh{ FarEntity, glm::translate(glm::mat4(1.0f), { 5.0f, 0.0f, 0.0f }), Triangle, Indices } };
		PF_CHECK(Tests, !RaycastMeshes(WorldRay, NoHit));

		const glm::mat4 RotatedNonUniform =
			glm::rotate(glm::scale(glm::mat4(1.0f), { 2.0f, 0.5f, 1.0f }), glm::radians(35.0f), { 0.0f, 0.0f, 1.0f });
		const std::array<PickableMesh, 1> TransformedMesh{
			PickableMesh{ NearEntity, RotatedNonUniform, Triangle, Indices } };
		const auto TransformedHit = RaycastMeshes(WorldRay, TransformedMesh);
		PF_CHECK(Tests, TransformedHit && TransformedHit->Entity == NearEntity);
		PF_CHECK(Tests, std::abs(glm::length(WorldRay.Direction) - 1.0f) < 1.0e-5f);
		const std::array<PickableMesh, 1> BeyondFarPlane{
			PickableMesh{ NearEntity, glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, -2000.0f }), Triangle, Indices } };
		PF_CHECK(Tests, !RaycastMeshes(*CenterRay, BeyondFarPlane));
		const std::array<uint32_t, 3> InvalidIndices{ 0, 1, 9 };
		const std::array<PickableMesh, 1> InvalidGeometry{
			PickableMesh{ NearEntity, glm::mat4(1.0f), Triangle, InvalidIndices } };
		PF_CHECK(Tests, !RaycastMeshes(WorldRay, InvalidGeometry));

		const std::array<glm::vec3, 4> FloorVertices{
			glm::vec3(-2.0f, 4.0f, -2.0f), glm::vec3(2.0f, 4.0f, -2.0f),
			glm::vec3(2.0f, 4.0f, 2.0f), glm::vec3(-2.0f, 4.0f, 2.0f) };
		const std::array<uint32_t, 6> FloorIndices{ 0, 1, 2, 0, 2, 3 };
		const std::array<glm::vec3, 4> LowerFloorVertices{
			glm::vec3(-2.0f, 2.0f, -2.0f), glm::vec3(2.0f, 2.0f, -2.0f),
			glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(-2.0f, 2.0f, 2.0f) };
		const UUID FloorEntity{ 3, 3 };
		const std::array<PickableMesh, 2> StackedFloors{
			PickableMesh{ FarEntity, glm::mat4(1.0f), LowerFloorVertices, FloorIndices },
			PickableMesh{ FloorEntity, glm::mat4(1.0f), FloorVertices, FloorIndices } };
		const auto NearestFloor = RaycastSupportingSurface(
			Ray{ { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, StackedFloors);
		PF_CHECK(Tests, NearestFloor && NearestFloor->Entity == FloorEntity);
		PF_CHECK(Tests, NearestFloor && std::abs(NearestFloor->Position.y - 4.0f) < 1.0e-4f &&
			std::abs(NearestFloor->Distance - 1.0f) < 1.0e-4f && NearestFloor->Normal.y > 0.999f);
		const std::array<glm::vec3, 3> WallVertices{
			glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 2.0f, -1.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
		const std::array<uint32_t, 3> WallIndices{ 0, 1, 2 };
		const std::array<PickableMesh, 1> VerticalWall{
			PickableMesh{ FloorEntity, glm::mat4(1.0f), WallVertices, WallIndices } };
		PF_CHECK(Tests, !RaycastSupportingSurface(
			Ray{ { 0.0f, 3.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, VerticalWall));
		const std::array<glm::vec3, 3> SlopeVertices{
			glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(2.0f, 1.0f, -1.0f), glm::vec3(2.0f, 1.0f, 1.0f) };
		const std::array<uint32_t, 3> SlopeIndices{ 0, 1, 2 };
		const std::array<PickableMesh, 1> SlopeMesh{
			PickableMesh{ FloorEntity, glm::mat4(1.0f), SlopeVertices, SlopeIndices } };
		const auto SlopeHit = RaycastSupportingSurface(
			Ray{ { 1.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, SlopeMesh);
		PF_CHECK(Tests, SlopeHit && std::abs(SlopeHit->Position.y - 0.5f) < 1.0e-4f && SlopeHit->Normal.y > 0.89f);
		PF_CHECK(Tests, !RaycastSupportingSurface(
			Ray{ { 1.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, SlopeMesh, 0.95f));
		PF_CHECK(Tests, SnapValueToIncrement(1.26f, 0.5f) == 1.5f);
		PF_CHECK(Tests, SnapValueToIncrement(-1.26f, 0.5f) == -1.5f);
		PF_CHECK(Tests, SnapValueToIncrement(2.0f, 0.0f) == std::nullopt);
		PF_CHECK(Tests, SnapValueToIncrement(std::numeric_limits<float>::infinity(), 1.0f) == std::nullopt);

		Scene HierarchyScene;
		auto Parent = HierarchyScene.CreateEntity("Picking parent");
		auto Child = HierarchyScene.CreateEntity("Picking child");
		PF_CHECK(Tests, Parent && Child);
		if (Parent && Child)
		{
			TransformComponent ParentTransform;
			ParentTransform.Translation = { 0.5f, 0.25f, 0.0f };
			ParentTransform.Rotation = glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			ParentTransform.Scale = { 1.5f, 0.75f, 1.0f };
			TransformComponent ChildTransform;
			ChildTransform.Rotation = glm::angleAxis(glm::radians(-15.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			ChildTransform.Scale = { 0.8f, 1.2f, 1.0f };
			PF_CHECK(Tests, Parent->SetTransform(ParentTransform).has_value());
			PF_CHECK(Tests, Child->SetTransform(ChildTransform).has_value());
			PF_CHECK(Tests, Child->SetParent(*Parent).has_value());
			const auto ChildWorld = Child->GetWorldMatrix();
			PF_CHECK(Tests, ChildWorld.has_value());
			if (ChildWorld)
			{
				const glm::vec3 Center = glm::vec3((*ChildWorld)[3]);
				const glm::vec3 Normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(*ChildWorld))) *
					glm::vec3(0.0f, 0.0f, 1.0f));
				const Ray ChildRay{ Center + Normal * 4.0f, -Normal };
				const std::array<PickableMesh, 1> ParentedMesh{
					PickableMesh{ Child->GetUUID(), *ChildWorld, Triangle, Indices } };
				const auto ChildHit = RaycastMeshes(ChildRay, ParentedMesh);
				PF_CHECK(Tests, ChildHit && ChildHit->Entity == Child->GetUUID());
				PF_CHECK(Tests, ChildHit && std::abs(ChildHit->Distance - 4.0f) < 1.0e-3f);
			}
		}

		const glm::mat4 ParentWorld = glm::translate(glm::mat4(1.0f), { 3.0f, -1.0f, 2.0f }) *
			glm::rotate(glm::mat4(1.0f), glm::radians(25.0f), { 0.0f, 0.0f, 1.0f }) *
			glm::scale(glm::mat4(1.0f), { 2.0f, 3.0f, 0.5f });
		const glm::quat LocalRotation = glm::angleAxis(glm::radians(40.0f), glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
		const auto ParentAxis = GetWorldGizmoAxis(ParentWorld, LocalRotation, 1);
		const auto WorldAxis = GetGizmoAxis(ParentWorld, LocalRotation, 0, false);
		const auto LocalDelta = WorldDeltaToLocalAxisDelta(2.5f, ParentWorld, LocalRotation, 1);
		PF_CHECK(Tests, ParentAxis && LocalDelta && WorldAxis && ParentAxis->WorldUnitsPerLocalUnit > 0.0f);
		PF_CHECK(Tests, WorldAxis && glm::length(WorldAxis->Direction - glm::vec3(1.0f, 0.0f, 0.0f)) < 1.0e-6f &&
			std::abs(WorldAxis->WorldUnitsPerLocalUnit - 1.0f) < 1.0e-6f);
		if (ParentAxis && LocalDelta)
		{
			const glm::vec3 LocalMovement = LocalRotation * glm::vec3(0.0f, 1.0f, 0.0f) * *LocalDelta;
			const glm::vec3 WorldMovement = glm::mat3(ParentWorld) * LocalMovement;
			PF_CHECK(Tests, glm::length(WorldMovement - ParentAxis->Direction * 2.5f) < 1.0e-4f);
		}
		PF_CHECK(Tests, !GetWorldGizmoAxis(ParentWorld, LocalRotation, 3));
		PF_CHECK(Tests, !WorldDeltaToLocalAxisDelta(std::numeric_limits<float>::quiet_NaN(), ParentWorld, LocalRotation, 1));

		TransformComponent Initial;
		Initial.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
		const auto Translated = ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Translate, 0, 2.0f);
		PF_CHECK(Tests, Translated && glm::length(Translated->Translation - glm::vec3(0.0f, 2.0f, 0.0f)) < 1.0e-4f);
		const auto Rotated = ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Rotate, 2, glm::half_pi<float>());
		PF_CHECK(Tests, Rotated && std::abs(glm::length(Rotated->Rotation) - 1.0f) < 1.0e-5f);
		const auto Scaled = ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Scale, 1, 0.5f);
		PF_CHECK(Tests, Scaled && std::abs(Scaled->Scale.y - 1.5f) < 1.0e-5f);
		PF_CHECK(Tests, !ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Scale, 0, -1.0f));
		PF_CHECK(Tests, !ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Rotate, 3, 0.5f));
		PF_CHECK(Tests, !ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Translate, 0,
			std::numeric_limits<float>::infinity()));
	}

	void TestEditorWorkspacePanelState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		PF_CHECK(Tests, CurrentDefaultEditorLayoutVersion == 4);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("0") == 0);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("1") == 1);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("01") == 1);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("2") == 2);
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("3"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("4294967296"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("remembered"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference(" 1"));
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, true, 2) == 2);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, false, 2) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(false, true, 1) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, true, std::nullopt) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::ContentBrowser) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Console) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Scene) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Inspector) == EditorDefaultDockRegion::Right);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::MaterialDetails) == EditorDefaultDockRegion::Right);
		PF_CHECK(Tests, EditorPanelDescriptors.size() == static_cast<size_t>(EditorPanel::Count));
		constexpr std::array<std::string_view, static_cast<size_t>(EditorPanel::Count)> StablePanelNames = {
			"Scene Viewport", "Hierarchy", "Inspector", "Content Browser", "Console", "Scene", "Material Editor"
		};

		EditorPanelVisibility Panels;
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::SceneViewport));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Hierarchy));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Inspector));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::ContentBrowser));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Console));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::MaterialDetails));

		for (size_t Index = 0; Index < EditorPanelDescriptors.size(); ++Index)
		{
			const EditorPanelDescriptor& Descriptor = EditorPanelDescriptors[Index];
			PF_CHECK(Tests, Descriptor.WindowName == StablePanelNames[Index]);
			PF_CHECK(Tests, !Descriptor.SettingsKey.empty());
			PF_CHECK(Tests, !Descriptor.WindowName.empty());
			if (Descriptor.Panel == EditorPanel::MaterialDetails)
				PF_CHECK(Tests, Descriptor.IsUtilityPanel && !Descriptor.DefaultVisible &&
					Descriptor.SettingsKey == "MaterialEditor");
			for (size_t OtherIndex = Index + 1; OtherIndex < EditorPanelDescriptors.size(); ++OtherIndex)
			{
				PF_CHECK(Tests, Descriptor.SettingsKey != EditorPanelDescriptors[OtherIndex].SettingsKey);
				PF_CHECK(Tests, Descriptor.WindowName != EditorPanelDescriptors[OtherIndex].WindowName);
			}
		}

		PF_CHECK(Tests, Panels.SetVisible(EditorPanel::Scene, true));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, Panels.SetVisible(EditorPanel::Console, false));
		PF_CHECK(Tests, !Panels.SetVisible(EditorPanel::Console, false));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Console));
		Panels.ResetToDefaults();
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Console));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::MaterialDetails));
		PF_CHECK(Tests, Panels.GetVisibility(EditorPanel::Scene) != nullptr);
		PF_CHECK(Tests, Panels.GetVisibility(EditorPanel::Count) == nullptr);
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Count));
		PF_CHECK(Tests, !Panels.SetVisible(EditorPanel::Count, true));
		PF_CHECK(Tests, !ShouldApplyFirstOpenPanelDocking(true, true));
		PF_CHECK(Tests, !ShouldApplyFirstOpenPanelDocking(false, false));
		PF_CHECK(Tests, ShouldApplyFirstOpenPanelDocking(false, true));
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".material", true, false) == AssetBrowserOpenAction::None);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".material", true, true) == AssetBrowserOpenAction::ShowMaterialDetails);
		// Utility-panel opening is intentionally independent of document-host preferences.
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".scene", true, true) == AssetBrowserOpenAction::OpenScene);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".png", true, true) == AssetBrowserOpenAction::None);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".scene", false, true) == AssetBrowserOpenAction::None);
		const std::string DraftKey = MakeMaterialDraftKey("C:/Project", "material-uuid");
		PF_CHECK(Tests, DraftKey == MakeMaterialDraftKey("C:/Project", "material-uuid"));
		PF_CHECK(Tests, DraftKey != MakeMaterialDraftKey("D:/Project", "material-uuid"));
		PF_CHECK(Tests, DraftKey != MakeMaterialDraftKey("C:/Project", "other-material-uuid"));
	}

	void TestEditorDeferredOperationQueue(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		EditorDeferredOperationQueue Operations;
		std::vector<int> ExecutionOrder;
		Operations.Enqueue([&ExecutionOrder] { ExecutionOrder.push_back(1); });
		Operations.Enqueue([&ExecutionOrder, &Operations]
		{
			ExecutionOrder.push_back(2);
			Operations.Enqueue([&ExecutionOrder] { ExecutionOrder.push_back(4); });
		});

		PF_CHECK(Tests, ExecutionOrder.empty());
		PF_CHECK(Tests, Operations.Size() == 2);
		PF_CHECK(Tests, Operations.ExecuteNext());
		PF_CHECK(Tests, ExecutionOrder == std::vector<int>{ 1 });
		PF_CHECK(Tests, Operations.ExecuteNext());
		PF_CHECK(Tests, ExecutionOrder == (std::vector<int>{ 1, 2 }));
		PF_CHECK(Tests, Operations.Size() == 1);
		PF_CHECK(Tests, Operations.ExecuteNext());
		PF_CHECK(Tests, ExecutionOrder == (std::vector<int>{ 1, 2, 4 }));
		PF_CHECK(Tests, !Operations.ExecuteNext());
		Operations.Enqueue([] {});
		Operations.Clear();
		PF_CHECK(Tests, Operations.Size() == 0);
		PF_CHECK(Tests, !Operations.ExecuteNext());
	}

	void TestEditorAssetDestinationPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const std::filesystem::path Source = "Assets/Materials/Amber.material";
		const auto Extensionless = ResolveAssetDestinationFilename("Amber Copy", Source);
		PF_CHECK(Tests, Extensionless && *Extensionless == "Amber Copy.material");
		const auto MatchingExtension = ResolveAssetDestinationFilename("Amber-Renamed.MATERIAL", Source);
		PF_CHECK(Tests, MatchingExtension && *MatchingExtension == "Amber-Renamed.MATERIAL");
		const auto EmptyName = ResolveAssetDestinationFilename("", Source);
		PF_CHECK(Tests, !EmptyName && EmptyName.error() == AssetDestinationNameError::InvalidFilename);
		const auto RelativePath = ResolveAssetDestinationFilename("nested/Amber.material", Source);
		PF_CHECK(Tests, !RelativePath && RelativePath.error() == AssetDestinationNameError::InvalidFilename);
		const auto InvalidCharacter = ResolveAssetDestinationFilename("Amber?.material", Source);
		PF_CHECK(Tests, !InvalidCharacter && InvalidCharacter.error() == AssetDestinationNameError::InvalidFilename);
		const auto TrailingSpace = ResolveAssetDestinationFilename("Amber .material ", Source);
		PF_CHECK(Tests, !TrailingSpace && TrailingSpace.error() == AssetDestinationNameError::InvalidFilename);
		const auto MismatchedExtension = ResolveAssetDestinationFilename("Amber.png", Source);
		PF_CHECK(Tests, !MismatchedExtension && MismatchedExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		const auto ReservedExtension = ResolveAssetDestinationFilename("Amber.meta", Source);
		PF_CHECK(Tests, !ReservedExtension &&
			ReservedExtension.error() == AssetDestinationNameError::ReservedSidecarExtension);
		const auto DestinationBreadcrumbs = BuildAssetDestinationBreadcrumbs("Assets/Materials/Props");
		PF_CHECK(Tests, DestinationBreadcrumbs.size() == 3);
		PF_CHECK(Tests, DestinationBreadcrumbs[0] == std::filesystem::path("Assets"));
		PF_CHECK(Tests, DestinationBreadcrumbs[1] == std::filesystem::path("Assets/Materials"));
		PF_CHECK(Tests, DestinationBreadcrumbs[2] == std::filesystem::path("Assets/Materials/Props"));
		const auto RootBreadcrumb = BuildAssetDestinationBreadcrumbs("Assets");
		PF_CHECK(Tests, RootBreadcrumb.size() == 1 && RootBreadcrumb.front() == std::filesystem::path("Assets"));
		const auto NewSceneName = ResolveAssetDestinationFilename(
			"Overview", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, NewSceneName && *NewSceneName == "Overview.scene");
		const auto SavedSceneName = ResolveAssetDestinationFilename(
			"Overview Copy.scene", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, SavedSceneName && *SavedSceneName == "Overview Copy.scene");
		const auto NewPrefabName = ResolveAssetDestinationFilename(
			"Environment", std::filesystem::path("Assets/Prefabs/NewPrefab.prefab"));
		PF_CHECK(Tests, NewPrefabName && *NewPrefabName == "Environment.prefab");
		const auto WrongSceneExtension = ResolveAssetDestinationFilename(
			"Overview.prefab", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, !WrongSceneExtension &&
			WrongSceneExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		const auto WrongPrefabExtension = ResolveAssetDestinationFilename(
			"Environment.scene", std::filesystem::path("Assets/Prefabs/NewPrefab.prefab"));
		PF_CHECK(Tests, !WrongPrefabExtension &&
			WrongPrefabExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, false, false, false, false, false) ==
			AssetDestinationConflict::None);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, false, true, true, false) ==
			AssetDestinationConflict::SamePath);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, false, false, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(true, false, true, false, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, false, true, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, true, true, false, false, true) ==
			AssetDestinationConflict::CaseOnlyRename);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, true, true, false, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, false, false, false, false, true) ==
			AssetDestinationConflict::InspectionFailed);
	}

	void TestEditorAssetSelectorPolicy(TestRunner& Tests)
	{
		using PulseForgeEditor::AssetSelectorMatchesSearch;
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", ""));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "stone"));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "TEXTURES"));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Materials/青い.material", "青い"));
		PF_CHECK(Tests, !AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "roughness"));
		PF_CHECK(Tests, !AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "Assets/Models"));
	}

	void TestEditorLayoutOpeningPreferencePersistence(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		PF_CHECK(Tests, ShouldBuildDefaultDockspace(false));
		PF_CHECK(Tests, !ShouldBuildDefaultDockspace(true));

		const auto TestID = PulseForge::UUID::Generate();
		PF_CHECK(Tests, TestID.has_value());
		if (!TestID)
			return;

		std::error_code Error;
		const std::filesystem::path SettingsDirectory = std::filesystem::temp_directory_path() /
			("PulseForgeEditorLayoutPreference_" + TestID->ToString());
		const bool DirectoryCreated = std::filesystem::create_directory(SettingsDirectory, Error);
		PF_CHECK(Tests, DirectoryCreated && !Error);
		if (!DirectoryCreated || Error)
			return;
		struct ScopedDirectoryCleanup
		{
			std::filesystem::path Path;
			~ScopedDirectoryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Path, CleanupError);
			}
		} Cleanup{ SettingsDirectory };

		const wchar_t* ExistingSettingsDirectory = _wgetenv(L"PULSEFORGE_EDITOR_SETTINGS_DIR");
		const std::optional<std::wstring> PreviousSettingsDirectory = ExistingSettingsDirectory
			? std::optional<std::wstring>(ExistingSettingsDirectory) : std::nullopt;
		const std::wstring SettingsDirectoryValue = SettingsDirectory.native();
		const errno_t EnvironmentResult = _wputenv_s(
			L"PULSEFORGE_EDITOR_SETTINGS_DIR", SettingsDirectoryValue.c_str());
		PF_CHECK(Tests, EnvironmentResult == 0);
		if (EnvironmentResult != 0)
			return;
		struct ScopedSettingsEnvironment
		{
			std::optional<std::wstring> PreviousValue;
			~ScopedSettingsEnvironment()
			{
				(void)_wputenv_s(L"PULSEFORGE_EDITOR_SETTINGS_DIR",
					PreviousValue ? PreviousValue->c_str() : L"");
			}
		} EnvironmentCleanup{ PreviousSettingsDirectory };

		struct ScopedImGuiContext
		{
			ImGuiContext* Previous = ImGui::GetCurrentContext();
			ImGuiContext* Context = ImGui::CreateContext();
			~ScopedImGuiContext()
			{
				if (Context)
					ImGui::DestroyContext(Context);
				ImGui::SetCurrentContext(Previous);
			}
		};

		const std::filesystem::path SettingsFile = SettingsDirectory / L"imgui.ini";
		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (!Initialized)
				return;
			PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == DefaultDocumentOpeningPreference);
			PF_CHECK(Tests, Layout.SetDocumentOpeningPreference(2));
			const auto Saved = Layout.SaveNow();
			PF_CHECK(Tests, Saved.has_value());
		}

		std::ifstream SavedSettingsInput(SettingsFile, std::ios::binary);
		const std::string SavedSettings(
			(std::istreambuf_iterator<char>(SavedSettingsInput)), std::istreambuf_iterator<char>());
		PF_CHECK(Tests, SavedSettings.find("DocumentOpeningPreference=2") != std::string::npos);

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (Initialized)
				PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == 2);
		}

		{
			std::ofstream IncompleteSettingsOutput(SettingsFile, std::ios::binary | std::ios::trunc);
			IncompleteSettingsOutput << "[PulseForgeWorkspace][State]\n"
				"StateFormat=1\nDocumentOpeningPreference=1\nHierarchy=0\n";
			PF_CHECK(Tests, static_cast<bool>(IncompleteSettingsOutput));
		}

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (!Initialized)
				return;
			PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == 1);
			PF_CHECK(Tests, !Layout.IsPanelVisible(EditorPanel::Hierarchy));
		}

		{
			std::ofstream InvalidSettingsOutput(SettingsFile, std::ios::binary | std::ios::trunc);
			InvalidSettingsOutput << "[PulseForgeWorkspace][State]\n"
				"StateFormat=99\nDocumentOpeningPreference=1\nHierarchy=0\n";
			PF_CHECK(Tests, static_cast<bool>(InvalidSettingsOutput));
		}

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (Initialized)
			{
				PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == DefaultDocumentOpeningPreference);
				PF_CHECK(Tests, Layout.IsPanelVisible(EditorPanel::Hierarchy));
			}
		}
	}

	void TestEditorWorkspaceViewHelpers(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", "cube"));
		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", "SAMPLE"));
		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", ""));
		PF_CHECK(Tests, !ContainsCaseInsensitive("Sample Cube", "sphere"));
		PF_CHECK(Tests, !ContainsCaseInsensitive("abc", "abcd"));

		PF_CHECK(Tests, IsManagedAssetInFolder("Assets/Readme.txt", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Props/Cube.glb", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Props/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, IsManagedAssetInFolder("Assets/Models/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Cube.glb", "Assets/Materials"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/ModelsExtra/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Outside/Cube.glb", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/../Outside/Cube.glb", "Assets"));
		PF_CHECK(Tests, IsImmediateChildFolder("Assets/Models", "Assets"));
		PF_CHECK(Tests, IsImmediateChildFolder("Assets/Models/Props", "Assets/Models"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/Models/Props", "Assets"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/ModelsExtra", "Assets/Models"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/../Outside", "Assets"));
		PF_CHECK(Tests, IsManagedAssetInFolderSubtree("Assets/Models/Cube.glb", "Assets"));
		PF_CHECK(Tests, IsManagedAssetInFolderSubtree("Assets/Models/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolderSubtree("Assets/ModelsExtra/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "cube"));
		PF_CHECK(Tests, MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "models/cube"));
		PF_CHECK(Tests, !MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets/Materials", "cube"));
		PF_CHECK(Tests, !MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "missing"));

		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Scenes/Main.scene") == EditorIcon::Scene);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Models/Cube.GLB") == EditorIcon::Cube);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Materials/Default.material") == EditorIcon::Material);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Scripts/Move.lua") == EditorIcon::Script);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Textures/Icon.png") == EditorIcon::Image);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Audio/Click.wav") == EditorIcon::AudioSource);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Other/Readme.txt") == EditorIcon::File);

		PF_CHECK(Tests, IsFolderAncestorOrSelf("Assets/Models/Props", "Assets/Models"));
		PF_CHECK(Tests, IsFolderAncestorOrSelf("Assets/Models", "Assets/Models"));
		PF_CHECK(Tests, !IsFolderAncestorOrSelf("Assets/ModelsExtra", "Assets/Models"));

		const auto FrontFacingAxes = ProjectViewportOrientationAxes({ 0.0f, 0.0f, -1.0f });
		PF_CHECK(Tests, FrontFacingAxes.has_value());
		if (FrontFacingAxes)
		{
			PF_CHECK(Tests, glm::length((*FrontFacingAxes)[0].ScreenDirection - glm::vec2(1.0f, 0.0f)) < 1.0e-5f);
			PF_CHECK(Tests, glm::length((*FrontFacingAxes)[1].ScreenDirection - glm::vec2(0.0f, -1.0f)) < 1.0e-5f);
			PF_CHECK(Tests, std::abs((*FrontFacingAxes)[2].Depth + 1.0f) < 1.0e-5f);
		}
		const auto RotatedAxes = ProjectViewportOrientationAxes(glm::normalize(glm::vec3(1.0f, 1.0f, -1.0f)));
		PF_CHECK(Tests, RotatedAxes.has_value());
		if (RotatedAxes)
			PF_CHECK(Tests, glm::length((*RotatedAxes)[2].ScreenDirection) > 0.1f);
		PF_CHECK(Tests, !ProjectViewportOrientationAxes(glm::vec3(0.0f)));
	}

	void TestViewportMouseCaptureState(TestRunner& Tests)
	{
		PulseForgeEditor::Detail::ViewportMouseCaptureState Capture;
		PF_CHECK(Tests, !Capture.OwnsMouse());
		PF_CHECK(Tests, !Capture.Begin(true, false, true, false, false)); // RMB outside the viewport image.
		PF_CHECK(Tests, !Capture.Begin(true, true, false, false, false)); // Unfocused window.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, true, false)); // Gizmo drag owns the pointer.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, false, true)); // Runtime camera is active.
		PF_CHECK(Tests, Capture.Begin(true, true, true, false, false));
		PF_CHECK(Tests, Capture.OwnsMouse()); // Capture owns mouse routing until release/cancellation.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, false, false)); // No duplicate capture transition.
		Capture.End(); // Same transition is used for RMB release, focus loss, and layout/project changes.
		PF_CHECK(Tests, !Capture.OwnsMouse());
		PF_CHECK(Tests, Capture.Begin(true, true, true, false, false));
		Capture.End();
		PF_CHECK(Tests, !Capture.OwnsMouse());
	}

	void TestEditorImGuiRendererMath(TestRunner& Tests)
	{
		using namespace PulseForgeEditor::ImGuiRendererMath;
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Position) == 0);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Color) == 1);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::TexCoord) == 2);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Normal) == 3);

		const auto ClippedToOldSwapchain = PulseForge::IntersectScissorRect({ 0, 0, 2560, 1361 }, 1280, 720);
		PF_CHECK(Tests, ClippedToOldSwapchain && ClippedToOldSwapchain->X == 0 && ClippedToOldSwapchain->Y == 0 &&
			ClippedToOldSwapchain->Width == 1280 && ClippedToOldSwapchain->Height == 720);
		const auto PartiallyVisible = PulseForge::IntersectScissorRect({ 1000, 600, 500, 300 }, 1280, 720);
		PF_CHECK(Tests, PartiallyVisible && PartiallyVisible->X == 1000 && PartiallyVisible->Y == 600 &&
			PartiallyVisible->Width == 280 && PartiallyVisible->Height == 120);
		PF_CHECK(Tests, !PulseForge::IntersectScissorRect({ 1280, 0, 1, 1 }, 1280, 720));
		PF_CHECK(Tests, !PulseForge::IntersectScissorRect({ 0, 0, 1, 1 }, 0, 720));

		const glm::vec2 DisplayPosition{ 20.0f, 30.0f };
		const glm::vec2 DisplaySize{ 100.0f, 50.0f };
		const auto TopLeft = PositionToVulkanNdc(DisplayPosition, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, TopLeft && glm::length(*TopLeft - glm::vec2(-1.0f, 1.0f)) < 1.0e-6f);
		const auto Center = PositionToVulkanNdc(DisplayPosition + DisplaySize * 0.5f, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, Center && glm::length(*Center) < 1.0e-6f);
		const auto BottomRight = PositionToVulkanNdc(DisplayPosition + DisplaySize, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, BottomRight && glm::length(*BottomRight - glm::vec2(1.0f, -1.0f)) < 1.0e-6f);
		PF_CHECK(Tests, !PositionToVulkanNdc({ 0.0f, 0.0f }, DisplayPosition, { 0.0f, 50.0f }));
		PF_CHECK(Tests, !PositionToVulkanNdc({ std::numeric_limits<float>::infinity(), 0.0f }, DisplayPosition, DisplaySize));

		const auto Extent = CalculateFramebufferExtent(DisplaySize, { 1.5f, 2.0f });
		PF_CHECK(Tests, Extent && Extent->Width == 150 && Extent->Height == 100);
		const auto FractionalExtent = CalculateFramebufferExtent({ 10.1f, 20.2f }, { 1.5f, 1.25f });
		PF_CHECK(Tests, FractionalExtent && FractionalExtent->Width == 16 && FractionalExtent->Height == 26);
		PF_CHECK(Tests, !CalculateFramebufferExtent({ 0.0f, 1.0f }, { 1.0f, 1.0f }));
		PF_CHECK(Tests, !CalculateFramebufferExtent(DisplaySize, { 1.0f, 0.0f }));
		PF_CHECK(Tests, !CalculateFramebufferExtent({ std::numeric_limits<float>::infinity(), 1.0f }, { 1.0f, 1.0f }));
		const auto LargeExtent = CalculateFramebufferExtent({ 100000.0f, 50000.0f }, { 1.0f, 1.0f });
		PF_CHECK(Tests, LargeExtent && LargeExtent->Width == 100000 && LargeExtent->Height == 50000);

		const FramebufferExtent ScissorExtent{ 200, 100 };
		const glm::vec2 Scale{ 2.0f, 2.0f };
		const auto Full = ClipRectToScissor(
			DisplayPosition,
			DisplayPosition + DisplaySize,
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, Full && *Full && (*Full)->X == 0 && (*Full)->Y == 0 &&
			(*Full)->Width == 200 && (*Full)->Height == 100);

		const auto Fractional = ClipRectToScissor(
			DisplayPosition + glm::vec2(1.2f, 2.4f),
			DisplayPosition + glm::vec2(5.1f, 6.2f),
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, Fractional && *Fractional && (*Fractional)->X == 2 && (*Fractional)->Y == 4 &&
			(*Fractional)->Width == 9 && (*Fractional)->Height == 9);

		const auto PartlyOutside = ClipRectToScissor(
			DisplayPosition + glm::vec2(-10.0f, -5.0f),
			DisplayPosition + glm::vec2(5.0f, 8.0f),
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, PartlyOutside && *PartlyOutside && (*PartlyOutside)->X == 0 && (*PartlyOutside)->Y == 0 &&
			(*PartlyOutside)->Width == 10 && (*PartlyOutside)->Height == 16);

		const auto FullyOutside = ClipRectToScissor(
			DisplayPosition + glm::vec2(150.0f, 10.0f),
			DisplayPosition + glm::vec2(160.0f, 20.0f),
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, FullyOutside && !*FullyOutside);
		PF_CHECK(Tests, !ClipRectToScissor({ 0.0f, 0.0f }, { 1.0f, 1.0f }, {}, { 1.0f, 1.0f }, {}));
	}

	void TestEditorPlatformViewportSubmissionOrder(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		std::vector<int> Events;
		for (int ViewportID : { 1, 3 })
		{
			RenderPlatformViewportSequentially(
				[&] { Events.push_back(ViewportID * 10 + 1); },
				[&] { Events.push_back(ViewportID * 10 + 2); },
				[&] { Events.push_back(ViewportID * 10 + 3); },
				[&] { Events.push_back(ViewportID * 10 + 4); });
		}
		const std::vector<int> Expected{ 11, 12, 13, 14, 31, 32, 33, 34 };
		PF_CHECK(Tests, Events == Expected);
	}

	void TestEditorFontAndIconDescriptors(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		int GlfwWindowMarker = 0;
		int NativeWindowMarker = 0;
		const EditorGlfwViewportHandles ViewportHandles{
			.GlfwWindow = &GlfwWindowMarker,
			.NativeWindow = &NativeWindowMarker
		};
		PF_CHECK(Tests, GetGlfwContextWindow(ViewportHandles) == &GlfwWindowMarker);
		PF_CHECK(Tests, GetGlfwContextWindow(ViewportHandles) != &NativeWindowMarker);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(590.0f) == EditorViewportToolbarLayout::Full);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(589.0f) == EditorViewportToolbarLayout::Compact);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(300.0f) == EditorViewportToolbarLayout::Compact);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(299.0f) == EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(-1.0f) == EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(std::numeric_limits<float>::quiet_NaN()) ==
			EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(100.0f) == EditorStyleMetrics::PropertyLabelWidthMinimum);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(300.0f) == 90.0f);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(1000.0f) == EditorStyleMetrics::PropertyLabelWidthMaximum);
		PF_CHECK(Tests, GetMaterialPropertyLayout(260.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(360.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(500.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(559.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(560.0f) == MaterialPropertyLayout::TwoColumn);
		PF_CHECK(Tests, GetMaterialPropertyLayout(700.0f) == MaterialPropertyLayout::TwoColumn);
		PF_CHECK(Tests, GetMaterialPropertyLabelWidth(700.0f) == 264.0f);
		PF_CHECK(Tests, GetMaterialPropertyLabelWidth(1000.0f) == 264.0f);
		PF_CHECK(Tests, GetMaterialPropertyLayout(-1.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(std::numeric_limits<float>::quiet_NaN()) ==
			MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(std::numeric_limits<float>::infinity()) ==
			MaterialPropertyLayout::Stacked);
		const MaterialValuePairLayout NarrowMaterialValuePair = GetMaterialValuePairLayout(260.0f, 8.0f);
		PF_CHECK(Tests, NarrowMaterialValuePair.Stacked && NarrowMaterialValuePair.FirstWidth == 260.0f &&
			NarrowMaterialValuePair.SecondWidth == 260.0f);
		const MaterialValuePairLayout RegularMaterialValuePair = GetMaterialValuePairLayout(360.0f, 8.0f);
		PF_CHECK(Tests, !RegularMaterialValuePair.Stacked && RegularMaterialValuePair.FirstWidth == 176.0f &&
			RegularMaterialValuePair.SecondWidth == 176.0f &&
			RegularMaterialValuePair.FirstWidth + RegularMaterialValuePair.SecondWidth + 8.0f <= 360.0f);
		const MaterialValuePairLayout BrowserWidthMaterialValuePair = GetMaterialValuePairLayout(500.0f, 8.0f);
		PF_CHECK(Tests, !BrowserWidthMaterialValuePair.Stacked && BrowserWidthMaterialValuePair.FirstWidth == 246.0f &&
			BrowserWidthMaterialValuePair.SecondWidth == 246.0f &&
			BrowserWidthMaterialValuePair.FirstWidth + BrowserWidthMaterialValuePair.SecondWidth + 8.0f <= 500.0f);
		const MaterialValuePairLayout WideMaterialValuePair = GetMaterialValuePairLayout(700.0f, 8.0f);
		PF_CHECK(Tests, !WideMaterialValuePair.Stacked && WideMaterialValuePair.FirstWidth == 346.0f &&
			WideMaterialValuePair.SecondWidth == 346.0f);
		const MaterialValuePairLayout InvalidMaterialValuePair = GetMaterialValuePairLayout(
			std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
		PF_CHECK(Tests, InvalidMaterialValuePair.Stacked && InvalidMaterialValuePair.FirstWidth == 0.0f &&
			InvalidMaterialValuePair.SecondWidth == 0.0f);
		for (const MaterialValuePairLayout Layout : { NarrowMaterialValuePair, RegularMaterialValuePair,
			BrowserWidthMaterialValuePair, WideMaterialValuePair, InvalidMaterialValuePair })
		{
			PF_CHECK(Tests, std::isfinite(Layout.FirstWidth) && Layout.FirstWidth >= 0.0f);
			PF_CHECK(Tests, std::isfinite(Layout.SecondWidth) && Layout.SecondWidth >= 0.0f);
		}
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarHeight == 36.0f);
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarPaddingX == 7.0f);
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarPaddingY == 4.0f);
		PF_CHECK(Tests, UseCompactVector3PropertyInput(229.0f));
		PF_CHECK(Tests, !UseCompactVector3PropertyInput(230.0f));
		PF_CHECK(Tests, UseCompactVector3PropertyInput(std::numeric_limits<float>::quiet_NaN()));
		const ContentBrowserVerticalLayout NoSelection = GetContentBrowserVerticalLayout(320.0f, false, true);
		PF_CHECK(Tests, NoSelection.AssetItemsHeight == 320.0f && NoSelection.DetailsHeight == 0.0f);
		const ContentBrowserVerticalLayout CompactSelection = GetContentBrowserVerticalLayout(120.0f, true, true);
		PF_CHECK(Tests, CompactSelection.SelectionSummaryHeight ==
			ContentBrowserLayoutMetrics::CompactSelectionSummaryHeight);
		PF_CHECK(Tests, CompactSelection.AssetItemsHeight == 92.0f && CompactSelection.DetailsHeight == 0.0f);
		const ContentBrowserVerticalLayout ExpandedCompactSelection =
			GetContentBrowserVerticalLayout(120.0f, true, true, true);
		PF_CHECK(Tests, ExpandedCompactSelection.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, ExpandedCompactSelection.DetailsHeight == 0.0f && ExpandedCompactSelection.AssetItemsHeight == 92.0f);
		const ContentBrowserVerticalLayout RegularAssetDetails = GetContentBrowserVerticalLayout(320.0f, true, false);
		PF_CHECK(Tests, RegularAssetDetails.DetailsHeight == ContentBrowserLayoutMetrics::RegularDetailsMaximumHeight);
		PF_CHECK(Tests, RegularAssetDetails.AssetItemsHeight >= ContentBrowserLayoutMetrics::MinimumAssetItemsHeight);
		const ContentBrowserVerticalLayout ShortMaterialSummary = GetContentBrowserVerticalLayout(180.0f, true, true);
		PF_CHECK(Tests, ShortMaterialSummary.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, ShortMaterialSummary.DetailsHeight == 0.0f && ShortMaterialSummary.AssetItemsHeight == 152.0f);
		const ContentBrowserVerticalLayout TallMaterialSummary = GetContentBrowserVerticalLayout(840.0f, true, true);
		PF_CHECK(Tests, TallMaterialSummary.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, TallMaterialSummary.DetailsHeight == 0.0f && TallMaterialSummary.AssetItemsHeight == 812.0f);
		const ContentBrowserVerticalLayout InvalidContentHeight = GetContentBrowserVerticalLayout(
			std::numeric_limits<float>::quiet_NaN(), true, true);
		PF_CHECK(Tests, InvalidContentHeight.AssetItemsHeight == 0.0f && InvalidContentHeight.SelectionSummaryHeight == 0.0f);
		PF_CHECK(Tests, GetEditorEntityIcon({}) == EditorIcon::Entity);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasCamera = true, .HasMesh = true }) == EditorIcon::Camera);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasMesh = true, .HasPointLight = true }) == EditorIcon::Cube);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasSpotLight = true, .HasPointLight = true }) == EditorIcon::SpotLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasPointLight = true, .HasDirectionalLight = true }) == EditorIcon::PointLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasDirectionalLight = true }) == EditorIcon::DirectionalLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasEnvironmentLight = true }) == EditorIcon::Environment);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasAudioSource = true, .HasAudioListener = true }) == EditorIcon::AudioSource);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasAudioListener = true }) == EditorIcon::AudioListener);
		PF_CHECK(Tests, EditorFontDescriptors.size() == 3);
		PF_CHECK(Tests, EditorFontDescriptors[0].Role == EditorFontRole::Interface);
		PF_CHECK(Tests, EditorFontDescriptors[1].Role == EditorFontRole::Emphasis);
		PF_CHECK(Tests, EditorFontDescriptors[2].Role == EditorFontRole::Monospace);
		PF_CHECK(Tests, EditorFontDescriptors[0].RelativePath == "Fonts/Inter-Regular.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[1].RelativePath == "Fonts/Inter-SemiBold.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[2].RelativePath == "Fonts/JetBrainsMono-Regular.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[0].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorFontDescriptors[1].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorFontDescriptors[2].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorIconFontPixelSize == EditorFontDescriptors[0].PixelSize);
		PF_CHECK(Tests, IsValidEditorTextGlyphRanges());
		PF_CHECK(Tests, EditorTextGlyphRanges.back() == 0);
		for (size_t Index = 0; Index + 1 < EditorTextGlyphRanges.size() - 1; Index += 2)
		{
			PF_CHECK(Tests, EditorTextGlyphRanges[Index] <= EditorTextGlyphRanges[Index + 1]);
			if (Index > 0)
				PF_CHECK(Tests, EditorTextGlyphRanges[Index - 1] < EditorTextGlyphRanges[Index]);
		}
		for (const uint32_t Codepoint : {
			0x011e, 0x011f, 0x0130, 0x0131, 0x015e, 0x015f, 0x010c, 0x0141, 0x017d,
			0x0150, 0x00c5, 0x00e9, 0x00df, 0x20ac, 0x03a9, 0x0416, 0x1e0a, 0x2014 })
			PF_CHECK(Tests, EditorTextGlyphRangeContains(Codepoint));
		PF_CHECK(Tests, !EditorTextGlyphRangeContains(0x4e2d));
		PF_CHECK(Tests, EditorFontRoleHasIcons(EditorFontRole::Interface));
		PF_CHECK(Tests, EditorFontRoleHasIcons(EditorFontRole::Emphasis));
		PF_CHECK(Tests, !EditorFontRoleHasIcons(EditorFontRole::Monospace));

		const std::string_view TwoByteText = "\xc4\x9eX";
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 1) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 2) == 2);
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 3) == 3);
		const std::string_view ThreeByteText = "\xe2\x82\xacX";
		PF_CHECK(Tests, Utf8SafePrefixLength(ThreeByteText, 2) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(ThreeByteText, 3) == 3);
		const std::string_view FourByteText = "\xf0\x9f\x98\x80X";
		PF_CHECK(Tests, Utf8SafePrefixLength(FourByteText, 3) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(FourByteText, 4) == 4);

		for (const EditorColorValue& Color : EditorPalette)
		{
			PF_CHECK(Tests, Color.Red >= 0.0f && Color.Red <= 1.0f);
			PF_CHECK(Tests, Color.Green >= 0.0f && Color.Green <= 1.0f);
			PF_CHECK(Tests, Color.Blue >= 0.0f && Color.Blue <= 1.0f);
			PF_CHECK(Tests, Color.Alpha >= 0.0f && Color.Alpha <= 1.0f);
		}
		constexpr std::array<EditorColorToken, 4> GizmoColorTokens = {
			EditorColorToken::GizmoAxisX,
			EditorColorToken::GizmoAxisY,
			EditorColorToken::GizmoAxisZ,
			EditorColorToken::GizmoHighlight
		};
		for (const EditorColorToken Token : GizmoColorTokens)
		{
			const EditorColorValue Color = GetEditorColorValue(Token);
			const EditorColorValue SrgbOutput = ConvertEditorColorForOutput(
				Color, PulseForge::OutputColorEncoding::SrgbAttachment);
			const EditorColorValue UnormOutput = ConvertEditorColorForOutput(
				Color, PulseForge::OutputColorEncoding::UnormAttachment);
			PF_CHECK(Tests, SrgbOutput.Alpha == Color.Alpha);
			PF_CHECK(Tests, UnormOutput.Red == Color.Red && UnormOutput.Green == Color.Green &&
				UnormOutput.Blue == Color.Blue && UnormOutput.Alpha == Color.Alpha);
		}

		const EditorColorValue DisplayColor{ 0.5f, 0.04045f, 1.0f, 0.37f };
		const EditorColorValue LinearColor = ConvertEditorColorForOutput(
			DisplayColor, PulseForge::OutputColorEncoding::SrgbAttachment);
		PF_CHECK(Tests, std::abs(LinearColor.Red - 0.21404114f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(LinearColor.Green - 0.003130805f) < 1.0e-7f);
		PF_CHECK(Tests, LinearColor.Blue == 1.0f);
		PF_CHECK(Tests, LinearColor.Alpha == DisplayColor.Alpha);
		const EditorColorValue UnormColor = ConvertEditorColorForOutput(
			DisplayColor, PulseForge::OutputColorEncoding::UnormAttachment);
		PF_CHECK(Tests, UnormColor.Red == DisplayColor.Red && UnormColor.Green == DisplayColor.Green &&
			UnormColor.Blue == DisplayColor.Blue && UnormColor.Alpha == DisplayColor.Alpha);
		PF_CHECK(Tests, SelectEditorUiCompositionPath(PulseForge::OutputColorEncoding::SrgbAttachment) ==
			EditorUiCompositionPath::DirectSrgbSwapchain);
		PF_CHECK(Tests, SelectEditorUiCompositionPath(PulseForge::OutputColorEncoding::UnormAttachment) ==
			EditorUiCompositionPath::SrgbIntermediateToUnormSwapchain);
		PF_CHECK(Tests, !SelectEditorUiCompositionPath(static_cast<PulseForge::OutputColorEncoding>(255)));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(false, 1280, 720, 1280, 720));
		PF_CHECK(Tests, IsEditorUiCompositionTargetReusable(true, 1280, 720, 1280, 720));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(true, 1280, 720, 1280, 721));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(true, 1280, 720, 0, 720));
		PF_CHECK(Tests, LinearToSrgbForEditorUi(0.0f) == 0.0f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(1.0f) == 1.0f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(0.5f) - 0.7353569f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(0.0031308f) - 0.0404499f) < 1.0e-6f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(-1.0f) == 0.0f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(2.0f) == 1.0f);

		const float WhiteOverBlackLinear = BlendSourceOverLinear(1.0f, 0.0f, 0.5f);
		PF_CHECK(Tests, WhiteOverBlackLinear == 0.5f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(WhiteOverBlackLinear) - 0.7353569f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(0.5f - LinearToSrgbForEditorUi(WhiteOverBlackLinear)) > 0.2f);
		PF_CHECK(Tests, BlendSourceOverLinear(1.0f, 0.25f, 0.0f) == 0.25f);
		PF_CHECK(Tests, BlendSourceOverLinear(0.25f, 0.75f, 1.0f) == 0.25f);
		PF_CHECK(Tests, std::abs(BlendSourceOverLinear(0.8f, 0.2f, 0.25f) - 0.35f) < 1.0e-6f);
		const float FirstLayer = BlendSourceOverLinear(0.8f, 0.1f, 0.5f);
		const float OrderedLayers = BlendSourceOverLinear(0.2f, FirstLayer, 0.25f);
		PF_CHECK(Tests, std::abs(OrderedLayers - 0.3875f) < 1.0e-6f);

		std::array<bool, static_cast<size_t>(EditorIcon::Count)> SeenIcons{};
		for (size_t Index = 0; Index < EditorIconDescriptors.size(); ++Index)
		{
			const EditorIconDescriptor& Descriptor = EditorIconDescriptors[Index];
			const size_t IconIndex = static_cast<size_t>(Descriptor.Icon);
			PF_CHECK(Tests, IconIndex < SeenIcons.size());
			if (IconIndex < SeenIcons.size())
			{
				PF_CHECK(Tests, !SeenIcons[IconIndex]);
				SeenIcons[IconIndex] = true;
			}
			PF_CHECK(Tests, !Descriptor.Name.empty());
			PF_CHECK(Tests, Descriptor.Codepoint >= 0xe000 && Descriptor.Codepoint <= 0xf8ff);
			PF_CHECK(Tests, EditorIconGlyphRanges[Index * 2] == Descriptor.Codepoint);
			PF_CHECK(Tests, EditorIconGlyphRanges[Index * 2 + 1] == Descriptor.Codepoint);
			if (Index > 0)
				PF_CHECK(Tests, EditorIconDescriptors[Index - 1].Codepoint < Descriptor.Codepoint);
			PF_CHECK(Tests, FindEditorIcon(Descriptor.Icon) == &Descriptor);
		}
		PF_CHECK(Tests, std::all_of(SeenIcons.begin(), SeenIcons.end(), [](bool Seen) { return Seen; }));
		PF_CHECK(Tests, EditorIconGlyphRanges.back() == 0);
		PF_CHECK(Tests, FindEditorIcon(EditorIcon::Count) == nullptr);
		const EditorIconDescriptor* SelectIcon = FindEditorIcon(EditorIcon::Select);
		PF_CHECK(Tests, SelectIcon && SelectIcon->Codepoint == 0xf245);
	}

	void TestEditorViewportCameraController(TestRunner& Tests)
	{
		PulseForgeEditor::EditorViewportCamera Camera;
		PF_CHECK(Tests, glm::all(glm::equal(Camera.GetPosition(), glm::vec3(0.0f, 0.0f, 3.1f))));
		PF_CHECK(Tests, Camera.GetViewProjection(16.0f / 9.0f).has_value());

		Camera.Focus(glm::vec3(1.0f, 2.0f, -3.0f), 2.0f, 16.0f / 9.0f);
		const glm::vec3 FocusedPosition = Camera.GetPosition();
		PF_CHECK(Tests, glm::length(FocusedPosition - glm::vec3(1.0f, 2.0f, -3.0f)) > 0.001f);
		PF_CHECK(Tests, std::isfinite(glm::length(FocusedPosition - glm::vec3(1.0f, 2.0f, -3.0f))));

		Camera.Orbit(15.0f, -8.0f);
		const glm::vec3 OrbitPosition = Camera.GetPosition();
		PF_CHECK(Tests, glm::length(OrbitPosition - FocusedPosition) > 0.001f);
		Camera.Pan(12.0f, -7.0f, 720.0f);
		const glm::vec3 PanPosition = Camera.GetPosition();
		PF_CHECK(Tests, glm::length(PanPosition - OrbitPosition) > 0.001f);
		Camera.Dolly(0.5f);
		const glm::vec3 DollyPosition = Camera.GetPosition();
		PF_CHECK(Tests, glm::length(DollyPosition - PanPosition) > 0.001f);

		const PulseForgeEditor::ViewportCameraMoveKeys MoveKeys{ .Forward = true };
		Camera.Move(MoveKeys, 0.1);
		PF_CHECK(Tests, glm::length(Camera.GetPosition() - DollyPosition) > 0.001f);

		const glm::vec3 BeforeInvalidFocus = Camera.GetPosition();
		Camera.Focus(glm::vec3(0.0f), std::numeric_limits<float>::quiet_NaN(), 1.0f);
		Camera.Focus(glm::vec3(0.0f), 1.0f, 0.0f);
		PF_CHECK(Tests, glm::all(glm::equal(Camera.GetPosition(), BeforeInvalidFocus)));

		Camera.Reset();
		PF_CHECK(Tests, glm::all(glm::equal(Camera.GetPosition(), glm::vec3(0.0f, 0.0f, 3.1f))));
		PF_CHECK(Tests, glm::length(Camera.GetForward() - glm::vec3(0.0f, 0.0f, -1.0f)) < 0.0001f);
	}

	void TestEditorConsoleHistory(TestRunner& Tests)
	{
		PulseForgeEditor::EditorConsoleHistory History;
		std::vector<PulseForgeEditor::EditorConsoleMessage> Messages;
		uint64_t Revision = 0;
		History.Append("APP", PulseForgeEditor::EditorConsoleSeverity::Info, "first\r\nmessage");
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.size() == 1);
		PF_CHECK(Tests, Messages[0].Sequence == 1);
		PF_CHECK(Tests, Messages[0].Logger == "APP");
		PF_CHECK(Tests, Messages[0].Severity == PulseForgeEditor::EditorConsoleSeverity::Info);
		PF_CHECK(Tests, Messages[0].Text == "first  message");
		PF_CHECK(Tests, !History.CopyMessagesIfChanged(Revision, Messages, Revision));

		const std::string Oversized(PulseForgeEditor::EditorConsoleHistory::MaxMessageBytes + 32, 'x');
		History.Append("PULSEFORGE", PulseForgeEditor::EditorConsoleSeverity::Error, Oversized);
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.back().Text.size() == PulseForgeEditor::EditorConsoleHistory::MaxMessageBytes + 12);
		PF_CHECK(Tests, Messages.back().Text.ends_with(" [truncated]"));

		for (size_t Index = 0; Index < PulseForgeEditor::EditorConsoleHistory::Capacity + 1; ++Index)
			History.Append("TEST", PulseForgeEditor::EditorConsoleSeverity::Debug, "entry");
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.size() == PulseForgeEditor::EditorConsoleHistory::Capacity);
		PF_CHECK(Tests, Messages.front().Sequence == 4);
		PF_CHECK(Tests, Messages.back().Sequence == PulseForgeEditor::EditorConsoleHistory::Capacity + 3);

		History.Clear();
		PF_CHECK(Tests, History.CopyMessagesIfChanged(Revision, Messages, Revision));
		PF_CHECK(Tests, Messages.empty());
	}

	void TestEditorContentBrowserState(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		EditorContentBrowserState Browser;
		Browser.SetAssets({
			{ AssetID{ 0, 1 }, "Assets/Textures/Sky/blue.png" },
			{ AssetID{ 0, 2 }, "Assets/Textures/red.png" },
			{ AssetID{ 0, 3 }, "Assets/Materials/metal.material" },
			{ AssetID{ 0, 4 }, "Outside/loose.scene" }
		});

		const EditorContentFolderNode& Root = Browser.FolderRoot();
		PF_CHECK(Tests, Root.Name == "Assets");
		PF_CHECK(Tests, Root.Children.size() == 2);
		PF_CHECK(Tests, Root.Children[0].Name == "Materials");
		PF_CHECK(Tests, Root.Children[1].Name == "Textures");
		PF_CHECK(Tests, Browser.FindFolder("Assets/Textures/Sky") != nullptr);
		PF_CHECK(Tests, !Browser.IsKnownFolder("Assets/Missing"));
		PF_CHECK(Tests, Browser.Assets().size() == 4);

		Browser.RebuildFilteredAssetIndices();
		PF_CHECK(Tests, Browser.FilteredAssetIndices().empty());

		Browser.SetCurrentFolder("Assets/Textures");
		Browser.RebuildFilteredAssetIndices();
		PF_CHECK(Tests, Browser.FilteredAssetIndices().size() == 1);
		PF_CHECK(Tests, Browser.Assets()[Browser.FilteredAssetIndices()[0]].ProjectRelativePath ==
			"Assets/Textures/red.png");

		Browser.SearchText() = "BLUE";
		Browser.RebuildFilteredAssetIndices();
		PF_CHECK(Tests, Browser.FilteredAssetIndices().size() == 1);
		PF_CHECK(Tests, Browser.Assets()[Browser.FilteredAssetIndices()[0]].ID == AssetID(0, 1));

		Browser.SelectedFolder() = std::filesystem::path("Assets/Textures/Sky");
		Browser.RebaseFoldersAfterMove("Assets/Textures", "Assets/Art/Textures");
		PF_CHECK(Tests, Browser.CurrentFolder() == "Assets/Art/Textures");
		PF_CHECK(Tests, Browser.SelectedFolder() &&
			*Browser.SelectedFolder() == "Assets/Art/Textures/Sky");

		Browser.SetAssets({ { AssetID{ 0, 3 }, "Assets/Materials/metal.material" } });
		PF_CHECK(Tests, Browser.CurrentFolder() == "Assets");
		PF_CHECK(Tests, !Browser.SelectedFolder());
		Browser.GridView() = false;
		Browser.ResetNavigation();
		PF_CHECK(Tests, Browser.CurrentFolder() == "Assets");
		PF_CHECK(Tests, Browser.SearchText().empty());
		PF_CHECK(Tests, Browser.IsGridView());

		Browser.SelectedAsset() = AssetID{ 0, 3 };
		Browser.ClearProject();
		PF_CHECK(Tests, Browser.Assets().empty());
		PF_CHECK(Tests, !Browser.SelectedAsset());
		PF_CHECK(Tests, Browser.FolderRoot().Children.empty());
	}

	void TestEditorAssetWorkflowState(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		EditorAssetWorkflowState Workflow;
		const AssetRecord Material{ AssetID{ 2, 1 }, "Assets/Materials/Rough Clay.material" };

		Workflow.BeginAssetDestination(Material, false);
		PF_CHECK(Tests, Workflow.TakeDestinationDialogRequest());
		PF_CHECK(Tests, !Workflow.TakeDestinationDialogRequest());
		PF_CHECK(Tests, Workflow.DestinationAsset() == Material.ID);
		PF_CHECK(Tests, !Workflow.IsDuplicate());
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::None);
		PF_CHECK(Tests, Workflow.DestinationFolder() == "Assets/Materials");
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "Rough Clay.material");
		Workflow.ValidationError() = "invalid destination";

		Workflow.BeginAssetDestination(Material, true);
		PF_CHECK(Tests, Workflow.TakeDestinationDialogRequest());
		PF_CHECK(Tests, Workflow.IsDuplicate());
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "Rough Clay Copy.material");
		PF_CHECK(Tests, Workflow.ValidationError().empty());

		Workflow.BeginSceneDestination(false, std::nullopt);
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::NewScene);
		PF_CHECK(Tests, Workflow.DestinationFolder() == "Assets");
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "NewScene.scene");

		const AssetRecord Scene{ AssetID{ 2, 2 }, "Assets/Scenes/Overview.scene" };
		Workflow.BeginSceneDestination(true, Scene);
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::SaveSceneAs);
		PF_CHECK(Tests, Workflow.DestinationFolder() == "Assets/Scenes");
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "Overview Copy.scene");

		Workflow.BeginPrefabDestination(UUID{ 9, 4 });
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::Prefab);
		PF_CHECK(Tests, (Workflow.PrefabRoot() == UUID{ 9, 4 }));
		PF_CHECK(Tests, std::string(Workflow.FilenameBuffer().data()) == "NewPrefab.prefab");
		Workflow.DismissDestination();
		PF_CHECK(Tests, !Workflow.DestinationAsset());
		PF_CHECK(Tests, !Workflow.PrefabRoot());
		PF_CHECK(Tests, Workflow.CreateKind() == ProjectAssetDestinationCreateKind::None);
		PF_CHECK(Tests, Workflow.ValidationError().empty());

		Workflow.BeginDelete(Material.ID);
		PF_CHECK(Tests, Workflow.TakeDeleteDialogRequest());
		PF_CHECK(Tests, Workflow.PendingDeleteAsset() == Material.ID);
		PF_CHECK(Tests, !Workflow.TakeDeleteDialogRequest());
		Workflow.DismissDelete();
		PF_CHECK(Tests, !Workflow.PendingDeleteAsset());
	}

	void TestEditorSceneWorkspaceState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const auto First = PulseForge::UUID::Generate();
		const auto Second = PulseForge::UUID::Generate();
		const auto Third = PulseForge::UUID::Generate();
		PF_CHECK(Tests, First.has_value() && Second.has_value() && Third.has_value());
		if (!First || !Second || !Third)
			return;

		EditorSceneWorkspaceState State;
		State.BeginHierarchyFrame();
		State.RecordVisibleHierarchyEntity(*First);
		State.RecordVisibleHierarchyEntity(*Second);
		PF_CHECK(Tests, State.VisibleHierarchyOrder().empty());
		State.CompleteHierarchyFrame();
		PF_CHECK(Tests, State.VisibleHierarchyOrder().size() == 2);
		PF_CHECK(Tests, State.VisibleHierarchyOrder()[0] == *First);
		PF_CHECK(Tests, State.VisibleHierarchyOrder()[1] == *Second);

		State.Selection().SelectSingle(*Second);
		State.BeginHierarchyFrame();
		State.RecordVisibleHierarchyEntity(*Second);
		State.RecordVisibleHierarchyEntity(*Third);
		PF_CHECK(Tests, State.VisibleHierarchyOrder()[0] == *First);
		State.CompleteHierarchyFrame();
		PF_CHECK(Tests, State.Selection().SelectRange(State.VisibleHierarchyOrder(), *Third));
		PF_CHECK(Tests, State.Selection().GetEntities().size() == 2);
		PF_CHECK(Tests, State.Selection().GetEntities()[0] == *Second);
		PF_CHECK(Tests, State.Selection().GetEntities()[1] == *Third);

		State.Visibility().SetVisible(*First, false);
		PF_CHECK(Tests, !State.Visibility().IsLocallyVisible(*First));
		State.Reset();
		PF_CHECK(Tests, State.Selection().GetEntities().empty());
		PF_CHECK(Tests, State.Visibility().IsLocallyVisible(*First));
		PF_CHECK(Tests, State.VisibleHierarchyOrder().empty());
	}

	void TestEditorSceneHistoryState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		PulseForge::Scene SceneValue;
		const auto Original = SceneValue.CreateEntity("Original");
		PF_CHECK(Tests, Original.has_value());
		if (!Original)
			return;

		EditorSceneWorkspaceState Workspace;
		Workspace.Selection().SelectSingle(Original->GetUUID());
		Workspace.Visibility().SetVisible(Original->GetUUID(), false);
		const auto Snapshot = EditorSceneHistoryState::CaptureSnapshot(SceneValue, Workspace);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot)
			return;

		EditorSceneHistoryState History;
		History.Reset(&SceneValue);
		PF_CHECK(Tests, !History.IsDirty());
		const auto Additional = SceneValue.CreateEntity("Additional");
		PF_CHECK(Tests, Additional.has_value());
		if (!Additional)
			return;
		Workspace.Selection().SelectSingle(Additional->GetUUID());
		Workspace.Visibility().SetVisible(Original->GetUUID(), true);
		History.UpdateDirty(&SceneValue);
		PF_CHECK(Tests, History.IsDirty());

		PF_CHECK(Tests, EditorSceneHistoryState::RestoreSnapshot(*Snapshot, SceneValue, Workspace).has_value());
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 1);
		PF_CHECK(Tests, Workspace.Selection().GetPrimary() == Original->GetUUID());
		PF_CHECK(Tests, !Workspace.Visibility().IsLocallyVisible(Original->GetUUID()));

		const auto Invalid = EditorSceneHistoryState::RestoreSnapshot("invalid snapshot", SceneValue, Workspace);
		PF_CHECK(Tests, !Invalid);
		PF_CHECK(Tests, SceneValue.GetEntityCount() == 1);
		PF_CHECK(Tests, Workspace.Selection().GetPrimary() == Original->GetUUID());

		const auto Changed = SceneValue.CreateEntity("Changed");
		PF_CHECK(Tests, Changed.has_value());
		History.UpdateDirty(&SceneValue);
		PF_CHECK(Tests, History.IsDirty());
		History.MarkSaved(&SceneValue);
		PF_CHECK(Tests, !History.IsDirty());
		History.Reset(nullptr);
		PF_CHECK(Tests, !History.IsDirty());
	}
	void TestEditorMaterialDraftStore(TestRunner& Tests)
	{
		PulseForgeEditor::EditorMaterialDraftStore Drafts;
		const std::string FirstKey = "project-a:material-1";
		const std::string SecondKey = "project-a:material-2";
		const std::string OtherProjectKey = "project-b:material-1";
		PulseForge::MaterialAssetDesc FirstMaterial;
		FirstMaterial.BaseColorFactor = { 0.2f, 0.4f, 0.6f, 1.0f };
		PulseForge::MaterialAssetDesc SecondMaterial;
		SecondMaterial.RoughnessFactor = 0.35f;
		Drafts.PutDraft(FirstKey, FirstMaterial);
		Drafts.PutDraft(SecondKey, SecondMaterial);
		PF_CHECK(Tests, Drafts.ContainsDraft(FirstKey));
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));

		PF_CHECK(Tests, Drafts.BeginEdit(FirstKey, FirstMaterial));
		Drafts.FindDraft(FirstKey)->BaseColorFactor.x = 0.8f;
		Drafts.FinalizePendingEdits(true);
		PF_CHECK(Tests, Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, !Drafts.CanUndo(FirstKey));
		Drafts.FinalizePendingEdits(false);
		PF_CHECK(Tests, Drafts.CanUndo(FirstKey));
		PF_CHECK(Tests, Drafts.IsDirty(FirstKey));

		const auto UndoChanged = Drafts.ApplyHistoryStep(FirstKey, false);
		PF_CHECK(Tests, UndoChanged.has_value() && *UndoChanged);
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, Drafts.FindDraft(FirstKey)->BaseColorFactor.x == 0.2f);
		const auto RedoChanged = Drafts.ApplyHistoryStep(FirstKey, true);
		PF_CHECK(Tests, RedoChanged.has_value() && *RedoChanged);
		PF_CHECK(Tests, Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, Drafts.FindDraft(FirstKey)->BaseColorFactor.x == 0.8f);
		Drafts.MarkSaved(FirstKey);
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));

		PulseForge::MaterialAssetDesc Replacement;
		Replacement.BaseColorFactor = { 0.7f, 0.1f, 0.3f, 1.0f };
		Drafts.ReplaceDraft(FirstKey, Replacement);
		PF_CHECK(Tests, !Drafts.IsDirty(FirstKey));
		PF_CHECK(Tests, !Drafts.CanUndo(FirstKey));
		PF_CHECK(Tests, Drafts.FindDraft(FirstKey)->BaseColorFactor.x == 0.7f);

		Drafts.SetError(FirstKey, "load failed");
		PF_CHECK(Tests, Drafts.FindError(FirstKey) && *Drafts.FindError(FirstKey) == "load failed");
		Drafts.ClearError(FirstKey);
		PF_CHECK(Tests, Drafts.FindError(FirstKey) == nullptr);

		Drafts.PutDraft(OtherProjectKey, FirstMaterial);
		PF_CHECK(Tests, Drafts.BeginEdit(OtherProjectKey, FirstMaterial));
		Drafts.FindDraft(OtherProjectKey)->MetallicFactor = 0.9f;
		Drafts.FinalizePendingEdits(false);
		Drafts.ClearProject("project-a:");
		PF_CHECK(Tests, !Drafts.ContainsDraft(FirstKey));
		PF_CHECK(Tests, !Drafts.ContainsDraft(SecondKey));
		PF_CHECK(Tests, Drafts.ContainsDraft(OtherProjectKey));
		PF_CHECK(Tests, Drafts.IsDirty(OtherProjectKey));
	}
}

void RunEditorTests(TestRunner& Tests)
{
	TestEditorViewportMath(Tests);
	TestEditorViewportCameraController(Tests);
	TestEditorConsoleHistory(Tests);
	TestEditorContentBrowserState(Tests);
	TestEditorAssetWorkflowState(Tests);
	TestEditorSceneWorkspaceState(Tests);
	TestEditorSceneHistoryState(Tests);
	TestEditorMaterialDraftStore(Tests);
	TestEditorWorkspacePanelState(Tests);
	TestEditorDeferredOperationQueue(Tests);
	TestEditorAssetDestinationPolicy(Tests);
	TestEditorAssetSelectorPolicy(Tests);
	TestEditorLayoutOpeningPreferencePersistence(Tests);
	TestEditorWorkspaceViewHelpers(Tests);
	TestViewportMouseCaptureState(Tests);
	TestEditorFontAndIconDescriptors(Tests);
	TestEditorImGuiRendererMath(Tests);
	TestEditorPlatformViewportSubmissionOrder(Tests);
}
