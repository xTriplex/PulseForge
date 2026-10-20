#!/usr/bin/env python3
"""Test bounded dirty-close automation bookkeeping without a macOS desktop."""

from __future__ import annotations

from collections import deque
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import Mock, patch

import MacOSDocumentCloseSmoke as smoke
import MacOSBundleSmoke as bundle
import MacOSAutomationClient as automation
import MacOSDirectAX as direct_ax
import MacOSNativeDialogAutomation as native_dialog
import MacOSScreenshot as screenshot
import MacOSWorkspaceRestoreSmoke as workspace_restore
import MacOSDocumentHostSmoke as document_host


class FakeProcess:
    def __init__(self, returncode: int | None = None):
        self.returncode = returncode

    def poll(self) -> int | None:
        return self.returncode


class FakeCapture:
    def __init__(self, events: list[dict[str, str]] | None = None):
        self.events: queue.Queue[dict[str, str]] = queue.Queue(maxsize=128)
        for event in events or []:
            self.events.put_nowait(event)
        self.pending: deque[dict[str, str]] = deque()
        self.queue_overflow_error: str | None = None
        self.control_overflow_error: str | None = None
        self.reader_error: str | None = None
        self.log_path = Path("synthetic-dirty-close.log")
        self.telemetry_dropped = 0

    def watermark(self) -> int:
        return 4

    def recent_telemetry(self, expected: dict[str, str]) -> list[dict[str, str]]:
        return []


def marker(kind: str, **fields: str) -> dict[str, str]:
    return {"kind": kind, "_capture_index": "1", "raw": f"PF_DOC_CLOSE_UI|{kind}", **fields}


class DirtyCloseProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.protocol = smoke.MarkerProtocol("cancel-save", "host-a", "asset-a", 0)

    def test_native_window_activation_requires_the_expected_focused_window(self) -> None:
        output = (
            "DIRECT_AX_RESULT status=window-focused; pid=4321; target_title=Host Test Fixture 1; "
            "focused_title=Host Test Fixture 1; ax_raise_error=0; focus_error=0; attempts=2\n"
        )
        self.assertEqual(
            "Host Test Fixture 1", direct_ax.parse_window_activation(output, 4321, "Host Test Fixture 1")
        )
        with self.assertRaisesRegex(ValueError, "identity or action mismatch"):
            direct_ax.parse_window_activation(output.replace("focused_title=Host Test Fixture 1", "focused_title=PulseForge Editor"), 4321, "Host Test Fixture 1")
        with self.assertRaisesRegex(ValueError, "expected one focused-window result"):
            direct_ax.parse_window_activation("DIRECT_AX_RESULT status=window-focus-timeout; pid=4321\n", 4321, "Host Test Fixture 1")

    def test_open_panel_readiness_requires_exact_enabled_control_and_folder_state(self) -> None:
        output = (
            "DIRECT_AX_NODE index=12; parent=2; depth=3; role=AXButton; subrole=; name=Open; enabled=true; focused=0\n"
            "DIRECT_AX_NODE index=13; parent=2; depth=3; role=AXButton; subrole=; name=Cancel; enabled=true; focused=0\n"
        )
        self.assertTrue(native_dialog.ax_node_enabled(output, "AXButton", "Open"))
        self.assertFalse(native_dialog.ax_node_enabled(output, "AXButton", "Save"))
        self.assertFalse(native_dialog.ax_node_enabled(output.replace("enabled=true", "enabled=false", 1), "AXButton", "Open"))
        self.assertFalse(native_dialog.go_folder_sheet_closed_in_expected_directory(
            "DIRECT_AX_NODE index=1; role=AXSheet; name=Go to Folder", "Dialog Files With Spaces Ω"
        ))
        self.assertTrue(native_dialog.go_folder_sheet_closed_in_expected_directory(
            "DIRECT_AX_NODE index=1; role=AXPopUpButton; name=Dialog Files With Spaces Ω", "Dialog Files With Spaces Ω"
        ))
        self.assertFalse(native_dialog.go_to_folder_path_field_visible(
            "DIRECT_AX_NODE index=1; role=AXTextField; name=PathTextField; enabled=true; focused=false"
        ))
        self.assertTrue(native_dialog.go_to_folder_path_field_visible(
            "DIRECT_AX_WINDOW pid=42; title=Open\n"
            "DIRECT_AX_NODE index=1; role=AXSheet; name=GoToWindow; enabled=true\n"
            "DIRECT_AX_NODE index=2; role=AXTextField; name=PathTextField; enabled=true; focused=false"
        ))
        self.assertFalse(native_dialog.go_to_folder_path_field_visible(
            "DIRECT_AX_NODE index=1; role=AXSheet; name=GoToWindow\n"
            "DIRECT_AX_NODE index=2; role=AXTextField; name=Search; enabled=true; focused=true"
        ))
        self.assertFalse(native_dialog.go_to_folder_path_field_visible(
            "DIRECT_AX_NODE index=1; role=AXSheet; name=GoToWindow\n"
            "DIRECT_AX_NODE index=2; role=AXTextField; name=PathTextField; enabled=false; focused=true"
        ))

    def test_permission_bridge_class_exposes_only_scalar_no_argument_methods(self) -> None:
        platform_dir = Path(__file__).parent
        header = (platform_dir / "MacOSAutomationPermissionBridge.h").read_text(encoding="utf-8")
        metadata = ET.parse(platform_dir / "PFAutomationPermission.bridgesupport").getroot()
        expected = (
            "probeSystemEventsDescriptor",
            "readSystemEventsNativeAddress",
            "typeWildCardEventClass",
            "typeWildCardEventID",
            "determineSystemEventsPermission",
            "preflightQuartzEventAccess",
        )

        self.assertEqual("signatures", metadata.tag)
        self.assertEqual("1.0", metadata.attrib.get("version"))
        self.assertIn("@interface PFAutomationPermissionBridge : NSObject", header)
        bridge_classes = [entry for entry in metadata.findall("class") if entry.attrib.get("name") == "PFAutomationPermissionBridge"]
        self.assertEqual(1, len(bridge_classes))
        self.assertEqual(len(expected), len(bridge_classes[0].findall("method")))
        for method in expected:
            self.assertIn(f"- (int32_t){method};", header)
            metadata_method = next(item for item in bridge_classes[0].findall("method") if item.attrib.get("selector") == method)
            self.assertNotEqual("true", metadata_method.attrib.get("class_method"))
            retval = metadata_method.find("retval")
            return_types = [retval.attrib[key] for key in ("type", "type64") if key in retval.attrib]
            self.assertTrue(return_types)
            self.assertTrue(all(return_type == "i" for return_type in return_types))
            self.assertEqual([], metadata_method.findall("arg"))
        self.assertEqual(len(expected), header.count("- (int32_t)"))

    def test_probe_failure_report_preserves_nested_loader_diagnostics(self) -> None:
        result = {
            "stage": "custom-framework-binary-load",
            "message": "NSBundle could not load the private permission framework",
            "frameworkPath": "/tmp/PFAutomationPermission.framework",
            "executablePath": "/tmp/PFAutomationPermission.framework/PFAutomationPermission",
            "hostExecutablePath": "/usr/bin/osascript",
            "hostTranslated": "0",
            "nativeError": {
                "domain": "NSCocoaErrorDomain",
                "code": "3587",
                "description": "Bundle is not loadable",
                "failureReason": "Architecture mismatch",
                "debugDescription": "have arm64, need arm64e",
                "underlying": {"domain": "NSPOSIXErrorDomain", "code": "86"},
            },
        }

        detail = automation._format_probe_implementation_error(result)
        parsed = json.loads(detail.split("; diagnostic=", 1)[1])

        self.assertEqual("custom-framework-binary-load", parsed["stage"])
        self.assertEqual("3587", parsed["nativeError"]["code"])
        self.assertEqual("have arm64, need arm64e", parsed["nativeError"]["debugDescription"])
        self.assertEqual("86", parsed["nativeError"]["underlying"]["code"])

    def test_permission_probe_launches_system_events_with_a_bounded_noninteractive_request(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()
            result = {
                "stage": "complete", "descriptorType": 1, "wildcardClass": 2, "wildcardEvent": 2,
                "appleEventsStatus": 0, "systemEventsTargetRunning": True, "axTrusted": True, "quartzAllowed": True,
            }

            class Probe:
                pid = 129
                returncode = 0

                @staticmethod
                def communicate(timeout: float) -> tuple[str, None]:
                    return json.dumps(result), None

            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "run", return_value=Mock(returncode=0, stderr="")
            ) as launch_target, patch.object(automation.subprocess, "Popen", return_value=Probe()):
                state, _, detail = automation.probe_osascript_access()

        self.assertEqual("OSASCRIPT_AX_APPLE_EVENTS_AUTHORIZED", state)
        self.assertIn("SystemEventsTargetRunning=true", detail)
        launch_target.assert_called_once_with(
            ["/usr/bin/open", "-gj", "-b", "com.apple.systemevents"],
            capture_output=True,
            text=True,
            timeout=3.0,
            check=False,
        )

    def test_system_events_launch_timeout_fails_without_claiming_permission_denial(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()
            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "run", side_effect=subprocess.TimeoutExpired(["/usr/bin/open"], 3.0)
            ), patch.object(automation.subprocess, "Popen") as launch_probe:
                state, pid, detail = automation.probe_osascript_access()

        self.assertEqual("PROBE_TIMED_OUT", state)
        self.assertIsNone(pid)
        self.assertIn("System Events launch request timed out", detail)
        launch_probe.assert_not_called()

    def test_system_events_launch_failure_is_target_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()
            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "run", return_value=Mock(returncode=1, stderr="application unavailable")
            ) as launch_target, patch.object(automation.subprocess, "Popen") as launch_probe:
                state, pid, detail = automation.probe_osascript_access()

        self.assertEqual("APPLE_EVENTS_TARGET_UNAVAILABLE", state)
        self.assertIsNone(pid)
        self.assertIn("could not be started", detail)
        launch_target.assert_called_once()
        launch_probe.assert_not_called()

    def test_decision_pointer_telemetry_before_prompt_does_not_hide_response(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            script = (
                "print('PF_DOC_CLOSE_UI|DECISION_POINTER|scenario=cancel-save|host=host-a|asset=asset-a|"
                "generation=1|choice=cancel|sample=1|hovered=0|clicked=0')\n"
                "print('PF_DOC_CLOSE_UI|PROMPT|scenario=cancel-save|host=host-a|asset=asset-a|generation=1|sample=1')\n"
            )
            process = subprocess.Popen(
                [sys.executable, "-c", script],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            capture = smoke.BoundedCapture(process, Path(temporary) / "telemetry-before-prompt.log")
            process.wait(timeout=5)
            capture.thread.join(timeout=5)
            self.assertTrue(capture.reader_finished.is_set())

            prompt = smoke.wait_event(
                capture,
                FakeProcess(),
                "PROMPT",
                "cancel-save",
                time.monotonic() + 1.0,
                after_index=0,
            )
            self.assertEqual("PROMPT", prompt["kind"])
            self.assertEqual(1, len(capture.recent_telemetry({"scenario": "cancel-save", "host": "host-a"})))
            self.assertEqual(1, self.protocol.accept_prompt(prompt))

    def test_tab_delivery_diagnostics_are_retained_as_bounded_telemetry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            script = (
                "print('PF_DOC_CLOSE_DIAG|TAB_INPUT|scenario=close-retry|host=host-a|asset=asset-a|"
                "generation=0|sample=1|viewport=10|mouse_viewport=10|rect=100,100,200,130|mouse=190,115|"
                "native_known=1|native_focused=1|native_hovered=1|tab_hovered=1|close_hovered=1|active=1|"
                "down=1|clicked=1|released=0|pressed=0')\n"
                "print('PF_DOC_CLOSE_DIAG|TAB_CLOSE_REQUEST|scenario=close-retry|host=host-a|asset=asset-a|"
                "generation=0|pending_before=0|pending_host=|open=1|dirty=1|value=42|saved=10')\n"
            )
            process = subprocess.Popen(
                [sys.executable, "-c", script],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            capture = smoke.BoundedCapture(process, Path(temporary) / "tab-diagnostics.log")
            process.wait(timeout=5)
            capture.thread.join(timeout=5)
            expected = {"scenario": "close-retry", "host": "host-a", "asset": "asset-a"}
            telemetry = capture.recent_telemetry(expected)
            self.assertEqual(["DIAG_TAB_INPUT", "DIAG_TAB_CLOSE_REQUEST"], [event["kind"] for event in telemetry])
            self.assertIn("close_hovered=1", telemetry[0]["raw"])

    def test_cocoa_observer_startup_diagnostic_does_not_block_ready_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            script = (
                "print('PF_DOC_CLOSE_DIAG|COCOA_LOCAL_MONITOR|scenario=cancel-save|host=host-a|asset=asset-a|"
                "started=1|pid=123')\n"
                "print('PF_DOC_CLOSE_UI|READY|scenario=cancel-save|host=host-a|asset=asset-a|generation=0')\n"
            )
            process = subprocess.Popen(
                [sys.executable, "-c", script],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            capture = smoke.BoundedCapture(process, Path(temporary) / "cocoa-observer-startup.log")
            process.wait(timeout=5)
            capture.thread.join(timeout=5)

            ready = smoke.wait_event(
                capture,
                FakeProcess(),
                "READY",
                "cancel-save",
                time.monotonic() + 1.0,
                after_index=0,
            )
            self.assertEqual("READY", ready["kind"])
            self.assertEqual(
                ["DIAG_COCOA_LOCAL_MONITOR"],
                [event["kind"] for event in capture.recent_telemetry({"scenario": "cancel-save"})],
            )

    def test_save_press_or_release_telemetry_without_correlated_action_still_fails(self) -> None:
        cases = (
            (
                "press-without-release",
                "PF_DOC_CLOSE_DIAG|GLFW_MOUSE_BUTTON|scenario=save-retry|host=host-a|asset=asset-a|generation=1|sequence=1|"
                "viewport=7|main_window=0|button=left|action=press|modifiers=0|local=55,20|steady_ms=1.0|native_focused=1|native_hovered=1",
                "PF_DOC_CLOSE_DIAG|DECISION_HEARTBEAT|scenario=save-retry|host=host-a|asset=asset-a|generation=1|choice=save|"
                "frame=100|phase=awaiting-first-action|hovered=1|active=1|filtered_down=1|raw_down=1|raw_clicked=1|raw_released=0|"
                "pressed=0|native_focused=1|native_hovered=1|window_viewport=7|mouse_viewport=7",
            ),
            (
                "release-without-action",
                "PF_DOC_CLOSE_DIAG|GLFW_MOUSE_BUTTON|scenario=save-retry|host=host-a|asset=asset-a|generation=1|sequence=2|"
                "viewport=7|main_window=0|button=left|action=release|modifiers=0|local=55,20|steady_ms=2.0|native_focused=1|native_hovered=1",
                "PF_DOC_CLOSE_DIAG|DECISION_HEARTBEAT|scenario=save-retry|host=host-a|asset=asset-a|generation=1|choice=save|"
                "frame=101|phase=awaiting-first-action|hovered=1|active=0|filtered_down=0|raw_down=0|raw_clicked=0|raw_released=1|"
                "pressed=0|native_focused=1|native_hovered=1|window_viewport=7|mouse_viewport=7",
            ),
        )
        for name, button_event, heartbeat in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                script = f"print({button_event!r})\nprint({heartbeat!r})\n"
                child = subprocess.Popen(
                    [sys.executable, "-c", script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
                )
                capture = smoke.BoundedCapture(child, Path(temporary) / f"{name}.log")
                child.wait(timeout=5)
                capture.thread.join(timeout=5)
                telemetry = capture.recent_telemetry({"scenario": "save-retry", "generation": "1"})
                self.assertEqual(["DIAG_GLFW_MOUSE_BUTTON", "DIAG_DECISION_HEARTBEAT"], [item["kind"] for item in telemetry])

                evidence = smoke.ActionEvidence(label="Save", dispatch_attempted=True, api_exit_code=0)
                with self.assertRaisesRegex(RuntimeError, "Close response missing"):
                    smoke.await_application_event(
                        capture,
                        FakeProcess(),
                        "ACTION",
                        "save-retry",
                        time.monotonic() + 1.0,
                        evidence,
                        lambda _event: None,
                        after_index=0,
                        response_seconds=0.03,
                        missing_message="Close response missing",
                        diagnose=lambda *_: "release telemetry did not include the semantic ACTION",
                    )
                self.assertEqual("MISSING", evidence.application_response)

    def test_cancel_press_and_release_are_retained_with_prompt_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = (
                "PF_DOC_CLOSE_DIAG|GLFW_MOUSE_BUTTON|scenario=cancel-save|host=host-a|asset=asset-a|generation=3|"
                "sequence=1|viewport=7|main_window=0|native_window=0x1234|button=left|action=press|modifiers=0|"
                "local=55,20|steady_ms=1.0|native_focused=1|native_hovered=1\n"
                "PF_DOC_CLOSE_DIAG|GLFW_MOUSE_BUTTON|scenario=cancel-save|host=host-a|asset=asset-a|generation=3|"
                "sequence=2|viewport=7|main_window=0|native_window=0x1234|button=left|action=release|modifiers=0|"
                "local=55,20|steady_ms=2.0|native_focused=1|native_hovered=1\n"
                "PF_DOC_CLOSE_DIAG|DECISION_HEARTBEAT|scenario=cancel-save|host=host-a|asset=asset-a|generation=3|"
                "choice=cancel|frame=101|phase=awaiting-first-action|hovered=1|active=0|filtered_down=0|raw_down=0|"
                "raw_clicked=0|raw_released=1|pressed=0|native_focused=1|native_hovered=1|window_viewport=7|mouse_viewport=7\n"
            )
            child = subprocess.Popen(
                [sys.executable, "-c", f"print({output!r}, end='')"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            capture = smoke.BoundedCapture(child, Path(temporary) / "cancel-pointer-diagnostics.log")
            child.wait(timeout=5)
            capture.thread.join(timeout=5)
            telemetry = capture.recent_telemetry(
                {"scenario": "cancel-save", "host": "host-a", "asset": "asset-a", "generation": "3"}
            )
            self.assertEqual(
                ["DIAG_GLFW_MOUSE_BUTTON", "DIAG_GLFW_MOUSE_BUTTON", "DIAG_DECISION_HEARTBEAT"],
                [event["kind"] for event in telemetry],
            )
            self.assertEqual("press", telemetry[0]["action"])
            self.assertEqual("release", telemetry[1]["action"])
            self.assertEqual("0x1234", telemetry[1]["native_window"])
            self.assertEqual("1", telemetry[2]["raw_released"])

    def test_osascript_probe_checks_its_own_accessibility_appleevents_and_quartz_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()
            result = {
                "stage": "complete",
                "descriptorType": 1936289382,
                "wildcardClass": 707406378,
                "wildcardEvent": 707406378,
                "appleEventsStatus": 0,
                "systemEventsTargetRunning": True,
                "axTrusted": True,
                "quartzAllowed": False,
            }

            class Probe:
                pid = 123
                returncode = 0

                @staticmethod
                def communicate(timeout: float) -> tuple[str, None]:
                    return json.dumps(result), None

            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ) as launch:
                state, pid, detail = automation.probe_osascript_access(prepare_target=False)

        self.assertEqual("OSASCRIPT_AX_APPLE_EVENTS_AUTHORIZED", state)
        self.assertEqual(123, pid)
        self.assertIn("caller=/usr/bin/osascript", detail)
        self.assertIn("AEDeterminePermissionToAutomateTarget", detail)
        self.assertIn("CGPreflightPostEventAccess=false", detail)
        self.assertIn("determineSystemEventsPermission", launch.call_args.args[0][-1])
        self.assertEqual("/usr/bin/osascript", launch.call_args.args[0][0])

    def test_osascript_probe_classifies_appleevents_denial_separately_from_other_privileges(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()
            result = {
                "stage": "complete", "descriptorType": 1, "wildcardClass": 2, "wildcardEvent": 2,
                "appleEventsStatus": -1743, "axTrusted": True, "quartzAllowed": True,
                "systemEventsTargetRunning": True,
            }

            class Probe:
                pid = 124
                returncode = 0

                @staticmethod
                def communicate(timeout: float) -> tuple[str, None]:
                    return json.dumps(result), None

            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ):
                state, pid, detail = automation.probe_osascript_access(prepare_target=False)

        self.assertEqual("APPLE_EVENTS_DENIED", state)
        self.assertEqual(124, pid)
        self.assertIn("AXIsProcessTrusted=true", detail)
        self.assertIn("CGPreflightPostEventAccess=true", detail)

    def test_osascript_probe_reports_accessibility_denial_and_keeps_quartz_separate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()
            result = {
                "stage": "complete", "descriptorType": 1, "wildcardClass": 2, "wildcardEvent": 2,
                "appleEventsStatus": 0, "axTrusted": False, "quartzAllowed": False,
                "systemEventsTargetRunning": True,
            }

            class Probe:
                pid = 1241
                returncode = 0

                @staticmethod
                def communicate(timeout: float) -> tuple[str, None]:
                    return json.dumps(result), None

            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ):
                state, _, detail = automation.probe_osascript_access(prepare_target=False)

        self.assertEqual("AX_UNTRUSTED", state)
        self.assertIn("CGPreflightPostEventAccess=false", detail)

    def test_osascript_preflight_distinguishes_consent_target_and_native_bridge_errors(self) -> None:
        cases = (
            (-1744, "APPLE_EVENTS_CONSENT_REQUIRED"),
            (-600, "APPLE_EVENTS_TARGET_UNAVAILABLE"),
            (-32003, "APPLE_EVENTS_UNSUPPORTED"),
            (-32001, "PROBE_IMPLEMENTATION_ERROR"),
        )
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()
            for status, expected in cases:
                result = {
                    "stage": "complete", "descriptorType": 1, "wildcardClass": 2, "wildcardEvent": 2,
                    "appleEventsStatus": status, "axTrusted": True, "quartzAllowed": True,
                    "systemEventsTargetRunning": status != -600,
                }

                class Probe:
                    pid = 125
                    returncode = 0

                    @staticmethod
                    def communicate(timeout: float) -> tuple[str, None]:
                        return json.dumps(result), None

                with self.subTest(status=status), patch.dict(
                    os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}
                ), patch.object(automation.subprocess, "Popen", return_value=Probe()):
                    state, _, _ = automation.probe_osascript_access(prepare_target=False)
                    self.assertEqual(expected, state)

    def test_osascript_probe_rejects_malformed_or_failed_bridge_invocation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()

            class Probe:
                pid = 126
                returncode = 0

                @staticmethod
                def communicate(timeout: float) -> tuple[str, None]:
                    return "execution error: Ref has incompatible type (-2700)", None

            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ):
                state, _, detail = automation.probe_osascript_access(prepare_target=False)
            self.assertEqual("PROBE_IMPLEMENTATION_ERROR", state)
            self.assertIn("invalid probe serialization", detail)

            Probe.returncode = 1
            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ):
                state, _, detail = automation.probe_osascript_access(prepare_target=False)
            self.assertEqual("PROBE_IMPLEMENTATION_ERROR", state)
            self.assertIn("exit_code=1", detail)

            Probe.returncode = 0
            Probe.communicate = staticmethod(lambda timeout: (json.dumps({
                "stage": "native-AEAddressDesc-access", "error": "Ref has incompatible type (-2700)"
            }), None))
            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ):
                state, _, detail = automation.probe_osascript_access(prepare_target=False)
            self.assertEqual("PROBE_IMPLEMENTATION_ERROR", state)
            self.assertIn("native-AEAddressDesc-access", detail)
            self.assertIn("-2700", detail)

    def test_osascript_probe_timeout_is_reported_and_owned_child_is_reaped(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()

            class Probe:
                pid = 127
                returncode = -9
                communicate = Mock(side_effect=[
                    subprocess.TimeoutExpired(["/usr/bin/osascript"], 4), ("", None)
                ])

            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ), patch.object(automation.os, "killpg") as kill_group:
                state, pid, detail = automation.probe_osascript_access(prepare_target=False)

        self.assertEqual("PROBE_TIMED_OUT", state)
        self.assertEqual(127, pid)
        kill_group.assert_called_once_with(127, automation.signal.SIGKILL)
        self.assertEqual(2, Probe.communicate.call_count)
        self.assertIn("osascript", detail)

    def test_osascript_probe_reports_cleanup_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bridge = Path(temporary) / "PFAutomationPermission.framework"
            bridge.mkdir()

            class Probe:
                pid = 128
                returncode = None
                communicate = Mock(side_effect=[
                    subprocess.TimeoutExpired(["/usr/bin/osascript"], 4),
                    subprocess.TimeoutExpired(["/usr/bin/osascript"], 2),
                    subprocess.TimeoutExpired(["/usr/bin/osascript"], 1),
                ])

                @staticmethod
                def kill() -> None:
                    pass

            with patch.dict(os.environ, {"PULSEFORGE_MACOS_AUTOMATION_BRIDGE": str(bridge)}), patch.object(
                automation.subprocess, "Popen", return_value=Probe()
            ), patch.object(automation.os, "killpg"):
                state, pid, detail = automation.probe_osascript_access(prepare_target=False)

        self.assertEqual("PROBE_CLEANUP_FAILED", state)
        self.assertEqual(128, pid)
        self.assertIn("did not exit", detail)


    def test_prompt_response_reconciles_uncertain_dispatch_when_pointer_probe_timed_out(self) -> None:
        capture = FakeCapture(
            [marker("PROMPT", scenario="cancel-save", host="host-a", asset="asset-a", generation="1", sample="1")]
        )
        evidence = smoke.ActionEvidence(
            label="close tab",
            dispatch_attempted=True,
            api_exit_code=124,
            api_result="Quartz call timed out",
            transport_uncertain=True,
            pointer_observation="TIMED_OUT",
        )
        diagnostic_calls: list[str] = []
        response = smoke.await_application_event(
            capture,
            FakeProcess(),
            "PROMPT",
            "cancel-save",
            time.monotonic() + 1.0,
            evidence,
            self.protocol.accept_prompt,
            after_index=0,
            diagnose=lambda *_: diagnostic_calls.append("unexpected diagnostic") or "pointer observation unavailable",
        )
        self.assertEqual("PROMPT", response["kind"])
        self.assertEqual("PASS_TRANSPORT_UNCERTAIN_RECONCILED", evidence.semantic_outcome)
        self.assertEqual(1, self.protocol.generation)
        self.assertEqual([], diagnostic_calls, "a confirmed response must not depend on optional pointer diagnostics")

    def test_click_dispatch_does_not_depend_on_repeated_accessibility_bounds_query(self) -> None:
        class Process:
            pid = 42

            @staticmethod
            def poll() -> None:
                return None

        capture = FakeCapture()
        activation_output = (
            "DIRECT_AX_RESULT status=window-focused; pid=42; target_title=Host Test Fixture 1; "
            "focused_title=Host Test Fixture 1; ax_raise_error=0; focus_error=0; attempts=1\n"
        )
        with patch.object(smoke, "run_direct_ax", return_value=(0, activation_output, "")) as activate, patch.object(
            smoke, "run_bounded_command", return_value=(0, "Quartz event posted", "")
        ) as dispatch:
            evidence = smoke.click_point(
                Process(), capture, Path("pointer-driver"), None, (50, 75), time.monotonic() + 5.0, "Cancel"
            )

        self.assertTrue(evidence.dispatch_attempted)
        self.assertEqual(0, evidence.api_exit_code)
        activate.assert_called_once()
        self.assertEqual(42, activate.call_args.args[0])
        self.assertEqual("activate-window", activate.call_args.kwargs["operation"])
        self.assertEqual(smoke.FIXTURE_TITLE, activate.call_args.kwargs["window_title"])
        dispatch.assert_called_once()
        self.assertEqual(5, len(dispatch.call_args.args[0]))
        self.assertEqual(evidence.action_id, dispatch.call_args.args[0][2])

    def test_malformed_accessibility_bounds_are_diagnostic_only(self) -> None:
        class Process:
            pid = 42

        with patch.object(
            smoke,
            "run_osascript",
            return_value=(0, f"PulseForge\n{smoke.FIXTURE_TITLE}\nnot-bounds"),
        ):
            with patch("builtins.print") as diagnostic:
                bounds = smoke.query_fixture_window(Process(), time.monotonic() + 5.0)

        self.assertIsNone(bounds)
        self.assertIn("category=MALFORMED", diagnostic.call_args.args[0])

    def test_missing_response_fails_within_action_bound(self) -> None:
        evidence = smoke.ActionEvidence(label="Cancel", dispatch_attempted=True, api_exit_code=0)
        started = time.monotonic()
        with self.assertRaisesRegex(RuntimeError, "expected application response missing"):
            smoke.await_application_event(
                FakeCapture(),
                FakeProcess(),
                "ACTION",
                "cancel-save",
                started + 1.0,
                evidence,
                lambda _event: None,
                after_index=0,
                response_seconds=0.03,
                diagnose=lambda *_: "no pointer result",
            )
        self.assertLess(time.monotonic() - started, 0.25)
        self.assertEqual("MISSING", evidence.application_response)

    def test_explicit_dispatch_failure_without_response_fails_immediately(self) -> None:
        evidence = smoke.ActionEvidence(
            label="Discard",
            dispatch_attempted=True,
            api_exit_code=77,
            api_result="TCC_DENIED: event posting is not authorized",
        )
        started = time.monotonic()
        with self.assertRaisesRegex(RuntimeError, "dispatch API failed"):
            smoke.await_application_event(
                FakeCapture(),
                FakeProcess(),
                "ACTION",
                "cancel-save",
                started + 5.0,
                evidence,
                lambda _event: None,
                after_index=0,
                diagnose=lambda *_: "diagnostic only",
            )
        self.assertLess(time.monotonic() - started, 0.25)
        self.assertEqual("FAIL_DISPATCH_RESULT", evidence.semantic_outcome)

    def test_identity_generation_and_duplicate_sequence_are_rejected(self) -> None:
        self.protocol.accept_prompt(
            marker("PROMPT", scenario="cancel-save", host="host-a", asset="asset-a", generation="1", sample="1")
        )
        action = marker(
            "ACTION",
            scenario="cancel-save",
            host="host-a",
            asset="asset-a",
            generation="1",
            sequence="1",
            choice="cancel",
        )
        self.protocol.accept_action(action, "cancel", 1)
        with self.assertRaisesRegex(RuntimeError, "identity changed"):
            self.protocol.check_identity({**action, "host": "other-host"})
        with self.assertRaisesRegex(RuntimeError, "generation"):
            self.protocol.accept_action({**action, "generation": "0", "sequence": "2"}, "cancel", 1)
        with self.assertRaisesRegex(RuntimeError, "sequence did not advance exactly once"):
            self.protocol.accept_action(action, "cancel", 1)

    def test_action_sequence_gap_is_rejected(self) -> None:
        self.protocol.accept_prompt(
            marker("PROMPT", scenario="cancel-save", host="host-a", asset="asset-a", generation="1", sample="1")
        )
        action = marker(
            "ACTION",
            scenario="cancel-save",
            host="host-a",
            asset="asset-a",
            generation="1",
            sequence="2",
            choice="cancel",
        )
        with self.assertRaisesRegex(RuntimeError, "sequence did not advance exactly once"):
            self.protocol.accept_action(action, "cancel", 1)

    def test_uncertain_action_can_reconcile_to_clean_exit_but_not_running_process(self) -> None:
        evidence = smoke.ActionEvidence(
            label="Save",
            dispatch_attempted=True,
            api_exit_code=124,
            api_result="AXPress timed out",
            transport_uncertain=True,
        )
        self.assertEqual("PASS_TRANSPORT_UNCERTAIN_RECONCILED", smoke.reconcile_action_response(evidence, True))
        smoke.verify_successful_exit(0, {"kind": "PASS", "scenario": "cancel-save"})
        with self.assertRaisesRegex(RuntimeError, "remains open"):
            smoke.verify_successful_exit(None, {"kind": "PASS", "scenario": "cancel-save"})

    def test_success_marker_does_not_hide_child_crash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            process = subprocess.Popen(
                [sys.executable, "-c", "print('PF_DOC_CLOSE_UI|PASS|scenario=cancel-save'); raise SystemExit(6)"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            capture = smoke.BoundedCapture(process, Path(temporary) / "child.log")
            process.wait(timeout=5)
            capture.thread.join(timeout=5)
            event = smoke.wait_event(capture, process, "PASS", "cancel-save", time.monotonic() + 0.2)
            self.assertEqual("PASS", event["kind"])
            with self.assertRaisesRegex(RuntimeError, "status 6"):
                smoke.verify_successful_exit(process.returncode, event)

    def test_required_marker_after_prefix_truncation_remains_available(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(smoke, "MAX_LOG_BYTES", 128):
                script = "import sys; print('x' * 600); print('PF_DOC_CLOSE_UI|PASS|scenario=cancel-save')"
                process = subprocess.Popen(
                    [sys.executable, "-c", script],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                capture = smoke.BoundedCapture(process, Path(temporary) / "truncated.log")
                process.wait(timeout=5)
                capture.thread.join(timeout=5)
                self.assertTrue(capture.log_truncated)
                event = smoke.wait_event(capture, process, "PASS", "cancel-save", time.monotonic() + 0.2)
                self.assertIn("PASS", event["raw"])

    def test_bundle_smoke_tracks_late_required_marker_after_log_truncation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker_text = "PULSEFORGE_EDITOR_PROJECT_RENDER_SMOKE_SHUTDOWN=PASS"
            script = f"import sys; print('x' * 400000); print({marker_text!r})"
            with patch.object(bundle, "MAX_LOG_BYTES", 256):
                result = bundle.run_bounded(
                    [sys.executable, "-c", script],
                    root,
                    dict(__import__("os").environ),
                    5,
                    root / "bundle.log",
                    (marker_text,),
                )
            self.assertTrue(result.truncated)
            self.assertIn(marker_text, result.seen_markers)
            bundle.verify_output("synthetic bundle child", result, (marker_text,))

    def test_bundle_smoke_rejects_validation_error_after_log_truncation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker_text = "PULSEFORGE_EDITOR_PROJECT_RENDER_SMOKE_SHUTDOWN=PASS"
            script = f"import sys; print('x' * 400000); print('VUID-vk-test-00001'); print({marker_text!r})"
            with patch.object(bundle, "MAX_LOG_BYTES", 256):
                result = bundle.run_bounded(
                    [sys.executable, "-c", script],
                    root,
                    dict(__import__("os").environ),
                    5,
                    root / "bundle-validation-error.log",
                    (marker_text,),
                )
            with self.assertRaisesRegex(RuntimeError, "validation errors"):
                bundle.verify_output("synthetic bundle child", result, (marker_text,))


    def test_capture_uses_runner_temp_and_verifies_nonempty_image(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            def fake_capture(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
                self.assertEqual("/usr/sbin/screencapture", command[0])
                self.assertEqual("-x", command[1])
                self.assertEqual("-C", command[2])
                Path(command[-1]).write_bytes(bytes((137, 80, 78, 71, 13, 10, 26, 10)) + b"image")
                self.assertEqual(2.0, kwargs["timeout"])
                return subprocess.CompletedProcess(command, 0, "", "")

            with patch.dict(os.environ, {"RUNNER_TEMP": temporary}), patch.object(
                screenshot.subprocess, "run", side_effect=fake_capture
            ):
                result = screenshot.capture_screenshot("macos-test-checkpoint-loaded.png")

            self.assertTrue(result.startswith("CAPTURED; bytes="))
            self.assertTrue((Path(temporary) / "macos-test-checkpoint-loaded.png").is_file())

    def test_capture_failure_is_reported_without_claiming_an_image(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            failed = subprocess.CompletedProcess(
                ["/usr/sbin/screencapture"], 1, "", "screen recording not authorized"
            )
            with patch.dict(os.environ, {"RUNNER_TEMP": temporary}), patch.object(
                screenshot.subprocess, "run", return_value=failed
            ):
                result = screenshot.capture_screenshot("macos-test-checkpoint-panel.png")

            self.assertTrue(result.startswith("TCC_DENIED;"))
            self.assertFalse((Path(temporary) / "macos-test-checkpoint-panel.png").exists())

    def test_semantic_queue_overflow_is_sticky_and_reader_keeps_draining(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            script = (
                "import sys; "
                "[print('PF_DOC_CLOSE_UI|NOISE|scenario=cancel-save|n=' + str(i)) for i in range(140)]; "
                "print('TAIL_DRAINED')"
            )
            process = subprocess.Popen(
                [sys.executable, "-c", script],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            capture = smoke.BoundedCapture(process, Path(temporary) / "overflow.log")
            process.wait(timeout=5)
            capture.thread.join(timeout=5)
            self.assertIsNotNone(capture.queue_overflow_error)
            self.assertEqual(141, capture.lines_read)
            self.assertTrue(capture.reader_finished.is_set())
            with self.assertRaisesRegex(RuntimeError, "semantic-marker queue overflow"):
                smoke.wait_event(
                    capture,
                    FakeProcess(),
                    "PASS",
                    "cancel-save",
                    time.monotonic() + 0.2,
                )

    def test_output_reader_failure_is_explicit(self) -> None:
        class BrokenStream:
            def __iter__(self):
                raise OSError("synthetic pipe read failure")

        class BrokenProcess:
            stdout = BrokenStream()

        with tempfile.TemporaryDirectory() as temporary:
            capture = smoke.BoundedCapture(BrokenProcess(), Path(temporary) / "reader-error.log")  # type: ignore[arg-type]
            capture.thread.join(timeout=2)
            self.assertIn("synthetic pipe read failure", capture.reader_error or "")
            self.assertTrue(capture.reader_finished.is_set())
            with self.assertRaisesRegex(RuntimeError, "output capture failed"):
                smoke.wait_event(
                    capture,
                    FakeProcess(),
                    "PASS",
                    "cancel-save",
                    time.monotonic() + 0.2,
                )


class WorkspaceRestoreObservationTests(unittest.TestCase):
    class Process:
        pid = 876
        returncode: int | None = None

        def poll(self) -> int | None:
            return self.returncode

    class Capture:
        restored_sessions = 2
        reader_error: str | None = None

        def __init__(self) -> None:
            self.fixture_ready = threading.Event()
            self.fixture_ready.set()

    class Clock:
        def __init__(self) -> None:
            self.now = 0.0

        def monotonic(self) -> float:
            return self.now

        def sleep(self, seconds: float) -> None:
            self.now += seconds

    VALID_WINDOWS = "PulseForge Editor\n1\tPulseForge Editor\n2\tHost Test Fixture 1\n3\tHost Test Fixture 2\n"

    def run_wait(self, observations: list[tuple[int, str]], deadline_seconds: float = 5.0) -> tuple[list[tuple[int, str]], int]:
        clock = self.Clock()
        capture = self.Capture()
        with patch.object(workspace_restore.time, "monotonic", clock.monotonic), patch.object(
            workspace_restore.time, "sleep", clock.sleep
        ), patch.object(workspace_restore, "run_osascript", side_effect=observations) as query:
            windows = workspace_restore.wait_for_fixture_windows(
                self.Process(), capture, deadline_seconds, expected_restore_count=2
            )
        return windows, query.call_count

    def test_transient_timeout_then_native_snapshot_succeeds_under_one_deadline(self) -> None:
        windows, attempts = self.run_wait([(124, "AppleScript operation timed out"), (0, self.VALID_WINDOWS)])
        self.assertEqual(3, len(windows))
        self.assertEqual(2, attempts)

    def test_persistent_timeout_fails_with_bounded_attempt_diagnostics(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "persistent Accessibility observation timeout.*attempts=3"):
            self.run_wait([(124, "AppleScript operation timed out")] * 3)

    def test_explicit_authorization_denial_fails_without_retry(self) -> None:
        with patch.object(
            workspace_restore, "run_osascript", return_value=(1, "not authorized to send Apple events (-1743)")
        ) as query:
            with self.assertRaisesRegex(workspace_restore.WindowObservationError, "AX_AUTHORIZATION_DENIED"):
                workspace_restore.query_windows(self.Process(), time.monotonic() + 2.0)
        query.assert_called_once()

    def test_editor_exit_and_wrong_process_identity_fail_immediately(self) -> None:
        process = self.Process()
        process.returncode = 3
        with self.assertRaisesRegex(workspace_restore.WindowObservationError, "EDITOR_EXITED"):
            workspace_restore.query_windows(process, time.monotonic() + 2.0)
        with patch.object(workspace_restore, "run_osascript", return_value=(0, "Finder\n1\tPulseForge Editor\n")):
            with self.assertRaisesRegex(workspace_restore.WindowObservationError, "AX_WRONG_PROCESS_IDENTITY"):
                workspace_restore.query_windows(self.Process(), time.monotonic() + 2.0)
        with patch.object(workspace_restore, "run_osascript", return_value=(0, "PulseForge\nmalformed window row")):
            with self.assertRaisesRegex(workspace_restore.WindowObservationError, "AX_MALFORMED_PROTOCOL"):
                workspace_restore.query_windows(self.Process(), time.monotonic() + 2.0)

    def test_editor_output_reader_failure_fails_before_accepting_windows(self) -> None:
        capture = self.Capture()
        capture.reader_error = "synthetic reader failure"
        with patch.object(workspace_restore, "run_osascript", return_value=(0, self.VALID_WINDOWS)) as query:
            with self.assertRaisesRegex(RuntimeError, "output capture failed"):
                workspace_restore.wait_for_fixture_windows(self.Process(), capture, time.monotonic() + 1.0, 2)
        query.assert_not_called()

    def test_restoration_markers_and_incomplete_window_set_do_not_pass(self) -> None:
        incomplete = "PulseForge Editor\n1\tPulseForge Editor\n2\tHost Test Fixture 1\n"
        clock = self.Clock()
        with patch.object(workspace_restore.time, "monotonic", clock.monotonic), patch.object(
            workspace_restore.time, "sleep", clock.sleep
        ), patch.object(workspace_restore, "run_osascript", return_value=(0, incomplete)):
            with self.assertRaisesRegex(RuntimeError, "overall readiness deadline expired"):
                workspace_restore.wait_for_fixture_windows(self.Process(), self.Capture(), 1.0, 2)


class DocumentHostAXWindowCountTests(unittest.TestCase):
    def test_accepts_only_a_single_count_for_the_expected_editor_process(self) -> None:
        output = (
            "DIRECT_AX_TARGET_PROCESS pid=4321; name=PulseForge Editor; bundle=example.editor\n"
            "DIRECT_AX_RESULT status=window-count; pid=4321; windows=3; elapsed_ms=2.5\n"
        )
        self.assertEqual(3, document_host.parse_ax_window_count(output, 4321))

    def test_rejects_wrong_process_malformed_and_duplicate_count_results(self) -> None:
        with self.assertRaisesRegex(ValueError, "identity/value mismatch"):
            document_host.parse_ax_window_count(
                "DIRECT_AX_RESULT status=window-count; pid=4322; windows=3; elapsed_ms=2\n", 4321
            )
        with self.assertRaisesRegex(ValueError, "malformed"):
            document_host.parse_ax_window_count("DIRECT_AX_RESULT status=window-count; pid=x; windows=3\n", 4321)
        with self.assertRaisesRegex(ValueError, "got 2"):
            document_host.parse_ax_window_count(
                "DIRECT_AX_RESULT status=window-count; pid=4321; windows=3\n"
                "DIRECT_AX_RESULT status=window-count; pid=4321; windows=3\n",
                4321,
            )


if __name__ == "__main__":
    unittest.main()
