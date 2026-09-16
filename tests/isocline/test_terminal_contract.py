#!/usr/bin/env python3

# test_terminal_contract.py
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

"""Regressions for terminal reply ownership and signal dispositions."""
import os
from pathlib import Path
import signal
import sys
import tempfile
import termios
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "core"))
from test_idle_hook_interactive import IdleHookSession, normalize_terminal_output


class TerminalContractTests(unittest.TestCase):
    binary: str

    def start(self, scenario: str, *args: str) -> IdleHookSession:
        directory = tempfile.TemporaryDirectory(prefix="isocline-terminal-contract-")
        self.addCleanup(directory.cleanup)
        query = scenario in ("query", "osc")
        session = IdleHookSession(self.binary, directory.name,
                                  argv=[self.binary, scenario, *args],
                                  cursor_response=None if query else b"\x1b[1;1R",
                                  terminal_size=(24, 100))
        self.addCleanup(session.close)
        session.wait_for(b"QUERY_READY" if query else b"SIGNAL_READY")
        return session

    def test_rejected_replies_preserve_every_byte(self) -> None:
        for data in (b"ordinary", b"\x00", b"\x1b", b"\x1b[", b"\x1b[12;",
                     b"\x1b[1;2A", b"\x1b[0;1R", b"\x1b[1;0R",
                     f"\x1b[{sys.maxsize + 1};1R".encode(),
                     f"\x1b[1;{sys.maxsize + 1}R".encode(),
                     b"\x1b[99999999999999999999999999999;1R",
                     b"\x1b[200~pasted\x1b[201~",
                     b"\x1b]0;title\x07", b"\x1b[" + b"1" * 260):
            with self.subTest(data=data):
                session = self.start("query", str(len(data)))
                session.wait_for(b"\x1b[6n")
                session.write(data)
                self.assertEqual(session.wait_for_exit(), 0)
                output = normalize_terminal_output(bytes(session.output))
                self.assertIn(b"QUERY:0:0:0\n", output)
                self.assertIn(data.hex().encode() + b"\nREPLAY_DONE", output)

    def test_matching_reply_consumes_only_the_reply(self) -> None:
        for row, column in ((12, 34), (sys.maxsize, sys.maxsize)):
            with self.subTest(row=row, column=column):
                session = self.start("query", "4")
                session.wait_for(b"\x1b[6n")
                session.write(f"\x1b[{row};{column}Rtail".encode())
                self.assertEqual(session.wait_for_exit(), 0)
                output = normalize_terminal_output(bytes(session.output))
                self.assertIn(f"QUERY:1:{row}:{column}\n".encode(), output)
                self.assertIn(b"7461696c\nREPLAY_DONE", output)

    def test_osc_requires_a_complete_matching_reply(self) -> None:
        payload = b"\x1b]4;0;rgb:ff/ff/ff"
        for data in (b"ordinary", payload, payload + b"\x1b",
                     payload + b"\x00typed\x07", b"\x1b]4;1;rgb:ff/ff/ff\x07"):
            with self.subTest(data=data):
                session = self.start("osc", str(len(data)))
                session.wait_for(b"\x1b]4;0;?\x07")
                session.write(data)
                self.assertEqual(session.wait_for_exit(), 0)
                output = normalize_terminal_output(bytes(session.output))
                self.assertIn(b"QUERY:0:0:0\n", output)
                self.assertIn(data.hex().encode() + b"\nREPLAY_DONE", output)
        for ending in (b"\x07", b"\x1b\\"):
            with self.subTest(ending=ending):
                session = self.start("osc", "4")
                session.wait_for(b"\x1b]4;0;?\x07")
                session.write(payload + ending + b"tail")
                self.assertEqual(session.wait_for_exit(), 0)
                output = normalize_terminal_output(bytes(session.output))
                self.assertIn(b"QUERY:1:0:0\n", output)
                self.assertIn(b"7461696c\nREPLAY_DONE", output)

    def test_default_signal_actions(self) -> None:
        for signum in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM):
            with self.subTest(signum=signum):
                session = self.start("default")
                os.kill(session.pid, signum)
                self.assertEqual(session.wait_for_exit(), -signum)
                self.assertTrue(termios.tcgetattr(session.fd)[3] & termios.ICANON)

    def test_ignored_and_custom_signals_resume_raw_input(self) -> None:
        for scenario in ("ignore", "custom", "siginfo"):
            for signum in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM):
                with self.subTest(scenario=scenario, signum=signum):
                    session = self.start(scenario)
                    original = termios.tcgetattr(session.fd)
                    os.kill(session.pid, signum)
                    if scenario == "ignore":
                        session.pump(0.1)
                    else:
                        session.wait_for(b"HANDLED\r\n")
                    self.assertEqual(termios.tcgetattr(session.fd), original)
                    session.write(b"\r")
                    self.assertEqual(session.wait_for_exit(), 0)
                    output = normalize_terminal_output(bytes(session.output))
                    expected = b"HANDLER:0:0" if scenario == "ignore" else f"HANDLER:{signum}:1".encode()
                    self.assertIn(expected, output)
                    self.assertIn(b"RESTORED:1", output)

    def test_default_disposition_restored_on_teardown(self) -> None:
        session = self.start("default")
        session.write(b"\r")
        self.assertEqual(session.wait_for_exit(), 0)
        self.assertIn(b"RESTORED:1", session.output)

    def test_reset_hand_is_honored(self) -> None:
        session = self.start("reset")
        if b"UNREPORTED_RESETHAND" in session.output:
            session.write(b"\r")
            self.assertEqual(session.wait_for_exit(), 77)
            self.skipTest("sigaction does not report SA_RESETHAND on this platform")
        os.kill(session.pid, signal.SIGINT)
        session.wait_for(b"HANDLED\r\n")
        os.kill(session.pid, signal.SIGINT)
        self.assertEqual(session.wait_for_exit(), -signal.SIGINT)


if __name__ == "__main__":
    TerminalContractTests.binary = str(Path(sys.argv[1]).resolve())
    unittest.main(argv=[sys.argv[0]], verbosity=2)
