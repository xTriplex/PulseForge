#!/usr/bin/env python3
"""Apply a bounded workflow_dispatch patch and record its exact source identity."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path, PurePosixPath, PureWindowsPath


MAX_EVENT_BYTES = 256 * 1024
MAX_COMPRESSED_BYTES = 8 * 1024 * 1024
MAX_PATCH_BYTES = 32 * 1024 * 1024
MAX_CHANGED_PATHS = 1_000
MAX_CHANGED_PATH_BYTES = 100 * 1024
MAX_BINARY_SECTION_BYTES = 4 * 1024 * 1024
MAX_BINARY_TOTAL_BYTES = 8 * 1024 * 1024
SUPPORTED_SELECTIONS = {
    "full",
    "automation-probe-only",
    "macos-desktop-only",
    "windows-asset-diagnostic-only",
}
SHA1_PATTERN = re.compile(r"^[0-9a-fA-F]{40}$")
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")


class PatchValidationError(RuntimeError):
    pass


def git(repository: Path, arguments: list[str], data: bytes | None = None,
        env: dict[str, str] | None = None, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        ["git", *arguments],
        cwd=repository,
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        check=False,
    )
    if check and result.returncode != 0:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise PatchValidationError(f"git {' '.join(arguments[:3])} failed: {message}")
    return result


def decode_patch(compressed: bytes, expected_sha256: str, expected_compressed_sha256: str) -> bytes:
    if len(compressed) > MAX_COMPRESSED_BYTES:
        raise PatchValidationError(
            f"compressed patch is {len(compressed)} bytes; limit is {MAX_COMPRESSED_BYTES}"
        )
    if not SHA256_PATTERN.fullmatch(expected_compressed_sha256):
        raise PatchValidationError("compressed-file SHA-256 must contain exactly 64 hexadecimal characters")
    actual_compressed_sha256 = hashlib.sha256(compressed).hexdigest()
    if actual_compressed_sha256.lower() != expected_compressed_sha256.lower():
        raise PatchValidationError(
            f"compressed-file SHA-256 mismatch: expected {expected_compressed_sha256.lower()}, "
            f"got {actual_compressed_sha256}"
        )
    try:
        decompressor = zlib.decompressobj(wbits=16 + zlib.MAX_WBITS)
        patch = decompressor.decompress(compressed, MAX_PATCH_BYTES + 1)
        if len(patch) > MAX_PATCH_BYTES or decompressor.unconsumed_tail:
            raise PatchValidationError(
                f"decompressed patch exceeds the {MAX_PATCH_BYTES}-byte limit"
            )
        patch += decompressor.flush()
        if len(patch) > MAX_PATCH_BYTES:
            raise PatchValidationError(
                f"decompressed patch exceeds the {MAX_PATCH_BYTES}-byte limit"
            )
        if not decompressor.eof:
            raise PatchValidationError("compressed patch is truncated")
        if decompressor.unused_data:
            raise PatchValidationError("compressed patch contains trailing or concatenated data")
    except (OSError, EOFError, zlib.error) as error:
        raise PatchValidationError(f"compressed patch is malformed or truncated: {error}") from error
    if not patch:
        raise PatchValidationError("patch is empty")
    if not SHA256_PATTERN.fullmatch(expected_sha256):
        raise PatchValidationError("patch SHA-256 must contain exactly 64 hexadecimal characters")
    actual_sha256 = hashlib.sha256(patch).hexdigest()
    if actual_sha256.lower() != expected_sha256.lower():
        raise PatchValidationError(
            f"patch SHA-256 mismatch: expected {expected_sha256.lower()}, got {actual_sha256}"
        )
    return patch


def validate_binary_patch_size(patch: bytes) -> None:
    total = 0
    for section in patch.split(b"diff --git ")[1:]:
        marker = section.find(b"GIT binary patch\n")
        if marker < 0:
            continue
        size = len(section) - marker
        if size > MAX_BINARY_SECTION_BYTES:
            raise PatchValidationError(
                f"binary patch section is {size} bytes; per-file limit is {MAX_BINARY_SECTION_BYTES}"
            )
        total += size
        if total > MAX_BINARY_TOTAL_BYTES:
            raise PatchValidationError(
                f"binary patch sections total {total} bytes; aggregate limit is {MAX_BINARY_TOTAL_BYTES}"
            )


def changed_paths(repository: Path, patch: bytes) -> list[str]:
    result = git(repository, ["apply", "--numstat", "-z"], data=patch, check=False)
    if result.returncode != 0:
        diagnostic = result.stderr.decode("utf-8", errors="replace").strip()
        raise PatchValidationError(f"patch is malformed or unsupported: {diagnostic}")
    paths: list[str] = []
    for record in result.stdout.split(b"\0"):
        if not record:
            continue
        fields = record.split(b"\t", 2)
        if len(fields) != 3 or not fields[2]:
            raise PatchValidationError("patch contains an unsupported rename or path record")
        try:
            paths.append(fields[2].decode("utf-8", errors="strict"))
        except UnicodeDecodeError as error:
            raise PatchValidationError("patch paths must be valid UTF-8") from error
    if not paths:
        raise PatchValidationError("patch contains no applicable file changes")
    if len(paths) > MAX_CHANGED_PATHS:
        raise PatchValidationError(f"patch changes {len(paths)} paths; limit is {MAX_CHANGED_PATHS}")
    return paths


def index_submodule_paths(repository: Path) -> set[str]:
    result = git(repository, ["ls-files", "--stage", "-z"])
    submodules: set[str] = set()
    for record in result.stdout.split(b"\0"):
        if not record:
            continue
        metadata, path = record.split(b"\t", 1)
        if metadata.split(b" ", 1)[0] == b"160000":
            try:
                submodules.add(path.decode("utf-8", errors="strict"))
            except UnicodeDecodeError as error:
                raise PatchValidationError("repository contains a non-UTF-8 submodule path") from error
    return submodules


def index_paths(repository: Path) -> set[str]:
    result = git(repository, ["ls-files", "-z"])
    paths: set[str] = set()
    for record in result.stdout.split(b"\0"):
        if not record:
            continue
        try:
            paths.add(record.decode("utf-8", errors="strict"))
        except UnicodeDecodeError as error:
            raise PatchValidationError("repository contains a non-UTF-8 tracked path") from error
    return paths


def is_reparse_point(path: Path) -> bool:
    file_stat = path.lstat()
    attributes = getattr(file_stat, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return path.is_symlink() or bool(attributes & reparse_flag)


def validate_paths(repository: Path, paths: list[str]) -> None:
    root = repository.resolve(strict=True)
    submodules = index_submodule_paths(repository)
    tracked_paths = index_paths(repository)
    folded_tracked_paths: dict[str, set[str]] = {}
    for tracked in tracked_paths:
        folded_tracked_paths.setdefault(tracked.casefold(), set()).add(tracked)
    unique_paths = sorted(set(paths))
    if len(unique_paths) != len(paths):
        raise PatchValidationError("patch contains duplicate file-path records")
    if sum(len(name.encode("utf-8")) + 1 for name in unique_paths) > MAX_CHANGED_PATH_BYTES:
        raise PatchValidationError(f"patch paths exceed the {MAX_CHANGED_PATH_BYTES}-byte path-list limit")
    folded_changed_paths: dict[str, str] = {}
    for name in unique_paths:
        path = PurePosixPath(name)
        raw_parts = name.split("/")
        windows_path = PureWindowsPath(name)
        if (not name or path.is_absolute() or windows_path.is_absolute() or windows_path.drive
                or "\\" in name or "\n" in name or "\r" in name or "\t" in name
                or any(part in ("", ".", "..") or part.lower() == ".git" for part in raw_parts)):
            raise PatchValidationError(f"patch path is invalid or outside the repository: {name!r}")
        if len(name.encode("utf-8")) > 4096:
            raise PatchValidationError(f"patch path exceeds the 4096-byte limit: {name[:120]!r}")
        folded_name = name.casefold()
        if folded_name == ".patch-validation-tools" or folded_name.startswith(".patch-validation-tools/"):
            raise PatchValidationError("patch paths cannot target the isolated verifier checkout")
        if folded_name == ".github/workflows" or folded_name.startswith(".github/workflows/"):
            raise PatchValidationError(
                "workflow YAML changes cannot be tested by a source patch; a separately authorized "
                "published workflow revision is required"
            )
        if folded_name == ".gitmodules":
            raise PatchValidationError("submodule configuration changes are unsupported in patch validation")
        if any(
            folded_name == item.casefold() or folded_name.startswith(item.casefold() + "/")
            for item in submodules
        ):
            raise PatchValidationError(f"submodule changes are unsupported in patch validation: {name}")
        previous_name = folded_changed_paths.setdefault(folded_name, name)
        if previous_name != name:
            raise PatchValidationError(
                f"patch paths alias each other on case-insensitive filesystems: {previous_name!r} and {name!r}"
            )
        candidate = root.joinpath(*path.parts)
        current = root
        for part in path.parts[:-1]:
            current = current / part
            if os.path.lexists(current) and is_reparse_point(current):
                raise PatchValidationError(f"patch path traverses a linked/reparse directory: {name}")
        if os.path.lexists(candidate) and is_reparse_point(candidate):
            raise PatchValidationError(f"patch path directly targets a linked/reparse object: {name}")
        try:
            candidate.parent.resolve(strict=False).relative_to(root)
        except ValueError as error:
            raise PatchValidationError(f"patch path escapes the repository: {name}") from error
        aliases = sorted(folded_tracked_paths.get(folded_name, set()) - {name})
        if aliases:
            raise PatchValidationError(
                f"patch path aliases a tracked path on case-insensitive filesystems: {name!r} and {aliases[0]!r}"
            )
    path_set = set(unique_paths)
    for name in unique_paths:
        parent = PurePosixPath(name).parent
        while parent != PurePosixPath("."):
            if str(parent) in path_set:
                raise PatchValidationError(
                    f"patch contains overlapping file/directory paths that cannot be safely ordered: {parent}, {name}"
                )
            parent = parent.parent


def build_result_tree(repository: Path, base_sha: str, patch: bytes) -> str:
    with tempfile.TemporaryDirectory(prefix="pulseforge-patch-index-") as directory:
        index_path = Path(directory) / "index"
        env = os.environ.copy()
        env["GIT_INDEX_FILE"] = str(index_path)
        git(repository, ["read-tree", base_sha], env=env)
        git(repository, ["apply", "--cached", "--binary"], data=patch, env=env)
        return git(repository, ["write-tree"], env=env).stdout.decode("ascii").strip()


def apply_patch_payload(repository: Path, base_sha: str, compressed_patch: bytes, expected_sha256: str,
                        expected_compressed_sha256: str, selection: str,
                        workflow_sha: str = "", transport_workflow_sha: str = "",
                        expected_tree: str = "") -> dict[str, object]:
    if not SHA1_PATTERN.fullmatch(base_sha):
        raise PatchValidationError("base SHA must contain exactly 40 hexadecimal characters")
    if selection not in SUPPORTED_SELECTIONS:
        raise PatchValidationError(f"unsupported validation selection: {selection!r}")
    head = git(repository, ["rev-parse", "--verify", "HEAD^{commit}"]).stdout.decode("ascii").strip()
    if head.lower() != base_sha.lower():
        raise PatchValidationError(f"checked-out base does not match request: expected {base_sha}, got {head}")
    git(repository, ["cat-file", "-e", f"{base_sha}^{{commit}}"])
    patch = decode_patch(compressed_patch, expected_sha256, expected_compressed_sha256)
    validate_binary_patch_size(patch)
    paths = changed_paths(repository, patch)
    validate_paths(repository, paths)
    git(repository, ["apply", "--check", "--binary"], data=patch)
    result_tree = build_result_tree(repository, base_sha, patch)
    if expected_tree and result_tree.lower() != expected_tree.lower():
        raise PatchValidationError(
            f"applied Git tree differs from the prepared local source identity: expected {expected_tree}, got {result_tree}"
        )
    git(repository, ["apply", "--binary"], data=patch)
    for name in paths:
        path = repository.joinpath(*PurePosixPath(name).parts)
        if path.is_symlink():
            target = (path.parent / os.readlink(path)).resolve(strict=False)
            try:
                target.relative_to(repository.resolve(strict=True))
            except ValueError as error:
                raise PatchValidationError(f"patch creates a symlink escaping the repository: {name}") from error
    patch_hash = hashlib.sha256(patch).hexdigest()
    return {
        "schema_version": 1,
        "validation_kind": "EXPERIMENTAL_PATCHED_SOURCE",
        "base_sha": head,
        "patch_sha256": patch_hash,
        "patch_compressed_sha256": hashlib.sha256(compressed_patch).hexdigest(),
        "resulting_tree": result_tree,
        "validation_selection": selection,
        "workflow_definition_sha": workflow_sha,
        "transport_workflow_sha": transport_workflow_sha,
        "changed_paths": sorted(paths),
        "warning": "This run validates the patched source tree, not the unmodified base commit.",
    }


def inputs_from_event(event_path: Path) -> dict[str, str]:
    with event_path.open("rb") as event_file:
        event_data = event_file.read(MAX_EVENT_BYTES + 1)
    if len(event_data) > MAX_EVENT_BYTES:
        raise PatchValidationError(f"workflow event exceeds the {MAX_EVENT_BYTES}-byte limit")
    try:
        event = json.loads(event_data.decode("utf-8", errors="strict"))
    except (UnicodeDecodeError, ValueError, RecursionError) as error:
        raise PatchValidationError(f"workflow event JSON is malformed: {error}") from error
    if not isinstance(event, dict):
        raise PatchValidationError("workflow event must be a JSON object")
    inputs = event.get("inputs")
    if not isinstance(inputs, dict):
        raise PatchValidationError("workflow event has no inputs object")
    if inputs.get("validation_mode") != "patch-validation":
        raise PatchValidationError("workflow event is not a patch-validation dispatch")
    base_sha = inputs.get("patch_base_sha")
    selection = inputs.get("patch_validation_selection")
    outer_patch_sha = inputs.get("patch_sha256")
    request_json = inputs.get("patch_request_json")
    if (not isinstance(base_sha, str) or not base_sha or not isinstance(selection, str) or not selection
            or not isinstance(outer_patch_sha, str) or not SHA256_PATTERN.fullmatch(outer_patch_sha)):
        raise PatchValidationError("patch base SHA, checksum and validation selection are required")
    if not isinstance(request_json, str) or not request_json:
        raise PatchValidationError("complete patch manifest JSON is required")
    if len(request_json.encode("utf-8")) > 64 * 1024:
        raise PatchValidationError("complete patch manifest exceeds the 64 KiB event-field bound")
    try:
        manifest = json.loads(request_json)
    except (ValueError, RecursionError) as error:
        raise PatchValidationError(f"complete patch manifest JSON is invalid: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("kind") != "manifest":
        raise PatchValidationError("patch request must contain a complete manifest object")
    manifest_patch_sha = manifest.get("patch_sha256")
    if not isinstance(manifest_patch_sha, str) or not SHA256_PATTERN.fullmatch(manifest_patch_sha):
        raise PatchValidationError("complete patch manifest has an invalid patch checksum")
    if (manifest.get("base_sha") != base_sha or manifest.get("selection") != selection
            or manifest_patch_sha.lower() != outer_patch_sha.lower()):
        raise PatchValidationError("outer dispatch values differ from the complete patch manifest")
    required = {
        "transfer_id", "count", "run_ids", "patch_sha256", "compressed_sha256",
        "transport_workflow_sha", "expected_tree",
    }
    missing = sorted(key for key in required if key not in manifest)
    if missing:
        raise PatchValidationError(f"complete patch manifest is missing: {', '.join(missing)}")
    if (not isinstance(manifest["transfer_id"], str) or not re.fullmatch(r"[0-9a-f]{32}", manifest["transfer_id"])
            or not isinstance(manifest["count"], int) or isinstance(manifest["count"], bool)
            or not isinstance(manifest["run_ids"], list)
            or not isinstance(manifest["patch_sha256"], str)
            or not isinstance(manifest["compressed_sha256"], str)
            or not isinstance(manifest["transport_workflow_sha"], str)
            or not isinstance(manifest["expected_tree"], str)
            or not SHA1_PATTERN.fullmatch(manifest["expected_tree"])
            or not re.fullmatch(r"[0-9a-fA-F]{40}", manifest["transport_workflow_sha"])):
        raise PatchValidationError("complete patch manifest has invalid field types")
    return {
        "patch_base_sha": base_sha,
        "patch_validation_selection": selection,
        "patch_transfer_id": manifest["transfer_id"],
        "patch_transfer_chunk_count": str(manifest["count"]),
        "patch_transfer_run_ids": json.dumps(manifest["run_ids"], separators=(",", ":")),
        "patch_sha256": manifest["patch_sha256"],
        "patch_compressed_sha256": manifest["compressed_sha256"],
        "patch_expected_tree": manifest["expected_tree"],
        "transport_workflow_sha": manifest["transport_workflow_sha"],
    }


def write_identity(identity: dict[str, object], destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_text(json.dumps(identity, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, destination)
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8", newline="\n") as summary:
            summary.write(
                "## EXPERIMENTAL PATCHED-SOURCE VALIDATION\n\n"
                f"- Base commit: `{identity['base_sha']}`\n"
                f"- Patch SHA-256: `{identity['patch_sha256']}`\n"
                f"- Resulting Git tree: `{identity['resulting_tree']}`\n"
                f"- Selection: `{identity['validation_selection']}`\n"
                f"- Workflow definition SHA: `{identity['workflow_definition_sha'] or 'not supplied'}`\n"
                "- This validates the patched tree. It does not validate the unmodified base commit.\n"
            )
    print(
        "EXPERIMENTAL_PATCH_VALIDATION=PASS "
        f"base={identity['base_sha']} patch_sha256={identity['patch_sha256']} "
        f"tree={identity['resulting_tree']} selection={identity['validation_selection']}"
    )


def run_self_tests() -> None:
    with tempfile.TemporaryDirectory(prefix="pulseforge-patch-validation-test-") as directory:
        source = Path(directory) / "source"
        target = Path(directory) / "target"
        source.mkdir()
        git(source, ["init", "--quiet"])
        git(source, ["config", "user.name", "Patch Validation Test"])
        git(source, ["config", "user.email", "patch-validation@example.invalid"])
        (source / "keep.txt").write_text("base\n", encoding="utf-8")
        (source / "delete Ω.txt").write_text("remove\n", encoding="utf-8")
        git(source, ["add", "--", "keep.txt", "delete Ω.txt"])
        git(source, ["commit", "--quiet", "-m", "synthetic patch base"])
        base_sha = git(source, ["rev-parse", "HEAD"]).stdout.decode("ascii").strip()
        git(Path(directory), ["clone", "--quiet", "--no-hardlinks", str(source), str(target)])
        (source / "keep.txt").write_text("patched Ω\n", encoding="utf-8")
        (source / "delete Ω.txt").unlink()
        (source / "new Ω.txt").write_text("added with café\n", encoding="utf-8")
        git(source, ["add", "--all", "--force"])
        patch = git(source, ["diff", "--cached", "--binary", "--full-index", "--no-renames", base_sha]).stdout
        compressed = gzip.compress(patch, mtime=0)
        digest = hashlib.sha256(patch).hexdigest()
        compressed_digest = hashlib.sha256(compressed).hexdigest()
        expected_tree = git(source, ["write-tree"]).stdout.decode("ascii").strip()
        identity = apply_patch_payload(
            target, base_sha, compressed, digest, compressed_digest, "full", "self-test", expected_tree=expected_tree
        )
        if identity["base_sha"] != base_sha or not (target / "new Ω.txt").read_text(encoding="utf-8").startswith("added"):
            raise PatchValidationError("valid Unicode add/modify/delete self-test failed")
        if identity["resulting_tree"] != expected_tree:
            raise PatchValidationError("valid patch did not reproduce its prepared Git tree identity")
        if (target / "delete Ω.txt").exists() or (target / "keep.txt").read_text(encoding="utf-8") != "patched Ω\n":
            raise PatchValidationError("valid patch did not preserve the expected file changes")

        malformed_patch = b"not a unified patch\n"
        malformed_compressed = gzip.compress(malformed_patch, mtime=0)
        truncated_compressed = gzip.compress(patch, mtime=0)[:-4]
        oversized_patch = b"x" * (MAX_PATCH_BYTES + 1)
        oversized_compressed = gzip.compress(oversized_patch, mtime=0)
        failures: list[tuple[str, str, bytes, str, str]] = [
            ("wrong base", "0" * 40, compressed, digest, compressed_digest),
            ("wrong checksum", base_sha, compressed, "0" * 64, compressed_digest),
            ("wrong compressed checksum", base_sha, compressed, digest, "0" * 64),
            ("malformed patch", base_sha, malformed_compressed, hashlib.sha256(malformed_patch).hexdigest(),
             hashlib.sha256(malformed_compressed).hexdigest()),
            ("malformed gzip", base_sha, b"not-gzip", "0" * 64, hashlib.sha256(b"not-gzip").hexdigest()),
            ("truncated gzip", base_sha, truncated_compressed, digest, hashlib.sha256(truncated_compressed).hexdigest()),
            ("oversized compressed payload", base_sha, b"x" * (MAX_COMPRESSED_BYTES + 1), digest,
             hashlib.sha256(b"x" * (MAX_COMPRESSED_BYTES + 1)).hexdigest()),
            ("oversized decompressed patch", base_sha, oversized_compressed, hashlib.sha256(oversized_patch).hexdigest(),
             hashlib.sha256(oversized_compressed).hexdigest()),
            ("trailing gzip data", base_sha, compressed + b"trailing", digest,
             hashlib.sha256(compressed + b"trailing").hexdigest()),
        ]
        for label, requested_base, payload, checksum, compressed_checksum in failures:
            try:
                apply_patch_payload(target, requested_base, payload, checksum, compressed_checksum, "full", "self-test")
            except (PatchValidationError, subprocess.CalledProcessError):
                continue
            raise PatchValidationError(f"self-test accepted invalid input: {label}")
        for invalid_path in (
            "../outside",
            ".git/config",
            ".patch-validation-tools/.github/scripts/apply_patch_validation.py",
            ".github/workflows/other.yml",
            ".GITHUB/WORKFLOWS/other.yml",
            ".GitModules",
            "KEEP.TXT",
        ):
            try:
                validate_paths(target, [invalid_path])
            except PatchValidationError:
                continue
            raise PatchValidationError(f"self-test accepted an unsupported path: {invalid_path}")
        for invalid_selection in ("", "unknown-selection"):
            try:
                apply_patch_payload(target, base_sha, compressed, digest, compressed_digest, invalid_selection, "self-test")
            except PatchValidationError:
                continue
            raise PatchValidationError(f"self-test accepted an unsupported selection: {invalid_selection!r}")
        try:
            validate_binary_patch_size(b"diff --git a/large.bin b/large.bin\nGIT binary patch\n" +
                                       b"x" * (MAX_BINARY_SECTION_BYTES + 1))
        except PatchValidationError:
            pass
        else:
            raise PatchValidationError("self-test accepted an oversized binary patch section")
        event_file = Path(directory) / "event.json"
        valid_inputs = {
            "validation_mode": "patch-validation",
            "patch_base_sha": base_sha,
            "patch_sha256": digest,
            "patch_validation_selection": "full",
            "patch_request_json": json.dumps({
                "kind": "manifest", "base_sha": base_sha, "selection": "full",
                "transfer_id": "0123456789abcdef0123456789abcdef", "count": 1, "run_ids": [1],
                "patch_sha256": digest, "compressed_sha256": compressed_digest,
                "expected_tree": git(source, ["write-tree"]).stdout.decode("ascii").strip(),
                "transport_workflow_sha": "a" * 40,
            }),
        }
        event_file.write_text(json.dumps({"inputs": valid_inputs}), encoding="utf-8")
        if inputs_from_event(event_file)["patch_base_sha"] != base_sha:
            raise PatchValidationError("self-test did not read workflow_dispatch JSON inputs")
        mismatched_outer_hash = dict(valid_inputs, patch_sha256="0" * 64)
        event_file.write_text(json.dumps({"inputs": mismatched_outer_hash}), encoding="utf-8")
        try:
            inputs_from_event(event_file)
        except PatchValidationError:
            pass
        else:
            raise PatchValidationError("self-test accepted an outer patch hash that differs from its manifest")
        for malformed_event in ("{", "[]", json.dumps({"inputs": {}})):
            event_file.write_text(malformed_event, encoding="utf-8")
            try:
                inputs_from_event(event_file)
            except PatchValidationError:
                continue
            raise PatchValidationError("self-test accepted malformed or incomplete workflow event JSON")
        event_file.write_bytes(b"x" * (MAX_EVENT_BYTES + 1))
        try:
            inputs_from_event(event_file)
        except PatchValidationError:
            pass
        else:
            raise PatchValidationError("self-test accepted an oversized workflow event")
        git(target, ["update-index", "--add", "--cacheinfo", f"160000,{base_sha},Vendor/Child"])
        try:
            validate_paths(target, ["vendor/child/file.txt"])
        except PatchValidationError:
            pass
        else:
            raise PatchValidationError("self-test accepted a path inside a submodule")
        print("PATCH_VALIDATION_SELF_TEST=PASS; file checksums, malformed/truncated gzip, base, size, path, submodule and Unicode cases")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify and apply a bounded experimental source patch before CI source processing.",
        epilog=(
            "Limits: <= 8 MiB compressed; <= 32 MiB decoded; <= 4 MiB per binary patch section "
            "and <= 8 MiB aggregate binary sections; <= 1000 paths and <= 100 KiB path list. "
            "Patching workflow YAML after dispatch does not change the "
            "workflow definition already executing; workflow-definition changes require separate explicit "
            "authorization to publish and dispatch that revision. Experimental results validate the patched "
            "tree, never the unmodified base commit."
        ),
    )
    parser.add_argument("--event", type=Path, help="GitHub event JSON file")
    parser.add_argument("--repository", type=Path, help="checked-out repository root")
    parser.add_argument("--identity-file", type=Path, help="write verified patch identity JSON")
    parser.add_argument("--patch-file", type=Path, help="assembled gzip patch file from short-lived transport artifacts")
    parser.add_argument("--workflow-definition-sha", default="", help="SHA of the running workflow definition")
    parser.add_argument("--self-test", action="store_true", help="run bounded synthetic verifier regressions")
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            run_self_tests()
            return 0
        if not arguments.event or not arguments.repository or not arguments.identity_file or not arguments.patch_file:
            parser.error("--event, --repository, --identity-file, and --patch-file are required unless --self-test is selected")
        values = inputs_from_event(arguments.event)
        if arguments.patch_file.stat().st_size > MAX_COMPRESSED_BYTES:
            raise PatchValidationError(
                f"compressed patch file is {arguments.patch_file.stat().st_size} bytes; limit is {MAX_COMPRESSED_BYTES}"
            )
        compressed_patch = arguments.patch_file.read_bytes()
        identity = apply_patch_payload(
            arguments.repository.resolve(strict=True),
            values["patch_base_sha"],
            compressed_patch,
            values["patch_sha256"],
            values["patch_compressed_sha256"],
            values["patch_validation_selection"],
            arguments.workflow_definition_sha,
            values["transport_workflow_sha"],
            values["patch_expected_tree"],
        )
        write_identity(identity, arguments.identity_file)
    except (PatchValidationError, OSError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"EXPERIMENTAL_PATCH_VALIDATION=FAIL reason={error}", file=sys.stderr, flush=True)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
