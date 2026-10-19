#!/usr/bin/env python3
"""Run relocated PulseForge editor and player bundles from unrelated working directories."""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str, log_path: Path | None = None) -> int:
    print(f"macOS bundle smoke failed: {message}", file=sys.stderr)
    if log_path and log_path.is_file():
        print(f"Output retained at: {log_path}", file=sys.stderr)
        print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    return 1


def run_bounded(command: list[str], cwd: Path, environment: dict[str, str], timeout: int, log_path: Path) -> tuple[int, str]:
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
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            output, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                output, _ = process.communicate(timeout=5)
            except subprocess.TimeoutExpired as error:
                output = error.stdout or ""
                if isinstance(output, bytes):
                    output = output.decode("utf-8", errors="replace")
                log_path.write_text(output, encoding="utf-8")
                raise TimeoutError("process group survived bounded SIGKILL cleanup")
        log_path.write_text(output, encoding="utf-8")
        raise TimeoutError(f"process exceeded the {timeout}s bound; process group was terminated")
    log_path.write_text(output, encoding="utf-8")
    print(output, end="")
    return process.returncode, output


def verify_output(name: str, output: str, exit_code: int, markers: tuple[str, ...]) -> None:
    if exit_code != 0:
        raise RuntimeError(f"{name} exited with status {exit_code}")
    for marker in markers:
        if marker not in output:
            raise RuntimeError(f"{name} output did not contain required evidence marker: {marker}")
    validation_errors = any(
        "NVRHI:" in line and ("[error]" in line.lower() or "[critical]" in line.lower())
        for line in output.splitlines()
    )
    if "VUID-" in output or "Vulkan error (" in output or validation_errors:
        raise RuntimeError(f"{name} emitted a Vulkan/NVRHI validation error")


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
            editor_environment = os.environ.copy()
            editor_environment["PULSEFORGE_EDITOR_SETTINGS_DIR"] = str(isolated_settings)
            editor_environment.pop("DYLD_LIBRARY_PATH", None)
            editor_code, editor_output = run_bounded(
                [str(editor_executable), "--project-open-render-test", str(editor_project_path)],
                editor_cwd,
                editor_environment,
                args.timeout_seconds,
                args.log_directory / "macos-editor-bundle-smoke.log",
            )
            verify_output(
                "editor",
                editor_output,
                editor_code,
                (
                    "Profile: Project open to first renderable viewport preparation took",
                    "Profile: Project open to first rendered viewport frame took",
                    "PULSEFORGE_EDITOR_PROJECT_RENDER_SMOKE=PASS; requesting orderly shutdown",
                    "PULSEFORGE_EDITOR_PROJECT_RENDER_SMOKE_SHUTDOWN=PASS",
                ),
            )
            print("MACOS_EDITOR_BUNDLE_SMOKE=PASS; relocated bundle/resources, external project with spaces and Ω, unrelated cwd, render callback, clean shutdown")

            player_environment = os.environ.copy()
            player_environment["PULSEFORGE_PLAYER_SMOKE_EXIT_AFTER_RENDER"] = "1"
            player_environment.pop("DYLD_LIBRARY_PATH", None)
            player_code, player_output = run_bounded(
                [str(player_executable)],
                player_cwd,
                player_environment,
                args.timeout_seconds,
                args.log_directory / "macos-player-bundle-smoke.log",
            )
            verify_output(
                "player",
                player_output,
                player_code,
                (
                    "PULSEFORGE_PLAYER_RUNTIME_SMOKE=PASS; scripting advanced; physics_steps=",
                    "PULSEFORGE_PLAYER_RELOCATION_SMOKE=PASS; rendered scene geometry after runtime advancement and requesting orderly shutdown",
                    "PULSEFORGE_PLAYER_RELOCATION_SMOKE_SHUTDOWN=PASS",
                ),
            )
            print("MACOS_PLAYER_BUNDLE_SMOKE=PASS; relocated bundle/resources, unrelated cwd, scene geometry render callback, clean shutdown")
            print("MACOS_RELOCATED_BUNDLE_SMOKES=PASS; startup/resource lookup/render callbacks/shutdown only; no pixel or native-dialog claim")
            shutil.rmtree(root)
            return 0
        except (OSError, shutil.Error, subprocess.SubprocessError, TimeoutError, RuntimeError) as error:
            print(f"Bundle evidence retained under: {root}", file=sys.stderr)
            return fail(str(error))
    except (OSError, RuntimeError) as error:
        return fail(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
