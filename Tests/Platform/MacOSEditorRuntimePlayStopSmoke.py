#!/usr/bin/env python3
"""Run the Debug editor's synthetic runtime Play/Stop workflow in a bounded process."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from MacOSDocumentHostSmoke import stop_process_group
from MacOSScreenshot import capture_screenshot

PASS_MARKER = (
    b"PULSEFORGE_EDITOR_RUNTIME_PLAY_STOP=PASS; synthetic EditorApplication Play/Stop transition rendered successfully in "
    b"Edit and Play states; the authored scene was preserved."
)
FAIL_MARKER = b"PULSEFORGE_EDITOR_RUNTIME_PLAY_STOP=FAIL;"
PLAY_STARTED_MARKER = b"PULSEFORGE_EDITOR_RUNTIME_PLAY_STOP=PLAY_STARTED;"
MAX_LOG_BYTES = 128 * 1024
MAX_CONSOLE_BYTES = 64 * 1024
READ_CHUNK_BYTES = 4096
TEMP_PREFIX = "PulseForge Editor Runtime Play Stop \u03a9 "


class BoundedOutput:
    def __init__(self, process: subprocess.Popen[str], log_path: Path) -> None:
        self.process = process
        self.log_path = log_path
        self.buffer = bytearray()
        self.console_bytes = 0
        self.pass_seen = False
        self.fail_seen = False
        self.play_started = threading.Event()
        self.log_truncated = False
        self.console_truncated = False
        self.reader_error: str | None = None
        self.thread = threading.Thread(target=self._read, name="PulseForge Play/Stop output", daemon=True)
        self.thread.start()

    def _read(self) -> None:
        scan_tail = b""
        assert self.process.stdout is not None
        try:
            while True:
                text_chunk = self.process.stdout.read(READ_CHUNK_BYTES)
                if not text_chunk:
                    break
                chunk = text_chunk.encode("utf-8", errors="replace")

                searchable = scan_tail + chunk
                self.pass_seen |= PASS_MARKER in searchable
                self.fail_seen |= FAIL_MARKER in searchable
                if PLAY_STARTED_MARKER in searchable:
                    self.play_started.set()
                scan_tail = searchable[
                    -(max(len(PASS_MARKER), len(FAIL_MARKER), len(PLAY_STARTED_MARKER)) - 1) :
                ]

                remaining = MAX_LOG_BYTES - len(self.buffer)
                if remaining > 0:
                    self.buffer.extend(chunk[:remaining])
                if len(chunk) > remaining:
                    self.log_truncated = True

                visible = b""
                if self.console_bytes < MAX_CONSOLE_BYTES:
                    visible = chunk[: MAX_CONSOLE_BYTES - self.console_bytes]
                    sys.stdout.write(visible.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                    self.console_bytes += len(visible)
                if self.console_bytes >= MAX_CONSOLE_BYTES and len(chunk) > len(visible):
                    self.console_truncated = True
        except Exception as error:
            self.reader_error = f"{type(error).__name__}: {error}"
        finally:
            try:
                marker = b"\n[output truncated at 128 KiB]\n"
                if self.log_truncated:
                    self.log_path.write_bytes(self.buffer[: MAX_LOG_BYTES - len(marker)] + marker)
                else:
                    self.log_path.write_bytes(self.buffer)
                if self.console_truncated:
                    print("\n[editor console output truncated at 64 KiB]")
            except OSError as error:
                self.reader_error = f"could not write bounded log {self.log_path}: {error}"


def report_failure(message: str, log_path: Path) -> int:
    print(f"macOS editor runtime Play/Stop smoke failed: {message}", file=sys.stderr)
    if log_path.is_file():
        print(f"Editor output: {log_path}", file=sys.stderr)
        print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    return 1


def remove_temporary_root(root: Path, temp_parent: Path) -> str | None:
    try:
        if root.is_symlink():
            return f"refusing to remove a symlink temporary path: {root}"
        resolved_root = root.resolve(strict=True)
        resolved_parent = temp_parent.resolve(strict=True)
        if resolved_root.parent != resolved_parent or not resolved_root.name.startswith(TEMP_PREFIX):
            return f"refusing to remove unexpected temporary path: {resolved_root}"
        if not resolved_root.is_dir():
            return f"refusing to remove non-directory temporary path: {resolved_root}"
        shutil.rmtree(resolved_root)
        return None
    except OSError as error:
        return f"could not remove temporary root {root}: {error}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()

    if args.timeout_seconds <= 0:
        return report_failure("timeout must be positive", args.log_directory / "macos-editor-runtime-play-stop.log")
    try:
        editor = args.editor.resolve(strict=True)
        project = args.project.resolve(strict=True)
    except OSError as error:
        return report_failure(f"smoke input is unavailable: {error}", args.log_directory / "macos-editor-runtime-play-stop.log")
    if not editor.is_file() or not os.access(editor, os.X_OK):
        return report_failure(f"editor executable is missing or not executable: {editor}", args.log_directory / "macos-editor-runtime-play-stop.log")
    if not project.is_file() or project.suffix != ".pfproj":
        return report_failure(f"project fixture is missing or invalid: {project}", args.log_directory / "macos-editor-runtime-play-stop.log")

    log_path = args.log_directory / "macos-editor-runtime-play-stop.log"
    try:
        args.log_directory.mkdir(parents=True, exist_ok=True)
        runner_temp = os.environ.get("RUNNER_TEMP")
        temp_parent = Path(runner_temp).resolve(strict=True) if runner_temp else Path(tempfile.gettempdir()).resolve(strict=True)
        root = Path(tempfile.mkdtemp(prefix=TEMP_PREFIX, dir=temp_parent)).resolve(strict=True)
    except OSError as error:
        return report_failure(f"could not prepare isolated smoke paths: {error}", log_path)

    process: subprocess.Popen[bytes] | None = None
    capture: BoundedOutput | None = None
    timeout_expired = False
    cleanup_error: str | None = None
    result_error: str | None = None
    exit_code: int | None = None
    deadline = time.monotonic() + args.timeout_seconds
    root_cleanup_error: str | None = None
    try:
        working_directory = root / "Unrelated Working Directory \u03a9"
        settings_directory = root / "Isolated Editor Settings \u03a9"
        working_directory.mkdir()
        settings_directory.mkdir()
        environment = os.environ.copy()
        environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(settings_directory)
        command = [str(editor), "--project", str(project), "--editor-runtime-play-stop-test"]
        print(f"MACOS_EDITOR_RUNTIME_PLAY_STOP_COMMAND={command!r}")
        print(f"MACOS_EDITOR_RUNTIME_PLAY_STOP_CWD={working_directory}")
        print(f"MACOS_EDITOR_RUNTIME_PLAY_STOP_SETTINGS={settings_directory}")
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
        capture = BoundedOutput(process, log_path)
        while process.poll() is None and time.monotonic() < deadline and not capture.play_started.is_set():
            capture.play_started.wait(timeout=min(0.1, max(0.01, deadline - time.monotonic())))
        if capture.play_started.is_set():
            if process.poll() is None:
                screenshot = capture_screenshot(
                    "macos-editor-checkpoint-play-mode-scene-viewport.png",
                    timeout_seconds=max(0.1, min(2.0, deadline - time.monotonic())),
                )
                print(f"MACOS_EDITOR_RUNTIME_PLAY_STOP_SCREENSHOT=checkpoint=play-mode-viewport; {screenshot}")
        try:
            exit_code = process.wait(timeout=max(0.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            timeout_expired = True
            result_error = f"editor did not complete within {args.timeout_seconds} seconds"
    except (OSError, subprocess.SubprocessError) as error:
        result_error = f"could not launch or wait for editor: {error}"
    finally:
        if process is not None:
            try:
                if process.poll() is None and (timeout_expired or result_error):
                    screenshot = capture_screenshot(
                        "macos-editor-runtime-play-stop-checkpoint-unexpected-failure.png"
                    )
                    print(f"MACOS_EDITOR_RUNTIME_PLAY_STOP_FAILURE_SCREENSHOT={screenshot}")
                if not stop_process_group(process):
                    cleanup_error = f"could not stop editor process group PID {process.pid}"
                if capture is not None:
                    capture.thread.join(timeout=5)
                    if capture.thread.is_alive():
                        if not stop_process_group(process):
                            cleanup_error = f"output remained open and process group PID {process.pid} did not stop"
                        capture.thread.join(timeout=5)
                    if capture.thread.is_alive():
                        cleanup_error = f"output reader remained active after stopping process group PID {process.pid}"
            except OSError as error:
                cleanup_error = f"could not clean editor process group PID {process.pid}: {error}"
            finally:
                root_cleanup_error = remove_temporary_root(root, temp_parent)
        else:
            root_cleanup_error = remove_temporary_root(root, temp_parent)

    if timeout_expired:
        return report_failure(result_error or "editor timed out", log_path)
    if result_error:
        return report_failure(result_error, log_path)
    if cleanup_error:
        return report_failure(cleanup_error, log_path)
    if root_cleanup_error:
        return report_failure(root_cleanup_error, log_path)
    if capture is None:
        return report_failure("editor process did not start", log_path)
    if capture.reader_error:
        return report_failure(f"editor output capture failed: {capture.reader_error}", log_path)
    if capture.fail_seen:
        return report_failure("the editor emitted the explicit Play/Stop FAIL marker", log_path)
    if exit_code != 0:
        return report_failure(f"editor exited with status {exit_code}", log_path)
    if not capture.pass_seen:
        return report_failure("the exact Play/Stop PASS marker was not observed", log_path)

    print(
        "MACOS_EDITOR_RUNTIME_PLAY_STOP_SMOKE=PASS; bounded Debug editor subprocess exited normally after its "
        "synthetic app-owned Play/Stop fixture; this is not pointer-input evidence"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
