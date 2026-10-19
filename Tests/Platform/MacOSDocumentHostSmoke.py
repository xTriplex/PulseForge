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
TCC_DENIALS = ("not allowed assistive access", "not authorized to send apple events", "(-1719)", "(-1743)")

WINDOW_QUERY = '''on run argv
    tell application "System Events"
        tell process (item 1 of argv)
            get name of every window
        end tell
    end tell
end run
'''

MAIN_WINDOW_CLOSE = '''on run argv
    tell application "System Events"
        tell process (item 1 of argv)
            set editorWindows to every window whose name contains "PulseForge Editor"
            if (count of editorWindows) is 0 then error "No PulseForge Editor top-level window was exposed"
            click close button of item 1 of editorWindows
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
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.buffer = bytearray()
        self.log_path = log_path

        def read() -> None:
            assert process.stdout is not None
            for line in process.stdout:
                encoded = line.encode("utf-8", errors="replace")
                if len(self.buffer) < MAX_LOG_BYTES:
                    self.buffer.extend(encoded[: MAX_LOG_BYTES - len(self.buffer)])
                print(line, end="")
                self.lines.put(line)
            self.lines.put(None)
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


def run_osascript(script: str, process_name: str, timeout: float) -> tuple[int, str]:
    try:
        result = subprocess.run(
            ["/usr/bin/osascript", "-", process_name],
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
    observed_native_windows = False
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
                if "document tab" in lowered and (" failed" in lowered or " timed out" in lowered):
                    failures.append(line.rstrip())
                    break

            query_code, query_output = run_osascript(WINDOW_QUERY, editor.stem, timeout=min(5.0, max(0.1, deadline - time.monotonic())))
            if query_code != 0:
                if has_tcc_denial(query_output):
                    saw_tcc_denial = True
                    break
                failures.append(f"Accessibility window query failed: {query_output.strip()}")
                break
            window_names = [name.strip() for name in query_output.strip().split(",") if name.strip()]
            fixture_titles = [name for name in window_names if "Host Test Fixture" in name]
            if len(window_names) >= 2 and fixture_titles:
                observed_native_windows = True

            output_text = capture.buffer.decode("utf-8", errors="replace")
            if all(marker in output_text for marker in SUCCESS_MARKERS) and observed_native_windows:
                print(
                    "MACOS_DOCUMENT_HOST_NATIVE_WINDOWS=PASS; accessibility observed native fixture windows: "
                    + repr(fixture_titles)
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
            return fail("Accessibility did not observe a separate Host Test Fixture native window", log_path)

        output_text = capture.buffer.decode("utf-8", errors="replace")
        missing = [marker for marker in SUCCESS_MARKERS if marker not in output_text]
        if missing:
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail("fixture did not produce required round-trip evidence: " + repr(missing), log_path)

        close_code, close_output = run_osascript(
            MAIN_WINDOW_CLOSE,
            editor.stem,
            timeout=min(10.0, max(0.1, deadline - time.monotonic())),
        )
        if close_code != 0:
            stop_process_group(process)
            capture.thread.join(timeout=5)
            if has_tcc_denial(close_output):
                print("MACOS_DOCUMENT_HOST_ACCESSIBILITY=UNAVAILABLE; native-window close request NOT RUN; acceptance blocked")
                return 77
            return fail(f"could not request normal main-window close: {close_output.strip()}", log_path)
        try:
            exit_code = process.wait(timeout=max(0.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            stop_process_group(process)
            capture.thread.join(timeout=5)
            return fail("editor did not exit after the native main-window close request before timeout", log_path)
        capture.thread.join(timeout=5)
        if capture.thread.is_alive():
            return fail("editor output reader remained active after process exit", log_path)
        if exit_code != 0:
            return fail(f"editor exited with status {exit_code}", log_path)
        output_text = capture.buffer.decode("utf-8", errors="replace")
        if any(marker not in output_text for marker in SUCCESS_MARKERS):
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
