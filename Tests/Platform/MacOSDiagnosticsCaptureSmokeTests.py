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
            "gpu_timing_requested": False,
            "gpu_timing_enabled": False, "gpu_timing_available": False,
            "gpu_timing_failure_reported": False,
            "gpu_timing_result_status": "no-samples", "gpu_timing_unavailable_reason": "not available",
            "gpu_timing_positive_duration_count": 0, "gpu_timing_zero_duration_count": 0,
            "snapshot_monotonic_ns": 5000, "capture_enabled_at_export": False,
            "gpu_timings": [],
            "resource_segments_in_snapshot": 1, "resource_segments_exported": 1,
            "resource_segments": [{"timestamp_monotonic_ns": 1000, "workload": "fixture",
                                    "content_fingerprint": "fixture-hash",
                                    "content_fingerprint_unavailable_reason": "", "resources": []}],
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

    def test_isolated_zero_sample_with_positive_evidence_is_accepted(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata.update({
            "gpu_timing_supported": True, "gpu_timing_enabled": True,
            "gpu_timing_requested": True,
            "gpu_timing_available": True, "gpu_timing_result_status": "informative",
            "gpu_timing_positive_duration_count": 1, "gpu_timing_zero_duration_count": 1,
            "gpu_timings": [
                {"session_id": 5, "duration_ns": 1200, "quality": "positive-duration"},
                {"session_id": 5, "duration_ns": 0, "quality": "zero-duration"},
            ],
        })
        self.check_document(document)

    def test_truthful_zero_duration_only_result_is_accepted(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata.update({
            "gpu_timing_supported": True, "gpu_timing_enabled": True,
            "gpu_timing_requested": True,
            "gpu_timing_available": False, "gpu_timing_result_status": "zero-duration-only",
            "gpu_timing_unavailable_reason": (
                "GPU queries returned zero-duration samples; they do not establish useful frame-duration evidence."
            ),
            "gpu_timing_positive_duration_count": 0, "gpu_timing_zero_duration_count": 2,
            "gpu_timings": [
                {"session_id": 5, "duration_ns": 0, "quality": "zero-duration"},
                {"session_id": 5, "duration_ns": 0, "quality": "zero-duration"},
            ],
        })
        self.check_document(document)

    def test_zero_duration_sequence_cannot_claim_informative_availability(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata.update({
            "gpu_timing_supported": True, "gpu_timing_enabled": True,
            "gpu_timing_requested": True,
            "gpu_timing_available": True, "gpu_timing_result_status": "informative",
            "gpu_timing_positive_duration_count": 0, "gpu_timing_zero_duration_count": 2,
            "gpu_timings": [
                {"session_id": 5, "duration_ns": 0, "quality": "zero-duration"},
                {"session_id": 5, "duration_ns": 0, "quality": "zero-duration"},
            ],
        })
        with self.assertRaisesRegex(ValueError, "informative GPU timing"):
            self.check_document(document)

    def test_gpu_sample_quality_must_match_duration(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata.update({
            "gpu_timing_supported": True, "gpu_timing_enabled": True,
            "gpu_timing_requested": True,
            "gpu_timing_available": True, "gpu_timing_result_status": "informative",
            "gpu_timing_positive_duration_count": 1, "gpu_timing_zero_duration_count": 0,
            "gpu_timings": [{"session_id": 5, "duration_ns": 0, "quality": "positive-duration"}],
        })
        with self.assertRaisesRegex(ValueError, "quality"):
            self.check_document(document)

    def test_timing_collection_failure_requires_explicit_failure_reason(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata.update({
            "gpu_timing_supported": True, "gpu_timing_requested": True,
            "gpu_timing_enabled": True, "gpu_timing_available": False,
            "gpu_timing_result_status": "failed", "gpu_timings": [],
        })
        with self.assertRaisesRegex(ValueError, "explicit failure and reason"):
            self.check_document(document)

    def test_supported_gpu_failure_does_not_satisfy_smoke_acceptance(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata.update({
            "gpu_timing_supported": True, "gpu_timing_requested": True,
            "gpu_timing_enabled": True, "gpu_timing_available": False,
            "gpu_timing_failure_reported": True, "gpu_timing_result_status": "failed",
            "gpu_timing_unavailable_reason": "Injected query retrieval failure",
        })
        with self.assertRaisesRegex(ValueError, "did not produce a usable or backend-limited"):
            self.check_document(document)

    def test_gpu_sample_must_name_its_capture_session(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata.update({
            "gpu_timing_supported": True, "gpu_timing_requested": True,
            "gpu_timing_enabled": True, "gpu_timing_available": True,
            "gpu_timing_result_status": "informative", "gpu_timing_positive_duration_count": 1,
            "gpu_timings": [{"duration_ns": 10, "quality": "positive-duration"}],
        })
        with self.assertRaisesRegex(ValueError, "different capture session"):
            self.check_document(document)

    def test_unknown_gpu_status_is_rejected(self) -> None:
        document = valid_trace()
        document["metadata"]["gpu_timing_result_status"] = "maybe"
        with self.assertRaisesRegex(ValueError, "missing or unknown"):
            self.check_document(document)

    def test_resource_segment_must_be_inside_capture_and_have_identity(self) -> None:
        document = valid_trace()
        document["metadata"]["resource_segments"][0]["timestamp_monotonic_ns"] = 9000
        with self.assertRaisesRegex(ValueError, "outside the capture interval"):
            self.check_document(document)
        document = valid_trace()
        document["metadata"]["resource_segments"][0]["content_fingerprint"] = ""
        with self.assertRaisesRegex(ValueError, "lacks content identity"):
            self.check_document(document)

    def test_active_export_uses_snapshot_boundary_without_claiming_stop(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata["capture_stop_monotonic_ns"] = 0
        metadata["capture_enabled_at_export"] = True
        metadata["snapshot_monotonic_ns"] = 5000
        self.check_document(document)
        metadata["snapshot_monotonic_ns"] = 1100
        with self.assertRaisesRegex(ValueError, "capture stop boundary"):
            self.check_document(document)

    def test_active_export_cannot_claim_a_stop_timestamp(self) -> None:
        document = valid_trace()
        metadata = document["metadata"]
        metadata["capture_enabled_at_export"] = True
        with self.assertRaisesRegex(ValueError, "valid, bounded capture interval"):
            self.check_document(document)

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
