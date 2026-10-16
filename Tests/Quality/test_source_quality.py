import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "Tools" / "Quality" / "CheckSourceQuality.py"
SPEC = importlib.util.spec_from_file_location("source_quality", SCRIPT)
source_quality = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = source_quality
SPEC.loader.exec_module(source_quality)

RULES = {
    "source_roots": [],
    "aggregate_engine_target": "PulseForge",
    "editor_core_target": "PulseForgeEditorCore",
    "editor_core_allowed_dependencies": [],
    "editor_core_forbidden_includes": [],
    "editor_ui_target": "PulseForgeEditorUI",
    "editor_ui_allowed_dependencies": [],
    "conditional_target_options": {},
    "owner_scoped_test_targets": [],
    "source_target_ownership": {},
    "forbidden_dependency_closures": {},
    "public_header_roots": {},
    "public_header_owners": {},
    "non_api_public_headers": [],
}


def make_target(kind, **properties):
    return source_quality.ConfiguredTarget(
        kind=kind,
        sources=properties.get("sources", []),
        links=properties.get("links", []),
        interface=properties.get("interface", []),
        definitions=properties.get("definitions", []),
    )


class SourceQualityRulesTests(unittest.TestCase):
    def test_file_length_alone_does_not_fail_quality_run(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source_root = root / "src"
            source_root.mkdir()
            (source_root / "Long.cpp").write_text("// line\n" * 10000, encoding="utf-8")
            graph = source_quality.ConfiguredGraph("Debug", {}, {
                "PulseForgeEditorCore": make_target("STATIC_LIBRARY"),
                "PulseForgeEditorUI": make_target("STATIC_LIBRARY"),
            })
            rules = {**RULES, "source_roots": ["src"]}
            self.assertEqual(source_quality.run(root, rules, graph, include_git_checks=False), [])

    def test_architecture_violations_still_fail_quality_run(self):
        graph = source_quality.ConfiguredGraph("Debug", {}, {
            "PulseForgeEditorCore": make_target("STATIC_LIBRARY", links=["PulseForgeEditorUI"]),
            "PulseForgeEditorUI": make_target("STATIC_LIBRARY", links=["PulseForgeEditorCore"]),
        })
        issues = source_quality.run(Path.cwd(), RULES, graph, include_git_checks=False)
        self.assertTrue(any("dependency cycle" in issue for issue in issues))
        self.assertTrue(any("has unreviewed dependencies" in issue for issue in issues))

    def test_dependency_policy_includes_all_configured_link_properties(self):
        graph = source_quality.ConfiguredGraph("Debug", {}, {
            "PulseForgeEditorCore": make_target(
                "STATIC_LIBRARY",
                links=["PulseForgeAssetServices", "PulseForgeScene"],
                interface=["PulseForgeAssetServices", "PulseForgeScene", "PulseForgeImGui"]),
        })
        issues = source_quality.check_dependencies(
            graph, "PulseForgeEditorCore", ["PulseForgeAssetServices", "PulseForgeScene"])
        self.assertEqual(len(issues), 1)
        self.assertIn("PulseForgeImGui", issues[0])

    def test_dependency_policy_checks_transitive_closures(self):
        graph = source_quality.ConfiguredGraph("Debug", {}, {
            "PulseForgeAssetImporters": make_target("STATIC_LIBRARY", links=["PulseForgeMeshData"]),
            "PulseForgeMeshData": make_target("STATIC_LIBRARY", links=["PulseForgeRenderer"]),
            "PulseForgeRenderer": make_target("STATIC_LIBRARY"),
        })
        self.assertEqual(source_quality.check_reachable_dependencies_excluded(
            graph, "PulseForgeAssetImporters", ["PulseForgeApplication"]), [])
        self.assertIn("PulseForgeRenderer", source_quality.check_reachable_dependencies_excluded(
            graph, "PulseForgeAssetImporters", ["PulseForgeRenderer"])[0])

    def test_configured_graph_detects_cycles_and_engine_editor_edges(self):
        graph = source_quality.ConfiguredGraph("Debug", {}, {
            "PulseForgeCore": make_target(
                "STATIC_LIBRARY", sources=["PulseForge/Source/Private/Core/Core.cpp"], links=["PulseForgeEditorUI"]),
            "PulseForgeEditorUI": make_target("STATIC_LIBRARY", links=["PulseForgeCore"]),
        })
        issues = source_quality.check_target_graph(graph, Path.cwd())
        self.assertTrue(any("dependency cycle" in issue for issue in issues))
        self.assertTrue(any("depends on editor/client targets" in issue for issue in issues))

    def test_configuration_coverage_reads_configured_definitions(self):
        source = "PulseForge/Source/Private/Core/Core.cpp"
        graph = source_quality.ConfiguredGraph("Release", {}, {
            "PulseForgeCore": make_target("STATIC_LIBRARY", sources=[source], definitions=["PF_RELEASE"])
        })
        self.assertEqual(source_quality.check_configuration_target_coverage(graph, Path.cwd()), [])
        graph.targets["PulseForgeCore"].definitions = ["PF_DEBUG"]
        self.assertTrue(source_quality.check_configuration_target_coverage(graph, Path.cwd()))

    def test_source_ownership_uses_target_source_metadata(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "PulseForge" / "Source" / "Private" / "Core.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("", encoding="utf-8")
            graph = source_quality.ConfiguredGraph("Debug", {"PULSEFORGE_BUILD_EDITOR": "OFF"}, {
                "PulseForgeCore": make_target(
                    "STATIC_LIBRARY", sources=["PulseForge/Source/Private/Core.cpp"])
            })
            self.assertEqual(source_quality.check_source_ownership(root, graph), [])
            graph.targets["PulseForgeOther"] = make_target(
                "STATIC_LIBRARY", sources=["PulseForge/Source/Private/Core.cpp"])
            self.assertTrue(source_quality.check_source_ownership(root, graph))

    def test_public_header_include_boundary(self):
        forbidden = ["PulseForge/Source/Private/", "Core/PulseForgePCH.h"]
        self.assertEqual(source_quality.check_includes("Public.h", '#include "Core/Core.h"', forbidden), [])
        self.assertTrue(source_quality.check_includes(
            "Public.h", '#include "PulseForge/Source/Private/Renderer.h"', forbidden))

    def test_public_headers_require_a_live_nonaggregate_owner(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            public_directory = root / "PulseForge" / "Source" / "Public" / "Assets"
            public_directory.mkdir(parents=True)
            header = public_directory / "ImageImport.h"
            header.write_text("#pragma once\n", encoding="utf-8")
            relative = "PulseForge/Source/Public/Assets/ImageImport.h"
            rules = {
                "aggregate_engine_target": "PulseForge",
                "public_header_roots": {"PulseForge/Source/Public": []},
                "public_header_owners": {relative: "PulseForgeAssetImporters"},
                "non_api_public_headers": [],
            }
            graph = source_quality.ConfiguredGraph("Debug", {}, {
                "PulseForgeAssetImporters": make_target("STATIC_LIBRARY")
            })
            self.assertEqual(source_quality.check_public_header_ownership(root, rules, graph), [])
            rules["public_header_owners"][relative] = "PulseForge"
            self.assertTrue(any("aggregate target" in issue for issue in
                                source_quality.check_public_header_ownership(root, rules, graph)))
            rules["public_header_owners"][relative] = "PulseForgeMissing"
            self.assertTrue(any("unavailable owner" in issue for issue in
                                source_quality.check_public_header_ownership(root, rules, graph)))
            rules["public_header_owners"].clear()
            self.assertTrue(any("no owner-scoped" in issue for issue in
                                source_quality.check_public_header_ownership(root, rules, graph)))

    def test_cpp_line_endings_follow_editorconfig(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            (root / ".editorconfig").write_text(
                "root = true\n[*]\nend_of_line = lf\n[*.cpp]\nend_of_line = crlf\n", encoding="utf-8")
            self.assertEqual(source_quality._editorconfig_line_ending(root, "Code.cpp"), b"\r\n")
            self.assertEqual(source_quality._editorconfig_line_ending(root, "script.py"), b"\n")
            path = root / "Code.cpp"
            path.write_bytes(b"int value;\n")
            self.assertTrue(source_quality.check_cpp_hygiene(root, ["Code.cpp"]))

    def test_changed_paths_use_explicit_base_and_include_full_branch(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)

            def git(*arguments):
                result = subprocess.run(["git", *arguments], cwd=root, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout.strip()

            git("init", "-b", "main")
            git("config", "user.name", "PulseForge Test")
            git("config", "user.email", "pulseforge-test@example.invalid")
            (root / "old.cpp").write_text("int old_value;\n", encoding="utf-8")
            git("add", "old.cpp")
            git("commit", "-m", "baseline")
            base = git("rev-parse", "HEAD")
            (root / "old.cpp").write_text("int old_value; \n", encoding="utf-8")
            git("commit", "-am", "introduce whitespace issue")
            (root / "later.txt").write_text("later\n", encoding="utf-8")
            git("add", "later.txt")
            git("commit", "-m", "later clean change")
            self.assertEqual(source_quality.changed_paths(root, base), ["later.txt", "old.cpp"])
            self.assertTrue(source_quality.check_git_hygiene(root, base))

    def test_quality_entry_consumes_actual_configured_cmake_metadata(self):
        cmake = os.environ.get("PULSEFORGE_QUALITY_CMAKE_COMMAND", "cmake")
        generator = os.environ.get("PULSEFORGE_QUALITY_GENERATOR")
        platform = os.environ.get("PULSEFORGE_QUALITY_GENERATOR_PLATFORM")
        make_program = os.environ.get("PULSEFORGE_QUALITY_MAKE_PROGRAM")
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            (root / "A.cpp").write_text("int a;\n", encoding="utf-8")
            (root / "B.cpp").write_text("int b;\n", encoding="utf-8")
            (root / "Core.cpp").write_text("int core;\n", encoding="utf-8")
            exporter = (REPOSITORY / "Tools" / "Quality" / "ExportConfiguredTargetGraph.cmake").as_posix()
            cmake_variable = "$" + "{A_DEPS}"
            cmake_target = "$" + "{Target}"
            cmakelists = "\n".join([
                "cmake_minimum_required(VERSION 3.24)",
                "project(PulseForgeGraphFixture NONE)",
                "add_library(PulseForgeA INTERFACE)",
                "add_library(PulseForgeB INTERFACE)",
                "add_library(PulseForgeScene INTERFACE)",
                "add_library(PulseForgeImGui INTERFACE)",
                "add_library(PulseForgeEditorUI INTERFACE)",
                "add_library(PulseForgeEditorCore INTERFACE)",
                "set_property(TARGET PulseForgeA PROPERTY SOURCES A.cpp)",
                "set_property(TARGET PulseForgeB PROPERTY SOURCES B.cpp)",
                "set_property(TARGET PulseForgeEditorCore PROPERTY SOURCES Core.cpp)",
                "set(A_DEPS PulseForgeB)",
                f"target_link_libraries(PulseForgeA INTERFACE {cmake_variable})",
                "target_link_libraries(PulseForgeB INTERFACE PulseForgeA)",
                "target_link_libraries(PulseForgeEditorCore INTERFACE PulseForgeScene)",
                "target_link_libraries(PulseForgeEditorCore INTERFACE PulseForgeImGui)",
                "foreach(Target PulseForgeA PulseForgeB PulseForgeEditorCore)",
                f"    set_property(TARGET {cmake_target} PROPERTY COMPILE_DEFINITIONS PF_DEBUG)",
                "endforeach()",
                f"include(\"{exporter}\")",
                "",
            ])
            (root / "CMakeLists.txt").write_text(cmakelists, encoding="utf-8")
            (root / "rules.json").write_text(json.dumps({
                "source_roots": [],
                "source_target_ownership": {},
                "owner_scoped_test_targets": [],
                "conditional_target_options": {},
                "aggregate_engine_target": "PulseForge",
                "editor_core_target": "PulseForgeEditorCore",
                "editor_core_allowed_dependencies": ["PulseForgeScene"],
                "editor_ui_target": "PulseForgeEditorUI",
                "editor_ui_allowed_dependencies": [],
                "editor_core_forbidden_includes": [],
                "public_header_roots": {},
                "public_header_owners": {},
                "non_api_public_headers": [],
                "forbidden_dependency_closures": {},
            }), encoding="utf-8")
            build = root / "build"
            command = [cmake, "-S", str(root), "-B", str(build)]
            command.append("-DCMAKE_BUILD_TYPE:STRING=Debug")
            if generator:
                command.extend(["-G", generator])
            if platform:
                command.extend(["-A", platform])
            if make_program:
                command.append(f"-DCMAKE_MAKE_PROGRAM:FILEPATH={make_program}")
            configured = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(configured.returncode, 0, configured.stdout)
            manifest = build / "PulseForgeTargetGraph-Debug.txt"
            if not manifest.exists():
                manifest = build / "PulseForgeTargetGraph-Release.txt"
            self.assertTrue(manifest.exists(), configured.stdout)
            checked = subprocess.run([
                sys.executable, "-B", str(SCRIPT),
                "--root", str(root),
                "--rules", str(root / "rules.json"),
                "--graph-manifest", str(manifest),
                "--graph-only",
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(checked.returncode, 0, checked.stdout)
            self.assertIn("dependency cycle", checked.stdout)
            self.assertIn("PulseForgeImGui", checked.stdout)


if __name__ == "__main__":
    unittest.main()
