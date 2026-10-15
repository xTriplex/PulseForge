from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Tools" / "Quality"))

import CheckCodeQualityTools as quality_tools


REPOSITORY = Path(__file__).resolve().parents[2]


class CodeQualityToolTests(unittest.TestCase):
	def setUp(self):
		self.temporary_directory = tempfile.TemporaryDirectory()
		self.root = Path(self.temporary_directory.name)
		subprocess.run(["git", "init", "-b", "main"], cwd=self.root, check=True,
			stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
		subprocess.run(["git", "config", "user.name", "PulseForge Test"], cwd=self.root, check=True)
		subprocess.run(["git", "config", "user.email", "pulseforge-test@example.invalid"], cwd=self.root, check=True)
		(self.root / ".clang-format").write_bytes((REPOSITORY / ".clang-format").read_bytes())
		self.source = self.root / "Folder with spaces" / "Sample source.cpp"
		self.source.parent.mkdir()
		self.source.write_text("int Value(int value)\n{\n\treturn value;\n}\n", encoding="utf-8")
		subprocess.run(["git", "add", "."], cwd=self.root, check=True)
		subprocess.run(["git", "commit", "-m", "baseline"], cwd=self.root, check=True,
			stdout=subprocess.DEVNULL)
		self.base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.root, text=True).strip()

	def tearDown(self):
		self.temporary_directory.cleanup()

	def test_changed_line_ranges_and_metrics_include_space_paths_and_untracked_sources(self):
		self.source.write_text(
			"int Value(int value)\n{\n\tif (value > 0) return value;\n\treturn -value;\n}\n",
			encoding="utf-8")
		added = self.root / "Another folder" / "New source.cpp"
		added.parent.mkdir()
		added.write_text("int Added() { return 7; }\n", encoding="utf-8")
		changed = quality_tools.changed_cpp_paths(self.root, self.base)
		self.assertEqual(changed, ["Another folder/New source.cpp", "Folder with spaces/Sample source.cpp"])
		self.assertEqual(
			quality_tools.changed_line_ranges(self.root, "Folder with spaces/Sample source.cpp", self.base),
			[(3, 4)],
		)
		version, functions = quality_tools.changed_function_metrics(self.root, changed, self.base)
		self.assertEqual(version, quality_tools.LIZARD_VERSION)
		self.assertEqual({function["name"] for function in functions}, {"Added", "Value"})
		self.assertTrue(all(function["nloc"] > 0 for function in functions))

	def test_changed_lines_are_checked_by_the_pinned_clang_formatter(self):
		executable = os.environ.get("PULSEFORGE_CLANG_FORMAT_EXECUTABLE")
		if not executable:
			self.fail("CMake must provide the required pinned clang-format path to this test")
		self.source.write_text("int Value(int value){return value;}\n", encoding="utf-8")
		paths = ["Folder with spaces/Sample source.cpp"]
		issues = quality_tools.check_formatting(self.root, paths, self.base, executable)
		self.assertEqual(len(issues), 1)

		formatted = subprocess.run([executable, "--style=file", str(self.source)], cwd=self.root,
			text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
		self.source.write_text(formatted.stdout, encoding="utf-8")
		self.assertEqual(quality_tools.check_formatting(self.root, paths, self.base, executable), [])


if __name__ == "__main__":
	unittest.main()
