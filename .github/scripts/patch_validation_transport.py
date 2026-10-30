#!/usr/bin/env python3
"""Upload and assemble bounded patch-validation chunks using short-lived Actions artifacts."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import io
import json
import os
import re
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
import zlib
from pathlib import Path


MAX_EVENT_BYTES = 256 * 1024
MAX_API_RESPONSE_BYTES = 1024 * 1024
MAX_CHUNK_BYTES = 44_000
MAX_COMPRESSED_BYTES = 8 * 1024 * 1024
MAX_CHUNK_COUNT = (MAX_COMPRESSED_BYTES + MAX_CHUNK_BYTES - 1) // MAX_CHUNK_BYTES
MAX_RUN_IDS = MAX_CHUNK_COUNT
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
TRANSFER_RE = re.compile(r"^[0-9a-f]{32}$")
ARTIFACT_PREFIX = "pulseforge-patch"


class TransportError(RuntimeError):
    pass


def read_event(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) > MAX_EVENT_BYTES:
        raise TransportError(f"event JSON exceeds {MAX_EVENT_BYTES} bytes")
    try:
        event = json.loads(data.decode("utf-8", errors="strict"))
    except (UnicodeDecodeError, ValueError, RecursionError) as error:
        raise TransportError(f"event JSON is invalid: {error}") from error
    if not isinstance(event, dict) or not isinstance(event.get("inputs"), dict):
        raise TransportError("workflow event must contain an inputs object")
    return event["inputs"]


def require_text(inputs: dict[str, object], key: str) -> str:
    value = inputs.get(key)
    if not isinstance(value, str) or not value:
        raise TransportError(f"workflow input {key} is missing")
    return value


def request_payload(inputs: dict[str, object]) -> dict[str, object]:
    encoded = require_text(inputs, "patch_request_json")
    if len(encoded.encode("utf-8")) > 64 * 1024:
        raise TransportError("structured patch request exceeds the 64 KiB event-field bound")
    try:
        request = json.loads(encoded)
    except (ValueError, RecursionError) as error:
        raise TransportError(f"structured patch request JSON is invalid: {error}") from error
    if not isinstance(request, dict):
        raise TransportError("structured patch request must be a JSON object")
    return request


def parse_bounded_int(value: str, label: str, minimum: int, maximum: int) -> int:
    if not value.isascii() or not value.isdecimal():
        raise TransportError(f"{label} must be a decimal integer")
    result = int(value)
    if not minimum <= result <= maximum:
        raise TransportError(f"{label} must be between {minimum} and {maximum}")
    return result


def write_json_atomic(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def upload_chunk(event_path: Path, output_directory: Path) -> Path:
    inputs = read_event(event_path)
    if inputs.get("validation_mode") != "patch-transfer":
        raise TransportError("workflow event is not a patch-transfer dispatch")
    payload = request_payload(inputs)
    if payload.get("kind") != "chunk":
        raise TransportError("patch request does not describe a transport chunk")
    transfer_id = require_text(payload, "transfer_id")
    if not TRANSFER_RE.fullmatch(transfer_id):
        raise TransportError("patch transfer ID must be 32 lowercase hexadecimal characters")
    index = parse_bounded_int(require_text(payload, "index"), "chunk index", 0, MAX_CHUNK_COUNT - 1)
    raw_count = payload.get("count")
    if type(raw_count) is int:
        if not 1 <= raw_count <= MAX_CHUNK_COUNT:
            raise TransportError(f"chunk count must be between 1 and {MAX_CHUNK_COUNT}")
        count = raw_count
    elif isinstance(raw_count, str):
        count = parse_bounded_int(raw_count, "chunk count", 1, MAX_CHUNK_COUNT)
    else:
        raise TransportError("chunk count must be a bounded integer")
    if index >= count:
        raise TransportError("chunk index must be less than chunk count")
    patch_sha = require_text(payload, "patch_sha256").lower()
    chunk_sha = require_text(payload, "chunk_sha256").lower()
    if not SHA256_RE.fullmatch(patch_sha) or not SHA256_RE.fullmatch(chunk_sha):
        raise TransportError("patch and chunk checksums must be 64 hexadecimal characters")
    encoded = require_text(payload, "chunk_base64")
    try:
        chunk = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error) as error:
        raise TransportError(f"chunk base64 is malformed: {error}") from error
    if not chunk or len(chunk) > MAX_CHUNK_BYTES:
        raise TransportError(f"chunk size must be between 1 and {MAX_CHUNK_BYTES} bytes")
    actual_sha = hashlib.sha256(chunk).hexdigest()
    if actual_sha != chunk_sha:
        raise TransportError(f"chunk SHA-256 mismatch: expected {chunk_sha}, got {actual_sha}")
    expected_label = f"{transfer_id}-{index}-{count}"
    if require_text(inputs, "patch_transport_label") != expected_label:
        raise TransportError("transport label does not match the structured chunk identity")
    output_directory.mkdir(parents=True, exist_ok=True)
    chunk_path = output_directory / "patch-chunk.bin"
    chunk_path.write_bytes(chunk)
    write_json_atomic(output_directory / "patch-chunk.json", {
        "schema_version": 1,
        "transfer_id": transfer_id,
        "index": index,
        "count": count,
        "patch_sha256": patch_sha,
        "chunk_sha256": actual_sha,
        "chunk_bytes": len(chunk),
    })
    return output_directory


def github_get(url: str, token: str) -> bytes:
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}",
                 "X-GitHub-Api-Version": "2022-11-28"},
    )
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=30) as response:
            data = response.read(MAX_API_RESPONSE_BYTES + 1)
            if len(data) > MAX_API_RESPONSE_BYTES:
                raise TransportError("GitHub API metadata response exceeded its bound")
            return data
    except urllib.error.HTTPError as error:
        raise TransportError(f"GitHub artifact API returned HTTP {error.code}") from error
    except (urllib.error.URLError, TimeoutError) as error:
        raise TransportError(f"GitHub API request failed ({type(error).__name__})") from None


def download_artifact_zip(api_url: str, token: str) -> bytes:
    request = urllib.request.Request(
        api_url,
        headers={"Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}",
                 "X-GitHub-Api-Version": "2022-11-28"},
    )
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    opener = urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(request, timeout=30) as response:
            raise TransportError(f"artifact API unexpectedly returned HTTP {response.status} instead of a redirect")
    except urllib.error.HTTPError as error:
        if error.code not in (301, 302, 303, 307, 308):
            raise TransportError(f"GitHub artifact download returned HTTP {error.code}") from error
        location = error.headers.get("Location")
    except (urllib.error.URLError, TimeoutError) as error:
        raise TransportError(f"GitHub artifact request failed ({type(error).__name__})") from None
    if not location or not location.startswith("https://"):
        raise TransportError("GitHub artifact download did not provide a safe HTTPS URL")
    # The signed artifact URL is intentionally fetched without the GitHub API token.
    try:
        with urllib.request.urlopen(urllib.request.Request(location), timeout=60) as archive:
            data = archive.read(MAX_CHUNK_BYTES + 1024 * 1024 + 1)
    except (urllib.error.URLError, TimeoutError) as error:
        raise TransportError(f"signed artifact download failed ({type(error).__name__})") from None
    if len(data) > MAX_CHUNK_BYTES + 1024 * 1024:
        raise TransportError("artifact archive exceeded the bounded download size")
    return data


def join_verified_chunks(parts: list[bytes | None], expected_sha256: str) -> bytes:
    if not parts or any(part is None for part in parts):
        raise TransportError("one or more patch chunks are missing")
    chunks = [part for part in parts if part is not None]
    if any(not chunk or len(chunk) > MAX_CHUNK_BYTES for chunk in chunks):
        raise TransportError("patch chunk size is outside the transport bounds")
    if any(len(chunk) != MAX_CHUNK_BYTES for chunk in chunks[:-1]):
        raise TransportError("all non-final patch chunks must have the configured full chunk size")
    compressed = b"".join(chunks)
    if len(compressed) > MAX_COMPRESSED_BYTES:
        raise TransportError(f"assembled compressed patch exceeds {MAX_COMPRESSED_BYTES} bytes")
    actual_sha256 = hashlib.sha256(compressed).hexdigest()
    if actual_sha256 != expected_sha256:
        raise TransportError("assembled compressed-file SHA-256 mismatch")
    return compressed


def read_zip_entry_bounded(archive: zipfile.ZipFile, name: str, maximum: int) -> bytes:
    with archive.open(name, "r") as entry:
        data = entry.read(maximum + 1)
    if len(data) > maximum:
        raise TransportError(f"transport archive entry {name!r} exceeds its {maximum}-byte bound")
    return data


def assemble(event_path: Path, output_path: Path, repository: str, api_url: str, token: str,
             workflow_sha: str) -> Path:
    inputs = read_event(event_path)
    if inputs.get("validation_mode") != "patch-validation":
        raise TransportError("workflow event is not a patch-validation dispatch")
    payload = request_payload(inputs)
    if payload.get("kind") != "manifest":
        raise TransportError("patch request does not describe a complete transport manifest")
    transfer_id = require_text(payload, "transfer_id")
    if not TRANSFER_RE.fullmatch(transfer_id):
        raise TransportError("patch transfer ID must be 32 lowercase hexadecimal characters")
    patch_sha = require_text(payload, "patch_sha256").lower()
    compressed_sha = require_text(payload, "compressed_sha256").lower()
    if not SHA256_RE.fullmatch(patch_sha) or not SHA256_RE.fullmatch(compressed_sha):
        raise TransportError("patch and compressed-file checksums must be 64 hexadecimal characters")
    if require_text(inputs, "patch_sha256").lower() != patch_sha:
        raise TransportError("outer and manifest patch checksums differ")
    expected_transport_sha = require_text(payload, "transport_workflow_sha")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", expected_transport_sha) or expected_transport_sha.lower() != workflow_sha.lower():
        raise TransportError("transport artifacts and validation run must use the same exact workflow-definition SHA")
    raw_count = payload.get("count")
    if type(raw_count) is int:
        if not 1 <= raw_count <= MAX_CHUNK_COUNT:
            raise TransportError(f"chunk count must be between 1 and {MAX_CHUNK_COUNT}")
        count = raw_count
    elif isinstance(raw_count, str):
        count = parse_bounded_int(raw_count, "chunk count", 1, MAX_CHUNK_COUNT)
    else:
        raise TransportError("chunk count must be a bounded integer")
    run_ids = payload.get("run_ids")
    if not isinstance(run_ids, list) or len(run_ids) != count or len(run_ids) > MAX_RUN_IDS:
        raise TransportError("transport run ID count does not match the declared chunk count")
    if any(type(run_id) is not int or run_id <= 0 for run_id in run_ids) or len(set(run_ids)) != len(run_ids):
        raise TransportError("transport run IDs must be unique positive integers")
    if require_text(inputs, "patch_base_sha") != require_text(payload, "base_sha"):
        raise TransportError("outer and manifest base SHA values differ")
    if inputs.get("patch_validation_selection") != payload.get("selection"):
        raise TransportError("outer and manifest validation selections differ")
    parts: list[bytes | None] = [None] * count
    expected_artifact_names = {f"{ARTIFACT_PREFIX}-{transfer_id}-{index}-{count}" for index in range(count)}
    for run_id in run_ids:
        run_data = github_get(f"{api_url}/repos/{repository}/actions/runs/{run_id}", token)
        try:
            run = json.loads(run_data.decode("utf-8", errors="strict"))
        except (UnicodeDecodeError, ValueError) as error:
            raise TransportError(f"transport run {run_id} metadata is invalid") from error
        if not isinstance(run, dict):
            raise TransportError(f"transport run {run_id} metadata is not an object")
        run_repository = run.get("repository")
        if not isinstance(run_repository, dict) or str(run_repository.get("full_name", "")).casefold() != repository.casefold():
            raise TransportError(f"transport run {run_id} belongs to another repository")
        workflow_path = str(run.get("path", "")).split("@", 1)[0]
        if run.get("event") != "workflow_dispatch" or not workflow_path.endswith(".github/workflows/portable-editor-ci.yml"):
            raise TransportError(f"transport run {run_id} was not created by the expected workflow dispatch")
        if run.get("status") != "completed" or run.get("conclusion") != "success":
            raise TransportError(f"transport run {run_id} has not completed successfully")
        if str(run.get("head_sha", "")).lower() != expected_transport_sha.lower():
            raise TransportError(f"transport run {run_id} used a different workflow-definition SHA")
        artifacts_data = github_get(f"{api_url}/repos/{repository}/actions/runs/{run_id}/artifacts", token)
        try:
            artifacts_response = json.loads(artifacts_data.decode("utf-8", errors="strict"))
        except (UnicodeDecodeError, ValueError) as error:
            raise TransportError(f"transport run {run_id} artifact list is invalid") from error
        if not isinstance(artifacts_response, dict) or not isinstance(artifacts_response.get("artifacts"), list):
            raise TransportError(f"transport run {run_id} artifact list is not an array")
        artifacts = artifacts_response["artifacts"]
        matches = [item for item in artifacts if item.get("name") in expected_artifact_names and not item.get("expired")]
        if len(matches) != 1:
            raise TransportError(f"transport run {run_id} must contain exactly one expected chunk artifact")
        artifact = matches[0]
        zip_bytes = download_artifact_zip(f"{api_url}/repos/{repository}/actions/artifacts/{artifact['id']}/zip", token)
        try:
            import io
            with zipfile.ZipFile(io.BytesIO(zip_bytes)) as archive:
                names = sorted(archive.namelist())
                if names != ["patch-chunk.bin", "patch-chunk.json"]:
                    raise TransportError(f"transport artifact {artifact['name']} has unexpected files")
                if sum(info.file_size for info in archive.infolist()) > MAX_CHUNK_BYTES + 16 * 1024:
                    raise TransportError(f"transport artifact {artifact['name']} expands beyond its bound")
                chunk = read_zip_entry_bounded(archive, "patch-chunk.bin", MAX_CHUNK_BYTES)
                metadata_bytes = read_zip_entry_bounded(archive, "patch-chunk.json", 16 * 1024)
                metadata = json.loads(metadata_bytes.decode("utf-8", errors="strict"))
        except (zipfile.BadZipFile, KeyError, UnicodeDecodeError, ValueError, EOFError,
                RuntimeError, NotImplementedError, zlib.error) as error:
            raise TransportError(f"transport artifact {artifact['name']} is malformed") from error
        if not isinstance(metadata, dict):
            raise TransportError(f"transport artifact {artifact['name']} metadata is not an object")
        index = metadata.get("index")
        if (metadata.get("schema_version") != 1 or metadata.get("transfer_id") != transfer_id
                or metadata.get("count") != count or metadata.get("patch_sha256") != patch_sha
                or type(index) is not int or not 0 <= index < count or parts[index] is not None):
            raise TransportError(f"transport artifact {artifact['name']} metadata does not match the request")
        expected_title = f"Portable editor CI · Patch transfer · {transfer_id}-{index}-{count}"
        if run.get("display_title") != expected_title:
            raise TransportError(f"transport run {run_id} title does not match the requested chunk identity")
        if artifact["name"] != f"{ARTIFACT_PREFIX}-{transfer_id}-{index}-{count}":
            raise TransportError(f"transport artifact {artifact['name']} has an incorrect chunk index")
        if len(chunk) != metadata.get("chunk_bytes") or hashlib.sha256(chunk).hexdigest() != metadata.get("chunk_sha256"):
            raise TransportError(f"transport artifact {artifact['name']} chunk checksum mismatch")
        if len(chunk) != MAX_CHUNK_BYTES and index != count - 1:
            raise TransportError(f"transport artifact {artifact['name']} is not a full intermediate chunk")
        parts[index] = chunk
    compressed = join_verified_chunks(parts, compressed_sha)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(compressed)
    return output_path


def run_self_tests() -> None:
    with tempfile.TemporaryDirectory(prefix="pulseforge-patch-transport-test-") as directory:
        root = Path(directory)
        event_path = root / "event.json"
        output = root / "output"
        chunk = b"chunk with Unicode source identity \xcf\x89\n"
        digest = hashlib.sha256(chunk).hexdigest()
        valid = {
            "validation_mode": "patch-transfer",
            "patch_transport_label": "0123456789abcdef0123456789abcdef-0-1",
            "patch_request_json": json.dumps({
                "kind": "chunk", "transfer_id": "0123456789abcdef0123456789abcdef",
                "index": "0", "count": "1", "chunk_base64": base64.b64encode(chunk).decode("ascii"),
                "chunk_sha256": digest, "patch_sha256": "a" * 64,
            }),
        }
        event_path.write_text(json.dumps({"inputs": valid}), encoding="utf-8")
        upload_chunk(event_path, output)
        if (output / "patch-chunk.bin").read_bytes() != chunk:
            raise TransportError("self-test did not preserve chunk bytes")
        metadata = json.loads((output / "patch-chunk.json").read_text(encoding="utf-8"))
        if metadata.get("chunk_sha256") != digest or metadata.get("index") != 0:
            raise TransportError("self-test chunk metadata differs from its input")
        full_chunk = b"a" * MAX_CHUNK_BYTES
        tail_chunk = b"last chunk"
        assembled_source = full_chunk + tail_chunk
        chunks = [full_chunk, tail_chunk]
        assembled_hash = hashlib.sha256(assembled_source).hexdigest()
        if join_verified_chunks(chunks, assembled_hash) != assembled_source:
            raise TransportError("self-test did not reassemble a complete chunked file")
        for label, invalid_parts, invalid_hash in (
            ("missing chunk", [full_chunk, None], assembled_hash),
            ("short intermediate chunk", [b"short", tail_chunk], assembled_hash),
            ("wrong assembled checksum", chunks, "0" * 64),
        ):
            try:
                join_verified_chunks(invalid_parts, invalid_hash)
            except TransportError:
                continue
            raise TransportError(f"self-test accepted invalid assembled transfer: {label}")
        archive_bytes = io.BytesIO()
        with zipfile.ZipFile(archive_bytes, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("oversized.bin", b"x" * 1024)
        with zipfile.ZipFile(io.BytesIO(archive_bytes.getvalue())) as archive:
            try:
                read_zip_entry_bounded(archive, "oversized.bin", 64)
            except TransportError:
                pass
            else:
                raise TransportError("self-test accepted an oversized expanded artifact entry")
        for label, changed in (
            ("wrong checksum", {**valid, "patch_request_json": json.dumps({**json.loads(valid["patch_request_json"]), "chunk_sha256": "0" * 64})}),
            ("bad index", {**valid, "patch_request_json": json.dumps({**json.loads(valid["patch_request_json"]), "index": "1"})}),
            ("malformed base64", {**valid, "patch_request_json": json.dumps({**json.loads(valid["patch_request_json"]), "chunk_base64": "%%%"})}),
            ("wrong mode", {**valid, "validation_mode": "patch-validation"}),
        ):
            event_path.write_text(json.dumps({"inputs": changed}), encoding="utf-8")
            try:
                upload_chunk(event_path, root / ("bad-" + label.replace(" ", "-")))
            except TransportError:
                continue
            raise TransportError(f"self-test accepted invalid transport input: {label}")
    print("PATCH_TRANSPORT_SELF_TEST=PASS; Unicode chunk, checksum, bounds, mode and index checks")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", choices=("upload", "assemble"))
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--event", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--repository", default="")
    parser.add_argument("--api-url", default="https://api.github.com")
    parser.add_argument("--workflow-sha", default=os.environ.get("WORKFLOW_DEFINITION_SHA", ""))
    arguments = parser.parse_args()
    try:
        if arguments.self_test:
            run_self_tests()
            return 0
        if not arguments.mode or not arguments.event or not arguments.output:
            parser.error("mode, --event and --output are required unless --self-test is selected")
        if arguments.mode == "upload":
            result = upload_chunk(arguments.event, arguments.output)
        else:
            token = os.environ.get("GITHUB_TOKEN", "")
            if not arguments.repository or not token or not arguments.workflow_sha:
                raise TransportError("assembly requires repository identity, workflow SHA and the read-only Actions token")
            result = assemble(arguments.event, arguments.output, arguments.repository,
                              arguments.api_url, token, arguments.workflow_sha)
        print(f"PATCH_TRANSPORT_{arguments.mode.upper()}=PASS file={result}")
        return 0
    except (OSError, TransportError) as error:
        print(f"PATCH_TRANSPORT_{arguments.mode.upper()}=FAIL {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
