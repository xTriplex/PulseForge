#!/usr/bin/env python3
"""Drive real Cocoa Open/Save panels for the native-dialog adapter fixture."""

from __future__ import annotations

import argparse
import os
import queue
import signal
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

MAX_LOG_BYTES = 64 * 1024
TCC_DENIALS = (
    "not allowed assistive access",
    "not authorized to send apple events",
    "not permitted to send apple events",
    "(-1719)",
    "(-1743)",
)


class FixtureOutput:
    def __init__(self, process: subprocess.Popen[str], log_path: Path):
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.buffer = bytearray()
        self.log_path = log_path

        def read_output() -> None:
            assert process.stdout is not None
            for line in process.stdout:
                encoded = line.encode("utf-8", errors="replace")
                if len(self.buffer) < MAX_LOG_BYTES:
                    self.buffer.extend(encoded[: MAX_LOG_BYTES - len(self.buffer)])
                print(line, end="")
                self.lines.put(line)
            self.lines.put(None)
            log_path.write_bytes(self.buffer)

        self.thread = threading.Thread(target=read_output, daemon=True)
        self.thread.start()


def fail(message: str, log_path: Path | None = None, exit_code: int = 1) -> int:
    print(f"macOS native-dialog automation failed: {message}", file=sys.stderr)
    if log_path and log_path.is_file():
        print(f"Fixture log: {log_path}", file=sys.stderr)
    return exit_code


def is_tcc_denial(output: str) -> bool:
    lowered = output.lower()
    return any(marker in lowered for marker in TCC_DENIALS)


def terminate_fixture(process: subprocess.Popen[str]) -> bool:
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
    # The fixture may have exited while a child still holds its process group.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    return process.poll() is not None


def stop_fixture(process: subprocess.Popen[str], output_reader: FixtureOutput) -> bool:
    stopped = terminate_fixture(process)
    output_reader.thread.join(timeout=5)
    return stopped and not output_reader.thread.is_alive()


def apple_script(source: str, *arguments: str, timeout: int = 30) -> tuple[int, str]:
    try:
        result = subprocess.run(
            ["/usr/bin/osascript", "-", *arguments],
            input=source,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        return result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        return 124, output + "\nAppleScript exceeded its time bound\n"


ACCESSIBILITY_PROBE = '''on run argv
    tell application "System Events"
        tell process (item 1 of argv)
            get name of every window
        end tell
    end tell
end run
'''

SELECT_SCRIPT = '''on run argv
    set targetProcessName to item 1 of argv
    set operation to item 2 of argv
    set directoryPath to item 3 of argv
    set selectedName to item 4 of argv
    tell application "System Events"
        tell process targetProcessName
            set frontmost to true
            if operation is "open-select" then
                keystroke "g" using {command down, shift down}
                delay 0.5
                keystroke directoryPath
                key code 36
                delay 1
                keystroke selectedName
                delay 0.5
                key code 36
            else if operation is "open-cancel" or operation is "save-cancel" then
                key code 53
            else if operation is "save-select" then
                key code 36
            end if
        end tell
    end tell
end run
'''


def run_case(fixture: Path, scenario: str, expected_path: Path, log_path: Path, timeout_seconds: int) -> tuple[int, str]:
    deadline = time.monotonic() + timeout_seconds

    def remaining_seconds() -> float:
        return max(0.0, deadline - time.monotonic())

    environment = os.environ.copy()
    environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(expected_path.parent / "Editor Settings Ω")
    Path(environment["PULSEFORGE_EDITOR_SETTINGS_DIR"]).mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen(
        [str(fixture), "--scenario", scenario, "--path", str(expected_path)],
        cwd=expected_path.parent,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    output_reader = FixtureOutput(process, log_path)
    while True:
        try:
            line = output_reader.lines.get(timeout=remaining_seconds())
        except queue.Empty:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived bounded timeout cleanup"
            return 1, f"native dialog fixture did not become ready within its {timeout_seconds}s case bound"
        if line is None:
            try:
                exit_code = process.wait(timeout=remaining_seconds())
            except subprocess.TimeoutExpired:
                if not stop_fixture(process, output_reader):
                    return 1, "fixture process group or output reader survived bounded output-close cleanup"
                return 1, "fixture closed its output but remained alive past its case deadline"
            output_reader.thread.join(timeout=5)
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            if output_reader.thread.is_alive():
                return 1, "fixture output reader remained active after process exit"
            return exit_code or 1, "fixture exited before opening a native panel"
        if line.startswith(f"NFD_DIALOG_READY={scenario};"):
            break
        if line.startswith("NFD_FIXTURE=FAIL;"):
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader remained active after setup failure"
            return process.returncode or 1, f"fixture failed before opening a panel: {line.rstrip()}"

    process_name = fixture.stem
    remaining = remaining_seconds()
    if remaining <= 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived bounded deadline cleanup"
        return 1, f"native dialog fixture exhausted its {timeout_seconds}s case bound before the Accessibility probe"
    probe_code, probe_output = apple_script(ACCESSIBILITY_PROBE, process_name, timeout=min(30, remaining))
    if probe_code != 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived cleanup after the Accessibility probe"
        if is_tcc_denial(probe_output):
            print("NFD_UI_AUTOMATION=UNAVAILABLE; reason=macOS TCC denied Accessibility or Apple Events; native dialog cases NOT RUN")
            return 77, probe_output
        return 1, f"Accessibility capability probe failed without an identified TCC denial: {probe_output}"

    remaining = remaining_seconds()
    if remaining <= 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived bounded deadline cleanup"
        return 1, f"native dialog fixture exhausted its {timeout_seconds}s case bound before panel automation"
    automation_code, automation_output = apple_script(
        SELECT_SCRIPT,
        process_name,
        scenario,
        str(expected_path.parent),
        expected_path.name,
        timeout=min(30, remaining),
    )
    if automation_code != 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived cleanup after panel automation"
        if is_tcc_denial(automation_output):
            print("NFD_UI_AUTOMATION=UNAVAILABLE; reason=macOS TCC denied Accessibility or Apple Events; native dialog cases NOT RUN")
            return 77, automation_output
        return 1, f"native panel automation failed: {automation_output}"

    try:
        exit_code = process.wait(timeout=remaining_seconds())
    except subprocess.TimeoutExpired:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived bounded timeout cleanup"
        return 1, f"native dialog fixture exceeded its {timeout_seconds}s overall case bound"
    output_reader.thread.join(timeout=5)
    if output_reader.thread.is_alive():
        return 1, "native dialog fixture output did not close after process exit"
    if exit_code != 0:
        return exit_code, f"fixture exited with status {exit_code}"

    full_log = output_reader.buffer.decode("utf-8", errors="replace")
    if "NFD_DIALOG_POST_USE=PASS" not in full_log or f"scenario={scenario}" not in full_log:
        return 1, "fixture did not prove post-dialog window usability"
    if scenario.endswith("cancel"):
        expected_marker = f"NFD_DIALOG_RESULT=CANCELLED; scenario={scenario}"
    else:
        expected_marker = f"NFD_DIALOG_RESULT=SELECTED; scenario={scenario}; path={expected_path}"
    if expected_marker not in full_log:
        return 1, f"fixture did not report the expected result marker: {expected_marker}"
    return 0, f"NFD_DIALOG_SCENARIO=PASS; scenario={scenario}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()
    fixture = args.fixture.resolve(strict=True)
    if not fixture.is_file() or not os.access(fixture, os.X_OK):
        return fail(f"native-dialog fixture is not executable: {fixture}")
    if args.timeout_seconds <= 0:
        return fail("timeout must be positive")
    args.log_directory.mkdir(parents=True, exist_ok=True)
    runner_temp = os.environ.get("RUNNER_TEMP")
    parent = Path(runner_temp).resolve() if runner_temp else Path(tempfile.gettempdir())
    root = Path(tempfile.mkdtemp(prefix="PulseForge NFD Ω ", dir=parent))
    try:
        selection_directory = root / "Dialog Files With Spaces Ω"
        selection_directory.mkdir()
        open_file = selection_directory / "Selected Ω.pfproj"
        open_file.write_text("native open selection fixture\n", encoding="utf-8")
        cases = (
            ("open-select", open_file),
            ("open-cancel", selection_directory / "Cancel Open Ω.pfproj"),
            ("save-select", selection_directory / "Saved Ω.pfproj"),
            ("save-cancel", selection_directory / "Cancel Save Ω.pfproj"),
        )
        for scenario, expected_path in cases:
            status, detail = run_case(
                fixture,
                scenario,
                expected_path,
                args.log_directory / f"native-dialog-{scenario}.log",
                args.timeout_seconds,
            )
            if status == 77:
                print("NFD_UI_AUTOMATION_ACCEPTANCE=BLOCKED; accessibility capability unavailable; not a pass")
                return 77
            if status != 0:
                return fail(f"{scenario}: {detail}")
            print(detail)
        if not (selection_directory / "Saved Ω.pfproj").is_file():
            return fail("Save selection returned success but no selected path was written")
        print("NFD_NATIVE_DIALOG_ACCEPTANCE=PASS; Open/Save selected and canceled with non-ASCII paths; adapter remained usable")
        return 0
    finally:
        if os.environ.get("PULSEFORGE_KEEP_NFD_SMOKE_FILES") == "1":
            print(f"NFD_SMOKE_TEMP_ROOT={root}")
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
