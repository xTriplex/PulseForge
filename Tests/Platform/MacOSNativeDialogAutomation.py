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
MAX_CONSOLE_BYTES = 32 * 1024
TCC_DENIALS = (
    "not allowed assistive access",
    "not authorized to send apple events",
    "not permitted to send apple events",
    "error=-1719",
    "error=-1743",
    "nfd_tcc_denial",
    "(-1719)",
    "(-1743)",
)
ACCESSIBILITY_TIMEOUTS = (
    "nfd_accessibility_query_timeout",
    "error=-1712",
    "(-1712)",
)
OBSERVATION_TOTAL_SECONDS = 35.0

SWIFT_AX_SNAPSHOT = r"""
import AppKit
import ApplicationServices
import Foundation

func readAttribute(_ element: AXUIElement, _ name: String) -> (AXError, CFTypeRef?) {
    var value: CFTypeRef?
    let error = AXUIElementCopyAttributeValue(element, name as CFString, &value)
    return (error, value)
}

func boundedText(_ value: String) -> String {
    let flattened = value.replacingOccurrences(of: "\n", with: "\\n")
        .replacingOccurrences(of: "\r", with: "\\r")
    return String(flattened.prefix(240))
}

func textAttribute(_ element: AXUIElement, _ name: String) -> String {
    let (error, value) = readAttribute(element, name)
    guard error == .success, let value else { return "AXERR_\(error.rawValue)" }
    return boundedText(String(describing: value))
}

func actionNames(_ element: AXUIElement) -> String {
    var names: CFArray?
    let error = AXUIElementCopyActionNames(element, &names)
    guard error == .success, let names else { return "AXERR_\(error.rawValue)" }
    return boundedText((names as? [String] ?? []).joined(separator: ","))
}

var visited = 0
func describe(_ element: AXUIElement, _ depth: Int) {
    if visited >= 64 || depth > 5 { return }
    visited += 1
    let role = textAttribute(element, "AXRole")
    let subrole = textAttribute(element, "AXSubrole")
    let title = textAttribute(element, "AXTitle")
    let description = textAttribute(element, "AXDescription")
    let enabled = textAttribute(element, "AXEnabled")
    let actions = actionNames(element)
    print(
        "AX_NODE depth=\(depth); role=\(role); subrole=\(subrole); title=\(title); " +
            "description=\(description); enabled=\(enabled); actions=\(actions)"
    )
    if ["AXOutline", "AXBrowser", "AXTable", "AXList", "AXCollection"].contains(role) {
        return
    }
    let (error, value) = readAttribute(element, "AXChildren")
    guard error == .success else {
        print("AX_CHILDREN_ERROR depth=\(depth); error=\(error.rawValue)")
        return
    }
    guard let value else {
        print("AX_CHILDREN_VALUE depth=\(depth); value=nil")
        return
    }
    guard let children = value as? [AXUIElement] else {
        print("AX_CHILDREN_VALUE depth=\(depth); cf_type_id=\(CFGetTypeID(value))")
        return
    }
    for child in children {
        if visited >= 64 { break }
        describe(child, depth + 1)
    }
}

let pidText = ProcessInfo.processInfo.environment["PULSEFORGE_AX_SNAPSHOT_PIDS"] ?? ""
let pids = pidText.split(separator: ",").compactMap { Int32($0) }
print("AX_API_TRUSTED=\(AXIsProcessTrusted()); pids=\(pids)")
for pid in pids {
    let application = AXUIElementCreateApplication(pid)
    let running = NSRunningApplication(processIdentifier: pid)
    let processName = boundedText(running?.localizedName ?? "unknown")
    let bundleIdentifier = running?.bundleIdentifier ?? "unknown"
    print("AX_PROCESS pid=\(pid); name=\(processName); bundle=\(bundleIdentifier)")
    let (error, value) = readAttribute(application, "AXWindows")
    guard error == .success else {
        print("AX_WINDOWS_ERROR pid=\(pid); error=\(error.rawValue)")
        continue
    }
    guard let value else {
        print("AX_WINDOWS_VALUE pid=\(pid); value=nil")
        continue
    }
    guard let windows = value as? [AXUIElement] else {
        print("AX_WINDOWS_VALUE pid=\(pid); cf_type_id=\(CFGetTypeID(value))")
        continue
    }
    print("AX_WINDOWS pid=\(pid); count=\(windows.count)")
    for (index, window) in windows.prefix(8).enumerated() {
        let role = textAttribute(window, "AXRole")
        let subrole = textAttribute(window, "AXSubrole")
        let title = textAttribute(window, "AXTitle")
        print("AX_WINDOW pid=\(pid); index=\(index + 1); role=\(role); subrole=\(subrole); title=\(title)")
        if role == "AXSheet" || subrole == "AXDialog" || title == "Open" || title == "Save" {
            describe(window, 0)
        }
    }
}
"""


class FixtureOutput:
    def __init__(self, process: subprocess.Popen[str], log_path: Path):
        self.lines: queue.Queue[str | None] = queue.Queue(maxsize=128)
        self.buffer = bytearray()
        self.log_path = log_path
        self.ready_line: str | None = None
        self.post_use_line: str | None = None
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


def is_tcc_denial(output: str) -> bool:
    lowered = output.lower()
    return any(marker in lowered for marker in TCC_DENIALS)


def is_accessibility_timeout(output: str) -> bool:
    lowered = output.lower()
    return any(marker in lowered for marker in ACCESSIBILITY_TIMEOUTS)


def classify_observation_error(exit_code: int, output: str) -> str:
    lowered = output.lower()
    if is_tcc_denial(output):
        return "tcc-authorization-denial"
    if "-25211" in lowered:
        return "accessibility-api-disabled"
    if is_accessibility_timeout(output) or exit_code == 124:
        return "timeout"
    if "-1728" in lowered:
        return "missing-attribute-or-element"
    if "-25202" in lowered:
        return "invalid-or-stale-accessibility-element"
    if "-25204" in lowered:
        return "accessibility-cannot-complete"
    if "-25205" in lowered or "-25213" in lowered:
        return "unsupported-accessibility-attribute"
    if exit_code != 0:
        return "other-accessibility-error"
    return "snapshot-returned"


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


def apple_script(
    source: str,
    *arguments: str,
    timeout: float = 30,
    cleanup_timeout: float = 5,
) -> tuple[int, str]:
    try:
        process = subprocess.Popen(
            ["/usr/bin/osascript", "-", *arguments],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
    except OSError as error:
        return 127, f"Unable to start AppleScript automation: {error}"
    try:
        output, _ = process.communicate(input=source, timeout=timeout)
        return process.returncode, output
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=cleanup_timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=cleanup_timeout)
            except subprocess.TimeoutExpired:
                return 124, output + "\nAppleScript process group survived SIGKILL cleanup\n"
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        if process.stdin:
            process.stdin.close()
        if process.stdout:
            process.stdout.close()
        return 124, output + "\nAppleScript exceeded its time bound\n"


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


def capture_waiting_observation(fixture_pid: int, scenario: str, log_path: Path) -> str:
    started = time.monotonic()
    deadline = started + OBSERVATION_TOTAL_SECONDS
    lines = [
        f"NFD_WAITING_OBSERVATION_BEGIN; scenario={scenario}; fixture_pid={fixture_pid}; "
        "no input or focus actions issued"
    ]
    screenshot_path = log_path.with_name(f"native-dialog-{scenario}-observation.png")
    try:
        capture = subprocess.run(
            ["/usr/sbin/screencapture", "-x", "-t", "png", str(screenshot_path)],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        if capture.returncode == 0 and screenshot_path.is_file() and screenshot_path.stat().st_size > 0:
            lines.append(f"NFD_WAITING_SCREENSHOT=CAPTURED; bytes={screenshot_path.stat().st_size}; path={screenshot_path}")
        else:
            lines.append(
                "NFD_WAITING_SCREENSHOT=FAILED; "
                f"exit_code={capture.returncode}; stdout={capture.stdout[:400]!r}; stderr={capture.stderr[:400]!r}"
            )
    except (OSError, subprocess.TimeoutExpired) as error:
        lines.append(f"NFD_WAITING_SCREENSHOT=FAILED; error={type(error).__name__}: {error}")

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
        code, output = apple_script(
            WAITING_PROCESS_SNAPSHOT,
            str(process_id),
            scenario,
            timeout=per_process_timeout,
            cleanup_timeout=0.1,
        )
        category = classify_observation_error(code, output)
        lines.append(
            f"NFD_OBSERVATION_AX_EXIT; pid={process_id}; category={category}; "
            f"exit_code={code}; output={output[:8000]!r}"
        )
    remaining = deadline - time.monotonic()
    swift_path = shutil.which("swift")
    if swift_path and remaining > 0.5:
        environment = os.environ.copy()
        environment["PULSEFORGE_AX_SNAPSHOT_PIDS"] = ",".join(str(process_id) for process_id, _ in candidates)
        try:
            swift_process = subprocess.Popen(
                [swift_path, "-e", SWIFT_AX_SNAPSHOT],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=environment,
                start_new_session=True,
            )
            try:
                swift_stdout, swift_stderr = swift_process.communicate(timeout=min(10.0, remaining))
                swift_exit_code = swift_process.returncode
                try:
                    os.killpg(swift_process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                lines.append(
                    f"NFD_DIRECT_AX_SNAPSHOT; exit_code={swift_exit_code}; "
                    f"stdout={swift_stdout[:12000]!r}; stderr={swift_stderr[:2000]!r}"
                )
            except subprocess.TimeoutExpired as error:
                partial_stdout = error.stdout or ""
                partial_stderr = error.stderr or ""
                if isinstance(partial_stdout, bytes):
                    partial_stdout = partial_stdout.decode("utf-8", errors="replace")
                if isinstance(partial_stderr, bytes):
                    partial_stderr = partial_stderr.decode("utf-8", errors="replace")
                try:
                    os.killpg(swift_process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    swift_process.communicate(timeout=0.2)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(swift_process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    try:
                        swift_process.communicate(timeout=0.5)
                    except subprocess.TimeoutExpired:
                        lines.append("NFD_DIRECT_AX_SNAPSHOT_CHILD_CLEANUP=FAILED; process group survived SIGKILL")
                lines.append(
                    "NFD_DIRECT_AX_SNAPSHOT=TIMEOUT; "
                    f"stdout={partial_stdout[:12000]!r}; stderr={partial_stderr[:2000]!r}"
                )
        except OSError as error:
            lines.append(f"NFD_DIRECT_AX_SNAPSHOT=FAILED; error={type(error).__name__}: {error}")
    elif not swift_path:
        lines.append("NFD_DIRECT_AX_SNAPSHOT=UNAVAILABLE; reason=swift executable was not found")
    else:
        lines.append(
            "NFD_DIRECT_AX_SNAPSHOT=UNAVAILABLE; "
            "reason=observation deadline had less than 0.5 seconds remaining"
        )
    lines.append(f"NFD_WAITING_OBSERVATION_END; elapsed_seconds={time.monotonic() - started:.2f}")
    report = "\n".join(lines)
    try:
        log_path.write_text(report + "\n", encoding="utf-8")
    except OSError as error:
        report += f"\nNFD_WAITING_OBSERVATION_LOG_WRITE_FAILED; {error}"
    print(report, flush=True)
    return report


ACCESSIBILITY_CONTROL_HANDLERS = '''on matchesDialog(candidateElement, operation)
    tell application "System Events"
        try
            set candidateRole to role of candidateElement as text
            set candidateSubrole to subrole of candidateElement as text
        on error errorMessage number errorNumber
            my classifyAccessibilityError(errorNumber, errorMessage)
            return false
        end try
        if candidateRole is not "AXSheet" and candidateSubrole is not "AXDialog" then return false
        set cancelButton to my findDialogButton(candidateElement, "Cancel")
        if cancelButton is missing value then return false
        set cancelEnabled to false
        try
            set cancelEnabled to enabled of cancelButton
        on error errorMessage number errorNumber
            my classifyAccessibilityError(errorNumber, errorMessage)
        end try
        if operation is "open-select" or operation is "open-cancel" then
            set primaryButton to my findDialogButton(candidateElement, "Open")
        else
            set primaryButton to my findDialogButton(candidateElement, "Save")
        end if
        if primaryButton is missing value then return false
        return cancelEnabled
    end tell
end matchesDialog

on dialogChildren(dialogElement)
    set childElements to missing value
    try
        tell application "System Events" to set childElements to UI elements of dialogElement
        if my uiChildSource is "" then set my uiChildSource to "System Events UI elements"
        return childElements
    on error errorMessage number errorNumber
        if errorNumber is not -1728 then
            my classifyAccessibilityError(errorNumber, errorMessage)
            error "NFD_ACCESSIBILITY_CHILD_QUERY_FAILED; source=System Events UI elements; error=" & ¬
                errorNumber & "; " & errorMessage number errorNumber
        end if
    end try
    try
        tell application "System Events" to set childElements to value of attribute "AXChildren" of dialogElement
        if childElements is missing value then
            set my uiChildSource to "AXChildren returned missing after System Events -1728"
        else if my uiChildSource is "" then
            set my uiChildSource to "AXChildren fallback after System Events -1728"
        else if my uiChildSource does not contain "AXChildren fallback" then
            set my uiChildSource to (my uiChildSource) & "; AXChildren fallback after System Events -1728"
        end if
        return childElements
    on error errorMessage number errorNumber
        my classifyAccessibilityError(errorNumber, errorMessage)
        set my uiChildSource to "children unavailable; System Events UI elements=-1728; AXChildren=" & ¬
            errorNumber & ": " & errorMessage
        return missing value
    end try
end dialogChildren

on countMatchingDialogs(candidateProcess, operation)
    set matchingCount to 0
    tell application "System Events"
        set candidateWindowCount to count of windows of candidateProcess
        repeat with windowIndex from 1 to candidateWindowCount
            set candidateWindow to window windowIndex of candidateProcess
            if my matchesDialog(candidateWindow, operation) then
                set matchingCount to matchingCount + 1
            end if
            set sheetCount to count of sheets of candidateWindow
            repeat with sheetIndex from 1 to sheetCount
                set candidateSheet to sheet sheetIndex of candidateWindow
                if my matchesDialog(candidateSheet, operation) then
                    set matchingCount to matchingCount + 1
                end if
            end repeat
        end repeat
    end tell
    return matchingCount
end countMatchingDialogs

on findDialogButton(candidateElement, requestedName)
    set my uiScanVisited to 0
    return my findDialogElement(candidateElement, "AXButton", requestedName, 0, 0)
end findDialogButton

on findDialogTextField(candidateElement, requestedIndex)
    set my uiScanVisited to 0
    set my uiFieldMatches to 0
    return my findDialogElement(candidateElement, "AXTextField", "", requestedIndex, 0)
end findDialogTextField

on findDialogElement(container, requestedRole, requestedName, requestedIndex, depth)
    if depth > 8 or my uiScanVisited >= 96 then return missing value
    set children to my dialogChildren(container)
    if children is missing value then return missing value
    set childCount to count of children
    if requestedRole is "AXTextField" then
        repeat with childIndex from 1 to childCount
            if my uiScanVisited >= 96 then exit repeat
            set foundElement to my findDialogChild(item childIndex of children, requestedRole, requestedName, requestedIndex, depth)
            if foundElement is not missing value then return foundElement
        end repeat
    else
        repeat with childIndex from childCount to 1 by -1
            if my uiScanVisited >= 96 then exit repeat
            set foundElement to my findDialogChild(item childIndex of children, requestedRole, requestedName, requestedIndex, depth)
            if foundElement is not missing value then return foundElement
        end repeat
    end if
    return missing value
end findDialogElement

on findDialogChild(childElement, requestedRole, requestedName, requestedIndex, depth)
    if my uiScanVisited >= 96 then return missing value
    set my uiScanVisited to (my uiScanVisited) + 1
    set childRole to "?"
    try
        tell application "System Events" to set childRole to role of childElement as text
    on error errorMessage number errorNumber
        my classifyAccessibilityError(errorNumber, errorMessage)
    end try
    if childRole is requestedRole and requestedRole is "AXButton" then
        set childName to ""
        set childDescription to ""
        tell application "System Events"
            try
                set childName to name of childElement as text
            on error errorMessage number errorNumber
                my classifyAccessibilityError(errorNumber, errorMessage)
            end try
            try
                set childDescription to description of childElement as text
            on error errorMessage number errorNumber
                my classifyAccessibilityError(errorNumber, errorMessage)
            end try
        end tell
        if childName is requestedName or childDescription is requestedName then return childElement
    else if childRole is requestedRole and requestedRole is "AXTextField" then
        set my uiFieldMatches to (my uiFieldMatches) + 1
        if my uiFieldMatches is requestedIndex then return childElement
    end if
    if childRole is "AXGroup" or childRole is "AXToolbar" or childRole is "AXSplitGroup" or ¬
        childRole is "AXRadioGroup" or childRole is "AXTabGroup" or childRole is "AXSheet" or childRole is "AXWindow" then
        return my findDialogElement(childElement, requestedRole, requestedName, requestedIndex, depth + 1)
    end if
    return missing value
end findDialogChild

on describeDialogControls(candidateElement, operation)
    set my uiScanVisited to 0
    set my uiScanReport to ""
    set my uiChildSource to ""
    if operation is "open-select" or operation is "open-cancel" then
        set primaryName to "Open"
    else
        set primaryName to "Save"
    end if
    my appendDialogChildren(candidateElement, 0)
    return "visited=" & (my uiScanVisited as text) & "; child-source=" & my uiChildSource & ¬
        "; expected=Cancel," & primaryName & "; controls=[" & (my uiScanReport) & "]"
end describeDialogControls

on appendDialogChildren(container, depth)
    if depth > 8 or my uiScanVisited >= 32 or (length of (my uiScanReport)) > 2600 then return
    set children to my dialogChildren(container)
    if children is missing value then return
    set childCount to count of children
    repeat with childIndex from childCount to 1 by -1
        if my uiScanVisited >= 32 or (length of (my uiScanReport)) > 2600 then exit repeat
        set childElement to item childIndex of children
        set my uiScanVisited to (my uiScanVisited) + 1
        set childRole to "?"
        set childName to ""
        set childSubrole to "?"
        tell application "System Events"
            try
                set childRole to role of childElement as text
            on error errorMessage number errorNumber
                my classifyAccessibilityError(errorNumber, errorMessage)
            end try
            try
                set childSubrole to subrole of childElement as text
            on error errorMessage number errorNumber
                my classifyAccessibilityError(errorNumber, errorMessage)
            end try
            if childRole is "AXButton" or childRole is "AXTextField" then
                try
                    set childName to name of childElement as text
                on error errorMessage number errorNumber
                    my classifyAccessibilityError(errorNumber, errorMessage)
                end try
            end if
        end tell
        set itemState to childRole & "/" & childSubrole
        if childName is not "" then set itemState to itemState & ": name='" & childName & "'"
        if childRole is "AXButton" then
            tell application "System Events"
                try
                    set childDescription to description of childElement as text
                    set itemState to itemState & " description='" & childDescription & "'"
                on error errorMessage number errorNumber
                    my classifyAccessibilityError(errorNumber, errorMessage)
                end try
                try
                    set itemState to itemState & " enabled=" & ((enabled of childElement) as text)
                on error errorMessage number errorNumber
                    my classifyAccessibilityError(errorNumber, errorMessage)
                end try
                try
                    set itemState to itemState & " actions=" & (actions of childElement as text)
                on error errorMessage number errorNumber
                    my classifyAccessibilityError(errorNumber, errorMessage)
                end try
            end tell
        end if
        if my uiScanReport is not "" then set my uiScanReport to (my uiScanReport) & "; "
        set my uiScanReport to (my uiScanReport) & (depth as text) & ":" & itemState
        if childRole is "AXGroup" or childRole is "AXToolbar" or childRole is "AXSplitGroup" or ¬
            childRole is "AXRadioGroup" or childRole is "AXTabGroup" or childRole is "AXSheet" or childRole is "AXWindow" then
            my appendDialogChildren(childElement, depth + 1)
        end if
    end repeat
end appendDialogChildren

on classifyAccessibilityError(errorNumber, errorMessage)
    if errorNumber is -1712 then
        error "NFD_ACCESSIBILITY_QUERY_TIMEOUT; " & errorMessage number -1712
    end if
    if errorNumber is -1719 or errorNumber is -1743 then
        error "NFD_TCC_DENIAL; error=" & errorNumber & "; " & errorMessage number errorNumber
    end if
    if errorNumber is -1728 then
        set missingState to "AXError=-1728 (missing attribute or element): " & errorMessage
        if my uiScanReport is "" then
            set my uiScanReport to missingState
        else
            set my uiScanReport to (my uiScanReport) & "; " & missingState
        end if
        return
    end if
    error "NFD_ACCESSIBILITY_QUERY_FAILED; error=" & errorNumber & "; " & errorMessage number errorNumber
end classifyAccessibilityError

on pressDialogButton(buttonElement, buttonName)
    tell application "System Events"
        set availableActions to "unavailable"
        try
            set availableActions to actions of buttonElement as text
        on error errorMessage number errorNumber
            my classifyAccessibilityError(errorNumber, errorMessage)
            error "NFD_ACCESSIBILITY_ACTION_QUERY_FAILED; button=" & buttonName & "; " & errorMessage number errorNumber
        end try
        if availableActions does not contain "AXPress" then
            error "NFD_ACCESSIBILITY_ACTION_MISSING; button=" & buttonName & "; actions=" & availableActions
        end if
        try
            perform action "AXPress" of buttonElement
        on error errorMessage number errorNumber
            my classifyAccessibilityError(errorNumber, errorMessage)
            error "NFD_ACCESSIBILITY_PRESS_FAILED; button=" & buttonName & "; " & errorMessage number errorNumber
        end try
    end tell
end pressDialogButton
'''

WAITING_PROCESS_SNAPSHOT = '''property uiScanVisited : 0
property uiFieldMatches : 0
property uiScanReport : ""
property uiChildSource : ""

on run argv
    set targetPID to item 1 of argv as integer
    set operation to item 2 of argv
    tell application "System Events"
        try
            set targetProcess to first application process whose unix id is targetPID
            set processName to name of targetProcess as text
            set processBundle to bundle identifier of targetProcess as text
            set processVisible to visible of targetProcess as text
            set windowCount to count of windows of targetProcess
        on error errorMessage number errorNumber
            return "NFD_OBSERVATION_AX_ERROR; pid=" & targetPID & "; stage=process-or-windows; error=" & ¬
                errorNumber & "; " & errorMessage
        end try
        set snapshot to "NFD_OBSERVATION_PROCESS; pid=" & targetPID & "; name=" & processName & ¬
            "; bundle=" & processBundle & "; visible=" & processVisible & "; windows=" & windowCount
        repeat with windowIndex from 1 to windowCount
            try
                set candidateWindow to window windowIndex of targetProcess
                set windowName to name of candidateWindow as text
                set windowRole to role of candidateWindow as text
                set windowSubrole to subrole of candidateWindow as text
                set controlState to my describeDialogControls(candidateWindow, operation)
                set sheetCount to count of sheets of candidateWindow
                set snapshot to snapshot & " | WINDOW index=" & windowIndex & "; name=" & windowName & ¬
                    "; role=" & windowRole & "; subrole=" & windowSubrole & "; " & controlState & "; sheets=" & sheetCount
                repeat with sheetIndex from 1 to sheetCount
                    set candidateSheet to sheet sheetIndex of candidateWindow
                    set sheetName to name of candidateSheet as text
                    set sheetRole to role of candidateSheet as text
                    set sheetSubrole to subrole of candidateSheet as text
                    set controlState to my describeDialogControls(candidateSheet, operation)
                    set snapshot to snapshot & " | SHEET index=" & sheetIndex & "; name=" & sheetName & ¬
                        "; role=" & sheetRole & "; subrole=" & sheetSubrole & "; " & controlState
                end repeat
            on error errorMessage number errorNumber
                set snapshot to snapshot & " | WINDOW index=" & windowIndex & "; query-error=" & ¬
                    errorNumber & "; " & errorMessage
            end try
        end repeat
        return snapshot
    end tell
end run
''' + ACCESSIBILITY_CONTROL_HANDLERS


PANEL_READY_PROBE = '''property uiScanVisited : 0
property uiFieldMatches : 0
property uiScanReport : ""
property uiChildSource : ""

on run argv
    set targetPID to item 1 of argv as integer
    set operation to item 2 of argv
    tell application "System Events"
        set hostProcess to first application process whose unix id is targetPID
        set hostName to name of hostProcess as text
        set targetProcess to hostProcess
        set panelProcessID to targetPID
        set processName to hostName
        set processBundleID to "host application process"
        set panelServiceCount to 0
        set panelServiceProcesses to {}
        set discoveryState to ""
        try
            with timeout of 2 seconds
                set panelServiceProcesses to every application process whose ¬
                    bundle identifier is "com.apple.appkit.xpc.openAndSavePanelService"
            end timeout
            set panelServiceCount to count of panelServiceProcesses
        on error errorMessage number errorNumber
            if errorNumber is -1712 then
                error "NFD_ACCESSIBILITY_QUERY_TIMEOUT; query=open-save-panel-service-process; " & ¬
                    "error=" & errorNumber & "; " & errorMessage number -1712
            end if
            if errorNumber is -1719 or errorNumber is -1743 then
                error "NFD_TCC_DENIAL; query=open-save-panel-service-process; error=" & ¬
                    errorNumber & "; " & errorMessage number errorNumber
            end if
            set discoveryState to "panel-service lookup error=" & errorNumber & "; " & errorMessage
        end try
        set candidateProcesses to {hostProcess}
        set candidateProcessIDs to {targetPID}
        repeat with serviceIndex from 1 to panelServiceCount
            set serviceProcess to item serviceIndex of panelServiceProcesses
            try
                with timeout of 1 second
                    set serviceProcessID to unix id of serviceProcess as integer
                end timeout
                if serviceProcessID is not in candidateProcessIDs then
                    set end of candidateProcessIDs to serviceProcessID
                    set end of candidateProcesses to serviceProcess
                end if
            on error errorMessage number errorNumber
                if errorNumber is -1712 then
                    error "NFD_ACCESSIBILITY_QUERY_TIMEOUT; query=panel-service-process-pid; " & ¬
                        "error=" & errorNumber & "; " & errorMessage number -1712
                end if
                if errorNumber is -1719 or errorNumber is -1743 then
                    error "NFD_TCC_DENIAL; query=panel-service-process-pid; error=" & ¬
                        errorNumber & "; " & errorMessage number errorNumber
                end if
                error "NFD_ACCESSIBILITY_QUERY_FAILED; query=panel-service-process-pid; error=" & ¬
                    errorNumber & "; " & errorMessage number errorNumber
            end try
        end repeat
        set candidateProcessCount to count of candidateProcesses
        set processIDsText to candidateProcessIDs as text
        set candidateSummary to "fixture pid=" & targetPID & "; candidate pids=" & processIDsText
        set fixtureMatchCount to 0
        try
            with timeout of 2 seconds
                set fixtureMatchCount to my countMatchingDialogs(hostProcess, operation)
            end timeout
        on error errorMessage number errorNumber
            my classifyAccessibilityError(errorNumber, errorMessage)
            error "NFD_ACCESSIBILITY_QUERY_FAILED; query=fixture-dialog-candidates; pid=" & ¬
                targetPID & "; error=" & errorNumber & "; " & errorMessage number errorNumber
        end try
        if fixtureMatchCount > 1 then
            return "NFD_PANEL_AMBIGUOUS=pid=" & targetPID & "; fixture matching windows/sheets=" & fixtureMatchCount
        end if
        set panelOwnerFound to false
        if fixtureMatchCount is 1 then
            set targetProcess to hostProcess
            set panelProcessID to targetPID
            set processName to hostName
            set processBundleID to "host application process"
            set panelOwnerFound to true
        else
            set serviceMatchCount to 0
            set serviceTargetProcess to missing value
            set serviceTargetProcessID to 0
            set serviceTargetName to ""
            set serviceTargetBundleID to ""
            if candidateProcessCount > 1 then
                repeat with processIndex from 2 to candidateProcessCount
                    set candidateProcess to item processIndex of candidateProcesses
                    try
                        with timeout of 2 seconds
                            set candidateProcessID to unix id of candidateProcess as integer
                            set candidateProcessName to name of candidateProcess as text
                            set candidateBundleID to bundle identifier of candidateProcess as text
                            set candidateMatches to my countMatchingDialogs(candidateProcess, operation)
                        end timeout
                    on error errorMessage number errorNumber
                        my classifyAccessibilityError(errorNumber, errorMessage)
                        error "NFD_ACCESSIBILITY_QUERY_FAILED; query=panel-service-dialog-candidates; " & ¬
                            "error=" & errorNumber & "; " & errorMessage number errorNumber
                    end try
                    if candidateMatches > 0 then
                        set serviceMatchCount to serviceMatchCount + candidateMatches
                        if serviceTargetProcess is missing value then
                            set serviceTargetProcess to candidateProcess
                            set serviceTargetProcessID to candidateProcessID
                            set serviceTargetName to candidateProcessName
                            set serviceTargetBundleID to candidateBundleID
                        end if
                    end if
                end repeat
            end if
            set candidateSummary to candidateSummary & "; service matching windows/sheets=" & serviceMatchCount
            if serviceMatchCount > 1 then
                return "NFD_PANEL_AMBIGUOUS=pid=" & targetPID & "; " & candidateSummary
            end if
            if serviceMatchCount is 1 then
                set targetProcess to serviceTargetProcess
                set panelProcessID to serviceTargetProcessID
                set processName to serviceTargetName
                set processBundleID to serviceTargetBundleID
                set panelOwnerFound to true
            end if
        end if
        set foregroundState to "unavailable"
        try
            set foregroundProcess to first process whose frontmost is true
            set foregroundName to name of foregroundProcess as text
            set foregroundPID to unix id of foregroundProcess as text
            set foregroundState to foregroundName & " (pid=" & foregroundPID & ")"
            if foregroundPID is not (targetPID as text) then
                try
                    with timeout of 2 seconds
                        set foregroundWindow to window 1 of foregroundProcess
                        set foregroundWindowName to name of foregroundWindow as text
                        set foregroundWindowRole to role of foregroundWindow as text
                        set foregroundWindowSubrole to subrole of foregroundWindow as text
                    end timeout
                    set foregroundState to foregroundState & "; frontmost window='" & foregroundWindowName & ¬
                        "' | " & foregroundWindowRole & " | " & foregroundWindowSubrole
                on error errorMessage number errorNumber
                    set foregroundState to foregroundState & "; frontmost window query error=" & errorNumber & ": " & errorMessage
                end try
            end if
        on error errorMessage number errorNumber
            set foregroundState to "query error=" & errorNumber & ": " & errorMessage
        end try
        set lastState to "frontmost process=" & foregroundState & "; panel owner='" & processName & ¬
            "' (pid=" & panelProcessID & "; bundle=" & processBundleID & "); selected controls=" & ¬
            panelOwnerFound & "; process candidates=" & candidateProcessCount & ¬
            "; panel-service count=" & panelServiceCount & "; discovery=" & discoveryState & ¬
            "; candidate state=" & candidateSummary
            try
                with timeout of 2 seconds
                    set windowCount to count of windows of targetProcess
                end timeout
            on error errorMessage number errorNumber
                error "NFD_ACCESSIBILITY_QUERY_FAILED; pid=" & targetPID & "; process=" & processName & ¬
                    "; query=window-count; error=" & errorNumber & "; " & errorMessage
            end try
            repeat with windowIndex from 1 to windowCount
                set candidateWindow to missing value
                try
                    with timeout of 2 seconds
                        set candidateWindow to window windowIndex of targetProcess
                        set windowName to name of candidateWindow as text
                        set windowRole to role of candidateWindow as text
                        set windowSubrole to subrole of candidateWindow as text
                    end timeout
                    set candidateState to "window " & windowIndex & " '" & windowName & "' | " & windowRole & ¬
                        " | " & windowSubrole & " | checking expected controls"
                    with timeout of 2 seconds
                        set panelReady to my matchesDialog(candidateWindow, operation)
                    end timeout
                    set candidateState to candidateState & "; expected buttons present=" & panelReady
                    if not panelReady then
                        if windowRole is "AXSheet" or windowSubrole is "AXDialog" then
                            with timeout of 2 seconds
                                set controlState to my describeDialogControls(candidateWindow, operation)
                            end timeout
                        else
                            set controlState to "descendant scan skipped for non-dialog window"
                        end if
                        set candidateState to candidateState & "; " & controlState
                    end if
                    set lastState to lastState & " | " & candidateState
                    if panelReady then
                        set cancelActions to "unavailable"
                        set primaryActions to "unavailable"
                        try
                            with timeout of 2 seconds
                                set cancelButton to my findDialogButton(candidateWindow, "Cancel")
                                set cancelActions to actions of cancelButton as text
                                if operation is "open-select" or operation is "open-cancel" then
                                    set primaryButton to my findDialogButton(candidateWindow, "Open")
                                else
                                    set primaryButton to my findDialogButton(candidateWindow, "Save")
                                end if
                                set primaryActions to actions of primaryButton as text
                            end timeout
                        end try
                        return "NFD_PANEL_READY=pid=" & targetPID & "; panel_pid=" & panelProcessID & ¬
                            "; panel_process=" & processName & "; process=" & processName & "; " & lastState & ¬
                            "; child-source=" & my uiChildSource & "; Cancel actions=" & cancelActions & ¬
                            "; primary actions=" & primaryActions
                    end if
                on error errorMessage number errorNumber
                    if errorNumber is -1712 then
                        error "NFD_ACCESSIBILITY_QUERY_TIMEOUT; pid=" & targetPID & "; process=" & processName & ¬
                            "; query=window-" & windowIndex & "; state=" & lastState & ¬
                            "; error=" & errorNumber & "; " & errorMessage
                    end if
                    if errorMessage contains "NFD_TCC_DENIAL" then error errorMessage number errorNumber
                    if errorNumber is -1719 or errorNumber is -1743 then
                        error "NFD_TCC_DENIAL; pid=" & targetPID & "; process=" & processName & ¬
                            "; query=window-" & windowIndex & "; error=" & errorNumber & "; " & errorMessage number errorNumber
                    end if
                    set lastState to lastState & " | window " & windowIndex & " query error=" & errorNumber & "; " & errorMessage
                end try
                if candidateWindow is not missing value then
                    try
                        with timeout of 2 seconds
                            set sheetCount to count of sheets of candidateWindow
                        end timeout
                        set lastState to lastState & "; window " & windowIndex & " sheets=" & sheetCount
                        repeat with sheetIndex from 1 to sheetCount
                            try
                                with timeout of 2 seconds
                                    set candidateSheet to sheet sheetIndex of candidateWindow
                                    set sheetName to name of candidateSheet as text
                                    set sheetRole to role of candidateSheet as text
                                    set sheetSubrole to subrole of candidateSheet as text
                                end timeout
                                set candidateState to "sheet " & sheetIndex & " '" & sheetName & "' | " & sheetRole & ¬
                                    " | " & sheetSubrole & " | checking expected controls"
                                with timeout of 2 seconds
                                    set panelReady to my matchesDialog(candidateSheet, operation)
                                end timeout
                                set candidateState to candidateState & "; expected buttons present=" & panelReady
                                if not panelReady then
                                    with timeout of 2 seconds
                                        set controlState to my describeDialogControls(candidateSheet, operation)
                                    end timeout
                                    set candidateState to candidateState & "; " & controlState
                                end if
                                set lastState to lastState & " | " & candidateState
                                if panelReady then
                                    set cancelActions to "unavailable"
                                    set primaryActions to "unavailable"
                                    try
                                        with timeout of 2 seconds
                                            set cancelButton to my findDialogButton(candidateSheet, "Cancel")
                                            set cancelActions to actions of cancelButton as text
                                            if operation is "open-select" or operation is "open-cancel" then
                                                set primaryButton to my findDialogButton(candidateSheet, "Open")
                                            else
                                                set primaryButton to my findDialogButton(candidateSheet, "Save")
                                            end if
                                            set primaryActions to actions of primaryButton as text
                                        end timeout
                                    end try
                                    return "NFD_PANEL_READY=pid=" & targetPID & "; panel_pid=" & panelProcessID & ¬
                                        "; panel_process=" & processName & "; process=" & processName & "; " & lastState & ¬
                                        "; child-source=" & my uiChildSource & "; Cancel actions=" & cancelActions & ¬
                                        "; primary actions=" & primaryActions
                                end if
                            on error errorMessage number errorNumber
                                if errorNumber is -1712 then
                                    error "NFD_ACCESSIBILITY_QUERY_TIMEOUT; pid=" & targetPID & "; process=" & processName & ¬
                                        "; query=window-" & windowIndex & "/sheet-" & sheetIndex & ¬
                                        "; state=" & lastState & "; error=" & errorNumber & "; " & errorMessage ¬
                                        number -1712
                                end if
                                if errorMessage contains "NFD_TCC_DENIAL" then error errorMessage number errorNumber
                                if errorNumber is -1719 or errorNumber is -1743 then
                                    error "NFD_TCC_DENIAL; pid=" & targetPID & "; process=" & processName & ¬
                                        "; query=window-" & windowIndex & "/sheet-" & sheetIndex & ¬
                                        "; error=" & errorNumber & "; " & errorMessage number errorNumber
                                end if
                                set lastState to lastState & " | sheet " & sheetIndex & " query error=" & errorNumber & "; " & errorMessage
                            end try
                        end repeat
                    on error errorMessage number errorNumber
                        if errorNumber is -1712 then
                            if errorMessage contains "NFD_ACCESSIBILITY_QUERY_TIMEOUT" then error errorMessage number -1712
                            error "NFD_ACCESSIBILITY_QUERY_TIMEOUT; pid=" & targetPID & "; process=" & processName & ¬
                                "; query=window-" & windowIndex & "/sheets; state=" & lastState & ¬
                                "; error=" & errorNumber & "; " & errorMessage number -1712
                        end if
                        if errorMessage contains "NFD_TCC_DENIAL" then error errorMessage number errorNumber
                        if errorNumber is -1719 or errorNumber is -1743 then
                            error "NFD_TCC_DENIAL; pid=" & targetPID & "; process=" & processName & ¬
                                "; query=window-" & windowIndex & "/sheets; error=" & errorNumber & "; " & errorMessage ¬
                                number errorNumber
                        end if
                        set lastState to lastState & " | window " & windowIndex & " sheets query error=" & errorNumber & "; " & errorMessage
                    end try
                end if
            end repeat
        return "NFD_PANEL_WAIT=pid=" & targetPID & "; process=" & processName & "; operation=" & operation & ¬
            "; last role/subrole/expected-control state=" & lastState
    end tell
end run
''' + ACCESSIBILITY_CONTROL_HANDLERS

SELECT_SCRIPT = '''property uiScanVisited : 0
property uiFieldMatches : 0
property uiScanReport : ""
property uiChildSource : ""

on run argv
    set targetPID to item 1 of argv as integer
    set operation to item 2 of argv
    set directoryPath to item 3 of argv
    set selectedName to item 4 of argv
    tell application "System Events"
        set targetProcess to first application process whose unix id is targetPID
        set frontmost of targetProcess to true
        if not (frontmost of targetProcess) then error "Native panel process did not become frontmost"
        with timeout of 2 seconds
            set currentPanelMatches to my countMatchingDialogs(targetProcess, operation)
        end timeout
        if currentPanelMatches is not 1 then
            error "Native panel target changed or became ambiguous before interaction; matching windows/sheets=" & ¬
                currentPanelMatches
        end if
        set dialogPanel to missing value
        repeat 100 times
            with timeout of 2 seconds
                set candidateWindowCount to count of windows of targetProcess
            end timeout
            repeat with windowIndex from 1 to candidateWindowCount
                set candidateWindow to missing value
                try
                    with timeout of 2 seconds
                        set candidateWindow to window windowIndex of targetProcess
                        if my matchesDialog(candidateWindow, operation) then
                            set dialogPanel to candidateWindow
                        end if
                    end timeout
                end try
                if dialogPanel is not missing value then exit repeat
                if candidateWindow is not missing value then
                    try
                        with timeout of 2 seconds
                            set sheetCount to count of sheets of candidateWindow
                        end timeout
                        repeat with sheetIndex from 1 to sheetCount
                            try
                                with timeout of 2 seconds
                                    set candidateSheet to sheet sheetIndex of candidateWindow
                                    if my matchesDialog(candidateSheet, operation) then
                                        set dialogPanel to candidateSheet
                                    end if
                                end timeout
                            end try
                            if dialogPanel is not missing value then exit repeat
                        end repeat
                    end try
                end if
                if dialogPanel is not missing value then exit repeat
            end repeat
            if dialogPanel is not missing value then exit repeat
            delay 0.1
        end repeat
        if dialogPanel is missing value then error "The observable native file panel disappeared before interaction"
        if operation is "open-select" then
            keystroke "g" using {command down, shift down}
            set goSheet to missing value
            set goSheetParent to missing value
            repeat 100 times
                repeat with candidateWindow in windows of targetProcess
                    try
                        if (count of sheets of candidateWindow) > 0 then
                            set possibleSheet to sheet 1 of candidateWindow
                            if my findDialogTextField(possibleSheet, 1) is not missing value then
                                set goSheet to possibleSheet
                                set goSheetParent to candidateWindow
                                exit repeat
                            end if
                        end if
                    end try
                end repeat
                if goSheet is not missing value then exit repeat
                delay 0.1
            end repeat
            if goSheet is missing value then error "Go to Folder sheet did not expose a text field after Cmd-Shift-G"
            set goTextField to my findDialogTextField(goSheet, 1)
            set value of goTextField to directoryPath
            if (value of goTextField as text) is not directoryPath then
                error "Go to Folder field did not retain the requested Unicode directory path"
            end if
            set goButton to my findDialogButton(goSheet, "Go")
            if goButton is missing value then error "Go to Folder confirmation button was not exposed"
            if not enabled of goButton then error "Go to Folder confirmation remained disabled"
            my pressDialogButton(goButton, "Go")
            set goSheetClosed to false
            repeat 100 times
                try
                    if (count of sheets of goSheetParent) is 0 then
                        set goSheetClosed to true
                        exit repeat
                    end if
                end try
                delay 0.1
            end repeat
            if not goSheetClosed then error "Go to Folder sheet remained visible after confirmation"
            keystroke selectedName
            repeat 100 times
                try
                    set openButton to my findDialogButton(dialogPanel, "Open")
                    if openButton is not missing value and enabled of openButton then exit repeat
                end try
                delay 0.1
            end repeat
            set openButton to my findDialogButton(dialogPanel, "Open")
            if openButton is missing value then error "Open button disappeared after selecting the Unicode filename"
            if not enabled of openButton then error "Open remained disabled after selecting the Unicode filename"
            my pressDialogButton(openButton, "Open")
        else if operation is "open-cancel" or operation is "save-cancel" then
            set cancelButton to my findDialogButton(dialogPanel, "Cancel")
            if cancelButton is missing value then error "Cancel button disappeared before the cancellation action"
            my pressDialogButton(cancelButton, "Cancel")
        else if operation is "save-select" then
            set savePanel to dialogPanel
            set saveTextField to my findDialogTextField(savePanel, 1)
            if saveTextField is missing value then error "Save panel did not expose its filename field"
            set value of saveTextField to selectedName
            if (value of saveTextField as text) is not selectedName then
                error "Save filename field did not retain the requested Unicode filename"
            end if
            repeat 100 times
                set saveButton to my findDialogButton(savePanel, "Save")
                if saveButton is not missing value and enabled of saveButton then exit repeat
                delay 0.1
            end repeat
            if saveButton is missing value then error "Save button disappeared before confirmation"
            if not enabled of saveButton then error "Save remained disabled after filename entry"
            my pressDialogButton(saveButton, "Save")
        end if
    end tell
    return "NFD_PANEL_INTERACTION=PASS; pid=" & targetPID & "; scenario=" & operation & ¬
        "; child-source=" & my uiChildSource
end run
''' + ACCESSIBILITY_CONTROL_HANDLERS


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
    while True:
        probe_remaining = max(0.0, probe_deadline - time.monotonic())
        if probe_remaining <= 0:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after the panel readiness deadline"
            return 1, f"native panel did not become Accessibility-ready; last_state={last_panel_state!r}"
        if output_reader.failure_lines:
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after a panel-readiness failure"
            return process.returncode or 1, "; ".join(output_reader.failure_lines)
        if process.poll() is not None:
            output_reader.thread.join(timeout=5)
            return process.returncode or 1, "native-dialog fixture exited before its panel became Accessibility-ready"
        probe_code, probe_output = apple_script(
            PANEL_READY_PROBE,
            process_id,
            scenario,
            timeout=min(5, max(1, int(probe_remaining))),
        )
        if probe_code != 0:
            if is_tcc_denial(probe_output):
                observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
                capture_waiting_observation(process.pid, scenario, observation_log)
                if not stop_fixture(process, output_reader):
                    return 1, "fixture process group or output reader survived cleanup after the Accessibility authorization denial"
                print("NFD_UI_AUTOMATION=UNAVAILABLE; reason=macOS TCC denied Accessibility or Apple Events; native dialog cases NOT RUN")
                return 77, probe_output
            if is_accessibility_timeout(probe_output) or probe_code == 124:
                if "process group survived SIGKILL cleanup" in probe_output:
                    return 1, f"timed-out Accessibility probe process did not terminate; output={probe_output!r}"
                last_panel_state = f"bounded Accessibility query timed out; exit_code={probe_code}; output={probe_output!r}"
                observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
                capture_waiting_observation(process.pid, scenario, observation_log)
                if not stop_fixture(process, output_reader):
                    return 1, "fixture process group or output reader survived cleanup after waiting-state observation"
                return 1, f"native panel readiness probe timed out; read-only waiting-state observation captured; last_state={last_panel_state!r}"
            if probe_code != 0:
                observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
                capture_waiting_observation(process.pid, scenario, observation_log)
                if not stop_fixture(process, output_reader):
                    return 1, "fixture process group or output reader survived cleanup after an unexpected Accessibility failure"
                return 1, (
                    "Accessibility snapshot query failed without an identified TCC denial; "
                    f"osascript_exit_code={probe_code}; output={probe_output!r}"
                )
        if is_tcc_denial(probe_output):
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            capture_waiting_observation(process.pid, scenario, observation_log)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after an Accessibility authorization denial"
            print("NFD_UI_AUTOMATION=UNAVAILABLE; reason=macOS TCC denied Accessibility or Apple Events; native dialog cases NOT RUN")
            return 77, probe_output
        if is_accessibility_timeout(probe_output):
            last_panel_state = f"bounded Accessibility query timed out; {probe_output.strip()}"
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            capture_waiting_observation(process.pid, scenario, observation_log)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after waiting-state observation"
            return 1, f"native panel readiness reported an Accessibility timeout; read-only observation captured; last_state={last_panel_state!r}"
        if probe_output.startswith(f"NFD_PANEL_READY=pid={process_id};"):
            print(probe_output)
            panel_process_id = next(
                (field.partition("=")[2] for field in probe_output.split("; ") if field.startswith("panel_pid=")),
                "",
            )
            if not panel_process_id.isdecimal() or int(panel_process_id) <= 0:
                if not stop_fixture(process, output_reader):
                    return 1, "fixture process group or output reader survived invalid panel-process cleanup"
                return 1, f"Accessibility readiness did not identify the native panel owner PID: {probe_output!r}"
            break
        if not probe_output.startswith(f"NFD_PANEL_WAIT=pid={process_id};"):
            observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
            capture_waiting_observation(process.pid, scenario, observation_log)
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after unexpected Accessibility output"
            return 1, f"Accessibility probe returned an unexpected state: {probe_output!r}"
        last_panel_state = probe_output.strip()
        time.sleep(0.5)
        observation_log = log_path.with_name(f"native-dialog-{scenario}-observation.log")
        capture_waiting_observation(process.pid, scenario, observation_log)
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived cleanup after waiting-state observation"
        return 1, f"native panel did not meet readiness on the first snapshot; read-only observation captured; last_state={last_panel_state!r}"

    remaining = remaining_seconds()
    if remaining <= 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived bounded deadline cleanup"
        return 1, f"native dialog fixture exhausted its {timeout_seconds}s case bound before panel automation"
    automation_code, automation_output = apple_script(
        SELECT_SCRIPT,
        panel_process_id,
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
        return 1, f"native panel automation failed; osascript_exit_code={automation_code}; output={automation_output!r}"
    print(automation_output)

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
