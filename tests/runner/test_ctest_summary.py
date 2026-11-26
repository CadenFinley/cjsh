#!/usr/bin/env python3

# test_ctest_summary.py
#
# This file is part of cjsh, CJ's Shell
#
# MIT License
#
# Copyright (c) 2026 Caden Finley
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Exercise the summary through real CTest runs, including failure and filtering."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class CTestSummaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cjsh ctest summary ")
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        self.runner = Path(__file__).resolve().parent
        custom = (self.runner / "CTestCustom.cmake.in").read_text()
        custom = custom.replace("@CMAKE_COMMAND@", shutil.which("cmake"))
        custom = custom.replace("@CMAKE_CURRENT_SOURCE_DIR@", str(self.runner.parent))
        (self.build / "CTestCustom.cmake").write_text(custom)
        (self.build / "CTestTestfile.cmake").write_text("")
        (self.build / "emit.py").write_text(
            "import json, sys\n"
            "from pathlib import Path\n"
            "output, status = json.loads(Path(sys.argv[1]).read_text())\n"
            "sys.stdout.buffer.write(output.encode('utf-8', 'surrogateescape') + b'\\n')\n"
            "sys.exit(status)\n"
        )

    def add_suite(self, name, output, status=0, properties=""):
        payload = self.build / (name + ".json")
        payload.write_text(json.dumps([output, status]))
        arguments = [name, sys.executable, str(self.build / "emit.py"), str(payload)]
        with (self.build / "CTestTestfile.cmake").open("a") as tests:
            tests.write("add_test(" + " ".join(f"[==[{arg}]==]" for arg in arguments) + ")\n")
            if properties:
                tests.write(f"set_tests_properties({name} PROPERTIES {properties})\n")

    def run_ctest(self, *arguments, status=0):
        result = subprocess.run(
            ["ctest", "--test-dir", str(self.build), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        self.assertEqual(result.returncode, status, result.stdout)
        return result.stdout

    def assert_counts(self, output, total, passed, failed, skipped):
        self.assertIn(
            f"Total individual tests: {total}\nPassed: {passed}\n"
            f"Failed: {failed}\nSkipped: {skipped}\n",
            output,
        )

    def test_combines_formats_without_counting_diagnostics_twice(self):
        self.add_suite(
            "shell.test_demo",
            "\033[32mPASS\033[0m: first\n  [PASS] second\nthird: PASS\n"
            "BYPASS FAILURE PASS_suffix NOT_PASS\nPassed: 3\nFailed: 0",
        )
        self.add_suite(
            "native",
            "1/1 Testing: diagnostic text\n\udcff: No such file or directory\n"
            "PASS: example\nAll 7 native tests passed",
        )
        self.add_suite("python", "Ran 5 tests in 0.1s\n\nOK (skipped=1)")
        self.add_suite("loop", "Loop syntax: 148 checks, 0 failures")
        self.add_suite("child", "PASS: all 16 child launches preserved SIGTERM status")
        self.add_suite("generate_completions_integration_tests", "scenario passed")
        self.add_suite("agent", "All 8 agent-mode tests passed")
        output = self.run_ctest("--parallel", "4")
        self.assert_counts(output, 188, 187, 0, 1)
        self.assertIn("100% individual tests passed out of 187 executed", output)

    def test_failed_and_unreported_suites(self):
        self.add_suite(
            "shell.test_demo", "PASS: first\nPASS: second\nFAIL: third\nPassed: 2\nFailed: 1", 1
        )
        self.add_suite("native", "1/7 native tests failed", 1)
        self.add_suite(
            "python", "Ran 5 tests in 0.1s\nFAILED (failures=1, errors=1, skipped=1)", 1
        )
        self.add_suite("crash", "stopped before reporting results", 1)
        output = self.run_ctest(status=8)
        self.assert_counts(output, 15, 10, 4, 1)
        self.assertIn("Suites without complete individual results: 1", output)
        self.assertNotIn("% individual tests passed", output)

    def test_filtered_repeated_and_empty_runs_do_not_reuse_counts(self):
        self.add_suite("small", "All 3 small tests passed")
        self.add_suite("large", "All 7 large tests passed")
        self.assert_counts(self.run_ctest(), 10, 10, 0, 0)
        # An interrupted older run may leave a temporary log behind.
        old_log = self.build / "Testing/Temporary/LastTest.log.tmp-old"
        old_log.write_text((self.build / "Testing/Temporary/LastTest.log").read_text())
        os.utime(old_log, (1, 1))
        self.assert_counts(self.run_ctest("-R", "^small$"), 3, 3, 0, 0)
        self.assert_counts(
            self.run_ctest("-R", "^large$", "--repeat", "until-fail:2"), 14, 14, 0, 0
        )
        self.assertNotIn("Total individual tests:", self.run_ctest("-N"))
        self.assertNotIn("Total individual tests:", self.run_ctest("-R", "absent"))

    def test_ctest_failure_takes_precedence_over_passing_counts(self):
        self.add_suite(
            "native", "All 7 native tests passed",
            properties='FAIL_REGULAR_EXPRESSION "passed"',
        )
        output = self.run_ctest(status=8)
        self.assertIn("Suites without complete individual results: 1", output)
        self.assertNotIn("100% individual tests passed", output)

    def test_shell_failure_after_passing_checks_has_incomplete_results(self):
        self.add_suite("shell.test_crash", "PASS: first\nPASS: second\nterminated", 1)
        self.add_suite("shell.test_empty", "no individual results", 1)
        output = self.run_ctest(status=8)
        self.assert_counts(output, 2, 2, 0, 0)
        self.assertIn("Suites without complete individual results: 2", output)
        self.assertNotIn("100% individual tests passed", output)

    def test_unittest_expected_failures_and_unexpected_successes(self):
        self.add_suite("expected", "Ran 5 tests in 0.1s\nOK (skipped=1, expected failures=1)")
        self.add_suite("unexpected", "Ran 3 tests in 0.1s\nFAILED (unexpected successes=1)", 1)
        output = self.run_ctest(status=8)
        self.assert_counts(output, 8, 5, 1, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
