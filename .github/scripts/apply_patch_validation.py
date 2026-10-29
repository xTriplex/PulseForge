#!/usr/bin/env python3
"""Apply a bounded workflow_dispatch patch and record its exact source identity."""

from __future__ import annotations

import argparse
import base64
import binascii
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
MAX_ENCODED_CHARS = 40_000
MAX_COMPRESSED_BYTES = 30_000
MAX_PATCH_BYTES = 2 * 1024 * 1024
MAX_CHANGED_PATHS = 200
MAX_CHANGED_PATH_BYTES = 20_000
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


def decode_patch(encoded: str, expected_sha256: str) -> bytes:
    if len(encoded) > MAX_ENCODED_CHARS:
        raise PatchValidationError(
            f"compressed/base64 patch is {len(encoded)} characters; limit is {MAX_ENCODED_CHARS}"
        )
    if len(encoded) % 4 != 0:
        raise PatchValidationError("compressed/base64 patch has invalid base64 length")
    try:
        compressed = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error) as error:
        raise PatchValidationError(f"compressed/base64 patch is malformed: {error}") from error
    if len(compressed) > MAX_COMPRESSED_BYTES:
        raise PatchValidationError(
            f"compressed patch is {len(compressed)} bytes; limit is {MAX_COMPRESSED_BYTES}"
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
        if folded_name == ".github/workflows" or folded_name.startswith(".github/workflows/"):
            raise PatchValidationError(
                "workflow YAML changes cannot be tested by patch dispatch; commit the workflow change "
                "and run the workflow defined by that commit"
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


def build_result_tree(repository: Path, base_sha: str, paths: list[str]) -> str:
    with tempfile.TemporaryDirectory(prefix="pulseforge-patch-index-") as directory:
        index_path = Path(directory) / "index"
        env = os.environ.copy()
        env["GIT_INDEX_FILE"] = str(index_path)
        git(repository, ["read-tree", base_sha], env=env)
        git(repository, ["--literal-pathspecs", "add", "--all", "--force", "--", *paths], env=env)
        return git(repository, ["write-tree"], env=env).stdout.decode("ascii").strip()


def apply_patch_payload(repository: Path, base_sha: str, encoded_patch: str, expected_sha256: str,
                        selection: str, workflow_sha: str = "") -> dict[str, object]:
    if not SHA1_PATTERN.fullmatch(base_sha):
        raise PatchValidationError("base SHA must contain exactly 40 hexadecimal characters")
    if selection not in SUPPORTED_SELECTIONS:
        raise PatchValidationError(f"unsupported validation selection: {selection!r}")
    head = git(repository, ["rev-parse", "--verify", "HEAD^{commit}"]).stdout.decode("ascii").strip()
    if head.lower() != base_sha.lower():
        raise PatchValidationError(f"checked-out base does not match request: expected {base_sha}, got {head}")
    git(repository, ["cat-file", "-e", f"{base_sha}^{{commit}}"])
    patch = decode_patch(encoded_patch, expected_sha256)
    paths = changed_paths(repository, patch)
    validate_paths(repository, paths)
    git(repository, ["apply", "--check", "--binary"], data=patch)
    git(repository, ["apply", "--binary"], data=patch)
    for name in paths:
        path = repository.joinpath(*PurePosixPath(name).parts)
        if path.is_symlink():
            target = (path.parent / os.readlink(path)).resolve(strict=False)
            try:
                target.relative_to(repository.resolve(strict=True))
            except ValueError as error:
                raise PatchValidationError(f"patch creates a symlink escaping the repository: {name}") from error
    result_tree = build_result_tree(repository, base_sha, paths)
    patch_hash = hashlib.sha256(patch).hexdigest()
    return {
        "schema_version": 1,
        "validation_kind": "EXPERIMENTAL_PATCHED_SOURCE",
        "base_sha": head,
        "patch_sha256": patch_hash,
        "resulting_tree": result_tree,
        "validation_selection": selection,
        "workflow_definition_sha": workflow_sha,
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
    required = {
        "patch_base_sha",
        "patch_payload_gzip_base64",
        "patch_sha256",
        "patch_validation_selection",
    }
    missing = sorted(key for key in required if not isinstance(inputs.get(key), str) or not inputs[key])
    if missing:
        raise PatchValidationError(f"patch validation inputs are missing: {', '.join(missing)}")
    return {key: inputs[key] for key in required}


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
        encoded = base64.b64encode(gzip.compress(patch, mtime=0)).decode("ascii")
        digest = hashlib.sha256(patch).hexdigest()
        identity = apply_patch_payload(target, base_sha, encoded, digest, "full", "self-test")
        if identity["base_sha"] != base_sha or not (target / "new Ω.txt").read_text(encoding="utf-8").startswith("added"):
            raise PatchValidationError("valid Unicode add/modify/delete self-test failed")
        if (target / "delete Ω.txt").exists() or (target / "keep.txt").read_text(encoding="utf-8") != "patched Ω\n":
            raise PatchValidationError("valid patch did not preserve the expected file changes")

        failures: list[tuple[str, str, str, str]] = [
            ("wrong base", "0" * 40, encoded, digest),
            ("wrong checksum", base_sha, encoded, "0" * 64),
            ("malformed patch", base_sha,
             base64.b64encode(gzip.compress(b"not a unified patch\n", mtime=0)).decode("ascii"),
             hashlib.sha256(b"not a unified patch\n").hexdigest()),
            ("malformed gzip", base_sha, base64.b64encode(b"not-gzip").decode("ascii"), "0" * 64),
            ("oversized payload", base_sha, "A" * (MAX_ENCODED_CHARS + 4), digest),
            ("trailing gzip data", base_sha,
             base64.b64encode(gzip.compress(patch, mtime=0) + b"trailing").decode("ascii"), digest),
            ("oversized decompressed patch", base_sha,
             base64.b64encode(gzip.compress(b"x" * (MAX_PATCH_BYTES + 1), mtime=0)).decode("ascii"),
             hashlib.sha256(b"x" * (MAX_PATCH_BYTES + 1)).hexdigest()),
        ]
        for label, requested_base, payload, checksum in failures:
            try:
                apply_patch_payload(target, requested_base, payload, checksum, "full", "self-test")
            except (PatchValidationError, subprocess.CalledProcessError):
                continue
            raise PatchValidationError(f"self-test accepted invalid input: {label}")
        for invalid_path in (
            "../outside",
            ".git/config",
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
                apply_patch_payload(target, base_sha, encoded, digest, invalid_selection, "self-test")
            except PatchValidationError:
                continue
            raise PatchValidationError(f"self-test accepted an unsupported selection: {invalid_selection!r}")
        event_file = Path(directory) / "event.json"
        valid_inputs = {
            "validation_mode": "patch-validation",
            "patch_base_sha": base_sha,
            "patch_payload_gzip_base64": encoded,
            "patch_sha256": digest,
            "patch_validation_selection": "full",
        }
        event_file.write_text(json.dumps({"inputs": valid_inputs}), encoding="utf-8")
        if inputs_from_event(event_file)["patch_base_sha"] != base_sha:
            raise PatchValidationError("self-test did not read workflow_dispatch JSON inputs")
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
    print("PATCH_VALIDATION_SELF_TEST=PASS; malformed, checksum, base, compressed/decompressed size, path, submodule and Unicode cases")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify and apply a bounded experimental source patch before CI source processing.",
        epilog=(
            "Limits: at most 100 helper include paths; <= 40000 base64 characters; <= 30000 compressed "
            "bytes; <= 2097152 decoded bytes; <= 200 changed paths and <= 20000 path-list bytes. "
            "Patching workflow YAML after dispatch does not change the "
            "workflow definition already executing; arbitrary workflow-graph changes require committing "
            "the workflow change and running that definition. Experimental results validate the patched "
            "tree, never the unmodified base commit."
        ),
    )
    parser.add_argument("--event", type=Path, help="GitHub event JSON file")
    parser.add_argument("--repository", type=Path, help="checked-out repository root")
    parser.add_argument("--identity-file", type=Path, help="write verified patch identity JSON")
    parser.add_argument("--workflow-definition-sha", default="", help="SHA of the running workflow definition")
    parser.add_argument("--self-test", action="store_true", help="run bounded synthetic verifier regressions")
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            run_self_tests()
            return 0
        if not arguments.event or not arguments.repository or not arguments.identity_file:
            parser.error("--event, --repository, and --identity-file are required unless --self-test is selected")
        values = inputs_from_event(arguments.event)
        identity = apply_patch_payload(
            arguments.repository.resolve(strict=True),
            values["patch_base_sha"],
            values["patch_payload_gzip_base64"],
            values["patch_sha256"],
            values["patch_validation_selection"],
            arguments.workflow_definition_sha,
        )
        write_identity(identity, arguments.identity_file)
    except (PatchValidationError, OSError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"EXPERIMENTAL_PATCH_VALIDATION=FAIL reason={error}", file=sys.stderr, flush=True)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
