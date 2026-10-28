#!/usr/bin/env python3
"""Cook an isolated PBR project and prove the cooked Player works without its source tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import wave

MAX_CAPTURE_BYTES = 1024 * 1024
PROBE_SCRIPT = "bfc10000-0000-4000-8000-000000000001"
DYNAMIC_SCRIPT = "bfc10000-0000-4000-8000-000000000002"
PHYSICS_ENTITY = "bfc10000-0000-4000-8000-000000000003"
AUDIO_ENTITY = "bfc10000-0000-4000-8000-000000000004"
SCRIPT_ENTITY = "bfc10000-0000-4000-8000-000000000005"
AUDIO_ASSET = "bfc10000-0000-4000-8000-000000000006"


def read_log_tail(log: Path) -> str:
    try:
        with log.open("rb") as captured:
            captured.seek(0, os.SEEK_END)
            length = captured.tell()
            captured.seek(max(0, length - MAX_CAPTURE_BYTES))
            return captured.read(MAX_CAPTURE_BYTES).decode("utf-8", errors="replace")
    except OSError as error:
        return f"Could not read process log {log}: {error}"


def require_markers(output: str, markers: tuple[str, ...], log: Path, product: str) -> None:
    absent = [marker for marker in markers if marker not in output]
    if absent:
        raise RuntimeError(f"{product} omitted evidence markers {absent}; inspect {log}\n{read_log_tail(log)}")


def make_package_read_only(package_root: Path, executable: Path) -> None:
    """Remove package write permission without stripping the entrypoint's execute bit."""
    for package_path in sorted(package_root.rglob("*"), reverse=True):
        if package_path.is_symlink():
            raise RuntimeError(f"refusing a linked path in the relocated standalone package: {package_path}")
        if package_path.is_dir():
            package_path.chmod(0o555)
        elif package_path == executable:
            package_path.chmod(0o555)
        else:
            package_path.chmod(0o444)
    package_root.chmod(0o555)


def capture_package_tree(package_root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for package_path in sorted(package_root.rglob("*")):
        if package_path.is_symlink():
            raise RuntimeError(f"refusing a linked path in the standalone package: {package_path}")
        if package_path.is_file():
            digest = hashlib.sha256()
            with package_path.open("rb") as package_file:
                for block in iter(lambda: package_file.read(1024 * 1024), b""):
                    digest.update(block)
            result[package_path.relative_to(package_root).as_posix()] = digest.hexdigest()
    return result


def remove_owned_acceptance_root(root: Path, parent: Path) -> None:
    """Remove only this harness's temporary root, reopening read-only package directories safely."""
    resolved_parent = parent.resolve(strict=True)
    if (root.is_symlink() or root.parent.resolve(strict=True) != resolved_parent
            or not root.name.startswith("PulseForge G13 Cooked Acceptance Ω ")
            or root.resolve(strict=True) != root):
        raise RuntimeError(f"refusing cleanup outside owned acceptance root: {root}")

    def restore_write_access(function, path_text: str, _error: object) -> None:
        path = Path(path_text)
        try:
            path.relative_to(root)
        except ValueError as error:
            raise RuntimeError(f"refusing to change permissions outside acceptance root: {path}") from error
        if path.is_symlink():
            path.unlink()
            return
        path.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        parent_path = path.parent
        try:
            parent_path.relative_to(root)
        except ValueError as error:
            raise RuntimeError(f"refusing to change permissions outside acceptance root: {parent_path}") from error
        if not parent_path.is_symlink():
            parent_path.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        function(path)

    shutil.rmtree(root, onerror=restore_write_access)


def report_failure(message: str, log: Path | None = None) -> int:
    print(f"COOKED_PLAYER_ACCEPTANCE=FAIL; {message}", file=sys.stderr)
    if log and log.is_file():
        print(f"Process log retained at {log}", file=sys.stderr)
        print(log.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    return 1


def run_bounded(command: list[str], cwd: Path, environment: dict[str, str], timeout: int, log: Path) -> str:
    print("COOKED_PLAYER_ACCEPTANCE_COMMAND=" + repr(command))
    print(f"COOKED_PLAYER_ACCEPTANCE_CWD={cwd}")
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=cwd, env=environment, stdout=output, stderr=subprocess.STDOUT)
        try:
            return_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            raise RuntimeError(f"process exceeded {timeout}s and was stopped; inspect {log}\n{read_log_tail(log)}")
    with log.open("rb") as captured:
        captured.seek(0, os.SEEK_END)
        length = captured.tell()
        captured.seek(max(0, length - MAX_CAPTURE_BYTES))
        text = captured.read(MAX_CAPTURE_BYTES).decode("utf-8", errors="replace")
    if return_code != 0:
        raise RuntimeError(f"process exited with status {return_code}; inspect {log}\n{text}")
    return text


def run_expected_failure(command: list[str], cwd: Path, environment: dict[str, str], timeout: int,
                         log: Path, expected_diagnostic: str) -> str:
    print("COOKED_PLAYER_NEGATIVE_COMMAND=" + repr(command))
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=cwd, env=environment, stdout=output, stderr=subprocess.STDOUT)
        try:
            return_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            raise RuntimeError(f"negative process exceeded {timeout}s; inspect {log}\n{read_log_tail(log)}")
    if return_code == 0:
        raise RuntimeError(f"invalid cooked input unexpectedly exited successfully; inspect {log}\n{read_log_tail(log)}")
    if return_code != 1:
        raise RuntimeError(f"invalid cooked input did not use the controlled application failure exit (got {return_code}); "
                           f"inspect {log}\n{read_log_tail(log)}")
    text = read_log_tail(log)
    if expected_diagnostic.casefold() not in text.casefold():
        raise RuntimeError(f"controlled failure omitted {expected_diagnostic!r}; inspect {log}\n{text[-MAX_CAPTURE_BYTES:]}")
    if "VUID-" in text or "Validation Error" in text:
        raise RuntimeError(f"negative process produced a renderer validation error; inspect {log}")
    return text


def write_meta(path: Path, identifier: str) -> None:
    path.with_name(path.name + ".meta").write_text(
        json.dumps({"format": "PulseForgeAssetMeta", "version": 1, "uuid": identifier, "importSettings": {}}, indent=2) + "\n",
        encoding="utf-8",
    )


def make_acceptance_project(source_project: Path, input_actions: Path, destination: Path) -> tuple[Path, Path]:
    shutil.copytree(source_project.parent, destination, symlinks=True, ignore=shutil.ignore_patterns(".pulseforge"))
    for path in destination.rglob("*"):
        if path.is_symlink():
            raise RuntimeError(f"refusing a linked path in the copied acceptance project: {path}")

    scene_path = destination / "Assets" / "Scenes" / "PBRMaterialsOverview.scene"
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    scene["entities"].append({
        "uuid": SCRIPT_ENTITY,
        "tag": {"name": "Cooked Lua process probe"},
        "transform": {"translation": [0.0, 0.0, 0.0], "rotation": [1.0, 0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
        "script": {"asset": PROBE_SCRIPT, "enabled": True},
        "parent": None,
    })
    scene["entities"].append({
        "uuid": PHYSICS_ENTITY,
        "tag": {"name": "Cooked physics process probe"},
        "transform": {"translation": [0.0, 3.0, 0.0], "rotation": [1.0, 0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
        "rigidbody": {"motionType": "dynamic", "mass": 1.0, "friction": 0.2, "restitution": 0.0, "allowSleeping": False},
        "boxCollider": {"halfExtents": [0.5, 0.5, 0.5]},
        "parent": None,
    })
    camera = next(entity for entity in scene["entities"] if entity.get("camera", {}).get("primary"))
    camera["audioListener"] = {"primary": True}
    scene["entities"].append({
        "uuid": AUDIO_ENTITY,
        "tag": {"name": "Cooked Null audio process probe"},
        "transform": {"translation": [0.0, 0.0, 0.0], "rotation": [1.0, 0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
        "audioSource": {"asset": AUDIO_ASSET, "volume": 0.1, "looping": True, "playOnStart": True, "spatialized": False},
        "parent": None,
    })
    scene_path.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")

    scripts = destination / "Assets" / "Scripts"
    scripts.mkdir(parents=True, exist_ok=True)
    probe = scripts / "CookedRuntimeProbe.lua"
    probe.write_text(
        "local changed = false\nfunction OnUpdate(deltaTime)\n"
        "  if not changed then\n"
        "    local updated, message = entity:set_translation(12.5, 0.0, 0.0)\n"
        "    assert(updated, message)\n    changed = true\n  end\nend\n",
        encoding="utf-8",
    )
    write_meta(probe, PROBE_SCRIPT)
    dynamic = scripts / "DynamicOnlyProbe.lua"
    dynamic.write_text("function OnCreate() return true end\n", encoding="utf-8")
    write_meta(dynamic, DYNAMIC_SCRIPT)

    audio = destination / "Assets" / "Audio" / "AcceptanceTone.wav"
    audio.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(audio), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(22050)
        output.writeframes(b"\x00\x00" * 2205)
    write_meta(audio, AUDIO_ASSET)
    shutil.copy2(input_actions, destination / "InputActions.json")
    return destination / source_project.name, scene_path


def verify_cook_manifest(path: Path, dynamic_root: str) -> None:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    products = {entry["product"] for entry in manifest["entries"]}
    required = {"scene", "mesh-primitive", "material", "texture", "environment", "script", "audio", "gameplay-input"}
    missing = sorted(required - products)
    if missing:
        raise RuntimeError(f"cook omitted required acceptance products: {missing}")
    if dynamic_root not in manifest["dynamicRoots"]:
        raise RuntimeError("cook manifest did not retain the explicit dynamic script root")
    if manifest.get("configuration") not in {"Debug", "Release", "Dist"}:
        raise RuntimeError("cook manifest did not record a supported build configuration")


def make_linux_standalone_environment(inherited: dict[str, str]) -> dict[str, str]:
    """Retain only the virtual X11 connection needed by the package-only runtime check."""
    environment = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C.UTF-8"}
    for variable in ("DISPLAY", "XAUTHORITY"):
        value = inherited.get(variable)
        if value:
            environment[variable] = value
    return environment


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cook", required=True, type=Path)
    parser.add_argument("--player", required=True, type=Path)
    parser.add_argument("--source-project", required=True, type=Path)
    parser.add_argument("--input-actions", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=("Windows", "Linux", "macOS"))
    parser.add_argument("--backend", default="Vulkan", choices=("Vulkan", "OpenGL"))
    parser.add_argument("--configuration", default="Debug", choices=("Debug", "Release", "Dist"),
                        help="Cook and export build configuration; detailed in-process assertions remain Debug-only")
    parser.add_argument("--export-only", action="store_true",
                        help="Cook and retain a package without launching it; use the independent desktop verifier next")
    parser.add_argument("--export", type=Path, help="Optional PulseForgeExport executable")
    parser.add_argument("--template-root", type=Path, help="Built cooked-player template root")
    parser.add_argument("--template-descriptor", type=Path, help="Machine-readable template descriptor")
    parser.add_argument("--runtime-driver-manifest", type=Path,
                        help="Optional platform Vulkan driver manifest for an isolated Linux software-driver test")
    parser.add_argument("--retain-export-to", type=Path,
                        help="Copy the verified exported package to this new, separately owned artifact path")
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()

    if args.export_only and not all((args.export, args.template_root, args.template_descriptor, args.retain_export_to)):
        parser.error("--export-only requires --export, --template-root, --template-descriptor, and --retain-export-to")

    root: Path | None = None
    try:
        for executable in (args.cook, args.player):
            if not executable.is_file():
                raise RuntimeError(f"executable does not exist: {executable}")
        if not args.source_project.is_file() or not args.input_actions.is_file():
            raise RuntimeError("acceptance project and input configuration must exist")
        source_project = args.source_project.resolve(strict=True)
        args.log_directory.mkdir(parents=True, exist_ok=True)
        parent = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())).resolve()
        parent.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix="PulseForge G13 Cooked Acceptance Ω ", dir=parent)).resolve(strict=True)
        project_root = root / "Source Project Ω"
        project_file, scene_path = make_acceptance_project(source_project, args.input_actions.resolve(strict=True), project_root)
        runtime_driver_manifest: Path | None = None
        if args.runtime_driver_manifest:
            if args.target != "Linux":
                raise RuntimeError("--runtime-driver-manifest is reserved for Linux platform-driver acceptance")
            runtime_driver_manifest = args.runtime_driver_manifest.resolve(strict=True)
            if not runtime_driver_manifest.is_file():
                raise RuntimeError(f"platform Vulkan driver manifest is not a file: {runtime_driver_manifest}")
        cooked_root = root / "Cooked Content Ω"
        unrelated_cwd = root / "Unrelated Working Directory Ω"
        unrelated_cwd.mkdir()
        logs = args.log_directory.resolve()
        cook_log = logs / "cooked-player-acceptance-cook.log"
        cook_output = run_bounded([
            str(args.cook.resolve()), "--project", str(project_file), "--output", str(cooked_root),
            "--target", args.target, "--backend", args.backend, "--configuration", args.configuration, "--dynamic", DYNAMIC_SCRIPT,
        ], unrelated_cwd, os.environ.copy(), args.timeout_seconds, cook_log)
        print(cook_output)
        manifest_path = cooked_root / "manifest.pfmanifest"
        if not manifest_path.is_file():
            raise RuntimeError("successful cook did not publish its manifest")
        verify_cook_manifest(manifest_path, DYNAMIC_SCRIPT)

        exported_package: Path | None = None
        if args.export or args.template_root or args.template_descriptor:
            if not all((args.export, args.template_root, args.template_descriptor)):
                raise RuntimeError("export acceptance requires --export, --template-root, and --template-descriptor together")
            project_description = json.loads(project_file.read_text(encoding="utf-8"))
            profile = {
                "format": "PulseForgeBuildProfile", "version": 1,
                "profileId": "bfc20000-0000-4000-8000-000000000001",
                "displayName": "Standalone Acceptance Ω", "targetPlatform": args.target,
                "architecture": "arm64" if args.target == "macOS" else "x64",
                "configuration": args.configuration, "renderer": "Vulkan",
                "startupScene": project_description["startScene"], "dynamicRoots": [DYNAMIC_SCRIPT],
                "defaultOutputPath": "Build/Exports/Acceptance", "window": {"width": 960, "height": 540},
            }
            profile_path = root / "Standalone Acceptance Ω.pfbuildprofile"
            profile_path.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
            exported_package = root / ("Standalone Cooked Game.app" if args.target == "macOS" else "Standalone Cooked Package")
            export_output = run_bounded([
                str(args.export.resolve()), "--profile", str(profile_path), "--project", str(project_file),
                "--template-root", str(args.template_root.resolve()), "--template-descriptor",
                str(args.template_descriptor.resolve()), "--output", str(exported_package),
            ], unrelated_cwd, os.environ.copy(), args.timeout_seconds, logs / "standalone-package-export.log")
            print(export_output)
            if not exported_package.is_dir():
                raise RuntimeError("export service returned success without publishing its package directory")

        # This is a disposable copy created by this harness. Remove the authored project and its local cache
        # before process launch so only the cooked root and deployed runtime resources can satisfy the Player.
        shutil.rmtree(project_root)
        if project_root.exists() or scene_path.exists():
            raise RuntimeError("isolated authoring source remained available after removal")
        if args.export_only:
            if exported_package is None or not exported_package.is_dir() or args.retain_export_to is None:
                raise RuntimeError("export-only acceptance did not produce a package")
            if args.target == "macOS":
                executable = exported_package / "Contents" / "MacOS" / "PulseForgeCookedGame"
            else:
                executable = exported_package / ("PulseForgeCookedGame.exe" if args.target == "Windows" else "PulseForgeCookedGame")
            if not executable.is_file():
                raise RuntimeError(f"exported executable is missing from its package: {executable}")
            retain_path = args.retain_export_to.absolute()
            try:
                retain_path.relative_to(root)
            except ValueError:
                pass
            else:
                raise RuntimeError("retained package destination must be outside the temporary acceptance root")
            if retain_path.exists() or retain_path.is_symlink():
                raise RuntimeError(f"retained package destination must not already exist: {retain_path}")
            if not retain_path.parent.resolve(strict=True).is_dir() or retain_path.parent.is_symlink():
                raise RuntimeError(f"retained package parent must be an existing, non-linked directory: {retain_path.parent}")
            shutil.copytree(exported_package, retain_path, symlinks=False)
            if any(path.is_symlink() for path in retain_path.rglob("*")):
                raise RuntimeError(f"refusing to retain a package containing a linked entry: {retain_path}")
            print(f"STANDALONE_PACKAGE_RETAINED={retain_path}")
            print(f"STANDALONE_PACKAGE_PRODUCTION=PASS; configuration={args.configuration}; source removed; external launch pending")
            remove_owned_acceptance_root(root, parent)
            return 0
        relocated_content = root / "Relocated Cooked Content Ω"
        cooked_root.rename(relocated_content)

        environment = os.environ.copy()
        environment["PULSEFORGE_COOKED_PLAYER_ACCEPTANCE"] = "1"
        if runtime_driver_manifest:
            environment["VK_DRIVER_FILES"] = str(runtime_driver_manifest)
            environment["VK_ICD_FILENAMES"] = str(runtime_driver_manifest)
        if sys.platform == "darwin":
            from MacOSBundleSmoke import sdk_assisted_runtime_environment

            environment = sdk_assisted_runtime_environment()
            environment["PULSEFORGE_COOKED_PLAYER_ACCEPTANCE"] = "1"
        player_log = logs / "cooked-player-acceptance-player.log"
        output = run_bounded([
            str(args.player.resolve()), "--cooked-content", str(relocated_content),
        ], unrelated_cwd, environment, args.timeout_seconds, player_log)
        required_markers = (
            "PULSEFORGE_COOKED_DYNAMIC_ROOT=PASS",
            "PULSEFORGE_COOKED_STARTUP_SCENE=PASS",
            "PULSEFORGE_COOKED_INPUT_MAP=PASS",
            "PULSEFORGE_COOKED_AUDIO_BACKEND=NULL",
            "PULSEFORGE_COOKED_AUDIO=PASS",
            "PULSEFORGE_COOKED_RUNTIME=PASS",
            "PULSEFORGE_COOKED_SCRIPT=PASS",
            "PULSEFORGE_COOKED_ENVIRONMENT=PASS",
            "PULSEFORGE_COOKED_RENDER_READBACK=PASS",
            "PULSEFORGE_COOKED_PLAYER_ACCEPTANCE=PASS",
            "PULSEFORGE_COOKED_PLAYER_SHUTDOWN=PASS",
        )
        require_markers(output, required_markers, player_log, "cooked Player")
        vulkan_errors = [line for line in output.splitlines()
                         if "VUID-" in line or ("Vulkan error (" in line and "id=Loader Message" not in line)]
        if vulkan_errors:
            raise RuntimeError(f"cooked Player emitted a Vulkan validation error; inspect {player_log}\n"
                               f"{read_log_tail(player_log)}")

        if exported_package is not None:
            relocated_package = root / ("Relocated Standalone Game.app" if args.target == "macOS" else "Relocated Standalone Package")
            exported_package.rename(relocated_package)
            if args.target == "macOS":
                executable = relocated_package / "Contents" / "MacOS" / "PulseForgeCookedGame"
            else:
                executable = relocated_package / ("PulseForgeCookedGame.exe" if args.target == "Windows" else "PulseForgeCookedGame")
            if not executable.is_file():
                raise RuntimeError(f"exported executable is missing from the relocated package: {executable}")
            make_package_read_only(relocated_package, executable)
            package_before_launch = capture_package_tree(relocated_package)
            if os.name == "nt":
                system_root = Path(os.environ.get("SystemRoot", r"C:\Windows"))
                standalone_environment = {"PATH": str(system_root / "System32"), "SystemRoot": str(system_root)}
                for variable in ("WINDIR", "SystemDrive"):
                    if variable in os.environ:
                        standalone_environment[variable] = os.environ[variable]
            elif sys.platform == "darwin":
                standalone_environment = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C.UTF-8"}
                standalone_environment["TMPDIR"] = str(Path(tempfile.gettempdir()))
                standalone_environment["DYLD_PRINT_LIBRARIES"] = "1"
            else:
                standalone_environment = make_linux_standalone_environment(os.environ)
            standalone_environment["PULSEFORGE_COOKED_PLAYER_ACCEPTANCE"] = "1"
            if runtime_driver_manifest:
                standalone_environment["VK_DRIVER_FILES"] = str(runtime_driver_manifest)
                standalone_environment["VK_ICD_FILENAMES"] = str(runtime_driver_manifest)
            writable_data = root / "Standalone Writable User Data"
            writable_data.mkdir()
            user_data_root = writable_data / "PulseForge"
            user_data_root.mkdir()
            standalone_environment["PULSEFORGE_USER_DATA_DIR"] = str(user_data_root)
            standalone_environment["HOME"] = str(writable_data)
            standalone_environment["XDG_CONFIG_HOME"] = str(writable_data / "config")
            standalone_environment["XDG_DATA_HOME"] = str(writable_data / "data")
            standalone_environment["TMPDIR"] = str(writable_data / "tmp")
            if args.target == "Windows":
                standalone_environment["APPDATA"] = str(writable_data / "Roaming")
                standalone_environment["LOCALAPPDATA"] = str(writable_data / "Local")
                standalone_environment["TEMP"] = str(writable_data / "Temp")
                standalone_environment["TMP"] = str(writable_data / "Temp")
                system_root = Path(os.environ["SystemRoot"])
                standalone_environment["PATH"] = str(system_root / "System32")
                standalone_environment["SystemRoot"] = str(system_root)
            for directory in ("config", "data", "tmp", "Roaming", "Local", "Temp"):
                (writable_data / directory).mkdir(exist_ok=True)
            package_output = run_bounded([str(executable.resolve())], unrelated_cwd, standalone_environment,
                                         args.timeout_seconds, logs / "standalone-package-player.log")
            require_markers(package_output, required_markers, logs / "standalone-package-player.log",
                            "relocated standalone Player")
            user_data_prefix = "PULSEFORGE_COOKED_USER_DATA=PASS; root="
            user_data_lines = [line for line in package_output.splitlines() if user_data_prefix in line]
            if len(user_data_lines) != 1:
                raise RuntimeError("standalone player did not report exactly one resolved user-data root")
            reported_user_data = Path(user_data_lines[0].split(user_data_prefix, 1)[1].strip())
            if reported_user_data.resolve() != user_data_root.resolve():
                raise RuntimeError(f"standalone player resolved unexpected user data: {reported_user_data}")
            package_vulkan_errors = [line for line in package_output.splitlines()
                                     if "VUID-" in line or ("Vulkan error (" in line and "id=Loader Message" not in line)]
            if package_vulkan_errors:
                package_log = logs / "standalone-package-player.log"
                raise RuntimeError(f"relocated standalone Player emitted a Vulkan validation error; "
                                   f"inspect {package_log}\n{read_log_tail(package_log)}")
            standalone_markers = ()
            if args.target == "macOS":
                standalone_markers = (
                    "PULSEFORGE_BUNDLED_VULKAN_RUNTIME=PASS;",
                )
            require_markers(package_output, standalone_markers, logs / "standalone-package-player.log",
                            "standalone platform runtime")
            if args.target == "macOS":
                bundled_moltenvk = str(relocated_package / "Contents" / "Frameworks" / "libMoltenVK.dylib")
                if bundled_moltenvk not in package_output:
                    raise RuntimeError("standalone Player did not load MoltenVK from its relocated app bundle")
            if capture_package_tree(relocated_package) != package_before_launch:
                raise RuntimeError("standalone Player changed content in its read-only package")
            print("STANDALONE_PACKAGE_ACCEPTANCE=PASS; relocated export ran with source/cache removed and developer Vulkan overrides absent")
            if args.retain_export_to:
                retain_path = args.retain_export_to.absolute()
                try:
                    retain_path.relative_to(root)
                except ValueError:
                    pass
                else:
                    raise RuntimeError("retained package destination must be outside the temporary acceptance root")
                if retain_path.exists() or retain_path.is_symlink():
                    raise RuntimeError(f"retained package destination must not already exist: {retain_path}")
                retain_parent = retain_path.parent.resolve(strict=True)
                if retain_path.parent.is_symlink() or not retain_parent.is_dir():
                    raise RuntimeError(f"retained package parent must be an existing, non-linked directory: {retain_path.parent}")
                shutil.copytree(relocated_package, retain_path, symlinks=False)
                for item in retain_path.rglob("*"):
                    if item.is_symlink():
                        raise RuntimeError(f"refusing to retain a package containing a linked entry: {item}")
                print(f"STANDALONE_PACKAGE_RETAINED={retain_path}")

        missing_root = root / "Missing Manifest Ω"
        missing_root.mkdir()
        run_expected_failure([str(args.player.resolve()), "--cooked-content", str(missing_root)], unrelated_cwd,
                             environment, min(args.timeout_seconds, 30), logs / "cooked-player-missing-manifest.log", "manifest")

        incompatible_root = root / "Incompatible Cooked Content Ω"
        shutil.copytree(relocated_content, incompatible_root)
        incompatible_manifest = incompatible_root / "manifest.pfmanifest"
        incompatible = json.loads(incompatible_manifest.read_text(encoding="utf-8"))
        incompatible["rendererBackend"] = "OpenGL" if args.backend == "Vulkan" else "Vulkan"
        incompatible_manifest.write_text(json.dumps(incompatible, sort_keys=True, separators=(",", ":")) + "\n",
                                         encoding="utf-8")
        run_expected_failure([str(args.player.resolve()), "--cooked-content", str(incompatible_root)], unrelated_cwd,
                             environment, min(args.timeout_seconds, 30), logs / "cooked-player-incompatible.log", "incompatible")

        missing_payload_root = root / "Missing Payload Cooked Content Ω"
        shutil.copytree(relocated_content, missing_payload_root)
        parsed_manifest = json.loads((missing_payload_root / "manifest.pfmanifest").read_text(encoding="utf-8"))
        scene_entry = next(entry for entry in parsed_manifest["entries"] if entry["product"] == "scene")
        (missing_payload_root / scene_entry["payload"]).unlink()
        run_expected_failure([str(args.player.resolve()), "--cooked-content", str(missing_payload_root)], unrelated_cwd,
                             environment, min(args.timeout_seconds, 30), logs / "cooked-player-missing-payload.log", "payload")

        corrupt_payload_root = root / "Corrupt Payload Cooked Content Ω"
        shutil.copytree(relocated_content, corrupt_payload_root)
        parsed_manifest = json.loads((corrupt_payload_root / "manifest.pfmanifest").read_text(encoding="utf-8"))
        scene_entry = next(entry for entry in parsed_manifest["entries"] if entry["product"] == "scene")
        corrupt_scene = corrupt_payload_root / scene_entry["payload"]
        corrupt_scene.write_bytes(b"\xa5" * scene_entry["bytes"])
        run_expected_failure([str(args.player.resolve()), "--cooked-content", str(corrupt_payload_root)], unrelated_cwd,
                             environment, min(args.timeout_seconds, 30), logs / "cooked-player-corrupt-payload.log", "digest")
        print("COOKED_PLAYER_ACCEPTANCE=PASS; production CLI cook, isolated source/cache removal, relocated provider, "
              "scene/runtime/input/Lua/physics/Null-audio, environment, indexed mesh and HDR/depth readback, "
              "orderly exit 0, missing/incompatible/corrupt content failures controlled")

        remove_owned_acceptance_root(root, parent)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError, TimeoutError, ValueError, KeyError) as error:
        print(f"Acceptance artifacts retained at {root}", file=sys.stderr)
        return report_failure(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
