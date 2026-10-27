#!/usr/bin/env python3
"""Cook an isolated PBR project and prove the cooked Player works without its source tree."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
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
            raise RuntimeError(f"process exceeded {timeout}s and was stopped; inspect {log}")
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
            raise RuntimeError(f"negative process exceeded {timeout}s; inspect {log}")
    if return_code == 0:
        raise RuntimeError(f"invalid cooked input unexpectedly exited successfully; inspect {log}")
    if return_code != 1:
        raise RuntimeError(f"invalid cooked input did not use the controlled application failure exit (got {return_code}); inspect {log}")
    text = log.read_text(encoding="utf-8", errors="replace")
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cook", required=True, type=Path)
    parser.add_argument("--player", required=True, type=Path)
    parser.add_argument("--source-project", required=True, type=Path)
    parser.add_argument("--input-actions", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=("Windows", "Linux", "macOS"))
    parser.add_argument("--backend", default="Vulkan", choices=("Vulkan", "OpenGL"))
    parser.add_argument("--log-directory", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()

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
        cooked_root = root / "Cooked Content Ω"
        unrelated_cwd = root / "Unrelated Working Directory Ω"
        unrelated_cwd.mkdir()
        logs = args.log_directory.resolve()
        cook_log = logs / "cooked-player-acceptance-cook.log"
        cook_output = run_bounded([
            str(args.cook.resolve()), "--project", str(project_file), "--output", str(cooked_root),
            "--target", args.target, "--backend", args.backend, "--configuration", "Debug", "--dynamic", DYNAMIC_SCRIPT,
        ], unrelated_cwd, os.environ.copy(), args.timeout_seconds, cook_log)
        print(cook_output)
        manifest_path = cooked_root / "manifest.pfmanifest"
        if not manifest_path.is_file():
            raise RuntimeError("successful cook did not publish its manifest")
        verify_cook_manifest(manifest_path, DYNAMIC_SCRIPT)

        # This is a disposable copy created by this harness. Remove the authored project and its local cache
        # before process launch so only the cooked root and deployed runtime resources can satisfy the Player.
        shutil.rmtree(project_root)
        if project_root.exists() or scene_path.exists():
            raise RuntimeError("isolated authoring source remained available after removal")
        relocated_content = root / "Relocated Cooked Content Ω"
        cooked_root.rename(relocated_content)

        environment = os.environ.copy()
        environment["PULSEFORGE_COOKED_PLAYER_ACCEPTANCE"] = "1"
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
        absent = [marker for marker in required_markers if marker not in output]
        if absent:
            raise RuntimeError(f"cooked Player omitted evidence markers {absent}; inspect {player_log}")
        vulkan_errors = [line for line in output.splitlines()
                         if "VUID-" in line or ("Vulkan error (" in line and "id=Loader Message" not in line)]
        if vulkan_errors:
            raise RuntimeError(f"cooked Player emitted a Vulkan validation error; inspect {player_log}")

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

        resolved_parent = parent.resolve(strict=True)
        if root.parent != resolved_parent or not root.name.startswith("PulseForge G13 Cooked Acceptance Ω ") or root.is_symlink():
            raise RuntimeError(f"refusing cleanup outside owned acceptance root: {root}")
        shutil.rmtree(root)
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError, TimeoutError, ValueError, KeyError) as error:
        print(f"Acceptance artifacts retained at {root}", file=sys.stderr)
        return report_failure(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
