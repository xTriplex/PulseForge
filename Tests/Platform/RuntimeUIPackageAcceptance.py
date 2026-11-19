#!/usr/bin/env python3
"""Export and exercise RuntimeUI or the G29 reference game through a relocated cooked Player."""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import plistlib
import selectors
import shutil
import struct
import subprocess
import sys
import tempfile

from StandalonePackageDesktopSmoke import WindowsPackageReadOnlyAccess, _build_environment, _make_read_only


RUNTIME_UI_REQUIRED_MARKERS = (
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
G29_REQUIRED_MARKERS = (
    "PULSEFORGE_G29_PACKAGE_MENU=PASS",
    "PULSEFORGE_G29_PACKAGE_GAMEPLAY=PASS",
    "PULSEFORGE_G29_PACKAGE_RIG_ANIMATED=PASS",
    "PULSEFORGE_G29_PACKAGE_SPACE_SCORE=PASS",
    "PULSEFORGE_G29_PACKAGE_ACCEPTANCE=PASS",
    "PULSEFORGE_G29_PACKAGE_SHUTDOWN=PASS",
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


def export_target(platform: str) -> tuple[str, str, str]:
    if platform == "win32":
        return "Windows", "x64", "PulseForgeCookedGame.exe"
    if platform == "darwin":
        return "macOS", "arm64", "PulseForgeCookedGame.app"
    if platform.startswith("linux"):
        return "Linux", "x64", "PulseForgeCookedGame"
    raise RuntimeError(f"G29 package acceptance does not support host platform {platform!r}")


def resolve_exported_executable(package: Path, platform: str, executable_relative_path: str) -> Path:
    if platform == "darwin":
        # The exporter copies template files relative to the template root into
        # the package root. The package root itself is therefore the .app bundle.
        target = package
        info_path = target / "Contents" / "Info.plist"
        try:
            with info_path.open("rb") as stream:
                executable_name = plistlib.load(stream).get("CFBundleExecutable")
        except (OSError, plistlib.InvalidFileException) as error:
            raise RuntimeError(f"macOS package has no readable CFBundleExecutable in {info_path}: {error}") from error
        if not isinstance(executable_name, str) or not executable_name or Path(executable_name).name != executable_name:
            raise RuntimeError(f"macOS package has an invalid CFBundleExecutable value: {executable_name!r}")
        target = target / "Contents" / "MacOS" / executable_name
    else:
        target = package / executable_relative_path
    if not target.is_file():
        raise RuntimeError(f"exported Player executable is missing: {target}")
    return target


def find_lavapipe_manifest() -> Path:
    candidates = [Path("/usr/share/vulkan/icd.d") / "lvp_icd.x86_64.json",
                  Path("/usr/share/vulkan/icd.d") / "lvp_icd.json",
                  Path("/etc/vulkan/icd.d") / "lvp_icd.x86_64.json",
                  Path("/etc/vulkan/icd.d") / "lvp_icd.json"]
    manifest = next((path for path in candidates if path.is_file() and not path.is_symlink()), None)
    if manifest is None:
        raise RuntimeError("Linux G29 package acceptance requires the installed system Lavapipe ICD")
    return manifest


def build_player_launch_command(executable: Path, platform: str, has_display: bool) -> list[str]:
    command = [str(executable)]
    if platform.startswith("linux"):
        if not has_display:
            raise RuntimeError("headless Linux G29 package acceptance requires its owned Xvfb server")
    return command


def configure_linux_xvfb_environment(environment: dict[str, str]) -> None:
    """Remove any runner display so the isolated Xvfb connection is explicit."""
    environment.pop("DISPLAY", None)
    environment.pop("XAUTHORITY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    environment["XDG_SESSION_TYPE"] = "x11"


def verify_linux_xvfb_display(display: str) -> None:
    """Confirm the launched Player can reach the owned X server before export smoke."""
    library = ctypes.util.find_library("X11")
    if not library:
        raise RuntimeError("Linux G29 package acceptance requires libX11 to verify its isolated Xvfb display")
    x11 = ctypes.CDLL(library)
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    connection = x11.XOpenDisplay(display.encode("ascii"))
    if not connection:
        raise RuntimeError(f"the isolated Xvfb server is running but XOpenDisplay({display!r}) failed")
    x11.XCloseDisplay(connection)


def start_linux_xvfb(work_root: Path, environment: dict[str, str]) -> tuple[subprocess.Popen[bytes], str]:
    """Start an owned headless X server and read its collision-free display number."""
    xvfb = shutil.which("Xvfb", path=environment.get("PATH"))
    if not xvfb:
        raise RuntimeError("Linux G29 package acceptance requires the system Xvfb executable")
    diagnostic_path = work_root / "g29-xvfb.log"
    diagnostic = diagnostic_path.open("wb")
    process = subprocess.Popen(
        [xvfb, "-displayfd", "1", "-screen", "0", "1280x720x24", "-nolisten", "tcp", "-ac"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=diagnostic, env=environment, bufsize=0)
    display_number: str | None = None
    try:
        if process.stdout is None:
            raise RuntimeError("Xvfb display-number pipe was not created")
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=15):
                raise RuntimeError("Xvfb did not publish a display number within 15 seconds")
            display_line = process.stdout.readline().decode("ascii", errors="strict").strip()
        if not display_line.isdecimal():
            raise RuntimeError(f"Xvfb returned an invalid display number: {display_line!r}")
        if process.poll() is not None:
            raise RuntimeError(f"Xvfb exited with status {process.returncode} after selecting display {display_line}")
        display_number = display_line
        print(f"G29_XVFB_DISPLAY=:{display_number}; tcp=disabled; access=isolated-local")
        return process, f":{display_number}"
    except Exception as error:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        diagnostic.flush()
        diagnostic.close()
        detail = diagnostic_path.read_text(encoding="utf-8", errors="replace")[-4096:]
        raise RuntimeError(f"Could not start the owned Xvfb server: {error}; {detail}") from error
    finally:
        if not diagnostic.closed:
            diagnostic.close()


def stop_linux_xvfb(process: subprocess.Popen[bytes]) -> None:
    """Stop only the server started by start_linux_xvfb, with bounded teardown."""
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    if process.stdout is not None:
        process.stdout.close()


def run_with_linux_xvfb(command: list[str], cwd: Path, timeout: int, log: Path,
                        environment: dict[str, str], work_root: Path) -> str:
    server, display = start_linux_xvfb(work_root, environment)
    environment["DISPLAY"] = display
    try:
        verify_linux_xvfb_display(display)
        print(f"G29_XVFB_CONNECTION=PASS; DISPLAY={display}; XAUTHORITY=unset; WAYLAND_DISPLAY=unset")
        return run(command, cwd, timeout, log, environment)
    finally:
        stop_linux_xvfb(server)


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
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--export", type=Path)
    parser.add_argument("--template-root", type=Path)
    parser.add_argument("--template-descriptor", type=Path)
    parser.add_argument("--source-project", type=Path)
    parser.add_argument("--scenario", choices=("runtime-ui", "g29"), default="runtime-ui")
    parser.add_argument("--timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    if args.self_test:
        expected = {
            "win32": ("Windows", "x64", "PulseForgeCookedGame.exe"),
            "linux": ("Linux", "x64", "PulseForgeCookedGame"),
            "linux2": ("Linux", "x64", "PulseForgeCookedGame"),
            "darwin": ("macOS", "arm64", "PulseForgeCookedGame.app"),
        }
        for platform, target in expected.items():
            if export_target(platform) != target:
                raise RuntimeError(f"incorrect G29 export target for {platform}: {export_target(platform)}")
        if build_player_launch_command(Path("player"), "linux", True) != ["player"]:
            raise RuntimeError("Linux G29 package acceptance did not use its owned Xvfb display")
        try:
            build_player_launch_command(Path("player"), "linux", False)
        except RuntimeError:
            pass
        else:
            raise RuntimeError("Linux G29 package acceptance launched without its owned Xvfb display")
        LinuxEnvironment = {"PATH": "/usr/bin:/bin", "DISPLAY": ":109", "XAUTHORITY": "/tmp/stale-auth"}
        configure_linux_xvfb_environment(LinuxEnvironment)
        if LinuxEnvironment.get("XDG_SESSION_TYPE") != "x11" or "DISPLAY" in LinuxEnvironment or "XAUTHORITY" in LinuxEnvironment:
            raise RuntimeError("Linux G29 package acceptance retained runner X11 or Wayland display state")
        try:
            export_target("plan9")
        except RuntimeError:
            pass
        else:
            raise RuntimeError("unsupported G29 package host was accepted")
        with tempfile.TemporaryDirectory(prefix="pf-g29-plist-self-test-") as temporary:
            package = Path(temporary) / "Relocated Read-only Package Ω.app"
            package.mkdir()
            executable = package / "Contents" / "MacOS" / "renamed-player"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"test executable")
            with (package / "Contents" / "Info.plist").open("wb") as stream:
                plistlib.dump({"CFBundleExecutable": "renamed-player"}, stream)
            resolved = resolve_exported_executable(package, "darwin", "PulseForgeCookedGame.app")
            if resolved != executable:
                raise RuntimeError(f"macOS package did not resolve Info.plist executable name: {resolved}")
        print("G29_PACKAGE_HARNESS_SELF_TEST=PASS; Windows, Linux, macOS and unsupported host mapping")
        return 0

    if not all((args.export, args.template_root, args.template_descriptor, args.source_project)):
        parser.error("--export, --template-root, --template-descriptor, and --source-project are required")

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
        is_g29 = args.scenario == "g29"
        prefix = "G29" if is_g29 else "G18"
        target_platform, architecture, executable_relative_path = export_target(sys.platform)
        profile = {
            "format": "PulseForgeBuildProfile",
            "version": 1,
            "profileId": "b0290000-0000-4000-8000-0000000001f0" if is_g29 else "b0180000-0000-4000-8000-0000000001f0",
            "displayName": "G29 Reference Game Package Acceptance" if is_g29 else "G18 Runtime UI Package Acceptance",
            "targetPlatform": target_platform,
            "architecture": architecture,
            "configuration": "Debug",
            "renderer": "Vulkan",
            "startupScene": project["startScene"],
            # Runtime Lua chooses the gameplay scene by stable asset ID, so the package
            # profile must declare it as a dynamic cook root.
            "dynamicRoots": ["b0180000-0000-4000-8000-000000000011"],
            "defaultOutputPath": "Build/Exports/G29Acceptance" if is_g29 else "Build/Exports/G18Acceptance",
            "window": {"width": 960, "height": 540},
        }
        profile_path = root / f"{prefix} Acceptance.pfbuildprofile"
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

        package_name = "Relocated Read-only Package Ω.app" if sys.platform == "darwin" else "Relocated Read-only Package Ω"
        package = root / package_name
        staging_package.rename(package)
        shutil.rmtree(project_root)
        if project_root.exists():
            raise RuntimeError("source project remained available after export")
        executable = resolve_exported_executable(package, sys.platform, executable_relative_path)

        writable = root / "Isolated Writable User Data"
        writable.mkdir()
        runtime_library: Path | None = None
        system_icd: Path | None = None
        if target_platform == "Windows":
            runtime_library, runtime_identity = prepare_windows_vulkan_runtime(root, package)
        elif target_platform == "Linux":
            system_icd = find_lavapipe_manifest()
            runtime_identity = f"system Lavapipe ICD={system_icd}"
        else:
            runtime_identity = "relocated macOS application bundle; runner MoltenVK device"
        environment = _build_environment(target_platform, writable, SystemVulkanIcd=system_icd,
                                         WindowsVulkanRuntimeLibrary=runtime_library)
        if target_platform == "Linux":
            configure_linux_xvfb_environment(environment)
        if is_g29:
            environment["PULSEFORGE_G29_PACKAGE_ACCEPTANCE"] = "1"
        else:
            environment["PULSEFORGE_RUNTIME_UI_PACKAGE_ACCEPTANCE"] = "1"
        user_data = Path(environment["PULSEFORGE_USER_DATA_DIR"])
        print(f"G18_PACKAGE_VULKAN_RUNTIME=PASS; {runtime_identity}")

        read_only = WindowsPackageReadOnlyAccess(package, root) if target_platform == "Windows" else None
        read_only_modes: list[tuple[Path, int]] = []
        try:
            if read_only is not None:
                read_only.apply()
            else:
                for path in package.rglob("*"):
                    if path.is_symlink():
                        raise RuntimeError(f"refusing linked package entry: {path}")
                    read_only_modes.append((path, path.stat().st_mode))
                read_only_modes.append((package, package.stat().st_mode))
                _make_read_only(package)
            before = digest_tree(package)
            process_log = root / f"{args.scenario}-package-player.log"
            player_command = build_player_launch_command(executable, sys.platform,
                                                         target_platform == "Linux" or bool(os.environ.get("DISPLAY")))
            if target_platform == "Linux":
                output = run_with_linux_xvfb(player_command, unrelated_cwd, args.timeout_seconds,
                                             process_log, environment, root)
            else:
                output = run(player_command, unrelated_cwd, args.timeout_seconds, process_log, environment)
            required_markers = G29_REQUIRED_MARKERS if is_g29 else RUNTIME_UI_REQUIRED_MARKERS
            missing = [marker for marker in required_markers if marker not in output]
            if missing:
                raise RuntimeError(f"{prefix} package omitted behavior markers {missing}; inspect {process_log}\n{output[-16000:]}")
            after = digest_tree(package)
            if before != after:
                raise RuntimeError("read-only G18 package contents changed during the interaction run")
            user_data_prefix = ("PULSEFORGE_G29_PACKAGE_USER_DATA=PASS; root=" if is_g29 else
                               "PULSEFORGE_RUNTIME_UI_PACKAGE_USER_DATA=PASS; root=")
            user_data_lines = [line for line in output.splitlines() if user_data_prefix in line]
            if len(user_data_lines) != 1:
                raise RuntimeError(f"Player did not report exactly one resolved {prefix} user-data root")
            reported_user_data = Path(user_data_lines[0].split(user_data_prefix, 1)[1].strip())
            if not user_data.is_dir() or reported_user_data.resolve() != user_data.resolve():
                raise RuntimeError(f"Player resolved unexpected {prefix} user data: {reported_user_data}")
            if is_g29:
                print("G29_REFERENCE_EXPORTED_PACKAGE=PASS; source removed; unrelated CWD; package write denied; "
                      "mapped Menu→Start and Space; gameplay scene; animated rig pose; exact +10 score; normal exit")
            else:
                print("G18_RUNTIME_UI_EXPORTED_PACKAGE=PASS; source removed; unrelated CWD; package write denied; "
                      "synthetic mapped Player input; menu/gameplay/HUD/pause/resume; normal exit")
        finally:
            if read_only is not None:
                read_only.restore()
            else:
                for path, mode in reversed(read_only_modes):
                    if path.exists():
                        path.chmod(mode)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"EXPORTED_PACKAGE_ACCEPTANCE=FAIL; {error}", file=sys.stderr)
        raise SystemExit(1)
