#!/usr/bin/env python3

# test_read_file_position.py
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

"""Verify read leaves unread bytes for subsequent builtin and external readers."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ReadPositionTests(unittest.TestCase):
    binary: str

    def check_read(self, data: bytes, script: str, expected: bytes, pipe=False):
        with tempfile.TemporaryDirectory(prefix="cjsh-read-position-") as directory:
            root = Path(directory)
            (root / "input").write_bytes(data)
            env = dict(os.environ, HOME=directory, XDG_CACHE_HOME=directory,
                       XDG_CONFIG_HOME=directory, LC_ALL="C")
            if pipe:
                result = subprocess.run([self.binary, "--no-source", "-c", script],
                                        input=data, cwd=root, env=env, capture_output=True, timeout=10)
            else:
                with (root / "input").open("rb") as source:
                    result = subprocess.run([self.binary, "--no-source", "-c", script],
                                            stdin=source, cwd=root, env=env,
                                            capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, b"")
            self.assertEqual(result.stdout, expected)

    def test_external_reader_gets_remainder(self):
        for pipe in (False, True):
            with self.subTest(pipe=pipe):
                self.check_read(b"first\nsecond\nthird\n",
                                'read -r line; printf "<%s>\\n" "$line"; /bin/cat',
                                b"<first>\nsecond\nthird\n", pipe=pipe)

    def test_count_and_delimiter(self):
        self.check_read(b"abc:def:tail\n",
                        'read -n 2 a; read -d : b; read -d : c; '
                        'printf "<%s><%s><%s>\\n" "$a" "$b" "$c"; /bin/cat',
                        b"<ab><c><def>\ntail\n")

    def test_duplicated_descriptors_share_position(self):
        self.check_read(b"first\nsecond\nthird\n",
                        'exec 3<input; exec 4<&3; read -u 3 a; read -u 4 b; '
                        'printf "%s:%s\\n" "$a" "$b"; /bin/cat <&3',
                        b"first:second\nthird\n")

    def test_long_line_crosses_buffer_boundary(self):
        self.check_read(b"x" * 10000 + b"\nremaining\n",
                        'read -r line; printf "%s\\n" "${#line}"; /bin/cat',
                        b"10000\nremaining\n")

    def test_eof_after_partial_line(self):
        self.check_read(b"last", 'read a; read b; status=$?; '
                        'printf "%s:%s:%s\\n" "$a" "$b" "$status"', b"last::1\n")


if __name__ == "__main__":
    ReadPositionTests.binary = str(Path(sys.argv.pop(1)).resolve())
    unittest.main()
