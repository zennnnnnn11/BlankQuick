"""Prove that the real CMake fence rejects violations at configure time."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ArchitectureFenceTests(unittest.TestCase):
    def configure(self, dependency, layer="domain", register=True):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            # Deliberately allowlist the edge: the layer rule must independently
            # prevent a developer from granting a data library system access.
            policy = {
                "layers": {"domain": ["domain"], "platform": ["domain", "platform"]},
                "target_layers": {"pure": "domain", "dependency": layer},
                "target_dependencies": {"pure": [dependency], "dependency": []},
            }
            (source / "policy.json").write_text(json.dumps(policy), encoding="utf-8")
            cmake = f'''cmake_minimum_required(VERSION 3.30)
project(ArchitectureFence LANGUAGES NONE)
include("{(ROOT / 'cmake/Architecture.cmake').as_posix()}")
add_library(pure INTERFACE)
add_library(dependency INTERFACE)
add_library(Qt6::Network INTERFACE IMPORTED)
add_library(Alias::Dependency ALIAS dependency)
target_link_libraries(pure INTERFACE {dependency})
'''
            if not register:
                cmake += "add_library(unregistered INTERFACE)\n"
            cmake += '''file(READ "${CMAKE_CURRENT_SOURCE_DIR}/policy.json" policy)
blank_check_directory("${policy}" "${CMAKE_CURRENT_SOURCE_DIR}")
'''
            (source / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
            return subprocess.run(["cmake", "-S", str(source), "-B", str(source / "build")],
                                  capture_output=True, text=True, encoding="utf-8", errors="replace")

    def test_data_can_link_data(self):
        result = self.configure("dependency")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_even_an_allowlisted_effect_is_rejected(self):
        result = self.configure("dependency", layer="platform")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot depend on platform", result.stderr)

    def test_network_library_is_rejected(self):
        result = self.configure("Qt6::Network")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot link external library Qt6::Network", result.stderr)

    def test_alias_cannot_hide_dependency(self):
        result = self.configure("Alias::Dependency", layer="platform")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is not an allowed dependency", result.stderr)

    def test_unregistered_target_cannot_bypass_policy(self):
        result = self.configure("dependency", register=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("register build target unregistered", result.stderr)

    def test_data_cannot_include_effect_classes_from_qt_core(self):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        import check_architecture
        for header in ("QProcess", "QtCore/QFile", "QtNetwork/QHostAddress", "Windows.h"):
            with self.subTest(header=header), self.assertRaisesRegex(ValueError, "effect API"):
                check_architecture.check_data_source("fixture.cpp", f"#include <{header}>")

    def test_compiler_rejects_effect_headers_even_through_macro(self):
        build = os.environ.get("BLANK_ARCHITECTURE_BUILD")
        compiler = os.environ.get("BLANK_CXX_COMPILER")
        if not build or not compiler:
            self.skipTest("Run under CTest to exercise the configured compiler fence")
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "violation.cpp"
            for header in ("windows.h", "QProcess", "QtCore/QFile"):
                with self.subTest(header=header):
                    source.write_text(f"#define EFFECT_HEADER <{header}>\n#include EFFECT_HEADER\n",
                                      encoding="utf-8")
                    result = subprocess.run(
                        [compiler, "/nologo", "/c", f"/I{Path(build) / 'data-boundary'}", str(source),
                         f"/Fo{Path(temporary) / 'violation.obj'}"],
                        capture_output=True, text=True, errors="replace")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("BLANK_DATA_BOUNDARY", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
