#!/usr/bin/env python3
"""Focused environment-isolation regressions for cooked-player acceptance."""

import unittest

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
