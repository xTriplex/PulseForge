#include "TestEditorSupport.h"
#include "EditorViewportCamera.h"
#include "Editor/EditorViewportPolicy.h"
#include "Editor/EditorViewportToolState.h"

#include <chrono>
#include <limits>

namespace
{
	void TestEditorMeshPickingCache(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const std::filesystem::path RepositoryRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
		const std::filesystem::path FirstProjectFile = RepositoryRoot / "PulseForgeGame" / "PulseForgeGame.pfproj";
		auto FirstProject = Project::Open(FirstProjectFile);
		PF_CHECK(Tests, FirstProject.has_value());
		if (!FirstProject) return;

		const auto FindGltfAsset = [](const Project& OpenProject) -> std::optional<AssetRecord>
		{
			const auto Assets = OpenProject.GetAssetRegistry().GetAssets();
			const auto Found = std::find_if(
				Assets.begin(), Assets.end(), [](const AssetRecord& Asset) { return Asset.ProjectRelativePath.extension() == ".gltf"; });
			return Found == Assets.end() ? std::nullopt : std::optional<AssetRecord>(*Found);
		};
		const auto FirstMesh = FindGltfAsset(*FirstProject);
		PF_CHECK(Tests, FirstMesh.has_value());
		if (!FirstMesh) return;

		EditorMeshPickingCache Cache;
		const auto FirstGeometry = Cache.Get(*FirstProject, FirstMesh->ID);
		PF_CHECK(Tests, FirstGeometry.has_value());
		PF_CHECK(Tests, FirstGeometry && !(*FirstGeometry)->Positions.empty() && !(*FirstGeometry)->Indices.empty());
		const auto CachedGeometry = Cache.Get(*FirstProject, FirstMesh->ID);
		PF_CHECK(Tests, CachedGeometry && FirstGeometry && *CachedGeometry == *FirstGeometry);
		PF_CHECK(Tests, Cache.GetGeometryCount() == 1 && Cache.GetFailureCount() == 0);

		const auto FirstProjectAssets = FirstProject->GetAssetRegistry().GetAssets();
		const auto NonMeshAsset = std::find_if(FirstProjectAssets.begin(), FirstProjectAssets.end(),
			[](const AssetRecord& Asset) { return Asset.ProjectRelativePath.extension() == ".material"; });
		PF_CHECK(Tests, NonMeshAsset != FirstProjectAssets.end());
		if (NonMeshAsset != FirstProjectAssets.end())
		{
			const auto FailedLoad = Cache.Get(*FirstProject, NonMeshAsset->ID);
			const auto CachedFailure = Cache.Get(*FirstProject, NonMeshAsset->ID);
			PF_CHECK(Tests, !FailedLoad && !CachedFailure && FailedLoad.error() == CachedFailure.error());
			PF_CHECK(Tests, Cache.GetFailureCount() == 1);
			Cache.Invalidate(NonMeshAsset->ID);
			PF_CHECK(Tests, Cache.GetFailureCount() == 0);
		}

		Cache.Invalidate(FirstMesh->ID);
		PF_CHECK(Tests, Cache.GetGeometryCount() == 0);
		const auto ReloadedGeometry = Cache.Get(*FirstProject, FirstMesh->ID);
		PF_CHECK(Tests, ReloadedGeometry && !(*ReloadedGeometry)->Positions.empty());
		PF_CHECK(Tests, Cache.GetGeometryCount() == 1);

		const std::filesystem::path SecondProjectFile =
			RepositoryRoot / "Samples" / "VisualValidation" / "PointLightShadows" / "PointLightShadows.pfproj";
		auto SecondProject = Project::Open(SecondProjectFile);
		PF_CHECK(Tests, SecondProject.has_value());
		if (!SecondProject) return;
		const auto SecondMesh = FindGltfAsset(*SecondProject);
		PF_CHECK(Tests, SecondMesh.has_value());
		if (!SecondMesh) return;
		const auto SecondGeometry = Cache.Get(*SecondProject, SecondMesh->ID);
		PF_CHECK(Tests, SecondGeometry && !(*SecondGeometry)->Positions.empty());
		PF_CHECK(Tests, Cache.GetGeometryCount() == 1 && Cache.GetFailureCount() == 0);
		Cache.Clear();
		PF_CHECK(Tests, Cache.GetGeometryCount() == 0 && Cache.GetFailureCount() == 0);
	}

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
		PF_CHECK(Tests, ResolveViewportKeyboardFocus(false, ViewportKeyboardFocusEvent::RenderedImageClicked));
		PF_CHECK(Tests, ResolveViewportKeyboardFocus(true, ViewportKeyboardFocusEvent::NoChange));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true, ViewportKeyboardFocusEvent::OtherEditorPanelClicked));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true, ViewportKeyboardFocusEvent::ApplicationFocusLost));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true, ViewportKeyboardFocusEvent::ViewportUnavailable));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true, ViewportKeyboardFocusEvent::PlayModeStarted));
		PF_CHECK(Tests,
			!CanHandleFocusedViewportToolShortcuts({ .SceneAvailable = true, .ViewportKeyboardFocused = true, .TextInputActive = true }));
		PF_CHECK(Tests,
			!CanHandleFocusedViewportToolShortcuts({ .SceneAvailable = true, .ViewportKeyboardFocused = true, .ModifierHeld = true }));
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
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true
		};
		PF_CHECK(Tests, CanNavigateViewportCamera(KeyboardNavigation));
		PF_CHECK(Tests, !CanNavigateViewportCamera({ .SceneAvailable = true, .ApplicationFocused = true }));
		PF_CHECK(
			Tests, CanNavigateViewportCamera({ .SceneAvailable = true, .RightMouseNavigationActive = true, .ApplicationFocused = true }));
		PF_CHECK(
			Tests, !CanNavigateViewportCamera(
					   { .SceneAvailable = true, .RuntimeActive = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true }));
		PF_CHECK(
			Tests, !CanNavigateViewportCamera(
					   { .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .TextInputActive = true }));
		PF_CHECK(
			Tests, !CanNavigateViewportCamera(
					   { .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .ActiveImGuiItem = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera(
							{ .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .PopupOpen = true }));
		PF_CHECK(Tests,
			!CanNavigateViewportCamera(
				{ .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .MenuNavigationActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera(
							{ .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .ModifierHeld = true }));
		PF_CHECK(Tests,
			!CanNavigateViewportCamera(
				{ .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .GizmoInteractionActive = true }));
		PF_CHECK(Tests,
			!CanNavigateViewportCamera(
				{ .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .CameraGestureActive = true }));
		PF_CHECK(
			Tests, !CanNavigateViewportCamera({ .SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = false }));

		const glm::vec3 CameraForward{ 0.0f, 0.0f, -1.0f };
		const glm::vec3 CameraRight{ 1.0f, 0.0f, 0.0f };
		const auto ForwardStep = CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Forward = true }, 5.0f, 0.5);
		const auto BackwardStep = CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Backward = true }, 5.0f, 0.1);
		const auto RightStep = CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Right = true }, 5.0f, 0.1);
		const auto LeftStep = CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Left = true }, 5.0f, 0.1);
		const auto UpStep = CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Up = true }, 5.0f, 0.1);
		const auto DownStep = CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Down = true }, 5.0f, 0.1);
		const auto DiagonalStep =
			CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Forward = true, .Right = true }, 5.0f, 0.1);
		const auto FastStep = CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Forward = true, .Fast = true }, 5.0f, 0.1);
		PF_CHECK(Tests, glm::length(ForwardStep - glm::vec3(0.0f, 0.0f, -0.5f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(BackwardStep - glm::vec3(0.0f, 0.0f, 0.5f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(RightStep - glm::vec3(0.5f, 0.0f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(LeftStep - glm::vec3(-0.5f, 0.0f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(UpStep - glm::vec3(0.0f, 0.5f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(DownStep - glm::vec3(0.0f, -0.5f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, std::abs(glm::length(DiagonalStep) - glm::length(RightStep)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(FastStep - ForwardStep * 4.0f) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Forward = true }, 5.0f, 0.0)) == 0.0f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(CameraForward, CameraRight, {}, 5.0f, 0.1)) == 0.0f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(CameraForward, CameraRight, { .Forward = true }, 5.0f, 2.0) -
									ForwardStep) < 1.0e-5f);

		const float WheelDollyDelta = GetViewportWheelDollyDelta(1.0f);
		const auto ZoomWithoutPivot = CalculateViewportCameraDolly(glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, WheelDollyDelta);
		const auto ZoomOutWithoutPivot =
			CalculateViewportCameraDolly(glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, GetViewportWheelDollyDelta(-1.0f));
		const auto ZoomWithPivot =
			CalculateViewportCameraDolly(glm::vec3(0.0f), CameraForward, glm::vec3(0.0f, 0.0f, -10.0f), 5.0f, WheelDollyDelta);
		PF_CHECK(Tests, ZoomWithoutPivot && ZoomWithoutPivot->Position.z < 0.0f);
		PF_CHECK(Tests, ZoomWithoutPivot && glm::length(ZoomWithoutPivot->FocusPivot - glm::vec3(0, 0, -10)) < 1.0e-5f);
		PF_CHECK(Tests, ZoomOutWithoutPivot && ZoomOutWithoutPivot->Position.z > 0.0f);
		PF_CHECK(Tests, ZoomWithPivot && ZoomWithPivot->Position.z < 0.0f);
		PF_CHECK(Tests, ZoomWithPivot && glm::length(ZoomWithPivot->FocusPivot - glm::vec3(0, 0, -10)) < 1.0e-5f);
		glm::vec3 RepeatedZoomPosition{ 0.0f };
		std::optional<glm::vec3> RepeatedZoomPivot;
		for (size_t Step = 0; Step < 100; ++Step)
		{
			const auto Updated =
				CalculateViewportCameraDolly(RepeatedZoomPosition, CameraForward, RepeatedZoomPivot, 5.0f, WheelDollyDelta);
			if (!Updated) break;
			RepeatedZoomPosition = Updated->Position;
			RepeatedZoomPivot = Updated->FocusPivot;
		}
		PF_CHECK(Tests, PulseForgeEditor::Detail::IsFinite(RepeatedZoomPosition) && RepeatedZoomPivot &&
							glm::length(RepeatedZoomPosition - *RepeatedZoomPivot) >= 0.149f);
		PF_CHECK(Tests, GetViewportWheelDollyDelta(100.0f) == 6.0f);
		PF_CHECK(Tests, GetViewportWheelDollyDelta(std::numeric_limits<float>::infinity()) == 0.0f);
		PF_CHECK(Tests,
			!CalculateViewportCameraDolly(glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, std::numeric_limits<float>::quiet_NaN()));

		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true }) == ViewportWheelAction::Zoom);
		PF_CHECK(
			Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .RightMouseHeld = true }) == ViewportWheelAction::None);
		PF_CHECK(Tests,
			ResolveViewportWheelAction({ .PointerOverViewportImage = true, .RightMouseNavigationActive = true, .RightMouseHeld = true }) ==
				ViewportWheelAction::AdjustFlySpeed);
		PF_CHECK(
			Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .RuntimeActive = true }) == ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .PopupOpen = true }) == ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({}) == ViewportWheelAction::ScrollPanel);
		PF_CHECK(Tests, FindEditorIcon(EditorIcon::Translate)->Codepoint != FindEditorIcon(EditorIcon::Scale)->Codepoint);

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
		const auto KnownProjectedPoint = ProjectWorldToViewportImage({ 0.25f, -0.5f, 0.5f }, glm::mat4(1.0f), ImageRect);
		PF_CHECK(Tests, KnownProjectedPoint && glm::length(*KnownProjectedPoint - glm::vec2(225.0f, 125.0f)) < 1.0e-5f);

		CameraComponent Camera;
		const auto Projection = Camera.GetProjectionMatrix(2.0f);
		PF_CHECK(Tests, Projection.has_value());
		if (!Projection) return;
		const glm::mat4 View = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 ViewProjection = *Projection * View;
		const auto ProjectedAbove = ProjectWorldToViewportImage({ 0.0f, 1.0f, 0.0f }, ViewProjection, ImageRect);
		const auto ProjectedBelow = ProjectWorldToViewportImage({ 0.0f, -1.0f, 0.0f }, ViewProjection, ImageRect);
		PF_CHECK(Tests, ProjectedAbove && ProjectedAbove->y < 100.0f);
		PF_CHECK(Tests, ProjectedBelow && ProjectedBelow->y > 100.0f);
		const ViewportImageRect DpiScaledRect{ { 200.0f, 100.0f }, { 600.0f, 300.0f } };
		const auto LogicalCenterNdc = ViewportImageToNdc({ 200.0f, 100.0f }, ImageRect);
		const auto DpiCenterNdc = ViewportImageToNdc({ 400.0f, 200.0f }, DpiScaledRect);
		PF_CHECK(Tests, LogicalCenterNdc && DpiCenterNdc && glm::length(*LogicalCenterNdc - *DpiCenterNdc) < 1.0e-5f);

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

		const std::array<ViewportImageRect, 2> ResizedImageRects{ ViewportImageRect{ { 13.0f, 27.0f }, { 613.0f, 327.0f } },
			ViewportImageRect{ { 41.0f, 19.0f }, { 341.0f, 619.0f } } };
		for (const ViewportImageRect& TestRect : ResizedImageRects)
		{
			const float AspectRatio = (TestRect.Maximum.x - TestRect.Minimum.x) / (TestRect.Maximum.y - TestRect.Minimum.y);
			const auto TestProjection = Camera.GetProjectionMatrix(AspectRatio);
			PF_CHECK(Tests, TestProjection.has_value());
			if (!TestProjection) continue;

			const std::array<glm::vec3, 3> CameraPositions{ glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(2.0f, 1.0f, 5.0f),
				glm::vec3(-3.0f, 2.0f, 4.0f) };
			const std::array<glm::vec3, 3> CameraTargets{ glm::vec3(0.4f, -0.2f, 0.0f), glm::vec3(-0.5f, 0.3f, -0.5f),
				glm::vec3(0.8f, -0.4f, -1.0f) };
			for (size_t CameraIndex = 0; CameraIndex < CameraPositions.size(); ++CameraIndex)
			{
				const glm::mat4 CameraView =
					glm::lookAtRH(CameraPositions[CameraIndex], CameraTargets[CameraIndex], glm::vec3(0.0f, 1.0f, 0.0f));
				const glm::mat4 TestViewProjection = *TestProjection * CameraView;
				const glm::vec3 WorldPoint = CameraTargets[CameraIndex] + glm::vec3(0.15f, 0.1f, 0.0f);
				const auto Projected = ProjectWorldToViewportImage(WorldPoint, TestViewProjection, TestRect);
				PF_CHECK(Tests, Projected.has_value());
				if (!Projected) continue;

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

		const std::array<glm::vec3, 3> Triangle{ glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
		const std::array<uint32_t, 3> Indices{ 0, 1, 2 };
		const UUID FarEntity{ 1, 1 };
		const UUID NearEntity{ 2, 2 };
		const glm::mat4 FarTransform = glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, -2.0f });
		const glm::mat4 NearTransform = glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, 1.0f });
		const std::array<PickableMesh, 2> Overlapping{ PickableMesh{ FarEntity, FarTransform, Triangle, Indices },
			PickableMesh{ NearEntity, NearTransform, Triangle, Indices } };
		const Ray WorldRay{ { 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, -1.0f } };
		const auto Nearest = RaycastMeshes(WorldRay, Overlapping);
		PF_CHECK(Tests, Nearest && Nearest->Entity == NearEntity);
		PF_CHECK(Tests, Nearest && std::abs(Nearest->Distance - 4.0f) < 1.0e-4f);
		const std::array<PickableMesh, 1> NoHit{ PickableMesh{
			FarEntity, glm::translate(glm::mat4(1.0f), { 5.0f, 0.0f, 0.0f }), Triangle, Indices } };
		PF_CHECK(Tests, !RaycastMeshes(WorldRay, NoHit));

		const glm::mat4 RotatedNonUniform =
			glm::rotate(glm::scale(glm::mat4(1.0f), { 2.0f, 0.5f, 1.0f }), glm::radians(35.0f), { 0.0f, 0.0f, 1.0f });
		const std::array<PickableMesh, 1> TransformedMesh{ PickableMesh{ NearEntity, RotatedNonUniform, Triangle, Indices } };
		const auto TransformedHit = RaycastMeshes(WorldRay, TransformedMesh);
		PF_CHECK(Tests, TransformedHit && TransformedHit->Entity == NearEntity);
		PF_CHECK(Tests, std::abs(glm::length(WorldRay.Direction) - 1.0f) < 1.0e-5f);
		const std::array<PickableMesh, 1> BeyondFarPlane{ PickableMesh{
			NearEntity, glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, -2000.0f }), Triangle, Indices } };
		PF_CHECK(Tests, !RaycastMeshes(*CenterRay, BeyondFarPlane));
		const std::array<uint32_t, 3> InvalidIndices{ 0, 1, 9 };
		const std::array<PickableMesh, 1> InvalidGeometry{ PickableMesh{ NearEntity, glm::mat4(1.0f), Triangle, InvalidIndices } };
		PF_CHECK(Tests, !RaycastMeshes(WorldRay, InvalidGeometry));

		const std::array<glm::vec3, 4> FloorVertices{ glm::vec3(-2.0f, 4.0f, -2.0f), glm::vec3(2.0f, 4.0f, -2.0f),
			glm::vec3(2.0f, 4.0f, 2.0f), glm::vec3(-2.0f, 4.0f, 2.0f) };
		const std::array<uint32_t, 6> FloorIndices{ 0, 1, 2, 0, 2, 3 };
		const std::array<glm::vec3, 4> LowerFloorVertices{ glm::vec3(-2.0f, 2.0f, -2.0f), glm::vec3(2.0f, 2.0f, -2.0f),
			glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(-2.0f, 2.0f, 2.0f) };
		const UUID FloorEntity{ 3, 3 };
		const std::array<PickableMesh, 2> StackedFloors{ PickableMesh{ FarEntity, glm::mat4(1.0f), LowerFloorVertices, FloorIndices },
			PickableMesh{ FloorEntity, glm::mat4(1.0f), FloorVertices, FloorIndices } };
		const auto NearestFloor = RaycastSupportingSurface(Ray{ { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, StackedFloors);
		PF_CHECK(Tests, NearestFloor && NearestFloor->Entity == FloorEntity);
		PF_CHECK(Tests, NearestFloor && std::abs(NearestFloor->Position.y - 4.0f) < 1.0e-4f &&
							std::abs(NearestFloor->Distance - 1.0f) < 1.0e-4f && NearestFloor->Normal.y > 0.999f);
		const std::array<glm::vec3, 3> WallVertices{ glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 2.0f, -1.0f),
			glm::vec3(0.0f, 0.0f, 1.0f) };
		const std::array<uint32_t, 3> WallIndices{ 0, 1, 2 };
		const std::array<PickableMesh, 1> VerticalWall{ PickableMesh{ FloorEntity, glm::mat4(1.0f), WallVertices, WallIndices } };
		PF_CHECK(Tests, !RaycastSupportingSurface(Ray{ { 0.0f, 3.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, VerticalWall));
		const std::array<glm::vec3, 3> SlopeVertices{ glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(2.0f, 1.0f, -1.0f),
			glm::vec3(2.0f, 1.0f, 1.0f) };
		const std::array<uint32_t, 3> SlopeIndices{ 0, 1, 2 };
		const std::array<PickableMesh, 1> SlopeMesh{ PickableMesh{ FloorEntity, glm::mat4(1.0f), SlopeVertices, SlopeIndices } };
		const auto SlopeHit = RaycastSupportingSurface(Ray{ { 1.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, SlopeMesh);
		PF_CHECK(Tests, SlopeHit && std::abs(SlopeHit->Position.y - 0.5f) < 1.0e-4f && SlopeHit->Normal.y > 0.89f);
		PF_CHECK(Tests, !RaycastSupportingSurface(Ray{ { 1.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, SlopeMesh, 0.95f));
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
				const glm::vec3 Normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(*ChildWorld))) * glm::vec3(0.0f, 0.0f, 1.0f));
				const Ray ChildRay{ Center + Normal * 4.0f, -Normal };
				const std::array<PickableMesh, 1> ParentedMesh{ PickableMesh{ Child->GetUUID(), *ChildWorld, Triangle, Indices } };
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
		PF_CHECK(Tests, !ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Translate, 0, std::numeric_limits<float>::infinity()));
	}

	void TestEditorViewportTargetExtent(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const auto AtScaleOne = CalculateEditorViewportTargetExtent(130.0, 65.0, 1.0, 1.0);
		PF_CHECK(Tests, AtScaleOne && AtScaleOne->Width == 160 && AtScaleOne->Height == 96);

		const auto AtHighDpi = CalculateEditorViewportTargetExtent(130.0, 65.0, 1.5, 1.5);
		PF_CHECK(Tests, AtHighDpi && AtHighDpi->Width == 224 && AtHighDpi->Height == 128);

		const auto InvalidScaleFallsBackToOne = CalculateEditorViewportTargetExtent(130.0, 65.0, 0.0, -1.0);
		PF_CHECK(Tests, InvalidScaleFallsBackToOne && InvalidScaleFallsBackToOne->Width == 160 && InvalidScaleFallsBackToOne->Height == 96);
		PF_CHECK(Tests, !CalculateEditorViewportTargetExtent(0.0, 65.0, 1.0, 1.0));
		PF_CHECK(Tests, !CalculateEditorViewportTargetExtent(std::numeric_limits<double>::infinity(), 65.0, 1.0, 1.0));
		PF_CHECK(Tests, !CalculateEditorViewportTargetExtent(1.0, 1.0, std::numeric_limits<double>::infinity(), 1.0));
		PF_CHECK(Tests, !CalculateEditorViewportTargetExtent(static_cast<double>((std::numeric_limits<uint32_t>::max)()), 1.0, 1.0, 1.0));

		const auto ActiveRuntime = GetEditorViewportSceneMode(true, true, true);
		PF_CHECK(Tests, ActiveRuntime.UseRuntimeScene && !ActiveRuntime.ViewProjectionValid);
		const auto MissingRuntimeScene = GetEditorViewportSceneMode(true, true, false);
		PF_CHECK(Tests, !MissingRuntimeScene.UseRuntimeScene && !MissingRuntimeScene.ViewProjectionValid);
		const auto InactiveRuntimeScene = GetEditorViewportSceneMode(false, true, true);
		PF_CHECK(Tests, !InactiveRuntimeScene.UseRuntimeScene && InactiveRuntimeScene.ViewProjectionValid);
		const auto NoScene = GetEditorViewportSceneMode(false, false, false);
		PF_CHECK(Tests, !NoScene.UseRuntimeScene && !NoScene.ViewProjectionValid);
	}

	void TestEditorViewportInitializationRetry(TestRunner& Tests)
	{
		using Retry = PulseForgeEditor::EditorViewportInitializationRetry;
		const Retry::TimePoint Start{};
		Retry Schedule;
		PF_CHECK(Tests, Schedule.CanAttempt(Start));

		Schedule.RecordFailure(Start);
		PF_CHECK(Tests, !Schedule.CanAttempt(Start));
		PF_CHECK(Tests, !Schedule.CanAttempt(Start + std::chrono::milliseconds(249)));
		PF_CHECK(Tests, Schedule.CanAttempt(Start + std::chrono::milliseconds(250)));

		const Retry::TimePoint FirstRetry = Start + std::chrono::milliseconds(250);
		Schedule.RecordFailure(FirstRetry);
		PF_CHECK(Tests, !Schedule.CanAttempt(FirstRetry + std::chrono::milliseconds(499)));
		PF_CHECK(Tests, Schedule.CanAttempt(FirstRetry + std::chrono::milliseconds(500)));

		Retry::TimePoint Failure = FirstRetry + std::chrono::milliseconds(500);
		for (const std::chrono::milliseconds Delay : { std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
				 std::chrono::milliseconds(4000), std::chrono::milliseconds(5000) })
		{
			Schedule.RecordFailure(Failure);
			PF_CHECK(Tests, !Schedule.CanAttempt(Failure + Delay - std::chrono::milliseconds(1)));
			Failure += Delay;
			PF_CHECK(Tests, Schedule.CanAttempt(Failure));
		}

		Schedule.RecordFailure(Failure);
		Schedule.Reset();
		PF_CHECK(Tests, Schedule.CanAttempt(Failure));
		Schedule.RecordFailure(Failure);
		PF_CHECK(Tests, !Schedule.CanAttempt(Failure));
	}

	void TestViewportMouseCaptureState(TestRunner& Tests)
	{
		PulseForgeEditor::Detail::ViewportMouseCaptureState Capture;
		PF_CHECK(Tests, !Capture.OwnsMouse());
		PF_CHECK(Tests, !Capture.Begin(true, false, true, false, false)); // RMB outside the viewport image.
		PF_CHECK(Tests, !Capture.Begin(true, true, false, false, false)); // Unfocused window.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, true, false));	  // Gizmo drag owns the pointer.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, false, true));	  // Runtime camera is active.
		PF_CHECK(Tests, Capture.Begin(true, true, true, false, false));
		PF_CHECK(Tests, Capture.OwnsMouse());							 // Capture owns mouse routing until release/cancellation.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, false, false)); // No duplicate capture transition.
		Capture.End(); // Same transition is used for RMB release, focus loss, and layout/project changes.
		PF_CHECK(Tests, !Capture.OwnsMouse());
		PF_CHECK(Tests, Capture.Begin(true, true, true, false, false));
		Capture.End();
		PF_CHECK(Tests, !Capture.OwnsMouse());
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

	void TestEditorViewportToolState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		EditorViewportToolState Tools;
		PF_CHECK(Tests, Tools.GetTool() == ViewportTool::Move);
		PF_CHECK(Tests, Tools.GetOperation() == TransformGizmoOperation::Translate);
		PF_CHECK(Tests, Tools.GetValues().LocalSpace);
		PF_CHECK(Tests, !Tools.GetValues().TransformSnapping);
		PF_CHECK(Tests, Tools.GetValues().TranslationSnapIncrement == 1.0f);
		PF_CHECK(Tests, Tools.GetValues().RotationSnapDegrees == 15.0f);
		PF_CHECK(Tests, Tools.GetValues().ScaleSnapIncrement == 0.1f);
		PF_CHECK(Tests, Tools.GetValues().ExposureEV == 0.0f);

		PF_CHECK(Tests, Tools.SetTool(ViewportTool::Rotate));
		PF_CHECK(Tests, Tools.GetTool() == ViewportTool::Rotate);
		PF_CHECK(Tests, Tools.GetOperation() == TransformGizmoOperation::Rotate);
		PF_CHECK(Tests, !Tools.SetTool(ViewportTool::Rotate));
		PF_CHECK(Tests, Tools.SetTool(ViewportTool::Scale));
		PF_CHECK(Tests, Tools.GetOperation() == TransformGizmoOperation::Scale);
		PF_CHECK(Tests, Tools.SetTool(ViewportTool::Select));
		PF_CHECK(Tests, Tools.GetTool() == ViewportTool::Select);
		PF_CHECK(Tests, Tools.GetOperation() == TransformGizmoOperation::Scale);
		PF_CHECK(Tests, Tools.SetTool(ViewportTool::Move));
		PF_CHECK(Tests, Tools.GetOperation() == TransformGizmoOperation::Translate);

		Tools.GetValues().LocalSpace = false;
		Tools.GetValues().TransformSnapping = true;
		Tools.GetValues().TranslationSnapIncrement = 0.5f;
		Tools.GetValues().RotationSnapDegrees = 45.0f;
		Tools.GetValues().ScaleSnapIncrement = 0.25f;
		Tools.GetValues().ExposureEV = 2.5f;
		Tools.ToggleCoordinateSpace();
		PF_CHECK(Tests, Tools.GetValues().LocalSpace);
		Tools.ResetExposure();
		PF_CHECK(Tests, Tools.GetValues().ExposureEV == 0.0f);
		PF_CHECK(Tests, Tools.GetValues().TransformSnapping);
		PF_CHECK(Tests, Tools.GetValues().TranslationSnapIncrement == 0.5f);
		PF_CHECK(Tests, Tools.GetValues().RotationSnapDegrees == 45.0f);
		PF_CHECK(Tests, Tools.GetValues().ScaleSnapIncrement == 0.25f);
	}

} // namespace

void RunEditorViewportTests(TestRunner& Tests)
{
	TestEditorMeshPickingCache(Tests);
	TestEditorViewportMath(Tests);
	TestEditorViewportTargetExtent(Tests);
	TestEditorViewportInitializationRetry(Tests);
	TestViewportMouseCaptureState(Tests);
	TestEditorViewportCameraController(Tests);
	TestEditorViewportToolState(Tests);
}
