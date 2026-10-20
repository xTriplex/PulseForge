#!/usr/bin/env python3
"""Test bounded dirty-close automation bookkeeping without a macOS desktop."""

from __future__ import annotations

from collections import deque
import os
import queue
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import MacOSDocumentCloseSmoke as smoke
import MacOSBundleSmoke as bundle
import MacOSAutomationClient as automation
import MacOSScreenshot as screenshot


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

    def test_osascript_access_probe_checks_ax_trust_without_attempting_apple_events(self) -> None:
        class Probe:
            pid = 123
            returncode = 0

            @staticmethod
            def communicate(timeout: float) -> tuple[str, None]:
                return "AX=true\n", None

        with patch.object(automation.subprocess, "Popen", return_value=Probe()) as launch:
            state, pid, detail = automation.probe_osascript_access()

        self.assertEqual("AX_TRUSTED", state)
        self.assertEqual(123, pid)
        self.assertIn("Apple Events permission is checked by each bounded System Events operation", detail)
        self.assertNotIn("AEDeterminePermissionToAutomateTarget", launch.call_args.args[0][-1])

    def test_osascript_access_probe_distinguishes_ax_trust_for_its_own_client(self) -> None:
        class Probe:
            pid = 124
            returncode = 0

            @staticmethod
            def communicate(timeout: float) -> tuple[str, None]:
                return "AX=false\n", None

        with patch.object(automation.subprocess, "Popen", return_value=Probe()):
            state, pid, detail = automation.probe_osascript_access()

        self.assertEqual("AX_UNTRUSTED", state)
        self.assertEqual(124, pid)
        self.assertIn("AXIsProcessTrusted=false", detail)

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
        with patch.object(smoke, "run_osascript", return_value=(0, "focus pass")) as focus, patch.object(
            smoke, "run_bounded_command", return_value=(0, "Quartz event posted", "")
        ) as dispatch:
            evidence = smoke.click_point(
                Process(), capture, Path("pointer-driver"), None, (50, 75), time.monotonic() + 5.0, "Cancel"
            )

        self.assertTrue(evidence.dispatch_attempted)
        self.assertEqual(0, evidence.api_exit_code)
        focus.assert_called_once()
        self.assertEqual(smoke.FOCUS_TARGET, focus.call_args.args[0])
        dispatch.assert_called_once()

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


if __name__ == "__main__":
    unittest.main()
