"""Run a bounded diagnostics-enabled editor workload and validate its exported trace."""

from __future__ import annotations

import argparse
import json
import math
import os
import signal
import subprocess
import sys
import time
import tempfile
from pathlib import Path

PANEL_READY = "PULSEFORGE_DIAGNOSTICS_PERFORMANCE_PANEL=READY;"
SMOKE_PASS = "PULSEFORGE_DIAGNOSTICS_SMOKE=PASS;"


def validate_panel_ready(output: str) -> dict[str, str]:
    lines = [line for line in output.splitlines() if PANEL_READY in line]
    if len(lines) != 1:
        raise ValueError(f"expected one visible Performance panel observation, got {len(lines)}")
    fields = {
        key.strip(): value.strip()
        for item in lines[0].split(PANEL_READY, 1)[1].split("; ")
        if "=" in item
        for key, value in [item.split("=", 1)]
    }
    try:
        owner_viewport = int(fields["owner_viewport"])
        rectangle = tuple(float(value) for value in fields["rect"].split(","))
    except (KeyError, ValueError) as error:
        raise ValueError("visible Performance panel observation has invalid owner or geometry") from error
    if (fields.get("utility_window") != "1" or fields.get("capture_controls") != "1" or owner_viewport <= 0
            or len(rectangle) != 4 or not all(math.isfinite(value) for value in rectangle)
            or rectangle[2] < 320.0 or rectangle[3] < 240.0):
        raise ValueError("Performance panel was not visible in a usable utility window")
    return fields


def validate_trace(path: Path) -> dict[str, object]:
    document = json.loads(path.read_text(encoding="utf-8"))
    metadata = document["metadata"]
    events = document["traceEvents"]
    start = int(metadata["capture_start_monotonic_ns"])
    stop = int(metadata["capture_stop_monotonic_ns"])
    session = int(metadata["capture_id"])
    if session <= 0 or stop < start or not events:
        raise ValueError("trace has no valid, bounded capture interval or events")
    if metadata["engine_build_identifier"] in ("", "unavailable"):
        raise ValueError("trace does not identify its engine build")
    if metadata["platform"] == "unknown" or metadata["configuration"] == "unknown":
        raise ValueError("trace does not identify platform and configuration")
    if not metadata["workload"]:
        raise ValueError("trace does not identify the workload")
    if not metadata["content_fingerprint"] and not metadata["content_fingerprint_unavailable_reason"]:
        raise ValueError("trace lacks both content identity and an unavailable reason")

    names: set[str] = set()
    event_kinds: set[str] = set()
    for event in events:
        args = event.get("args", {})
        if int(args.get("session_id", -1)) != session:
            raise ValueError("trace event belongs to a different capture session")
        timestamp_us = float(event["ts"])
        if not math.isfinite(timestamp_us) or timestamp_us < 0:
            raise ValueError("trace event timestamp is not finite and nonnegative")
        names.add(str(event["name"]))
        event_kinds.add(str(event["ph"]))
        if event["ph"] == "X":
            duration_us = float(event["dur"])
            completed_ns = int(args["completion_timestamp_ns"])
            if not math.isfinite(duration_us) or duration_us < 0:
                raise ValueError("trace duration is not finite and nonnegative")
            if completed_ns > stop or (timestamp_us + duration_us) * 1000 > stop - start + 1:
                raise ValueError("completed CPU slice extends beyond the capture stop boundary")

    required = {
        "Cold project open and startup scene",
        "Warm project open and startup scene",
        "Project metadata read",
        "Project metadata deserialize",
        "Project asset registry rebuild",
        "Project startup scene validation",
        "first viewport prepared",
        "first viewport rendered",
        "Application layer update",
        "Editor Play transition",
    }
    if missing := required - names:
        raise ValueError(f"trace is missing required phases: {sorted(missing)}")
    if not {"X", "C", "i"}.issubset(event_kinds):
        raise ValueError("trace lacks duration, counter, and resource observations")

    resources = metadata.get("resources", [])
    if not resources or not any(item.get("label") == "Registered project assets" for item in resources):
        raise ValueError("trace lacks project resource accounting")
    if not any(item.get("quality") == "unavailable" and item.get("unavailable_reason")
               for item in resources):
        raise ValueError("trace does not explain an unavailable accounting category")
    if metadata["gpu_timing_supported"] and metadata["gpu_timing_enabled"]:
        if not metadata.get("gpu_timings"):
            raise ValueError("GPU timing was supported and enabled but produced no completed sample")
    return {
        "capture_id": session,
        "events": len(events),
        "resources": len(resources),
        "gpu_timing_supported": metadata["gpu_timing_supported"],
        "gpu_timing_enabled": metadata["gpu_timing_enabled"],
        "gpu_samples": len(metadata.get("gpu_timings", [])),
        "workload": metadata["workload"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--screenshot", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=float, default=240.0)
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    for path in (args.editor, args.project):
        if not path.exists():
            parser.error(f"required input does not exist: {path}")
    for path in (args.trace, args.log, args.screenshot):
        path.parent.mkdir(parents=True, exist_ok=True)

    command = [str(args.editor), "--project", str(args.project), "--diagnostics-capture-smoke", str(args.trace)]
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="pulseforge-diagnostics-settings-") as settings_directory:
        settings_path = Path(settings_directory) / "imgui.ini"
        settings_path.write_text("", encoding="utf-8")
        environment = os.environ.copy()
        environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = settings_directory
        with args.log.open("w", encoding="utf-8", buffering=1) as log_file:
            process = subprocess.Popen(
                command, stdout=log_file, stderr=subprocess.STDOUT, start_new_session=True, env=environment
            )
        screenshot_taken = False
        try:
            while process.poll() is None and time.monotonic() - started < args.timeout_seconds:
                text = args.log.read_text(encoding="utf-8", errors="replace") if args.log.exists() else ""
                if "PULSEFORGE_DIAGNOSTICS_SMOKE=FAIL;" in text:
                    raise RuntimeError("editor reported a diagnostics workload failure")
                if PANEL_READY in text and not screenshot_taken:
                    result = subprocess.run(["screencapture", "-x", str(args.screenshot)],
                                            capture_output=True, text=True, timeout=15)
                    if result.returncode != 0 or not args.screenshot.is_file() or args.screenshot.stat().st_size == 0:
                        raise RuntimeError(f"Performance panel screenshot failed: {result.stderr.strip()}")
                    screenshot_taken = True
                    print("MACOS_DIAGNOSTICS_PANEL_SCREENSHOT=CAPTURED")
                time.sleep(0.1)
            if process.poll() is None:
                raise TimeoutError("diagnostics editor did not exit before the bounded deadline")
            output = args.log.read_text(encoding="utf-8", errors="replace")
            if process.returncode != 0:
                raise RuntimeError(f"editor normal shutdown returned {process.returncode}")
            if SMOKE_PASS not in output:
                raise RuntimeError("editor exited normally without the complete diagnostics PASS marker")
            if not screenshot_taken:
                raise RuntimeError("Performance panel did not reach its ready state and produce a screenshot")
            validate_panel_ready(output)
            report = validate_trace(args.trace)
            print("MACOS_DIAGNOSTICS_APPLICATION_COMPLETION=exit_code=0; orderly_shutdown=1")
            print(f"MACOS_DIAGNOSTICS_TRACE=VALID; {json.dumps(report, sort_keys=True)}")
            return 0
        except (OSError, RuntimeError, TimeoutError, ValueError, subprocess.SubprocessError, json.JSONDecodeError) as error:
            print(f"MACOS_DIAGNOSTICS_SMOKE=FAIL; {type(error).__name__}: {error}", file=sys.stderr)
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=10)
                print("MACOS_DIAGNOSTICS_FAILURE_CLEANUP=FORCED; not acceptance evidence", file=sys.stderr)
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
