#!/usr/bin/env python3
"""Run the existing Debug synthetic document-host round-trip in a native macOS editor process."""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from MacOSAutomationClient import capture_failure_context
from MacOSDirectAX import run_direct_ax
from MacOSScreenshot import capture_screenshot

MAX_LOG_BYTES = 128 * 1024
MAX_CONSOLE_BYTES = 64 * 1024
TCC_DENIALS = ("not allowed assistive access", "not authorized to send apple events", "(-1743)")

# Workspace restoration retains its previously validated System Events client.
# Document-host close and native dialogs use the direct Swift AX client below.
WINDOW_QUERY = '''on run argv
    set targetPID to item 1 of argv as integer
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        tell targetProcess
            set processName to name as text
            set windowNames to name of every window
            set indexedWindowNames to {}
            repeat with windowIndex from 1 to count of windowNames
                set end of indexedWindowNames to (windowIndex as text) & tab & (item windowIndex of windowNames as text)
            end repeat
            set AppleScript's text item delimiters to linefeed
            set windowSnapshot to indexedWindowNames as text
            set AppleScript's text item delimiters to ""
            return processName & linefeed & windowSnapshot
        end tell
    end tell
end run
'''

QUIT_APPLICATION = '''on run argv
    set targetPID to item 1 of argv as integer
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set frontmost of targetProcess to true
        keystroke "q" using {command down}
        return "MACOS_APPLICATION_QUIT_KEY=PASS; pid=" & (targetPID as text)
    end tell
end run
'''
SUCCESS_MARKERS = (
    "Document tab reattachment UI fixture passed:",
    "Document tab native-window round-trip 3/3 passed: the same session was dragged into the main strip and back into a native host",
    "Document tab repeated-transfer UI fixture passed: the same session completed 3 native/main round trips and grouping operations.",
    "Document tab group UI fixture passed for round trip 3: the same session joined an existing native tab stack and the empty source host was retired.",
    "Document tab native-group tear-off UI fixture passed: the tab entered a new native host and the original group remained intact.",
)


class BoundedOutput:
    def __init__(self, process: subprocess.Popen[str], log_path: Path):
        self.buffer = bytearray()
        self.log_path = log_path
        self.markers: set[str] = set()
        self.failure_count = 0
        self.failure_samples: list[str] = []
        self.console_bytes = 0
        self.console_output_failed = False
        self.total_bytes = 0
        self.truncated = False
        self.reader_error: str | None = None
        self.fixture_ready = threading.Event()
        self.finished = threading.Event()

        def read() -> None:
            assert process.stdout is not None
            try:
                for line in process.stdout:
                    encoded = line.encode("utf-8", errors="replace")
                    self.total_bytes += len(encoded)
                    if len(self.buffer) < MAX_LOG_BYTES:
                        self.buffer.extend(encoded[: MAX_LOG_BYTES - len(self.buffer)])
                    else:
                        self.truncated = True
                    if self.console_bytes < MAX_CONSOLE_BYTES and not self.console_output_failed:
                        printable = encoded[: MAX_CONSOLE_BYTES - self.console_bytes]
                        try:
                            print(printable.decode("utf-8", errors="replace"), end="")
                            self.console_bytes += len(printable)
                        except (OSError, ValueError) as error:
                            self.console_output_failed = True
                            self.reader_error = self.reader_error or (
                                f"could not forward editor output to the workflow log: {type(error).__name__}: {error}"
                            )
                    for marker in SUCCESS_MARKERS:
                        if marker in line:
                            self.markers.add(marker)
                    lowered = line.lower()
                    if "development document-host fixture enabled" in lowered:
                        self.fixture_ready.set()
                    if "document tab" in lowered and (" failed" in lowered or " timed out" in lowered or "[error]" in lowered):
                        self.failure_count += 1
                        if len(self.failure_samples) < 16:
                            self.failure_samples.append(line.rstrip()[:2048])
            except Exception as error:
                self.reader_error = f"{type(error).__name__}: {error}"
            finally:
                try:
                    self.truncated = self.total_bytes > len(self.buffer)
                    output = bytes(self.buffer)
                    if self.truncated:
                        suffix = b"\n[document-host output truncated; marker and failure scanning continued]\n"
                        output = output[: MAX_LOG_BYTES - len(suffix)] + suffix
                    log_path.write_bytes(output)
                except OSError as error:
                    self.reader_error = self.reader_error or f"could not write bounded editor log: {error}"
                self.finished.set()

        self.thread = threading.Thread(target=read, daemon=True)
        self.thread.start()


def stop_process_group(process: subprocess.Popen[str]) -> bool:
    process_group = process.pid
    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        return process.poll() is not None
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process_group, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            return False
    group_deadline = time.monotonic() + 0.5
    while time.monotonic() < group_deadline:
        try:
            os.killpg(process_group, 0)
        except ProcessLookupError:
            return process.poll() is not None
        time.sleep(0.025)
    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        return process.poll() is not None
    group_deadline = time.monotonic() + 5.0
    while time.monotonic() < group_deadline:
        try:
            os.killpg(process_group, 0)
        except ProcessLookupError:
            return process.poll() is not None
        time.sleep(0.05)
    return False


def run_osascript(
    script: str,
    process_id: str,
    timeout: float,
    *script_arguments: str,
    controller_label: str | None = None,
) -> tuple[int, str]:
    command = ["/usr/bin/osascript", "-", process_id, *script_arguments]
    bounded_timeout = max(0.1, timeout)
    if controller_label is not None:
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
        except OSError as error:
            return 127, f"could not launch /usr/bin/osascript: {type(error).__name__}: {error}"
        print(
            "MACOS_DOCUMENT_HOST_AX_CLIENT="
            f"operation={controller_label}; executable=/usr/bin/osascript; pid={process.pid}; "
            f"parent_pid={os.getpid()}; parent_executable={Path(sys.executable).resolve()}; target_pid={process_id}"
        )
        try:
            output, _ = process.communicate(input=script, timeout=bounded_timeout)
            return process.returncode, output
        except subprocess.TimeoutExpired as error:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                return 124, f"timed-out osascript client PID {process.pid} did not terminate after SIGKILL"
            output = error.stdout or ""
            if isinstance(output, bytes):
                output = output.decode("utf-8", errors="replace")
            return 124, output + "\nAppleScript operation timed out\n"
    try:
        result = subprocess.run(
            command,
            input=script,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=bounded_timeout,
            check=False,
        )
        return result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        return 124, output + "\nAppleScript operation timed out\n"


def has_tcc_denial(output: str) -> bool:
    lowered = output.lower()
    return any(message in lowered for message in TCC_DENIALS)


def close_failure_diagnostics(
    process_id: str,
    window_arguments: tuple[str, str],
    deadline: float,
) -> list[str]:
    remaining = deadline - time.monotonic()
    if remaining <= 0.1:
        return ["DIRECT_AX_SNAPSHOT=DEADLINE_EXHAUSTED"]
    code, output, error = run_direct_ax(
        int(process_id), operation="snapshot", window_title=window_arguments[1], timeout=min(5.0, remaining - 0.05)
    )
    result = output + error if code == 0 else f"DIRECT_AX_SNAPSHOT=ERROR; exit_code={code}; stdout={output!r}; stderr={error!r}"
    print(f"MACOS_DOCUMENT_HOST_FAILURE_DIAGNOSTIC={result}")
    diagnostics = [result]
    return diagnostics


def capture_failure_screenshot(process: subprocess.Popen[str]) -> str:
    if process.poll() is not None:
        return f"NOT_CAPTURED_PROCESS_ALREADY_EXITED; exit_code={process.returncode}"
    context = capture_failure_context("document-host-AX-close", process.pid)
    print(f"MACOS_DOCUMENT_HOST_AUTOMATION_CONTEXT={context!r}")
    screenshot = capture_screenshot("macos-document-host-checkpoint-unexpected-failure.png")
    return screenshot


def classify_accessibility_failure(code: int, output: str) -> str:
    lowered = output.lower()
    if "trusted=false" in lowered:
        return "DIRECT_AX_CLIENT_UNTRUSTED"
    if code == 124 or "-1712" in lowered or "timed out" in lowered:
        return "AX_QUERY_TIMEOUT"
    if "-25202" in lowered or "invalid ui element" in lowered or "stale ax element" in lowered:
        return "STALE_AX_ELEMENT"
    if "-25205" in lowered:
        return "AX_ATTRIBUTE_UNSUPPORTED"
    if "-25212" in lowered:
        return "AX_ATTRIBUTE_HAS_NO_VALUE"
    if "axclosebutton is missing" in lowered or "missing value" in lowered:
        return "AX_ATTRIBUTE_MISSING"
    if "-25204" in lowered:
        return "AX_COMMUNICATION_ERROR"
    if "-1719" in lowered:
        return "AX_INVALID_INDEX"
    if "-1728" in lowered or "can't get" in lowered or "can’t get" in lowered:
        return "AX_ELEMENT_LOOKUP_ERROR"
    return "AX_ACCESSIBILITY_ERROR"


def fail(message: str, log_path: Path) -> int:
    print(f"macOS document-host smoke failed: {message}", file=sys.stderr)
    print(f"Editor output: {log_path}", file=sys.stderr)
    if log_path.is_file():
        print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    return 1


def remove_temporary_root(root: Path, parent: Path) -> str | None:
    try:
        if root.is_symlink():
            return f"refusing to remove symlink document-host test root: {root}"
        resolved_root = root.resolve(strict=True)
        resolved_parent = parent.resolve(strict=True)
        if resolved_root.parent != resolved_parent or not resolved_root.name.startswith("PulseForge Document Hosts Ω "):
            return f"refusing to remove unexpected document-host test root: {resolved_root}"
        shutil.rmtree(resolved_root)
        return None
    except OSError as error:
        return f"could not remove document-host test root {root}: {error}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    args = parser.parse_args()

    editor = args.editor.resolve(strict=True)
    project = args.project.resolve(strict=True)
    if not editor.is_file() or not os.access(editor, os.X_OK):
        return fail(f"editor executable is missing or not executable: {editor}", args.log_directory / "document-host-smoke.log")
    if not project.is_file() or project.suffix != ".pfproj":
        return fail(f"project fixture is missing or invalid: {project}", args.log_directory / "document-host-smoke.log")
    if args.timeout_seconds <= 0:
        return fail("timeout must be positive", args.log_directory / "document-host-smoke.log")

    args.log_directory.mkdir(parents=True, exist_ok=True)
    runner_temp = os.environ.get("RUNNER_TEMP")
    temp_parent = Path(runner_temp).resolve() if runner_temp else Path(tempfile.gettempdir())
    root = Path(tempfile.mkdtemp(prefix="PulseForge Document Hosts Ω ", dir=temp_parent))
    working_directory = root / "Unrelated Working Directory Ω"
    settings_directory = root / "Isolated Editor Settings Ω"
    working_directory.mkdir()
    settings_directory.mkdir()
    log_path = args.log_directory / "macos-document-host-smoke.log"
    environment = os.environ.copy()
    environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(settings_directory)
    command = [
        str(editor),
        "--project",
        str(project),
        "--document-host-test-fixture",
        "--document-host-test-tab-reattach",
    ]
    print(f"MACOS_DOCUMENT_HOST_COMMAND={command!r}")
    print(f"MACOS_DOCUMENT_HOST_CWD={working_directory}")
    process = subprocess.Popen(
        command,
        cwd=working_directory,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    capture = BoundedOutput(process, log_path)
    deadline = time.monotonic() + args.timeout_seconds
    fixture_ready = False
    observed_native_windows = False
    next_window_query = 0.0
    observed_process_name = "<unknown>"
    observed_windows: list[tuple[int, str]] = []
    fixture_titles: list[str] = []
    direct_ax_client_untrusted = False
    failures: list[str] = []

    try:
        while time.monotonic() < deadline:
            fixture_ready = capture.fixture_ready.is_set()
            if capture.reader_error:
                failures.append(f"editor output reader failed: {capture.reader_error}")
                break
            if capture.failure_count:
                failures.extend(capture.failure_samples)
                if not failures:
                    failures.append(
                        f"editor emitted {capture.failure_count} document-tab failures; samples={capture.failure_samples!r}"
                    )
                break

            # Accessibility queries can block while the editor is still creating its
            # first project window. Wait for the fixture's explicit ready log before
            # asking System Events to inspect native windows.
            if not fixture_ready:
                if process.poll() is not None:
                    break
                capture.finished.wait(timeout=min(0.1, max(0.01, deadline - time.monotonic())))
                continue

            now = time.monotonic()
            if (
                not observed_native_windows
                and all(marker in capture.markers for marker in SUCCESS_MARKERS)
                and now >= next_window_query
            ):
                next_window_query = now + 2.0
                query_code, query_output, query_error = run_direct_ax(
                    process.pid, operation="window-list", timeout=min(12.0, max(0.1, deadline - now))
                )
                query_output += query_error
                print(f"MACOS_DOCUMENT_HOST_NATIVE_WINDOW_SNAPSHOT={query_output.strip()!r}")
                if query_code != 0:
                    if "trusted=false" in query_output:
                        direct_ax_client_untrusted = True
                        break
                    failures.append(f"Native window-list query failed; exit_code={query_code}; {query_output.strip()}")
                    break
                snapshot_lines = [entry for entry in query_output.splitlines() if entry.startswith("DIRECT_CG_WINDOW ")]
                target_line = next((entry for entry in query_output.splitlines() if entry.startswith("DIRECT_AX_TARGET_PROCESS ")), "")
                observed_process_name = target_line.split("name=", 1)[1].strip() if "name=" in target_line else f"pid={process.pid}"
                observed_windows = []
                for entry in snapshot_lines:
                    fields = dict(part.split("=", 1) for part in entry.split("; ") if "=" in part)
                    index_text = fields.get("index", "")
                    window_title = fields.get("title", "")
                    if index_text.isdigit():
                        observed_windows.append((int(index_text), window_title))
                fixture_titles = [title for _, title in observed_windows if "Host Test Fixture" in title]
                if len(observed_windows) >= 2 and fixture_titles:
                    observed_native_windows = True

            if all(marker in capture.markers for marker in SUCCESS_MARKERS) and observed_native_windows:
                print(
                    "MACOS_DOCUMENT_HOST_NATIVE_WINDOWS=PASS; pid="
                    + str(process.pid)
                    + "; process="
                    + observed_process_name
                    + "; indexed windows="
                    + repr(observed_windows)
                )
                break
            if process.poll() is not None:
                break

        if direct_ax_client_untrusted:
            print(
                "MACOS_DOCUMENT_HOST_FAILURE_SCREENSHOT="
                + capture_failure_screenshot(process)
            )
            stopped = stop_process_group(process)
            capture.thread.join(timeout=5)
            if not stopped or capture.thread.is_alive():
                return fail("cleanup failed after the direct AX client reported itself untrusted", log_path)
            print("MACOS_DOCUMENT_HOST_ACCESSIBILITY=UNAVAILABLE; direct Swift AX client reported itself untrusted; "
                  "native-window verification NOT RUN; acceptance blocked")
            return 77
        if failures:
            print(
                "MACOS_DOCUMENT_HOST_FAILURE_SCREENSHOT="
                + capture_failure_screenshot(process)
            )
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail("fixture reported failure: " + "; ".join(failures), log_path)
        if not observed_native_windows:
            print(
                "MACOS_DOCUMENT_HOST_FAILURE_SCREENSHOT="
                + capture_failure_screenshot(process)
            )
            stop_process_group(process)
            capture.thread.join(timeout=5)
            if not fixture_ready:
                return fail("editor did not report that the document-host fixture reached its UI-ready state", log_path)
            return fail("Accessibility did not observe a separate Host Test Fixture native window", log_path)

        missing = [marker for marker in SUCCESS_MARKERS if marker not in capture.markers]
        if missing:
            print(
                "MACOS_DOCUMENT_HOST_FAILURE_SCREENSHOT="
                + capture_failure_screenshot(process)
            )
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail("fixture did not produce required round-trip evidence: " + repr(missing), log_path)

        main_windows = [(index, title) for index, title in observed_windows if "PulseForge Editor" in title]
        if len(main_windows) != 1:
            print(
                "MACOS_DOCUMENT_HOST_FAILURE_SCREENSHOT="
                + capture_failure_screenshot(process)
            )
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail(
                "Accessibility did not identify exactly one main editor window by title; "
                f"pid={process.pid}; process={observed_process_name}; indexed windows={observed_windows!r}",
                log_path,
            )
        main_window_index, main_window_name = main_windows[0]
        print(
            f"MACOS_DOCUMENT_HOST_PROCESS=PASS; pid={process.pid}; process={observed_process_name}; "
            f"main_window_index={main_window_index}; main_window={main_window_name!r}; "
            f"all_windows={observed_windows!r}"
        )
        screenshot = capture_screenshot("macos-editor-checkpoint-loaded-scene-and-separate-document-windows.png")
        print(
            "MACOS_DOCUMENT_HOST_UI_SCREENSHOT="
            f"loaded_scene_viewport_and_separate_native_documents; {screenshot}"
        )

        diagnostic_arguments = (str(main_window_index), main_window_name)
        diagnostic_output: list[str] = []
        exit_code: int | None = None
        preflight_timeout = min(12.0, max(0.1, deadline - time.monotonic()))
        preflight_code, preflight_output, preflight_stderr = run_direct_ax(
            process.pid,
            operation="snapshot",
            window_title=main_window_name,
            timeout=preflight_timeout,
        )
        print(
            f"MACOS_DOCUMENT_HOST_CLOSE_PRE_ACTION_PREFLIGHT={'PASS' if preflight_code == 0 else 'FAILED'}; "
            f"pid={process.pid}; window={main_window_name!r}; exit_code={preflight_code}; "
            f"stdout={preflight_output.strip()!r}; stderr={preflight_stderr.strip()!r}; actual_close_request=still-required"
        )
        # Attempt the actual semantic close action directly. The read-only AX
        # inspection above is diagnostic only. It cannot prevent or satisfy
        # the required close action, even when the query itself is unavailable.
        close_code, close_output, close_stderr = run_direct_ax(
            process.pid,
            operation="close-main-window",
            timeout=min(12.0, max(0.1, deadline - time.monotonic())),
        )
        close_output += close_stderr
        print(f"MACOS_DOCUMENT_HOST_DIRECT_AX_CLOSE={close_output.strip()!r}")
        if "process_group_cleanup=failed" in close_output:
            diagnostic_output = close_failure_diagnostics(str(process.pid), diagnostic_arguments, deadline)
            screenshot_output = capture_failure_screenshot(process)
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail(
                f"direct AX client cleanup failed after close dispatch; client details={close_output.strip()}; "
                f"diagnostics={diagnostic_output!r}; {screenshot_output}",
                log_path,
            )
        if close_code != 0:
            close_category = classify_accessibility_failure(close_code, close_output)
            print(
                "MACOS_DOCUMENT_HOST_AX_CLOSE_RESULT="
                f"{close_category}; pid={process.pid}; window={main_window_name!r}; "
                f"exit_code={close_code}; detail={close_output.strip()}"
            )
            if "trusted=false" in close_output:
                print(
                    "MACOS_DOCUMENT_HOST_FAILURE_SCREENSHOT="
                    + capture_failure_screenshot(process)
                )
                stop_process_group(process)
                capture.thread.join(timeout=5)
                print(
                    "MACOS_DOCUMENT_HOST_ACCESSIBILITY=UNAVAILABLE; native-window close request NOT RUN; "
                    f"reason=direct Swift AX client reported itself untrusted; detail={close_output.strip()}; acceptance blocked"
                )
                return 77
            if "DIRECT_AX_DISPATCH attempted=true" in close_output:
                try:
                    exit_code = process.wait(timeout=min(15.0, max(0.1, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    exit_code = None
                if exit_code == 0 and all(marker in capture.markers for marker in SUCCESS_MARKERS):
                    print(
                        "MACOS_DOCUMENT_HOST_CLOSE_PRESS=UNCERTAIN_AX_RESULT_RECONCILED_AFTER_NORMAL_EXIT; "
                        f"pid={process.pid}; window={main_window_name!r}; fixture completed and process exited 0; "
                        f"detail={close_output.strip()}"
                    )
                    close_code = 0
                else:
                    diagnostic_output = close_failure_diagnostics(
                        str(process.pid), diagnostic_arguments, deadline
                    )
                    screenshot_output = capture_failure_screenshot(process)
                    stop_process_group(process)
                    capture.thread.join(timeout=5)
                    return fail(
                        f"Accessibility main-window close operation {close_category} while the editor remained open; "
                        + f"pid={process.pid}; process={observed_process_name}; window={main_window_name!r}; "
                        + f"exit_code={close_code}; "
                        + repr(diagnostic_output)
                        + "; "
                        + screenshot_output
                        + "; "
                        + close_output.strip(),
                        log_path,
                    )
            if close_code != 0 and (close_code != 124 and close_category != "AX_QUERY_TIMEOUT"):
                diagnostic_output = close_failure_diagnostics(
                    str(process.pid), diagnostic_arguments, deadline
                )
                screenshot_output = capture_failure_screenshot(process)
                stop_process_group(process)
                capture.thread.join(timeout=5)
                return fail(
                    f"could not request normal main-window close ({close_category}); "
                    f"pid={process.pid}; process={observed_process_name}; window={main_window_name!r}; "
                    f"exit_code={close_code}; "
                    f"diagnostics={diagnostic_output!r}; {screenshot_output}; detail={close_output.strip()}",
                    log_path,
                )
        else:
            print(close_output.strip())
        try:
            if exit_code is None:
                exit_code = process.wait(timeout=max(0.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            diagnostic_output = close_failure_diagnostics(
                str(process.pid), diagnostic_arguments, deadline
            )
            screenshot_output = capture_failure_screenshot(process)
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail(
                "editor did not exit after the native main-window close request before timeout; "
                f"pid={process.pid}; process={observed_process_name}; window={main_window_name!r}; "
                f"diagnostics={diagnostic_output!r}; {screenshot_output}",
                log_path,
            )
        capture.thread.join(timeout=5)
        if capture.thread.is_alive():
            return fail("editor output reader remained active after process exit", log_path)
        if capture.reader_error:
            return fail(f"editor output reader failed: {capture.reader_error}", log_path)
        if exit_code != 0:
            return fail(f"editor exited with status {exit_code}", log_path)
        if any(marker not in capture.markers for marker in SUCCESS_MARKERS):
            return fail("fixture success evidence was incomplete at shutdown", log_path)
        print(
            "MACOS_DOCUMENT_HOST_SMOKE=PASS; native NSWindow observed; synthetic fixture preserved one session through "
            "native/main/native transfers and grouping; normal window-close shutdown; no physical pointer claim"
        )
        return 0
    finally:
        process_group_stopped = stop_process_group(process)
        capture.thread.join(timeout=5)
        if capture.thread.is_alive() and process.stdout is not None:
            try:
                process.stdout.close()
            except (OSError, ValueError):
                pass
            capture.thread.join(timeout=2)
        if not process_group_stopped or capture.thread.is_alive():
            raise RuntimeError(
                f"document-host child/process group or output reader remains active; "
                f"pid={process.pid}; temporary root preserved at {root}"
            )
        if os.environ.get("PULSEFORGE_KEEP_DOCUMENT_HOST_SMOKE_FILES") == "1":
            print(f"MACOS_DOCUMENT_HOST_TEMP_ROOT={root}")
        else:
            cleanup_error = remove_temporary_root(root, temp_parent)
            if cleanup_error:
                raise RuntimeError(f"MACOS_DOCUMENT_HOST_CLEANUP=FAILED; {cleanup_error}")


if __name__ == "__main__":
    raise SystemExit(main())
