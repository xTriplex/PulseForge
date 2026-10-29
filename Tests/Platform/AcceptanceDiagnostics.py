"""Output setup shared by the platform acceptance command-line helpers."""

from __future__ import annotations

import sys


def configure_utf8_diagnostics() -> None:
    """Keep Unicode paths printable without changing the process or machine code page."""
    for name, stream in (("stdout", sys.stdout), ("stderr", sys.stderr)):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")
        encoding = getattr(stream, "encoding", None)
        if encoding is not None and encoding.replace("-", "").casefold() != "utf8":
            raise RuntimeError(
                f"could not configure {name} for UTF-8 diagnostics; active encoding is {encoding!r}")
