from __future__ import annotations

import argparse
import dataclasses
import json
import re
import subprocess
import sys
from pathlib import Path


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl"}
IMPLEMENTATION_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)', re.MULTILINE)
FIRST_PARTY_ROOTS = ("PulseForge", "PulseForgeEditor", "PulseForgeGame", "Tests")


@dataclasses.dataclass
class ConfiguredTarget:
    kind: str
    sources: list[str] = dataclasses.field(default_factory=list)
    links: list[str] = dataclasses.field(default_factory=list)
    interface: list[str] = dataclasses.field(default_factory=list)
    definitions: list[str] = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class ConfiguredGraph:
    configuration: str
    options: dict[str, str]
    targets: dict[str, ConfiguredTarget]


def normalize(path: str | Path) -> str:
    return str(path).replace("\\", "/")


def _list_value(value: str) -> list[str]:
    if not value:
        return []
    return [item for item in value.split(";") if item]


def parse_graph_manifest(path: Path) -> ConfiguredGraph:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise RuntimeError(f"Could not read configured CMake target graph {path}: {error}") from error

    configuration = ""
    options: dict[str, str] = {}
    targets: dict[str, ConfiguredTarget] = {}
    for number, line in enumerate(lines, 1):
        if not line:
            continue
        fields = line.split("|", 2)
        if fields[0] == "CONFIG":
            if len(fields) != 2:
                raise RuntimeError(f"Malformed configured target graph configuration at {path}:{number}")
            if configuration:
                raise RuntimeError(f"Configured target graph has more than one configuration in {path}")
            configuration = fields[1]
            continue
        if len(fields) != 3:
            raise RuntimeError(f"Malformed configured target graph record at {path}:{number}")
        record, name, value = fields
        if "$<" in value:
            raise RuntimeError(
                f"CMake did not resolve generator expressions for {record} {name} in {path}:{number}: {value}")
        if record == "OPTION":
            if name in options:
                raise RuntimeError(f"Configured target graph repeats option {name} in {path}")
            options[name] = value
            continue
        if record == "TARGET":
            if name in targets:
                raise RuntimeError(f"Configured target graph repeats target {name} in {path}")
            targets[name] = ConfiguredTarget(value)
            continue
        target = targets.get(name)
        if target is None:
            raise RuntimeError(f"Configured target graph lists {record} before target {name} in {path}:{number}")
        if record == "SOURCES":
            target.sources = _list_value(value)
        elif record == "LINKS":
            target.links = _list_value(value)
        elif record == "INTERFACE":
            target.interface = _list_value(value)
        elif record == "DEFINITIONS":
            target.definitions = _list_value(value)
        else:
            raise RuntimeError(f"Unknown configured target graph record {record} in {path}:{number}")

    if not configuration:
        raise RuntimeError(f"Configured target graph {path} has no configuration record")
    return ConfiguredGraph(configuration, options, targets)


def file_line_limit(relative_path: str, suffix: str, rules: dict) -> int:
    path = normalize(relative_path)
    exception = rules.get("size_exceptions", {}).get(path)
    if exception:
        return int(exception["max_lines"])
    if suffix.lower() in {".h", ".hpp", ".inl"}:
        return int(rules["default_header_hard_limit"])
    return int(rules["default_cpp_hard_limit"])


def find_size_violations(relative_path: str, line_count: int, rules: dict) -> list[str]:
    suffix = Path(relative_path).suffix
    limit = file_line_limit(relative_path, suffix, rules)
    if line_count <= limit:
        return []
    return [f"{normalize(relative_path)} has {line_count} lines (hard limit {limit})"]


def find_size_reviews(relative_path: str, line_count: int, rules: dict) -> list[str]:
    if Path(relative_path).suffix.lower() in {".h", ".hpp", ".inl"}:
        return []
    threshold = int(rules["cpp_responsibility_review_threshold"])
    exception = rules.get("size_exceptions", {}).get(normalize(relative_path))
    if line_count <= threshold and not exception:
        return []
    if exception:
        reason = exception.get("reason", "legacy responsibility review")
    else:
        reason = "review whether this file owns more than one responsibility"
    return [f"{normalize(relative_path)} has {line_count} lines; {reason}"]


def _source_path(root: Path, source: str) -> str | None:
    candidate = Path(source)
    if not candidate.is_absolute():
        candidate = root / candidate
    try:
        return normalize(candidate.resolve().relative_to(root.resolve()))
    except ValueError:
        return None


def _first_party_source(root: Path, source: str) -> str | None:
    relative = _source_path(root, source)
    if relative is None:
        return None
    path = Path(relative)
    if path.suffix.lower() not in IMPLEMENTATION_SUFFIXES:
        return None
    parts = path.parts
    if not parts or parts[0] not in FIRST_PARTY_ROOTS:
        return None
    if parts[0] == "Tests":
        return relative
    if "Source" not in parts or "Private" not in parts:
        return None
    if parts.index("Private") <= parts.index("Source"):
        return None
    return relative


def _has_first_party_compilation_source(root: Path, target: ConfiguredTarget) -> bool:
    for source in target.sources:
        relative = _source_path(root, source)
        if relative is None:
            continue
        path = Path(relative)
        if path.suffix.lower() not in IMPLEMENTATION_SUFFIXES or not path.parts:
            continue
        if path.parts[0] == "Tests":
            return True
        if (len(path.parts) >= 2 and
                path.parts[0] in {"PulseForge", "PulseForgeEditor", "PulseForgeGame"} and
                path.parts[1] == "Source"):
            return True
    return False


def check_configuration_target_coverage(graph: ConfiguredGraph, root: Path) -> list[str]:
    expected = {"DEBUG": "PF_DEBUG", "RELEASE": "PF_RELEASE", "DIST": "PF_DIST"}.get(
        graph.configuration.upper())
    if expected is None:
        return [f"Unsupported source-quality configuration {graph.configuration}; use Debug, Release, or Dist"]
    missing = sorted(
        name for name, target in graph.targets.items()
        if _has_first_party_compilation_source(root, target) and expected not in target.definitions)
    if missing:
        return [f"First-party targets are missing {expected}: " + ", ".join(missing)]
    return []


def _link_values(target: ConfiguredTarget) -> set[str]:
    return {value for value in target.links if value} | {value for value in target.interface if value}


def _first_party_targets(graph: ConfiguredGraph) -> set[str]:
    return {name for name in graph.targets if name.startswith("PulseForge")}


def check_target_graph(graph: ConfiguredGraph, root: Path) -> list[str]:
    targets = _first_party_targets(graph)
    graph_edges = {
        name: sorted(_link_values(info) & targets)
        for name, info in graph.targets.items() if name in targets
    }
    issues: list[str] = []
    complete: set[str] = set()
    active: list[str] = []

    def visit(target: str) -> None:
        if target in complete:
            return
        if target in active:
            start = active.index(target)
            issues.append("CMake PulseForge target dependency cycle: " + " -> ".join(active[start:] + [target]))
            return
        active.append(target)
        for dependency in graph_edges[target]:
            visit(dependency)
        active.pop()
        complete.add(target)

    for target in sorted(graph_edges):
        visit(target)

    engine_targets = {
        name for name, info in graph.targets.items()
        if any((_source_path(root, source) or "").startswith("PulseForge/Source/") for source in info.sources)
    }

    def reachable(start: str) -> set[str]:
        reached: set[str] = set()
        pending = list(graph_edges.get(start, []))
        while pending:
            current = pending.pop()
            if current in reached:
                continue
            reached.add(current)
            pending.extend(graph_edges.get(current, []))
        return reached

    for target in sorted(engine_targets):
        forbidden = sorted(name for name in reachable(target)
                           if name.startswith("PulseForgeEditor") or name.startswith("PulseForgeGame"))
        if forbidden:
            issues.append(f"Engine target {target} depends on editor/client targets: {', '.join(forbidden)}")
    return issues


def _implementation_owners(root: Path, graph: ConfiguredGraph) -> dict[str, list[str]]:
    owners: dict[str, list[str]] = {}
    for target, info in graph.targets.items():
        for source in info.sources:
            relative = _first_party_source(root, source)
            if relative is not None and "/Source/Private/" in f"/{relative}/":
                owners.setdefault(relative, []).append(target)
    return owners


def check_source_ownership(root: Path, graph: ConfiguredGraph) -> list[str]:
    owners = _implementation_owners(root, graph)
    issues: list[str] = []
    for source, source_owners in sorted(owners.items()):
        if len(source_owners) != 1:
            issues.append(f"First-party implementation {source} has multiple CMake owners: {', '.join(source_owners)}")

    source_roots = ["PulseForge/Source/Private", "PulseForgeGame/Source/Private"]
    if graph.options.get("PULSEFORGE_BUILD_EDITOR") == "ON":
        source_roots.append("PulseForgeEditor/Source/Private")
    for source_root in source_roots:
        directory = root / source_root
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix.lower() in IMPLEMENTATION_SUFFIXES:
                relative = normalize(path.relative_to(root))
                if relative not in owners:
                    issues.append(f"First-party implementation {relative} has no owner in the configured CMake graph")
    return issues


def check_expected_source_targets(
    root: Path,
    graph: ConfiguredGraph,
    expected_owners: dict[str, str],
    conditional_targets: dict[str, str],
) -> list[str]:
    owners = _implementation_owners(root, graph)
    issues = []
    for source, expected in expected_owners.items():
        if expected not in graph.targets:
            condition = conditional_targets.get(expected)
            if condition and graph.options.get(condition) == "OFF":
                continue
            issues.append(f"{normalize(source)} expects unavailable CMake owner {expected}")
            continue
        actual = owners.get(normalize(source), [])
        if expected not in actual:
            issues.append(f"{normalize(source)} must be owned by {expected}")
        other_owners = sorted(set(actual) - {expected})
        if other_owners:
            issues.append(f"{normalize(source)} is also owned by: {', '.join(other_owners)}")
    return issues


def target_dependencies(graph: ConfiguredGraph, target: str) -> set[str] | None:
    target_info = graph.targets.get(target)
    if target_info is None:
        return None
    return _link_values(target_info)


def check_dependencies(graph: ConfiguredGraph, target: str, allowed: list[str]) -> list[str]:
    dependencies = target_dependencies(graph, target)
    if dependencies is None:
        return [f"Configured CMake graph has no enabled target {target}"]
    expected = set(allowed)
    unexpected = sorted(dependencies - expected)
    missing = sorted(expected - dependencies)
    issues = []
    if unexpected:
        issues.append(f"{target} has unreviewed dependencies: {', '.join(unexpected)}")
    if missing:
        issues.append(f"{target} is missing required dependencies: {', '.join(missing)}")
    return issues


def check_reachable_dependencies_excluded(
    graph: ConfiguredGraph,
    target: str,
    forbidden: list[str],
) -> list[str]:
    if target not in graph.targets:
        return [f"Configured CMake graph has no enabled target {target}"]
    visited: set[str] = set()
    pending = list(_link_values(graph.targets[target]))
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        dependency = graph.targets.get(current)
        if dependency is not None:
            pending.extend(_link_values(dependency))
    present = sorted(visited & set(forbidden))
    return [f"{target} reaches forbidden dependencies: {', '.join(present)}"] if present else []


def check_tests_avoid_aggregate_engine(
    graph: ConfiguredGraph,
    aggregate: str,
    test_targets: list[str],
    conditional_targets: dict[str, str],
) -> list[str]:
    issues = []
    for target in test_targets:
        dependencies = target_dependencies(graph, target)
        if dependencies is None:
            condition = conditional_targets.get(target)
            if condition and graph.options.get(condition) == "OFF":
                continue
            issues.append(f"Owner-scoped test target {target} is missing from the configured CMake graph")
        elif aggregate in dependencies:
            issues.append(f"Owner-scoped test target {target} must link implementation owners, not {aggregate}")
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


def check_public_header_ownership(root: Path, rules: dict, graph: ConfiguredGraph) -> list[str]:
    owners = {normalize(path): target for path, target in rules.get("public_header_owners", {}).items()}
    non_api = {normalize(path) for path in rules.get("non_api_public_headers", [])}
    public_roots = set(rules["public_header_roots"])
    discovered: set[str] = set()
    issues: list[str] = []
    editor_enabled = graph.options.get("PULSEFORGE_BUILD_EDITOR") == "ON"

    for public_root in sorted(public_roots):
        directory = root / public_root
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in {".h", ".hpp"}:
                continue
            relative = normalize(path.relative_to(root))
            discovered.add(relative)
            if relative in non_api:
                continue
            target = owners.get(relative)
            if target is None:
                issues.append(f"Public header {relative} has no owner-scoped compile target")
            elif target == rules.get("aggregate_engine_target"):
                issues.append(f"Public header {relative} must not use aggregate target {target} as its owner")
            elif (not relative.startswith("PulseForgeEditor/Source/Public/") or editor_enabled) and target not in graph.targets:
                issues.append(f"Public header {relative} names unavailable owner target {target}")

    for relative in sorted(owners.keys() - discovered):
        issues.append(f"Public header ownership rule refers to missing file {relative}")
    for relative in sorted(non_api - discovered):
        issues.append(f"Non-API public-header exception refers to missing file {relative}")
    for relative in sorted(discovered & non_api):
        if relative in owners:
            issues.append(f"Non-API header {relative} must not also have a compile owner")
    return issues


def target_source_paths(root: Path, graph: ConfiguredGraph, target: str) -> list[str]:
    target_info = graph.targets.get(target)
    if target_info is None:
        return []
    return [relative for source in target_info.sources
            if (relative := _source_path(root, source)) is not None and
            relative.startswith("PulseForgeEditor/Source/Private/") and
            Path(relative).suffix.lower() in IMPLEMENTATION_SUFFIXES]


def _editorconfig_line_ending(root: Path, relative: str) -> bytes | None:
    attributes = _git(root, ["check-attr", "-z", "eol", "--", relative], binary=True)
    if attributes.returncode == 0:
        fields = attributes.stdout.split(b"\0")
        if len(fields) >= 3 and fields[2] not in {b"", b"unspecified", b"unset", b"set"}:
            attribute_eol = {b"crlf": b"\r\n", b"lf": b"\n"}.get(fields[2])
            if attribute_eol:
                return attribute_eol
    path = root / ".editorconfig"
    if not path.exists():
        return None
    current_match = True
    selected: str | None = None
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            pattern = line[1:-1].strip()
            current_match = Path(relative).match(pattern) or pattern == "*"
            continue
        if current_match and "=" in line:
            key, value = (part.strip() for part in line.split("=", 1))
            if key == "end_of_line":
                selected = value.lower()
    return {"crlf": b"\r\n", "lf": b"\n", "cr": b"\r"}.get(selected or "")


def check_cpp_hygiene(root: Path, changed_paths: list[str]) -> list[str]:
    issues = []
    for relative in changed_paths:
        path = root / relative
        if path.suffix.lower() not in {".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl"} or not path.is_file():
            continue
        data = path.read_bytes()
        expected_eol = _editorconfig_line_ending(root, relative)
        if expected_eol and data:
            if expected_eol == b"\r\n" and re.search(rb"(?<!\r)\n", data):
                issues.append(f"{normalize(relative)} must use CRLF line endings (see .editorconfig)")
            elif expected_eol == b"\n" and re.search(rb"\r\n", data):
                issues.append(f"{normalize(relative)} must use LF line endings (see .editorconfig)")
            if not data.endswith(expected_eol):
                issues.append(f"{normalize(relative)} must end with a newline matching .editorconfig")
        for number, line in enumerate(data.splitlines(), 1):
            if line.endswith((b" ", b"\t")):
                issues.append(f"{normalize(relative)}:{number} has trailing whitespace")
    return issues


def _git(root: Path, arguments: list[str], *, binary: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *arguments], cwd=root, text=not binary,
        encoding=None if binary else "utf-8", errors=None if binary else "replace",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def changed_paths(root: Path, comparison_base: str) -> list[str]:
    verified = _git(root, ["rev-parse", "--verify", "--quiet", f"{comparison_base}^{{commit}}"])
    if verified.returncode != 0:
        raise RuntimeError(f"Invalid source-quality comparison base {comparison_base!r}: {verified.stderr.strip()}")
    diff = _git(root, ["diff", "--name-only", "--find-renames", "-z", comparison_base, "--"], binary=True)
    if diff.returncode != 0:
        raise RuntimeError(diff.stderr.strip() or "git diff failed while collecting branch changes")
    paths = {normalize(item.decode("utf-8", errors="strict")) for item in diff.stdout.split(b"\0") if item}
    untracked = _git(root, ["ls-files", "--others", "--exclude-standard", "-z"], binary=True)
    if untracked.returncode != 0:
        raise RuntimeError(untracked.stderr.strip() or "git ls-files failed while collecting untracked sources")
    paths.update(normalize(item.decode("utf-8", errors="strict")) for item in untracked.stdout.split(b"\0") if item)
    return sorted(paths)


def check_git_hygiene(root: Path, comparison_base: str) -> list[str]:
    paths = changed_paths(root, comparison_base)
    issues = check_cpp_hygiene(root, paths)
    diff = _git(root, ["diff", "--check", comparison_base, "--"])
    if diff.returncode != 0:
        issues.append(diff.stdout.strip() or diff.stderr.strip() or "git diff --check failed")
    return issues


def run_graph_checks(root: Path, rules: dict, graph: ConfiguredGraph) -> list[str]:
    issues = []
    issues.extend(check_configuration_target_coverage(graph, root))
    issues.extend(check_target_graph(graph, root))
    issues.extend(check_source_ownership(root, graph))
    conditional_targets = rules.get("conditional_target_options", {})
    issues.extend(check_expected_source_targets(
        root, graph, rules.get("source_target_ownership", {}), conditional_targets))
    issues.extend(check_tests_avoid_aggregate_engine(
        graph,
        rules["aggregate_engine_target"],
        rules["owner_scoped_test_targets"],
        conditional_targets))

    for key in ("editor_core_target", "editor_ui_target"):
        target = rules[key]
        if target not in graph.targets:
            condition = conditional_targets.get(target)
            if condition and graph.options.get(condition) == "OFF":
                continue
        issues.extend(check_dependencies(graph, target, rules[key.replace("target", "allowed_dependencies")]))

    for target, forbidden in rules.get("forbidden_dependency_closures", {}).items():
        issues.extend(check_reachable_dependencies_excluded(graph, target, forbidden))

    core_target = rules["editor_core_target"]
    forbidden = rules["editor_core_forbidden_includes"]
    for relative in target_source_paths(root, graph, core_target):
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
    issues.extend(check_public_header_ownership(root, rules, graph))
    return issues


def run(
    root: Path,
    rules: dict,
    graph: ConfiguredGraph,
    include_git_checks: bool = True,
    comparison_base: str | None = None,
) -> tuple[list[str], list[str]]:
    issues: list[str] = []
    reviews: list[str] = []
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
            reviews.extend(find_size_reviews(relative, line_count, rules))

    issues.extend(run_graph_checks(root, rules, graph))
    if include_git_checks:
        if not comparison_base:
            raise RuntimeError("A Git comparison base is required for source hygiene checks")
        issues.extend(check_git_hygiene(root, comparison_base))
    return issues, reviews


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--rules", type=Path, required=True)
    parser.add_argument("--graph-manifest", type=Path, required=True)
    parser.add_argument("--comparison-base")
    parser.add_argument("--skip-git-checks", action="store_true")
    parser.add_argument("--graph-only", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    root = args.root.resolve()
    rules = json.loads(args.rules.read_text(encoding="utf-8"))
    graph = parse_graph_manifest(args.graph_manifest)
    if args.graph_only:
        issues = run_graph_checks(root, rules, graph)
        reviews: list[str] = []
    else:
        issues, reviews = run(
            root, rules, graph,
            include_git_checks=not args.skip_git_checks,
            comparison_base=args.comparison_base)
    if reviews:
        print(f"Source responsibility review notices ({len(reviews)}):")
        for review in reviews:
            print(f" - {review}")
    if issues:
        print(f"PulseForge source quality checks failed ({graph.configuration} graph):")
        for issue in issues:
            print(f" - {issue}")
        return 1
    print(f"PulseForge source quality checks passed ({graph.configuration} configured CMake graph).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
