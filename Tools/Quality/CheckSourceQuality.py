from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)', re.MULTILINE)
TARGET_CALL_RE = re.compile(r"\b(add_library|target_link_libraries)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\b(.*?)\)", re.DOTALL)
VISIBILITY = {"PUBLIC", "PRIVATE", "INTERFACE", "debug", "optimized", "general"}


def normalize(path: str | Path) -> str:
    return str(path).replace("\\", "/")


def file_line_limit(relative_path: str, suffix: str, rules: dict) -> int:
    path = normalize(relative_path)
    exception = rules.get("size_exceptions", {}).get(path)
    if exception:
        return int(exception["max_lines"])
    return int(rules["default_header_line_limit"] if suffix.lower() in {".h", ".hpp", ".inl"}
               else rules["default_cpp_line_limit"])


def find_size_violations(relative_path: str, line_count: int, rules: dict) -> list[str]:
    suffix = Path(relative_path).suffix
    limit = file_line_limit(relative_path, suffix, rules)
    if line_count <= limit:
        return []
    return [f"{normalize(relative_path)} has {line_count} lines (limit {limit})"]


def parse_target_call(cmake: str, call: str, target: str) -> str | None:
    for match in TARGET_CALL_RE.finditer(cmake):
        if match.group(1) == call and match.group(2) == target:
            return match.group(3)
    return None


def parse_target_dependencies(cmake: str, target: str) -> set[str] | None:
    body = parse_target_call(cmake, "target_link_libraries", target)
    if body is None:
        return None
    return {token for token in re.findall(r"[A-Za-z_][A-Za-z0-9_.:+-]*", body)
            if token not in VISIBILITY}


def check_dependencies(cmake: str, target: str, allowed: list[str]) -> list[str]:
    dependencies = parse_target_dependencies(cmake, target)
    if dependencies is None:
        return [f"CMake has no target_link_libraries call for {target}"]
    expected = set(allowed)
    if dependencies == expected:
        return []
    unexpected = sorted(dependencies - expected)
    missing = sorted(expected - dependencies)
    issues = []
    if unexpected:
        issues.append(f"{target} has unreviewed dependencies: {', '.join(unexpected)}")
    if missing:
        issues.append(f"{target} is missing required dependencies: {', '.join(missing)}")
    return issues


def check_includes(source_name: str, content: str, forbidden: list[str]) -> list[str]:
    lowered = [(name, name.lower()) for name in forbidden]
    issues = []
    for include in INCLUDE_RE.findall(content):
        normalized = normalize(include)
        candidate = normalized.lower()
        for name, pattern in lowered:
            if candidate == pattern or candidate.startswith(pattern):
                issues.append(f"{normalize(source_name)} includes forbidden dependency {include}")
    return issues


def target_source_paths(cmake: str, target: str) -> list[str]:
    body = parse_target_call(cmake, "add_library", target)
    if body is None:
        return []
    return re.findall(r"(?:PulseForgeEditor/Source/Private/[A-Za-z0-9_./-]+\.cpp)", body)


def check_cpp_hygiene(root: Path, changed_paths: list[str]) -> list[str]:
    issues = []
    for relative in changed_paths:
        path = root / relative
        if path.suffix.lower() not in {".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl"} or not path.is_file():
            continue
        data = path.read_bytes()
        if data and not data.endswith(b"\r\n"):
            issues.append(f"{normalize(relative)} must end with a CRLF newline (see .editorconfig)")
        for number, line in enumerate(data.splitlines(), 1):
            if line.endswith((b" ", b"\t")):
                issues.append(f"{normalize(relative)}:{number} has trailing whitespace")
    return issues


def changed_paths(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "diff", "--name-only", "HEAD"], cwd=root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git diff failed")
    paths = {normalize(line) for line in result.stdout.splitlines() if line}
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard"], cwd=root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if untracked.returncode == 0:
        paths.update(normalize(line) for line in untracked.stdout.splitlines() if line)
    if not paths:
        committed = subprocess.run(
            ["git", "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"], cwd=root,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if committed.returncode == 0:
            paths.update(normalize(line) for line in committed.stdout.splitlines() if line)
    return sorted(paths)


def run(root: Path, rules: dict, include_git_checks: bool = True) -> list[str]:
    issues = []
    for source_root in rules["source_roots"]:
        directory = root / source_root
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            relative = normalize(path.relative_to(root))
            try:
                line_count = len(path.read_text(encoding="utf-8", errors="strict").splitlines())
            except UnicodeDecodeError:
                issues.append(f"{relative} is not valid UTF-8 source text")
                continue
            issues.extend(find_size_violations(relative, line_count, rules))

    cmake_path = root / "CMakeLists.txt"
    cmake = cmake_path.read_text(encoding="utf-8")
    for key in ("editor_core_target", "editor_ui_target"):
        target = rules[key]
        issues.extend(check_dependencies(cmake, target, rules[key.replace("target", "allowed_dependencies")]))

    core_target = rules["editor_core_target"]
    forbidden = rules["editor_core_forbidden_includes"]
    for relative in target_source_paths(cmake, core_target):
        source = root / relative
        if not source.is_file():
            issues.append(f"{relative} listed in {core_target} does not exist")
            continue
        issues.extend(check_includes(relative, source.read_text(encoding="utf-8"), forbidden))

    for public_root, forbidden_paths in rules["public_header_roots"].items():
        directory = root / public_root
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix.lower() in {".h", ".hpp"}:
                relative = normalize(path.relative_to(root))
                issues.extend(check_includes(relative, path.read_text(encoding="utf-8"), forbidden_paths))

    if include_git_checks:
        paths = changed_paths(root)
        issues.extend(check_cpp_hygiene(root, paths))
        diff = subprocess.run(
            ["git", "diff", "--check", "HEAD"], cwd=root, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if diff.returncode != 0:
            issues.append(diff.stdout.strip() or diff.stderr.strip() or "git diff --check failed")
    return issues


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--rules", type=Path, required=True)
    parser.add_argument("--skip-git-checks", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    rules = json.loads(args.rules.read_text(encoding="utf-8"))
    issues = run(root, rules, include_git_checks=not args.skip_git_checks)
    if issues:
        print("PulseForge source quality checks failed:")
        for issue in issues:
            print(f" - {issue}")
        return 1
    print("PulseForge source quality checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
