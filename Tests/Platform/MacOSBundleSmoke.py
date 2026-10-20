#!/usr/bin/env python3
"""Run relocated PulseForge editor and player bundles from unrelated working directories."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

MAX_LOG_BYTES = 256 * 1024
MAX_CONSOLE_BYTES = 32 * 1024
MAX_ERROR_SAMPLES = 24


@dataclass
class CapturedRun:
    returncode: int
    output: str
    seen_markers: set[str]
    validation_error_count: int
    validation_error_samples: list[str]
    reader_error: str | None
    truncated: bool


def fail(message: str, log_path: Path | None = None) -> int:
    print(f"macOS bundle smoke failed: {message}", file=sys.stderr)
    if log_path and log_path.is_file():
        print(f"Output retained at: {log_path}", file=sys.stderr)
        print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    return 1


def stop_process_group(process: subprocess.Popen[str]) -> bool:
    process_group = process.pid
    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        return process.poll() is not None
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process_group, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            return False
    group_deadline = time.monotonic() + 0.5
    while time.monotonic() < group_deadline:
        try:
            os.killpg(process_group, 0)
        except ProcessLookupError:
            return process.poll() is not None
        time.sleep(0.025)
    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        return process.poll() is not None
    group_deadline = time.monotonic() + 5.0
    while time.monotonic() < group_deadline:
        try:
            os.killpg(process_group, 0)
        except ProcessLookupError:
            return process.poll() is not None
        time.sleep(0.05)
    return False


def run_bounded(
    command: list[str],
    cwd: Path,
    environment: dict[str, str],
    timeout: int,
    log_path: Path,
    required_markers: tuple[str, ...],
) -> CapturedRun:
    print("MACOS_BUNDLE_SMOKE_COMMAND=" + repr(command))
    print(f"MACOS_BUNDLE_SMOKE_CWD={cwd}")
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    captured = bytearray()
    required = set(required_markers)
    seen_markers: set[str] = set()
    validation_error_samples: list[str] = []
    validation_error_count = 0
    console_bytes = 0
    console_output_failed = False
    total_bytes = 0
    reader_error: str | None = None
    reader_finished = threading.Event()

    def read_output() -> None:
        nonlocal console_bytes, total_bytes, validation_error_count, reader_error, console_output_failed
        assert process.stdout is not None
        try:
            for line in process.stdout:
                encoded = line.encode("utf-8", errors="replace")
                total_bytes += len(encoded)
                if len(captured) < MAX_LOG_BYTES:
                    captured.extend(encoded[: MAX_LOG_BYTES - len(captured)])
                for marker in required:
                    if marker in line:
                        seen_markers.add(marker)
                lowered = line.lower()
                if (
                    "VUID-" in line
                    or "Vulkan error (" in line
                    or ("NVRHI:" in line and ("[error]" in lowered or "[critical]" in lowered))
                ):
                    validation_error_count += 1
                    if len(validation_error_samples) < MAX_ERROR_SAMPLES:
                        validation_error_samples.append(encoded[:2048].decode("utf-8", errors="replace"))
                if console_bytes < MAX_CONSOLE_BYTES and not console_output_failed:
                    printable_bytes = encoded[: MAX_CONSOLE_BYTES - console_bytes]
                    try:
                        print(printable_bytes.decode("utf-8", errors="replace"), end="")
                        console_bytes += len(printable_bytes)
                    except (OSError, ValueError) as error:
                        console_output_failed = True
                        reader_error = reader_error or (
                            f"could not forward {command[0]} output to the workflow log: {type(error).__name__}: {error}"
                        )
        except Exception as error:
            reader_error = f"{type(error).__name__}: {error}"
        finally:
            reader_finished.set()

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    timed_out = False
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if not stop_process_group(process):
            if process.stdout is not None:
                process.stdout.close()
            reader.join(timeout=3)
            log_path.write_bytes(captured)
            raise TimeoutError(f"bundle process group PID {process.pid} survived bounded timeout cleanup")
    reader.join(timeout=5)
    if reader.is_alive():
        stop_process_group(process)
        if process.stdout is not None:
            process.stdout.close()
        reader.join(timeout=3)
        log_path.write_bytes(captured)
        raise TimeoutError("process group or bundle output reader remained active after bounded cleanup")
    truncated = total_bytes > len(captured)
    log = bytes(captured)
    if truncated:
        suffix = b"\n[MACOS_BUNDLE_SMOKE_LOG=TRUNCATED; markers and validation errors were streamed separately]\n"
        log = log[: max(0, MAX_LOG_BYTES - len(suffix))] + suffix
    log_path.write_bytes(log)
    output = log.decode("utf-8", errors="replace")
    if timed_out:
        raise TimeoutError(f"process exceeded the {timeout}s bound; process group was terminated; log={log_path}")
    return CapturedRun(
        returncode=process.returncode,
        output=output,
        seen_markers=seen_markers,
        validation_error_count=validation_error_count,
        validation_error_samples=validation_error_samples,
        reader_error=reader_error,
        truncated=truncated,
    )


def verify_output(name: str, captured: CapturedRun, markers: tuple[str, ...]) -> None:
    if captured.returncode != 0:
        raise RuntimeError(f"{name} exited with status {captured.returncode}")
    if captured.reader_error:
        raise RuntimeError(f"{name} output reader failed: {captured.reader_error}")
    missing = [marker for marker in markers if marker not in captured.seen_markers]
    if missing:
        raise RuntimeError(f"{name} output did not contain required streamed evidence markers: {missing!r}")
    if captured.validation_error_count:
        raise RuntimeError(
            f"{name} emitted {captured.validation_error_count} Vulkan/NVRHI validation errors; "
            f"samples={captured.validation_error_samples!r}"
        )


def remove_temporary_root(root: Path, parent: Path) -> None:
    if root.is_symlink():
        raise RuntimeError(f"refusing to remove symlink bundle-smoke root: {root}")
    resolved_root = root.resolve(strict=True)
    resolved_parent = parent.resolve(strict=True)
    if resolved_root.parent != resolved_parent or not resolved_root.name.startswith("PulseForge G05 bundle Ω "):
        raise RuntimeError(f"refusing to remove unexpected bundle-smoke root: {resolved_root}")
    shutil.rmtree(resolved_root)


def require_resources(bundle: Path, executable_name: str, paths: tuple[Path, ...]) -> Path:
    resources = bundle / "Contents" / "Resources"
    executable = bundle / "Contents" / "MacOS" / executable_name
    for relative in paths:
        path = resources / relative
        exists_with_content = path.is_file() if path.is_file() else path.is_dir() and any(path.iterdir())
        if not exists_with_content:
            raise RuntimeError(f"{bundle.name} is missing deployed runtime resources: {path}")
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise RuntimeError(f"{bundle.name} executable is missing or not executable: {executable}")
    return executable


def sdk_assisted_runtime_environment() -> dict[str, str]:
    environment = os.environ.copy()
    sdk = environment.get("VULKAN_SDK")
    if not sdk or not Path(sdk).is_dir():
        raise RuntimeError("relocated developer-bundle smoke requires the configured Vulkan SDK runtime")
    icd_manifest = Path(sdk) / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json"
    if not icd_manifest.is_file():
        raise RuntimeError(f"relocated developer-bundle smoke requires the SDK MoltenVK ICD manifest: {icd_manifest}")
    layer_directory = Path(sdk) / "share" / "vulkan" / "explicit_layer.d"
    if not layer_directory.is_dir():
        raise RuntimeError(
            "relocated developer-bundle smoke requires the SDK validation layer directory: "
            f"{layer_directory}"
        )
    for name in tuple(environment):
        if name.startswith("DYLD_") or name.startswith("VK_"):
            environment.pop(name, None)
    environment["VK_DRIVER_FILES"] = str(icd_manifest)
    environment["VK_LAYER_PATH"] = str(layer_directory)
    print(
        "MACOS_BUNDLE_RUNTIME_CONTRACT=SDK_ASSISTED_DEVELOPER_BUNDLE; VULKAN_SDK, its MoltenVK ICD, and validation "
        "layers selected; inherited DYLD/VK overrides removed"
    )
    return environment


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor-bundle", required=True, type=Path)
    parser.add_argument("--editor-project", required=True, type=Path)
    parser.add_argument("--player-bundle", required=True, type=Path)
    parser.add_argument("--log-directory", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()

    try:
        editor_bundle = args.editor_bundle.resolve(strict=True)
        editor_project = args.editor_project.resolve(strict=True)
        player_bundle = args.player_bundle.resolve(strict=True)
        if any(bundle.suffix != ".app" or not bundle.is_dir() for bundle in (editor_bundle, player_bundle)):
            raise RuntimeError("both editor and player inputs must be built .app bundles")
        if not editor_project.is_file() or editor_project.suffix != ".pfproj":
            raise RuntimeError(f"expected an editor project file, got {editor_project}")
        if args.timeout_seconds <= 0:
            raise RuntimeError("timeout must be positive")

        runner_temp = os.environ.get("RUNNER_TEMP")
        temporary_parent = Path(runner_temp).resolve() if runner_temp else Path(tempfile.gettempdir())
        temporary_parent.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix="PulseForge G05 bundle Ω ", dir=temporary_parent))
        args.log_directory.mkdir(parents=True, exist_ok=True)
        try:
            relocated_editor = root / "Relocated Editor Ω" / editor_bundle.name
            relocated_player = root / "Relocated Player Ω" / player_bundle.name
            copied_project_root = root / "Project With Spaces Ω"
            editor_cwd = root / "Unrelated Editor Working Directory Ω"
            player_cwd = root / "Unrelated Player Working Directory Ω"
            relocated_editor.parent.mkdir(parents=True)
            relocated_player.parent.mkdir(parents=True)
            shutil.copytree(editor_bundle, relocated_editor)
            shutil.copytree(player_bundle, relocated_player)
            shutil.copytree(editor_project.parent, copied_project_root)
            editor_cwd.mkdir()
            player_cwd.mkdir()

            editor_executable = require_resources(
                relocated_editor,
                "PulseForgeEditor",
                (Path("Shaders"), Path("EditorAssets/Fonts"), Path("EditorAssets/Icons")),
            )
            player_executable = require_resources(
                relocated_player,
                "PulseForgeGame",
                (Path("Shaders"), Path("Assets/Scenes"), Path("Assets/Materials"), Path("PulseForgeGame.pfproj")),
            )

            editor_project_path = copied_project_root / editor_project.name
            isolated_settings = root / "Isolated Editor Settings Ω"
            isolated_settings.mkdir()
            editor_environment = sdk_assisted_runtime_environment()
            editor_environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(isolated_settings)
            editor_markers = (
                "Profile: Project open to first renderable viewport preparation took",
                "Profile: Project open to first rendered viewport frame took",
                "PULSEFORGE_EDITOR_PROJECT_RENDER_SMOKE=PASS; requesting orderly shutdown",
                "PULSEFORGE_EDITOR_PROJECT_RENDER_SMOKE_SHUTDOWN=PASS",
            )
            editor_output = run_bounded(
                [str(editor_executable), "--project-open-render-test", str(editor_project_path)],
                editor_cwd,
                editor_environment,
                args.timeout_seconds,
                args.log_directory / "macos-editor-bundle-smoke.log",
                editor_markers,
            )
            verify_output("editor", editor_output, editor_markers)
            print("MACOS_EDITOR_BUNDLE_SMOKE=PASS; relocated bundle/resources, external project with spaces and Ω, unrelated cwd, render callback, clean shutdown")

            player_environment = sdk_assisted_runtime_environment()
            player_environment["PULSEFORGE_PLAYER_SMOKE_EXIT_AFTER_RENDER"] = "1"
            player_markers = (
                "PULSEFORGE_PLAYER_RUNTIME_SMOKE=PASS; scripting advanced; physics_steps=",
                "PULSEFORGE_PLAYER_RELOCATION_SMOKE=PASS; rendered scene geometry after runtime advancement and requesting orderly shutdown",
                "PULSEFORGE_PLAYER_RELOCATION_SMOKE_SHUTDOWN=PASS",
            )
            player_output = run_bounded(
                [str(player_executable)],
                player_cwd,
                player_environment,
                args.timeout_seconds,
                args.log_directory / "macos-player-bundle-smoke.log",
                player_markers,
            )
            verify_output("player", player_output, player_markers)
            print("MACOS_PLAYER_BUNDLE_SMOKE=PASS; relocated bundle/resources, unrelated cwd, scene geometry render callback, clean shutdown")
            print("MACOS_RELOCATED_BUNDLE_SMOKES=PASS; startup/resource lookup/render callbacks/shutdown only; no pixel or native-dialog claim")
            remove_temporary_root(root, temporary_parent)
            return 0
        except (OSError, shutil.Error, subprocess.SubprocessError, TimeoutError, RuntimeError) as error:
            print(f"Bundle evidence retained under: {root}", file=sys.stderr)
            return fail(str(error))
    except (OSError, RuntimeError) as error:
        return fail(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
