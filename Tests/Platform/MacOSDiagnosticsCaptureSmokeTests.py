"""Deterministic contract checks for the macOS diagnostics smoke trace validator."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
