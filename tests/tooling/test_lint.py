#!/usr/bin/env python3

# test_lint.py
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

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("cjsh_lint", ROOT / "tools/lint.py")
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)


class LintTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cjsh lint tests ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / "build/release"
        self.build.mkdir(parents=True)
        self.cpp = self.make_file("cjsh-core/src/example.cpp", "int main() { return 0; }\n")
        self.header = self.make_file("cjsh-core/src/example.h", "#pragma once\n")
        self.c = self.make_file("cjsh-isocline/src/example.c", "int example(void) { return 0; }\n")
        self.c_header = self.make_file("cjsh-isocline/src/example.h", "#pragma once\n")
        self.entries = [self.entry(self.cpp), self.entry(self.c)]
        self.save_database()

    def make_file(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def entry(self, source):
        return {
            "directory": str(self.build),
            "file": str(source),
            "arguments": ["clang" if source.suffix == ".c" else "clang++",
                          "-DTEST=1", "-I", str(source.parent), "-o", "example.o", "-c", str(source)],
        }

    def save_database(self):
        (self.build / "compile_commands.json").write_text(json.dumps(self.entries), encoding="utf-8")

    def load(self):
        return lint.load_database(self.build, lint.discover_files(self.root))

    def run_main(self, *args):
        with patch.object(lint, "ROOT", self.root), patch.object(lint, "add_macos_sdk"), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return lint.main(["--build-dir", str(self.build), *args])

    def test_discovers_headers_sources_tests_but_not_build_files(self):
        test = self.make_file("tests/example.cpp", "")
        self.make_file("build/generated.cpp", "")
        self.make_file("cjsh-core/README.md", "")
        self.assertEqual(set(lint.discover_files(self.root)),
                         {self.cpp, self.header, self.c, self.c_header, test})

    def test_loads_quoted_commands_and_relative_files(self):
        self.entries[0]["command"] = shlex.join(self.entries[0].pop("arguments"))
        self.entries[0]["file"] = "../../cjsh-core/src/example.cpp"
        self.save_database()
        self.assertEqual(self.load()[self.cpp]["arguments"], self.entry(self.cpp)["arguments"])

    def test_missing_and_empty_database_fail_closed(self):
        (self.build / "compile_commands.json").unlink()
        with self.assertRaisesRegex(ValueError, "cmake --preset release"):
            self.load()
        self.entries = []
        self.save_database()
        with self.assertRaisesRegex(ValueError, "Empty or invalid"):
            self.load()

    def test_conflicting_commands_fail_closed(self):
        duplicate = self.entry(self.cpp)
        duplicate["arguments"].append("-DOTHER=1")
        self.entries.append(duplicate)
        self.save_database()
        with self.assertRaisesRegex(ValueError, "Multiple compile commands"):
            self.load()

    def test_unknown_uncompiled_source_fails_closed(self):
        self.make_file("tests/forgotten.cpp", "")
        with self.assertRaisesRegex(ValueError, "tests/forgotten.cpp"):
            lint.verify_source_coverage(self.root, lint.discover_files(self.root), self.load())

    def test_included_fragment_requires_compiled_owner_and_include(self):
        fragment = self.make_file("cjsh-isocline/src/edit/editline_menu.c", "")
        owner = self.make_file("cjsh-isocline/src/edit/editline.c", '#include "editline_menu.c"\n')
        self.entries.append(self.entry(owner))
        self.save_database()
        files = lint.discover_files(self.root)
        lint.verify_source_coverage(self.root, files, self.load())
        owner.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, fragment.name):
            lint.verify_source_coverage(self.root, files, self.load())
        owner.write_text('#include "editline_menu.c"\n', encoding="utf-8")
        self.entries.pop()
        self.save_database()
        with self.assertRaises(ValueError):
            lint.verify_source_coverage(self.root, files, self.load())

    def test_header_commands_preserve_flags_and_select_language(self):
        commands = self.load()
        original = json.dumps(list(commands.values()))
        for header, language in ((self.header, "c++-header"), (self.c_header, "c-header")):
            entry = lint.header_command(header, commands, self.root)
            self.assertEqual(entry["arguments"][-3:], ["-x", language, str(header)])
            self.assertIn("-DTEST=1", entry["arguments"])
            self.assertIn(str(header.parent), entry["arguments"])
            self.assertNotIn("-o", entry["arguments"])
            self.assertNotIn("-c", entry["arguments"])
        self.assertNotIn(str(self.cpp), lint.header_command(self.header, commands, self.root)["arguments"])
        self.assertEqual(original, json.dumps(list(commands.values())))

    def test_macos_uses_configured_sdk_and_adds_libcxx(self):
        sdk = self.root / "configured sdk"
        (sdk / "usr/include/c++/v1").mkdir(parents=True)
        commands = self.load()
        for entry in commands.values():
            entry["arguments"].extend(["-isysroot", str(sdk)])
        with patch.object(lint.platform, "system", return_value="Darwin"), \
                patch.object(lint.subprocess, "run", return_value=subprocess.CompletedProcess(
                    [], 0, stdout="/different/default/sdk\n")):
            lint.add_macos_sdk(commands)
        self.assertIn(str(sdk / "usr/include/c++/v1"), commands[self.cpp]["arguments"])
        self.assertNotIn("-isystem", commands[self.c]["arguments"])
        self.assertEqual(commands[self.cpp]["arguments"].count("-isysroot"), 1)

    def test_nonzero_tool_status_and_timeout_are_failures(self):
        success, output = lint.check_one([sys.executable, "-c", "print('problem'); raise SystemExit(1)"], 10)
        self.assertFalse(success)
        self.assertIn("problem", output)
        with patch.object(lint.subprocess, "run", side_effect=subprocess.TimeoutExpired("tool", 1)):
            self.assertFalse(lint.check_one(["tool"], 1)[0])

    def test_format_only_does_not_need_database_or_tidy(self):
        (self.build / "compile_commands.json").unlink()
        with patch.object(lint.shutil, "which", return_value="/fake/clang-format") as which, \
                patch.object(lint, "run_checks", return_value=True) as checks:
            self.assertEqual(self.run_main("--format-only"), 0)
        which.assert_called_once_with("clang-format")
        tasks = checks.call_args.args[1]
        self.assertEqual(len(tasks), 4)
        self.assertIn("--Werror", tasks[0][1])
        self.assertIn("--dry-run", tasks[0][1])

    def test_both_checks_run_and_any_failure_fails(self):
        with patch.object(lint.shutil, "which", return_value="/fake/tool"), \
                patch.object(lint, "run_checks", side_effect=[False, True]) as checks:
            self.assertEqual(self.run_main(), 1)
        self.assertEqual(checks.call_count, 2)

    def test_tidy_uses_temporary_database_and_warnings_as_errors(self):
        original = (self.build / "compile_commands.json").read_bytes()

        def inspect_tasks(label, tasks, jobs, timeout):
            self.assertEqual(label, "clang-tidy")
            self.assertEqual({path for path, _ in tasks}, {self.c, self.cpp, self.header, self.c_header})
            for _, command in tasks:
                self.assertIn("--warnings-as-errors=*", command)
                temporary_db = Path(command[command.index("-p") + 1]) / "compile_commands.json"
                self.assertTrue(temporary_db.is_file())
                self.assertNotEqual(temporary_db.parent, self.build)
                self.assertEqual(len(json.loads(temporary_db.read_text())), 4)
            return False

        with patch.object(lint.shutil, "which", return_value="/fake/tool"), \
                patch.object(lint, "run_checks", side_effect=inspect_tasks):
            self.assertEqual(self.run_main("--tidy-only"), 1)
        self.assertEqual((self.build / "compile_commands.json").read_bytes(), original)

    def test_missing_tool_is_configuration_error(self):
        with patch.object(lint.shutil, "which", return_value=None):
            self.assertEqual(self.run_main(), 2)

    def test_bad_database_is_configuration_error(self):
        (self.build / "compile_commands.json").write_text("not json", encoding="utf-8")
        with patch.object(lint.shutil, "which", return_value="/fake/tool"):
            self.assertEqual(self.run_main(), 2)

    def test_nonpositive_jobs_are_rejected(self):
        with self.assertRaises(SystemExit) as result:
            self.run_main("--jobs", "0")
        self.assertEqual(result.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
