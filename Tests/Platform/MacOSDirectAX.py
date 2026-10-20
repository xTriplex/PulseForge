"""Bounded Accessibility operations performed by the Swift interpreter client itself."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess


SWIFT_AX_OPERATION = r"""
import AppKit
import ApplicationServices
import CoreGraphics
import Darwin
import Foundation

func value(_ element: AXUIElement, _ attribute: String) -> CFTypeRef? {
    var result: CFTypeRef?
    let error = AXUIElementCopyAttributeValue(element, attribute as CFString, &result)
    guard error == .success else { return nil }
    return result
}

func string(_ element: AXUIElement, _ attribute: String) -> String? {
    guard let result = value(element, attribute) else { return nil }
    return result as? String
}

func described(_ element: AXUIElement, _ attribute: String) -> String {
    guard let result = value(element, attribute) else { return "<unavailable>" }
    return String(describing: result)
}

let env = ProcessInfo.processInfo.environment
guard let pidText = env["PULSEFORGE_AX_TARGET_PID"], let targetPID = pid_t(pidText), targetPID > 0 else {
    fputs("DIRECT_AX_ERROR stage=arguments reason=invalid-target-pid\n", stderr)
    exit(2)
}
let requestedTitle = env["PULSEFORGE_AX_WINDOW_TITLE"] ?? ""
let operation = env["PULSEFORGE_AX_OPERATION"] ?? "snapshot"
let deepSnapshot = env["PULSEFORGE_AX_DEEP_SNAPSHOT"] == "1"
let controlName = env["PULSEFORGE_AX_CONTROL_NAME"] ?? ""
let controlValue = env["PULSEFORGE_AX_CONTROL_VALUE"] ?? ""
let client = NSRunningApplication(processIdentifier: getpid())
var executablePath = [CChar](repeating: 0, count: 4096)
let executablePathLength = executablePath.withUnsafeMutableBufferPointer {
    proc_pidpath(getpid(), $0.baseAddress, UInt32($0.count))
}
let clientPath = executablePathLength > 0
    ? executablePath.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    : (CommandLine.arguments.first ?? "unknown")
let clientName = client?.localizedName ?? "command-line Swift client"
print("DIRECT_AX_CLIENT pid=\(getpid()); path=\(clientPath); name=\(clientName); trusted=\(AXIsProcessTrusted())")
let targetProcess = NSRunningApplication(processIdentifier: targetPID)
print("DIRECT_AX_TARGET_PROCESS pid=\(targetPID); name=\(targetProcess?.localizedName ?? "unknown"); bundle=\(targetProcess?.bundleIdentifier ?? "unknown")")
fflush(stdout)

if operation == "window-list" {
    let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    let entries = (CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? [])
        .filter {
            ($0[kCGWindowOwnerPID as String] as? NSNumber)?.int32Value == targetPID &&
                (($0[kCGWindowLayer as String] as? NSNumber)?.intValue ?? -1) == 0
        }
    print("DIRECT_CG_RESULT status=snapshot; pid=\(targetPID); windows=\(entries.count)")
    for (index, entry) in entries.enumerated() {
        let number = (entry[kCGWindowNumber as String] as? NSNumber)?.uint32Value ?? 0
        let title = (entry[kCGWindowName as String] as? String) ?? "<unavailable>"
        let bounds = entry[kCGWindowBounds as String].map { String(describing: $0) } ?? "<unavailable>"
        print("DIRECT_CG_WINDOW pid=\(targetPID); index=\(index + 1); window_number=\(number); title=\(title); bounds=\(bounds)")
    }
    exit(0)
}

let app = AXUIElementCreateApplication(targetPID)
AXUIElementSetMessagingTimeout(app, 1.0)
if operation == "close-main-window" {
    print("DIRECT_AX_STAGE operation=close-main-window; stage=main-window-query")
    fflush(stdout)
    var rawWindow: CFTypeRef?
    let windowError = AXUIElementCopyAttributeValue(app, "AXMainWindow" as CFString, &rawWindow)
    guard windowError == .success, let rawWindow, CFGetTypeID(rawWindow) == AXUIElementGetTypeID() else {
        print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=main-window; ax_error=\(windowError.rawValue)")
        exit(5)
    }
    let window = rawWindow as! AXUIElement
    AXUIElementSetMessagingTimeout(window, 1.0)
    print("DIRECT_AX_STAGE operation=close-main-window; stage=close-button-query")
    fflush(stdout)
    guard let rawCloseButton = value(window, "AXCloseButton"),
          CFGetTypeID(rawCloseButton) == AXUIElementGetTypeID() else {
        print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=close-button")
        exit(6)
    }
    let closeButton = rawCloseButton as! AXUIElement
    print("DIRECT_AX_STAGE operation=close-main-window; stage=close-button-actions")
    fflush(stdout)
    var actions: CFArray?
    let actionsError = AXUIElementCopyActionNames(closeButton, &actions)
    guard actionsError == .success, (actions as? [String] ?? []).contains("AXPress") else {
        print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=close-button-actions; ax_error=\(actionsError.rawValue); actions=\(actions as? [String] ?? [])")
        exit(7)
    }
    AXUIElementSetMessagingTimeout(closeButton, 1.0)
    print("DIRECT_AX_DISPATCH attempted=true; operation=close-main-window; pid=\(targetPID); window=AXMainWindow")
    fflush(stdout)
    let pressError = AXUIElementPerformAction(closeButton, kAXPressAction as CFString)
    print("DIRECT_AX_RESULT status=\(pressError == .success ? "action-returned" : "action-error"); operation=close-main-window; pid=\(targetPID); ax_error=\(pressError.rawValue)")
    exit(pressError == .success ? 0 : 8)
}
let queryStarted = ProcessInfo.processInfo.systemUptime
var rawWindows: CFTypeRef?
let windowsError = AXUIElementCopyAttributeValue(app, "AXWindows" as CFString, &rawWindows)
guard windowsError == .success, let rawWindows, let windows = rawWindows as? [AXUIElement] else {
    print("DIRECT_AX_RESULT status=error; stage=copy-windows; pid=\(targetPID); ax_error=\(windowsError.rawValue); elapsed_ms=\((ProcessInfo.processInfo.systemUptime - queryStarted) * 1000)")
    exit(3)
}
var titleMatches: [AXUIElement] = []
var windowReadErrors: [(Int, Int32)] = []
for (index, window) in windows.enumerated() {
    AXUIElementSetMessagingTimeout(window, 0.75)
    if requestedTitle.isEmpty {
        titleMatches.append(window)
        continue
    }
    print("DIRECT_AX_WINDOW_TITLE_QUERY_BEGIN index=\(index); pid=\(targetPID)")
    fflush(stdout)
    var rawTitle: CFTypeRef?
    let titleError = AXUIElementCopyAttributeValue(window, "AXTitle" as CFString, &rawTitle)
    if titleError == .success, let title = rawTitle as? String {
        if title.contains(requestedTitle) { titleMatches.append(window) }
        print("DIRECT_AX_WINDOW_TITLE_QUERY_END index=\(index); ax_error=0; title=\(title)")
    } else {
        windowReadErrors.append((index, titleError.rawValue))
        print("DIRECT_AX_WINDOW_TITLE_QUERY_END index=\(index); ax_error=\(titleError.rawValue); title=<unavailable>")
    }
    fflush(stdout)
}
let matching = !titleMatches.isEmpty || requestedTitle.isEmpty || operation != "snapshot"
    ? titleMatches
    : windows.filter { string($0, "AXSubrole") == "AXDialog" || string($0, "AXRole") == "AXSheet" }
struct AXNode {
    let element: AXUIElement
    let parent: Int?
    let depth: Int
}
func collect(_ roots: [AXUIElement], depthLimit: Int = 8) -> [AXNode] {
    var pending = roots.map { ($0, Optional<Int>.none, 0) }
    var found: [AXNode] = []
    var next = 0
    while next < pending.count, found.count < 256 {
        let (element, parent, depth) = pending[next]
        next += 1
        AXUIElementSetMessagingTimeout(element, 1.0)
        let index = found.count
        found.append(AXNode(element: element, parent: parent, depth: depth))
        guard depth < depthLimit, let rawChildren = value(element, "AXChildren"),
              let children = rawChildren as? [AXUIElement] else { continue }
        pending.append(contentsOf: children.map { ($0, Optional(index), depth + 1) })
    }
    return found
}
let needsDescendants = deepSnapshot || operation == "press-control" || operation == "set-value" || operation == "select-element"
let descendants: [AXNode] = needsDescendants
    ? collect(matching, depthLimit: operation == "snapshot" ? 4 : 8)
    : []
func labels(_ element: AXUIElement) -> [String] {
    var result = ["AXTitle", "AXDescription", "AXIdentifier", "AXValue", "AXFilename", "AXURL", "AXDocument"]
        .compactMap { string(element, $0) }
        .filter { !$0.isEmpty }
    if let rawURL = value(element, "AXURL"), let url = rawURL as? URL {
        result.append(url.path)
        result.append(url.lastPathComponent)
    }
    return result
}
func titleOrDescription(_ element: AXUIElement) -> String {
    labels(element).first ?? ""
}
if operation == "snapshot" {
    let snapshotDepth = deepSnapshot ? 4 : 8
    print("DIRECT_AX_RESULT status=snapshot; pid=\(targetPID); windows=\(matching.count); all_windows=\(windows.count); depth_limit=\(deepSnapshot ? snapshotDepth : 0); node_limit=256; elapsed_ms=\((ProcessInfo.processInfo.systemUptime - queryStarted) * 1000); window_read_errors=\(windowReadErrors)")
    fflush(stdout)
    for (index, item) in matching.enumerated() {
        AXUIElementSetMessagingTimeout(item, 0.75)
        print("DIRECT_AX_WINDOW_SNAPSHOT_BEGIN index=\(index + 1)")
        fflush(stdout)
        var rawTitle: CFTypeRef?
        let titleError = AXUIElementCopyAttributeValue(item, "AXTitle" as CFString, &rawTitle)
        let windowTitle = titleError == .success ? (rawTitle as? String ?? "") : "<AXError:\(titleError.rawValue)>"
        print("DIRECT_AX_WINDOW pid=\(targetPID); index=\(index + 1); role=AXWindow; title=\(windowTitle)")
        fflush(stdout)
    }
    guard deepSnapshot else { exit(0) }
    for (index, node) in descendants.enumerated() {
        let element = node.element
        let role = string(element, "AXRole") ?? ""
        let subrole = string(element, "AXSubrole") ?? ""
        let name = titleOrDescription(element)
        let enabled = described(element, "AXEnabled")
        var actions: CFArray?
        let actionError = AXUIElementCopyActionNames(element, &actions)
        print("DIRECT_AX_NODE index=\(index); parent=\(node.parent.map(String.init) ?? "none"); depth=\(node.depth); role=\(role); subrole=\(subrole); name=\(name); enabled=\(enabled); actions_error=\(actionError.rawValue); actions=\(actions as? [String] ?? [])")
    }
    exit(0)
}
if operation == "press-control" || operation == "set-value" || operation == "select-element" {
    let wantedRole = operation == "press-control" ? "AXButton" : operation == "set-value" ? "AXTextField" : "AXRow/AXCell"
    func labelMatches(_ element: AXUIElement) -> Bool {
        labels(element).contains { $0 == controlName || $0.hasSuffix("/" + controlName) }
    }
    var controls = descendants.enumerated().compactMap { index, node -> (Int, AXNode)? in
        let role = string(node.element, "AXRole") ?? ""
        let roleMatches = operation == "select-element"
            ? (role == "AXRow" || role == "AXCell")
            : role == wantedRole
        return roleMatches && (controlName.isEmpty || labelMatches(node.element)) ? (index, node) : nil
    }
    if operation == "select-element", controls.isEmpty {
        for (_, node) in descendants.enumerated() where
            ["AXStaticText", "AXTextField", "AXCell", "AXRow"].contains(string(node.element, "AXRole") ?? "") &&
            labelMatches(node.element) {
            var parent = node.parent
            while let parentIndex = parent {
                let ancestor = descendants[parentIndex]
                let role = string(ancestor.element, "AXRole") ?? ""
                if role == "AXRow" || role == "AXCell" {
                    controls.append((parentIndex, ancestor))
                    break
                }
                parent = ancestor.parent
            }
        }
    }
    guard controls.count == 1, let (_, controlNode) = controls.first else {
        let candidates = descendants.map { string($0.element, "AXRole") ?? "" }
            .filter { $0 == "AXRow" || $0 == "AXCell" }
        print("DIRECT_AX_RESULT status=control-selection-error; operation=\(operation); role=\(wantedRole); name=\(controlName); matches=\(controls.count); candidate_roles=\(candidates)")
        exit(8)
    }
    let control = controlNode.element
    if operation == "set-value" {
        let setError = AXUIElementSetAttributeValue(control, "AXValue" as CFString, controlValue as CFString)
        let observedValue = string(control, "AXValue") ?? "<unavailable>"
        let verified = setError == .success && observedValue == controlValue
        print("DIRECT_AX_RESULT status=\(verified ? "value-verified" : "value-error"); operation=set-value; name=\(titleOrDescription(control)); expected=\(controlValue); observed=\(observedValue); ax_error=\(setError.rawValue)")
        exit(verified ? 0 : 9)
    }
    if operation == "select-element" {
        let role = string(control, "AXRole") ?? ""
        AXUIElementSetMessagingTimeout(control, 1.0)
        let selectError = AXUIElementSetAttributeValue(control, "AXSelected" as CFString, kCFBooleanTrue)
        let selected = (value(control, "AXSelected") as? Bool) ?? false
        let verified = selectError == .success && selected
        print("DIRECT_AX_RESULT status=\(verified ? "selection-verified" : "selection-error"); operation=select-element; role=\(role); name=\(titleOrDescription(control)); ax_error=\(selectError.rawValue); selected=\(selected)")
        exit(verified ? 0 : 12)
    }
    AXUIElementSetMessagingTimeout(control, 1.0)
    let enabled = (value(control, "AXEnabled") as? Bool) ?? false
    guard enabled else {
        print("DIRECT_AX_RESULT status=control-disabled; operation=press-control; name=\(titleOrDescription(control))")
        exit(10)
    }
    print("DIRECT_AX_DISPATCH attempted=true; operation=press-control; name=\(titleOrDescription(control)); target_pid=\(targetPID)")
    fflush(stdout)
    let pressError = AXUIElementPerformAction(control, kAXPressAction as CFString)
    print("DIRECT_AX_RESULT status=\(pressError == .success ? "action-returned" : "action-error"); operation=press-control; name=\(titleOrDescription(control)); ax_error=\(pressError.rawValue)")
    exit(pressError == .success ? 0 : 11)
}
guard matching.count == 1, let window = matching.first else {
    print("DIRECT_AX_RESULT status=window-selection-error; pid=\(targetPID); requested_title=\(requestedTitle); matches=\(matching.count); windows=\(windows.compactMap { string($0, "AXTitle") })")
    exit(4)
}
let title = string(window, "AXTitle") ?? ""
let role = string(window, "AXRole") ?? ""
let subrole = string(window, "AXSubrole") ?? ""
print("DIRECT_AX_TARGET pid=\(targetPID); role=\(role); subrole=\(subrole); title=\(title)")

if operation == "close-window" {
    guard let closeButton = value(window, "AXCloseButton"), CFGetTypeID(closeButton) == AXUIElementGetTypeID() else {
        print("DIRECT_AX_RESULT status=error; stage=close-button; reason=missing-or-invalid")
        exit(5)
    }
    var actions: CFArray?
    let actionError = AXUIElementCopyActionNames(closeButton as! AXUIElement, &actions)
    guard actionError == .success, (actions as? [String] ?? []).contains("AXPress") else {
        print("DIRECT_AX_RESULT status=error; stage=close-button-actions; ax_error=\(actionError.rawValue); actions=\(actions as? [String] ?? [])")
        exit(6)
    }
    AXUIElementSetMessagingTimeout(closeButton as! AXUIElement, 1.0)
    print("DIRECT_AX_DISPATCH attempted=true; operation=close-window; pid=\(targetPID); title=\(title)")
    fflush(stdout)
    let pressError = AXUIElementPerformAction(closeButton as! AXUIElement, kAXPressAction as CFString)
    print("DIRECT_AX_RESULT status=\(pressError == .success ? "action-returned" : "action-error"); operation=close-window; pid=\(targetPID); title=\(title); ax_error=\(pressError.rawValue)")
    exit(pressError == .success ? 0 : 7)
}

fputs("DIRECT_AX_ERROR stage=arguments reason=unsupported-operation operation=\(operation)\n", stderr)
exit(2)
"""


def run_direct_ax(
    target_pid: int,
    *,
    operation: str,
    window_title: str = "",
    control_name: str = "",
    control_value: str = "",
    deep_snapshot: bool = False,
    timeout: float = 8.0,
) -> tuple[int, str, str]:
    """Run a direct AX query/action in a bounded `swift -e` client process."""
    swift = shutil.which("swift")
    if swift is None:
        return 127, "", "Swift interpreter was not found"
    environment = os.environ.copy()
    environment.update(
        {
            "PULSEFORGE_AX_TARGET_PID": str(target_pid),
            "PULSEFORGE_AX_OPERATION": operation,
            "PULSEFORGE_AX_WINDOW_TITLE": window_title,
            "PULSEFORGE_AX_CONTROL_NAME": control_name,
            "PULSEFORGE_AX_CONTROL_VALUE": control_value,
            "PULSEFORGE_AX_DEEP_SNAPSHOT": "1" if deep_snapshot else "0",
        }
    )
    try:
        process = subprocess.Popen(
            [swift, "-e", SWIFT_AX_OPERATION],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            start_new_session=True,
        )
    except OSError as error:
        return 127, "", f"Could not start direct AX client {swift!r}: {error}"
    try:
        stdout, stderr = process.communicate(timeout=max(0.1, timeout))
        return process.returncode or 0, stdout, stderr
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
                stdout, stderr = process.communicate(timeout=0.75)
            except subprocess.TimeoutExpired as error:
                stdout = error.stdout or ""
                stderr = error.stderr or ""
                if isinstance(stdout, bytes):
                    stdout = stdout.decode("utf-8", errors="replace")
                if isinstance(stderr, bytes):
                    stderr = stderr.decode("utf-8", errors="replace")
                return 124, stdout, stderr + f"\nDIRECT_AX_RESULT status=timeout; process_group_cleanup=failed; client_pid={process.pid}"
        return 124, stdout, stderr + "\nDIRECT_AX_RESULT status=timeout; process_group_reaped=true"
