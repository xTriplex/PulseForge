"""Deterministic contract checks for the macOS diagnostics smoke trace validator."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import MacOSDiagnosticsCaptureSmoke as smoke
from MacOSDiagnosticsCaptureSmoke import validate_trace


def valid_trace() -> dict[str, object]:
    required = [
        "Cold project open and startup scene", "Warm project open and startup scene",
        "Project metadata read", "Project metadata deserialize", "Project asset registry rebuild",
        "Project startup scene validation", "first viewport prepared", "first viewport rendered",
        "Application layer update", "Editor Play transition",
    ]
    return {
        "metadata": {
            "capture_id": 5, "capture_start_monotonic_ns": 1000, "capture_stop_monotonic_ns": 5000,
            "engine_build_identifier": "test-build", "platform": "macOS", "configuration": "Debug",
            "workload": "fixture", "content_fingerprint": "fixture-hash",
            "content_fingerprint_unavailable_reason": "", "gpu_timing_supported": False,
            "gpu_timing_enabled": False, "gpu_timings": [],
            "resources": [
                {"label": "Registered project assets", "quality": "exact-payload"},
                {"label": "Derived data bytes", "quality": "unavailable", "unavailable_reason": "not present"},
            ],
        },
        "traceEvents": [
            {"name": name, "ph": "X", "ts": 0.1, "dur": 0.1,
             "args": {"session_id": 5, "completion_timestamp_ns": 1200}}
            for name in required
        ] + [
            {"name": "cache hits", "ph": "C", "ts": 0.2, "args": {"session_id": 5}},
            {"name": "resource count", "ph": "i", "ts": 0.2, "args": {"session_id": 5}},
        ],
    }


class DiagnosticsCaptureSmokeTests(unittest.TestCase):
    def check_document(self, document: dict[str, object]) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            validate_trace(path)

    def test_complete_meaningful_capture_is_accepted(self) -> None:
        self.check_document(valid_trace())

    def test_incomplete_trace_event_kinds_are_rejected(self) -> None:
        document = valid_trace()
        document["traceEvents"] = [event for event in document["traceEvents"] if event["ph"] != "i"]
        with self.assertRaisesRegex(ValueError, "duration, counter, and resource"):
            self.check_document(document)

    def test_wrong_capture_session_is_rejected(self) -> None:
        document = valid_trace()
        document["traceEvents"][0]["args"]["session_id"] = 8
        with self.assertRaisesRegex(ValueError, "different capture session"):
            self.check_document(document)

    def test_slice_past_stop_boundary_is_rejected(self) -> None:
        document = valid_trace()
        document["traceEvents"][0]["args"]["completion_timestamp_ns"] = 9000
        with self.assertRaisesRegex(ValueError, "beyond the capture stop"):
            self.check_document(document)

    def test_screenshot_followup_requires_the_exact_editor_window_to_be_focused(self) -> None:
        output = (
            "DIRECT_AX_RESULT status=window-focused; pid=4321; target_title=EditorWorkflowOverview.scene - "
            "PulseForge Editor; focused_title=EditorWorkflowOverview.scene - PulseForge Editor; "
            "ax_raise_error=0; focus_error=0; attempts=2"
        )
        with patch.object(smoke, "run_direct_ax", return_value=(0, output, "")) as activate:
            title = smoke.activate_editor_window(4321)
        activate.assert_called_once_with(
            4321, operation="activate-window", window_title="PulseForge Editor", timeout=10.0
        )
        self.assertEqual("EditorWorkflowOverview.scene - PulseForge Editor", title)

    def test_screenshot_followup_rejects_unverified_editor_activation(self) -> None:
        with patch.object(smoke, "run_direct_ax", return_value=(16, "DIRECT_AX_RESULT status=window-focus-timeout", "")):
            with self.assertRaisesRegex(RuntimeError, "could not reactivate"):
                smoke.activate_editor_window(4321)

    def test_performance_panel_ready_requires_visible_utility_geometry(self) -> None:
        marker = (
            "PULSEFORGE_DIAGNOSTICS_PERFORMANCE_PANEL=READY; utility_window=1; capture_controls=1; "
            "owner_viewport=3158883903; rect=10,20,420,300; resources=19; events=9"
        )
        self.assertEqual("3158883903", smoke.validate_panel_ready(marker)["owner_viewport"])

    def test_performance_panel_marker_without_geometry_is_rejected(self) -> None:
        marker = "PULSEFORGE_DIAGNOSTICS_PERFORMANCE_PANEL=READY; utility_window=1; capture_controls=1"
        with self.assertRaisesRegex(ValueError, "invalid owner or geometry"):
            smoke.validate_panel_ready(marker)

    def test_performance_panel_title_bar_only_geometry_is_rejected(self) -> None:
        marker = (
            "PULSEFORGE_DIAGNOSTICS_PERFORMANCE_PANEL=READY; utility_window=1; capture_controls=1; "
            "owner_viewport=3158883903; rect=60,123,32,44; resources=19; events=9"
        )
        with self.assertRaisesRegex(ValueError, "usable utility window"):
            smoke.validate_panel_ready(marker)


if __name__ == "__main__":
    unittest.main()
