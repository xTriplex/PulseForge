"""Bounded Accessibility operations performed by the Swift interpreter client itself."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess


SWIFT_AX_OPERATION = r"""
import AppKit
import ApplicationServices
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

if operation == "client-preflight" {
    print("DIRECT_AX_RESULT status=client-preflight; pid=\(getpid()); target_pid=\(targetPID); ax_target_query=not-performed")
    exit(0)
}

let app = AXUIElementCreateApplication(targetPID)
AXUIElementSetMessagingTimeout(app, 1.0)
if operation == "close-main-window" {
    print("DIRECT_AX_STAGE operation=close-main-window; stage=main-window-query")
    fflush(stdout)
    var rawWindows: CFTypeRef?
    let windowError = AXUIElementCopyAttributeValue(app, "AXWindows" as CFString, &rawWindows)
    guard windowError == .success, let rawWindows, let windows = rawWindows as? [AXUIElement] else {
        print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=window-list; ax_error=\(windowError.rawValue)")
        exit(5)
    }
    let windowCandidates = windows.filter {
        string($0, "AXRole") == "AXWindow" &&
        string($0, "AXSubrole") == "AXStandardWindow" &&
        (string($0, "AXTitle") ?? "").contains(requestedTitle)
    }
    guard !requestedTitle.isEmpty, windowCandidates.count == 1, let window = windowCandidates.first else {
        let titles = windows.map { string($0, "AXTitle") ?? "<unavailable>" }
        print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=window-selection; requested_title=\(requestedTitle); matches=\(windowCandidates.count); titles=\(titles)")
        exit(5)
    }
    AXUIElementSetMessagingTimeout(window, 1.0)
    let windowRole = string(window, "AXRole") ?? "<unavailable>"
    let windowSubrole = string(window, "AXSubrole") ?? "<unavailable>"
    let windowTitle = string(window, "AXTitle") ?? "<unavailable>"
    print("DIRECT_AX_CLOSE_WINDOW pid=\(targetPID); role=\(windowRole); subrole=\(windowSubrole); title=\(windowTitle)")
    print("DIRECT_AX_STAGE operation=close-main-window; stage=close-button-query")
    fflush(stdout)
    var rawCloseButton: CFTypeRef?
    let closeAttributeError = AXUIElementCopyAttributeValue(window, "AXCloseButton" as CFString, &rawCloseButton)
    var closeButton: AXUIElement?
    var selectedBy = "AXCloseButton"
    if closeAttributeError == .success, let rawCloseButton,
       CFGetTypeID(rawCloseButton) == AXUIElementGetTypeID() {
        let resolvedCloseButton = rawCloseButton as! AXUIElement
        closeButton = resolvedCloseButton
    }
    if closeButton == nil {
        var rawChildren: CFTypeRef?
        let childrenError = AXUIElementCopyAttributeValue(window, "AXChildren" as CFString, &rawChildren)
        let children = (rawChildren as? [AXUIElement]) ?? []
        let closeCandidates = children.filter {
            string($0, "AXRole") == "AXButton" && string($0, "AXSubrole") == "AXCloseButton"
        }
        print("DIRECT_AX_CLOSE_FALLBACK close_attribute_error=\(closeAttributeError.rawValue); children_error=\(childrenError.rawValue); child_count=\(children.count); close_candidates=\(closeCandidates.count)")
        guard closeCandidates.count == 1, let candidate = closeCandidates.first else {
            let childSummary = children.prefix(24).map { child in
                let childRole = string(child, "AXRole") ?? "<unavailable>"
                let childSubrole = string(child, "AXSubrole") ?? "<unavailable>"
                let childName = titleOrDescription(child)
                var childActions: CFArray?
                let childActionError = AXUIElementCopyActionNames(child, &childActions)
                return "role=\(childRole);subrole=\(childSubrole);name=\(childName);actions_error=\(childActionError.rawValue);actions=\(childActions as? [String] ?? [])"
            }
            print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=close-button; requested_title=\(requestedTitle); close_attribute_error=\(closeAttributeError.rawValue); children_error=\(childrenError.rawValue); child_controls=\(childSummary)")
            exit(6)
        }
        closeButton = candidate
        selectedBy = "unique-AXChildren-AXButton-AXCloseButton"
    }
    guard let closeButton else {
        print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=close-button; reason=missing-or-invalid; close_attribute_error=\(closeAttributeError.rawValue)")
        exit(6)
    }
    print("DIRECT_AX_STAGE operation=close-main-window; stage=close-button-actions")
    fflush(stdout)
    var actions: CFArray?
    let actionsError = AXUIElementCopyActionNames(closeButton, &actions)
    guard actionsError == .success, (actions as? [String] ?? []).contains("AXPress") else {
        print("DIRECT_AX_RESULT status=error; operation=close-main-window; stage=close-button-actions; ax_error=\(actionsError.rawValue); actions=\(actions as? [String] ?? [])")
        exit(7)
    }
    AXUIElementSetMessagingTimeout(closeButton, 1.0)
    print("DIRECT_AX_DISPATCH attempted=true; operation=close-main-window; pid=\(targetPID); window_title=\(windowTitle); selected_by=\(selectedBy)")
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
if operation == "window-count" {
    print("DIRECT_AX_RESULT status=window-count; pid=\(targetPID); windows=\(windows.count); elapsed_ms=\((ProcessInfo.processInfo.systemUptime - queryStarted) * 1000)")
    exit(0)
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
if operation == "activate-window" {
    guard !requestedTitle.isEmpty, titleMatches.count == 1, let window = titleMatches.first else {
        print("DIRECT_AX_RESULT status=window-activation-error; stage=window-selection; pid=\(targetPID); requested_title=\(requestedTitle); matches=\(titleMatches.count)")
        exit(13)
    }
    let targetTitle = string(window, "AXTitle") ?? ""
    var rawActions: CFArray?
    let actionError = AXUIElementCopyActionNames(window, &rawActions)
    let actions = rawActions as? [String] ?? []
    guard actionError == .success, actions.contains("AXRaise") else {
        print("DIRECT_AX_RESULT status=window-activation-error; stage=raise-action; pid=\(targetPID); title=\(targetTitle); ax_error=\(actionError.rawValue); actions=\(actions)")
        exit(14)
    }
    _ = targetProcess?.activate(options: [])
    AXUIElementSetMessagingTimeout(window, 1.0)
    let raiseError = AXUIElementPerformAction(window, "AXRaise" as CFString)
    guard raiseError == .success else {
        print("DIRECT_AX_RESULT status=window-activation-error; stage=raise; pid=\(targetPID); title=\(targetTitle); ax_error=\(raiseError.rawValue)")
        exit(15)
    }
    let focusDeadline = ProcessInfo.processInfo.systemUptime + 2.0
    var attempts = 0
    var focusedTitle = "<unavailable>"
    var focusError: Int32 = 0
    while ProcessInfo.processInfo.systemUptime < focusDeadline {
        attempts += 1
        var rawFocusedWindow: CFTypeRef?
        focusError = AXUIElementCopyAttributeValue(app, "AXFocusedWindow" as CFString, &rawFocusedWindow).rawValue
        if focusError == 0, let rawFocusedWindow, CFGetTypeID(rawFocusedWindow) == AXUIElementGetTypeID() {
            focusedTitle = string(rawFocusedWindow as! AXUIElement, "AXTitle") ?? "<untitled>"
            if focusedTitle == targetTitle {
                print("DIRECT_AX_RESULT status=window-focused; pid=\(targetPID); target_title=\(targetTitle); focused_title=\(focusedTitle); ax_raise_error=\(raiseError.rawValue); focus_error=\(focusError); attempts=\(attempts)")
                exit(0)
            }
        }
        usleep(20000)
    }
    print("DIRECT_AX_RESULT status=window-focus-timeout; pid=\(targetPID); target_title=\(targetTitle); focused_title=\(focusedTitle); ax_raise_error=\(raiseError.rawValue); focus_error=\(focusError); attempts=\(attempts)")
    exit(16)
}
if operation == "observe-go-to-folder-state" {
    guard !requestedTitle.isEmpty, titleMatches.count == 1, let window = titleMatches.first else {
        print("DIRECT_AX_RESULT status=go-to-folder-state-error; stage=window-selection; requested_title=\(requestedTitle); matches=\(titleMatches.count)")
        exit(20)
    }
    struct PendingElement {
        let element: AXUIElement
        let depth: Int
    }
    var pending = [PendingElement(element: window, depth: 0)]
    var next = 0
    var sheetCount = 0
    var queryErrors: [String] = []
    let nodeLimit = 128
    let depthLimit = 8
    var visitedRoles: [String] = []
    while next < pending.count && next < nodeLimit {
        let item = pending[next]
        next += 1
        AXUIElementSetMessagingTimeout(item.element, 0.2)
        var rawRole: CFTypeRef?
        let roleError = AXUIElementCopyAttributeValue(item.element, "AXRole" as CFString, &rawRole)
        guard roleError == .success, let role = rawRole as? String else {
            queryErrors.append("role:\(roleError.rawValue)")
            continue
        }
        visitedRoles.append("\(role)@\(item.depth)")
        if role == "AXSheet" {
            sheetCount += 1
            var rawTitle: CFTypeRef?
            let titleError = AXUIElementCopyAttributeValue(item.element, "AXTitle" as CFString, &rawTitle)
            let title = titleError == .success ? (rawTitle as? String ?? "") : ""
            print("DIRECT_AX_GO_TO_FOLDER_NODE role=\(role); title=\(title); title_error=\(titleError.rawValue); depth=\(item.depth)")
        }
        if ["AXOutline", "AXBrowser", "AXTable", "AXList"].contains(role) { continue }
        var rawChildren: CFTypeRef?
        let childrenError = AXUIElementCopyAttributeValue(item.element, "AXChildren" as CFString, &rawChildren)
        if childrenError == .success {
            guard let rawChildren, let children = rawChildren as? [AXUIElement] else {
                queryErrors.append("children-invalid:\(item.depth)")
                continue
            }
            if item.depth >= depthLimit {
                if !children.isEmpty { queryErrors.append("depth-limit:\(item.depth)") }
            } else {
                pending.append(contentsOf: children.map { PendingElement(element: $0, depth: item.depth + 1) })
            }
        } else if childrenError != .noValue &&
            (childrenError != .attributeUnsupported ||
             ["AXWindow", "AXSheet", "AXGroup", "AXSplitGroup", "AXScrollArea", "AXBrowser", "AXOutline"].contains(role)) {
            queryErrors.append("children:\(childrenError.rawValue):\(item.depth)")
        }
    }
    let truncated = next < pending.count
    let complete = queryErrors.isEmpty && !truncated
    print("DIRECT_AX_RESULT status=go-to-folder-state; pid=\(targetPID); window_title=\(requestedTitle); visited=\(next); sheet_count=\(sheetCount); complete=\(complete); truncated=\(truncated); query_errors=\(queryErrors); roles=\(visitedRoles.prefix(48))")
    exit(complete ? 0 : 21)
}
if operation == "observe-open-button-state" {
    guard requestedTitle == "Open", titleMatches.count == 1, let window = titleMatches.first,
          string(window, "AXTitle") == requestedTitle else {
        print("DIRECT_AX_RESULT status=open-button-state-error; stage=window-selection; requested_title=\(requestedTitle); matches=\(titleMatches.count)")
        exit(22)
    }
    struct PendingControl {
        let element: AXUIElement
        let depth: Int
    }
    let ignoredSubtreeRoles: Set<String> = [
        "AXOutline", "AXBrowser", "AXTable", "AXList", "AXRow", "AXCell", "AXStaticText", "AXTextField",
        "AXButton", "AXImage", "AXPopUpButton", "AXCheckBox", "AXRadioButton", "AXSlider", "AXTextArea"
    ]
    var pending = [PendingControl(element: window, depth: 0)]
    var next = 0
    var matches: [(Bool?, Int32)] = []
    var queryErrors: [String] = []
    let nodeLimit = 128
    let depthLimit = 8
    while next < pending.count && next < nodeLimit {
        let item = pending[next]
        next += 1
        AXUIElementSetMessagingTimeout(item.element, 0.25)
        var rawRole: CFTypeRef?
        let roleError = AXUIElementCopyAttributeValue(item.element, "AXRole" as CFString, &rawRole)
        guard roleError == .success, let role = rawRole as? String else {
            queryErrors.append("role:\(roleError.rawValue)")
            continue
        }
        if role == "AXButton" {
            var names: [String] = []
            for attribute in ["AXTitle", "AXDescription", "AXIdentifier"] {
                var rawValue: CFTypeRef?
                let labelError = AXUIElementCopyAttributeValue(item.element, attribute as CFString, &rawValue)
                if labelError == .success, let label = rawValue as? String, !label.isEmpty {
                    names.append(label)
                } else if labelError != .noValue && labelError != .attributeUnsupported {
                    queryErrors.append("label:\(labelError.rawValue)")
                }
            }
            if names.contains("Open") {
                var rawEnabled: CFTypeRef?
                let enabledError = AXUIElementCopyAttributeValue(item.element, "AXEnabled" as CFString, &rawEnabled)
                let enabled = enabledError == .success ? (rawEnabled as? Bool) : nil
                matches.append((enabled, enabledError.rawValue))
            }
        }
        guard !ignoredSubtreeRoles.contains(role) else { continue }
        var rawChildren: CFTypeRef?
        let childrenError = AXUIElementCopyAttributeValue(item.element, "AXChildren" as CFString, &rawChildren)
        if childrenError == .success {
            guard let rawChildren, let children = rawChildren as? [AXUIElement] else {
                queryErrors.append("children-invalid:\(item.depth)")
                continue
            }
            if item.depth >= depthLimit {
                if !children.isEmpty { queryErrors.append("depth-limit:\(item.depth)") }
            } else {
                pending.append(contentsOf: children.map { PendingControl(element: $0, depth: item.depth + 1) })
            }
        } else if childrenError != .noValue && childrenError != .attributeUnsupported {
            queryErrors.append("children:\(childrenError.rawValue):\(item.depth)")
        }
    }
    let truncated = next < pending.count
    let complete = queryErrors.isEmpty && !truncated
    let enabled = matches.count == 1 ? (matches[0].0.map { String($0) } ?? "unknown") : "unknown"
    let enabledError = matches.count == 1 ? matches[0].1 : -1
    print("DIRECT_AX_RESULT status=open-button-state; pid=\(targetPID); window_title=\(requestedTitle); matches=\(matches.count); enabled=\(enabled); enabled_error=\(enabledError); complete=\(complete); truncated=\(truncated); visited=\(next); query_errors=\(queryErrors)")
    exit(complete ? 0 : 23)
}
struct AXNode {
    let element: AXUIElement
    let parent: Int?
    let depth: Int
}
func collect(_ roots: [AXUIElement], depthLimit: Int = 8, nodeLimit: Int = 256) -> [AXNode] {
    var pending = roots.map { ($0, Optional<Int>.none, 0) }
    var found: [AXNode] = []
    var next = 0
    while next < pending.count, found.count < nodeLimit {
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
let needsDescendants = deepSnapshot || operation == "press-control" || operation == "set-value" ||
    operation == "set-focused-value-confirm" || operation == "set-folder-path-value" ||
    operation == "select-element" || operation == "observe-element" || operation == "confirm-element"
let descendants: [AXNode] = {
    guard needsDescendants else { return [] }
    let usesVisibleBrowserColumn = operation == "select-element" || operation == "observe-element" ||
        operation == "confirm-element"
    if !usesVisibleBrowserColumn { return collect(matching, depthLimit: operation == "snapshot" ? 4 : 8) }

    let selectionStarted = ProcessInfo.processInfo.systemUptime
    print("DIRECT_AX_SELECT_STAGE stage=window-tree-begin; node_limit=96; depth_limit=3")
    fflush(stdout)
    // Finder panels expose column-view contents through AXColumns/AXVisibleColumns,
    // not only through the browser's AXChildren. The rightmost column is the
    // selected directory, so search it directly instead of walking every folder column.
    let windowNodes = collect(matching, depthLimit: 3, nodeLimit: 96)
    print("DIRECT_AX_SELECT_STAGE stage=window-tree-end; nodes=\(windowNodes.count); elapsed_ms=\((ProcessInfo.processInfo.systemUptime - selectionStarted) * 1000)")
    fflush(stdout)
    let browsers = windowNodes.enumerated().compactMap { index, node -> (Int, AXUIElement)? in
        string(node.element, "AXRole") == "AXBrowser" ? (index, node.element) : nil
    }
    print("DIRECT_AX_SELECT_STAGE stage=browsers-found; count=\(browsers.count)")
    fflush(stdout)
    var allNodes = windowNodes
    for (browserIndex, browser) in browsers {
        AXUIElementSetMessagingTimeout(browser, 0.5)
        print("DIRECT_AX_SELECT_STAGE stage=browser-columns-begin; index=\(browserIndex)")
        fflush(stdout)
        let visibleColumns = value(browser, "AXVisibleColumns") as? [AXUIElement]
        print("DIRECT_AX_SELECT_STAGE stage=visible-columns-read; index=\(browserIndex); count=\(visibleColumns?.count ?? -1); elapsed_ms=\((ProcessInfo.processInfo.systemUptime - selectionStarted) * 1000)")
        fflush(stdout)
        let columns = visibleColumns.flatMap { $0.isEmpty ? nil : $0 } ??
            (value(browser, "AXColumns") as? [AXUIElement] ?? [])
        print("DIRECT_AX_SELECT_STAGE stage=columns-selected; index=\(browserIndex); count=\(columns.count); elapsed_ms=\((ProcessInfo.processInfo.systemUptime - selectionStarted) * 1000)")
        fflush(stdout)
        let selectedDirectoryRoots = columns.isEmpty ? [browser] : [columns[columns.count - 1]]
        let browserNodes = collect(selectedDirectoryRoots, depthLimit: 8, nodeLimit: 96)
        print("DIRECT_AX_SELECT_STAGE stage=column-tree-end; index=\(browserIndex); nodes=\(browserNodes.count); elapsed_ms=\((ProcessInfo.processInfo.systemUptime - selectionStarted) * 1000)")
        fflush(stdout)
        let offset = allNodes.count
        allNodes.append(contentsOf: browserNodes.enumerated().map { _, node in
            let parent = node.parent.map { $0 + offset } ?? browserIndex
            return AXNode(element: node.element, parent: parent, depth: node.depth)
        })
    }
    return allNodes
}()
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
        let focused = described(element, "AXFocused")
        print("DIRECT_AX_NODE index=\(index); parent=\(node.parent.map(String.init) ?? "none"); depth=\(node.depth); role=\(role); subrole=\(subrole); name=\(name); enabled=\(enabled); focused=\(focused); actions_error=\(actionError.rawValue); actions=\(actions as? [String] ?? [])")
    }
    exit(0)
}
if operation == "press-control" || operation == "set-value" || operation == "set-focused-value-confirm" ||
    operation == "set-folder-path-value" || operation == "select-element" || operation == "observe-element" ||
    operation == "confirm-element" {
    let wantedRole = operation == "press-control" ? "AXButton" :
        (operation == "set-value" || operation == "set-focused-value-confirm" ||
            operation == "set-folder-path-value")
            ? "AXTextField" : "AXRow/AXCell/AXTextField"
    func labelMatches(_ element: AXUIElement) -> Bool {
        labels(element).contains { $0 == controlName || $0.hasSuffix("/" + controlName) }
    }
    var controls = descendants.enumerated().compactMap { index, node -> (Int, AXNode)? in
        let role = string(node.element, "AXRole") ?? ""
        let roleMatches = operation == "select-element" || operation == "observe-element"
            ? (role == "AXRow" || role == "AXCell" || role == "AXTextField")
            : operation == "confirm-element" ? role == "AXTextField" : role == wantedRole
        guard roleMatches else { return nil }
        let focusedMatch = operation == "set-focused-value-confirm" && (value(node.element, "AXFocused") as? Bool) == true
        let labelMatch = operation == "set-focused-value-confirm" ? focusedMatch : (controlName.isEmpty || labelMatches(node.element))
        if operation == "set-folder-path-value" {
            guard roleMatches, labelMatches(node.element) else { return nil }
            var parent = node.parent
            var folderSheetFound = false
            while let parentIndex = parent {
                let ancestor = descendants[parentIndex].element
                if string(ancestor, "AXRole") == "AXSheet" &&
                    labels(ancestor).contains(where: { $0 == "GoToWindow" || $0 == "Go to Folder" }) {
                    folderSheetFound = true
                    break
                }
                parent = descendants[parentIndex].parent
            }
            return folderSheetFound ? (index, node) : nil
        }
        return labelMatch ? (index, node) : nil
    }
    if operation == "select-element", controls.isEmpty {
        for (index, node) in descendants.enumerated() where
            ["AXStaticText", "AXTextField", "AXCell", "AXRow"].contains(string(node.element, "AXRole") ?? "") &&
            labelMatches(node.element) {
            var parent = node.parent
            var foundSelectableAncestor = false
            while let parentIndex = parent {
                let ancestor = descendants[parentIndex]
                let role = string(ancestor.element, "AXRole") ?? ""
                if role == "AXRow" || role == "AXCell" {
                    controls.append((parentIndex, ancestor))
                    foundSelectableAncestor = true
                    break
                }
                parent = ancestor.parent
            }
            if !foundSelectableAncestor, string(node.element, "AXRole") == "AXTextField" {
                controls.append((index, node))
            }
        }
    }
    guard controls.count == 1, let (controlIndex, controlNode) = controls.first else {
        let candidates = descendants.map { string($0.element, "AXRole") ?? "" }
            .filter { $0 == "AXRow" || $0 == "AXCell" || $0 == "AXTextField" }
        let locationNodes = descendants.compactMap { node -> String? in
            let role = string(node.element, "AXRole") ?? ""
            guard ["AXPopUpButton", "AXBrowser", "AXTextField", "AXOutline"].contains(role) else { return nil }
            let nodeLabels = labels(node.element).prefix(4).map { String($0.prefix(160)) }
            return "depth=\(node.depth); role=\(role); labels=\(nodeLabels)"
        }.prefix(8)
        let rowNodes = descendants.compactMap { node -> String? in
            let role = string(node.element, "AXRole") ?? ""
            guard ["AXRow", "AXCell", "AXStaticText", "AXTextField"].contains(role) else { return nil }
            let nodeLabels = labels(node.element).prefix(4).map { String($0.prefix(160)) }
            let parentRole = node.parent.map { string(descendants[$0].element, "AXRole") ?? "" } ?? "<root>"
            return "depth=\(node.depth); parent_role=\(parentRole); role=\(role); labels=\(nodeLabels)"
        }.prefix(24)
        print("DIRECT_AX_RESULT status=control-selection-error; operation=\(operation); role=\(wantedRole); name=\(controlName); matches=\(controls.count); candidate_roles=\(candidates); locations=\(Array(locationNodes)); row_candidates=\(Array(rowNodes))")
        exit(8)
    }
    let control = controlNode.element
    if operation == "observe-element" {
        let role = string(control, "AXRole") ?? ""
        var rawActions: CFArray?
        let actionsError = AXUIElementCopyActionNames(control, &rawActions)
        let actions = rawActions as? [String] ?? []
        guard role == "AXTextField", actionsError == .success, actions.contains("AXConfirm") else {
            print("DIRECT_AX_RESULT status=target-element-error; operation=observe-element; " +
                  "name=\(titleOrDescription(control)); role=\(role); actions_error=\(actionsError.rawValue); actions=\(actions)")
            exit(24)
        }
        var selectedSettable = DarwinBoolean(false)
        let selectedSettableError = AXUIElementIsAttributeSettable(control, "AXSelected" as CFString, &selectedSettable)
        var focusedSettable = DarwinBoolean(false)
        let focusedSettableError = AXUIElementIsAttributeSettable(control, "AXFocused" as CFString, &focusedSettable)
        var selectionOwners: [String] = []
        var parentIndex = controlNode.parent
        while let index = parentIndex, selectionOwners.count < 6 {
            let parentNode = descendants[index]
            let parent = parentNode.element
            let parentRole = string(parent, "AXRole") ?? ""
            if parentRole == "AXList" || parentRole == "AXBrowser" {
                var selectedChildrenSettable = DarwinBoolean(false)
                let selectedChildrenSettableError = AXUIElementIsAttributeSettable(
                    parent, "AXSelectedChildren" as CFString, &selectedChildrenSettable
                )
                var selectedChildren: CFTypeRef?
                let selectedChildrenError = AXUIElementCopyAttributeValue(
                    parent, "AXSelectedChildren" as CFString, &selectedChildren
                )
                let selectedChildrenValue = selectedChildrenError == .success
                    ? String(describing: selectedChildren)
                    : "AXError:\(selectedChildrenError.rawValue)"
                selectionOwners.append(
                    "role=\(parentRole); selected_children=\(selectedChildrenValue); " +
                    "selected_children_settable=\(selectedChildrenSettableError.rawValue):\(selectedChildrenSettable.boolValue)"
                )
            }
            parentIndex = parentNode.parent
        }
        print("DIRECT_AX_RESULT status=target-element-observed; operation=observe-element; " +
              "target_pid=\(targetPID); role=AXTextField; name=\(titleOrDescription(control)); " +
              "selected=\(described(control, "AXSelected")); focused=\(described(control, "AXFocused")); " +
              "selected_settable=\(selectedSettableError.rawValue):\(selectedSettable.boolValue); " +
              "focused_settable=\(focusedSettableError.rawValue):\(focusedSettable.boolValue); " +
              "selection_owners=\(selectionOwners); advertised_action=AXConfirm")
        exit(0)
    }
    if operation == "confirm-element" {
        var confirmActions: CFArray?
        let confirmActionsError = AXUIElementCopyActionNames(control, &confirmActions)
        let availableConfirmActions = confirmActions as? [String] ?? []
        guard (string(control, "AXRole") ?? "") == "AXTextField",
              confirmActionsError == .success, availableConfirmActions.contains("AXConfirm") else {
            print("DIRECT_AX_RESULT status=confirm-element-error; reason=exact-text-field-does-not-advertise-AXConfirm; " +
                  "name=\(titleOrDescription(control)); actions_error=\(confirmActionsError.rawValue); actions=\(availableConfirmActions)")
            exit(12)
        }
        AXUIElementSetMessagingTimeout(control, 1.0)
        print("DIRECT_AX_DISPATCH attempted=true; operation=confirm-element; target_pid=\(targetPID); " +
              "role=AXTextField; name=\(titleOrDescription(control)); advertised_action=AXConfirm")
        fflush(stdout)
        let confirmError = AXUIElementPerformAction(control, "AXConfirm" as CFString)
        print("DIRECT_AX_RESULT status=confirm-element-action-returned; operation=confirm-element; " +
              "role=AXTextField; name=\(titleOrDescription(control)); ax_error=\(confirmError.rawValue)")
        exit(confirmError == .success ? 0 : 13)
    }
    if operation == "set-value" {
        let setError = AXUIElementSetAttributeValue(control, "AXValue" as CFString, controlValue as CFString)
        let observedValue = string(control, "AXValue") ?? "<unavailable>"
        let verified = setError == .success && observedValue == controlValue
        print("DIRECT_AX_RESULT status=\(verified ? "value-verified" : "value-error"); operation=set-value; name=\(titleOrDescription(control)); expected=\(controlValue); observed=\(observedValue); ax_error=\(setError.rawValue)")
        exit(verified ? 0 : 9)
    }
    if operation == "set-folder-path-value" {
        let fieldEnabled = (value(control, "AXEnabled") as? Bool) ?? false
        guard fieldEnabled else {
            print("DIRECT_AX_RESULT status=folder-path-error; reason=field-disabled")
            exit(18)
        }
        let setError = AXUIElementSetAttributeValue(control, "AXValue" as CFString, controlValue as CFString)
        let observedValue = string(control, "AXValue") ?? "<unavailable>"
        guard setError == .success, observedValue == controlValue else {
            print("DIRECT_AX_RESULT status=folder-path-error; expected=\(controlValue); observed=\(observedValue); set_error=\(setError.rawValue)")
            exit(18)
        }
        var focusedSettable = DarwinBoolean(false)
        let focusedSettableError = AXUIElementIsAttributeSettable(control, "AXFocused" as CFString, &focusedSettable)
        var focused = (value(control, "AXFocused") as? Bool) == true
        var focusSetError: Int32 = 0
        if !focused {
            guard focusedSettableError == .success, focusedSettable.boolValue else {
                print("DIRECT_AX_RESULT status=folder-path-error; reason=field-not-focused-and-not-settable; focused_settable_error=\(focusedSettableError.rawValue)")
                exit(19)
            }
            focusSetError = AXUIElementSetAttributeValue(control, "AXFocused" as CFString, kCFBooleanTrue).rawValue
            focused = (value(control, "AXFocused") as? Bool) == true
        }
        guard focused else {
            print("DIRECT_AX_RESULT status=folder-path-error; reason=focus-not-confirmed; focus_set_error=\(focusSetError)")
            exit(19)
        }
        print("DIRECT_AX_RESULT status=folder-path-ready; value=\(observedValue); focused=true; " +
              "set_error=\(setError.rawValue); focus_settable_error=\(focusedSettableError.rawValue); " +
              "focus_set_error=\(focusSetError)")
        exit(0)
    }
    if operation == "set-focused-value-confirm" {
        let fieldEnabled = (value(control, "AXEnabled") as? Bool) ?? false
        guard fieldEnabled else {
            print("DIRECT_AX_RESULT status=focused-field-error; operation=\(operation); name=\(titleOrDescription(control)); reason=field-disabled")
            exit(18)
        }
        let setError = AXUIElementSetAttributeValue(control, "AXValue" as CFString, controlValue as CFString)
        let observedValue = string(control, "AXValue") ?? "<unavailable>"
        guard setError == .success, observedValue == controlValue else {
            print("DIRECT_AX_RESULT status=focused-field-error; operation=\(operation); name=\(titleOrDescription(control)); expected=\(controlValue); observed=\(observedValue); ax_error=\(setError.rawValue)")
            exit(18)
        }
        var fieldActions: CFArray?
        let fieldActionError = AXUIElementCopyActionNames(control, &fieldActions)
        let availableActions = fieldActions as? [String] ?? []
        guard fieldActionError == .success, availableActions.contains("AXConfirm") else {
            print("DIRECT_AX_RESULT status=focused-field-error; operation=confirm; name=\(titleOrDescription(control)); ax_error=\(fieldActionError.rawValue); actions=\(availableActions)")
            exit(19)
        }
        print("DIRECT_AX_DISPATCH attempted=true; operation=confirm-field-value; target_pid=\(targetPID); name=\(titleOrDescription(control)); field_value_verified=true; operation_kind=\(operation)")
        fflush(stdout)
        let confirmError = AXUIElementPerformAction(control, "AXConfirm" as CFString)
        print("DIRECT_AX_RESULT status=field-value-confirmed; name=\(titleOrDescription(control)); value=\(observedValue); set_error=\(setError.rawValue); action_error=\(confirmError.rawValue)")
        exit(confirmError == .success ? 0 : 20)
    }
    if operation == "select-element" {
        let role = string(control, "AXRole") ?? ""
        var rawActions: CFArray?
        let actionsError = AXUIElementCopyActionNames(control, &rawActions)
        let availableActions = rawActions as? [String] ?? []
        var selectedSettable = DarwinBoolean(false)
        let selectedSettableError = AXUIElementIsAttributeSettable(control, "AXSelected" as CFString, &selectedSettable)
        var rawAttributeNames: CFArray?
        let attributeNamesError = AXUIElementCopyAttributeNames(control, &rawAttributeNames)
        var parentSummary: [String] = []
        var parentIndex = controlNode.parent
        while let index = parentIndex, parentSummary.count < 6 {
            let parent = descendants[index]
            let parentRole = string(parent.element, "AXRole") ?? "<unavailable>"
            let parentName = titleOrDescription(parent.element)
            parentSummary.append("\(parentRole):\(parentName)")
            parentIndex = parent.parent
        }
        let browser = descendants.first { string($0.element, "AXRole") == "AXBrowser" }?.element
        let browserSelectedSettable: String
        var browserSelectedChildren: String = "<unavailable>"
        if let browser {
            var settable = DarwinBoolean(false)
            let settableError = AXUIElementIsAttributeSettable(browser, "AXSelectedChildren" as CFString, &settable)
            var rawSelectedChildren: CFTypeRef?
            let selectedChildrenError = AXUIElementCopyAttributeValue(browser, "AXSelectedChildren" as CFString, &rawSelectedChildren)
            if selectedChildrenError == .success {
                browserSelectedChildren = String(describing: rawSelectedChildren)
            } else {
                browserSelectedChildren = "AXError:\(selectedChildrenError.rawValue)"
            }
            browserSelectedSettable = "\(settableError.rawValue):\(settable.boolValue)"
        } else {
            browserSelectedSettable = "unavailable"
        }
        var selectionTarget = control
        var selectionRole = role
        var selectionSettable = selectedSettable.boolValue
        var selectionSettableError = selectedSettableError
        if role == "AXTextField", !selectionSettable {
            var parent = controlNode.parent
            while let parentIndex = parent {
                let ancestor = descendants[parentIndex].element
                let ancestorRole = string(ancestor, "AXRole") ?? ""
                if ancestorRole == "AXRow" || ancestorRole == "AXCell" {
                    var ancestorSettable = DarwinBoolean(false)
                    selectionSettableError = AXUIElementIsAttributeSettable(
                        ancestor, "AXSelected" as CFString, &ancestorSettable
                    )
                    if selectionSettableError == .success, ancestorSettable.boolValue {
                        selectionTarget = ancestor
                        selectionRole = ancestorRole
                        selectionSettable = true
                        break
                    }
                }
                parent = descendants[parentIndex].parent
            }
        }
        var listSelectionOwner: AXUIElement?
        var listSelectionChild: AXUIElement?
        if !selectionSettable, role == "AXTextField" {
            var childIndex = controlIndex
            var parentIndex = controlNode.parent
            while let index = parentIndex {
                let ancestor = descendants[index].element
                if string(ancestor, "AXRole") == "AXList" {
                    var settable = DarwinBoolean(false)
                    let settableError = AXUIElementIsAttributeSettable(
                        ancestor, "AXSelectedChildren" as CFString, &settable
                    )
                    if settableError == .success, settable.boolValue {
                        listSelectionOwner = ancestor
                        listSelectionChild = descendants[childIndex].element
                        break
                    }
                }
                childIndex = index
                parentIndex = descendants[index].parent
            }
        }
        print("DIRECT_AX_SELECT_TARGET role=\(role); name=\(titleOrDescription(control)); " +
              "attributes_error=\(attributeNamesError.rawValue); attributes=\(rawAttributeNames as? [String] ?? []); " +
              "actions_error=\(actionsError.rawValue); actions=\(rawActions as? [String] ?? []); " +
              "selected=\(described(control, "AXSelected")); selected_settable=\(selectedSettableError.rawValue):\(selectedSettable.boolValue); " +
              "selection_target_role=\(selectionRole); selection_target_settable=\(selectionSettableError.rawValue):\(selectionSettable); " +
              "parents=\(parentSummary); browser_selected_children=\(browserSelectedChildren); " +
              "browser_selected_children_settable=\(browserSelectedSettable)")
        fflush(stdout)
        if let listSelectionOwner, let listSelectionChild {
            AXUIElementSetMessagingTimeout(listSelectionOwner, 1.0)
            let selectionError = AXUIElementSetAttributeValue(
                listSelectionOwner,
                "AXSelectedChildren" as CFString,
                [listSelectionChild] as CFArray
            )
            var rawSelectedChildren: CFTypeRef?
            let selectedChildrenError = AXUIElementCopyAttributeValue(
                listSelectionOwner, "AXSelectedChildren" as CFString, &rawSelectedChildren
            )
            let selectedChildren = rawSelectedChildren as? [AXUIElement] ?? []
            let selected = selectedChildren.contains { CFEqual($0, listSelectionChild) }
            let verified = selectedChildrenError == .success && selected
            print("DIRECT_AX_RESULT status=\(verified ? "selection-verified" : "selection-error"); " +
                  "operation=select-element; role=\(role); name=\(titleOrDescription(control)); " +
                  "ax_error=\(selectionError.rawValue); selected=\(selected); " +
                  "selection_method=list-selected-children; owner_role=AXList; " +
                  "selected_children_error=\(selectedChildrenError.rawValue)")
            exit(verified ? 0 : 12)
        }
        if !selectionSettable, role == "AXTextField", availableActions.contains("AXOpen") {
            AXUIElementSetMessagingTimeout(control, 1.0)
            print("DIRECT_AX_DISPATCH attempted=true; operation=open-file; target_pid=\(targetPID); " +
                  "role=\(role); name=\(titleOrDescription(control)); advertised_action=AXOpen")
            fflush(stdout)
            let openError = AXUIElementPerformAction(control, "AXOpen" as CFString)
            print("DIRECT_AX_RESULT status=open-file-action-returned; operation=open-file; " +
                  "role=\(role); name=\(titleOrDescription(control)); ax_error=\(openError.rawValue)")
            exit(openError == .success ? 0 : 13)
        }
        guard selectionSettable else {
            print("DIRECT_AX_RESULT status=selection-error; operation=select-element; name=\(titleOrDescription(control)); " +
                  "reason=no-settable-row-or-cell-ancestor; selected_settable_error=\(selectionSettableError.rawValue)")
            exit(12)
        }
        AXUIElementSetMessagingTimeout(selectionTarget, 1.0)
        let selectError = AXUIElementSetAttributeValue(selectionTarget, "AXSelected" as CFString, kCFBooleanTrue)
        let selected = (value(selectionTarget, "AXSelected") as? Bool) ?? false
        let verified = selectError == .success && selected
        print("DIRECT_AX_RESULT status=\(verified ? "selection-verified" : "selection-error"); operation=select-element; " +
              "role=\(selectionRole); name=\(titleOrDescription(control)); ax_error=\(selectError.rawValue); selected=\(selected)")
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


def parse_window_activation(output: str, expected_pid: int, expected_title_fragment: str) -> str:
    """Require the intended process window to be the one AX reports as focused."""
    results = [line for line in output.splitlines() if line.startswith("DIRECT_AX_RESULT status=window-focused;")]
    if len(results) != 1:
        raise ValueError(f"expected one focused-window result, got {len(results)}")
    fields = dict(part.split("=", 1) for part in results[0].split("; ") if "=" in part)
    try:
        result_pid = int(fields["pid"])
        target_title = fields["target_title"]
        focused_title = fields["focused_title"]
        raise_error = int(fields["ax_raise_error"])
        focus_error = int(fields["focus_error"])
    except (KeyError, ValueError) as error:
        raise ValueError(f"malformed focused-window result: {results[0]!r}") from error
    if (
        result_pid != expected_pid
        or not expected_title_fragment
        or expected_title_fragment not in target_title
        or focused_title != target_title
        or raise_error != 0
        or focus_error != 0
    ):
        raise ValueError(f"focused-window identity or action mismatch: {results[0]!r}")
    return target_title
