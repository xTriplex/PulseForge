from __future__ import annotations

import argparse
import importlib.metadata
import json
import re
import subprocess
import sys
from pathlib import Path

from CheckSourceQuality import _git, changed_paths, normalize


CPP_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl"}
LIZARD_VERSION = "1.24.1"
CLANG_FORMAT_VERSION = "19.1.5"
MAX_REPORTED_FUNCTIONS = 10
HUNK_PATTERN = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.MULTILINE)


def _is_tracked(root: Path, relative: str) -> bool:
	return _git(root, ["ls-files", "--error-unmatch", "--", relative]).returncode == 0


def changed_line_ranges(root: Path, relative: str, comparison_base: str) -> list[tuple[int, int]]:
	path = root / relative
	if not _is_tracked(root, relative):
		if not path.is_file():
			return []
		return [(1, max(1, len(path.read_text(encoding="utf-8").splitlines())))]

	diff = _git(root, ["diff", "--unified=0", "--find-renames", comparison_base, "--", relative])
	if diff.returncode != 0:
		raise RuntimeError(diff.stderr.strip() or f"Could not read changed lines for {relative}")
	if not path.is_file():
		return []

	ranges = []
	for match in HUNK_PATTERN.finditer(diff.stdout):
		start = int(match.group(1))
		count = int(match.group(2) or "1")
		if count > 0:
			ranges.append((start, start + count - 1))
	return ranges


def changed_cpp_paths(root: Path, comparison_base: str) -> list[str]:
	return [
		relative for relative in changed_paths(root, comparison_base)
		if Path(relative).suffix.lower() in CPP_SUFFIXES and (root / relative).is_file()
	]


def changed_function_metrics(root: Path, paths: list[str], comparison_base: str):
	try:
		import lizard
	except ImportError as error:
		raise RuntimeError(
			f"Lizard {LIZARD_VERSION} is required. Run Tools/Quality/BootstrapQualityTools.py "
			f"with a build-local --venv before configuring tests: {error}") from error

	version = importlib.metadata.version("lizard")
	if version != LIZARD_VERSION:
		raise RuntimeError(f"Expected Lizard {LIZARD_VERSION}, found {version}")

	measured = []
	for relative in paths:
		path = root / relative
		ranges = changed_line_ranges(root, relative, comparison_base)
		try:
			file_info = lizard.analyze_file(str(path))
		except Exception as error:
			raise RuntimeError(f"Lizard could not analyze {relative}: {error}") from error
		for function in file_info.function_list:
			if not any(function.start_line <= end and function.end_line >= start for start, end in ranges):
				continue
			measured.append({
				"path": normalize(relative),
				"name": function.name,
				"start_line": function.start_line,
				"end_line": function.end_line,
				"nloc": function.nloc,
				"cyclomatic_complexity": function.cyclomatic_complexity,
			})
	return version, measured


def check_formatting(root: Path, paths: list[str], comparison_base: str, executable: str) -> list[str]:
	version = subprocess.run([executable, "--version"], cwd=root, text=True,
		encoding="utf-8", errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
	if version.returncode != 0 or f"clang-format version {CLANG_FORMAT_VERSION}" not in version.stdout:
		return [f"Expected clang-format {CLANG_FORMAT_VERSION}; got {version.stdout.strip() or version.returncode}"]

	issues = []
	for relative in paths:
		ranges = changed_line_ranges(root, relative, comparison_base)
		if not ranges:
			continue
		arguments = [executable, "--dry-run", "--Werror", "--style=file"]
		if _is_tracked(root, relative):
			arguments.extend(f"--lines={start}:{end}" for start, end in ranges)
		arguments.append(relative)
		result = subprocess.run(arguments, cwd=root, text=True,
			encoding="utf-8", errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
		if result.returncode != 0:
			output_lines = result.stdout.splitlines()
			preview = "\n".join(output_lines[:4])
			if len(output_lines) > 4:
				preview += f"\n... {len(output_lines) - 4} additional formatter diagnostics"
			issues.append(f"{relative} is not formatted by clang-format {CLANG_FORMAT_VERSION}:\n{preview}")
	return issues


def write_metrics_report(
	path: Path, configuration: str, comparison_base: str, tool_version: str,
	changed_files: list[str], functions: list[dict],
) -> None:
	report = {
		"tool": "Lizard",
		"version": tool_version,
		"metrics": {
			"nloc": "non-comment source lines within a function",
			"cyclomatic_complexity": "Lizard's cyclomatic complexity number; this is not cognitive complexity",
		},
		"configuration": configuration,
		"comparison_base": comparison_base,
		"changed_cpp_files": len(changed_files),
		"changed_functions_measured": len(functions),
		"largest_functions_by_nloc": sorted(functions, key=lambda f: (-f["nloc"], f["path"], f["start_line"]))[:MAX_REPORTED_FUNCTIONS],
		"largest_functions_by_cyclomatic_complexity": sorted(
			functions,
			key=lambda f: (-f["cyclomatic_complexity"], f["path"], f["start_line"]),
		)[:MAX_REPORTED_FUNCTIONS],
	}
	path.parent.mkdir(parents=True, exist_ok=True)
	path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--root", type=Path, required=True)
	parser.add_argument("--comparison-base", required=True)
	parser.add_argument("--configuration", required=True)
	parser.add_argument("--clang-format", required=True)
	parser.add_argument("--metrics-output", type=Path, required=True)
	args = parser.parse_args()
	root = args.root.resolve()
	try:
		paths = changed_cpp_paths(root, args.comparison_base)
		format_issues = check_formatting(root, paths, args.comparison_base, args.clang_format)
		version, functions = changed_function_metrics(root, paths, args.comparison_base)
		write_metrics_report(args.metrics_output, args.configuration, args.comparison_base,
			version, paths, functions)
	except (OSError, RuntimeError, UnicodeError) as error:
		print(f"PulseForge code quality tools unavailable or failed: {error}", file=sys.stderr)
		return 1

	print(f"Formatting check: clang-format {CLANG_FORMAT_VERSION}, {len(paths)} changed C++ files")
	print(f"Function metrics: Lizard {version}, {len(functions)} changed functions in {len(paths)} files")
	print(f"Metrics written to {args.metrics_output}")
	if format_issues:
		print("Changed-code formatting failed:", file=sys.stderr)
		for issue in format_issues[:20]:
			print(f" - {issue}", file=sys.stderr)
		if len(format_issues) > 20:
			print(f" - {len(format_issues) - 20} additional files have formatting violations", file=sys.stderr)
		return 1
	print("Changed-code formatting passed; function size and cyclomatic complexity are reported for review.")
	return 0


if __name__ == "__main__":
	sys.exit(main())
