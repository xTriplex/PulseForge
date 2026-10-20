#!/usr/bin/env python3
"""Exercise document-close decisions through macOS Accessibility and synthetic OS input."""

from __future__ import annotations

import argparse
from collections import deque
import math
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

MAX_LOG_BYTES = 128 * 1024
MAX_CONSOLE_BYTES = 48 * 1024
MAX_EVENT_COUNT = 128
FIXTURE_TITLE = "Host Test Fixture 1"
MAIN_TITLE_FRAGMENT = "PulseForge Editor"
SCENARIOS = ("cancel-save", "discard", "save-retry", "close-retry")
TCC_DENIALS = ("not allowed assistive access", "not authorized to send apple events", "(-1743)")
POINT = re.compile(r"^(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)$")

WINDOW_QUERY = '''on run argv
    set targetPID to item 1 of argv as integer
    set expectedTitle to item 2 of argv as text
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set processName to name of targetProcess as text
        tell targetProcess
            set matches to {}
            repeat with windowIndex from 1 to count of windows
                set targetWindow to window windowIndex
                if (name of targetWindow as text) is expectedTitle then
                    set end of matches to windowIndex as text
                end if
            end repeat
        end tell
        set AppleScript's text item delimiters to ","
        set matchList to matches as text
        set AppleScript's text item delimiters to ""
        return processName & linefeed & matchList
    end tell
end run
'''

WINDOW_BOUNDS_QUERY = '''on run argv
    set targetPID to item 1 of argv as integer
    set expectedTitle to item 2 of argv as text
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set matches to {}
        repeat with targetWindow in windows of targetProcess
            if (name of targetWindow as text) is expectedTitle then set end of matches to targetWindow
        end repeat
        if (count of matches) is not 1 then error "Expected one fixture window named " & quoted form of expectedTitle
        set targetWindow to item 1 of matches
        set windowPosition to position of targetWindow
        set windowSize to size of targetWindow
        return (name of targetProcess as text) & linefeed & expectedTitle & linefeed & ¬
            ((item 1 of windowPosition) as text) & "," & ((item 2 of windowPosition) as text) & ¬
            "," & ((item 1 of windowSize) as text) & "," & ((item 2 of windowSize) as text)
    end tell
end run
'''

FOCUS_TARGET = '''on run argv
    set targetPID to item 1 of argv as integer
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set frontmost of targetProcess to true
        return "MACOS_DOC_CLOSE_FOCUS=PASS; pid=" & (targetPID as text) & "; process=" & (name of targetProcess as text)
    end tell
end run
'''

POINTER_OBSERVE = '''use framework "AppKit"

on currentMouseLocation(targetX, targetY)
    set screenCountText to "UNAVAILABLE"
    set frameText to "UNAVAILABLE"
    set metadataReady to false
    set metadataError to ""
    try
        set screensList to (current application's NSScreen's screens()) as list
        set screenCount to count of screensList
        set screenCountText to screenCount as text
        if screenCount > 0 then
            set mainScreen to current application's NSScreen's mainScreen()
            if mainScreen is not missing value then
                set frameParts to (mainScreen's frame()) as list
                set frameOrigin to item 1 of frameParts
                set frameSize to item 2 of frameParts
                set frameX to (item 1 of frameOrigin) as real
                set frameY to (item 2 of frameOrigin) as real
                set frameWidth to (item 1 of frameSize) as real
                set frameHeight to (item 2 of frameSize) as real
                set frameText to (frameX as text) & "," & (frameY as text) & "," & ¬
                    (frameWidth as text) & "," & (frameHeight as text)
                set metadataReady to true
            else
                set metadataError to "NSScreen.mainScreen returned no screen"
            end if
        else
            set metadataError to "NSScreen exposed no screens"
        end if
    on error errorMessage number errorNumber
        set metadataError to (errorNumber as text) & ":" & errorMessage
    end try
    try
        set locationRecord to (current application's NSEvent's mouseLocation()) as record
        set locationX to (x of locationRecord) as real
        set locationY to (y of locationRecord) as real
    on error errorMessage number errorNumber
        return "screen_count=" & screenCountText & "; main_frame=" & frameText & ¬
            "; raw_appkit=UNAVAILABLE; system_events_top_left_logical=UNAVAILABLE; " & ¬
            "target_match=UNAVAILABLE; mouse_error=" & ¬
            (errorNumber as text) & ":" & errorMessage
    end try
    set rawText to (locationX as text) & "," & (locationY as text)
    if metadataReady is false then
        return "screen_count=" & screenCountText & "; main_frame=" & frameText & "; raw_appkit=" & rawText & ¬
            "; system_events_top_left_logical=UNAVAILABLE; target_match=UNAVAILABLE; metadata_error=" & metadataError
    end if
    if screenCount is not 1 then
        return "screen_count=" & (screenCount as text) & "; main_frame=" & frameText & "; raw_appkit=" & rawText & ¬
            "; system_events_top_left_logical=UNAVAILABLE; target_match=UNAVAILABLE; reason=multiple screens"
    end if

    -- AppKit screen coordinates have a lower-left origin; System Events uses top-left screen coordinates.
    -- Only convert when one screen is exposed, so no multi-display origin or backing-scale guess is needed.
    set systemX to locationX - frameX
    set systemY to frameY + frameHeight - locationY
    set deltaX to systemX - targetX
    set deltaY to systemY - targetY
    if deltaX < 0 then set deltaX to -deltaX
    if deltaY < 0 then set deltaY to -deltaY
    set targetMatch to "0"
    if deltaX is less than or equal to 2.0 and deltaY is less than or equal to 2.0 then set targetMatch to "1"
    return "screen_count=" & (screenCount as text) & "; main_frame=" & frameText & "; raw_appkit=" & rawText & ¬
        "; system_events_top_left_logical=" & (systemX as text) & "," & (systemY as text) & ¬
        "; target_match=" & targetMatch & "; tolerance=2.0"
end currentMouseLocation

on run argv
    set clickX to item 1 of argv as integer
    set clickY to item 2 of argv as integer
    return my currentMouseLocation(clickX, clickY)
end run
'''

POINTER_DRIVER_SOURCE = r'''import CoreGraphics
import Darwin
import Foundation

func fail(_ message: String, _ status: Int32) -> Never {
    fputs(message + "\n", stderr)
    exit(status)
}

guard CommandLine.arguments.count == 4,
      let targetPID = Int32(CommandLine.arguments[1]),
      let x = Double(CommandLine.arguments[2]),
      let y = Double(CommandLine.arguments[3]) else {
    fail("CGEvent input requires PID and finite desktop coordinates", 2)
}
guard x.isFinite, y.isFinite else {
    fail("CGEvent input coordinates are not finite", 2)
}
guard CGPreflightPostEventAccess() else {
    fail("TCC_DENIED: CGPreflightPostEventAccess rejected synthetic pointer input", 77)
}

let location = CGPoint(x: x, y: y)
func post(_ type: CGEventType) {
    guard let event = CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: location, mouseButton: .left) else {
        fail("CGEventCreateMouseEvent failed for event type \(type.rawValue)", 3)
    }
    event.post(tap: .cghidEventTap)
}

post(.mouseMoved)
Thread.sleep(forTimeInterval: 0.05)
post(.leftMouseDown)
Thread.sleep(forTimeInterval: 0.05)
post(.leftMouseUp)
print("CGEventPost=POSTED; pid=\(targetPID); point=\(Int(x.rounded())),\(Int(y.rounded())); access=preflight-approved")
'''

TYPE_EDITOR_VALUE = '''on run argv
    set targetPID to item 1 of argv as integer
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set frontmost of targetProcess to true
        keystroke "a" using {command down}
        keystroke "42"
        key code 36
        return "MACOS_DOC_CLOSE_TEXT_INPUT=PASS; pid=" & (targetPID as text) & "; value=42"
    end tell
end run
'''

MAIN_WINDOW_CLOSE = '''on run argv
    set targetPID to item 1 of argv as integer
    set expectedFragment to item 2 of argv as text
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        tell targetProcess
            set matches to {}
            repeat with targetWindow in windows
                set windowTitle to name of targetWindow as text
                if windowTitle contains expectedFragment then set end of matches to targetWindow
            end repeat
            if (count of matches) is not 1 then error "Expected one main PulseForge Editor window for PID " & (targetPID as text)
            set closeControl to value of attribute "AXCloseButton" of item 1 of matches
            if closeControl is missing value then error "Main editor AXCloseButton is missing"
            perform action "AXPress" of closeControl
            return "MACOS_DOC_CLOSE_MAIN_AX_PRESS=PASS; pid=" & (targetPID as text)
        end tell
    end tell
end run
'''


class BoundedCapture:
    def __init__(self, process: subprocess.Popen[str], log_path: Path) -> None:
        self.process = process
        self.log_path = log_path
        self.events: queue.Queue[dict[str, str]] = queue.Queue(maxsize=MAX_EVENT_COUNT)
        self.pending: deque[dict[str, str]] = deque(maxlen=MAX_EVENT_COUNT)
        self.buffer = bytearray()
        self.console_bytes = 0
        self.log_truncated = False
        self.console_truncated = False
        self.reader_error: str | None = None
        self.thread = threading.Thread(target=self._read, name="PulseForge close-smoke output", daemon=True)
        self.thread.start()

    def _read(self) -> None:
        assert self.process.stdout is not None
        try:
            for line in self.process.stdout:
                encoded = line.encode("utf-8", errors="replace")
                remaining = MAX_LOG_BYTES - len(self.buffer)
                if remaining > 0:
                    self.buffer.extend(encoded[:remaining])
                if len(encoded) > remaining:
                    self.log_truncated = True

                if self.console_bytes < MAX_CONSOLE_BYTES:
                    visible = encoded[: MAX_CONSOLE_BYTES - self.console_bytes]
                    sys.stdout.write(visible.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                    self.console_bytes += len(visible)
                    if len(visible) < len(encoded):
                        self.console_truncated = True
                else:
                    self.console_truncated = True

                marker = parse_marker(line.rstrip("\r\n"))
                if marker is not None:
                    try:
                        self.events.put_nowait(marker)
                    except queue.Full as error:
                        raise RuntimeError("editor emitted more than 128 unconsumed close-fixture markers") from error
        except Exception as error:
            self.reader_error = f"{type(error).__name__}: {error}"
        finally:
            try:
                marker = b"\n[output truncated at 128 KiB]\n" if self.log_truncated else b""
                self.log_path.write_bytes(self.buffer[: MAX_LOG_BYTES - len(marker)] + marker)
                if self.console_truncated:
                    print("\n[editor console output truncated at 48 KiB]")
            except OSError as error:
                self.reader_error = f"could not write bounded log {self.log_path}: {error}"


class MarkerProtocol:
    def __init__(self, scenario: str, host: str, asset: str, generation: int) -> None:
        self.scenario = scenario
        self.host = host
        self.asset = asset
        self.generation = generation
        self.last_sequence = -1

    @classmethod
    def from_ready(cls, event: dict[str, str], scenario: str) -> MarkerProtocol:
        if event.get("scenario") != scenario:
            raise RuntimeError(f"READY scenario mismatch: {event.get('raw')!r}")
        for key in ("host", "asset"):
            if not event.get(key):
                raise RuntimeError(f"READY marker omitted {key}: {event.get('raw')!r}")
        for key in ("fixture_sessions", "fixture_hosts"):
            if event.get(key) != "1":
                raise RuntimeError(f"fixture must own exactly one session and host; {key}={event.get(key)!r}")
        try:
            generation = int(event["generation"])
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"READY marker has invalid generation: {event.get('raw')!r}") from error
        if generation != 0:
            raise RuntimeError(f"fixture must begin at generation 0: {event.get('raw')!r}")
        return cls(scenario, event["host"], event["asset"], generation)

    def check_identity(self, event: dict[str, str]) -> None:
        if event.get("scenario") != self.scenario or event.get("host") != self.host or event.get("asset") != self.asset:
            raise RuntimeError(
                "fixture identity changed during close interaction; "
                f"expected scenario/host/asset={self.scenario!r}/{self.host!r}/{self.asset!r}; marker={event.get('raw')!r}"
            )

    @staticmethod
    def marker_generation(event: dict[str, str]) -> int:
        try:
            return int(event["generation"])
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"marker has invalid or missing generation: {event.get('raw')!r}") from error

    def accept_prompt(self, event: dict[str, str]) -> int:
        self.check_identity(event)
        generation = self.marker_generation(event)
        if generation <= self.generation:
            raise RuntimeError(
                f"stale prompt coordinates: generation must advance beyond {self.generation}; marker={event.get('raw')!r}"
            )
        self.generation = generation
        return generation

    def accept_action(self, event: dict[str, str], choice: str, expected_generation: int) -> None:
        self.check_identity(event)
        if event.get("choice") != choice:
            raise RuntimeError(f"expected action {choice!r}, got {event.get('raw')!r}")
        generation = self.marker_generation(event)
        if generation != expected_generation or generation != self.generation:
            raise RuntimeError(
                f"action generation does not match the prompt/control snapshot {expected_generation}: {event.get('raw')!r}"
            )
        try:
            sequence = int(event["sequence"])
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"action marker has invalid or missing sequence: {event.get('raw')!r}") from error
        if sequence <= self.last_sequence:
            raise RuntimeError(
                f"action sequence did not advance beyond {self.last_sequence}: {event.get('raw')!r}"
            )
        self.last_sequence = sequence

    def accept_fixture(self, event: dict[str, str]) -> int:
        self.check_identity(event)
        generation = self.marker_generation(event)
        if generation <= self.generation:
            raise RuntimeError(
                f"fixture control generation must advance beyond {self.generation}; marker={event.get('raw')!r}"
            )
        self.generation = generation
        return generation

    def accept_control(self, event: dict[str, str]) -> int:
        self.check_identity(event)
        generation = self.marker_generation(event)
        if generation < self.generation:
            raise RuntimeError(
                f"stale close-control coordinates: generation must be at least {self.generation}; marker={event.get('raw')!r}"
            )
        self.generation = generation
        return generation

    def accept_closed(self, event: dict[str, str]) -> None:
        self.check_identity(event)
        generation = self.marker_generation(event)
        if generation <= self.generation:
            raise RuntimeError(
                f"closed marker generation must advance beyond {self.generation}; marker={event.get('raw')!r}"
            )
        try:
            sequence = int(event["sequence"])
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"closed marker has invalid or missing sequence: {event.get('raw')!r}") from error
        if sequence != self.last_sequence:
            raise RuntimeError(
                f"closed marker sequence changed without another UI action; expected={self.last_sequence}; "
                f"marker={event.get('raw')!r}"
            )
        self.generation = generation


def parse_marker(line: str) -> dict[str, str] | None:
    marker_start = line.find("PF_DOC_CLOSE_UI|")
    if marker_start < 0:
        return None
    line = line[marker_start:]
    parts = line.split("|")
    if len(parts) < 2:
        return {"kind": "INVALID"}
    event = {"kind": parts[1]}
    for field in parts[2:]:
        key, separator, value = field.partition("=")
        if not separator or not key or key in event:
            return {"kind": "INVALID", "raw": line[:1024]}
        event[key] = value
    event["raw"] = line[:1024]
    return event


def run_osascript(script: str, *arguments: str, timeout: float) -> tuple[int, str]:
    try:
        result = subprocess.run(
            ["/usr/bin/osascript", "-", *arguments],
            input=script,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=max(0.1, timeout),
            check=False,
        )
        return result.returncode, result.stdout[-4096:]
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        return 124, (output + "\nAppleScript operation timed out\n")[-4096:]


def is_tcc_denial(output: str) -> bool:
    lowered = output.lower()
    return any(pattern in lowered for pattern in TCC_DENIALS)


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
        os.killpg(process.pid, 0)
    except ProcessLookupError:
        return process.poll() is not None
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        return False
    return process.poll() is not None


def run_bounded_command(command: list[str], timeout: float) -> tuple[int, str, str]:
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=max(0.1, timeout))
    except subprocess.TimeoutExpired as error:
        stopped = stop_process_group(process)
        try:
            stdout, stderr = process.communicate(timeout=1.0)
        except subprocess.TimeoutExpired:
            stdout, stderr = "", ""
        if not stopped:
            raise RuntimeError(f"timed-out child process group remains active: PID {process.pid}") from error
        raise RuntimeError(
            f"command timed out after {timeout:.1f}s; command={command!r}; "
            f"stdout={stdout[-1000:]!r}; stderr={stderr[-1000:]!r}"
        ) from error
    return process.returncode, stdout[-4096:], stderr[-4096:]


def bounded_timeout(deadline: float, cap: float = 8.0) -> float:
    return max(0.1, min(cap, deadline - time.monotonic()))


def wait_event(capture: BoundedCapture, process: subprocess.Popen[str], kind: str, scenario: str, deadline: float) -> dict[str, str]:
    while time.monotonic() < deadline:
        if capture.reader_error:
            raise RuntimeError(f"editor output capture failed: {capture.reader_error}")
        if process.poll() is not None:
            raise RuntimeError(f"editor exited before {kind}; exit_code={process.returncode}")
        event = None
        for pending in tuple(capture.pending):
            if pending.get("kind") == kind and pending.get("scenario") == scenario:
                capture.pending.remove(pending)
                event = pending
                break
        if event is None:
            try:
                event = capture.events.get(timeout=min(0.25, max(0.01, deadline - time.monotonic())))
            except queue.Empty:
                continue
        if event.get("kind") == "FAIL":
            raise RuntimeError(f"fixture reported failure: {event.get('raw', '<invalid marker>')}")
        if event.get("scenario") != scenario:
            raise RuntimeError(f"fixture marker has wrong scenario: {event.get('raw', event)!r}")
        if event.get("kind") == kind:
            return event
        if event.get("kind") == "INVALID":
            raise RuntimeError(f"malformed fixture marker: {event.get('raw', '<invalid marker>')!r}")
        if event.get("kind") in {"READY", "PROMPT", "ACTION", "CLOSED", "PASS"}:
            raise RuntimeError(f"unexpected {event.get('kind')} event while waiting for {kind}: {event.get('raw')!r}")
        if event.get("kind") in {"FIXTURE", "TEXT_CONTROL", "TEXT_POINTER", "TEXT_FOCUS", "TEXT_ENTRY", "CLOSE_CONTROL"}:
            if event.get("kind") == "FIXTURE":
                capture.pending = deque(
                    (pending for pending in capture.pending if pending.get("kind") != "FIXTURE"), maxlen=MAX_EVENT_COUNT
                )
            capture.pending.append(event)
            continue
        raise RuntimeError(f"unexpected marker while waiting for {kind}: {event.get('raw')!r}")
    raise RuntimeError(f"timed out waiting for {kind} marker for {scenario}")


def query_fixture_window(
    process: subprocess.Popen[str], deadline: float
) -> tuple[float, float, float, float]:
    last_state = "not queried"
    process_lookup_deadline = min(deadline, time.monotonic() + 5.0)
    while time.monotonic() < deadline:
        code, output = run_osascript(WINDOW_QUERY, str(process.pid), FIXTURE_TITLE, timeout=bounded_timeout(deadline, 4.0))
        if code != 0:
            if is_tcc_denial(output):
                raise RuntimeError(f"TCC_DENIED: Accessibility authorization is required: {output.strip()}")
            if code == 124:
                last_state = f"Accessibility query timed out: {output.strip()}"
                continue
            lowered = output.lower()
            if "-1719" in lowered and "unix id" in lowered and "process" in lowered:
                last_state = f"System Events has not exposed fixture PID {process.pid}; AXError=-1719: {output.strip()}"
                if time.monotonic() < process_lookup_deadline:
                    time.sleep(min(0.1, max(0.0, process_lookup_deadline - time.monotonic())))
                    continue
                raise RuntimeError(f"AX_PROCESS_LOOKUP_INVALID_INDEX after fixture readiness: {last_state}")
            raise RuntimeError(f"Accessibility query failed with exit code {code}: {output.strip()}")
        lines = [line.strip() for line in output.splitlines() if line.strip()]
        if len(lines) >= 2:
            indices = [value for value in lines[1].split(",") if value]
            if "PulseForge" not in lines[0]:
                raise RuntimeError(f"PID {process.pid} resolves to unexpected process {lines[0]!r}")
            if len(indices) == 1 and indices[0].isdigit():
                bounds_code, bounds = run_osascript(
                    WINDOW_BOUNDS_QUERY,
                    str(process.pid),
                    FIXTURE_TITLE,
                    timeout=bounded_timeout(deadline, 4.0),
                )
                if bounds_code != 0:
                    if is_tcc_denial(bounds):
                        raise RuntimeError(
                            f"TCC_DENIED: Accessibility authorization is required for the read-only window preflight: "
                            f"{bounds.strip()}"
                        )
                    raise RuntimeError(
                        f"read-only Accessibility window preflight failed; exit_code={bounds_code}; "
                        f"detail={bounds.strip()}"
                    )
                bound_lines = [line.strip() for line in bounds.splitlines() if line.strip()]
                if len(bound_lines) != 3 or bound_lines[0] != lines[0] or bound_lines[1] != FIXTURE_TITLE:
                    raise RuntimeError(
                        f"read-only Accessibility window preflight returned inconsistent process/window identity: "
                        f"{bounds.strip()!r}"
                    )
                try:
                    window_bounds = tuple(float(value) for value in bound_lines[2].split(","))
                except ValueError as error:
                    raise RuntimeError(f"Accessibility fixture bounds are malformed: {bounds.strip()!r}") from error
                if len(window_bounds) != 4 or not all(math.isfinite(value) for value in window_bounds):
                    raise RuntimeError(f"Accessibility fixture bounds are non-finite or incomplete: {bounds.strip()!r}")
                if window_bounds[2] <= 0 or window_bounds[3] <= 0:
                    raise RuntimeError(f"Accessibility fixture window has no usable area: {bounds.strip()!r}")
                print(
                    f"MACOS_DOC_CLOSE_PREFLIGHT=PASS; pid={process.pid}; process={bound_lines[0]}; "
                    f"window={bound_lines[1]}; accessibility_bounds={bound_lines[2]!r}; "
                    "ImGui viewport points=desktop logical; AX bounds=desktop logical; "
                    "no Retina scale conversion applied"
                )
                return window_bounds
            last_state = f"expected exactly one {FIXTURE_TITLE!r}, observed indices {indices!r}"
        else:
            last_state = f"Accessibility returned incomplete window snapshot: {output.strip()!r}"
        time.sleep(min(0.2, max(0.0, deadline - time.monotonic())))
    raise RuntimeError(f"timed out locating unique native fixture window; last state: {last_state}")


def parse_point(value: str, field: str) -> tuple[int, int]:
    match = POINT.fullmatch(value)
    if not match:
        raise RuntimeError(f"fixture gave invalid {field} desktop-logical coordinate: {value!r}")
    x, y = (float(match.group(1)), float(match.group(2)))
    if not math.isfinite(x) or not math.isfinite(y):
        raise RuntimeError(f"fixture gave non-finite {field} coordinate: {value!r}")
    return int(round(x)), int(round(y))


def compile_pointer_driver(temporary_root: Path) -> Path:
    source = temporary_root / "PulseForgePointerInput.swift"
    executable = temporary_root / "PulseForgePointerInput"
    source.write_text(POINTER_DRIVER_SOURCE, encoding="utf-8")
    try:
        exit_code, stdout, stderr = run_bounded_command(
            ["/usr/bin/xcrun", "swiftc", "-O", str(source), "-framework", "CoreGraphics", "-o", str(executable)],
            timeout=30,
        )
    except (OSError, RuntimeError) as error:
        raise RuntimeError(f"bounded Quartz pointer-driver compilation failed: {error}") from error
    if exit_code != 0 or not executable.is_file():
        raise RuntimeError(
            f"Quartz pointer-driver compilation failed; exit_code={exit_code}; "
            f"stdout={stdout[-2000:]!r}; stderr={stderr[-2000:]!r}"
        )
    return executable


def click_point(
    process: subprocess.Popen[str],
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float],
    point: tuple[int, int],
    deadline: float,
    label: str,
) -> None:
    window_x, window_y, window_width, window_height = fixture_bounds
    if not (window_x <= point[0] < window_x + window_width and window_y <= point[1] < window_y + window_height):
        raise RuntimeError(
            f"ImGui desktop point for {label} falls outside the fixture AX bounds; "
            f"point={point!r}; bounds={fixture_bounds!r}; no window-origin translation was applied"
        )

    code, output = run_osascript(FOCUS_TARGET, str(process.pid), timeout=bounded_timeout(deadline, 8.0))
    if code != 0:
        if is_tcc_denial(output):
            raise RuntimeError(f"TCC_DENIED: Accessibility authorization is required to focus the fixture: {output.strip()}")
        raise RuntimeError(f"could not focus the fixture before clicking {label}; exit_code={code}; detail={output.strip()}")

    code, pointer_before = run_osascript(
        POINTER_OBSERVE, str(point[0]), str(point[1]), timeout=bounded_timeout(deadline, 3.0)
    )
    if code != 0:
        if is_tcc_denial(pointer_before):
            raise RuntimeError(f"TCC_DENIED: Accessibility authorization is required to observe pointer state: {pointer_before.strip()}")
        raise RuntimeError(
            f"could not observe pointer before clicking {label}; exit_code={code}; detail={pointer_before.strip()}"
        )

    try:
        exit_code, stdout, stderr = run_bounded_command(
            [str(pointer_driver), str(process.pid), str(point[0]), str(point[1])],
            timeout=bounded_timeout(deadline, 5.0),
        )
    except (OSError, RuntimeError) as error:
        raise RuntimeError(f"bounded Quartz pointer input for {label} failed: {type(error).__name__}: {error}") from error
    if exit_code == 77:
        raise RuntimeError(f"TCC_DENIED: {stderr.strip()}")
    if exit_code != 0:
        raise RuntimeError(
            f"Quartz pointer input for {label} failed; exit_code={exit_code}; "
            f"stdout={stdout[-1000:]!r}; stderr={stderr[-1000:]!r}"
        )

    print(
        f"MACOS_DOC_CLOSE_POINTER_INPUT=DRIVER_RETURNED; control={label}; "
        f"driver_output={stdout.strip()!r}; delivery=not-yet-observed; pointer_before={pointer_before.strip()!r}"
    )

    observation_deadline = min(deadline, time.monotonic() + 3.0)
    last_state = "pointer observation not attempted"
    observed_pointer = False
    while time.monotonic() < observation_deadline:
        code, observation = run_osascript(
            POINTER_OBSERVE,
            str(point[0]),
            str(point[1]),
            timeout=bounded_timeout(observation_deadline, 1.0),
        )
        if code == 0 and "target_match=1" in observation:
            print(
                f"MACOS_DOC_CLOSE_POINTER_OBSERVATION=TARGET_MATCH; control={label}; "
                f"pointer_after={observation.strip()}; fixture_state_confirmation=pending"
            )
            return
        if code != 0:
            if is_tcc_denial(observation):
                raise RuntimeError(
                    f"TCC_DENIED: Accessibility authorization is required to observe pointer delivery: {observation.strip()}"
                )
            if code != 124:
                raise RuntimeError(
                    f"pointer observation for {label} failed; exit_code={code}; detail={observation.strip()}"
                )
            last_state = f"observation timed out: {observation.strip()}"
        else:
            observed_pointer = True
            last_state = observation.strip()
        time.sleep(min(0.1, max(0.0, observation_deadline - time.monotonic())))
    if observed_pointer:
        print(
            f"MACOS_DOC_CLOSE_POINTER_OBSERVATION=TARGET_NOT_CONFIRMED; control={label}; "
            f"requested={point!r}; last_observed={last_state!r}; fixture_state_confirmation=required"
        )
        return
    print(
        f"MACOS_DOC_CLOSE_POINTER_OBSERVATION=UNAVAILABLE; control={label}; "
        f"requested={point!r}; last_observed={last_state!r}; fixture_state_confirmation=required"
    )


def capture_text_focus_timeout(
    process: subprocess.Popen[str], scenario: str, point: tuple[int, int], log_directory: Path, cause: str
) -> str:
    code, bounds = run_osascript(
        WINDOW_BOUNDS_QUERY, str(process.pid), FIXTURE_TITLE, timeout=5.0
    )
    if code != 0:
        if is_tcc_denial(bounds):
            bounds_state = f"window_bounds=TCC_DENIED; exit_code={code}; detail={bounds.strip()!r}"
        elif code == 124:
            bounds_state = f"window_bounds=TIMED_OUT; exit_code={code}; detail={bounds.strip()!r}"
        else:
            bounds_state = f"window_bounds=ERROR; exit_code={code}; detail={bounds.strip()!r}"
    else:
        bounds_state = f"window_bounds={bounds.strip()!r}"

    screenshot = log_directory / f"macos-document-close-{scenario}-text-focus-timeout.png"
    try:
        result = subprocess.run(
            ["/usr/sbin/screencapture", "-x", "-C", "-t", "png", str(screenshot)],
            capture_output=True,
            text=True,
            timeout=5.0,
            check=False,
        )
        if result.returncode == 0 and screenshot.is_file() and screenshot.stat().st_size > 0:
            screenshot_state = f"screenshot=CAPTURED; bytes={screenshot.stat().st_size}; path={screenshot}"
        else:
            state = "TCC_DENIED" if is_tcc_denial(result.stderr + result.stdout) else "FAILED"
            screenshot_state = (
                f"screenshot={state}; exit_code={result.returncode}; "
                f"stdout={result.stdout[-400:]!r}; stderr={result.stderr[-400:]!r}"
            )
    except (OSError, subprocess.TimeoutExpired) as error:
        state = "TIMED_OUT" if isinstance(error, subprocess.TimeoutExpired) else "FAILED"
        screenshot_state = f"screenshot={state}; error={type(error).__name__}: {error}"
    return f"{cause}; click_point={point!r}; {bounds_state}; {screenshot_state}"


def capture_action_timeout_screenshot(
    log_directory: Path,
    process: subprocess.Popen[str],
    scenario: str,
    choice: str,
) -> str:
    screenshot = log_directory / f"macos-document-close-{scenario}-action-timeout.png"
    try:
        result = subprocess.run(
            ["/usr/sbin/screencapture", "-x", "-C", "-t", "png", str(screenshot)],
            capture_output=True,
            text=True,
            timeout=5.0,
            check=False,
        )
        if result.returncode == 0 and screenshot.is_file() and screenshot.stat().st_size > 0:
            screenshot_state = f"CAPTURED; bytes={screenshot.stat().st_size}; path={screenshot}"
        else:
            state = "TCC_DENIED" if is_tcc_denial(result.stderr + result.stdout) else "FAILED"
            screenshot_state = (
                f"{state}; exit_code={result.returncode}; "
                f"stdout={result.stdout[-400:]!r}; stderr={result.stderr[-400:]!r}"
            )
    except (OSError, subprocess.TimeoutExpired) as error:
        state = "TIMED_OUT" if isinstance(error, subprocess.TimeoutExpired) else "FAILED"
        screenshot_state = f"{state}; error={type(error).__name__}: {error}"

    process_state = "running" if process.poll() is None else f"exited={process.returncode}"
    return (
        f"scenario={scenario}; choice={choice}; pid={process.pid}; process_state={process_state}; "
        f"screenshot={screenshot_state}"
    )


def click_document_tab_close(
    process: subprocess.Popen[str],
    capture: BoundedCapture,
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float],
    deadline: float,
    protocol: MarkerProtocol,
    label: str,
) -> None:
    control = wait_event(capture, process, "CLOSE_CONTROL", protocol.scenario, deadline)
    protocol.accept_control(control)
    point = parse_point(f"{control.get('x', '')},{control.get('y', '')}", "document tab close")
    click_point(process, pointer_driver, fixture_bounds, point, deadline, label)


def enter_editor_value(
    process: subprocess.Popen[str],
    capture: BoundedCapture,
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float],
    deadline: float,
    protocol: MarkerProtocol,
) -> None:
    control = wait_event(capture, process, "TEXT_CONTROL", protocol.scenario, deadline)
    protocol.check_identity(control)
    if control.get("generation") != "0" or control.get("value") != "20":
        raise RuntimeError(f"editor text control started in an unexpected state: {control.get('raw')!r}")
    point = parse_point(f"{control.get('x', '')},{control.get('y', '')}", "editable value")
    click_point(process, pointer_driver, fixture_bounds, point, deadline, "Editable value")

    pointer_deadline = min(deadline, time.monotonic() + 8.0)
    try:
        pointer = wait_event(capture, process, "TEXT_POINTER", protocol.scenario, pointer_deadline)
    except RuntimeError as error:
        raise RuntimeError(
            capture_text_focus_timeout(
                process, protocol.scenario, point, capture.log_path.parent,
                f"ImGui reported no mouse, hover, or active-state change after the click; "
                f"initial_pointer=({control.get('mouse_x')},{control.get('mouse_y')}); {error}",
            )
        ) from error
    protocol.check_identity(pointer)
    try:
        mouse_x = float(pointer["mouse_x"])
        mouse_y = float(pointer["mouse_y"])
    except (KeyError, ValueError) as error:
        raise RuntimeError(f"ImGui pointer-state marker has invalid coordinates: {pointer.get('raw')!r}") from error
    if not math.isfinite(mouse_x) or not math.isfinite(mouse_y):
        raise RuntimeError(f"ImGui pointer-state marker has non-finite coordinates: {pointer.get('raw')!r}")
    if pointer.get("hovered") != "1" or not math.isclose(mouse_x, point[0], abs_tol=1.5) or not math.isclose(
        mouse_y, point[1], abs_tol=1.5
    ):
        raise RuntimeError(
            capture_text_focus_timeout(
                process, protocol.scenario, point, capture.log_path.parent,
                f"ImGui did not hover the clicked text control at its reported mouse position: {pointer.get('raw')!r}",
            )
        )
    print(
        f"MACOS_DOC_CLOSE_HOVER=PASS; scenario={protocol.scenario}; "
        f"mouse=({mouse_x:.1f},{mouse_y:.1f}); control=({point[0]},{point[1]})"
    )

    focus_deadline = min(deadline, time.monotonic() + 5.0)
    try:
        focus = wait_event(capture, process, "TEXT_FOCUS", protocol.scenario, focus_deadline)
    except RuntimeError as error:
        raise RuntimeError(
            capture_text_focus_timeout(
                process, protocol.scenario, point, capture.log_path.parent,
                f"ImGui saw the editable control hovered but did not report focus: {error}",
            )
        ) from error
    protocol.check_identity(focus)
    if focus.get("generation") != "0" or focus.get("active") != "1" or focus.get("value") != "20":
        raise RuntimeError(f"editable value did not receive focus before text entry: {focus.get('raw')!r}")
    code, output = run_osascript(TYPE_EDITOR_VALUE, str(process.pid), timeout=bounded_timeout(deadline, 8.0))
    if code != 0:
        if is_tcc_denial(output):
            raise RuntimeError(f"TCC_DENIED: Accessibility authorization is required for OS keyboard input: {output.strip()}")
        raise RuntimeError(f"OS keyboard input failed; exit_code={code}; detail={output.strip()}")
    print(output.strip())

    entered = wait_event(capture, process, "TEXT_ENTRY", protocol.scenario, deadline)
    protocol.check_identity(entered)
    expected = {"generation": "0", "value": "42", "saved": "10", "dirty": "1"}
    if any(entered.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"editor did not retain the entered value as a dirty document: {entered.get('raw')!r}")
    print(f"MACOS_DOC_CLOSE_TEXT_ENTRY=PASS; scenario={protocol.scenario}; value=42; saved=10; dirty=1")


def get_prompt_point(event: dict[str, str], scenario: str, host: str, choice: str) -> tuple[int, int]:
    if event.get("host") != host:
        raise RuntimeError(f"prompt host does not match READY: {event.get('raw')!r}")
    if event.get("scenario") != scenario:
        raise RuntimeError(f"prompt scenario mismatch: {event.get('raw')!r}")
    return parse_point(event.get(choice, ""), choice)


def expect_action(
    capture: BoundedCapture,
    process: subprocess.Popen[str],
    deadline: float,
    protocol: MarkerProtocol,
    choice: str,
    expected_generation: int,
    *,
    resolved: str,
    pending: str,
    opened: str,
    dirty: str,
    value: str,
    saved: str,
    error: str = "",
) -> dict[str, str]:
    try:
        action = wait_event(capture, process, "ACTION", protocol.scenario, deadline)
    except RuntimeError as error:
        if str(error).startswith("timed out waiting for ACTION marker"):
            evidence = capture_action_timeout_screenshot(
                capture.log_path.parent, process, protocol.scenario, choice
            )
            print(f"MACOS_DOC_CLOSE_ACTION_TIMEOUT_OBSERVATION={evidence}")
            raise RuntimeError(f"{error}; {evidence}") from error
        raise
    expected = {
        "choice": choice,
        "resolved": resolved,
        "pending": pending,
        "open": opened,
        "host_same": "1" if opened == "1" else "0",
        "dirty": dirty,
        "value": value,
        "saved": saved,
        "error": error,
    }
    for key, value_expected in expected.items():
        if action.get(key) != value_expected:
            raise RuntimeError(
                f"unexpected {choice} state for {protocol.scenario}: field {key} expected {value_expected!r}, "
                f"got {action.get(key)!r}; marker={action.get('raw')!r}"
            )
    protocol.accept_action(action, choice, expected_generation)
    print(
        f"MACOS_DOC_CLOSE_FIXTURE_ACTION=PASS; scenario={protocol.scenario}; "
        f"choice={choice}; resolved={resolved}; pending={pending}; open={opened}; dirty={dirty}; "
        f"value={value}; saved={saved}"
    )
    return action


def require_pass(capture: BoundedCapture, process: subprocess.Popen[str], deadline: float, protocol: MarkerProtocol) -> None:
    event = wait_event(capture, process, "PASS", protocol.scenario, deadline)
    protocol.check_identity(event)
    if event.get("scenario") != protocol.scenario:
        raise RuntimeError(f"unexpected PASS marker: {event.get('raw')!r}")


def run_decisions(
    process: subprocess.Popen[str],
    capture: BoundedCapture,
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float],
    ready: dict[str, str],
    scenario: str,
    deadline: float,
) -> None:
    if (ready.get("saved"), ready.get("value"), ready.get("dirty")) != ("10", "20", "1"):
        raise RuntimeError(f"fixture READY state mismatch: {ready.get('raw')!r}")
    protocol = MarkerProtocol.from_ready(ready, scenario)
    host = protocol.host
    print(f"MACOS_DOC_CLOSE_READY=PASS; scenario={scenario}; host={host}; asset={protocol.asset}")
    enter_editor_value(process, capture, pointer_driver, fixture_bounds, deadline, protocol)

    click_document_tab_close(process, capture, pointer_driver, fixture_bounds, deadline, protocol, "document tab close")
    prompt = wait_event(capture, process, "PROMPT", scenario, deadline)
    prompt_generation = protocol.accept_prompt(prompt)
    print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")

    if scenario == "cancel-save":
        click_point(process, pointer_driver, fixture_bounds, get_prompt_point(prompt, scenario, host, "cancel"), deadline, "Cancel")
        expect_action(capture, process, deadline, protocol, "cancel", prompt_generation, resolved="1", pending="0", opened="1", dirty="1", value="42", saved="10")
        click_document_tab_close(process, capture, pointer_driver, fixture_bounds, deadline, protocol, "document tab close after cancel")
        prompt = wait_event(capture, process, "PROMPT", scenario, deadline)
        prompt_generation = protocol.accept_prompt(prompt)
        print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")
        click_point(process, pointer_driver, fixture_bounds, get_prompt_point(prompt, scenario, host, "save"), deadline, "Save")
        expect_action(capture, process, deadline, protocol, "save", prompt_generation, resolved="1", pending="0", opened="0", dirty="0", value="42", saved="42")
    elif scenario == "discard":
        click_point(process, pointer_driver, fixture_bounds, get_prompt_point(prompt, scenario, host, "discard"), deadline, "Discard")
        expect_action(capture, process, deadline, protocol, "discard", prompt_generation, resolved="1", pending="0", opened="0", dirty="0", value="10", saved="10")
    elif scenario == "save-retry":
        click_point(process, pointer_driver, fixture_bounds, get_prompt_point(prompt, scenario, host, "save"), deadline, "Save (expected failure)")
        expect_action(
            capture, process, deadline, protocol, "save", prompt_generation, resolved="0", pending="1", opened="1", dirty="1", value="42", saved="10",
            error="The%20document%20could%20not%20be%20saved%3B%20it%20remains%20open.",
        )
        prompt = wait_event(capture, process, "PROMPT", scenario, deadline)
        prompt_generation = protocol.accept_prompt(prompt)
        print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")
        click_point(process, pointer_driver, fixture_bounds, get_prompt_point(prompt, scenario, host, "save"), deadline, "Save (retry)")
        expect_action(capture, process, deadline, protocol, "save", prompt_generation, resolved="1", pending="0", opened="0", dirty="0", value="42", saved="42")
    elif scenario == "close-retry":
        click_point(process, pointer_driver, fixture_bounds, get_prompt_point(prompt, scenario, host, "save"), deadline, "Save (expected close preflight failure)")
        expect_action(
            capture, process, deadline, protocol, "save", prompt_generation, resolved="0", pending="1", opened="1", dirty="0", value="42", saved="42",
            error="The%20Debug%20fixture%20is%20configured%20to%20reject%20close.",
        )
        prompt = wait_event(capture, process, "PROMPT", scenario, deadline)
        prompt_generation = protocol.accept_prompt(prompt)
        print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")
        click_point(process, pointer_driver, fixture_bounds, get_prompt_point(prompt, scenario, host, "cancel"), deadline, "Cancel pending close")
        expect_action(capture, process, deadline, protocol, "cancel", prompt_generation, resolved="1", pending="0", opened="1", dirty="0", value="42", saved="42")
        fixture = wait_event(capture, process, "FIXTURE", scenario, deadline)
        fixture_generation = protocol.accept_fixture(fixture)
        click_point(process, pointer_driver, fixture_bounds, parse_point(fixture.get("allow_close", ""), "Allow close"), deadline, "Allow close")
        expect_action(capture, process, deadline, protocol, "allow-close", fixture_generation, resolved="1", pending="0", opened="1", dirty="0", value="42", saved="42")
        click_document_tab_close(process, capture, pointer_driver, fixture_bounds, deadline, protocol, "document tab close after recovery")
        closed = wait_event(capture, process, "CLOSED", scenario, deadline)
        protocol.accept_closed(closed)
        if closed.get("open") != "0" or closed.get("saved") != "42":
            raise RuntimeError(f"clean close after preflight recovery did not preserve the saved session state: {closed.get('raw')!r}")
        print(f"MACOS_DOC_CLOSE_FIXTURE_CLOSED=PASS; scenario={scenario}; saved={closed.get('saved')}")
    else:
        raise RuntimeError(f"unsupported close scenario: {scenario}")

    require_pass(capture, process, deadline, protocol)
    print(f"MACOS_DOC_CLOSE_DECISION_SCENARIO=PASS; scenario={scenario}")


def exit_normally(process: subprocess.Popen[str], deadline: float) -> None:
    code, output = run_osascript(
        MAIN_WINDOW_CLOSE, str(process.pid), MAIN_TITLE_FRAGMENT, timeout=bounded_timeout(deadline, 8.0)
    )
    if code != 0:
        if is_tcc_denial(output):
            raise RuntimeError(f"TCC_DENIED: Accessibility is required for normal application exit: {output.strip()}")
        raise RuntimeError(f"could not request normal main-window close; exit_code={code}; detail={output.strip()}")
    print(output.strip())
    try:
        exit_code = process.wait(timeout=max(0.1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("editor did not exit through the normal main-window close path") from error
    if exit_code != 0:
        raise RuntimeError(f"editor exited with status {exit_code} after normal close")
    print(f"MACOS_DOC_CLOSE_NORMAL_EXIT=PASS; pid={process.pid}; exit_code={exit_code}")


def remove_temporary_root(root: Path, parent: Path) -> str | None:
    try:
        if root.is_symlink():
            return f"refusing to remove symlink temporary root: {root}"
        resolved_root = root.resolve(strict=True)
        resolved_parent = parent.resolve(strict=True)
        if resolved_root.parent != resolved_parent or not resolved_root.name.startswith("PulseForge Document Close Ω "):
            return f"refusing to remove unexpected temporary root: {resolved_root}"
        shutil.rmtree(resolved_root)
        return None
    except OSError as error:
        return f"could not remove temporary root {root}: {error}"


def run_scenario(editor: Path, project: Path, log_directory: Path, scenario: str, timeout_seconds: int) -> None:
    runner_temp = os.environ.get("RUNNER_TEMP")
    parent = Path(runner_temp).resolve(strict=True) if runner_temp else Path(tempfile.gettempdir()).resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="PulseForge Document Close Ω ", dir=parent)).resolve(strict=True)
    work = root / "Unrelated Working Directory Ω"
    settings = root / "Isolated Editor Settings Ω"
    log_path = log_directory / f"macos-document-close-{scenario}.log"
    environment = os.environ.copy()
    environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(settings)
    command = [str(editor), "--project", str(project), "--document-host-close-ui-test", scenario]
    process: subprocess.Popen[str] | None = None
    capture: BoundedCapture | None = None
    failure: str | None = None
    try:
        work.mkdir()
        settings.mkdir()
        pointer_driver = compile_pointer_driver(root)
        print(f"MACOS_DOC_CLOSE_COMMAND={command!r}")
        print(f"MACOS_DOC_CLOSE_SETTINGS={settings}")
        process = subprocess.Popen(
            command,
            cwd=work,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        capture = BoundedCapture(process, log_path)
        deadline = time.monotonic() + timeout_seconds
        ready = wait_event(capture, process, "READY", scenario, deadline)
        fixture_bounds = query_fixture_window(process, deadline)
        run_decisions(process, capture, pointer_driver, fixture_bounds, ready, scenario, deadline)
        exit_normally(process, deadline)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        failure = str(error)
    finally:
        cleanup_failure = None
        process_stopped = process is None or process.poll() is not None
        if process is not None and not process_stopped:
            process_stopped = stop_process_group(process)
            if not process_stopped:
                cleanup_failure = f"could not stop fixture process group PID {process.pid}"
        if capture is not None:
            capture.thread.join(timeout=5)
            if capture.thread.is_alive():
                cleanup_failure = f"editor output reader remained active after bounded process cleanup PID {process.pid}"
            elif capture.reader_error:
                cleanup_failure = capture.reader_error
        if process_stopped:
            root_failure = remove_temporary_root(root, parent) if root.exists() else None
        else:
            root_failure = f"preserved temporary root because fixture process group may still be active: {root}"
        if cleanup_failure:
            failure = f"{failure}; {cleanup_failure}" if failure else cleanup_failure
        if root_failure:
            failure = f"{failure}; {root_failure}" if failure else root_failure
    if failure:
        detail = f"; log={log_path}"
        if log_path.is_file():
            detail += "\n" + log_path.read_text(encoding="utf-8", errors="replace")
        raise RuntimeError(f"{scenario}: {failure}{detail}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        print("macOS document-close smoke failed: timeout must be positive", file=sys.stderr)
        return 1
    try:
        editor = args.editor.resolve(strict=True)
        project = args.project.resolve(strict=True)
        if not editor.is_file() or not os.access(editor, os.X_OK):
            raise RuntimeError(f"editor executable is missing or not executable: {editor}")
        if not project.is_file() or project.suffix != ".pfproj":
            raise RuntimeError(f"project fixture is missing or invalid: {project}")
        args.log_directory.mkdir(parents=True, exist_ok=True)
        for scenario in SCENARIOS:
            run_scenario(editor, project, args.log_directory, scenario, args.timeout_seconds)
            print(f"MACOS_DOC_CLOSE_SCENARIO=PASS; scenario={scenario}")
    except (OSError, RuntimeError) as error:
        print(f"macOS document-close smoke failed: {error}", file=sys.stderr)
        return 1
    print(
        "MACOS_DOCUMENT_CLOSE_SMOKE=PASS; editor text entry and document-tab close used OS pointer/keyboard input, "
        "modal decisions used OS pointer input, and application exit used Accessibility; synthetic input only, "
        "not physical pointer acceptance"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
