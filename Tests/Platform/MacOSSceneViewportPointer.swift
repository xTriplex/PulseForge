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
    fail("usage: helper <drag-left|press-right|move-right|release-right> PID ACTION_ID coordinates...", 2)
}

let mode = CommandLine.arguments[1]
let isCoordinatePair = mode == "drag-left" || mode == "move-right"
let expectedCount = isCoordinatePair ? 8 : 6
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
if isCoordinatePair {
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

guard ["drag-left", "press-right", "move-right", "release-right"].contains(mode) else {
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
