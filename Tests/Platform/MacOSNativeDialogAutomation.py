#!/usr/bin/env python3
"""Drive real Cocoa Open/Save panels for the native-dialog adapter fixture."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import os
import queue
import signal
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

from MacOSDirectAX import run_direct_ax
from MacOSScreenshot import capture_screenshot

MAX_LOG_BYTES = 64 * 1024
MAX_CONSOLE_BYTES = 32 * 1024
OBSERVATION_TOTAL_SECONDS = 35.0
ACTION_RESPONSE_SECONDS = 15.0


def ax_node_enabled(output: str, role: str, name: str) -> bool:
    for line in output.splitlines():
        if not line.startswith("DIRECT_AX_NODE "):
            continue
        fields = dict(part.split("=", 1) for part in line.split("; ") if "=" in part)
        if fields.get("role") == role and fields.get("name") == name:
            return fields.get("enabled") == "true"
    return False


def open_button_state_ready(output: str, fixture_pid: int, panel_title: str) -> bool:
    results = [line for line in output.splitlines() if line.startswith("DIRECT_AX_RESULT status=open-button-state;")]
    if len(results) != 1:
        raise RuntimeError(f"expected one targeted Open-button observation, found {len(results)}; output={output[-1600:]!r}")
    fields: dict[str, str] = {}
    for part in results[0].removeprefix("DIRECT_AX_RESULT ").split("; "):
        if "=" not in part:
            raise RuntimeError(f"malformed Open-button observation field: {part!r}")
        key, value = part.split("=", 1)
        if key in fields:
            raise RuntimeError(f"duplicate Open-button observation field: {key!r}")
        fields[key] = value
    required_fields = {
        "status", "pid", "window_title", "matches", "enabled", "enabled_error", "complete", "truncated", "visited", "query_errors"
    }
    if not required_fields.issubset(fields) or fields.get("status") != "open-button-state":
        raise RuntimeError(f"malformed Open-button observation; fields={fields!r}")
    if fields.get("pid") != str(fixture_pid) or fields.get("window_title") != panel_title:
        raise RuntimeError(f"Open-button observation target mismatch; fields={fields!r}")
    if fields.get("complete") != "true" or fields.get("truncated") != "false" or fields.get("query_errors") != "[]":
        raise RuntimeError(f"Open-button observation incomplete; fields={fields!r}")
    return fields.get("matches") == "1" and fields.get("enabled") == "true" and fields.get("enabled_error") == "0"


def open_panel_dismissed(output: str, panel_title: str) -> bool:
    results = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_RESULT status=open-button-state-error; stage=window-selection;")
    ]
    if len(results) != 1:
        return False
    fields = dict(
        part.split("=", 1)
        for part in results[0].removeprefix("DIRECT_AX_RESULT ").split("; ")
        if "=" in part
    )
    return fields.get("requested_title") == panel_title and fields.get("matches") == "0"


def open_file_selection_ready(output: str, expected_name: str) -> bool:
    results = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_RESULT status=selection-verified; operation=select-element;")
    ]
    if len(results) != 1:
        return False
    fields = dict(
        part.split("=", 1)
        for part in results[0].removeprefix("DIRECT_AX_RESULT ").split("; ")
        if "=" in part
    )
    return (
        fields.get("operation") == "select-element"
        and fields.get("role") in {"AXRow", "AXCell", "AXTextField"}
        and fields.get("name") == expected_name
        and fields.get("ax_error") == "0"
        and fields.get("selected") == "true"
    )


def open_file_action_dispatched(output: str, expected_name: str) -> bool:
    dispatches = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_DISPATCH attempted=true; operation=open-file;")
    ]
    if len(dispatches) != 1:
        return False
    dispatch_fields = dict(
        part.split("=", 1)
        for part in dispatches[0].removeprefix("DIRECT_AX_DISPATCH ").split("; ")
        if "=" in part
    )
    if dispatch_fields.get("name") != expected_name or dispatch_fields.get("advertised_action") != "AXOpen":
        return False
    results = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_RESULT status=open-file-action-returned; operation=open-file;")
    ]
    if len(results) > 1:
        return False
    if not results:
        return True
    fields = dict(
        part.split("=", 1)
        for part in results[0].removeprefix("DIRECT_AX_RESULT ").split("; ")
        if "=" in part
    )
    return (
        fields.get("role") == "AXTextField"
        and fields.get("name") == expected_name
    )


def open_file_action_error(output: str, expected_name: str) -> int | None:
    dispatches = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_DISPATCH attempted=true; operation=open-file;")
    ]
    results = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_RESULT status=open-file-action-returned; operation=open-file;")
    ]
    if len(dispatches) != 1 or len(results) != 1:
        return None
    dispatch_fields = dict(
        part.split("=", 1) for part in dispatches[0].removeprefix("DIRECT_AX_DISPATCH ").split("; ") if "=" in part
    )
    result_fields = dict(
        part.split("=", 1) for part in results[0].removeprefix("DIRECT_AX_RESULT ").split("; ") if "=" in part
    )
    if dispatch_fields.get("name") != expected_name or dispatch_fields.get("advertised_action") != "AXOpen":
        return None
    try:
        ax_error = int(result_fields["ax_error"])
    except (KeyError, ValueError):
        return None
    if result_fields.get("role") != "AXTextField" or result_fields.get("name") != expected_name:
        return None
    return ax_error


def open_file_confirm_action_dispatched(output: str, expected_name: str) -> bool:
    dispatches = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_DISPATCH attempted=true; operation=confirm-element;")
    ]
    results = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_RESULT status=confirm-element-action-returned; operation=confirm-element;")
    ]
    if len(dispatches) != 1 or len(results) != 1:
        return False
    dispatch_fields = dict(
        part.split("=", 1) for part in dispatches[0].removeprefix("DIRECT_AX_DISPATCH ").split("; ") if "=" in part
    )
    result_fields = dict(
        part.split("=", 1) for part in results[0].removeprefix("DIRECT_AX_RESULT ").split("; ") if "=" in part
    )
    return (
        dispatch_fields.get("role") == "AXTextField"
        and dispatch_fields.get("name") == expected_name
        and dispatch_fields.get("advertised_action") == "AXConfirm"
        and result_fields.get("role") == "AXTextField"
        and result_fields.get("name") == expected_name
        and _is_integer(result_fields.get("ax_error", ""))
    )


def _is_integer(value: str) -> bool:
    try:
        int(value)
        return True
    except ValueError:
        return False


def navigate_open_panel_to_folder(fixture_pid: int, panel_title: str, folder: Path, deadline: float) -> str:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise RuntimeError("Open panel folder-navigation deadline expired before dispatch")
    shortcut = run_keyboard_input("--go-to-folder", fixture_pid, timeout=min(5.0, remaining))
    print(
        f"NFD_GO_TO_FOLDER_SHORTCUT=TRANSPORT_RETURNED; action_id={shortcut}; target_pid={fixture_pid}",
        flush=True,
    )
    sheet = run_direct_ax_until(
        fixture_pid,
        panel_title,
        deadline,
        go_to_folder_path_field_visible,
        "the Open panel Go to Folder path field",
        operation="snapshot",
    )
    if "\n" in str(folder) or "\r" in str(folder):
        raise RuntimeError("Go to Folder path contains a line break and cannot be represented safely")
    set_code, set_output, set_error = run_direct_ax(
        fixture_pid,
        operation="set-folder-path-value",
        window_title=panel_title,
        control_name="PathTextField",
        control_value=str(folder),
        timeout=min(8.0, max(0.1, deadline - time.monotonic())),
    )
    set_output += set_error
    if set_code != 0 or not folder_path_field_ready(set_output, str(folder)):
        raise RuntimeError(f"Go to Folder path was not set and focused; exit_code={set_code}; output={set_output!r}")
    print(f"NFD_GO_TO_FOLDER_PATH=CONFIRMED; target_pid={fixture_pid}; path={str(folder)!r}; {set_output.strip()!r}")
    return_action = run_keyboard_input("--press-return", fixture_pid, timeout=min(5.0, max(0.1, deadline - time.monotonic())))
    print(f"NFD_GO_TO_FOLDER_CONFIRM=TRANSPORT_RETURNED; action_id={return_action}; target_pid={fixture_pid}")
    dismissed = run_direct_ax_until(
        fixture_pid,
        panel_title,
        deadline,
        go_to_folder_sheet_dismissed,
        "the Go to Folder sheet to dismiss after the exact path was confirmed",
        operation="observe-go-to-folder-state",
    )
    return sheet + set_output + dismissed


def run_keyboard_input(mode: str, target_pid: int, *, text: str = "", timeout: float) -> str:
    runner_temp = os.environ.get("RUNNER_TEMP")
    if not runner_temp:
        raise RuntimeError("RUNNER_TEMP is unavailable for the compiled Quartz keyboard helper")
    executable = Path(runner_temp) / "PulseForgePointerInput"
    if not executable.is_file():
        raise RuntimeError(f"compiled Quartz helper is missing: {executable}")
    action_id = uuid.uuid4().hex
    arguments = [str(executable), mode, str(target_pid), action_id]
    if mode == "--type-text":
        arguments.append(text)
    process = subprocess.Popen(
        arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=max(0.1, timeout))
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=0.25)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                stdout, stderr = process.communicate(timeout=1.0)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    f"Quartz keyboard helper survived SIGKILL; action_id={action_id}; pid={process.pid}"
                ) from error
        raise RuntimeError(
            f"Quartz keyboard helper timed out; action_id={action_id}; pid={target_pid}; "
            f"stdout={stdout[-1200:]!r}; stderr={stderr[-1200:]!r}"
        )
    if process.returncode != 0:
        category = "TCC_DENIED" if process.returncode == 77 else "DISPATCH_FAILED"
        raise RuntimeError(
            f"{category}; Quartz keyboard helper exit_code={process.returncode}; action_id={action_id}; "
            f"stdout={stdout[-1200:]!r}; stderr={stderr[-1200:]!r}"
        )
    if "CGEventPost=KEY_SEQUENCE_RETURNED" not in stdout:
        raise RuntimeError(f"Quartz keyboard helper returned no sequence result; action_id={action_id}; output={stdout!r}")
    print(f"NFD_KEYBOARD_INPUT=TRANSPORT_RETURNED; mode={mode}; action_id={action_id}; target_pid={target_pid}; output={stdout.strip()!r}")
    return action_id


def wait_for_open_button(fixture_pid: int, panel_title: str, deadline: float) -> str:
    last_output = ""
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        code, output, error = run_direct_ax(
            fixture_pid,
            operation="observe-open-button-state",
            window_title=panel_title,
            timeout=min(8.0, max(0.1, remaining)),
        )
        last_output = output + error
        if code == 0 and open_button_state_ready(last_output, fixture_pid, panel_title):
            return last_output
        if code == 22 and open_panel_dismissed(last_output, panel_title):
            return last_output
        if code not in (0, 124):
            raise RuntimeError(f"Open panel snapshot failed while awaiting selected file; exit_code={code}; {last_output!r}")
        time.sleep(min(0.15, max(0.0, deadline - time.monotonic())))
    raise RuntimeError(f"Open did not become enabled after filename selection; last_snapshot={last_output[-4000:]!r}")


def select_open_file(fixture_pid: int, panel_title: str, expected_path: Path, timeout: float) -> tuple[int, str]:
    deadline = time.monotonic() + max(0.1, timeout)
    attempts = 0
    while time.monotonic() < deadline:
        attempts += 1
        query_started = time.monotonic()
        code, output, error = run_direct_ax(
            fixture_pid,
            operation="select-element",
            window_title=panel_title,
            control_name=expected_path.name,
            timeout=min(6.0, max(0.1, deadline - time.monotonic())),
        )
        result = output + error
        stage_lines = [line for line in result.splitlines() if line.startswith("DIRECT_AX_SELECT_STAGE ")]
        target_line = next((line for line in result.splitlines() if line.startswith("DIRECT_AX_SELECT_TARGET ")), "none")
        print(
            f"NFD_OPEN_FILE_SELECT_QUERY=attempt={attempts}; exit_code={code}; "
            f"elapsed_seconds={time.monotonic() - query_started:.3f}; "
            f"client_started={any(line.startswith('DIRECT_AX_CLIENT ') for line in result.splitlines())}; "
            f"last_stage={stage_lines[-1] if stage_lines else 'none'}; "
            f"target_info={target_line}; "
            f"dispatch_attempted={'DIRECT_AX_DISPATCH attempted=true' in result}; "
            f"result={next((line for line in result.splitlines() if line.startswith('DIRECT_AX_RESULT ')), 'none')}",
            flush=True,
        )
        if open_file_action_dispatched(result, expected_path.name):
            return code, result
        if code == 0 and "DIRECT_AX_RESULT status=selection-verified; operation=select-element;" in result:
            if not open_file_selection_ready(result, expected_path.name):
                raise RuntimeError(
                    f"Open-file selection result did not identify the exact selected file; output={result[-2400:]!r}"
                )
            print(
                f"NFD_OPEN_FILE_SELECTION=CONFIRMED; attempts={attempts}; pid={fixture_pid}; "
                f"panel={panel_title!r}; file={expected_path.name!r}; {result.strip()!r}",
                flush=True,
            )
            return code, result

        # Only a zero-match result indicates that the browser has not exposed the destination yet.
        # Any ambiguous or timed-out action stops without retrying a potentially dispatched action.
        no_matching_row = False
        if code == 8:
            matches = [
                line for line in result.splitlines()
                if line.startswith("DIRECT_AX_RESULT status=control-selection-error; operation=select-element;")
            ]
            if len(matches) == 1:
                fields = dict(part.split("=", 1) for part in matches[0].split("; ") if "=" in part)
                no_matching_row = fields.get("matches") == "0"
        if no_matching_row:
            time.sleep(min(0.15, max(0.0, deadline - time.monotonic())))
            continue
        raise RuntimeError(
            f"Expected Open-file row selection was not confirmed; exit_code={code}; output={result[-2400:]!r}"
        )
    else:
        raise RuntimeError(
            f"Expected Open-file row did not become accessible before its deadline; "
            f"attempts={attempts}; output={result[-2400:]!r}"
        )
    raise RuntimeError("Open-file selection loop ended without a terminal result")


def run_direct_ax_until(
    fixture_pid: int,
    panel_title: str,
    deadline: float,
    accept: Callable[[str], bool],
    description: str,
    *,
    deep_snapshot: bool = True,
    operation: str = "snapshot",
) -> str:
    last_output = ""
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        code, output, error = run_direct_ax(
            fixture_pid,
            operation=operation,
            window_title=panel_title,
            deep_snapshot=deep_snapshot,
            timeout=min(8.0, max(0.1, remaining)),
        )
        last_output = output + error
        if code == 0 and accept(last_output):
            return last_output
        if code not in (0, 124):
            raise RuntimeError(f"Accessibility observation failed while awaiting {description}; exit_code={code}; {last_output!r}")
        time.sleep(min(0.15, max(0.0, deadline - time.monotonic())))
    raise RuntimeError(f"Timed out awaiting {description}; last_snapshot={last_output[-4000:]!r}")


def go_to_folder_path_field_visible(output: str) -> bool:
    lines = output.splitlines()
    has_folder_sheet = any(
        (
            line.startswith("DIRECT_AX_NODE ")
            and "role=AXSheet" in line
            and ("name=GoToWindow" in line or "name=Go to Folder" in line)
        )
        or (line.startswith("DIRECT_AX_WINDOW ") and "title=Go to Folder" in line)
        for line in lines
    )
    if not has_folder_sheet:
        return False
    for line in lines:
        if not line.startswith("DIRECT_AX_NODE "):
            continue
        fields = dict(part.split("=", 1) for part in line.split("; ") if "=" in part)
        if (
            fields.get("role") == "AXTextField"
            and fields.get("name") == "PathTextField"
            and fields.get("enabled", "").lower() in {"true", "1"}
        ):
            return True
    return False


def go_to_folder_sheet_dismissed(output: str) -> bool:
    results = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_RESULT status=go-to-folder-state;")
    ]
    if len(results) != 1:
        return False
    fields = dict(part.split("=", 1) for part in results[0].split("; ") if "=" in part)
    if (
        fields.get("window_title") != "Open"
        or fields.get("complete") != "true"
        or fields.get("truncated") != "false"
        or fields.get("query_errors") != "[]"
    ):
        return False
    try:
        sheet_count = int(fields["sheet_count"])
    except (KeyError, ValueError):
        return False
    return sheet_count == 0


def folder_path_field_ready(output: str, expected_path: str) -> bool:
    results = [
        line for line in output.splitlines()
        if line.startswith("DIRECT_AX_RESULT status=folder-path-ready;")
    ]
    if len(results) != 1:
        return False
    fields = dict(part.split("=", 1) for part in results[0].split("; ") if "=" in part)
    return fields.get("value") == expected_path and fields.get("focused") == "true"

class FixtureOutput:
    def __init__(self, process: subprocess.Popen[str], log_path: Path):
        self.lines: queue.Queue[str | None] = queue.Queue(maxsize=128)
        self.buffer = bytearray()
        self.log_path = log_path
        self.ready_line: str | None = None
        self.post_use_line: str | None = None
        self.focus_line: str | None = None
        self.result_line: str | None = None
        self.failure_lines: list[str] = []
        self.console_bytes = 0

        def read_output() -> None:
            assert process.stdout is not None
            for line in process.stdout:
                encoded = line.encode("utf-8", errors="replace")
                if len(self.buffer) < MAX_LOG_BYTES:
                    self.buffer.extend(encoded[: MAX_LOG_BYTES - len(self.buffer)])
                if self.console_bytes < MAX_CONSOLE_BYTES:
                    printable = encoded[: MAX_CONSOLE_BYTES - self.console_bytes].decode("utf-8", errors="replace")
                    print(printable, end="")
                    self.console_bytes += len(encoded)
                if line.startswith("NFD_DIALOG_READY="):
                    self.ready_line = line.rstrip()
                elif line.startswith("NFD_DIALOG_POST_USE="):
                    self.post_use_line = line.rstrip()
                elif line.startswith("NFD_DIALOG_FOCUS="):
                    self.focus_line = line.rstrip()
                elif line.startswith("NFD_DIALOG_RESULT="):
                    self.result_line = line.rstrip()
                elif line.startswith("NFD_FIXTURE=FAIL;") and len(self.failure_lines) < 8:
                    self.failure_lines.append(line.rstrip())
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

        self.thread = threading.Thread(target=read_output, daemon=True)
        self.thread.start()


def fail(message: str, log_path: Path | None = None, exit_code: int = 1) -> int:
    print(f"macOS native-dialog automation failed: {message}", file=sys.stderr)
    if log_path and log_path.is_file():
        print(f"Fixture log: {log_path}", file=sys.stderr)
    return exit_code


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


def _process_listing(fixture_pid: int) -> tuple[list[tuple[int, str]], str]:
    try:
        result = subprocess.run(
            ["/bin/ps", "-A", "-o", "pid=", "-o", "comm=", "-o", "args="],
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return [(fixture_pid, "fixture PID supplied by subprocess launcher")], f"NFD_OBSERVATION_PROCESS_LIST_ERROR; {error}"
    if result.returncode != 0:
        return (
            [(fixture_pid, "fixture PID supplied by subprocess launcher")],
            f"NFD_OBSERVATION_PROCESS_LIST_ERROR; exit_code={result.returncode}; stderr={result.stderr[:400]!r}",
        )

    records: dict[int, str] = {}
    for line in result.stdout.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) < 2:
            continue
        try:
            process_id = int(fields[0])
        except ValueError:
            continue
        command = " ".join(fields[1:])
        records[process_id] = command

    fixture_identity = records.get(fixture_pid, "fixture PID supplied by subprocess launcher")
    service_pids = [
        (process_id, command)
        for process_id, command in records.items()
        if process_id != fixture_pid and "openandsavepanelservice" in command.lower()
    ]
    service_pids.sort(key=lambda entry: entry[0])
    candidates = [(fixture_pid, fixture_identity), *service_pids]
    return candidates, f"NFD_OBSERVATION_SERVICE_PIDS={[process_id for process_id, _ in service_pids]}"


def capture_waiting_observation(
    fixture_pid: int,
    scenario: str,
    log_path: Path,
    after_interaction_failure: bool = False,
) -> str:
    started = time.monotonic()
    deadline = started + OBSERVATION_TOTAL_SECONDS
    interaction_state = (
        "post-interaction failure; no further input or focus actions issued during observation"
        if after_interaction_failure
        else "no input or focus actions issued before or during observation"
    )
    lines = [
        f"NFD_WAITING_OBSERVATION_BEGIN; scenario={scenario}; fixture_pid={fixture_pid}; "
        f"state={interaction_state}"
    ]
    screenshot_state = capture_screenshot(f"native-dialog-{scenario}-observation.png")
    lines.append(f"NFD_WAITING_SCREENSHOT={screenshot_state}")

    candidates, enumeration_state = _process_listing(fixture_pid)
    lines.append(enumeration_state)
    for index, (process_id, identity) in enumerate(candidates):
        remaining = deadline - time.monotonic()
        uninspected = len(candidates) - index
        if remaining <= 0:
            lines.append(
                "NFD_OBSERVATION_DEADLINE; "
                f"uninspected_pids={[pid for pid, _ in candidates[index:]]}"
            )
            break
        per_process_timeout = min(2.5, max(0.05, remaining / max(1, uninspected) - 0.25))
        lines.append(f"NFD_OBSERVATION_CANDIDATE; pid={process_id}; ps_identity={identity[:280]}")
        code, output, error = run_direct_ax(
            process_id, operation="snapshot", deep_snapshot=True, timeout=per_process_timeout
        )
        output += error
        category = "DIRECT_AX_OK" if code == 0 else "DIRECT_AX_TIMEOUT" if code == 124 else f"DIRECT_AX_ERROR_{code}"
        lines.append(
            f"NFD_OBSERVATION_DIRECT_AX; pid={process_id}; category={category}; "
            f"exit_code={code}; output={output[:8000]!r}"
        )
    lines.append(f"NFD_WAITING_OBSERVATION_END; elapsed_seconds={time.monotonic() - started:.2f}")
    report = "\n".join(lines)
    try:
        log_path.write_text(report + "\n", encoding="utf-8")
    except OSError as error:
        report += f"\nNFD_WAITING_OBSERVATION_LOG_WRITE_FAILED; {error}"
    print(report, flush=True)
    return report


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
        if output_reader.ready_line:
            if not output_reader.ready_line.startswith(f"NFD_DIALOG_READY={scenario};"):
                if not stop_fixture(process, output_reader):
                    return 1, "fixture process group or output reader remained active after a mismatched readiness marker"
                return 1, f"fixture reported readiness for an unexpected scenario: {output_reader.ready_line}"
            break
        if output_reader.failure_lines:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader remained active after setup failure"
            return process.returncode or 1, "; ".join(output_reader.failure_lines)
        remaining = remaining_seconds()
        if remaining <= 0:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived bounded timeout cleanup"
            return 1, f"native dialog fixture did not become ready within its {timeout_seconds}s case bound"
        try:
            line = output_reader.lines.get(timeout=min(0.5, remaining))
        except queue.Empty:
            continue
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
        if line.startswith("NFD_FIXTURE=FAIL;"):
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader remained active after setup failure"
            return process.returncode or 1, f"fixture failed before opening a panel: {line.rstrip()}"

    process_id = str(process.pid)
    remaining = remaining_seconds()
    if remaining <= 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived bounded deadline cleanup"
        return 1, f"native dialog fixture exhausted its {timeout_seconds}s case bound before the Accessibility probe"
    probe_deadline = min(deadline, time.monotonic() + 30)
    last_panel_state = "no Accessibility snapshot returned"
    panel_title = "Open" if scenario.startswith("open-") else "Save"
    while True:
        probe_remaining = max(0.0, probe_deadline - time.monotonic())
        if probe_remaining <= 0:
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            capture_waiting_observation(process.pid, scenario, observation_log)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after the panel readiness deadline"
            return 1, (
                "native panel did not become Accessibility-ready before the existing readiness deadline; "
                f"read-only waiting-state observation captured; last_state={last_panel_state!r}"
            )
        if output_reader.failure_lines:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after a panel-readiness failure"
            return process.returncode or 1, "; ".join(output_reader.failure_lines)
        if process.poll() is not None:
            output_reader.thread.join(timeout=5)
            return process.returncode or 1, "native-dialog fixture exited before its panel became Accessibility-ready"
        probe_code, probe_output, probe_error = run_direct_ax(
            process.pid,
            operation="snapshot",
            window_title=panel_title,
            deep_snapshot=True,
            timeout=min(10.0, max(0.1, probe_remaining)),
        )
        probe_output += probe_error
        client_line = next((line for line in probe_output.splitlines() if line.startswith("DIRECT_AX_CLIENT ")), "client_identity=missing")
        target_line = next((line for line in probe_output.splitlines() if line.startswith("DIRECT_AX_TARGET_PROCESS ")), "target_identity=missing")
        expected_button = "Open" if scenario == "open-select" else "Save" if scenario == "save-select" else "Cancel"
        has_dialog = any(
            "DIRECT_AX_WINDOW " in line
            and ("subrole=AXDialog" in line or f"title={panel_title}" in line)
            for line in probe_output.splitlines()
        )
        has_expected_button = any(
            "DIRECT_AX_NODE " in line
            and "role=AXButton" in line
            and f"name={expected_button}" in line
            for line in probe_output.splitlines()
        )
        if probe_code == 0 and has_dialog and has_expected_button:
            panel_process_id = process_id
            print(f"NFD_DIRECT_AX_OBSERVATION=PASS; {client_line}; {target_line}; snapshot={probe_output[-9000:]!r}")
            print(
                f"NFD_PANEL_READY=pid={process_id}; panel_pid={process_id}; client=swift-interpreter; "
                f"scenario={scenario}; expected_path={expected_path}; initial_directory={expected_path.parent}; "
                f"expected_button={expected_button}; button_enabled_not_required=true"
            )
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            observation_report = capture_waiting_observation(process.pid, scenario, observation_log)
            screenshot_state = next(
                (line for line in observation_report.splitlines() if line.startswith("NFD_WAITING_SCREENSHOT=")),
                "NFD_WAITING_SCREENSHOT=NOT_REPORTED",
            )
            print(f"NFD_READY_STATE_OBSERVATION=COMPLETE; scenario={scenario}; {screenshot_state}", flush=True)
            break
        if probe_code == 124 and "process_group_cleanup=failed" in probe_output:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived direct AX observer cleanup failure"
            return 1, f"direct AX observer was not reaped; {probe_output!r}"
        if probe_code != 0 and "trusted=false" in probe_output:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after direct AX trust failure"
            return 77, f"DIRECT_AX_CLIENT_UNTRUSTED; scenario={scenario}; output={probe_output!r}"
        if probe_code != 0 and probe_code != 124:
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            capture_waiting_observation(process.pid, scenario, observation_log)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after direct AX query failure"
            return 1, f"direct Swift AX query failed; exit_code={probe_code}; output={probe_output!r}"
        if probe_code == 0 and "DIRECT_AX_RESULT status=snapshot" in probe_output:
            last_panel_state = f"dialog/control not ready; scenario={scenario}; output={probe_output[-4000:]!r}"
            time.sleep(min(0.2, max(0.0, probe_deadline - time.monotonic())))
            continue
        if probe_code == 124:
            last_panel_state = f"direct AX observation timed out; output={probe_output[-1200:]!r}"
            time.sleep(min(0.2, max(0.0, probe_deadline - time.monotonic())))
            continue
        observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
        capture_waiting_observation(process.pid, scenario, observation_log)
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived malformed direct AX response cleanup"
        return 1, f"direct AX client returned malformed or unexpected output; exit_code={probe_code}; {probe_output!r}"


    remaining = remaining_seconds()
    if remaining <= 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived bounded deadline cleanup"
        return 1, f"native dialog fixture exhausted its {timeout_seconds}s case bound before panel automation"
    if scenario == "open-select":
        last_open_step = "Open panel activation not attempted"
        try:
            activation_code, activation_output, activation_error = run_direct_ax(
                int(panel_process_id),
                operation="activate-window",
                window_title=panel_title,
                timeout=min(8.0, remaining_seconds()),
            )
            activation_output += activation_error
            if activation_code != 0 or "DIRECT_AX_RESULT status=window-focused" not in activation_output:
                raise RuntimeError(
                    f"Open panel was not the confirmed focused AX window before exact file selection; "
                    f"exit_code={activation_code}; {activation_output!r}"
                )
            print(f"NFD_OPEN_PANEL_FOCUS=CONFIRMED; {activation_output.strip()!r}", flush=True)

            helper_deadline = min(deadline, time.monotonic() + 25.0)
            selection_code, selection_output = select_open_file(
                int(panel_process_id), panel_title, expected_path,
                timeout=max(0.1, helper_deadline - time.monotonic())
            )
            automation_code = selection_code
            last_open_step = "exact Open-panel filename selection operation returned"
            automation_output = selection_output
            open_action_sent = open_file_action_dispatched(selection_output, expected_path.name)
            open_button_confirmed = False
            if open_action_sent:
                ax_error = open_file_action_error(selection_output, expected_path.name)
                if ax_error == -25205:
                    state_code, panel_state, state_error = run_direct_ax(
                        int(panel_process_id),
                        operation="observe-open-button-state",
                        window_title=panel_title,
                        timeout=min(8.0, max(0.1, helper_deadline - time.monotonic())),
                    )
                    panel_state += state_error
                    if state_code == 22 and open_panel_dismissed(panel_state, panel_title):
                        last_open_step = "unsupported AXOpen was followed by panel dismissal; reconciling semantic result without replay"
                    elif state_code == 0 and not open_button_state_ready(panel_state, int(panel_process_id), panel_title):
                        automation_output += panel_state
                        navigation = navigate_open_panel_to_folder(
                            int(panel_process_id), panel_title, expected_path.parent, helper_deadline
                        )
                        automation_output += navigation
                        confirm_code, confirm_output, confirm_error = run_direct_ax(
                            int(panel_process_id),
                            operation="confirm-element",
                            window_title=panel_title,
                            control_name=expected_path.name,
                            timeout=min(8.0, max(0.1, helper_deadline - time.monotonic())),
                        )
                        confirm_output += confirm_error
                        automation_output += confirm_output
                        automation_code = confirm_code
                        if not open_file_confirm_action_dispatched(confirm_output, expected_path.name):
                            raise RuntimeError(
                                "The exact file did not expose one completed AXConfirm action after Go to Folder; "
                                f"exit_code={confirm_code}; output={confirm_output[-2400:]!r}"
                            )
                        panel_state = wait_for_open_button(
                            int(panel_process_id), panel_title, helper_deadline
                        )
                        automation_output += panel_state
                        if open_panel_dismissed(panel_state, panel_title):
                            last_open_step = "AXConfirm returned; Open panel dismissed and semantic result will be reconciled without replay"
                        else:
                            open_button_confirmed = open_button_state_ready(
                                panel_state, int(panel_process_id), panel_title
                            )
                            if not open_button_confirmed:
                                raise RuntimeError(
                                    "AXConfirm was dispatched once, but neither Open readiness nor panel dismissal was observed; "
                                    f"action_exit_code={confirm_code}; postcondition={panel_state[-2400:]!r}"
                                )
                    elif state_code == 0 and open_button_state_ready(panel_state, int(panel_process_id), panel_title):
                        automation_output += panel_state
                        open_button_confirmed = True
                    else:
                        raise RuntimeError(
                            "Could not establish the Open-panel postcondition after unsupported AXOpen; "
                            f"exit_code={state_code}; output={panel_state[-2400:]!r}"
                        )
                else:
                    last_open_step = "AXOpen dispatched; waiting for semantic fixture result without replay"
            else:
                panel_state = wait_for_open_button(int(panel_process_id), panel_title, helper_deadline)
                automation_output += panel_state
                open_button_confirmed = open_button_state_ready(panel_state, int(panel_process_id), panel_title)
        except (OSError, RuntimeError) as error:
            observation_log = log_path.with_name(f"native-dialog-{scenario}-selection-failure.log")
            capture_waiting_observation(process.pid, scenario, observation_log, after_interaction_failure=True)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived failed Open selection"
            return 1, (
                f"Open selection failed before the Open action; last_step={last_open_step}; "
                f"{error}"
            )
        if open_button_confirmed:
            automation_code, pressed, press_error = run_direct_ax(
                int(panel_process_id), operation="press-control", window_title=panel_title,
                control_name="Open", timeout=min(10.0, remaining_seconds())
            )
            automation_output += pressed + press_error
    elif scenario == "save-select":
        automation_code, automation_output, automation_error = run_direct_ax(
            int(panel_process_id),
            operation="set-value",
            window_title=panel_title,
            control_name="saveAsNameTextField",
            control_value=expected_path.name,
            timeout=min(10.0, remaining),
        )
        automation_output += automation_error
        if automation_code == 0:
            button = "Open" if scenario == "open-select" else "Save"
            automation_code, pressed, press_error = run_direct_ax(
                int(panel_process_id), operation="press-control", window_title=panel_title,
                control_name=button, timeout=min(10.0, remaining)
            )
            automation_output += pressed + press_error
    else:
        automation_code, automation_output, automation_error = run_direct_ax(
            int(panel_process_id), operation="press-control", window_title=panel_title,
            control_name="Cancel", timeout=min(10.0, remaining)
        )
        automation_output += automation_error
    expected_marker = (
        f"NFD_DIALOG_RESULT=CANCELLED; scenario={scenario}"
        if scenario.endswith("cancel")
        else f"NFD_DIALOG_RESULT=SELECTED; scenario={scenario}; path={expected_path}"
    )
    if "DIRECT_AX_DISPATCH attempted=true" in automation_output:
        response_deadline = min(deadline, time.monotonic() + ACTION_RESPONSE_SECONDS)
        while (
            output_reader.result_line != expected_marker
            and output_reader.result_line is None
            and process.poll() is None
            and time.monotonic() < response_deadline
        ):
            try:
                output_reader.lines.get(timeout=min(0.1, max(0.01, response_deadline - time.monotonic())))
            except queue.Empty:
                pass
        if output_reader.result_line != expected_marker:
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            capture_waiting_observation(process.pid, scenario, observation_log, after_interaction_failure=True)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after AX action response timeout"
            return 1, (
                f"direct AX action was submitted but the expected fixture result was absent; "
                f"expected={expected_marker!r}; observed={output_reader.result_line!r}; "
                f"swift_exit_code={automation_code}; output={automation_output!r}"
            )
        print(
            f"NFD_DIRECT_AX_ACTION=TRANSPORT_RECONCILED; scenario={scenario}; "
            f"expected_result={expected_marker!r}; no action was replayed",
            flush=True,
        )

    if automation_code != 0:
        if "process_group_cleanup=failed" in automation_output:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived direct AX action cleanup failure"
            return 1, f"direct AX action process was not reaped; output={automation_output!r}"
        if "trusted=false" in automation_output:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after direct AX trust denial"
            print("NFD_UI_AUTOMATION=UNAVAILABLE; client=Swift interpreter; reason=AXIsProcessTrusted=false; native dialog case NOT RUN")
            return 77, automation_output
        if "DIRECT_AX_DISPATCH attempted=true" not in automation_output:
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            capture_waiting_observation(process.pid, scenario, observation_log, after_interaction_failure=True)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after the panel failure observation"
            return 1, f"native panel AX action failed before dispatch; swift_exit_code={automation_code}; output={automation_output!r}"
    print(
        f"NFD_DIRECT_AX_DISPATCH_RESULT=RETURNED; scenario={scenario}; "
        f"expected_fixture_result_pending=true; {automation_output.strip()!r}"
    )

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
        detail = "; ".join(output_reader.failure_lines) or f"fixture exited with status {exit_code}"
        return exit_code, detail

    if not output_reader.post_use_line or f"scenario={scenario}" not in output_reader.post_use_line:
        return 1, "fixture did not prove post-dialog window usability"
    expected_focus_marker = f"NFD_DIALOG_FOCUS=PASS; scenario={scenario}"
    if output_reader.focus_line != expected_focus_marker:
        return 1, f"fixture did not prove parent-window focus restoration: {expected_focus_marker}"
    if scenario.endswith("cancel"):
        expected_marker = f"NFD_DIALOG_RESULT=CANCELLED; scenario={scenario}"
    else:
        expected_marker = f"NFD_DIALOG_RESULT=SELECTED; scenario={scenario}; path={expected_path}"
    if output_reader.result_line != expected_marker:
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
