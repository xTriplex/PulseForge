"""Detach the production Scene Viewport and exercise its real native-window camera path."""

from __future__ import annotations

import argparse
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

READY = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=READY;"
DETACHED = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=DETACHED;"
CAPTURED = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=CAPTURED;"
PASSED = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=PASS;"
CAMERA_READY = "MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=READY_FOR_CAMERA_INPUT;"
FAILURE_MARKERS = ("MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=FAILED;",)
EVIDENCE_MARKERS = (READY, DETACHED, CAMERA_READY, CAPTURED, PASSED, *FAILURE_MARKERS)


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


def screenshot(filename: str, checkpoint: str) -> None:
    result = capture_screenshot(filename)
    print(f"MACOS_SCENE_VIEWPORT_SCREENSHOT={checkpoint}; {result}")


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

        invalid_inputs = (
            ("action-id", ["drag-left", "123", "not-a-uuid", "10", "10", "20", "20"]),
            ("coordinates", ["drag-left", "123", uuid.uuid4().hex, "nan", "10", "20", "20"]),
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

        ready_line = wait_for(READY, "main Scene Viewport and image ready")
        screenshot("macos-scene-viewport-checkpoint-main.png", "main-owner-scene-loaded")
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
        screenshot("macos-scene-viewport-checkpoint-detached.png", "detached-native-window")

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
        image_center_x = (image_min_x + image_max_x) * 0.5
        image_center_y = (image_min_y + image_max_y) * 0.5
        if image_max_x - image_min_x < 80 or image_max_y - image_min_y < 80:
            raise RuntimeError("detached Scene Viewport image is too small for a bounded camera gesture")

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

        end_camera_x = min(image_max_x - 30.0, image_center_x + 90.0)
        end_camera_y = min(image_max_y - 30.0, image_center_y + 55.0)
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
        screenshot("macos-scene-viewport-checkpoint-camera-input.png", "camera-changed-after-detached-input")

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
    print("MACOS_SCENE_VIEWPORT_NATIVE_SMOKE=PASS; Scene Viewport detached into a secondary native window and actual right-drag changed the production camera")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
