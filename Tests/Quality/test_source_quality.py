import importlib.util
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "Tools" / "Quality" / "CheckSourceQuality.py"
SPEC = importlib.util.spec_from_file_location("source_quality", SCRIPT)
source_quality = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(source_quality)

RULES = {
    "default_cpp_line_limit": 1700,
    "default_header_line_limit": 800,
    "size_exceptions": {
        "Legacy.cpp": {"max_lines": 900, "reason": "test fixture"}
    },
}


class SourceQualityRulesTests(unittest.TestCase):
    def test_source_and_header_limits_are_distinct(self):
        self.assertEqual(source_quality.file_line_limit("New.cpp", ".cpp", RULES), 1700)
        self.assertEqual(source_quality.file_line_limit("New.h", ".h", RULES), 800)
        self.assertEqual(source_quality.find_size_violations("New.cpp", 1700, RULES), [])
        self.assertTrue(source_quality.find_size_violations("New.cpp", 1701, RULES))

    def test_size_exception_is_exact_and_bounded(self):
        self.assertEqual(source_quality.find_size_violations("Legacy.cpp", 900, RULES), [])
        self.assertTrue(source_quality.find_size_violations("Legacy.cpp", 901, RULES))
        self.assertTrue(source_quality.find_size_violations("Other.cpp", 1701, RULES))

    def test_dependency_policy_detects_added_link(self):
        cmake = "target_link_libraries(PulseForgeEditorCore PUBLIC PulseForgeAssetServices)"
        self.assertEqual(source_quality.check_dependencies(cmake, "PulseForgeEditorCore", ["PulseForgeAssetServices"]), [])
        changed = cmake[:-1] + " PulseForge)"
        self.assertTrue(source_quality.check_dependencies(changed, "PulseForgeEditorCore", ["PulseForgeAssetServices"]))

    def test_editor_core_include_boundary(self):
        forbidden = ["Core/PulseForgePCH.h", "imgui.h", "Renderer/Vulkan/"]
        self.assertEqual(source_quality.check_includes("State.cpp", '#include "Assets/Project.h"', forbidden), [])
        self.assertTrue(source_quality.check_includes("State.cpp", '#include "Core/PulseForgePCH.h"', forbidden))
        self.assertTrue(source_quality.check_includes("State.cpp", '#include <imgui.h>', forbidden))

    def test_public_header_cannot_include_private_implementation(self):
        forbidden = ["PulseForge/Source/Private/", "Core/PulseForgePCH.h"]
        self.assertEqual(source_quality.check_includes("Public.h", '#include "Core/Core.h"', forbidden), [])
        self.assertTrue(source_quality.check_includes(
            "Public.h", '#include "PulseForge/Source/Private/Renderer.h"', forbidden))

    def test_first_party_target_graph_rejects_cycles(self):
        cmake = """
add_library(PulseForgeCore STATIC Core.cpp)
add_library(PulseForgeScene STATIC Scene.cpp)
target_link_libraries(PulseForgeCore PUBLIC PulseForgeScene)
target_link_libraries(PulseForgeScene PUBLIC PulseForgeCore)
"""
        self.assertTrue(source_quality.check_target_graph(cmake))

    def test_engine_target_cannot_depend_on_editor(self):
        cmake = """
add_library(PulseForgeCore STATIC Core.cpp)
add_library(PulseForgeEditorUI STATIC UI.cpp)
target_link_libraries(PulseForgeCore PRIVATE PulseForgeEditorUI)
"""
        self.assertTrue(source_quality.check_target_graph(cmake))

    def test_first_party_implementation_must_have_one_owner(self):
        import tempfile

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "PulseForge" / "Source" / "Private" / "Core.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("", encoding="utf-8")
            self.assertEqual(source_quality.check_source_ownership(
                root, "add_library(PulseForgeCore STATIC PulseForge/Source/Private/Core.cpp)"), [])
            self.assertTrue(source_quality.check_source_ownership(
                root, "add_library(PulseForgeCore STATIC PulseForge/Source/Private/Core.cpp)\n"
                "add_library(PulseForgeTests STATIC PulseForge/Source/Private/Core.cpp)"))


if __name__ == "__main__":
    unittest.main()
