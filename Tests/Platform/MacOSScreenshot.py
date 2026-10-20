"""Bounded screenshots for macOS acceptance evidence."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path


def capture_screenshot(filename: str, timeout_seconds: float = 2.0) -> str:
    runner_temp = os.environ.get("RUNNER_TEMP")
    if not runner_temp:
        return "UNAVAILABLE:RUNNER_TEMP_NOT_SET"
    if Path(filename).name != filename or not filename.endswith(".png"):
        return f"FAILED:INVALID_FILENAME:{filename!r}"
    if timeout_seconds <= 0:
        return "FAILED:INVALID_TIMEOUT"

    try:
        output_directory = Path(runner_temp).resolve(strict=True)
        screenshot = output_directory / filename
        if screenshot.is_symlink():
            return f"FAILED:REFUSING_SYMLINK:{screenshot}"
        if screenshot.exists():
            if not screenshot.is_file():
                return f"FAILED:TARGET_IS_NOT_A_FILE:{screenshot}"
            screenshot.unlink()

        result = subprocess.run(
            ["/usr/sbin/screencapture", "-x", "-C", "-t", "png", str(screenshot)],
            capture_output=True,
            text=True,
            timeout=min(2.0, timeout_seconds),
            check=False,
        )
        if result.returncode == 0 and screenshot.is_file() and screenshot.stat().st_size > 0:
            return f"CAPTURED; bytes={screenshot.stat().st_size}; path={screenshot}"
        output = (result.stdout + result.stderr).strip()[-500:]
        lowered = output.lower()
        state = (
            "TCC_DENIED"
            if any(token in lowered for token in ("not authorized", "not permitted", "screen recording", "tcc"))
            else "FAILED"
        )
        if screenshot.is_file() and not screenshot.is_symlink():
            screenshot.unlink()
        return f"{state}; exit_code={result.returncode}; detail={output!r}; path={screenshot}"
    except subprocess.TimeoutExpired as error:
        try:
            if "screenshot" in locals() and screenshot.is_file() and not screenshot.is_symlink():
                screenshot.unlink()
        except OSError:
            pass
        return f"TIMED_OUT; timeout_seconds={min(2.0, timeout_seconds)}; detail={error}"
    except OSError as error:
        return f"FAILED; error={type(error).__name__}: {error}"
