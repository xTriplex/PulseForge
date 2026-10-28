#!/usr/bin/env python3
"""Focused environment-isolation regressions for cooked-player acceptance."""

from contextlib import redirect_stderr, redirect_stdout
from io import BytesIO, TextIOWrapper
import sys
import unittest

from AcceptanceDiagnostics import configure_utf8_diagnostics
from CookedPlayerAcceptance import make_linux_standalone_environment


class LinuxStandaloneEnvironmentTests(unittest.TestCase):
    def test_preserves_only_virtual_display_connection(self) -> None:
        inherited = {
            "DISPLAY": ":97",
            "XAUTHORITY": "/tmp/xvfb-auth",
            "VULKAN_SDK": "/developer/sdk",
            "VK_DRIVER_FILES": "/developer/sdk/icd.json",
            "LD_LIBRARY_PATH": "/developer/sdk/lib",
            "DYLD_LIBRARY_PATH": "/developer/sdk/lib",
            "HOME": "/developer/home",
        }

        environment = make_linux_standalone_environment(inherited)

        self.assertEqual(environment["DISPLAY"], ":97")
        self.assertEqual(environment["XAUTHORITY"], "/tmp/xvfb-auth")
        self.assertEqual(environment["PATH"], "/usr/bin:/bin:/usr/sbin:/sbin")
        self.assertEqual(environment["LANG"], "C.UTF-8")
        for variable in ("VULKAN_SDK", "VK_DRIVER_FILES", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "HOME"):
            self.assertNotIn(variable, environment)

    def test_missing_display_is_not_fabricated(self) -> None:
        environment = make_linux_standalone_environment({"VULKAN_SDK": "/developer/sdk"})

        self.assertNotIn("DISPLAY", environment)
        self.assertNotIn("XAUTHORITY", environment)
        self.assertNotIn("VULKAN_SDK", environment)


class Utf8DiagnosticTests(unittest.TestCase):
    def test_unicode_path_diagnostics_use_utf8_streams(self) -> None:
        stdout_bytes = BytesIO()
        stderr_bytes = BytesIO()
        stdout = TextIOWrapper(stdout_bytes, encoding="cp1252")
        stderr = TextIOWrapper(stderr_bytes, encoding="cp1252")

        with redirect_stdout(stdout), redirect_stderr(stderr):
            configure_utf8_diagnostics()
            print("STANDALONE_PACKAGE_RETAINED=C:/Runner Temp/PulseForge Standalone Ω")
            print("Acceptance artifacts retained at C:/Runner Temp/PulseForge Ω", file=sys.stderr)
            stdout.flush()
            stderr.flush()

        self.assertIn("Ω".encode("utf-8"), stdout_bytes.getvalue())
        self.assertIn("Ω".encode("utf-8"), stderr_bytes.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
