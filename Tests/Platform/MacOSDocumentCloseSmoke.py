#!/usr/bin/env python3
"""Exercise document-close decisions through macOS Accessibility and synthetic OS input."""

from __future__ import annotations

import argparse
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
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
import uuid
from pathlib import Path

from MacOSAutomationClient import capture_failure_context, probe_osascript_access
from MacOSDirectAX import parse_window_activation, run_direct_ax
from MacOSScreenshot import capture_screenshot

MAX_LOG_BYTES = 128 * 1024
MAX_CONSOLE_BYTES = 48 * 1024
MAX_EVENT_COUNT = 128
TELEMETRY_MARKERS = {
    "TEXT_POINTER", "DECISION_POINTER", "DIAG_TAB_INPUT", "DIAG_TAB_CLOSE_REQUEST",
    "DIAG_DECISION_HEARTBEAT", "DIAG_GLFW_MOUSE_BUTTON",
}
CONTROL_MARKERS = {"CLOSE_CONTROL", "PROMPT_GEOMETRY", "ALLOW_CLOSE_CONTROL"}
ACTION_RESPONSE_SECONDS = 15.0
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

POINTER_DRIVER_SOURCE = r'''import AppKit
import CoreGraphics
import Darwin
import Foundation

func fail(_ message: String, _ status: Int32) -> Never {
    fputs(message + "\n", stderr)
    exit(status)
}

func postKeyboard(_ keyCode: CGKeyCode, _ isDown: Bool, _ flags: CGEventFlags, _ source: CGEventSource,
                  _ actionID: String, _ eventTag: UInt64, _ targetPID: Int32, _ text: String? = nil) {
    guard let event = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: isDown) else {
        fail("CGEvent keyboard creation failed for keycode \(keyCode)", 3)
    }
    event.flags = flags
    if let text, isDown {
        let units = Array(text.utf16)
        units.withUnsafeBufferPointer { buffer in
            event.keyboardSetUnicodeString(stringLength: units.count, unicodeString: buffer.baseAddress!)
        }
    }
    event.setIntegerValueField(.eventSourceUserData, value: Int64(truncatingIfNeeded: eventTag))
    event.post(tap: .cghidEventTap)
    print("CGEventPost=KEY; action_id=\(actionID); target_pid=\(targetPID); keycode=\(keyCode); down=\(isDown ? 1 : 0); " +
          "unicode_units=\(text?.utf16.count ?? 0); post_returned=1; route=global-hid-event-stream")
}

if CommandLine.arguments.count >= 2 &&
   (CommandLine.arguments[1] == "--go-to-folder" || CommandLine.arguments[1] == "--type-text") {
    let mode = CommandLine.arguments[1]
    let expectedCount = mode == "--go-to-folder" ? 4 : 5
    guard CommandLine.arguments.count == expectedCount else {
        fail("keyboard input requires \(mode), PID, action identity" + (mode == "--type-text" ? " and text" : ""), 2)
    }
    guard let targetPID = Int32(CommandLine.arguments[2]), targetPID > 0 else {
        fail("keyboard target PID must be a positive 32-bit integer", 2)
    }
    let actionID = CommandLine.arguments[3]
    guard actionID.range(of: "^[0-9a-fA-F]{32}$", options: .regularExpression) != nil,
          let eventTag = UInt64(actionID.prefix(16), radix: 16) else {
        fail("keyboard action identity must be exactly 32 hexadecimal characters", 2)
    }
    guard CGPreflightPostEventAccess() else {
        fail("TCC_DENIED: CGPreflightPostEventAccess rejected native-dialog keyboard input", 77)
    }
    guard NSWorkspace.shared.frontmostApplication?.processIdentifier == targetPID else {
        fail("keyboard input refused because the expected native-dialog process is not frontmost", 4)
    }
    guard let source = CGEventSource(stateID: .hidSystemState) else {
        fail("CGEventSource creation failed for native-dialog keyboard input", 3)
    }
    if mode == "--go-to-folder" {
        postKeyboard(5, true, [.maskCommand, .maskShift], source, actionID, eventTag, targetPID)
        postKeyboard(5, false, [.maskCommand, .maskShift], source, actionID, eventTag, targetPID)
    } else {
        let text = CommandLine.arguments[4]
        guard !text.isEmpty else { fail("native-dialog keyboard text must not be empty", 2) }
        postKeyboard(0, true, [], source, actionID, eventTag, targetPID, text)
        postKeyboard(0, false, [], source, actionID, eventTag, targetPID)
    }
    print("CGEventPost=KEY_SEQUENCE_RETURNED; action_id=\(actionID); source_pid=\(getpid()); target_pid=\(targetPID); " +
          "access=preflight-approved; post_return_is_not_application_acknowledgement")
    exit(0)
}

guard CommandLine.arguments.count == 5 else {
    fail("CGEvent input requires PID, click identity and finite desktop coordinates", 2)
}
guard let targetPID = Int32(CommandLine.arguments[1]), targetPID > 0 else {
    fail("CGEvent input PID must be a positive 32-bit integer", 2)
}
let actionID = CommandLine.arguments[2]
guard actionID.range(of: "^[0-9a-fA-F]{32}$", options: .regularExpression) != nil else {
    fail("CGEvent action identity must be exactly 32 hexadecimal characters", 2)
}
guard let eventTag = UInt64(actionID.prefix(16), radix: 16) else {
    fail("CGEvent action identity does not contain a valid event tag", 2)
}
guard let x = Double(CommandLine.arguments[3]), let y = Double(CommandLine.arguments[4]) else {
    fail("CGEvent coordinates must be numeric", 2)
}
guard x.isFinite, y.isFinite else {
    fail("CGEvent input coordinates are not finite", 2)
}
guard CGPreflightPostEventAccess() else {
    fail("TCC_DENIED: CGPreflightPostEventAccess rejected synthetic pointer input", 77)
}

let location = CGPoint(x: x, y: y)
let sourcePID = getpid()
let sourceName = String(cString: getprogname())
func post(_ type: CGEventType, _ name: String, _ clickState: Int64) {
    let attemptUptime = ProcessInfo.processInfo.systemUptime
    guard let event = CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: location, mouseButton: .left) else {
        fail("CGEventCreateMouseEvent failed for event type \(type.rawValue)", 3)
    }
    event.setIntegerValueField(.mouseEventClickState, value: clickState)
    event.setIntegerValueField(.eventSourceUserData, value: Int64(truncatingIfNeeded: eventTag))
    event.post(tap: .cghidEventTap)
    let returnedUptime = ProcessInfo.processInfo.systemUptime
    print("CGEventPost=EVENT; click_id=\(actionID); event_tag=\(eventTag); event=\(name); attempt_uptime=\(attemptUptime); " +
          "returned_uptime=\(returnedUptime); source_pid=\(sourcePID); source_name=\(sourceName); target_pid=\(targetPID); " +
          "point=\(Int(x.rounded())),\(Int(y.rounded())); click_state=\(clickState); post_returned=1; route=global-hid-event-stream")
}

post(.mouseMoved, "move", 0)
Thread.sleep(forTimeInterval: 0.05)
post(.leftMouseDown, "down", 1)
Thread.sleep(forTimeInterval: 0.05)
post(.leftMouseUp, "up", 1)
print("CGEventPost=SEQUENCE_RETURNED; click_id=\(actionID); source_pid=\(sourcePID); source_name=\(sourceName); " +
      "target_pid=\(targetPID); access=preflight-approved; post_return_is_not_application_acknowledgement")
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
    set stage to "PROCESS_LOOKUP"
    try
        tell application "System Events"
            set targetProcess to first process whose unix id is targetPID
            tell targetProcess
                set stage to "WINDOW_LOOKUP"
                set matches to {}
                repeat with targetWindow in windows
                    set windowTitle to name of targetWindow as text
                    if windowTitle contains expectedFragment then set end of matches to targetWindow
                end repeat
                if (count of matches) is not 1 then error "Expected one main PulseForge Editor window for PID " & (targetPID as text)
                set stage to "CLOSE_BUTTON_LOOKUP"
                set closeControl to value of attribute "AXCloseButton" of item 1 of matches
                if closeControl is missing value then error "Main editor AXCloseButton is missing"
                set stage to "ACTION_DISPATCH"
                perform action "AXPress" of closeControl
                return "MACOS_DOC_CLOSE_MAIN_AX_PRESS=PASS; pid=" & (targetPID as text)
            end tell
        end tell
    on error errorMessage number errorNumber
        error ("MACOS_DOC_CLOSE_AX_FAILURE; stage=" & stage & "; AXError=" & (errorNumber as text) & "; detail=" & errorMessage) number errorNumber
    end try
end run
'''


class BoundedCapture:
    def __init__(self, process: subprocess.Popen[str], log_path: Path) -> None:
        self.process = process
        self.log_path = log_path
        self.events: queue.Queue[dict[str, str]] = queue.Queue(maxsize=MAX_EVENT_COUNT)
        self.pending: deque[dict[str, str]] = deque()
        self.telemetry: deque[dict[str, str]] = deque(maxlen=MAX_EVENT_COUNT)
        self.controls: deque[dict[str, str]] = deque(maxlen=MAX_EVENT_COUNT)
        self.telemetry_dropped = 0
        self.control_overflow_error: str | None = None
        self.failure_screenshot: str | None = None
        self.capture_index = 0
        self.capture_index_lock = threading.Lock()
        self.control_lock = threading.Lock()
        self.telemetry_lock = threading.Lock()
        self.lines_read = 0
        self.buffer = bytearray()
        self.console_bytes = 0
        self.console_output_failed = False
        self.log_truncated = False
        self.console_truncated = False
        self.reader_error: str | None = None
        self.queue_overflow_error: str | None = None
        self.reader_finished = threading.Event()
        self.thread = threading.Thread(target=self._read, name="PulseForge close-smoke output", daemon=True)
        self.thread.start()

    def _read(self) -> None:
        assert self.process.stdout is not None
        try:
            for line in self.process.stdout:
                self.lines_read += 1
                encoded = line.encode("utf-8", errors="replace")
                remaining = MAX_LOG_BYTES - len(self.buffer)
                if remaining > 0:
                    self.buffer.extend(encoded[:remaining])
                if len(encoded) > remaining:
                    self.log_truncated = True

                if self.console_bytes < MAX_CONSOLE_BYTES and not self.console_output_failed:
                    visible = encoded[: MAX_CONSOLE_BYTES - self.console_bytes]
                    try:
                        sys.stdout.write(visible.decode("utf-8", errors="replace"))
                        sys.stdout.flush()
                        self.console_bytes += len(visible)
                    except (OSError, ValueError) as error:
                        self.console_output_failed = True
                        self.reader_error = self.reader_error or (
                            f"could not forward editor output to the workflow log: {type(error).__name__}: {error}"
                        )
                    if len(visible) < len(encoded):
                        self.console_truncated = True
                else:
                    self.console_truncated = True

                try:
                    marker = parse_marker(line.rstrip("\r\n"))
                except Exception as error:
                    self.reader_error = self.reader_error or f"marker parser failed: {type(error).__name__}: {error}"
                    continue
                if marker is None:
                    continue
                with self.capture_index_lock:
                    self.capture_index += 1
                    marker["_capture_index"] = str(self.capture_index)
                if marker.get("kind") in TELEMETRY_MARKERS or marker.get("kind", "").startswith("DIAG_"):
                    with self.telemetry_lock:
                        if len(self.telemetry) == self.telemetry.maxlen:
                            self.telemetry_dropped += 1
                        self.telemetry.append(marker)
                    continue
                if marker.get("kind") in CONTROL_MARKERS:
                    with self.control_lock:
                        if len(self.controls) == self.controls.maxlen:
                            self.control_overflow_error = self.control_overflow_error or (
                                f"editor emitted more than {self.controls.maxlen} unconsumed control-geometry markers"
                            )
                        self.controls.append(marker)
                    continue
                try:
                    self.events.put_nowait(marker)
                except queue.Full:
                    self.queue_overflow_error = self.queue_overflow_error or (
                        f"editor emitted more than {self.events.maxsize} unconsumed semantic close-fixture markers"
                    )
        except Exception as error:
            self.reader_error = self.reader_error or f"{type(error).__name__}: {error}"
        finally:
            try:
                marker = b"\n[output truncated at 128 KiB]\n" if self.log_truncated else b""
                self.log_path.write_bytes(self.buffer[: MAX_LOG_BYTES - len(marker)] + marker)
                if self.console_truncated:
                    print("\n[editor console output truncated at 48 KiB]")
            except OSError as error:
                self.reader_error = self.reader_error or f"could not write bounded log {self.log_path}: {error}"
            self.reader_finished.set()

    def watermark(self) -> int:
        with self.capture_index_lock:
            return self.capture_index

    def take_controls(self) -> list[dict[str, str]]:
        with self.control_lock:
            controls = list(self.controls)
            self.controls.clear()
            return controls

    def restore_controls(self, controls: list[dict[str, str]]) -> None:
        with self.control_lock:
            for marker in reversed(controls):
                self.controls.appendleft(marker)

    def recent_telemetry(self, expected: dict[str, str]) -> list[dict[str, str]]:
        with self.telemetry_lock:
            return [
                marker
                for marker in self.telemetry
                if all(marker.get(key) == value for key, value in expected.items())
            ][-8:]


@dataclass
class ActionEvidence:
    label: str
    action_id: str = ""
    dispatch_attempted: bool = False
    api_exit_code: int | None = None
    api_result: str = "not attempted"
    transport_uncertain: bool = False
    pointer_observation: str = "NOT_REQUESTED"
    application_response: str = "PENDING"
    semantic_outcome: str = "PENDING"
    output_watermark: int = -1
    point: tuple[int, int] | None = None
    target_window_bounds: tuple[float, float, float, float] | None = None
    expected_marker_fields: dict[str, str] | None = None

    def summary(self) -> str:
        return (
            f"label={self.label}; dispatch_attempted={int(self.dispatch_attempted)}; "
            f"action_id={self.action_id}; "
            f"api_exit_code={self.api_exit_code}; transport_uncertain={int(self.transport_uncertain)}; "
            f"api_result={self.api_result!r}; "
            f"pointer_observation={self.pointer_observation}; application_response={self.application_response}; "
            f"semantic_outcome={self.semantic_outcome}; initial_ax_bounds={self.target_window_bounds!r}"
        )


class UnsafeChildCleanupError(RuntimeError):
    """A helper process group survived its bounded cleanup and may still own the desktop."""


class MarkerProtocol:
    def __init__(self, scenario: str, host: str, asset: str, generation: int) -> None:
        self.scenario = scenario
        self.host = host
        self.asset = asset
        self.generation = generation
        self.last_sequence = 0
        self.control_samples: dict[str, int] = {}
        self.prompt_geometry_sample = -1
        self.prompt_geometry: dict[str, str] = {}
        self.last_action_capture_index = -1

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
        if generation != self.generation + 1:
            raise RuntimeError(
                f"prompt generation must advance exactly once from {self.generation}; marker={event.get('raw')!r}"
            )
        sample = self.marker_sample(event)
        if sample <= self.prompt_geometry_sample:
            raise RuntimeError(
                f"prompt geometry sample did not advance beyond {self.prompt_geometry_sample}: {event.get('raw')!r}"
            )
        self.generation = generation
        self.prompt_geometry_sample = sample
        self.prompt_geometry = event.copy()
        return generation

    @staticmethod
    def marker_sample(event: dict[str, str]) -> int:
        try:
            return int(event["sample"])
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"control marker has invalid or missing sample: {event.get('raw')!r}") from error

    @staticmethod
    def marker_capture_index(event: dict[str, str]) -> int:
        try:
            return int(event["_capture_index"])
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"marker has invalid or missing capture index: {event.get('raw')!r}") from error

    def accept_prompt_geometry(self, event: dict[str, str]) -> None:
        self.check_identity(event)
        if self.marker_generation(event) != self.generation:
            raise RuntimeError(f"prompt geometry belongs to a stale generation: {event.get('raw')!r}")
        sample = self.marker_sample(event)
        if sample <= self.prompt_geometry_sample:
            raise RuntimeError(
                f"prompt geometry sample did not advance beyond {self.prompt_geometry_sample}: {event.get('raw')!r}"
            )
        self.prompt_geometry_sample = sample
        self.prompt_geometry = event.copy()

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
        if sequence != self.last_sequence + 1:
            raise RuntimeError(
                f"action sequence did not advance exactly once from {self.last_sequence}: {event.get('raw')!r}"
            )
        self.last_sequence = sequence
        self.last_action_capture_index = self.marker_capture_index(event)

    def accept_fixture(self, event: dict[str, str]) -> int:
        self.check_identity(event)
        generation = self.marker_generation(event)
        if generation != self.generation + 1:
            raise RuntimeError(
                f"fixture control generation must advance exactly once from {self.generation}; marker={event.get('raw')!r}"
            )
        self.generation = generation
        return generation

    def accept_control(self, event: dict[str, str]) -> int:
        self.check_identity(event)
        generation = self.marker_generation(event)
        if generation < self.generation or generation > self.generation + 1:
            raise RuntimeError(
                f"close-control generation is stale or skipped a state transition from {self.generation}; "
                f"marker={event.get('raw')!r}"
            )
        sample = self.marker_sample(event)
        kind = event.get("kind", "")
        previous_sample = self.control_samples.get(kind, -1)
        if sample <= previous_sample:
            raise RuntimeError(
                f"{kind} geometry sample did not advance beyond {previous_sample}: {event.get('raw')!r}"
            )
        self.control_samples[kind] = sample
        self.generation = generation
        return generation

    def accept_pass(self, event: dict[str, str]) -> None:
        self.check_identity(event)
        if self.marker_generation(event) != self.generation:
            raise RuntimeError(f"PASS generation does not match the terminal fixture state: {event.get('raw')!r}")
        try:
            sequence = int(event["sequence"])
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"PASS marker has invalid or missing sequence: {event.get('raw')!r}") from error
        if sequence != self.last_sequence or event.get("open") != "0":
            raise RuntimeError(
                f"PASS is not correlated with the final action/session state; expected sequence={self.last_sequence}; "
                f"marker={event.get('raw')!r}"
            )

    def accept_closed(self, event: dict[str, str]) -> None:
        self.check_identity(event)
        generation = self.marker_generation(event)
        if generation != self.generation:
            raise RuntimeError(
                f"closed marker must match the final close-control generation {self.generation}; marker={event.get('raw')!r}"
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
    diagnostic = False
    marker_start = line.find("PF_DOC_CLOSE_DIAG|")
    if marker_start >= 0:
        diagnostic = True
    else:
        marker_start = line.find("PF_DOC_CLOSE_UI|")
    if marker_start < 0:
        return None
    line = line[marker_start:]
    parts = line.split("|")
    if len(parts) < 2:
        return {"kind": "INVALID"}
    event = {"kind": f"DIAG_{parts[1]}" if diagnostic else parts[1]}
    for field in parts[2:]:
        key, separator, value = field.partition("=")
        if not separator or not key or key in event:
            return {"kind": "INVALID", "raw": line[:1024]}
        event[key] = value
    event["raw"] = line[:1024]
    return event


def run_osascript(script: str, *arguments: str, timeout: float) -> tuple[int, str]:
    try:
        process = subprocess.Popen(
            ["/usr/bin/osascript", "-", *arguments],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
    except OSError as error:
        return 127, f"could not launch /usr/bin/osascript: {type(error).__name__}: {error}"
    try:
        output, _ = process.communicate(input=script, timeout=max(0.1, timeout))
    except subprocess.TimeoutExpired as error:
        stopped = stop_process_group(process)
        try:
            output, _ = process.communicate(timeout=1.0)
        except subprocess.TimeoutExpired:
            output = ""
        detail = (output + "\nAppleScript operation timed out\n")[-4096:]
        if not stopped:
            raise UnsafeChildCleanupError(
                f"AppleScript helper process group PID {process.pid} survived bounded timeout cleanup"
            ) from error
        return 124, detail
    return process.returncode, output[-4096:]


def is_tcc_denial(output: str) -> bool:
    lowered = output.lower()
    return any(pattern in lowered for pattern in TCC_DENIALS)


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
            raise UnsafeChildCleanupError(f"timed-out child process group remains active: PID {process.pid}") from error
        return 124, stdout[-4096:], (stderr + f"\ncommand timed out after {timeout:.1f}s").strip()[-4096:]
    return process.returncode, stdout[-4096:], stderr[-4096:]


def bounded_timeout(deadline: float, cap: float = 8.0) -> float:
    return max(0.1, min(cap, deadline - time.monotonic()))


def is_transport_timeout(exit_code: int, output: str) -> bool:
    lowered = output.lower()
    return exit_code == 124 or "-1712" in lowered or "timed out" in lowered or "timeout" in lowered


def reconcile_action_response(evidence: ActionEvidence, response_observed: bool) -> str:
    if not evidence.dispatch_attempted:
        evidence.application_response = "NOT_EXPECTED"
        evidence.semantic_outcome = "FAIL_NOT_DISPATCHED"
        raise RuntimeError(f"{evidence.label}: action dispatch was not attempted; {evidence.summary()}")
    if not response_observed:
        evidence.application_response = "MISSING"
        evidence.semantic_outcome = "FAIL_RESPONSE_MISSING"
        raise RuntimeError(f"{evidence.label}: expected application response missing; {evidence.summary()}")

    evidence.application_response = "OBSERVED"
    if evidence.api_exit_code == 0:
        evidence.semantic_outcome = "PASS"
        return evidence.semantic_outcome
    if evidence.transport_uncertain:
        evidence.semantic_outcome = "PASS_TRANSPORT_UNCERTAIN_RECONCILED"
        return evidence.semantic_outcome

    evidence.semantic_outcome = "FAIL_DISPATCH_RESULT"
    raise RuntimeError(
        f"{evidence.label}: correlated application response arrived after a failed dispatch API result; "
        f"{evidence.summary()}"
    )


def verify_successful_exit(exit_code: int | None, fixture_completion: dict[str, str] | None) -> None:
    if fixture_completion is None or fixture_completion.get("kind") != "PASS":
        raise RuntimeError("expected validated fixture completion before orderly application shutdown was not observed")
    if exit_code is None:
        raise RuntimeError("child process remains open after the bounded shutdown wait")
    if exit_code != 0:
        raise RuntimeError(f"child process exited with status {exit_code} despite prior success evidence")


def wait_event(
    capture: BoundedCapture,
    process: subprocess.Popen[str],
    kind: str,
    scenario: str,
    deadline: float,
    *,
    after_index: int | None = None,
) -> dict[str, str]:
    while time.monotonic() < deadline:
        if capture.reader_error:
            raise RuntimeError(f"editor output capture failed: {capture.reader_error}")
        if capture.queue_overflow_error:
            raise RuntimeError(f"editor semantic-marker queue overflow: {capture.queue_overflow_error}")
        if capture.control_overflow_error:
            raise RuntimeError(f"editor control-geometry queue overflow: {capture.control_overflow_error}")
        event = None
        for pending in tuple(capture.pending):
            if pending.get("kind") == kind and pending.get("scenario") == scenario:
                capture.pending.remove(pending)
                event = pending
                break
        if event is None:
            try:
                event = capture.events.get_nowait()
            except queue.Empty:
                if process.poll() is not None:
                    raise RuntimeError(f"editor exited before {kind}; exit_code={process.returncode}")
                try:
                    event = capture.events.get(timeout=min(0.25, max(0.01, deadline - time.monotonic())))
                except queue.Empty:
                    continue
        if event.get("kind") == "FAIL":
            raise RuntimeError(f"fixture reported failure: {event.get('raw', '<invalid marker>')}")
        if event.get("kind") == "INVALID":
            raise RuntimeError(f"malformed fixture marker: {event.get('raw', '<invalid marker>')!r}")
        if event.get("scenario") != scenario:
            raise RuntimeError(f"fixture marker has wrong scenario: {event.get('raw', event)!r}")
        if event.get("kind") == kind:
            capture_index = MarkerProtocol.marker_capture_index(event)
            if after_index is not None and capture_index <= after_index:
                raise RuntimeError(
                    f"stale {kind} event predates the current action; watermark={after_index}; "
                    f"event_index={capture_index}; marker={event.get('raw')!r}"
                )
            return event
        if event.get("kind") in {"READY", "PROMPT", "ACTION", "CLOSED", "PASS"}:
            raise RuntimeError(f"unexpected {event.get('kind')} event while waiting for {kind}: {event.get('raw')!r}")
        if event.get("kind") in {"FIXTURE", "TEXT_CONTROL", "TEXT_FOCUS", "TEXT_ENTRY", "CLOSE_CONTROL"}:
            if event.get("kind") == "FIXTURE":
                capture.pending = deque(pending for pending in capture.pending if pending.get("kind") != "FIXTURE")
            if len(capture.pending) >= MAX_EVENT_COUNT:
                capture.queue_overflow_error = capture.queue_overflow_error or (
                    f"more than {MAX_EVENT_COUNT} semantic markers were retained while waiting for {kind}"
                )
                raise RuntimeError(f"editor semantic-marker queue overflow: {capture.queue_overflow_error}")
            capture.pending.append(event)
            continue
        raise RuntimeError(f"unexpected marker while waiting for {kind}: {event.get('raw')!r}")
    raise RuntimeError(f"timed out waiting for {kind} marker for {scenario}")


def await_application_event(
    capture: BoundedCapture,
    process: subprocess.Popen[str],
    kind: str,
    scenario: str,
    deadline: float,
    evidence: ActionEvidence,
    validate: Callable[[dict[str, str]], object],
    *,
    after_index: int,
    response_seconds: float = ACTION_RESPONSE_SECONDS,
    missing_message: str | None = None,
    diagnose: Callable[[BoundedCapture, subprocess.Popen[str], ActionEvidence, float], str] | None = None,
) -> dict[str, str]:
    collect_diagnostics = diagnose or capture_action_diagnostics
    if not evidence.dispatch_attempted:
        reconcile_action_response(evidence, response_observed=False)
    if evidence.api_exit_code != 0 and not evidence.transport_uncertain:
        evidence.application_response = "NOT_WAITED_DISPATCH_FAILED"
        evidence.semantic_outcome = "FAIL_DISPATCH_RESULT"
        detail = "; ".join(evidence.api_result.splitlines())[-1000:]
        diagnostics = collect_diagnostics(capture, process, evidence, deadline)
        raise RuntimeError(
            f"{evidence.label}: dispatch API failed; exit_code={evidence.api_exit_code}; {detail}; "
            f"{evidence.summary()}; diagnostics={diagnostics}"
        )

    response_deadline = min(deadline, time.monotonic() + response_seconds)
    try:
        event = wait_event(capture, process, kind, scenario, response_deadline, after_index=after_index)
    except RuntimeError as error:
        message = str(error)
        missing = message.startswith("timed out waiting for ") or message.startswith("editor exited before ")
        evidence.application_response = "MISSING" if missing else "INCONSISTENT"
        evidence.semantic_outcome = "FAIL_RESPONSE_MISSING" if missing else "FAIL_RESPONSE_INCONSISTENT"
        diagnostics = collect_diagnostics(capture, process, evidence, deadline)
        label = missing_message if missing and missing_message else (
            "expected application response missing" if missing else "application response was inconsistent"
        )
        raise RuntimeError(f"{kind} response after {evidence.label}: {label}; {evidence.summary()}; diagnostics={diagnostics}; {error}") from error

    try:
        validate(event)
        outcome = reconcile_action_response(evidence, response_observed=True)
    except RuntimeError:
        evidence.semantic_outcome = "FAIL_RESPONSE_INCONSISTENT"
        diagnostics = collect_diagnostics(capture, process, evidence, deadline)
        print(f"MACOS_DOC_CLOSE_ACTION_VALIDATION=FAILED; {evidence.summary()}; diagnostics={diagnostics}")
        raise
    print(
        f"MACOS_DOC_CLOSE_ACTION_OUTCOME=OBSERVED; action={evidence.label}; event={kind}; "
        f"transport={outcome}; {evidence.summary()}"
    )
    return event


def capture_action_diagnostics(
    capture: BoundedCapture,
    process: subprocess.Popen[str],
    evidence: ActionEvidence,
    deadline: float,
) -> str:
    if evidence.point is None:
        evidence.pointer_observation = "NOT_AVAILABLE_NO_TARGET_POINT"
    else:
        remaining = deadline - time.monotonic()
        if remaining <= 0.1:
            evidence.pointer_observation = "NOT_AVAILABLE_DEADLINE_EXHAUSTED"
        else:
            code, output = run_osascript(
                POINTER_OBSERVE,
                str(evidence.point[0]),
                str(evidence.point[1]),
                timeout=min(1.5, remaining),
            )
            if code == 0:
                evidence.pointer_observation = "AVAILABLE"
            elif is_tcc_denial(output):
                    evidence.pointer_observation = f"UNAVAILABLE:TCC_DENIED:{output.strip()[-500:]}"
            elif code == 124:
                    evidence.pointer_observation = f"UNAVAILABLE:TIMED_OUT:{output.strip()[-500:]}"
            else:
                    evidence.pointer_observation = f"UNAVAILABLE:ERROR({code}):{output.strip()[-500:]}"

    stack_sample = "NOT_REQUESTED"
    if evidence.application_response == "MISSING" and process.poll() is None:
        stack_sample = capture_owned_editor_stack_sample(process, capture, evidence, deadline)
    automation_context = capture_failure_context("dirty-close-action-outcome", process.pid)
    print(f"MACOS_DOC_CLOSE_AUTOMATION_CONTEXT={automation_context!r}")
    screenshot = capture_screenshot(
        f"macos-document-close-{evidence.label.lower().replace(' ', '-')}-outcome-missing.png"
    )
    capture.failure_screenshot = screenshot
    screenshot_state = screenshot
    telemetry = capture.recent_telemetry(evidence.expected_marker_fields or {})
    state = "running" if process.poll() is None else f"exited({process.returncode})"
    return (
        f"process={state}; pointer={evidence.pointer_observation}; screenshot={screenshot_state}; "
        f"stack_sample={stack_sample}; "
        f"recent_telemetry={[marker.get('raw') for marker in telemetry]!r}; "
        f"dropped_telemetry={capture.telemetry_dropped}"
    )


def capture_owned_editor_stack_sample(
    process: subprocess.Popen[str], capture: BoundedCapture, evidence: ActionEvidence, deadline: float
) -> str:
    if process.poll() is not None:
        return f"NOT_CAPTURED_PROCESS_EXITED({process.returncode})"
    scenario = (evidence.expected_marker_fields or {}).get("scenario", "unknown")
    action = evidence.action_id[:12] or "unidentified"
    name = f"macos-document-close-{scenario}-{action}-stack-sample.txt"
    output_path = capture.log_path.with_name(name)
    timeout = min(3.0, max(0.1, deadline - time.monotonic()))
    try:
        exit_code, stdout, stderr = run_bounded_command(
            ["/usr/bin/sample", str(process.pid), "1", "-file", str(output_path)], timeout=timeout
        )
    except (OSError, UnsafeChildCleanupError) as error:
        return f"CAPTURE_FAILED:{type(error).__name__}:{error}"
    if exit_code != 0 or not output_path.is_file():
        return f"CAPTURE_FAILED:exit_code={exit_code}; stdout={stdout[-500:]!r}; stderr={stderr[-500:]!r}"
    try:
        with output_path.open("rb") as stream:
            sample = stream.read(128 * 1024 + 1)
        truncated = len(sample) > 128 * 1024 or output_path.stat().st_size > 128 * 1024
        if truncated:
            sample = sample[: 128 * 1024] + b"\n[stack sample truncated at 128 KiB]\n"
            output_path.write_bytes(sample)
    except OSError as error:
        return f"CAPTURE_FAILED:could not bound or inspect output: {type(error).__name__}:{error}"
    return f"CAPTURED:path={output_path}; bytes={len(sample)}; truncated={int(truncated)}"


def drain_prompt_geometry(capture: BoundedCapture, protocol: MarkerProtocol) -> None:
    preserved: list[dict[str, str]] = []
    for marker in capture.take_controls():
        if marker.get("kind") != "PROMPT_GEOMETRY":
            preserved.append(marker)
            continue
        protocol.check_identity(marker)
        generation = MarkerProtocol.marker_generation(marker)
        if generation < protocol.generation:
            print(
                f"MACOS_DOC_CLOSE_CONTROL_GEOMETRY=STALE_IGNORED; expected_generation={protocol.generation}; "
                f"marker={marker.get('raw')!r}"
            )
            continue
        protocol.accept_prompt_geometry(marker)
    capture.restore_controls(preserved)


def wait_close_control(
    capture: BoundedCapture,
    process: subprocess.Popen[str],
    protocol: MarkerProtocol,
    deadline: float,
    kind: str = "CLOSE_CONTROL",
) -> dict[str, str]:
    while time.monotonic() < deadline:
        if capture.reader_error:
            raise RuntimeError(f"editor output capture failed: {capture.reader_error}")
        if capture.queue_overflow_error:
            raise RuntimeError(f"editor semantic-marker queue overflow: {capture.queue_overflow_error}")
        if capture.control_overflow_error:
            raise RuntimeError(f"editor control-geometry queue overflow: {capture.control_overflow_error}")
        candidate: dict[str, str] | None = None
        preserved: list[dict[str, str]] = []
        for marker in capture.take_controls():
            if marker.get("kind") == "PROMPT_GEOMETRY":
                protocol.check_identity(marker)
                generation = MarkerProtocol.marker_generation(marker)
                if generation == protocol.generation:
                    protocol.accept_prompt_geometry(marker)
                elif generation > protocol.generation:
                    raise RuntimeError(f"future prompt geometry arrived unexpectedly: {marker.get('raw')!r}")
                else:
                    print(
                        f"MACOS_DOC_CLOSE_CONTROL_GEOMETRY=STALE_IGNORED; expected_generation={protocol.generation}; "
                        f"marker={marker.get('raw')!r}"
                    )
                continue
            if marker.get("kind") != kind:
                preserved.append(marker)
                continue
            protocol.check_identity(marker)
            protocol.accept_control(marker)
            candidate = marker
        capture.restore_controls(preserved)
        if candidate is not None:
            print(
                f"MACOS_DOC_CLOSE_CONTROL=FRESH; kind={kind}; generation={protocol.generation}; "
                f"sample={candidate.get('sample')}; point=({candidate.get('x')},{candidate.get('y')})"
            )
            return candidate
        if process.poll() is not None:
            raise RuntimeError(f"editor exited before a fresh CLOSE_CONTROL marker; exit_code={process.returncode}")
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
    raise RuntimeError(f"timed out waiting for a fresh {kind} geometry marker")


def query_fixture_window(
    process: subprocess.Popen[str], deadline: float
) -> tuple[float, float, float, float] | None:
    timeout = min(1.5, max(0.0, deadline - time.monotonic()))
    if timeout <= 0.1:
        print("MACOS_DOC_CLOSE_AX_BOUNDS=UNAVAILABLE; reason=scenario deadline nearly exhausted")
        return None

    code, output = run_osascript(
        WINDOW_BOUNDS_QUERY,
        str(process.pid),
        FIXTURE_TITLE,
        timeout=timeout,
    )
    if code != 0:
        if is_tcc_denial(output):
            category = "TCC_DENIED"
        elif code == 124 or "-1712" in output or "timed out" in output.lower():
            category = "TIMED_OUT"
        else:
            category = f"ERROR({code})"
        print(
            f"MACOS_DOC_CLOSE_AX_BOUNDS=UNAVAILABLE; category={category}; pid={process.pid}; "
            f"detail={output.strip()[-500:]!r}; app control geometry and correlated responses remain required"
        )
        return None

    bound_lines = [line.strip() for line in output.splitlines() if line.strip()]
    if len(bound_lines) != 3 or "PulseForge" not in bound_lines[0] or bound_lines[1] != FIXTURE_TITLE:
        print(
            f"MACOS_DOC_CLOSE_AX_BOUNDS=UNAVAILABLE; category=INCONSISTENT_IDENTITY; pid={process.pid}; "
            f"detail={output.strip()[-500:]!r}; app control geometry and correlated responses remain required"
        )
        return None
    try:
        window_bounds = tuple(float(value) for value in bound_lines[2].split(","))
    except ValueError:
        print(
            f"MACOS_DOC_CLOSE_AX_BOUNDS=UNAVAILABLE; category=MALFORMED; pid={process.pid}; "
            f"detail={output.strip()[-500:]!r}; app control geometry and correlated responses remain required"
        )
        return None
    if len(window_bounds) != 4 or not all(math.isfinite(value) for value in window_bounds):
        print(
            f"MACOS_DOC_CLOSE_AX_BOUNDS=UNAVAILABLE; category=NONFINITE_OR_INCOMPLETE; pid={process.pid}; "
            f"detail={output.strip()[-500:]!r}; app control geometry and correlated responses remain required"
        )
        return None
    if window_bounds[2] <= 0 or window_bounds[3] <= 0:
        print(
            f"MACOS_DOC_CLOSE_AX_BOUNDS=UNAVAILABLE; category=NO_USABLE_AREA; pid={process.pid}; "
            f"detail={output.strip()[-500:]!r}; app control geometry and correlated responses remain required"
        )
        return None
    print(
        f"MACOS_DOC_CLOSE_AX_BOUNDS=AVAILABLE; pid={process.pid}; process={bound_lines[0]}; "
        f"window={bound_lines[1]}; accessibility_bounds={bound_lines[2]!r}; "
        "ImGui viewport points=desktop logical; AX bounds=desktop logical; no Retina scale conversion applied; "
        "diagnostic only"
    )
    return window_bounds


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
        if isinstance(error, UnsafeChildCleanupError):
            raise
        raise RuntimeError(f"bounded Quartz pointer-driver compilation failed: {error}") from error
    if exit_code != 0 or not executable.is_file():
        raise RuntimeError(
            f"Quartz pointer-driver compilation failed; exit_code={exit_code}; "
            f"stdout={stdout[-2000:]!r}; stderr={stderr[-2000:]!r}"
        )
    return executable


def click_point(
    process: subprocess.Popen[str],
    capture: BoundedCapture,
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float] | None,
    point: tuple[int, int],
    deadline: float,
    label: str,
) -> ActionEvidence:
    evidence = ActionEvidence(
        label=label,
        action_id=uuid.uuid4().hex,
        point=point,
        output_watermark=capture.watermark(),
        target_window_bounds=fixture_bounds,
    )
    print(
        f"MACOS_DOC_CLOSE_TARGET_PREPARATION=PASS; action={label}; pid={process.pid}; "
        f"action_id={evidence.action_id}; fresh_app_geometry_point={point!r}; initial_ax_bounds={fixture_bounds!r}; "
        "no repeated Accessibility bounds query gates dispatch"
    )

    try:
        code, output, error = run_direct_ax(
            process.pid,
            operation="activate-window",
            window_title=FIXTURE_TITLE,
            timeout=bounded_timeout(deadline, 10.0),
        )
        output += error
        activated_title = parse_window_activation(output, process.pid, FIXTURE_TITLE) if code == 0 else ""
    except (OSError, ValueError) as error:
        evidence.api_exit_code = 1
        evidence.api_result = f"intended native document window activation failed before dispatch: {error}"
        evidence.application_response = "NOT_EXPECTED"
        evidence.semantic_outcome = "FAIL_PREPARATION"
        raise RuntimeError(f"{label}: dispatch was not attempted; {evidence.summary()}") from error
    if code != 0:
        evidence.api_exit_code = code
        evidence.api_result = f"intended native document window activation failed: {output.strip()}"
        evidence.application_response = "NOT_EXPECTED"
        evidence.semantic_outcome = "FAIL_PREPARATION"
        raise RuntimeError(f"{label}: dispatch was not attempted; {evidence.summary()}")
    print(
        f"MACOS_DOC_CLOSE_NATIVE_WINDOW_READY=PASS; pid={process.pid}; title={activated_title!r}; "
        f"action_id={evidence.action_id}; focused_window_verified=1"
    )

    evidence.output_watermark = capture.watermark()
    try:
        exit_code, stdout, stderr = run_bounded_command(
            [str(pointer_driver), str(process.pid), evidence.action_id, str(point[0]), str(point[1])],
            timeout=bounded_timeout(deadline, 5.0),
        )
    except UnsafeChildCleanupError as error:
        evidence.dispatch_attempted = True
        evidence.api_exit_code = 124
        evidence.api_result = f"Quartz dispatch timed out and helper cleanup failed: {error}"
        raise
    except OSError as error:
        evidence.dispatch_attempted = False
        evidence.api_exit_code = 127
        evidence.api_result = f"driver launch failed before dispatch: {type(error).__name__}: {error}"
        return evidence
    evidence.dispatch_attempted = True
    evidence.api_exit_code = exit_code
    if exit_code == 0:
        evidence.api_result = stdout.strip() or "Quartz driver returned success"
    elif exit_code == 124:
        evidence.transport_uncertain = True
        evidence.api_result = f"Quartz dispatch timed out; delivery uncertain; {stderr.strip()}"
    elif exit_code == 77:
        evidence.api_result = f"TCC_DENIED: {stderr.strip()}"
    else:
        evidence.api_result = f"Quartz dispatch failed; stdout={stdout[-1000:]!r}; stderr={stderr[-1000:]!r}"
    print(
        f"MACOS_DOC_CLOSE_POINTER_INPUT=RESULT; {evidence.summary()}; api_detail={evidence.api_result!r}; "
        "posting return is transport evidence only"
    )
    return evidence


def click_document_tab_close(
    process: subprocess.Popen[str],
    capture: BoundedCapture,
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float],
    deadline: float,
    protocol: MarkerProtocol,
    label: str,
    response_kind: str = "PROMPT",
) -> tuple[dict[str, str], ActionEvidence]:
    drain_prompt_geometry(capture, protocol)
    control = wait_close_control(capture, process, protocol, deadline)
    point = parse_point(f"{control.get('x', '')},{control.get('y', '')}", "document tab close")
    evidence = click_point(process, capture, pointer_driver, fixture_bounds, point, deadline, label)
    evidence.expected_marker_fields = {
        "scenario": protocol.scenario,
        "host": protocol.host,
        "asset": protocol.asset,
    }

    def validate(event: dict[str, str]) -> None:
        protocol.check_identity(event)
        if response_kind == "PROMPT":
            protocol.accept_prompt(event)
        elif response_kind == "CLOSED":
            protocol.accept_closed(event)
        else:
            raise RuntimeError(f"unsupported close-tab response kind: {response_kind}")

    event = await_application_event(
        capture,
        process,
        response_kind,
        protocol.scenario,
        deadline,
        evidence,
        validate,
        after_index=evidence.output_watermark,
        missing_message="Close response missing",
    )
    if response_kind == "PROMPT":
        screenshot = capture_screenshot(
            f"macos-document-close-{protocol.scenario}-checkpoint-prompt-generation-{protocol.generation}.png"
        )
        print(
            f"MACOS_DOC_CLOSE_SCREENSHOT=checkpoint=prompt; scenario={protocol.scenario}; "
            f"generation={protocol.generation}; {screenshot}"
        )
    return event, evidence


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
    focus_evidence = click_point(process, capture, pointer_driver, fixture_bounds, point, deadline, "Editable value")
    focus_evidence.expected_marker_fields = {
        "scenario": protocol.scenario,
        "host": protocol.host,
        "asset": protocol.asset,
        "generation": "0",
    }

    def validate_focus(event: dict[str, str]) -> None:
        protocol.check_identity(event)
        if event.get("generation") != "0" or event.get("active") != "1" or event.get("clicked") != "1":
            raise RuntimeError(f"Input did not activate: {event.get('raw')!r}")
        if event.get("value") != "20":
            raise RuntimeError(f"Value changed during activation: {event.get('raw')!r}")

    await_application_event(
        capture,
        process,
        "TEXT_FOCUS",
        protocol.scenario,
        deadline,
        focus_evidence,
        validate_focus,
        after_index=focus_evidence.output_watermark,
        missing_message="Input did not activate",
    )

    type_evidence = ActionEvidence(
        label="OS keyboard edit",
        dispatch_attempted=True,
        output_watermark=capture.watermark(),
        point=point,
        expected_marker_fields={
            "scenario": protocol.scenario,
            "host": protocol.host,
            "asset": protocol.asset,
            "generation": "0",
        },
    )
    try:
        code, output = run_osascript(TYPE_EDITOR_VALUE, str(process.pid), timeout=bounded_timeout(deadline, 8.0))
        type_evidence.dispatch_attempted = code != 127
        type_evidence.api_exit_code = code
        type_evidence.transport_uncertain = is_transport_timeout(code, output)
        type_evidence.api_result = output.strip() if code in (0, 124) else (
            f"TCC_DENIED: {output.strip()}" if is_tcc_denial(output) else f"AppleScript dispatch failed: {output.strip()}"
        )
    except UnsafeChildCleanupError as error:
        type_evidence.dispatch_attempted = True
        type_evidence.api_exit_code = 124
        type_evidence.api_result = f"AppleScript keyboard dispatch timed out; helper cleanup failed: {error}"
        raise

    def validate_text_entry(entered: dict[str, str]) -> None:
        protocol.check_identity(entered)
        expected = {"generation": "0", "value": "42", "saved": "10", "dirty": "1"}
        if any(entered.get(key) != value for key, value in expected.items()):
            raise RuntimeError(f"editor did not retain the entered value as a dirty document: {entered.get('raw')!r}")

    await_application_event(
        capture,
        process,
        "TEXT_ENTRY",
        protocol.scenario,
        deadline,
        type_evidence,
        validate_text_entry,
        after_index=type_evidence.output_watermark,
        missing_message="Keyboard edit not observed",
    )
    screenshot = capture_screenshot(
        f"macos-document-close-{protocol.scenario}-checkpoint-text-entry.png"
    )
    print(f"MACOS_DOC_CLOSE_SCREENSHOT=checkpoint=text-entry; scenario={protocol.scenario}; {screenshot}")
    print(f"MACOS_DOC_CLOSE_TEXT_ENTRY=PASS; scenario={protocol.scenario}; value=42; saved=10; dirty=1")


def get_prompt_point(capture: BoundedCapture, protocol: MarkerProtocol, choice: str) -> tuple[int, int]:
    drain_prompt_geometry(capture, protocol)
    geometry = protocol.prompt_geometry
    if geometry.get("host") != protocol.host or geometry.get("scenario") != protocol.scenario:
        raise RuntimeError(f"prompt control geometry does not match the current document: {geometry.get('raw')!r}")
    if MarkerProtocol.marker_generation(geometry) != protocol.generation:
        raise RuntimeError(f"prompt control geometry is stale: {geometry.get('raw')!r}")
    return parse_point(geometry.get(choice, ""), choice)


def expect_action(
    capture: BoundedCapture,
    process: subprocess.Popen[str],
    deadline: float,
    protocol: MarkerProtocol,
    evidence: ActionEvidence,
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

    def validate(action: dict[str, str]) -> None:
        protocol.check_identity(action)
        for key, value_expected in expected.items():
            if action.get(key) != value_expected:
                raise RuntimeError(
                    f"unexpected {choice} state for {protocol.scenario}: field {key} expected {value_expected!r}, "
                    f"got {action.get(key)!r}; marker={action.get('raw')!r}"
                )
        protocol.accept_action(action, choice, expected_generation)

    evidence.expected_marker_fields = {
        "scenario": protocol.scenario,
        "host": protocol.host,
        "asset": protocol.asset,
        "generation": str(expected_generation),
        "choice": choice,
    }
    action = await_application_event(
        capture,
        process,
        "ACTION",
        protocol.scenario,
        deadline,
        evidence,
        validate,
        after_index=evidence.output_watermark,
    )
    print(
        f"MACOS_DOC_CLOSE_FIXTURE_ACTION=PASS; scenario={protocol.scenario}; "
        f"choice={choice}; resolved={resolved}; pending={pending}; open={opened}; dirty={dirty}; "
        f"value={value}; saved={saved}"
    )
    if opened == "1" and (choice == "cancel" or error):
        checkpoint = "after-cancel" if choice == "cancel" else "after-failed-save-or-close"
        screenshot = capture_screenshot(
            f"macos-document-close-{protocol.scenario}-checkpoint-{checkpoint}-sequence-{protocol.last_sequence}.png"
        )
        print(
            f"MACOS_DOC_CLOSE_SCREENSHOT=checkpoint={checkpoint}; scenario={protocol.scenario}; "
            f"sequence={protocol.last_sequence}; {screenshot}"
        )
    return action


def require_pass(capture: BoundedCapture, process: subprocess.Popen[str], deadline: float, protocol: MarkerProtocol) -> dict[str, str]:
    event = wait_event(
        capture,
        process,
        "PASS",
        protocol.scenario,
        deadline,
        after_index=protocol.last_action_capture_index,
    )
    protocol.accept_pass(event)
    return event


def click_decision(
    process: subprocess.Popen[str],
    capture: BoundedCapture,
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float],
    deadline: float,
    protocol: MarkerProtocol,
    choice: str,
    *,
    resolved: str,
    pending: str,
    opened: str,
    dirty: str,
    value: str,
    saved: str,
    error: str = "",
) -> dict[str, str]:
    point = get_prompt_point(capture, protocol, choice)
    evidence = click_point(process, capture, pointer_driver, fixture_bounds, point, deadline, choice.title())
    return expect_action(
        capture,
        process,
        deadline,
        protocol,
        evidence,
        choice,
        protocol.generation,
        resolved=resolved,
        pending=pending,
        opened=opened,
        dirty=dirty,
        value=value,
        saved=saved,
        error=error,
    )


def run_decisions(
    process: subprocess.Popen[str],
    capture: BoundedCapture,
    pointer_driver: Path,
    fixture_bounds: tuple[float, float, float, float],
    ready: dict[str, str],
    scenario: str,
    deadline: float,
) -> dict[str, str]:
    if (ready.get("saved"), ready.get("value"), ready.get("dirty")) != ("10", "20", "1"):
        raise RuntimeError(f"fixture READY state mismatch: {ready.get('raw')!r}")
    protocol = MarkerProtocol.from_ready(ready, scenario)
    print(f"MACOS_DOC_CLOSE_READY=PASS; scenario={scenario}; host={protocol.host}; asset={protocol.asset}")
    enter_editor_value(process, capture, pointer_driver, fixture_bounds, deadline, protocol)

    prompt, _ = click_document_tab_close(
        process, capture, pointer_driver, fixture_bounds, deadline, protocol, "document tab close"
    )
    prompt_generation = protocol.generation
    print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")

    if scenario == "cancel-save":
        click_decision(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "cancel",
            resolved="1", pending="0", opened="1", dirty="1", value="42", saved="10",
        )
        prompt, _ = click_document_tab_close(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "document tab close after cancel"
        )
        prompt_generation = protocol.generation
        print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")
        click_decision(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "save",
            resolved="1", pending="0", opened="0", dirty="0", value="42", saved="42",
        )
    elif scenario == "discard":
        click_decision(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "discard",
            resolved="1", pending="0", opened="0", dirty="0", value="10", saved="10",
        )
    elif scenario == "save-retry":
        click_decision(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "save",
            resolved="0", pending="1", opened="1", dirty="1", value="42", saved="10",
            error="The%20document%20could%20not%20be%20saved%3B%20it%20remains%20open.",
        )
        prompt = wait_event(capture, process, "PROMPT", scenario, deadline, after_index=protocol.last_action_capture_index)
        protocol.accept_prompt(prompt)
        prompt_generation = protocol.generation
        print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")
        click_decision(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "save",
            resolved="1", pending="0", opened="0", dirty="0", value="42", saved="42",
        )
    elif scenario == "close-retry":
        click_decision(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "save",
            resolved="0", pending="1", opened="1", dirty="0", value="42", saved="42",
            error="The%20Debug%20fixture%20is%20configured%20to%20reject%20close.",
        )
        prompt = wait_event(capture, process, "PROMPT", scenario, deadline, after_index=protocol.last_action_capture_index)
        protocol.accept_prompt(prompt)
        prompt_generation = protocol.generation
        print(f"MACOS_DOC_CLOSE_FIXTURE_PROMPT=OBSERVED; scenario={scenario}; generation={prompt_generation}")
        click_decision(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol, "cancel",
            resolved="1", pending="0", opened="1", dirty="0", value="42", saved="42",
        )
        fixture = wait_event(capture, process, "FIXTURE", scenario, deadline)
        fixture_generation = protocol.accept_fixture(fixture)
        allow_control = wait_close_control(capture, process, protocol, deadline, kind="ALLOW_CLOSE_CONTROL")
        allow_point = parse_point(f"{allow_control.get('x', '')},{allow_control.get('y', '')}", "Allow close")
        allow_evidence = click_point(process, capture, pointer_driver, fixture_bounds, allow_point, deadline, "Allow close")
        expect_action(
            capture, process, deadline, protocol, allow_evidence, "allow-close", fixture_generation,
            resolved="1", pending="0", opened="1", dirty="0", value="42", saved="42",
        )
        closed, _ = click_document_tab_close(
            process, capture, pointer_driver, fixture_bounds, deadline, protocol,
            "document tab close after recovery", response_kind="CLOSED",
        )
        if closed.get("open") != "0" or closed.get("saved") != "42":
            raise RuntimeError(f"clean close after preflight recovery did not preserve the saved session state: {closed.get('raw')!r}")
        print(f"MACOS_DOC_CLOSE_FIXTURE_CLOSED=PASS; scenario={scenario}; saved={closed.get('saved')}")
    else:
        raise RuntimeError(f"unsupported close scenario: {scenario}")

    completion = require_pass(capture, process, deadline, protocol)
    print(f"MACOS_DOC_CLOSE_DECISION_SCENARIO=PASS; scenario={scenario}")
    return completion


def capture_main_close_diagnostics(process: subprocess.Popen[str], deadline: float) -> str:
    screenshot_state = (
        capture_screenshot("macos-document-close-main-window-close-failure.png")
        if process.poll() is None
        else f"NOT_CAPTURED_PROCESS_ALREADY_EXITED; exit_code={process.returncode}"
    )
    remaining = deadline - time.monotonic()
    if remaining <= 0.1:
        return f"window_state=DEADLINE_EXHAUSTED; screenshot={screenshot_state}"
    code, output = run_osascript(
        WINDOW_QUERY,
        str(process.pid),
        FIXTURE_TITLE,
        timeout=min(2.0, remaining),
    )
    if code == 0:
        window_state = f"window_query=PASS; snapshot={output.strip()!r}"
    else:
        category = "TCC_DENIED" if is_tcc_denial(output) else "TIMED_OUT" if code == 124 else f"ERROR({code})"
        window_state = f"window_query={category}; detail={output.strip()[-500:]!r}"
    return f"{window_state}; screenshot={screenshot_state}"


def exit_normally(
    process: subprocess.Popen[str],
    deadline: float,
    fixture_completion: dict[str, str],
) -> None:
    if fixture_completion.get("kind") != "PASS":
        raise RuntimeError(f"cannot request native window close before fixture completion: {fixture_completion!r}")
    evidence = ActionEvidence(label="native main-window AXCloseButton")
    try:
        code, output = run_osascript(
            MAIN_WINDOW_CLOSE, str(process.pid), MAIN_TITLE_FRAGMENT, timeout=bounded_timeout(deadline, 8.0)
        )
    except UnsafeChildCleanupError as error:
        evidence.dispatch_attempted = True
        evidence.api_exit_code = 124
        evidence.transport_uncertain = True
        evidence.api_result = f"AXPress timed out; helper cleanup failed: {error}"
        raise UnsafeChildCleanupError(f"native main-window close helper cleanup failed; {evidence.summary()}") from error
    evidence.dispatch_attempted = code != 127
    evidence.api_exit_code = code
    stage_match = re.search(r"stage=([A-Z_]+)", output)
    action_stage = stage_match.group(1) if stage_match else "ACTION_DISPATCH" if is_transport_timeout(code, output) else "APPLE_SCRIPT"
    if code == 0:
        evidence.api_result = output.strip() or "AXPress returned"
    elif is_tcc_denial(output):
        evidence.api_result = f"TCC_DENIED: {output.strip()}"
        evidence.semantic_outcome = "FAIL_DISPATCH_RESULT"
        raise RuntimeError(f"TCC_DENIED: Accessibility is required for normal application exit: {evidence.api_result}")
    elif code == 124 or "-1712" in output or "timed out" in output.lower():
        evidence.transport_uncertain = True
        evidence.api_result = f"AXPress transport timed out; result uncertain: {output.strip()}"
    else:
        evidence.api_result = f"AXPress failed before shutdown observation: {output.strip()}"
        evidence.semantic_outcome = "FAIL_DISPATCH_RESULT"
        diagnostics = capture_main_close_diagnostics(process, deadline)
        raise RuntimeError(
            f"could not request normal main-window close; stage={action_stage}; exit_code={code}; "
            f"{evidence.api_result}; diagnostics={diagnostics}"
        )
    print(f"MACOS_DOC_CLOSE_MAIN_WINDOW_CLOSE={evidence.api_result!r}; {evidence.summary()}")
    try:
        exit_code = process.wait(timeout=min(10.0, max(0.1, deadline - time.monotonic())))
    except subprocess.TimeoutExpired as error:
        diagnostics = capture_main_close_diagnostics(process, deadline)
        evidence.application_response = "MISSING_PROCESS_EXIT"
        evidence.semantic_outcome = "FAIL_SHUTDOWN_TIMEOUT"
        raise RuntimeError(
            f"native close request did not complete shutdown; stage=SHUTDOWN_COMPLETION; "
            f"{evidence.summary()}; diagnostics={diagnostics}"
        ) from error
    evidence.application_response = "ORDERLY_EXIT_0" if exit_code == 0 else f"EXIT_{exit_code}"
    try:
        verify_successful_exit(exit_code, fixture_completion)
    except RuntimeError as error:
        evidence.semantic_outcome = "FAIL_SHUTDOWN_RESULT"
        raise RuntimeError(f"native close process result failed; {evidence.summary()}; {error}") from error
    evidence.semantic_outcome = "PASS" if code == 0 else "PASS_TRANSPORT_UNCERTAIN_RECONCILED"
    print(f"MACOS_DOC_CLOSE_NORMAL_EXIT=PASS; pid={process.pid}; exit_code={exit_code}; {evidence.summary()}")


def remove_temporary_root(root: Path, parent: Path, expected_prefix: str) -> str | None:
    try:
        if root.is_symlink():
            return f"refusing to remove symlink temporary root: {root}"
        resolved_root = root.resolve(strict=True)
        resolved_parent = parent.resolve(strict=True)
        if resolved_root.parent != resolved_parent or not resolved_root.name.startswith(expected_prefix):
            return f"refusing to remove unexpected temporary root: {resolved_root}"
        shutil.rmtree(resolved_root)
        return None
    except OSError as error:
        return f"could not remove temporary root {root}: {error}"


def capture_scenario_failure_screenshot(
    scenario: str,
    process: subprocess.Popen[str] | None,
    capture: BoundedCapture | None,
) -> None:
    if process is None or process.poll() is not None:
        return
    if capture is not None and capture.failure_screenshot is not None:
        screenshot = capture.failure_screenshot
    else:
        screenshot = capture_screenshot(
            f"macos-document-close-{scenario}-checkpoint-unexpected-failure.png"
        )
    print(
        f"MACOS_DOC_CLOSE_FAILURE_SCREENSHOT=checkpoint=unexpected-failure; "
        f"scenario={scenario}; {screenshot}"
    )


def run_scenario(
    editor: Path,
    project: Path,
    log_directory: Path,
    pointer_driver: Path,
    scenario: str,
    timeout_seconds: int,
) -> None:
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
    unsafe_cleanup = False
    try:
        work.mkdir()
        settings.mkdir()
        deadline = time.monotonic() + timeout_seconds
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
        ready = wait_event(capture, process, "READY", scenario, deadline)
        fixture_bounds = query_fixture_window(process, deadline)
        fixture_completion = run_decisions(process, capture, pointer_driver, fixture_bounds, ready, scenario, deadline)
        exit_normally(process, deadline, fixture_completion)
    except UnsafeChildCleanupError as error:
        failure = str(error)
        unsafe_cleanup = True
        capture_scenario_failure_screenshot(scenario, process, capture)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        failure = str(error)
        capture_scenario_failure_screenshot(scenario, process, capture)
    finally:
        cleanup_failure = None
        process_stopped = process is None
        if process is not None:
            process_stopped = stop_process_group(process)
            if not process_stopped:
                cleanup_failure = f"could not stop fixture process group PID {process.pid}"
                unsafe_cleanup = True
        if capture is not None:
            capture.thread.join(timeout=5)
            if capture.thread.is_alive():
                cleanup_failure = f"editor output reader remained active after bounded process cleanup PID {process.pid}"
                unsafe_cleanup = True
            elif capture.reader_error:
                cleanup_failure = capture.reader_error
        if process_stopped:
            root_failure = remove_temporary_root(root, parent, "PulseForge Document Close Ω ") if root.exists() else None
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
        if unsafe_cleanup:
            raise UnsafeChildCleanupError(f"{scenario}: {failure}{detail}")
        raise RuntimeError(f"{scenario}: {failure}{detail}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--scenario", action="append", choices=SCENARIOS)
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
        ax_state, ax_pid, ax_detail = probe_osascript_access()
        print(
            f"MACOS_DOC_CLOSE_OSASCRIPT_PERMISSION_PREFLIGHT={ax_state}; executable=/usr/bin/osascript; "
            f"probe_pid={ax_pid}; detail={ax_detail!r}"
        )
        if ax_state in {
            "AX_UNTRUSTED",
            "APPLE_EVENTS_DENIED",
            "APPLE_EVENTS_CONSENT_REQUIRED",
            "APPLE_EVENTS_TARGET_UNAVAILABLE",
            "APPLE_EVENTS_UNSUPPORTED",
        }:
            print(
                "MACOS_DOC_CLOSE_AUTOMATION=BLOCKED; dirty-close acceptance NOT RUN; client=osascript; "
                f"reason={ax_state}; no permission prompt was requested"
            )
            return 77
        if ax_state != "OSASCRIPT_AX_APPLE_EVENTS_AUTHORIZED":
            raise RuntimeError(f"could not nonpromptingly verify osascript automation permissions: {ax_state}; {ax_detail}")
        runner_temp = os.environ.get("RUNNER_TEMP")
        temp_parent = Path(runner_temp).resolve(strict=True) if runner_temp else Path(tempfile.gettempdir()).resolve(strict=True)
        driver_root = Path(tempfile.mkdtemp(prefix="PulseForge Document Close Driver Ω ", dir=temp_parent)).resolve(strict=True)
        failures: list[str] = []
        retain_driver_root = False
        try:
            pointer_driver = compile_pointer_driver(driver_root)
            scenarios = args.scenario or SCENARIOS
            for scenario in scenarios:
                try:
                    run_scenario(editor, project, args.log_directory, pointer_driver, scenario, args.timeout_seconds)
                    print(f"MACOS_DOC_CLOSE_SCENARIO=PASS; scenario={scenario}")
                except UnsafeChildCleanupError as error:
                    failures.append(f"{scenario}: {error}")
                    retain_driver_root = True
                    print(
                        f"MACOS_DOC_CLOSE_SCENARIO=FAIL_UNSAFE_CLEANUP; scenario={scenario}; reason={error}",
                        file=sys.stderr,
                    )
                    break
                except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                    failures.append(f"{scenario}: {error}")
                    print(f"MACOS_DOC_CLOSE_SCENARIO=FAIL; scenario={scenario}; reason={error}", file=sys.stderr)
        except UnsafeChildCleanupError as error:
            failures.append(f"helper cleanup: {error}")
            retain_driver_root = True
        finally:
            if retain_driver_root:
                print(f"MACOS_DOC_CLOSE_DRIVER_ROOT=PRESERVED_AFTER_UNSAFE_CLEANUP; path={driver_root}", file=sys.stderr)
            else:
                driver_cleanup_error = remove_temporary_root(driver_root, temp_parent, "PulseForge Document Close Driver Ω ")
                if driver_cleanup_error:
                    failures.append(driver_cleanup_error)
        if failures:
            print("Dirty-close scenario failures:\n" + "\n".join(failures), file=sys.stderr)
            return 1
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
