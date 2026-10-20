"""Narrow diagnostics for the macOS automation clients used by desktop smoke tests."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
from pathlib import Path


def _probe_script(bridge_path: Path) -> str:
    bridge = json.dumps(str(bridge_path))
    return f'''var stage = "ApplicationServices-framework-load";
var bridgePath = {bridge};
var descriptorType = -1;
var descriptorResult = -1;
var wildcardClass = -1;
var wildcardEvent = -1;
var appleEventsStatus = -32000;
var axTrusted = false;
var quartzAllowed = false;
var systemEventsTargetRunning = false;
var serializedResult = "";
try {{
    ObjC.import("AppKit");
    ObjC.import("ApplicationServices");
    stage = "custom-framework-load";
    ObjC.import(bridgePath);

    stage = "custom-framework-symbol-invocation:PFTypeWildCardEventClass";
    wildcardClass = $.PFTypeWildCardEventClass();
    if (wildcardClass !== 707406378)
        throw new Error("BridgeSupport wildcard function returned " + wildcardClass);

    stage = "System-Events-target-readiness";
    var systemEventsApplications = $.NSRunningApplication.runningApplicationsWithBundleIdentifier("com.apple.systemevents");
    for (var attempt = 0; attempt < 20 && ObjC.unwrap(systemEventsApplications.count) === 0; ++attempt) {{
        $.NSThread.sleepForTimeInterval(0.1);
        systemEventsApplications = $.NSRunningApplication.runningApplicationsWithBundleIdentifier("com.apple.systemevents");
    }}
    systemEventsTargetRunning = ObjC.unwrap(systemEventsApplications.count) > 0;

    stage = "custom-framework-symbol-invocation:PFProbeSystemEventsDescriptor";
    descriptorResult = $.PFProbeSystemEventsDescriptor();
    if (descriptorResult !== 0) throw new Error("descriptor construction returned " + descriptorResult);

    stage = "custom-framework-symbol-invocation:PFReadSystemEventsNativeAddress";
    descriptorType = $.PFReadSystemEventsNativeAddress();
    if (descriptorType < 0) throw new Error("native address access returned " + descriptorType);

    stage = "custom-framework-symbol-invocation:PFTypeWildCardEventID";
    wildcardEvent = $.PFTypeWildCardEventID();
    if (wildcardClass !== 707406378 || wildcardClass !== wildcardEvent) throw new Error("invalid wildcard constants");

    stage = "custom-framework-symbol-invocation:PFDetermineSystemEventsPermission";
    appleEventsStatus = systemEventsTargetRunning ? $.PFDetermineSystemEventsPermission() : -600;

    stage = "accessibility-trust-query";
    axTrusted = Boolean($.AXIsProcessTrusted());

    stage = "CoreGraphics-framework-load";
    ObjC.import("CoreGraphics");
    stage = "quartz-post-event-preflight";
    quartzAllowed = Boolean($.CGPreflightPostEventAccess());

    stage = "result-serialization";
    serializedResult = JSON.stringify({{stage: "complete", descriptorType: descriptorType, wildcardClass: wildcardClass,
        wildcardEvent: wildcardEvent, appleEventsStatus: appleEventsStatus,
        systemEventsTargetRunning: systemEventsTargetRunning, axTrusted: axTrusted, quartzAllowed: quartzAllowed}});
}} catch (error) {{
    serializedResult = JSON.stringify({{stage: stage, error: String(error)}});
}}
serializedResult;'''


def probe_osascript_access(timeout_seconds: float = 7.0, *, prepare_target: bool = True) -> tuple[str, int | None, str]:
    """Nonpromptingly query the actual osascript process for its three distinct privileges."""
    bridge_value = os.environ.get("PULSEFORGE_MACOS_AUTOMATION_BRIDGE")
    if not bridge_value:
        return "PROBE_IMPLEMENTATION_ERROR", None, "PULSEFORGE_MACOS_AUTOMATION_BRIDGE is not configured"
    bridge_path = Path(bridge_value)
    if not bridge_path.is_dir():
        return "PROBE_IMPLEMENTATION_ERROR", None, f"permission bridge framework is missing: {bridge_path}"
    if prepare_target:
        try:
            target_launch = subprocess.run(
                ["/usr/bin/open", "-gj", "-b", "com.apple.systemevents"],
                capture_output=True,
                text=True,
                timeout=3.0,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            return "PROBE_TIMED_OUT", None, f"bounded System Events launch request timed out: {error}"
        except OSError as error:
            return "PROBE_IMPLEMENTATION_ERROR", None, (
                f"could not request System Events startup: {type(error).__name__}: {error}"
            )
        if target_launch.returncode != 0:
            return "APPLE_EVENTS_TARGET_UNAVAILABLE", None, (
                f"System Events could not be started for the permission query; exit_code={target_launch.returncode}; "
                f"output={target_launch.stderr.strip()[-500:]!r}"
            )
    command = ["/usr/bin/osascript", "-l", "JavaScript", "-e", _probe_script(bridge_path)]
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
    except OSError as error:
        return "PROBE_IMPLEMENTATION_ERROR", None, f"{type(error).__name__}: {error}"

    try:
        output, _ = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            output, _ = process.communicate(timeout=2.0)
        except subprocess.TimeoutExpired:
            process.kill()
            try:
                output, _ = process.communicate(timeout=1.0)
            except subprocess.TimeoutExpired:
                return "PROBE_CLEANUP_FAILED", process.pid, "osascript did not exit after SIGKILL"
        return "PROBE_TIMED_OUT", process.pid, (
            f"osascript exceeded {timeout_seconds}s and was terminated; output={output.strip()[-1000:]!r}"
        )

    if process.returncode != 0:
        return "PROBE_IMPLEMENTATION_ERROR", process.pid, f"exit_code={process.returncode}; output={output.strip()[-1000:]!r}"
    try:
        result = json.loads(output.strip())
    except json.JSONDecodeError as error:
        return "PROBE_IMPLEMENTATION_ERROR", process.pid, f"invalid probe serialization: {error}; output={output.strip()[-1000:]!r}"
    if not isinstance(result, dict) or result.get("stage") != "complete":
        return "PROBE_IMPLEMENTATION_ERROR", process.pid, f"stage={result.get('stage') if isinstance(result, dict) else 'unknown'}; {result!r}"
    try:
        ae_status = int(result["appleEventsStatus"])
        ax_trusted = result["axTrusted"] is True
        quartz_allowed = result["quartzAllowed"] is True
        system_events_target_running = result["systemEventsTargetRunning"] is True
        descriptor_type = int(result["descriptorType"])
        wildcard_class = int(result["wildcardClass"])
        wildcard_event = int(result["wildcardEvent"])
    except (KeyError, TypeError, ValueError) as error:
        return "PROBE_IMPLEMENTATION_ERROR", process.pid, f"malformed probe result: {error}; result={result!r}"

    detail = (
        f"caller=/usr/bin/osascript; pid={process.pid}; SystemEventsTargetRunning={str(system_events_target_running).lower()}; "
        f"AXIsProcessTrusted={str(ax_trusted).lower()}; "
        f"AEDeterminePermissionToAutomateTarget(target=com.apple.systemevents, askUserIfNeeded=false)={ae_status}; "
        f"CGPreflightPostEventAccess={str(quartz_allowed).lower()}; descriptorType={descriptor_type}; "
        f"typeWildCard(class/event)={wildcard_class}/{wildcard_event}"
    )
    if ae_status == -1743:
        return "APPLE_EVENTS_DENIED", process.pid, detail
    if ae_status == -1744:
        return "APPLE_EVENTS_CONSENT_REQUIRED", process.pid, detail
    if ae_status == -600 or not system_events_target_running:
        return "APPLE_EVENTS_TARGET_UNAVAILABLE", process.pid, detail
    if ae_status == -32003:
        return "APPLE_EVENTS_UNSUPPORTED", process.pid, detail
    bridge_failures = {
        -32001: "descriptor construction failed",
        -32002: "native AEAddressDesc access failed",
        -32004: "native permission bridge raised an Objective-C exception",
    }
    if ae_status in bridge_failures:
        return "PROBE_IMPLEMENTATION_ERROR", process.pid, f"{bridge_failures[ae_status]}; {detail}"
    if ae_status != 0:
        return "APPLE_EVENTS_PROBE_ERROR", process.pid, detail
    if not ax_trusted:
        return "AX_UNTRUSTED", process.pid, detail
    # Quartz input is authorized and checked by the process that posts it. This value
    # describes osascript only and is deliberately not used to certify another client.
    return "OSASCRIPT_AX_APPLE_EVENTS_AUTHORIZED", process.pid, detail


def main() -> int:
    state, pid, detail = probe_osascript_access()
    print(f"MACOS_AUTOMATION_PERMISSION_PROBE={state}; caller=/usr/bin/osascript; pid={pid}; detail={detail!r}")
    if state == "OSASCRIPT_AX_APPLE_EVENTS_AUTHORIZED":
        return 0
    if state in {
        "AX_UNTRUSTED",
        "APPLE_EVENTS_DENIED",
        "APPLE_EVENTS_CONSENT_REQUIRED",
        "APPLE_EVENTS_TARGET_UNAVAILABLE",
        "APPLE_EVENTS_UNSUPPORTED",
    }:
        return 77
    return 1


def capture_failure_context(label: str, target_pid: int | None = None) -> str:
    """Collect bounded process ancestry and recent TCC diagnostics without changing permissions."""
    sections = [f"operation={label}; python_pid={os.getpid()}; parent_pid={os.getppid()}; python={Path(sys.executable).resolve()}"]
    process_command = ["/bin/ps", "-axo", "pid=,ppid=,comm="]
    try:
        result = subprocess.run(process_command, capture_output=True, text=True, timeout=1.5, check=False)
        if result.returncode == 0:
            processes: dict[int, tuple[int, str]] = {}
            for line in result.stdout.splitlines():
                fields = line.strip().split(maxsplit=2)
                if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit():
                    processes[int(fields[0])] = (int(fields[1]), fields[2])

            def ancestry(pid: int) -> list[str]:
                chain: list[str] = []
                visited: set[int] = set()
                while pid in processes and pid not in visited and len(chain) < 6:
                    visited.add(pid)
                    parent, executable = processes[pid]
                    chain.append(f"{pid}<-{parent}:{executable}")
                    pid = parent
                return chain

            relevant = [
                f"{pid}<-{parent}:{executable}"
                for pid, (parent, executable) in processes.items()
                if any(token in executable.lower() for token in ("hosted-compute-agent", "osascript", "system events", "pulseforge"))
            ][:24]
            sections.append("processes=" + " | ".join(relevant))
            sections.append("python_ancestry=" + " | ".join(ancestry(os.getpid())))
            if target_pid is not None:
                sections.append("target_ancestry=" + " | ".join(ancestry(target_pid)))
        else:
            sections.append(f"process_snapshot=FAILED; exit_code={result.returncode}; {result.stderr.strip()[-300:]}")
    except subprocess.TimeoutExpired:
        sections.append("process_snapshot=TIMED_OUT")
    except OSError as error:
        sections.append(f"process_snapshot=FAILED; {type(error).__name__}: {error}")

    tcc_command = [
        "/usr/bin/log", "show", "--last", "3m", "--style", "compact", "--predicate",
        'process == "tccd" AND (eventMessage CONTAINS[c] "osascript" OR '
        'eventMessage CONTAINS[c] "hosted-compute-agent" OR eventMessage CONTAINS[c] "System Events" OR '
        'eventMessage CONTAINS[c] "PulseForge")',
    ]
    try:
        result = subprocess.run(tcc_command, capture_output=True, text=True, timeout=2.5, check=False)
        if result.returncode == 0:
            lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
            sections.append("tcc_log=" + " | ".join(lines[-20:]) if lines else "tcc_log=NO_MATCHING_EVENTS")
        else:
            sections.append(f"tcc_log=UNAVAILABLE; exit_code={result.returncode}; {result.stderr.strip()[-300:]}")
    except subprocess.TimeoutExpired:
        sections.append("tcc_log=TIMED_OUT")
    except OSError as error:
        sections.append(f"tcc_log=UNAVAILABLE; {type(error).__name__}: {error}")

    return "\n".join(sections)[:12000]


if __name__ == "__main__":
    raise SystemExit(main())
