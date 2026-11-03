#!/usr/bin/env python3
"""Export the G19 sample and exercise cooked audio plus user mix persistence."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from RuntimeUIPackageAcceptance import digest_tree, prepare_windows_vulkan_runtime, run
from StandalonePackageDesktopSmoke import WindowsPackageReadOnlyAccess, _build_environment


EXERCISE_MARKERS = (
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_UI=PASS",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; Music +",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; Effects +",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; UI +",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; Mute music",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; Play effect",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; Pause gameplay",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; UI -",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACTION=PASS; Resume",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_SETTINGS_SAVED=PASS",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACCEPTANCE=PASS",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_SHUTDOWN=PASS",
)
VERIFY_MARKERS = (
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_UI=PASS",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_SETTINGS_RESTORED=PASS",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_ACCEPTANCE=PASS",
    "PULSEFORGE_AUDIO_MIXING_PACKAGE_SHUTDOWN=PASS",
)


def launch(executable: Path, cwd: Path, timeout: int, log_path: Path, environment: dict[str, str]) -> str:
    print(f"G19_PACKAGE_COMMAND={[str(executable)]!r}")
    print(f"G19_PACKAGE_CWD={cwd}")
    with log_path.open("wb") as log:
        process = subprocess.Popen([str(executable)], cwd=cwd, env=environment, stdout=log, stderr=subprocess.STDOUT)
        try:
            return_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            raise RuntimeError(f"G19 Player exceeded {timeout}s; inspect {log_path}") from error
    output = log_path.read_text(encoding="utf-8", errors="replace")
    print(output)
    if return_code != 0:
        raise RuntimeError(f"G19 Player exited {return_code}; inspect {log_path}")
    return output


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
            raise RuntimeError(f"required G19 package input is missing: {path}")
    if os.name != "nt":
        raise RuntimeError("the G19 exported-package interaction test requires Windows")

    temporary_parent = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())).resolve()
    temporary_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="PulseForge G19 Audio Mixing Package ", dir=temporary_parent) as name:
        root = Path(name).resolve(strict=True)
        source_root = root / "Audio Mixing Source Ω"
        shutil.copytree(args.source_project.resolve(strict=True).parent, source_root,
                        ignore=shutil.ignore_patterns(".pulseforge", "Build"), symlinks=False)
        if any(path.is_symlink() for path in source_root.rglob("*")):
            raise RuntimeError("G19 sample copy contains a linked source")
        project_file = source_root / args.source_project.name
        project = json.loads(project_file.read_text(encoding="utf-8"))
        profile = {
            "format": "PulseForgeBuildProfile",
            "version": 1,
            "profileId": "b0190000-0000-4000-8000-0000000001f0",
            "displayName": "G19 Audio Mixing Package Acceptance",
            "targetPlatform": "Windows",
            "architecture": "x64",
            "configuration": "Debug",
            "renderer": "Vulkan",
            "startupScene": project["startScene"],
            "dynamicRoots": [],
            "defaultOutputPath": "Build/Exports/G19Acceptance",
            "window": {"width": 960, "height": 540},
        }
        profile_path = root / "G19 Audio Mixing.pfbuildprofile"
        profile_path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
        package_staging = root / "Export Staging"
        unrelated_cwd = root / "Unrelated Working Directory Ω"
        unrelated_cwd.mkdir()
        export_output = run([
            str(args.export.resolve()), "--profile", str(profile_path), "--project", str(project_file),
            "--template-root", str(args.template_root.resolve()), "--template-descriptor",
            str(args.template_descriptor.resolve()), "--output", str(package_staging),
        ], unrelated_cwd, args.timeout_seconds)
        print(export_output)
        if not package_staging.is_dir():
            raise RuntimeError("G19 export returned success without producing a package")

        package = root / "Relocated Read-only G19 Package Ω"
        package_staging.rename(package)
        shutil.rmtree(source_root)
        executable = package / "PulseForgeCookedGame.exe"
        if not executable.is_file():
            raise RuntimeError(f"cooked Player executable is missing: {executable}")

        user_data = root / "Isolated Writable User Data"
        user_data.mkdir()
        runtime_library, runtime_identity = prepare_windows_vulkan_runtime(root, package)
        environment = _build_environment("Windows", user_data, WindowsVulkanRuntimeLibrary=runtime_library)
        print(f"G19_PACKAGE_VULKAN_RUNTIME={runtime_identity}")
        read_only = WindowsPackageReadOnlyAccess(package, root)
        try:
            read_only.apply()
            before = digest_tree(package)
            executable_log = root / "g19-audio-mixing-exercise.log"
            environment["PULSEFORGE_AUDIO_MIXING_PACKAGE_ACCEPTANCE"] = "exercise"
            first_output = launch(executable, unrelated_cwd, args.timeout_seconds, executable_log, environment)
            missing = [marker for marker in EXERCISE_MARKERS if marker not in first_output]
            if missing:
                raise RuntimeError(f"G19 Player omitted interaction evidence {missing}; inspect {executable_log}")

            user_data_root = Path(environment["PULSEFORGE_USER_DATA_DIR"]).resolve(strict=True)
            if user_data not in user_data_root.parents:
                raise RuntimeError("Player user settings escaped the isolated writable user-data root")
            if user_data_root == package or package in user_data_root.parents:
                raise RuntimeError("Player user settings resolved inside the read-only exported package")
            settings_files = list((user_data_root / "PulseForge" / "AudioSettings").glob("*.json"))
            if len(settings_files) != 1:
                raise RuntimeError(f"expected one project-scoped audio settings file, got {settings_files}")
            settings = json.loads(settings_files[0].read_text(encoding="utf-8"))
            if settings.get("projectId") != project["projectId"] or settings.get("version") != 1:
                raise RuntimeError("audio settings lack the expected format version or stable project identity")
            expected_gains = {"music": 0.9, "effects": 0.85, "ui": 0.9}
            saved_gains = settings.get("overrides")
            if not isinstance(saved_gains, dict) or set(saved_gains) != set(expected_gains):
                raise RuntimeError(f"saved user gains do not contain the expected buses: {saved_gains}")
            for bus, expected_gain in expected_gains.items():
                actual_gain = saved_gains[bus]
                if isinstance(actual_gain, bool) or not isinstance(actual_gain, (int, float)) or \
                        not math.isfinite(actual_gain) or abs(actual_gain - expected_gain) > 1.0e-5:
                    raise RuntimeError(
                        f"saved {bus} gain {actual_gain!r} does not match {expected_gain} within float tolerance")
            if settings.get("mutes", {}).get("music") is not True:
                raise RuntimeError("saved music mute state did not survive the sample interaction")

            environment["PULSEFORGE_AUDIO_MIXING_PACKAGE_ACCEPTANCE"] = "verify"
            verify_log = root / "g19-audio-mixing-restore.log"
            second_output = launch(executable, unrelated_cwd, args.timeout_seconds, verify_log, environment)
            missing = [marker for marker in VERIFY_MARKERS if marker not in second_output]
            if missing:
                raise RuntimeError(f"new Player process omitted settings restoration evidence {missing}; inspect {verify_log}")
            after = digest_tree(package)
            if before != after:
                raise RuntimeError("read-only relocated G19 package changed during audio interaction/settings restore")
            if not user_data.is_dir() or package in user_data.parents:
                raise RuntimeError("Player user settings were not kept outside the exported package")
            print("G19_AUDIO_MIXING_EXPORTED_PACKAGE=PASS; cooked sample; synthetic mapped UI actions; Null output; "
                  "pause/resume; isolated settings restored in a second process; package remained read-only")
        finally:
            read_only.restore()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"G19_AUDIO_MIXING_EXPORTED_PACKAGE=FAIL; {error}", file=sys.stderr)
        raise SystemExit(1)
