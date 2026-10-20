#!/usr/bin/env python3
"""Verify editor document-host persistence across two normal macOS process launches."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from MacOSDocumentHostSmoke import (  # noqa: E402
    QUIT_APPLICATION,
    WINDOW_QUERY,
    has_tcc_denial,
    run_osascript,
    stop_process_group,
)
from MacOSAutomationClient import capture_failure_context, probe_osascript_access
from MacOSScreenshot import capture_screenshot

MAX_LOG_BYTES = 128 * 1024
MAX_CONSOLE_BYTES = 64 * 1024
FIXTURE_ASSETS = {
    "d3110000-0000-4000-8000-000000000001",
    "d3110000-0000-4000-8000-000000000002",
}
FIXTURE_WINDOW_TITLES = {"Host Test Fixture 1", "Host Test Fixture 2"}
FIXTURE_READY = "Development document-host fixture enabled with opening preference"
RESTORED_COUNT = re.compile(r"(\d+) sessions restored from saved host state")
MAX_CONSECUTIVE_AX_TIMEOUTS = 3
AX_QUERY_INTERVAL_SECONDS = 0.5


class WindowObservationError(RuntimeError):
    def __init__(self, category: str, detail: str, transient: bool = False) -> None:
        super().__init__(f"{category}; {detail}")
        self.category = category
        self.transient = transient


class ProcessCapture:
    def __init__(self, process: subprocess.Popen[str], log_path: Path) -> None:
        self.log_path = log_path
        self.buffer = bytearray()
        self.console_bytes = 0
        self.fixture_ready = threading.Event()
        self.restored_sessions: int | None = None
        self.reader_error: str | None = None

        def read_output() -> None:
            try:
                assert process.stdout is not None
                for line in process.stdout:
                    encoded = line.encode("utf-8", errors="replace")
                    if len(self.buffer) < MAX_LOG_BYTES:
                        self.buffer.extend(encoded[: MAX_LOG_BYTES - len(self.buffer)])
                    if self.console_bytes < MAX_CONSOLE_BYTES:
                        printable = encoded[: MAX_CONSOLE_BYTES - self.console_bytes].decode("utf-8", errors="replace")
                        print(printable, end="", flush=True)
                        self.console_bytes += len(encoded)
                    if FIXTURE_READY in line:
                        match = RESTORED_COUNT.search(line)
                        if match:
                            self.restored_sessions = int(match.group(1))
                        self.fixture_ready.set()
            except Exception as error:  # Preserve a diagnostic if the pipe reader itself fails.
                self.reader_error = f"{type(error).__name__}: {error}"
            finally:
                marker = b"\n[output truncated at 128 KiB]\n" if len(self.buffer) >= MAX_LOG_BYTES else b""
                self.log_path.write_bytes(self.buffer + marker)

        self.thread = threading.Thread(target=read_output, name="PulseForge workspace smoke output", daemon=True)
        self.thread.start()


def fail(message: str, logs: list[Path]) -> int:
    print(f"macOS workspace-restore smoke failed: {message}", file=sys.stderr)
    for log_path in logs:
        print(f"Editor output: {log_path}", file=sys.stderr)
        if log_path.is_file():
            print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    return 1


def remove_temporary_root(root: Path, temp_parent: Path) -> str | None:
    try:
        if root.is_symlink():
            return f"refusing to remove a symlink temporary path: {root}"
        resolved_root = root.resolve(strict=True)
        resolved_parent = temp_parent.resolve(strict=True)
        if resolved_root.parent != resolved_parent or not resolved_root.name.startswith("PulseForge Workspace Restore Ω "):
            return f"refusing to remove unexpected temporary path: {resolved_root}"
        if not resolved_root.is_dir():
            return f"refusing to remove non-directory temporary path: {resolved_root}"
        shutil.rmtree(resolved_root)
        if resolved_root.exists():
            return f"temporary root still exists after removal: {resolved_root}"
        return None
    except OSError as error:
        return f"could not remove temporary root {root}: {error}"


def query_windows(process: subprocess.Popen[str], deadline: float) -> tuple[str, list[tuple[int, str]]]:
    if process.poll() is not None:
        raise WindowObservationError("EDITOR_EXITED", f"pid={process.pid}; exit_code={process.returncode}")
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise WindowObservationError("OVERALL_DEADLINE_EXPIRED", f"pid={process.pid}")
    code, output = run_osascript(
        WINDOW_QUERY,
        str(process.pid),
        min(5.0, remaining),
        controller_label="workspace native-window snapshot",
    )
    if process.poll() is not None:
        raise WindowObservationError("EDITOR_EXITED", f"pid={process.pid}; exit_code={process.returncode}")
    if code != 0:
        if has_tcc_denial(output) or "-1743" in output:
            category, transient = "AX_AUTHORIZATION_DENIED", False
        elif code == 124 or "-1712" in output or "timed out" in output.lower():
            category, transient = "AX_OBSERVATION_TIMEOUT", True
        elif "cannot get process" in output.lower() or "-600" in output:
            category, transient = "AX_TARGET_UNAVAILABLE", False
        else:
            category, transient = "AX_OBSERVATION_ERROR", False
        print(
            f"MACOS_WORKSPACE_RESTORE_AX_QUERY={category}; stage=process-window-snapshot; "
            f"editor_pid={process.pid}; observer_exit_code={code}; transient={str(transient).lower()}; "
            f"detail={output.strip()!r}"
        )
        if not transient:
            print(
                "MACOS_WORKSPACE_RESTORE_AUTOMATION_CONTEXT="
                + capture_failure_context("workspace process-window snapshot", process.pid)
            )
        raise WindowObservationError(category, f"observer_exit_code={code}; detail={output.strip()!r}", transient)
    lines = [line for line in output.splitlines() if line.strip()]
    if not lines:
        raise WindowObservationError("AX_MALFORMED_PROTOCOL", f"no process identity for PID {process.pid}")
    windows: list[tuple[int, str]] = []
    for line in lines[1:]:
        index_text, separator, title = line.partition("\t")
        if not separator or not index_text.strip().isdigit():
            raise WindowObservationError("AX_MALFORMED_PROTOCOL", f"invalid window row for PID {process.pid}: {line!r}")
        windows.append((int(index_text.strip()), title.strip()))
    process_name = lines[0].strip()
    if not process_name or "PulseForge" not in process_name:
        raise WindowObservationError(
            "AX_WRONG_PROCESS_IDENTITY", f"requested_pid={process.pid}; returned_process={process_name!r}"
        )
    return process_name, windows


def wait_for_fixture_windows(
    process: subprocess.Popen[str], capture: ProcessCapture, deadline: float, expected_restore_count: int
) -> list[tuple[int, str]]:
    next_query = 0.0
    last_snapshot: list[tuple[int, str]] = []
    last_process_name: str | None = None
    last_error: str | None = None
    attempts = 0
    consecutive_timeouts = 0
    started = time.monotonic()
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"editor exited before fixture windows appeared; exit_code={process.returncode}")
        if capture.reader_error:
            raise RuntimeError(f"editor output capture failed: {capture.reader_error}")
        if capture.fixture_ready.is_set():
            if capture.restored_sessions != expected_restore_count:
                raise RuntimeError(
                    "fixture startup reported an unexpected restored-session count; "
                    f"expected={expected_restore_count}; actual={capture.restored_sessions}"
                )
            now = time.monotonic()
            if now >= next_query:
                attempts += 1
                try:
                    process_name, snapshot = query_windows(process, deadline)
                    last_process_name = process_name
                    last_snapshot = snapshot
                    last_error = None
                    consecutive_timeouts = 0
                    next_query = now + AX_QUERY_INTERVAL_SECONDS
                except WindowObservationError as error:
                    last_error = str(error)
                    if not error.transient:
                        raise
                    consecutive_timeouts += 1
                    print(
                        f"MACOS_WORKSPACE_RESTORE_AX_RETRY=TRANSIENT_TIMEOUT; pid={process.pid}; "
                        f"attempt={attempts}; consecutive={consecutive_timeouts}; detail={error}"
                    )
                    if consecutive_timeouts >= MAX_CONSECUTIVE_AX_TIMEOUTS:
                        raise RuntimeError(
                            f"persistent Accessibility observation timeout; pid={process.pid}; attempts={attempts}; "
                            f"elapsed={time.monotonic() - started:.2f}s; last_successful_process={last_process_name!r}; "
                            f"last_successful_windows={last_snapshot!r}; last_error={last_error}"
                        ) from error
                    next_query = now + AX_QUERY_INTERVAL_SECONDS
                if process.poll() is not None:
                    raise RuntimeError(f"editor exited during native-window observation; pid={process.pid}; exit_code={process.returncode}")
                if capture.reader_error:
                    raise RuntimeError(f"editor output capture failed during native-window observation: {capture.reader_error}")
                titles = [title for _, title in last_snapshot]
                main_windows = [title for title in titles if "PulseForge Editor" in title]
                fixture_windows = [title for title in titles if title.startswith("Host Test Fixture ")]
                if (
                    len(main_windows) == 1
                    and len(fixture_windows) == len(FIXTURE_WINDOW_TITLES)
                    and set(fixture_windows) == FIXTURE_WINDOW_TITLES
                ):
                    print(
                        f"MACOS_WORKSPACE_RESTORE_WINDOWS=PASS; pid={process.pid}; process={process_name!r}; "
                        f"main={main_windows[0]!r}; fixtures={fixture_windows!r}"
                    )
                    return last_snapshot
        time.sleep(min(0.2, max(0.0, deadline - time.monotonic())))
    raise RuntimeError(
        f"overall readiness deadline expired waiting for main and both native fixture windows; pid={process.pid}; "
        f"attempts={attempts}; elapsed={time.monotonic() - started:.2f}s; "
        f"fixture_ready={capture.fixture_ready.is_set()}; restored_sessions={capture.restored_sessions}; "
        f"last_successful_process={last_process_name!r}; last_successful_windows={last_snapshot!r}; "
        f"last_error={last_error!r}"
    )


def verify_workspace(path: Path, project_id: str) -> dict[str, str]:
    if not path.is_file():
        raise RuntimeError(f"normal shutdown did not persist the workspace file: {path}")
    try:
        workspace = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"persisted workspace is not readable JSON: {error}") from error
    if not isinstance(workspace, dict):
        raise RuntimeError("persisted workspace root is not a JSON object")
    if workspace.get("project") != project_id:
        raise RuntimeError(f"workspace project identity mismatch: expected={project_id}; actual={workspace.get('project')}")
    hosts = workspace.get("hosts")
    if not isinstance(hosts, list):
        raise RuntimeError("persisted workspace has no host list")
    fixture_locations: dict[str, list[tuple[int, str]]] = {asset: [] for asset in FIXTURE_ASSETS}
    for host in hosts:
        if not isinstance(host, dict):
            continue
        role = host.get("role")
        host_id = host.get("id")
        if not isinstance(host_id, str):
            host_id = ""
        documents = host.get("documents")
        if isinstance(documents, list):
            for document in documents:
                if not isinstance(document, dict):
                    continue
                asset = document.get("asset")
                if isinstance(asset, str) and asset in fixture_locations:
                    fixture_locations[asset].append((role, host_id))
    asset_to_host: dict[str, str] = {}
    for asset, locations in fixture_locations.items():
        if len(locations) != 1:
            raise RuntimeError(
                f"fixture identity must occur exactly once in the saved workspace: {asset}; locations={locations!r}"
            )
        role, host_id = locations[0]
        if role != 1 or not host_id:
            raise RuntimeError(
                f"fixture identity is not assigned to a valid native host: {asset}; role={role!r}; host={host_id!r}"
            )
        asset_to_host[asset] = host_id
    if len(set(asset_to_host.values())) != len(FIXTURE_ASSETS):
        raise RuntimeError(f"fixture documents must belong to distinct native hosts: mapping={asset_to_host!r}")
    print(
        f"MACOS_WORKSPACE_RESTORE_SAVED=PASS; project={project_id}; "
        f"asset_to_native_host={asset_to_host!r}"
    )
    return asset_to_host


def request_normal_application_exit(
    process: subprocess.Popen[str], deadline: float
) -> None:
    code, output = run_osascript(
        QUIT_APPLICATION,
        str(process.pid),
        min(10.0, max(0.1, deadline - time.monotonic())),
    )
    if code != 0 and code != 124:
        category = "Accessibility authorization denied" if has_tcc_denial(output) else "normal application-quit request failed"
        raise RuntimeError(f"{category}; pid={process.pid}; exit_code={code}; detail={output.strip()}")
    if code == 124:
        print(f"MACOS_WORKSPACE_RESTORE_QUIT_ACTION=TIMED_OUT_WAITING_FOR_EXIT; pid={process.pid}; detail={output.strip()}")
    else:
        print(output.strip())
    try:
        exit_code = process.wait(timeout=max(0.1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"editor did not exit after the normal Command+Q application-quit request; "
            f"pid={process.pid}"
        ) from error
    if exit_code != 0:
        raise RuntimeError(f"editor exited with status {exit_code} after normal application quit")
    print(f"MACOS_WORKSPACE_RESTORE_NORMAL_SHUTDOWN=PASS; pid={process.pid}; exit_code={exit_code}")


def run_launch(
    editor: Path,
    project: Path,
    working_directory: Path,
    settings_directory: Path,
    log_path: Path,
    timeout_seconds: int,
    expected_restore_count: int,
) -> None:
    environment = os.environ.copy()
    environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(settings_directory)
    command = [str(editor), "--project", str(project), "--document-host-test-fixture"]
    print(f"MACOS_WORKSPACE_RESTORE_COMMAND={command!r}")
    print(f"MACOS_WORKSPACE_RESTORE_CWD={working_directory}")
    print(f"MACOS_WORKSPACE_RESTORE_SETTINGS={settings_directory}")
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
    capture = ProcessCapture(process, log_path)
    deadline = time.monotonic() + timeout_seconds
    stop_failure: str | None = None
    normal_shutdown = False
    try:
        windows = wait_for_fixture_windows(process, capture, deadline, expected_restore_count)
        if not windows:
            raise RuntimeError("document-host windows disappeared before normal application quit")
        if expected_restore_count:
            screenshot = capture_screenshot("macos-workspace-restore-checkpoint-restored-after-restart.png")
            print(f"MACOS_WORKSPACE_RESTORE_SCREENSHOT={screenshot}")
        request_normal_application_exit(process, deadline)
        normal_shutdown = True
    finally:
        if not normal_shutdown and process.poll() is None:
            screenshot = capture_screenshot("macos-workspace-restore-checkpoint-unexpected-failure.png")
            print(f"MACOS_WORKSPACE_RESTORE_FAILURE_SCREENSHOT={screenshot}")
        if process.poll() is None:
            if not stop_process_group(process):
                stop_failure = f"could not stop failed editor process group PID {process.pid}"
        capture.thread.join(timeout=5)
        if capture.thread.is_alive():
            raise RuntimeError(f"editor output reader remained active after PID {process.pid} exited")
        if capture.reader_error:
            raise RuntimeError(f"editor output capture failed: {capture.reader_error}")
        if stop_failure:
            raise RuntimeError(stop_failure)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    if args.timeout_seconds <= 0:
        print("timeout must be positive", file=sys.stderr)
        return 1
    try:
        editor = args.editor.resolve(strict=True)
        project = args.project.resolve(strict=True)
    except OSError as error:
        print(f"macOS workspace-restore smoke input is unavailable: {error}", file=sys.stderr)
        return 1
    if not editor.is_file() or not os.access(editor, os.X_OK):
        print(f"editor executable is missing or not executable: {editor}", file=sys.stderr)
        return 1
    if not project.is_file() or project.suffix != ".pfproj":
        print(f"project fixture is missing or invalid: {project}", file=sys.stderr)
        return 1
    try:
        project_data = json.loads(project.read_text(encoding="utf-8"))
        project_id = project_data["projectId"]
    except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        print(f"could not read the project identity required for workspace verification: {error}", file=sys.stderr)
        return 1
    if not isinstance(project_id, str) or not project_id:
        print("project fixture has no valid projectId", file=sys.stderr)
        return 1

    automation_state, automation_pid, automation_detail = probe_osascript_access()
    print(
        f"MACOS_WORKSPACE_RESTORE_OSASCRIPT_PREFLIGHT={automation_state}; "
        f"caller=/usr/bin/osascript; probe_pid={automation_pid}; detail={automation_detail!r}"
    )
    if automation_state != "OSASCRIPT_AX_APPLE_EVENTS_AUTHORIZED":
        print(
            "MACOS_WORKSPACE_RESTORE_AUTOMATION=BLOCKED; native-window acceptance NOT RUN; "
            f"reason={automation_state}; no permission prompt was requested",
            file=sys.stderr,
        )
        return 77 if automation_state in {
            "AX_UNTRUSTED",
            "APPLE_EVENTS_DENIED",
            "APPLE_EVENTS_CONSENT_REQUIRED",
            "APPLE_EVENTS_TARGET_UNAVAILABLE",
            "APPLE_EVENTS_UNSUPPORTED",
        } else 1

    args.log_directory.mkdir(parents=True, exist_ok=True)
    runner_temp = os.environ.get("RUNNER_TEMP")
    temp_parent = Path(runner_temp).resolve() if runner_temp else Path(tempfile.gettempdir())
    root = Path(tempfile.mkdtemp(prefix="PulseForge Workspace Restore Ω ", dir=temp_parent))
    working_directory = root / "Unrelated Working Directory Ω"
    settings_directory = root / "Isolated Editor Settings Ω"
    working_directory.mkdir()
    settings_directory.mkdir()
    logs = [args.log_directory / "macos-workspace-restore-first-launch.log",
            args.log_directory / "macos-workspace-restore-second-launch.log"]
    workspace_path = settings_directory / f"documents-{project_id}.json"
    result = 1
    try:
        run_launch(editor, project, working_directory, settings_directory, logs[0], args.timeout_seconds, 0)
        expected_mapping = verify_workspace(workspace_path, project_id)
        run_launch(editor, project, working_directory, settings_directory, logs[1], args.timeout_seconds, len(FIXTURE_ASSETS))
        restored_mapping = verify_workspace(workspace_path, project_id)
        if restored_mapping != expected_mapping:
            return fail(
                f"fixture-to-native-host assignments changed across the restored launch; "
                f"before={expected_mapping!r}; after={restored_mapping!r}",
                logs,
            )
        print(
            "MACOS_WORKSPACE_RESTORE_SMOKE=PASS; two separate editor processes; normal shutdown; "
            "same native fixture sessions and host identities restored; no physical pointer claim"
        )
        result = 0
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        result = fail(str(error), logs)
    finally:
        if os.environ.get("PULSEFORGE_KEEP_WORKSPACE_RESTORE_FILES") == "1":
            print(f"MACOS_WORKSPACE_RESTORE_TEMP_ROOT={root}")
        else:
            cleanup_error = remove_temporary_root(root, temp_parent)
            if cleanup_error:
                print(f"macOS workspace-restore smoke cleanup failed: {cleanup_error}", file=sys.stderr)
                result = 1
    return result


if __name__ == "__main__":
    raise SystemExit(main())
