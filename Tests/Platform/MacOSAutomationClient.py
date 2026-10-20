"""Narrow diagnostics for the macOS automation clients used by desktop smoke tests."""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
from pathlib import Path


def probe_osascript_access(timeout_seconds: float = 3.0) -> tuple[str, int | None, str]:
    """Nonpromptingly check Accessibility trust for the actual osascript executable."""
    command = [
        "/usr/bin/osascript",
        "-l",
        "JavaScript",
        "-e", "ObjC.import('ApplicationServices'); 'AX=' + $.AXIsProcessTrusted()",
    ]
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
    except OSError as error:
        return "PROBE_FAILED", None, f"{type(error).__name__}: {error}"

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
        return "PROBE_TIMED_OUT", process.pid, output.strip()[-500:]

    if process.returncode != 0:
        return "PROBE_FAILED", process.pid, f"exit_code={process.returncode}; output={output.strip()[-500:]!r}"
    match = re.fullmatch(r"AX=(true|false)", output.strip(), re.IGNORECASE)
    if not match:
        return "PROBE_FAILED", process.pid, f"unexpected osascript result={output.strip()[-500:]!r}"
    ax_trusted = match.group(1).lower() == "true"
    detail = (
        f"AXIsProcessTrusted={str(ax_trusted).lower()}; "
        "Apple Events permission is checked by each bounded System Events operation, not this AX probe"
    )
    if not ax_trusted:
        return "AX_UNTRUSTED", process.pid, detail
    return "AX_TRUSTED", process.pid, detail


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
        "/usr/bin/log",
        "show",
        "--last",
        "3m",
        "--style",
        "compact",
        "--predicate",
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
