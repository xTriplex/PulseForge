"""Detach the production Scene Viewport and exercise its real native-window camera path."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

from MacOSDirectAX import parse_window_activation, run_direct_ax
from MacOSDocumentCloseSmoke import run_bounded_command
from MacOSDocumentHostSmoke import BoundedOutput, preflight_direct_ax_client, stop_process_group
from MacOSScreenshot import capture_screenshot

MAIN_TITLE_FRAGMENT = "PulseForge Editor"
READY = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=READY;"
DETACHED = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=DETACHED;"
CAPTURED = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=CAPTURED;"
PASSED = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=PASS;"
REATTACHED = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=REATTACHED;"
CAMERA_READY = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=READY_FOR_CAMERA_INPUT;"
STATE = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=STATE;"
FAILURE_MARKERS = ("MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=FAILED;",)
EVIDENCE_MARKERS = (READY, DETACHED, CAMERA_READY, CAPTURED, PASSED, REATTACHED, STATE, *FAILURE_MARKERS)


def marker_fields(line: str, marker: str) -> dict[str, str]:
    if marker not in line:
        raise ValueError(f"expected marker {marker!r}, got {line!r}")
    fields: dict[str, str] = {}
    for part in line.split(marker, 1)[1].split("; "):
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key.strip()] = value.strip()
    return fields


def coordinate(fields: dict[str, str], name: str, count: int) -> tuple[float, ...]:
    raw = fields.get(name, "")
    parts = raw.split(",")
    if len(parts) != count:
        raise ValueError(f"invalid {name} coordinates: {raw!r}")
    values = tuple(float(part) for part in parts)
    if not all(value == value and abs(value) != float("inf") for value in values):
        raise ValueError(f"non-finite {name} coordinates: {raw!r}")
    return values


def activate_main_editor_window(pid: int, deadline: float) -> str:
    code, output, error = run_direct_ax(
        pid,
        operation="activate-window",
        window_title=MAIN_TITLE_FRAGMENT,
        timeout=min(10.0, max(0.1, deadline - time.monotonic())),
    )
    output += error
    if code != 0:
        raise RuntimeError(f"main editor activation failed during focus-loss test: {output.strip()!r}")
    parse_window_activation(output, pid, MAIN_TITLE_FRAGMENT)
    return output


def is_fresh_geometry_sample(line: str | None, previous_sample: int) -> bool:
    if line is None:
        return False
    fields = marker_fields(line, CAMERA_READY)
    try:
        sample = int(fields["sample"])
    except (KeyError, ValueError) as error:
        raise ValueError("camera geometry marker has no valid sample sequence") from error
    if sample <= 0:
        raise ValueError("camera geometry sample sequence must be positive")
    return sample > previous_sample


def screenshot(filename: str, checkpoint: str) -> bool:
    result = capture_screenshot(filename)
    print(f"MACOS_SCENE_VIEWPORT_SCREENSHOT={checkpoint}; {result}")
    return result.startswith("CAPTURED;") and "path=" in result


def visible_camera_region(
    image_minimum: tuple[float, float],
    image_maximum: tuple[float, float],
    displays: list[dict[str, float | int | str]],
    inset: float = 24.0,
    minimum_extent: float = 80.0,
) -> tuple[dict[str, float | int | str], tuple[float, float], tuple[float, float]]:
    candidates: list[tuple[float, dict[str, float | int | str], tuple[float, float], tuple[float, float]]] = []
    for display in displays:
        try:
            x, y = float(display["x"]), float(display["y"])
            width, height = float(display["width"]), float(display["height"])
            scale = float(display["backing_scale"])
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f"malformed native display geometry: {display!r}") from error
        if not all(value == value and abs(value) != float("inf") for value in (x, y, width, height, scale)):
            raise ValueError(f"non-finite native display geometry: {display!r}")
        if width <= 0.0 or height <= 0.0 or scale <= 0.0:
            continue
        # CGDisplayBounds and CGEvent locations are both in Quartz global display
        # coordinates. The backing scale is evidence only; do not rescale points.
        intersection_minimum = (max(image_minimum[0], x), max(image_minimum[1], y))
        intersection_maximum = (min(image_maximum[0], x + width), min(image_maximum[1], y + height))
        minimum = (intersection_minimum[0] + inset, intersection_minimum[1] + inset)
        maximum = (intersection_maximum[0] - inset, intersection_maximum[1] - inset)
        extent_x, extent_y = maximum[0] - minimum[0], maximum[1] - minimum[1]
        if extent_x >= minimum_extent and extent_y >= minimum_extent:
            candidates.append((extent_x * extent_y, display, minimum, maximum))
    if not candidates:
        raise ValueError(
            f"detached viewport has no usable visible region in native Quartz display geometry; "
            f"image={image_minimum}..{image_maximum}; displays={displays!r}"
        )
    _, display, minimum, maximum = max(candidates, key=lambda candidate: candidate[0])
    return display, minimum, maximum


def visible_camera_target(
    image_minimum: tuple[float, float],
    image_maximum: tuple[float, float],
    displays: list[dict[str, float | int | str]],
) -> tuple[dict[str, float | int | str], tuple[float, float], tuple[float, float], tuple[float, float]]:
    display, minimum, maximum = visible_camera_region(image_minimum, image_maximum, displays)
    center = ((minimum[0] + maximum[0]) * 0.5, (minimum[1] + maximum[1]) * 0.5)
    return display, minimum, maximum, center


def read_native_displays(driver: Path, timeout: float) -> list[dict[str, float | int | str]]:
    code, stdout, stderr = run_bounded_command([str(driver), "display-geometry"], timeout=timeout)
    prefix = "SCENE_VIEWPORT_DISPLAY_GEOMETRY="
    line = next((line for line in stdout.splitlines() if line.startswith(prefix)), None)
    if code != 0 or line is None:
        raise RuntimeError(f"native display geometry query failed; exit_code={code}; stdout={stdout!r}; stderr={stderr!r}")
    try:
        result = json.loads(line[len(prefix):])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"native display geometry protocol was malformed: {line!r}") from error
    if not isinstance(result, list) or not result:
        raise RuntimeError(f"native display geometry returned no active displays: {result!r}")
    return result


def remove_root(root: Path, parent: Path) -> str | None:
    try:
        if root.is_symlink():
            return f"refusing to remove symlink fixture root: {root}"
        resolved_root = root.resolve(strict=True)
        resolved_parent = parent.resolve(strict=True)
        if resolved_root.parent != resolved_parent or not resolved_root.name.startswith("PulseForge Scene Viewport "):
            return f"refusing to remove unexpected fixture root: {resolved_root}"
        shutil.rmtree(resolved_root)
        return None
    except OSError as error:
        return f"could not remove fixture root {root}: {error}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()

    try:
        editor = args.editor.resolve(strict=True)
        project = args.project.resolve(strict=True)
    except OSError as error:
        print(f"Scene Viewport smoke input resolution failed: {error}", file=sys.stderr)
        return 1
    log_path = args.log_directory / "macos-scene-viewport-native-smoke.log"
    if not editor.is_file() or not os.access(editor, os.X_OK):
        print(f"Scene Viewport smoke editor is unavailable: {editor}", file=sys.stderr)
        return 1
    if not project.is_file() or project.suffix != ".pfproj" or args.timeout_seconds <= 0:
        print(f"Scene Viewport smoke project or timeout is invalid: {project}", file=sys.stderr)
        return 1

    args.log_directory.mkdir(parents=True, exist_ok=True)
    runner_temp = os.environ.get("RUNNER_TEMP")
    temp_parent = Path(runner_temp).resolve() if runner_temp else Path(tempfile.gettempdir())
    root = Path(tempfile.mkdtemp(prefix="PulseForge Scene Viewport ", dir=temp_parent))
    working_directory = root / "unrelated working directory"
    settings_directory = root / "isolated editor settings"
    driver_source = Path(__file__).with_name("MacOSSceneViewportPointer.swift").resolve(strict=True)
    driver = root / "MacOSSceneViewportPointer"
    working_directory.mkdir()
    settings_directory.mkdir()
    deadline = time.monotonic() + args.timeout_seconds
    process: subprocess.Popen[str] | None = None
    capture: BoundedOutput | None = None
    failures: list[str] = []
    normal_shutdown_completed = False

    try:
        preflight_direct_ax_client(min(30.0, max(0.1, deadline - time.monotonic())))
        compile_timeout = min(30.0, max(0.1, deadline - time.monotonic()))
        code, stdout, stderr = run_bounded_command(
            ["/usr/bin/xcrun", "swiftc", "-O", str(driver_source), "-framework", "AppKit", "-o", str(driver)],
            timeout=compile_timeout,
        )
        print(f"MACOS_SCENE_VIEWPORT_POINTER_HELPER_BUILD=exit_code={code}; stdout={stdout[-1000:]!r}; stderr={stderr[-1000:]!r}")
        if code != 0 or not driver.is_file():
            raise RuntimeError("the bounded native pointer helper did not compile")

        display_query_timeout = min(5.0, max(0.1, deadline - time.monotonic()))
        displays = read_native_displays(driver, display_query_timeout)
        print(f"MACOS_SCENE_VIEWPORT_NATIVE_DISPLAYS={displays!r}")

        invalid_inputs = (
            ("action-id", ["drag-left", "123", "not-a-uuid", "10", "10", "20", "20"]),
            ("coordinates", ["drag-left", "123", uuid.uuid4().hex, "nan", "10", "20", "20"]),
            ("scroll-delta", ["scroll", "123", uuid.uuid4().hex, "10", "10", "0"]),
            ("key-hold-duration", ["key-hold", "123", uuid.uuid4().hex, "13", "3.0"]),
        )
        for label, helper_arguments in invalid_inputs:
            code, stdout, stderr = run_bounded_command([str(driver), *helper_arguments], timeout=5)
            if code != 2 or "no events were posted" not in stderr or "SCENE_VIEWPORT_CG_EVENT" in stdout:
                raise RuntimeError(
                    f"pointer helper accepted malformed {label} input or posted an event: "
                    f"exit_code={code}; stdout={stdout[-500:]!r}; stderr={stderr[-500:]!r}"
                )
            print(f"MACOS_SCENE_VIEWPORT_POINTER_INPUT_VALIDATION=PASS; case={label}; no events posted")

        environment = os.environ.copy()
        environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(settings_directory)
        command = [str(editor), "--project", str(project), "--scene-viewport-native-smoke"]
        print(f"MACOS_SCENE_VIEWPORT_COMMAND={command!r}; cwd={working_directory}")
        process = subprocess.Popen(
            command,
            cwd=working_directory,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        capture = BoundedOutput(process, log_path, EVIDENCE_MARKERS)

        def wait_for(marker: str, label: str) -> str:
            assert process is not None and capture is not None
            while time.monotonic() < deadline:
                if capture.reader_error:
                    raise RuntimeError(f"editor output reader failed while waiting for {label}: {capture.reader_error}")
                if marker in capture.markers:
                    line = capture.marker_lines.get(marker)
                    if not line:
                        raise RuntimeError(f"{label} was recorded without its bounded evidence line")
                    print(f"MACOS_SCENE_VIEWPORT_OBSERVED={label}; {line}")
                    return line
                failure = next((capture.marker_lines[item] for item in FAILURE_MARKERS if item in capture.markers), None)
                if failure:
                    raise RuntimeError(f"editor fixture failed: {failure}")
                if process.poll() is not None:
                    raise RuntimeError(f"editor exited with status {process.returncode} before {label}")
                capture.finished.wait(timeout=min(0.1, max(0.0, deadline - time.monotonic())))
            raise TimeoutError(f"timed out waiting for {label}; process_status={process.poll()}")

        def wait_for_state(after_sample: int, label: str, predicate) -> str:
            assert process is not None and capture is not None
            while time.monotonic() < deadline:
                if capture.reader_error:
                    raise RuntimeError(f"editor output reader failed while waiting for {label}: {capture.reader_error}")
                line = capture.marker_lines.get(STATE)
                if line:
                    fields = marker_fields(line, STATE)
                    try:
                        sample = int(fields["sample"])
                    except (KeyError, ValueError) as error:
                        raise ValueError(f"native camera state marker is malformed: {line!r}") from error
                    if sample > after_sample and predicate(fields):
                        print(f"MACOS_SCENE_VIEWPORT_OBSERVED={label}; {line}")
                        return line
                failure = next((capture.marker_lines[item] for item in FAILURE_MARKERS if item in capture.markers), None)
                if failure:
                    raise RuntimeError(f"editor fixture failed: {failure}")
                if process.poll() is not None:
                    raise RuntimeError(f"editor exited with status {process.returncode} before {label}")
                capture.finished.wait(timeout=min(0.1, max(0.0, deadline - time.monotonic())))
            raise TimeoutError(f"timed out waiting for {label}; after_sample={after_sample}; process_status={process.poll()}")

        ready_line = wait_for(READY, "main Scene Viewport and image ready")
        if not screenshot("macos-scene-viewport-checkpoint-main.png", "main-owner-scene-loaded"):
            raise RuntimeError("could not capture the main-owner screenshot evidence")
        activation_code, activation_output, activation_error = run_direct_ax(
            process.pid, operation="activate-window", window_title="PulseForge Editor", timeout=min(10.0, deadline - time.monotonic())
        )
        activation_output += activation_error
        if activation_code != 0:
            raise RuntimeError(f"main editor window activation failed before detach gesture: {activation_output.strip()!r}")
        main_title = parse_window_activation(activation_output, process.pid, "PulseForge Editor")
        initial = marker_fields(ready_line, READY)
        start_x, start_y = coordinate(initial, "title_center", 2)
        end_x, end_y = coordinate(initial, "tear_off_point", 2)
        tab_min_x, tab_min_y, tab_max_x, tab_max_y = coordinate(initial, "dock_tab_rect", 4)
        image_min_x, image_min_y = coordinate(initial, "image_min", 2)
        main_x, main_y, main_width, main_height = coordinate(initial, "main_rect", 4)
        if tab_max_x - tab_min_x < 30 or tab_max_y - tab_min_y < 8:
            raise RuntimeError("Scene Viewport did not report a usable dock-tab rectangle")
        if not (tab_min_x <= start_x <= tab_max_x and tab_min_y <= start_y <= tab_max_y):
            raise RuntimeError("Scene Viewport drag origin is outside its observed dock tab")
        if tab_max_y > image_min_y:
            raise RuntimeError("Scene Viewport dock-tab rectangle overlaps the rendered image origin")
        if not (main_x <= start_x <= main_x + main_width and main_y <= start_y <= main_y + main_height):
            raise RuntimeError("Scene Viewport tab drag origin is outside the main editor viewport")
        if main_x <= end_x <= main_x + main_width and main_y <= end_y <= main_y + main_height:
            raise RuntimeError("Scene Viewport tear-off destination is still inside the main editor viewport")
        detach_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "drag-left", str(process.pid), detach_id, str(start_x), str(start_y), str(end_x), str(end_y)],
            timeout=min(15.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_DETACH_INPUT=action_id={detach_id}; focused_window={main_title!r}; "
              f"exit_code={code}; output={stdout[-2500:]!r}; error={stderr[-1000:]!r}")
        if code != 0:
            raise RuntimeError("the single Scene Viewport title-bar drag did not complete")

        detached_line = wait_for(DETACHED, "Scene Viewport attached to a secondary native window")
        detached = marker_fields(detached_line, DETACHED)
        if detached.get("owner_viewport") == initial.get("owner_viewport") or detached.get("owner_window") == initial.get("owner_window"):
            raise RuntimeError("detached Scene Viewport did not acquire a distinct native owner")
        if not screenshot("macos-scene-viewport-checkpoint-detached.png", "detached-native-window"):
            raise RuntimeError("could not capture the detached-window screenshot evidence")

        ax_code, ax_output, ax_error = run_direct_ax(
            process.pid, operation="activate-window", window_title="Scene Viewport", timeout=min(10.0, deadline - time.monotonic())
        )
        ax_output += ax_error
        if ax_code != 0:
            raise RuntimeError(f"could not focus the observed Scene Viewport native window: {ax_output.strip()!r}")
        focused_title = parse_window_activation(ax_output, process.pid, "Scene Viewport")

        latest_camera_geometry = capture.marker_lines.get(CAMERA_READY)
        geometry_sequence = int(marker_fields(latest_camera_geometry, CAMERA_READY)["sample"]) if latest_camera_geometry else 0
        while time.monotonic() < deadline:
            if capture.reader_error:
                raise RuntimeError(f"editor output reader failed while waiting for fresh viewport geometry: {capture.reader_error}")
            latest_camera_geometry = capture.marker_lines.get(CAMERA_READY)
            if latest_camera_geometry:
                if is_fresh_geometry_sample(latest_camera_geometry, geometry_sequence):
                    break
            failure = next((capture.marker_lines[item] for item in FAILURE_MARKERS if item in capture.markers), None)
            if failure:
                raise RuntimeError(f"editor fixture failed: {failure}")
            if process.poll() is not None:
                raise RuntimeError(f"editor exited with status {process.returncode} before fresh viewport geometry")
            capture.finished.wait(timeout=min(0.1, max(0.0, deadline - time.monotonic())))
        else:
            raise TimeoutError(
                f"timed out waiting for a fresh stable viewport geometry sample after activation; "
                f"last_sequence={geometry_sequence}; process_status={process.poll()}"
            )
        assert latest_camera_geometry is not None
        camera_ready_line = latest_camera_geometry
        print(f"MACOS_SCENE_VIEWPORT_OBSERVED=post-activation stable geometry; {camera_ready_line}")
        camera_ready = marker_fields(camera_ready_line, CAMERA_READY)
        image_min_x, image_min_y = coordinate(camera_ready, "image_min", 2)
        image_max_x, image_max_y = coordinate(camera_ready, "image_max", 2)
        native_x, native_y, native_width, native_height = coordinate(camera_ready, "owner_native_rect", 4)
        if not (native_x <= image_min_x < image_max_x <= native_x + native_width and
                native_y <= image_min_y < image_max_y <= native_y + native_height):
            raise RuntimeError("fresh Scene Viewport image geometry is outside its focused native owner")
        selected_display, visible_minimum, visible_maximum, image_center = visible_camera_target(
            (image_min_x, image_min_y), (image_max_x, image_max_y), displays
        )
        image_center_x, image_center_y = image_center
        print(f"MACOS_SCENE_VIEWPORT_VISIBLE_INPUT_REGION=display_id={selected_display['id']}; "
              f"coordinate_space={selected_display['coordinate_space']!r}; origin={selected_display['origin']!r}; "
              f"backing_scale={selected_display['backing_scale']}; scale_applied={selected_display['scale_applied']}; "
              f"bounds={selected_display['x']},{selected_display['y']},{selected_display['width']},{selected_display['height']}; "
              f"image={image_min_x:.1f},{image_min_y:.1f}..{image_max_x:.1f},{image_max_y:.1f}; "
              f"usable={visible_minimum[0]:.1f},{visible_minimum[1]:.1f}.."
              f"{visible_maximum[0]:.1f},{visible_maximum[1]:.1f}")

        press_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "press-right", str(process.pid), press_id, str(image_center_x), str(image_center_y)],
            timeout=min(10.0, max(0.1, deadline - time.monotonic())),
        )
        right_press_attempted = code in (0, 124)
        print(f"MACOS_SCENE_VIEWPORT_CAMERA_PRESS=action_id={press_id}; focused_window={focused_title!r}; "
              f"exit_code={code}; output={stdout[-1200:]!r}; error={stderr[-800:]!r}")
        if code != 0:
            raise RuntimeError("the intended right-button press was not posted")
        wait_for(CAPTURED, "camera capture started on the detached native owner")

        capture_state_line = wait_for_state(
            0, "detached owner capture state", lambda fields: fields.get("focused") == "1" and fields.get("capture") == "1"
        )
        capture_state = marker_fields(capture_state_line, STATE)
        capture_state_sample = int(capture_state["sample"])
        speed_before_wheel = float(capture_state["camera_speed"])
        wheel_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "scroll", str(process.pid), wheel_id, str(image_center_x), str(image_center_y), "1"],
            timeout=min(8.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_OWNER_WHEEL=action_id={wheel_id}; exit_code={code}; "
              f"output={stdout[-1000:]!r}; error={stderr[-800:]!r}")
        if code != 0:
            raise RuntimeError("the single detached-owner wheel event was not posted")
        wheel_state_line = wait_for_state(
            capture_state_sample, "detached-owner fly speed changed from owner wheel input",
            lambda fields: fields.get("focused") == "1" and fields.get("capture") == "1" and
            float(fields.get("camera_speed", "nan")) > speed_before_wheel,
        )
        wheel_state = marker_fields(wheel_state_line, STATE)
        wheel_state_sample = int(wheel_state["sample"])

        end_camera_x = min(visible_maximum[0] - 8.0, image_center_x + 70.0)
        end_camera_y = min(visible_maximum[1] - 8.0, image_center_y + 45.0)
        if (end_camera_x, end_camera_y) == (image_center_x, image_center_y):
            raise RuntimeError("detached viewport geometry did not provide a camera movement path")
        move_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "move-right", str(process.pid), move_id, str(image_center_x), str(image_center_y),
             str(end_camera_x), str(end_camera_y)],
            timeout=min(15.0, max(0.1, deadline - time.monotonic())),
        )
        right_press_attempted = code != 0 or "event=right-up" not in stdout
        print(f"MACOS_SCENE_VIEWPORT_CAMERA_DRAG=action_id={move_id}; exit_code={code}; "
              f"output={stdout[-2500:]!r}; error={stderr[-1000:]!r}")
        if code != 0:
            raise RuntimeError("the bounded right-button drag continuation and release sequence failed")
        passed_line = wait_for(PASSED, "camera orientation changed and capture released")
        print(f"MACOS_SCENE_VIEWPORT_ACCEPTANCE={passed_line}")
        if not screenshot("macos-scene-viewport-checkpoint-camera-input.png", "camera-changed-after-detached-input"):
            raise RuntimeError("could not capture the changed-camera screenshot evidence")

        released_state_line = wait_for_state(
            wheel_state_sample, "normal right-button release restored detached navigation state",
            lambda fields: fields.get("focused") == "1" and fields.get("capture") == "0",
        )
        released_state = marker_fields(released_state_line, STATE)
        released_sample = int(released_state["sample"])
        position_before_key = coordinate(released_state, "camera_position", 3)
        key_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "key-hold", str(process.pid), key_id, "13", "0.5"],
            timeout=min(8.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_DETACHED_KEYBOARD=action_id={key_id}; keycode=13; "
              f"exit_code={code}; output={stdout[-1000:]!r}; error={stderr[-800:]!r}")
        if code != 0:
            raise RuntimeError("the bounded detached-owner keyboard input was not posted")
        moved_state_line = wait_for_state(
            released_sample, "camera translated from detached-owner keyboard input",
            lambda fields: fields.get("focused") == "1" and fields.get("keyboard_focused") == "1" and
            sum((a - b) ** 2 for a, b in zip(coordinate(fields, "camera_position", 3), position_before_key)) > 1.0e-6,
        )
        moved_state = marker_fields(moved_state_line, STATE)
        moved_sample = int(moved_state["sample"])

        pre_loss_state_line = wait_for_state(
            moved_sample, "detached owner ready before focus-loss capture",
            lambda fields: fields.get("focused") == "1" and fields.get("capture") == "0",
        )
        pre_loss_state = marker_fields(pre_loss_state_line, STATE)
        pre_loss_sample = int(pre_loss_state["sample"])
        normal_cursor_mode = pre_loss_state["cursor_mode"]
        raw_motion_before = pre_loss_state["raw_motion_enabled"]
        loss_press_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "press-right", str(process.pid), loss_press_id, str(image_center_x), str(image_center_y)],
            timeout=min(8.0, max(0.1, deadline - time.monotonic())),
        )
        right_press_attempted = code in (0, 124)
        print(f"MACOS_SCENE_VIEWPORT_FOCUS_LOSS_PRESS=action_id={loss_press_id}; exit_code={code}; "
              f"output={stdout[-1000:]!r}; error={stderr[-800:]!r}")
        if code != 0:
            raise RuntimeError("the deliberate focus-loss capture press was not posted")
        captured_state_line = wait_for_state(
            pre_loss_sample, "detached owner entered capture before focus loss",
            lambda fields: fields.get("focused") == "1" and fields.get("capture") == "1",
        )
        captured_state = marker_fields(captured_state_line, STATE)
        captured_sample = int(captured_state["sample"])

        activate_main_editor_window(process.pid, deadline)
        focus_loss_line = wait_for_state(
            captured_sample, "capture cancelled and cursor settings restored after owner focus loss",
            lambda fields: fields.get("focused") == "0" and fields.get("capture") == "0" and
            fields.get("cursor_mode") == normal_cursor_mode and fields.get("raw_motion_enabled") == raw_motion_before,
        )
        focus_loss_state = marker_fields(focus_loss_line, STATE)
        focus_loss_sample = int(focus_loss_state["sample"])
        focus_loss_position = coordinate(focus_loss_state, "camera_position", 3)

        release_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "release-right", str(process.pid), release_id, str(image_center_x), str(image_center_y)],
            timeout=min(5.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_FOCUS_LOSS_RELEASE_CLEANUP=action_id={release_id}; exit_code={code}; "
              f"output={stdout[-500:]!r}; error={stderr[-500:]!r}")
        if code != 0:
            raise RuntimeError("the matching release for the focus-loss capture could not be posted")
        right_press_attempted = False

        other_window_key_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "key-hold", str(process.pid), other_window_key_id, "13", "0.5"],
            timeout=min(8.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_NONOWNER_KEYBOARD=action_id={other_window_key_id}; "
              f"exit_code={code}; output={stdout[-1000:]!r}; error={stderr[-800:]!r}")
        if code != 0:
            raise RuntimeError("keyboard input directed to the main editor window was not posted")
        other_window_state_line = wait_for_state(
            focus_loss_sample, "main-window keyboard input left detached camera unchanged",
            lambda fields: fields.get("focused") == "0" and
            sum((a - b) ** 2 for a, b in zip(coordinate(fields, "camera_position", 3), focus_loss_position)) <= 1.0e-8,
        )
        other_window_state = marker_fields(other_window_state_line, STATE)
        other_window_sample = int(other_window_state["sample"])

        activation_code, activation_output, activation_error = run_direct_ax(
            process.pid, operation="activate-window", window_title="Scene Viewport",
            timeout=min(10.0, max(0.1, deadline - time.monotonic())),
        )
        activation_output += activation_error
        if activation_code != 0:
            raise RuntimeError(f"detached Scene Viewport focus reacquisition failed: {activation_output.strip()!r}")
        focused_title = parse_window_activation(activation_output, process.pid, "Scene Viewport")
        activation_state_line = wait_for_state(
            other_window_sample, "detached Scene Viewport geometry after AX activation",
            lambda fields: fields.get("owner_viewport") == detached.get("owner_viewport") and
            fields.get("capture") == "0" and
            all(key in fields for key in ("image_min", "image_max")),
        )
        activation_state = marker_fields(activation_state_line, STATE)
        activation_sample = int(activation_state["sample"])
        image_min = coordinate(activation_state, "image_min", 2)
        image_max = coordinate(activation_state, "image_max", 2)
        if image_max[0] <= image_min[0] or image_max[1] <= image_min[1]:
            raise RuntimeError("detached Scene Viewport had no usable image region after AX activation")
        reacquire_displays = read_native_displays(
            driver, timeout=min(5.0, max(0.1, deadline - time.monotonic()))
        )
        reacquire_display, reacquire_minimum, reacquire_maximum, reacquire_point = visible_camera_target(
            image_min, image_max, reacquire_displays
        )
        reacquire_x, reacquire_y = reacquire_point
        print(f"MACOS_SCENE_VIEWPORT_REACQUIRE_VISIBLE_REGION=display_id={reacquire_display['id']}; "
              f"coordinate_space={reacquire_display['coordinate_space']!r}; "
              f"bounds={reacquire_display['x']},{reacquire_display['y']},"
              f"{reacquire_display['width']},{reacquire_display['height']}; "
              f"image={image_min[0]:.1f},{image_min[1]:.1f}..{image_max[0]:.1f},{image_max[1]:.1f}; "
              f"usable={reacquire_minimum[0]:.1f},{reacquire_minimum[1]:.1f}.."
              f"{reacquire_maximum[0]:.1f},{reacquire_maximum[1]:.1f}; "
              f"point={reacquire_x:.1f},{reacquire_y:.1f}")
        click_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "click-left", str(process.pid), click_id, str(reacquire_x), str(reacquire_y)],
            timeout=min(8.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_REACQUIRE_CLICK=action_id={click_id}; window={focused_title!r}; "
              f"geometry_sample={activation_sample}; point={reacquire_x:.1f},{reacquire_y:.1f}; "
              f"exit_code={code}; output={stdout[-1200:]!r}; error={stderr[-800:]!r}")
        if code != 0:
            raise RuntimeError("the deliberate viewport click used to reacquire keyboard focus was not posted")
        keyboard_focus_line = wait_for_state(
            activation_sample, "viewport keyboard focus reacquired on detached owner after one click",
            lambda fields: fields.get("focused") == "1" and fields.get("keyboard_focused") == "1",
        )
        keyboard_focus_state = marker_fields(keyboard_focus_line, STATE)
        keyboard_focus_sample = int(keyboard_focus_state["sample"])
        keyboard_reacquire_position = coordinate(keyboard_focus_state, "camera_position", 3)
        reacquire_key_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "key-hold", str(process.pid), reacquire_key_id, "13", "0.5"],
            timeout=min(8.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_REACQUIRE_KEYBOARD=action_id={reacquire_key_id}; "
              f"exit_code={code}; output={stdout[-1000:]!r}; error={stderr[-800:]!r}")
        if code != 0:
            raise RuntimeError("keyboard input after detached-owner focus reacquisition was not posted")
        reactivated_state_line = wait_for_state(
            keyboard_focus_sample, "camera navigation resumed after detached-owner focus reacquisition",
            lambda fields: fields.get("focused") == "1" and fields.get("keyboard_focused") == "1" and
            sum((a - b) ** 2 for a, b in zip(coordinate(fields, "camera_position", 3), keyboard_reacquire_position)) > 1.0e-6,
        )
        reactivated_state = marker_fields(reactivated_state_line, STATE)
        reactivated_sample = int(reactivated_state["sample"])

        reattach_state_line = wait_for_state(
            reactivated_sample, "detached viewport geometry before reattachment",
            lambda fields: fields.get("focused") == "1" and fields.get("capture") == "0",
        )
        reattach_state = marker_fields(reattach_state_line, STATE)
        reattach_start_x, reattach_start_y = coordinate(reattach_state, "title_center", 2)
        tab_min_x, tab_min_y, tab_max_x, tab_max_y = coordinate(reattach_state, "dock_tab_rect", 4)
        if not (tab_min_x <= reattach_start_x <= tab_max_x and tab_min_y <= reattach_start_y <= tab_max_y):
            raise RuntimeError("fresh detached Scene Viewport title center is outside its observed dock tab")
        main_x, main_y, main_width, main_height = coordinate(reattach_state, "main_rect", 4)
        reattach_end_x = main_x + main_width * 0.5
        reattach_end_y = main_y + main_height * 0.55
        reattach_id = uuid.uuid4().hex
        code, stdout, stderr = run_bounded_command(
            [str(driver), "drag-left", str(process.pid), reattach_id, str(reattach_start_x), str(reattach_start_y),
             str(reattach_end_x), str(reattach_end_y)],
            timeout=min(15.0, max(0.1, deadline - time.monotonic())),
        )
        print(f"MACOS_SCENE_VIEWPORT_REATTACH_INPUT=action_id={reattach_id}; destination={reattach_end_x:.1f},{reattach_end_y:.1f}; "
              f"exit_code={code}; output={stdout[-2500:]!r}; error={stderr[-1000:]!r}")
        if code != 0:
            raise RuntimeError("the single Scene Viewport reattachment gesture did not complete")
        right_press_attempted = False
        reattached_line = wait_for(REATTACHED, "Scene Viewport reattached to its original main utility window")
        reattached = marker_fields(reattached_line, REATTACHED)
        if reattached.get("main_viewport") != initial.get("main_viewport") or reattached.get("scene_image_ready") != "1":
            raise RuntimeError(f"reattached Scene Viewport did not return to the original main owner: {reattached_line}")
        before_reattach_position = coordinate(reattach_state, "camera_position", 3)
        after_reattach_position = coordinate(reattached, "camera_position", 3)
        if sum((a - b) ** 2 for a, b in zip(before_reattach_position, after_reattach_position)) > 1.0e-6:
            raise RuntimeError("Scene Viewport camera position changed while reattaching")
        if not screenshot("macos-scene-viewport-checkpoint-reattached.png", "reattached-utility-panel"):
            raise RuntimeError("could not capture the reattached utility-panel screenshot evidence")

        close_code, close_stdout, close_stderr = run_direct_ax(
            process.pid,
            operation="close-main-window",
            window_title="PulseForge Editor",
            timeout=min(12.0, max(0.1, deadline - time.monotonic())),
        )
        close_output = close_stdout + close_stderr
        dispatched = "DIRECT_AX_DISPATCH attempted=true; operation=close-main-window" in close_output
        print(f"MACOS_SCENE_VIEWPORT_SHUTDOWN_DISPATCH=attempted={dispatched}; client_exit={close_code}; "
              f"output={close_output[-1800:]!r}")
        if not dispatched:
            raise RuntimeError("native main-window close was not confirmed as dispatched; no retry will be attempted")
        try:
            exit_code = process.wait(timeout=max(0.0, min(15.0, deadline - time.monotonic())))
        except subprocess.TimeoutExpired as error:
            raise TimeoutError("editor did not exit after the dispatched native main-window close") from error
        print(f"MACOS_SCENE_VIEWPORT_SHUTDOWN_COMPLETION=process_exit={exit_code}; expected_exit=0; "
              f"ax_client_exit={close_code}; action_result_is_reconciled_without_replay={close_code != 0}")
        if exit_code != 0:
            raise RuntimeError(f"editor exited with unexpected status after native main-window close: {exit_code}")
        if close_code != 0:
            print("MACOS_SCENE_VIEWPORT_SHUTDOWN_RECONCILIATION=dispatched AX result was ambiguous; normal exit 0 confirms completion")
        assert capture is not None
        capture.thread.join(timeout=5)
        if capture.thread.is_alive():
            raise RuntimeError("editor output reader remained active after normal application exit")
        if capture.reader_error:
            raise RuntimeError(f"editor output reader failed during normal shutdown: {capture.reader_error}")
        normal_shutdown_completed = True

    except (OSError, RuntimeError, TimeoutError, ValueError, subprocess.SubprocessError) as error:
        failures.append(f"{type(error).__name__}: {error}")
        if process is not None and process.poll() is None:
            screenshot("macos-scene-viewport-checkpoint-unexpected-failure.png", "unexpected-failure-before-cleanup")
        print(f"MACOS_SCENE_VIEWPORT_SMOKE_FAILURE={failures[-1]}", file=sys.stderr)
    finally:
        process_stopped = process is None or process.poll() is not None
        if process is not None and process.poll() is None:
            if 'right_press_attempted' in locals() and right_press_attempted:
                try:
                    fields = marker_fields(capture.marker_lines[DETACHED], DETACHED) if capture and DETACHED in capture.marker_lines else {}
                    image_min = coordinate(fields, "image_min", 2)
                    image_max = coordinate(fields, "image_max", 2)
                    cleanup_id = uuid.uuid4().hex
                    cleanup_point = ((image_min[0] + image_max[0]) * 0.5, (image_min[1] + image_max[1]) * 0.5)
                    code, stdout, stderr = run_bounded_command(
                        [str(driver), "release-right", str(process.pid), cleanup_id, str(cleanup_point[0]), str(cleanup_point[1])],
                        timeout=3,
                    )
                    print(f"MACOS_SCENE_VIEWPORT_RIGHT_RELEASE_CLEANUP=exit_code={code}; {stdout[-500:]!r}; {stderr[-300:]!r}")
                except (KeyError, OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
                    print(f"MACOS_SCENE_VIEWPORT_RIGHT_RELEASE_CLEANUP=FAILED; {error}", file=sys.stderr)
                    failures.append(f"right-button cleanup failed: {error}")
            stopped = stop_process_group(process)
            process_stopped = stopped
            if capture:
                capture.thread.join(timeout=5)
            if not stopped or (capture and capture.thread.is_alive()):
                failures.append("owned editor process group or output reader did not stop cleanly")
            if normal_shutdown_completed:
                failures.append("editor remained alive after normal-shutdown acceptance and required forced cleanup")
        if process_stopped and (capture is None or not capture.thread.is_alive()):
            cleanup = remove_root(root, temp_parent)
            if cleanup:
                failures.append(cleanup)
        else:
            failures.append(f"preserved temporary fixture root because an owned process may still use it: {root}")

    if failures:
        print(f"MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=FAIL; {failures!r}", file=sys.stderr)
        if log_path.is_file():
            print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        return 1
    print("MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=PASS; Scene Viewport detached into a secondary native window, actual right-drag changed the production camera, and the main window closed normally")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
