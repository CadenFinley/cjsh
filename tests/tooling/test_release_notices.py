#!/usr/bin/env python3

# test_release_notices.py
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

import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE_SCRIPT = ROOT / ".github/scripts/create-release-archive.sh"


class ReleaseNoticeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cjsh notice tests ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES", "README.md"):
            shutil.copyfile(ROOT / name, self.directory / name)
        self.binary = self.directory / "cjsh"
        self.binary.write_text("#!/bin/sh\nprintf '%s\\n' v9.8.7\n", encoding="utf-8")
        self.binary.chmod(0o755)

    def run_archive(self):
        return subprocess.run(
            ["sh", str(ARCHIVE_SCRIPT), str(self.binary), "9.8.7", "test-target"],
            cwd=self.directory, text=True, capture_output=True, timeout=30,
        )

    def test_archive_contains_complete_notices_and_executable(self):
        result = self.run_archive()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        archive = self.directory / "dist/cjsh-v9.8.7-test-target.tar.gz"
        with tarfile.open(archive, "r:gz") as package:
            base = "cjsh-v9.8.7-test-target"
            for name in ("LICENSE", "THIRD_PARTY_NOTICES", "README.md"):
                self.assertEqual(package.extractfile(f"{base}/{name}").read(), (ROOT / name).read_bytes())
            self.assertEqual(package.getmember(f"{base}/cjsh").mode & 0o777, 0o755)
        notices = (ROOT / "THIRD_PARTY_NOTICES").read_text(encoding="utf-8")
        self.assertIn("Copyright (c) 2021 Daan Leijen", notices)
        self.assertIn("Markus Kuhn -- 2007-05-26", notices)

    def test_missing_notices_prevent_archive_creation(self):
        (self.directory / "THIRD_PARTY_NOTICES").unlink()
        result = self.run_archive()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list((self.directory / "dist").glob("*.tar.gz")))

    def test_cmake_installs_notices_with_prefix_datadir_and_destdir(self):
        build = self.directory / "build"
        # A component install verifies the real install rules without needing to
        # compile the shell. The default (all-components) install includes it too.
        for datadir in ("share", "custom-data"):
            with self.subTest(datadir=datadir):
                configure = subprocess.run(
                    ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                     "-DCJSH_BUILD_TESTS=OFF", "-DBUILD_TESTING=OFF",
                     f"-DCMAKE_INSTALL_DATADIR={datadir}"],
                    text=True, capture_output=True, timeout=120,
                )
                self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
                stage = self.directory / "stage"
                env = dict(os.environ, DESTDIR=str(stage))
                install = subprocess.run(
                    ["cmake", "--install", str(build), "--prefix", "/cjsh-test-prefix",
                     "--component", "licenses"],
                    env=env, text=True, capture_output=True, timeout=30,
                )
                self.assertEqual(install.returncode, 0, install.stdout + install.stderr)
                for name in ("LICENSE", "THIRD_PARTY_NOTICES"):
                    installed = stage / "cjsh-test-prefix" / datadir / "licenses/cjsh" / name
                    self.assertEqual(installed.read_bytes(), (ROOT / name).read_bytes())


if __name__ == "__main__":
    unittest.main()
