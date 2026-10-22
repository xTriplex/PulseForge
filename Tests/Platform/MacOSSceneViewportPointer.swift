import AppKit
import CoreGraphics
import Darwin
import Foundation

func fail(_ message: String, _ status: Int32) -> Never {
    fputs(message + "\n", stderr)
    exit(status)
}

func parsePoint(_ xText: String, _ yText: String) -> CGPoint? {
    guard let x = Double(xText), let y = Double(yText), x.isFinite, y.isFinite else { return nil }
    return CGPoint(x: x, y: y)
}

guard CommandLine.arguments.count >= 2 else {
    fail("usage: helper <display-geometry|drag-left|press-right|move-right|release-right|scroll|key-hold> arguments...", 2)
}

let mode = CommandLine.arguments[1]
if mode == "display-geometry" {
    guard CommandLine.arguments.count == 2 else {
        fail("display-geometry accepts no additional arguments; no events were posted", 2)
    }
    var displayCount: UInt32 = 0
    guard CGGetActiveDisplayList(0, nil, &displayCount) == .success, displayCount > 0 else {
        fail("could not enumerate active display geometry", 3)
    }
    var displayIDs = [CGDirectDisplayID](repeating: 0, count: Int(displayCount))
    guard CGGetActiveDisplayList(displayCount, &displayIDs, &displayCount) == .success else {
        fail("could not read active display identifiers", 3)
    }
    let screensByDisplayID: [UInt32: NSScreen] = Dictionary(uniqueKeysWithValues: NSScreen.screens.compactMap { screen in
        guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return nil }
        return (number.uint32Value, screen)
    })
    let displays: [[String: Any]] = displayIDs.prefix(Int(displayCount)).map { displayID in
        let bounds = CGDisplayBounds(displayID)
        let screen = screensByDisplayID[displayID]
        return [
            "id": Int(displayID),
            "x": Double(bounds.origin.x),
            "y": Double(bounds.origin.y),
            "width": Double(bounds.size.width),
            "height": Double(bounds.size.height),
            "backing_scale": Double(screen?.backingScaleFactor ?? 1.0),
            "coordinate_space": "Quartz global display coordinates",
            "origin": "upper-left of main display",
            "scale_applied": false
        ]
    }
    guard let data = try? JSONSerialization.data(withJSONObject: displays, options: [.sortedKeys]),
          let json = String(data: data, encoding: .utf8) else {
        fail("could not serialize active display geometry", 3)
    }
    print("SCENE_VIEWPORT_DISPLAY_GEOMETRY=\(json)")
    exit(0)
}
let isCoordinatePair = mode == "drag-left" || mode == "move-right"
let isScroll = mode == "scroll"
let isKeyHold = mode == "key-hold"
let expectedCount = isCoordinatePair ? 8 : (isScroll ? 7 : 6)
guard CommandLine.arguments.count == expectedCount else {
    fail("malformed \(mode) argument count; no events were posted", 2)
}
guard let targetPID = Int32(CommandLine.arguments[2]), targetPID > 0 else {
    fail("target PID must be a positive 32-bit integer; no events were posted", 2)
}
let actionID = CommandLine.arguments[3]
guard actionID.range(of: "^[0-9a-fA-F]{32}$", options: .regularExpression) != nil,
      let eventTag = UInt64(actionID.prefix(16), radix: 16) else {
    fail("action identity must contain exactly 32 hexadecimal characters; no events were posted", 2)
}

let start: CGPoint
let end: CGPoint
if isKeyHold {
    guard let keyCode = UInt16(CommandLine.arguments[4]), keyCode <= 127,
          let duration = Double(CommandLine.arguments[5]), duration.isFinite, duration >= 0.05, duration <= 2.0 else {
        fail("key-hold requires a virtual key code in 0...127 and duration in 0.05...2.0; no events were posted", 2)
    }
    start = .zero
    end = CGPoint(x: CGFloat(keyCode), y: duration)
} else if isScroll {
    guard let point = parsePoint(CommandLine.arguments[4], CommandLine.arguments[5]),
          let delta = Int32(CommandLine.arguments[6]), delta != 0, abs(delta) <= 10 else {
        fail("scroll requires finite coordinates and a nonzero line delta within 10; no events were posted", 2)
    }
    start = point
    end = CGPoint(x: CGFloat(delta), y: 0)
} else if isCoordinatePair {
    guard let parsedStart = parsePoint(CommandLine.arguments[4], CommandLine.arguments[5]),
          let parsedEnd = parsePoint(CommandLine.arguments[6], CommandLine.arguments[7]) else {
        fail("drag coordinates must be finite numbers; no events were posted", 2)
    }
    start = parsedStart
    end = parsedEnd
} else {
    guard let point = parsePoint(CommandLine.arguments[4], CommandLine.arguments[5]) else {
        fail("event coordinates must be finite numbers; no events were posted", 2)
    }
    start = point
    end = point
}

guard ["drag-left", "click-left", "press-right", "move-right", "release-right", "scroll", "key-hold"].contains(mode) else {
    fail("unsupported pointer operation; no events were posted", 2)
}
guard CGPreflightPostEventAccess() else {
    fail("TCC_DENIED: CGPreflightPostEventAccess rejected Scene Viewport pointer input", 77)
}
guard NSWorkspace.shared.frontmostApplication?.processIdentifier == targetPID else {
    fail("the PulseForge editor process is not frontmost; no pointer events were posted", 4)
}
guard let source = CGEventSource(stateID: .hidSystemState) else {
    fail("CGEventSource creation failed; no pointer events were posted", 3)
}

let isRightButton = mode == "press-right" || mode == "move-right" || mode == "release-right"
let button: CGMouseButton = isRightButton ? .right : .left
func post(_ type: CGEventType, _ point: CGPoint, _ state: Int64, _ name: String) {
    guard let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point, mouseButton: button) else {
        fail("CGEvent creation failed for \(name)", 3)
    }
    event.setIntegerValueField(.mouseEventClickState, value: state)
    event.setIntegerValueField(.eventSourceUserData, value: Int64(truncatingIfNeeded: eventTag))
    let attempt = ProcessInfo.processInfo.systemUptime
    event.post(tap: .cghidEventTap)
    print("SCENE_VIEWPORT_CG_EVENT=POSTED; action_id=\(actionID); target_pid=\(targetPID); source_pid=\(getpid()); " +
          "event=\(name); point=\(Int(point.x.rounded())),\(Int(point.y.rounded())); click_state=\(state); " +
          "attempt_uptime=\(attempt); route=global-hid-event-stream; post_return_is_not_application_acknowledgement")
}

switch mode {
case "key-hold":
    let keyCode = CGKeyCode(UInt16(Int(end.x)))
    guard let down = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: true),
          let up = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: false) else {
        fail("CGEvent creation failed for key-hold; no keyboard events were posted", 3)
    }
    down.setIntegerValueField(.eventSourceUserData, value: Int64(truncatingIfNeeded: eventTag))
    up.setIntegerValueField(.eventSourceUserData, value: Int64(truncatingIfNeeded: eventTag))
    down.post(tap: .cghidEventTap)
    print("SCENE_VIEWPORT_CG_EVENT=POSTED; action_id=\(actionID); target_pid=\(targetPID); source_pid=\(getpid()); event=key-down; keycode=\(keyCode); route=global-hid-event-stream; post_return_is_not_application_acknowledgement")
    Thread.sleep(forTimeInterval: Double(end.y))
    up.post(tap: .cghidEventTap)
    print("SCENE_VIEWPORT_CG_EVENT=POSTED; action_id=\(actionID); target_pid=\(targetPID); source_pid=\(getpid()); event=key-up; keycode=\(keyCode); route=global-hid-event-stream; post_return_is_not_application_acknowledgement")
case "scroll":
    guard let event = CGEvent(scrollWheelEvent2Source: source, units: .line, wheelCount: 1,
                              wheel1: Int32(end.x), wheel2: 0, wheel3: 0) else {
        fail("CGEvent creation failed for scroll; no event was posted", 3)
    }
    event.location = start
    event.setIntegerValueField(.eventSourceUserData, value: Int64(truncatingIfNeeded: eventTag))
    event.post(tap: .cghidEventTap)
    print("SCENE_VIEWPORT_CG_EVENT=POSTED; action_id=\(actionID); target_pid=\(targetPID); source_pid=\(getpid()); event=scroll; point=\(Int(start.x.rounded())),\(Int(start.y.rounded())); delta=\(Int(end.x)); route=global-hid-event-stream; post_return_is_not_application_acknowledgement")
case "click-left":
    post(.mouseMoved, start, 0, "left-move-to-click")
    Thread.sleep(forTimeInterval: 0.05)
    post(.leftMouseDown, start, 1, "left-down")
    Thread.sleep(forTimeInterval: 0.05)
    post(.leftMouseUp, start, 1, "left-up")
case "drag-left":
    let down: CGEventType = button == .left ? .leftMouseDown : .rightMouseDown
    let moved: CGEventType = button == .left ? .leftMouseDragged : .rightMouseDragged
    let up: CGEventType = button == .left ? .leftMouseUp : .rightMouseUp
    let name = button == .left ? "left" : "right"
    post(.mouseMoved, start, 0, "\(name)-move-to-start")
    Thread.sleep(forTimeInterval: 0.05)
    post(down, start, 1, "\(name)-down")
    for index in 1...12 {
        Thread.sleep(forTimeInterval: 0.03)
        let amount = CGFloat(index) / 12.0
        let point = CGPoint(x: start.x + (end.x - start.x) * amount, y: start.y + (end.y - start.y) * amount)
        post(moved, point, 1, "\(name)-drag-\(index)")
    }
    Thread.sleep(forTimeInterval: 0.05)
    post(up, end, 1, "\(name)-up")
case "move-right":
    for index in 1...12 {
        Thread.sleep(forTimeInterval: 0.03)
        let amount = CGFloat(index) / 12.0
        let point = CGPoint(x: start.x + (end.x - start.x) * amount, y: start.y + (end.y - start.y) * amount)
        post(.rightMouseDragged, point, 1, "right-drag-\(index)")
    }
    Thread.sleep(forTimeInterval: 0.05)
    post(.rightMouseUp, end, 1, "right-up")
case "press-right":
    post(.mouseMoved, start, 0, "right-move-to-start")
    Thread.sleep(forTimeInterval: 0.05)
    post(.rightMouseDown, start, 1, "right-down")
case "release-right":
    post(.rightMouseUp, start, 1, "right-up-cleanup")
default:
    fail("unsupported pointer operation; no events were posted", 2)
}

print("SCENE_VIEWPORT_CG_EVENT=SEQUENCE_RETURNED; action_id=\(actionID); target_pid=\(targetPID); source_pid=\(getpid()); " +
      "operation=\(mode); posting_result_is_transport_evidence_only")
