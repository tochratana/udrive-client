# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("production_checks", ROOT / "admin/check_production.py")
checks = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checks)


class TestProductionChecks(unittest.TestCase):
    def test_missing_or_disabled_tests_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            with self.assertRaises(checks.CheckError):
                checks.read_cache(build)
            (build / "CMakeCache.txt").write_text("BUILD_TESTING:BOOL=OFF\n")
            with self.assertRaisesRegex(checks.CheckError, "BUILD_TESTING"):
                checks.read_cache(build)

    def test_build_from_another_checkout_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text(f"BUILD_TESTING:BOOL=ON\nCMAKE_HOME_DIRECTORY:INTERNAL={build}\n")
            with self.assertRaisesRegex(checks.CheckError, "another source checkout"):
                checks.read_cache(build)

    def test_valid_cache_preserves_tool_paths_with_spaces(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            tool = "/example/CMake Tools/bin/cmake"
            (build / "CMakeCache.txt").write_text(f"BUILD_TESTING:BOOL=ON\nCMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\nCMAKE_COMMAND:INTERNAL={tool}\n")
            self.assertEqual(checks.read_cache(build)["CMAKE_COMMAND"], tool)

    def test_empty_partial_or_malformed_test_inventory_is_rejected(self):
        for output in ["not json", "{}", '{"tests": []}', '{"tests": [{"name": "UnrelatedTest"}]}']:
            with self.subTest(output=output), self.assertRaises(checks.CheckError):
                checks.verify_test_plan(output)
        self.assertEqual(checks.verify_test_plan(json.dumps({"tests": [{"name": name} for name in checks.REQUIRED_TESTS]})), len(checks.REQUIRED_TESTS))

    def test_failed_policy_stops_before_build_or_test(self):
        with patch.object(checks, "run", side_effect=subprocess.CalledProcessError(1, ["python"])) as run:
            self.assertEqual(checks.main(["--policies-only"]), 1)
            self.assertEqual(run.call_count, 1)

    def test_disabled_tests_cannot_make_the_suite_appear_green(self):
        tests = [{"name": name} for name in checks.REQUIRED_TESTS]
        tests.append({"name": "DisabledRegressionTest", "properties": [{"name": "DISABLED", "value": True}]})
        with self.assertRaisesRegex(checks.CheckError, "Disabled tests"):
            checks.verify_test_plan(json.dumps({"tests": tests}))

    def test_failed_build_stops_before_ctest(self):
        cache = {"CMAKE_COMMAND": "cmake", "CMAKE_CTEST_COMMAND": "ctest"}
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            if command[0] == "cmake":
                raise subprocess.CalledProcessError(1, command)

        with patch.object(checks, "read_cache", return_value=cache), patch.object(checks, "resolve_tool", side_effect=lambda override, cached, name: name), patch.object(checks, "run", side_effect=run):
            self.assertEqual(checks.main(["--build-dir", "build-test"]), 1)
        self.assertFalse(any(command[0] == "ctest" for command in calls))

    def test_full_check_requires_inventory_and_propagates_test_failure(self):
        cache = {"CMAKE_COMMAND": "cmake", "CMAKE_CTEST_COMMAND": "ctest"}
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            if "--show-only=json-v1" in command:
                return subprocess.CompletedProcess(command, 0, json.dumps({"tests": [{"name": name} for name in checks.REQUIRED_TESTS]}))
            if command[0] == "ctest":
                self.assertIn("--no-tests=error", command)
                self.assertIn("--output-on-failure", command)
                raise subprocess.CalledProcessError(8, command)

        with patch.object(checks, "read_cache", return_value=cache), patch.object(checks, "resolve_tool", side_effect=lambda override, cached, name: name), patch.object(checks, "run", side_effect=run):
            self.assertEqual(checks.main(["--build-dir", "build-test"]), 1)
        self.assertEqual(len([command for command in calls if command[0] == "ctest"]), 2)


if __name__ == "__main__":
    unittest.main()
