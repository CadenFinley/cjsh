#!/usr/bin/env python3

# test_posix_interactive.py
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

"""Exercise POSIX option effects while a real terminal owns the shell."""

import os
from pathlib import Path
import re
import signal
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "core"))
from test_idle_hook_interactive import IdleHookSession


class PosixInteractiveTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="cjsh-posix-pty-")
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name)
        self.sh = self.home / "sh"
        self.sh.symlink_to(self.binary)

    def session(self, entry):
        argv = [self.binary, "--posix"] if entry == "flag" else [str(self.sh)]
        argv += ["--no-config", "--no-sh-warning", "--no-titleline", "-i"]
        session = IdleHookSession(argv[0], str(self.home), argv=argv)
        self.addCleanup(session.close)
        session.wait_for_prompt(0)
        return session

    def test_interactive_errors_preserve_shell(self):
        for entry in ("flag", "sh"):
            with self.subTest(entry=entry):
                session = self.session(entry)
                session.run_command(b'set -u; echo "$missing"')
                start = session.run_command(b"set +u; echo alive")
                session.wait_for_normalized(b"\nalive\n", start)
                session.run_command(b"set -o no_such_option")
                start = session.run_command(b"echo alive")
                session.wait_for_normalized(b"\nalive\n", start)

    def test_ignoreeof(self):
        for entry in ("flag", "sh"):
            with self.subTest(entry=entry):
                session = self.session(entry)
                session.run_command(b"set -o ignoreeof")
                start = len(session.output)
                session.write(b"\x04")
                session.wait_for(b"Use 'exit'", start)
                start = session.run_command(b"echo alive")
                session.wait_for_normalized(b"\nalive\n", start)
                session.run_command(b"set +o ignoreeof")
                session.write(b"\x04")
                self.assertEqual(session.wait_for_exit(), 0)

    def test_notify_timing_and_wait_status(self):
        for entry in ("flag", "sh"):
            for notify in (False, True):
                with self.subTest(entry=entry, notify=notify):
                    session = self.session(entry)
                    session.run_command(b"set -b" if notify else b"set +b")
                    start = session.run_command(b"sleep 60 &")
                    pid = int(re.search(rb"\[\d+\] (\d+)", session.output[start:]).group(1))
                    try:
                        session.write(b"echo ready")
                        session.pump(0.1)
                        start = len(session.output)
                        os.kill(pid, signal.SIGTERM)
                        session.pump(0.3)
                        if notify:
                            session.wait_for(b"SIGTERM", start)
                        else:
                            self.assertNotIn(b"SIGTERM", session.output[start:])
                        session.write(b"\r")
                        session.wait_for_prompt(start, command_completed=True)
                        session.wait_for(b"SIGTERM", start)
                        self.assertEqual(session.output[start:].count(b"SIGTERM"), 1)
                        start = session.run_command(f'wait {pid}; echo "status:$?"'.encode())
                        session.wait_for_normalized(f"\nstatus:{128 + signal.SIGTERM}\n".encode(), start)
                    finally:
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass


if __name__ == "__main__":
    PosixInteractiveTests.binary = str(Path(sys.argv[1]).resolve())
    unittest.main(argv=[sys.argv[0]], verbosity=2)
