#!/usr/bin/env python3
"""Export and exercise the RuntimeUI sample through a relocated cooked Player."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from StandalonePackageDesktopSmoke import WindowsPackageReadOnlyAccess, _build_environment


REQUIRED_MARKERS = (
    "PULSEFORGE_RUNTIME_UI_PACKAGE_MENU=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_GAMEPLAY=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_HUD=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_PAUSE=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_MODAL_INPUT=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_RESUME=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_RESUMED_GAMEPLAY=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_ACCEPTANCE=PASS",
    "PULSEFORGE_RUNTIME_UI_PACKAGE_SHUTDOWN=PASS",
    "Runtime UI font has unsupported text beyond Latin-1; replacement glyphs are shown",
)


def run(command: list[str], cwd: Path, timeout: int, log: Path | None = None,
        environment: dict[str, str] | None = None) -> str:
    print(f"G18_PACKAGE_COMMAND={command!r}")
    print(f"G18_PACKAGE_CWD={cwd}")
    if log is None:
        result = subprocess.run(command, cwd=cwd, env=environment, text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=timeout, check=False)
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise RuntimeError(f"command exited {result.returncode}: {command!r}\n{output[-16000:]}")
        return output

    with log.open("wb") as captured:
        process = subprocess.Popen(command, cwd=cwd, env=environment, stdout=captured, stderr=subprocess.STDOUT)
        try:
            result = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            raise RuntimeError(f"exported Player exceeded {timeout}s; inspect {log}") from error
    output = log.read_text(encoding="utf-8", errors="replace")
    if result != 0:
        raise RuntimeError(f"exported Player exited {result}; inspect {log}\n{output[-16000:]}")
    return output


def digest_tree(root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise RuntimeError(f"refusing linked package entry: {path}")
        if path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            result[path.relative_to(root).as_posix()] = digest
    return result


def environment_value(environment: dict[str, str], name: str) -> str | None:
    return next((value for key, value in environment.items() if key.casefold() == name.casefold()), None)


def pe_machine(path: Path) -> int:
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise RuntimeError(f"test Vulkan runtime is not a PE image: {path}")
        stream.seek(0x3C)
        offset = struct.unpack("<I", stream.read(4))[0]
        if offset < 0x40 or offset > path.stat().st_size - 6:
            raise RuntimeError(f"test Vulkan runtime has an invalid PE header: {path}")
        stream.seek(offset)
        if stream.read(4) != b"PE\0\0":
            raise RuntimeError(f"test Vulkan runtime has an invalid PE signature: {path}")
        return struct.unpack("<H", stream.read(2))[0]


def prepare_windows_vulkan_runtime(root: Path, package: Path) -> tuple[Path, str]:
    if os.name != "nt":
        raise RuntimeError("the relocated runtime UI package acceptance test requires Windows")
    supplied = environment_value(os.environ, "G14_WINDOWS_VULKAN_RUNTIME_LIBRARY")
    if supplied:
        source = Path(supplied)
        provider = "existing test-only Vulkan runtime"
    else:
        program_files = environment_value(os.environ, "ProgramFiles") or r"C:\Program Files"
        program_files_x86 = environment_value(os.environ, "ProgramFiles(x86)") or r"C:\Program Files (x86)"
        chrome_roots = [Path(program_files) / "Google/Chrome/Application",
                        Path(program_files_x86) / "Google/Chrome/Application"]
        chrome_root = next((candidate for candidate in chrome_roots
                            if (candidate / "chrome.exe").is_file()), None)
        if chrome_root is None:
            raise RuntimeError("the standard Windows runner has no installed Chrome SwiftShader test runtime")
        version_directories = []
        for candidate in chrome_root.iterdir():
            parts = candidate.name.split(".")
            if candidate.is_dir() and len(parts) >= 3 and all(part.isdigit() for part in parts):
                version_directories.append((tuple(int(part) for part in parts), candidate))
        version_directories.sort(key=lambda entry: entry[0], reverse=True)
        candidates = [chrome_root / "vk_swiftshader.dll"] + [
            directory / "vk_swiftshader.dll" for _, directory in version_directories
        ]
        source = next((candidate for candidate in candidates
                       if not candidate.is_symlink() and candidate.is_file()), None)
        if source is None:
            raise RuntimeError(f"Chrome has no installed SwiftShader Vulkan runtime: {chrome_root}")
        provider = "Chrome SwiftShader"

    if source.is_symlink() or not source.is_file():
        raise RuntimeError(f"the test Vulkan runtime is missing or linked: {source}")
    source = source.resolve(strict=True)
    if source.name.casefold() not in ("vulkan-1.dll", "vk_swiftshader.dll"):
        raise RuntimeError(f"the test Vulkan runtime has an unexpected name: {source.name}")
    if package == source or package in source.parents:
        raise RuntimeError("the test Vulkan runtime must remain outside the exported package")
    if source.stat().st_size < 1024 * 1024 or pe_machine(source) != 0x8664:
        raise RuntimeError("the test Vulkan runtime is too small or is not a Windows x64 PE image")

    runtime_directory = root / "External Vulkan Runtime"
    runtime_directory.mkdir()
    runtime_library = runtime_directory / "vulkan-1.dll"
    shutil.copyfile(source, runtime_library)
    source_digest = hashlib.sha256()
    copied_digest = hashlib.sha256()
    with source.open("rb") as source_stream, runtime_library.open("rb") as copied_stream:
        while chunk := source_stream.read(1024 * 1024):
            source_digest.update(chunk)
        while chunk := copied_stream.read(1024 * 1024):
            copied_digest.update(chunk)
    source_hash = source_digest.hexdigest()
    copied_hash = copied_digest.hexdigest()
    if source_hash != copied_hash:
        raise RuntimeError("the external test Vulkan runtime copy failed its SHA-256 check")
    return runtime_library, f"provider={provider}; sha256={source_hash}; external to package"


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")

    parser = argparse.ArgumentParser()
    parser.add_argument("--export", required=True, type=Path)
    parser.add_argument("--template-root", required=True, type=Path)
    parser.add_argument("--template-descriptor", required=True, type=Path)
    parser.add_argument("--source-project", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    for path in (args.export, args.template_root, args.template_descriptor, args.source_project):
        if not path.exists():
            raise RuntimeError(f"required G18 package input is missing: {path}")

    temporary_parent = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())).resolve()
    temporary_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="PulseForge G18 Runtime UI Package ", dir=temporary_parent) as name:
        root = Path(name).resolve(strict=True)
        project_root = root / "Source Project Ω"
        shutil.copytree(args.source_project.resolve(strict=True).parent, project_root,
                        ignore=shutil.ignore_patterns(".pulseforge", "Build"), symlinks=False)
        if any(path.is_symlink() for path in project_root.rglob("*")):
            raise RuntimeError("source sample copy contains a linked path")
        project_file = project_root / args.source_project.name
        project = json.loads(project_file.read_text(encoding="utf-8"))
        profile = {
            "format": "PulseForgeBuildProfile",
            "version": 1,
            "profileId": "b0180000-0000-4000-8000-0000000001f0",
            "displayName": "G18 Runtime UI Package Acceptance",
            "targetPlatform": "Windows",
            "architecture": "x64",
            "configuration": "Debug",
            "renderer": "Vulkan",
            "startupScene": project["startScene"],
            # Runtime Lua chooses the gameplay scene by stable asset ID, so the package
            # profile must declare it as a dynamic cook root.
            "dynamicRoots": ["b0180000-0000-4000-8000-000000000011"],
            "defaultOutputPath": "Build/Exports/G18Acceptance",
            "window": {"width": 960, "height": 540},
        }
        profile_path = root / "G18 Acceptance.pfbuildprofile"
        profile_path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
        staging_package = root / "Export Staging"
        unrelated_cwd = root / "Unrelated Working Directory Ω"
        unrelated_cwd.mkdir()
        export_output = run([
            str(args.export.resolve()), "--profile", str(profile_path), "--project", str(project_file),
            "--template-root", str(args.template_root.resolve()), "--template-descriptor",
            str(args.template_descriptor.resolve()), "--output", str(staging_package),
        ], unrelated_cwd, args.timeout_seconds)
        print(export_output)
        if not staging_package.is_dir():
            raise RuntimeError("export command returned success without producing a package")

        package = root / "Relocated Read-only Package Ω"
        staging_package.rename(package)
        shutil.rmtree(project_root)
        if project_root.exists():
            raise RuntimeError("source project remained available after export")
        executable = package / "PulseForgeCookedGame.exe"
        if not executable.is_file():
            raise RuntimeError(f"exported Player executable is missing: {executable}")

        writable = root / "Isolated Writable User Data"
        writable.mkdir()
        runtime_library, runtime_identity = prepare_windows_vulkan_runtime(root, package)
        environment = _build_environment("Windows", writable, WindowsVulkanRuntimeLibrary=runtime_library)
        environment["PULSEFORGE_RUNTIME_UI_PACKAGE_ACCEPTANCE"] = "1"
        user_data = Path(environment["PULSEFORGE_USER_DATA_DIR"])
        print(f"G18_PACKAGE_VULKAN_RUNTIME=PASS; {runtime_identity}")

        read_only = WindowsPackageReadOnlyAccess(package, root)
        try:
            read_only.apply()
            before = digest_tree(package)
            process_log = root / "runtime-ui-package-player.log"
            output = run([str(executable)], unrelated_cwd, args.timeout_seconds, process_log, environment)
            missing = [marker for marker in REQUIRED_MARKERS if marker not in output]
            if missing:
                raise RuntimeError(f"G18 package omitted behavior markers {missing}; inspect {process_log}\n{output[-16000:]}")
            after = digest_tree(package)
            if before != after:
                raise RuntimeError("read-only G18 package contents changed during the interaction run")
            user_data_prefix = "PULSEFORGE_RUNTIME_UI_PACKAGE_USER_DATA=PASS; root="
            user_data_lines = [line for line in output.splitlines() if user_data_prefix in line]
            if len(user_data_lines) != 1:
                raise RuntimeError("Player did not report exactly one resolved G18 user-data root")
            reported_user_data = Path(user_data_lines[0].split(user_data_prefix, 1)[1].strip())
            if not user_data.is_dir() or reported_user_data.resolve() != user_data.resolve():
                raise RuntimeError(f"Player resolved unexpected G18 user data: {reported_user_data}")
            print("G18_RUNTIME_UI_EXPORTED_PACKAGE=PASS; source removed; unrelated CWD; package write denied; "
                  "synthetic mapped Player input; menu/gameplay/HUD/pause/resume; normal exit")
        finally:
            read_only.restore()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"G18_RUNTIME_UI_EXPORTED_PACKAGE=FAIL; {error}", file=sys.stderr)
        raise SystemExit(1)
