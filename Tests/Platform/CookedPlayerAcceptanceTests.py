#!/usr/bin/env python3
"""Focused environment-isolation regressions for cooked-player acceptance."""

from contextlib import redirect_stderr, redirect_stdout
from io import BytesIO, TextIOWrapper
import hashlib
import sys
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

from AcceptanceDiagnostics import configure_utf8_diagnostics
from CookedPlayerAcceptance import (MAX_CAPTURE_BYTES, capture_dxc_identity, capture_toolchain_identity,
                                    capture_vulkan_sdk_identity, make_linux_standalone_environment,
                                    capture_release_runtime_identity, capture_selected_renderer_identity_from_log,
                                    package_tree_fingerprint,
                                    resolve_source_tree_identity, verify_cook_manifest, write_release_evidence)


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


class CookedGraphMaterialManifestTests(unittest.TestCase):
    def _verify(self, entries: list[dict[str, str]]) -> None:
        manifest = {
            "entries": entries,
            "dynamicRoots": ["bfc10000-0000-4000-8000-000000000002"],
            "configuration": "Debug",
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            verify_cook_manifest(path, "bfc10000-0000-4000-8000-000000000002")

    def test_requires_the_expected_cooked_graph_material(self) -> None:
        products = ("scene", "mesh-primitive", "material", "texture", "environment", "script", "audio", "gameplay-input")
        entries = [{"product": product, "assetId": "other"} for product in products]
        entries.append({"product": "graph-material", "assetId": "10000000-0000-4000-8000-000000000008"})
        self._verify(entries)

    def test_rejects_graph_material_product_for_an_unrelated_asset(self) -> None:
        products = ("scene", "mesh-primitive", "material", "texture", "environment", "script", "audio", "gameplay-input")
        entries = [{"product": product, "assetId": "other"} for product in products]
        entries.append({"product": "graph-material", "assetId": "other"})
        with self.assertRaisesRegex(RuntimeError, "expected graph-backed floor material"):
            self._verify(entries)


class ReleaseEvidenceTests(unittest.TestCase):
    def test_dxc_identity_uses_cmake_selection_not_process_environment_override(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            configured_dxc = root / "configured" / "dxc.exe"
            environment_dxc = root / "environment" / "dxc.exe"
            configured_dxc.parent.mkdir()
            environment_dxc.parent.mkdir()
            configured_dxc.write_bytes(b"configured")
            environment_dxc.write_bytes(b"environment")
            (root / "CMakeCache.txt").write_text(
                f"PULSEFORGE_DXC_EXECUTABLE:FILEPATH={configured_dxc}\n", encoding="utf-8")

            def run(command: list[str], **_kwargs):
                self.assertEqual(command, [str(configured_dxc.resolve()), "--version"])
                return type("Completed", (), {"returncode": 0, "stdout": "configured DXC 1.2", "stderr": ""})()

            with patch("CookedPlayerAcceptance.shutil.which", return_value=None):
                identity = capture_dxc_identity(root, {"PULSEFORGE_DXC_EXECUTABLE": str(environment_dxc)}, run)

        self.assertEqual(identity["status"], "observed")
        self.assertEqual(identity["path"], str(configured_dxc.resolve()))
        self.assertEqual(identity["versionOutput"], "configured DXC 1.2")

    def test_vulkan_sdk_version_can_come_from_observed_root_directory_name(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "1.4.363.0"
            sdk_root.mkdir()
            identity = capture_vulkan_sdk_identity({"VULKAN_SDK": str(sdk_root)})

        self.assertEqual(identity["status"], "observed")
        self.assertEqual(identity["version"], "1.4.363.0")
        self.assertEqual(identity["versionSource"], "rootDirectoryName")

    def test_vulkan_sdk_root_without_version_metadata_remains_explicitly_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk_root = Path(directory) / "custom-sdk"
            sdk_root.mkdir()
            identity = capture_vulkan_sdk_identity({"VULKAN_SDK": str(sdk_root)})

        self.assertEqual(identity["status"], "observed")
        self.assertEqual(identity["versionStatus"], "unavailable")
        self.assertEqual(identity["versionReason"], "SDK root has no readable version.txt or version.json")

    def test_renderer_identity_scans_retained_log_beyond_bounded_process_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "player.log"
            log.write_bytes(
                b"Selected Vulkan GPU: Test GPU (API 1.3.0, driver 42, maxBoundDescriptorSets 8, graphics queue 0, "
                b"present queue 0)\n" + b"tail output\n" * (MAX_CAPTURE_BYTES // 5))
            identity = capture_selected_renderer_identity_from_log(log, "Vulkan", required=True)

        self.assertEqual(identity["deviceName"], "Test GPU")
        self.assertEqual(identity["apiVersion"], "1.3.0")
        self.assertEqual(identity["driverVersion"], "42")

    def test_runtime_identity_uses_observed_renderer_sdk_dxc_and_cooked_manifest_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "VulkanSDK"
            (sdk / "bin").mkdir(parents=True)
            (sdk / "version.txt").write_text("1.4.321.1\n", encoding="utf-8")
            dxc = sdk / "bin" / "dxc"
            dxc.write_bytes(b"test executable placeholder")
            manifest = root / "manifest.pfmanifest"
            manifest_bytes = json.dumps({
                "format": "PulseForgeCookManifest", "version": 8, "projectId": "project-id",
                "startupScene": "scene-id", "targetPlatform": "Linux", "rendererBackend": "Vulkan",
                "runtimeABI": "pulseforge-runtime-v1", "configuration": "Release",
            }, sort_keys=True).encode("utf-8")
            manifest.write_bytes(manifest_bytes)
            observed_output = (
                "Selected Vulkan GPU: Test GPU (API 1.3.0, driver 42, maxBoundDescriptorSets 8, "
                "graphics queue 0, present queue 0)"
            )

            def run(command: list[str], **_kwargs):
                self.assertEqual(command, [str(dxc.resolve()), "--version"])
                return type("Completed", (), {"returncode": 0, "stdout": "libdxcompiler 1.9.2602.24", "stderr": ""})()

            identity = capture_release_runtime_identity(
                observed_output, "Vulkan", manifest, root / "build",
                {"VULKAN_SDK": str(sdk)}, run, player_launched=True)

        self.assertEqual(identity["selectedRenderer"], {
            "status": "observed", "backend": "Vulkan", "deviceName": "Test GPU",
            "apiVersion": "1.3.0", "driverVersion": "42",
        })
        self.assertEqual(identity["vulkanSdk"]["version"], "1.4.321.1")
        self.assertEqual(identity["dxc"]["versionOutput"], "libdxcompiler 1.9.2602.24")
        self.assertEqual(identity["cookedContent"]["version"], 8)
        self.assertEqual(identity["cookedContent"]["sha256"], hashlib.sha256(manifest_bytes).hexdigest())

    def test_vulkan_runtime_requires_selected_gpu_record_but_export_only_reports_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.pfmanifest"
            manifest.write_text("{}", encoding="utf-8")
            with patch("CookedPlayerAcceptance.shutil.which", return_value=None):
                with self.assertRaisesRegex(RuntimeError, "requires the selected Vulkan GPU/API/driver record"):
                    capture_release_runtime_identity("", "Vulkan", manifest, root / "build", {},
                                                     lambda *_args, **_kwargs: self.fail("DXC must not be queried"),
                                                     player_launched=True)
                identity = capture_release_runtime_identity(
                    "", "Vulkan", manifest, root / "build", {},
                    lambda *_args, **_kwargs: self.fail("DXC must not be queried"), player_launched=False)

        self.assertEqual(identity["selectedRenderer"], {
            "status": "unavailable", "reason": "export-only acceptance did not launch a Player process"})
        self.assertEqual(identity["vulkanSdk"]["status"], "unavailable")
        self.assertEqual(identity["dxc"]["status"], "unavailable")
        self.assertEqual(identity["cookedContent"]["status"], "unavailable")

    def test_source_tree_comes_from_applied_patch_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            identity_path = Path(directory) / "patch-validation-identity.json"
            identity_path.write_text(json.dumps({
                "validation_kind": "EXPERIMENTAL_PATCHED_SOURCE", "resulting_tree": "c" * 40,
            }), encoding="utf-8")
            with patch.dict(os.environ, {"RUNNER_TEMP": directory}, clear=False):
                with patch.dict(os.environ, {"PULSEFORGE_SOURCE_TREE": ""}):
                    self.assertEqual(resolve_source_tree_identity(), "c" * 40)

    def test_rejects_source_tree_override_that_disagrees_with_patch_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            identity_path = Path(directory) / "patch-validation-identity.json"
            identity_path.write_text(json.dumps({
                "validation_kind": "EXPERIMENTAL_PATCHED_SOURCE", "resulting_tree": "c" * 40,
            }), encoding="utf-8")
            with patch.dict(os.environ, {"RUNNER_TEMP": directory, "PULSEFORGE_SOURCE_TREE": "d" * 40}):
                with self.assertRaisesRegex(RuntimeError, "disagrees with the applied patch validation identity"):
                    resolve_source_tree_identity()

    def test_toolchain_identity_uses_the_configured_cmake_compiler_not_path_guessing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeCache.txt").write_text(
                "CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/cl.exe\n"
                "CMAKE_CXX_COMPILER_ID:STRING=MSVC\n"
                "CMAKE_CXX_COMPILER_VERSION:STRING=19.44.35207\n"
                "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n",
                encoding="utf-8",
            )
            with patch("CookedPlayerAcceptance.subprocess.run") as run:
                run.return_value = type("Completed", (), {"returncode": 0, "stdout": "cmake version 4.0", "stderr": ""})()
                identity = capture_toolchain_identity(root)

        self.assertEqual(identity["compilerId"], "MSVC")
        self.assertEqual(identity["compilerVersion"], "19.44.35207")
        self.assertEqual(identity["generator"], "Visual Studio 17 2022")
        self.assertNotIn("compilerCommand", identity)
        self.assertEqual(run.call_args_list[-1].args[0], ["cmake", "--version"])

    def test_toolchain_identity_finds_multiconfig_cache_above_template_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_root = Path(directory)
            template_root = build_root / "Release"
            template_root.mkdir()
            (build_root / "CMakeCache.txt").write_text(
                "CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/cl.exe\n"
                "CMAKE_CXX_COMPILER_ID:STRING=MSVC\n"
                "CMAKE_CXX_COMPILER_VERSION:STRING=19.44.35207\n"
                "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n",
                encoding="utf-8",
            )
            with patch("CookedPlayerAcceptance.subprocess.run") as run:
                run.return_value = type("Completed", (), {"returncode": 0, "stdout": "cmake version 4.0", "stderr": ""})()
                identity = capture_toolchain_identity(template_root)

        self.assertEqual(identity["compilerId"], "MSVC")
        self.assertEqual(identity["compilerVersion"], "19.44.35207")
        self.assertEqual(identity["generator"], "Ninja Multi-Config")
        self.assertNotIn("compilerCommand", identity)
        self.assertEqual(run.call_args_list[-1].args[0], ["cmake", "--version"])

    def test_toolchain_identity_reads_compiler_id_from_generated_cmake_description(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeCache.txt").write_text(
                "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n",
                encoding="utf-8",
            )
            compiler_files = root / "CMakeFiles" / "4.2.1"
            compiler_files.mkdir(parents=True)
            (compiler_files / "CMakeCXXCompiler.cmake").write_text(
                'set(CMAKE_CXX_COMPILER "C:/Program Files/VS/VC/Tools/MSVC/cl.exe")\n'
                'set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                'set(CMAKE_CXX_COMPILER_VERSION "19.44.35207")\n',
                encoding="utf-8",
            )
            with patch("CookedPlayerAcceptance.subprocess.run") as run:
                run.return_value = type("Completed", (), {"returncode": 0, "stdout": "cmake version 4.2.1", "stderr": ""})()
                identity = capture_toolchain_identity(root)

        self.assertEqual(identity["compilerId"], "MSVC")
        self.assertEqual(identity["compilerVersion"], "19.44.35207")
        self.assertEqual(identity["compilerPath"], "C:/Program Files/VS/VC/Tools/MSVC/cl.exe")
        self.assertEqual(identity["generator"], "Ninja Multi-Config")
        self.assertNotIn("compilerCommand", identity)
        self.assertEqual(run.call_args_list[-1].args[0], ["cmake", "--version"])

    def test_evidence_binds_source_template_toolchain_and_stable_package_digest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "package"
            package.mkdir()
            template_root = root / "template"
            template_root.mkdir()
            (package / "player.bin").write_bytes(b"player")
            (package / "Shaders").mkdir()
            (package / "Shaders" / "material.spv").write_bytes(b"shader")
            (template_root / "player.bin").write_bytes(b"player")
            (template_root / "Shaders").mkdir()
            (template_root / "Shaders" / "material.spv").write_bytes(b"shader")
            template = template_root / "PulseForgePlayerTemplate.json"
            template_bytes = json.dumps({
                "targetPlatform": "Linux", "architecture": "x64", "configuration": "Release",
                "renderer": "Vulkan", "runtimeABI": "pulseforge-runtime-test",
                "runtimeFiles": ["player.bin", "Shaders/material.spv"],
            }).encode("utf-8")
            template.write_bytes(template_bytes)
            output = root / "release-evidence.json"
            toolchain = {"compiler": "GCC 14", "cmake": "3.31"}
            runtime_identity = {"selectedRenderer": {"status": "unavailable", "reason": "not launched"}}

            evidence = write_release_evidence(package, template_root, template, output, "a" * 40, "Linux", "Vulkan",
                                              "Release", toolchain, runtime_identity)
            restored = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(restored["sourceTree"], "a" * 40)
            self.assertEqual(restored["runtimeABI"], "pulseforge-runtime-test")
            self.assertEqual(restored["toolchain"], toolchain)
            self.assertEqual(restored["runtimeIdentity"], runtime_identity)
            self.assertEqual(restored["templateDescriptorSha256"], hashlib.sha256(template_bytes).hexdigest())
            self.assertEqual(restored["packageFileCount"], 2)
            self.assertEqual(restored["packageEntryCount"], 4)
            self.assertEqual(restored["templateDescriptorRelativePath"], "PulseForgePlayerTemplate.json")
            self.assertFalse((package / "PulseForgePlayerTemplate.json").exists())
            self.assertEqual(evidence["packageTreeFingerprintSha256"], package_tree_fingerprint({
                ".": f"dir:{stat.S_IMODE(package.stat().st_mode):04o}",
                "Shaders/": f"dir:{stat.S_IMODE((package / 'Shaders').stat().st_mode):04o}",
                "player.bin": f"file:{stat.S_IMODE((package / 'player.bin').stat().st_mode):04o}:" + hashlib.sha256(b"player").hexdigest(),
                "Shaders/material.spv": f"file:{stat.S_IMODE((package / 'Shaders' / 'material.spv').stat().st_mode):04o}:" + hashlib.sha256(b"shader").hexdigest(),
            }))
            self.assertEqual(package_tree_fingerprint({"z": "2", "a": "1"}),
                             package_tree_fingerprint({"a": "1", "z": "2"}))

    def test_rejects_invalid_source_tree_and_mismatched_template(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "package"
            package.mkdir()
            (package / "player.bin").write_bytes(b"player")
            template_root = root / "template"
            template_root.mkdir()
            (template_root / "player.bin").write_bytes(b"player")
            template = template_root / "PulseForgePlayerTemplate.json"
            template_bytes = json.dumps({
                "targetPlatform": "Windows", "architecture": "x64", "configuration": "Debug",
                "renderer": "Vulkan", "runtimeABI": "pulseforge-runtime-test", "runtimeFiles": ["player.bin"],
            }).encode("utf-8")
            template.write_bytes(template_bytes)
            (package / "PulseForgePlayerTemplate.json").write_bytes(template_bytes)
            output = root / "release-evidence.json"
            toolchain = {"compiler": "MSVC"}
            runtime_identity = {}
            with self.assertRaisesRegex(RuntimeError, "source tree SHA"):
                write_release_evidence(package, template_root, template, output, "not-a-tree", "Windows", "Vulkan",
                                       "Debug", toolchain, runtime_identity)
            with self.assertRaisesRegex(RuntimeError, "template identity"):
                write_release_evidence(package, template_root, template, output, "b" * 40, "Linux", "Vulkan",
                                       "Debug", toolchain, runtime_identity)
            (package / "player.bin").write_bytes(b"different")
            with self.assertRaisesRegex(RuntimeError, "differs from its template source"):
                write_release_evidence(package, template_root, template, output, "b" * 40, "Windows", "Vulkan",
                                       "Debug", toolchain, runtime_identity)

    def test_template_descriptor_identity_is_retained_outside_the_runtime_package(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "package"
            package.mkdir()
            template_root = root / "template"
            template_root.mkdir()
            (package / "player.bin").write_bytes(b"player")
            (template_root / "player.bin").write_bytes(b"player")
            descriptor = {
                "targetPlatform": "Linux", "architecture": "x64", "configuration": "Release",
                "renderer": "Vulkan", "runtimeABI": "pulseforge-runtime-test", "runtimeFiles": ["player.bin"],
            }
            template = template_root / "PulseForgePlayerTemplate.json"
            template.write_text(json.dumps(descriptor), encoding="utf-8")
            evidence = write_release_evidence(package, template_root, template, root / "evidence.json", "c" * 40,
                                              "Linux", "Vulkan", "Release", {"compiler": "GCC"}, {})

            self.assertEqual(evidence["templateDescriptorSha256"], hashlib.sha256(template.read_bytes()).hexdigest())
            self.assertFalse((package / template.name).exists())

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
