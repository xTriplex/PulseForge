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
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
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
    set children to missing value
    try
        tell application "System Events" to set children to UI elements of container
    on error errorMessage number errorNumber
        my classifyAccessibilityError(errorNumber, errorMessage)
        return missing value
    end try
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
    if operation is "open-select" or operation is "open-cancel" then
        set primaryName to "Open"
    else
        set primaryName to "Save"
    end if
    my appendDialogChildren(candidateElement, 0)
    return "visited=" & (my uiScanVisited as text) & "; expected=Cancel," & primaryName & "; controls=[" & ¬
        (my uiScanReport) & "]"
end describeDialogControls

on appendDialogChildren(container, depth)
    if depth > 8 or my uiScanVisited >= 32 or (length of (my uiScanReport)) > 2600 then return
    set children to missing value
    try
        tell application "System Events" to set children to UI elements of container
    on error errorMessage number errorNumber
        my classifyAccessibilityError(errorNumber, errorMessage)
        return
    end try
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
    if errorNumber is not -1728 then
        error "NFD_ACCESSIBILITY_QUERY_FAILED; error=" & errorNumber & "; " & errorMessage number errorNumber
    end if
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

PANEL_READY_PROBE = '''property uiScanVisited : 0
property uiFieldMatches : 0
property uiScanReport : ""

on run argv
    set targetPID to item 1 of argv as integer
    set operation to item 2 of argv
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
        set processName to name of targetProcess as text
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
        set lastState to "frontmost process=" & foregroundState & "; no target windows observed"
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
                        return "NFD_PANEL_READY=pid=" & targetPID & "; process=" & processName & "; " & lastState & ¬
                            "; Cancel actions=" & cancelActions & "; primary actions=" & primaryActions
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
                                    return "NFD_PANEL_READY=pid=" & targetPID & "; process=" & processName & "; " & lastState & ¬
                                        "; Cancel actions=" & cancelActions & "; primary actions=" & primaryActions
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

on run argv
    set targetPID to item 1 of argv as integer
    set operation to item 2 of argv
    set directoryPath to item 3 of argv
    set selectedName to item 4 of argv
    tell application "System Events"
        set targetProcess to first process whose unix id is targetPID
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
    return "NFD_PANEL_INTERACTION=PASS; pid=" & targetPID & "; scenario=" & operation
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
                if not stop_fixture(process, output_reader):
                    return 1, "fixture process group or output reader survived cleanup after the Accessibility authorization denial"
                print("NFD_UI_AUTOMATION=UNAVAILABLE; reason=macOS TCC denied Accessibility or Apple Events; native dialog cases NOT RUN")
                return 77, probe_output
            if is_accessibility_timeout(probe_output) or probe_code == 124:
                if "process group survived SIGKILL cleanup" in probe_output:
                    return 1, f"timed-out Accessibility probe process did not terminate; output={probe_output!r}"
                last_panel_state = f"bounded Accessibility query timed out; exit_code={probe_code}; output={probe_output!r}"
                time.sleep(min(0.2, max(0.0, probe_deadline - time.monotonic())))
                continue
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after an unexpected Accessibility failure"
            return 1, (
                "Accessibility snapshot query failed without an identified TCC denial; "
                f"osascript_exit_code={probe_code}; output={probe_output!r}"
            )
        if is_tcc_denial(probe_output):
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after an Accessibility authorization denial"
            print("NFD_UI_AUTOMATION=UNAVAILABLE; reason=macOS TCC denied Accessibility or Apple Events; native dialog cases NOT RUN")
            return 77, probe_output
        if is_accessibility_timeout(probe_output):
            last_panel_state = f"bounded Accessibility query timed out; {probe_output.strip()}"
            time.sleep(min(0.2, max(0.0, probe_deadline - time.monotonic())))
            continue
        if probe_output.startswith(f"NFD_PANEL_READY=pid={process_id};"):
            print(probe_output)
            break
        if not probe_output.startswith(f"NFD_PANEL_WAIT=pid={process_id};"):
            if not stop_fixture(process, output_reader):
                return 1, "fixture process group or output reader survived cleanup after unexpected Accessibility output"
            return 1, f"Accessibility probe returned an unexpected state: {probe_output!r}"
        last_panel_state = probe_output.strip()
        time.sleep(min(0.2, max(0.0, probe_deadline - time.monotonic())))

    remaining = remaining_seconds()
    if remaining <= 0:
        if not stop_fixture(process, output_reader):
            return 1, "fixture process group or output reader survived bounded deadline cleanup"
        return 1, f"native dialog fixture exhausted its {timeout_seconds}s case bound before panel automation"
    automation_code, automation_output = apple_script(
        SELECT_SCRIPT,
        process_id,
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
