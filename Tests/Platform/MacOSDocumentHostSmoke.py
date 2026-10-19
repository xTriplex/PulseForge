#!/usr/bin/env python3
"""Run the existing Debug synthetic document-host round-trip in a native macOS editor process."""

from __future__ import annotations

import argparse
import os
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

MAX_LOG_BYTES = 128 * 1024
MAX_CONSOLE_BYTES = 64 * 1024
TCC_DENIALS = ("not allowed assistive access", "not authorized to send apple events", "(-1719)", "(-1743)")

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

MAIN_WINDOW_SHEET_SNAPSHOT = '''on run argv
    set targetPID to item 1 of argv as integer
    set expectedWindowName to item 2 of argv as text
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        tell targetProcess
            set targetWindow to first window whose name is expectedWindowName
            set windowTitle to name of targetWindow
            if windowTitle is not expectedWindowName then error "Main window changed before sheet snapshot: " & quoted form of windowTitle
            try
                set sheetNames to name of every sheet of targetWindow
                return "MACOS_DOCUMENT_HOST_MAIN_SHEETS=PASS; pid=" & (targetPID as text) & "; window=" & quoted form of windowTitle & "; sheets=" & (sheetNames as text)
            on error errorMessage number errorNumber
                return "MACOS_DOCUMENT_HOST_MAIN_SHEETS=UNAVAILABLE; pid=" & (targetPID as text) & "; window=" & quoted form of windowTitle & "; error=" & errorMessage & " (" & (errorNumber as text) & ")"
            end try
        end tell
    end tell
end run
'''

CLOSE_BUTTON_ACTIONS = '''on run argv
    set targetPID to item 1 of argv as integer
    set expectedWindowName to item 2 of argv as text
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set frontmost of targetProcess to true
        tell targetProcess
            set targetWindow to first window whose name is expectedWindowName
            set windowTitle to name of targetWindow
            if windowTitle is not expectedWindowName then error "Main window changed before close-action query: " & quoted form of windowTitle
            set windowRole to role of targetWindow
            set windowSubrole to subrole of targetWindow
            set closeControl to value of attribute "AXCloseButton" of targetWindow
            if closeControl is missing value then error "AXCloseButton is missing for PID " & (targetPID as text) & ", window=" & quoted form of windowTitle
            set closeRole to role of closeControl
            set closeSubrole to subrole of closeControl
            set availableActions to actions of closeControl
            if not (availableActions contains "AXPress") then
                error "AXCloseButton does not advertise AXPress for PID " & (targetPID as text) & ", window=" & ¬
                    quoted form of windowTitle & "; actions=" & (availableActions as text)
            end if
            return "MACOS_DOCUMENT_HOST_CLOSE_ACTIONS=PASS; pid=" & (targetPID as text) & "; window=" & quoted form of windowTitle & "; window role/subrole=" & windowRole & "/" & windowSubrole & "; close control role/subrole=" & closeRole & "/" & closeSubrole & "; actions=" & (availableActions as text)
        end tell
    end tell
end run
'''

MAIN_WINDOW_CLOSE = '''on run argv
    set targetPID to item 1 of argv as integer
    set expectedWindowName to item 2 of argv as text
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set frontmost of targetProcess to true
        tell targetProcess
            set targetWindow to first window whose name is expectedWindowName
            set windowTitle to name of targetWindow
            if windowTitle is not expectedWindowName then error "Main-window close target changed to " & quoted form of windowTitle
            set closeControl to value of attribute "AXCloseButton" of targetWindow
            if closeControl is missing value then error "AXCloseButton is missing for PID " & (targetPID as text) & ", window=" & quoted form of windowTitle
            try
                perform action "AXPress" of closeControl
            on error errorMessage number errorNumber
                error "AXPress failed for PID " & (targetPID as text) & ", window=" & quoted form of windowTitle & ": " & errorMessage & " (" & (errorNumber as text) & ")"
            end try
            return "MACOS_DOCUMENT_HOST_CLOSE_PRESS=RETURNED; pid=" & (targetPID as text) & "; window=" & quoted form of windowTitle & "; target=AXCloseButton; action=AXPress"
        end tell
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
        self.lines: queue.Queue[str | None] = queue.Queue(maxsize=128)
        self.buffer = bytearray()
        self.log_path = log_path
        self.markers: set[str] = set()
        self.failures: list[str] = []
        self.console_bytes = 0

        def read() -> None:
            assert process.stdout is not None
            for line in process.stdout:
                encoded = line.encode("utf-8", errors="replace")
                if len(self.buffer) < MAX_LOG_BYTES:
                    self.buffer.extend(encoded[: MAX_LOG_BYTES - len(self.buffer)])
                if self.console_bytes < MAX_CONSOLE_BYTES:
                    printable = encoded[: MAX_CONSOLE_BYTES - self.console_bytes].decode("utf-8", errors="replace")
                    print(printable, end="")
                    self.console_bytes += len(encoded)
                for marker in SUCCESS_MARKERS:
                    if marker in line:
                        self.markers.add(marker)
                lowered = line.lower()
                if "document tab" in lowered and (" failed" in lowered or " timed out" in lowered or "[error]" in lowered):
                    if len(self.failures) < 16:
                        self.failures.append(line.rstrip())
                try:
                    self.lines.put_nowait(line)
                except queue.Full:
                    try:
                        self.lines.get_nowait()
                    except queue.Empty:
                        pass
                    self.lines.put_nowait(line)
            try:
                self.lines.put_nowait(None)
            except queue.Full:
                try:
                    self.lines.get_nowait()
                except queue.Empty:
                    pass
                self.lines.put_nowait(None)
            log_path.write_bytes(self.buffer)

        self.thread = threading.Thread(target=read, daemon=True)
        self.thread.start()


def stop_process_group(process: subprocess.Popen[str]) -> bool:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            return False
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    return process.poll() is not None


def run_osascript(
    script: str,
    process_id: str,
    timeout: float,
    *script_arguments: str,
) -> tuple[int, str]:
    try:
        result = subprocess.run(
            ["/usr/bin/osascript", "-", process_id, *script_arguments],
            input=script,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=max(0.1, timeout),
            check=False,
        )
        return result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        return 124, output + "\nAppleScript operation timed out\n"


def close_failure_diagnostics(
    process_id: str,
    window_arguments: tuple[str, str],
    deadline: float,
) -> list[str]:
    diagnostics: list[str] = []
    for label, script, timeout in (
        ("MAIN_SHEETS", MAIN_WINDOW_SHEET_SNAPSHOT, 3.0),
        ("CLOSE_ACTIONS", CLOSE_BUTTON_ACTIONS, 3.0),
    ):
        code, output = run_osascript(
            script,
            process_id,
            min(timeout, max(0.1, deadline - time.monotonic())),
            *window_arguments,
        )
        if code == 0:
            result = output.strip()
        else:
            state = "TCC_DENIAL" if has_tcc_denial(output) else "TIMED_OUT" if code == 124 else "ERROR"
            result = f"{label}={state}; exit_code={code}; detail={output.strip()}"
        diagnostics.append(result)
        print(f"MACOS_DOCUMENT_HOST_FAILURE_DIAGNOSTIC={result}")
    return diagnostics


def has_tcc_denial(output: str) -> bool:
    lowered = output.lower()
    return any(message in lowered for message in TCC_DENIALS)


def fail(message: str, log_path: Path) -> int:
    print(f"macOS document-host smoke failed: {message}", file=sys.stderr)
    print(f"Editor output: {log_path}", file=sys.stderr)
    if log_path.is_file():
        print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    return 1


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
    saw_tcc_denial = False
    failures: list[str] = []

    try:
        while time.monotonic() < deadline:
            try:
                line = capture.lines.get(timeout=min(1.0, max(0.01, deadline - time.monotonic())))
            except queue.Empty:
                line = ""
            if line is None:
                break
            if line:
                lowered = line.lower()
                if "development document-host fixture enabled" in lowered:
                    fixture_ready = True

            if capture.failures:
                failures.extend(capture.failures)
                break

            # Accessibility queries can block while the editor is still creating its
            # first project window. Wait for the fixture's explicit ready log before
            # asking System Events to inspect native windows.
            if not fixture_ready:
                continue

            now = time.monotonic()
            if not observed_native_windows and now >= next_window_query:
                next_window_query = now + 1.0
                query_code, query_output = run_osascript(
                    WINDOW_QUERY, str(process.pid), timeout=min(5.0, max(0.1, deadline - now))
                )
                if query_code != 0:
                    if has_tcc_denial(query_output):
                        saw_tcc_denial = True
                        break
                    failures.append(f"Accessibility window query failed: {query_output.strip()}")
                    break
                snapshot_lines = [entry for entry in query_output.splitlines() if entry.strip()]
                observed_process_name = snapshot_lines[0].strip() if snapshot_lines else "<missing>"
                observed_windows = []
                for entry in snapshot_lines[1:]:
                    index_text, separator, window_title = entry.partition("\t")
                    if separator and index_text.strip().isdigit():
                        observed_windows.append((int(index_text.strip()), window_title.strip()))
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

        if saw_tcc_denial:
            stopped = stop_process_group(process)
            capture.thread.join(timeout=5)
            if not stopped or capture.thread.is_alive():
                return fail("TCC denial occurred and the editor process/output reader did not stop cleanly", log_path)
            print("MACOS_DOCUMENT_HOST_ACCESSIBILITY=UNAVAILABLE; native-window verification NOT RUN; acceptance blocked")
            return 77
        if failures:
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail("fixture reported failure: " + "; ".join(failures), log_path)
        if not observed_native_windows:
            stop_process_group(process)
            capture.thread.join(timeout=5)
            if not fixture_ready:
                return fail("editor did not report that the document-host fixture reached its UI-ready state", log_path)
            return fail("Accessibility did not observe a separate Host Test Fixture native window", log_path)

        missing = [marker for marker in SUCCESS_MARKERS if marker not in capture.markers]
        if missing:
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail("fixture did not produce required round-trip evidence: " + repr(missing), log_path)

        main_windows = [(index, title) for index, title in observed_windows if "PulseForge Editor" in title]
        if len(main_windows) != 1:
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

        diagnostic_arguments = (main_window_name,)
        diagnostic_output: list[str] = []
        exit_code: int | None = None
        close_preflight_code, close_preflight_output = run_osascript(
            CLOSE_BUTTON_ACTIONS,
            str(process.pid),
            min(5.0, max(0.1, deadline - time.monotonic())),
            *diagnostic_arguments,
        )
        if close_preflight_code != 0:
            if has_tcc_denial(close_preflight_output):
                stop_process_group(process)
                capture.thread.join(timeout=5)
                print(
                    "MACOS_DOCUMENT_HOST_ACCESSIBILITY=UNAVAILABLE; native-window close request NOT RUN; "
                    f"reason=TCC denial; detail={close_preflight_output.strip()}; acceptance blocked"
                )
                return 77
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail(
                "Accessibility could not inspect the semantic AXCloseButton before closing; "
                f"pid={process.pid}; process={observed_process_name}; window={main_window_name!r}; "
                f"exit_code={close_preflight_code}; detail={close_preflight_output.strip()}",
                log_path,
            )
        print(close_preflight_output.strip())
        time.sleep(min(0.5, max(0.0, deadline - time.monotonic())))
        close_code, close_output = run_osascript(
            MAIN_WINDOW_CLOSE,
            str(process.pid),
            min(10.0, max(0.1, deadline - time.monotonic())),
            *diagnostic_arguments,
        )
        if close_code != 0:
            if has_tcc_denial(close_output):
                stop_process_group(process)
                capture.thread.join(timeout=5)
                print(
                    "MACOS_DOCUMENT_HOST_ACCESSIBILITY=UNAVAILABLE; native-window close request NOT RUN; "
                    f"reason=TCC denial; detail={close_output.strip()}; acceptance blocked"
                )
                return 77
            if close_code == 124:
                try:
                    exit_code = process.wait(timeout=min(10.0, max(0.1, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    exit_code = None
                if exit_code == 0:
                    print(
                        "MACOS_DOCUMENT_HOST_CLOSE_PRESS=TIMED_OUT_AFTER_NORMAL_EXIT; "
                        f"pid={process.pid}; window={main_window_name!r}; process exited normally after AXPress request; "
                        f"diagnostics={diagnostic_output!r}; detail={close_output.strip()}"
                    )
                else:
                    diagnostic_output = close_failure_diagnostics(
                        str(process.pid), diagnostic_arguments, deadline
                    )
                    stop_process_group(process)
                    capture.thread.join(timeout=5)
                    return fail(
                        "Accessibility AXPress timed out while the editor remained open; "
                        + f"pid={process.pid}; process={observed_process_name}; window={main_window_name!r}; "
                        + repr(diagnostic_output)
                        + "; "
                        + close_output.strip(),
                        log_path,
                    )
            else:
                diagnostic_output = close_failure_diagnostics(
                    str(process.pid), diagnostic_arguments, deadline
                )
                stop_process_group(process)
                capture.thread.join(timeout=5)
                return fail(
                    "could not request normal main-window close; "
                    f"pid={process.pid}; process={observed_process_name}; window={main_window_name!r}; "
                    f"diagnostics={diagnostic_output!r}; detail={close_output.strip()}",
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
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail(
                "editor did not exit after the native main-window close request before timeout; "
                f"pid={process.pid}; process={observed_process_name}; window={main_window_name!r}; "
                f"diagnostics={diagnostic_output!r}",
                log_path,
            )
        capture.thread.join(timeout=5)
        if capture.thread.is_alive():
            return fail("editor output reader remained active after process exit", log_path)
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
        if process.poll() is None:
            stop_process_group(process)
        capture.thread.join(timeout=5)
        if os.environ.get("PULSEFORGE_KEEP_DOCUMENT_HOST_SMOKE_FILES") == "1":
            print(f"MACOS_DOCUMENT_HOST_TEMP_ROOT={root}")
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
