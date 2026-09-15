#!/usr/bin/env python3

# test_batch2_interactive.py
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

"""External terminal modes, nested suspension and session-private history drafts."""
import json
import os
from pathlib import Path
import shlex
import sys
import tempfile
import termios
import unittest

from test_idle_hook_interactive import IdleHookSession, PROMPT_INPUT_START


class InteractiveTests(unittest.TestCase):
    binary: str

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="cjsh-batch2-pty-")
        self.addCleanup(directory.cleanup)
        self.home = Path(directory.name)

    def session(self, history=False, *, terminal_size=None, syntax_highlighting=False):
        args = [self.binary, "--no-config", "--no-titleline", "--no-prompt-vars",
                "--no-completions"]
        if not syntax_highlighting:
            args.append("--no-syntax-highlighting")
        if not history:
            args.append("--no-history")
        s = IdleHookSession(self.binary, str(self.home), argv=args, terminal_size=terminal_size)
        self.addCleanup(s.close)
        s.wait_for_prompt(0)
        return s

    def test_command_path_hint_tracks_cursor(self):
        for name in ("pathprobe", "pathother"):
            executable = self.home / name
            executable.write_text("#!/bin/sh\nexit 0\n")
            executable.chmod(0o755)
        session = self.session(terminal_size=(24, 160))
        session.run_command(f"export PATH={shlex.quote(str(self.home))}:$PATH".encode())
        session.write(b"\x1b[200~pathprobe | pathother arg\x1b[201~")
        session.pump(0.2)

        start = len(session.output)
        session.write(b"\x01")  # Home: touch the first command without changing input.
        session.wait_for_normalized(f"({self.home}/pathprobe)".encode(), start)

        start = len(session.output)
        session.write(b"\x05")  # End: arguments should not show a command path.
        session.pump(0.2)
        # Cursor movement can redraw once with the old status before the status refresh.
        # Check the final frame, not the accumulated terminal output.
        final_frame = bytes(session.output[start:]).rsplit(PROMPT_INPUT_START, 1)[-1]
        self.assertNotIn(f"({self.home}/pathprobe)".encode(), final_frame)
        self.assertNotIn(f"({self.home}/pathother)".encode(), final_frame)

        start = len(session.output)
        session.write(b"\x1b[D" * 4)  # Immediately after the second command name.
        session.wait_for_normalized(f"({self.home}/pathother)".encode(), start)

    def test_shell_command_hints_use_completion_source_style(self):
        session = self.session(terminal_size=(24, 160), syntax_highlighting=True)
        session.run_command(b'cjshopt style_def builtin "ansi-red"')
        session.run_command(b"hintfunction() { :; }")
        session.run_command(b"alias hintalias='echo [value]'")
        session.run_command(b"abbr hintabbr='echo [value]'")
        for command, expected in (
            (b"echo", b"(builtin) - Write arguments to standard output"),
            (b"hintfunction", b"(function)"),
            (b"hintalias", b"(alias) - echo [value]"),
            (b"hintabbr", b"(abbreviation) - echo [value]"),
        ):
            start = len(session.output)
            session.write(b"\x1b[200~" + command + b"\x1b[201~")
            session.wait_for_normalized(expected, start)
            source = expected.split(b" - ", 1)[0]
            self.assertIn(b"\x1b[37m" + source, bytes(session.output[start:]))
            session.write(b"\x03")
            session.pump(0.1)

        # Command syntax colors do not override the completion-style source tag.
        session.run_command(b'cjshopt style_def builtin "ansi-blue"')
        start = len(session.output)
        session.write(b"echo")
        session.wait_for_normalized(b"(builtin) - Write arguments to standard output", start)
        self.assertIn(b"\x1b[37m(builtin)", bytes(session.output[start:]))

    def test_palette_tracks_binding_changes_between_prompts(self):
        # Keep the custom entry below the initial viewport so the search must find it.
        session = self.session(terminal_size=(24, 80))

        def search(expected):
            start = len(session.output)
            session.write(b"\x1bpzzpalettefixture")
            # Underlining the search match inserts ANSI codes inside the title.
            session.wait_for_normalized(expected, start)
            end = len(session.output)
            session.write(b"\x03")
            session.wait_for_prompt(end)

        for title in ("zzpalettefixture First", "zzpalettefixture Changed"):
            session.run_command(shlex.join([
                "cjshopt", "keybind", "ext", "set", "palette:fixture",
                "--title", title, ":",
            ]).encode())
            search(title.encode())
            # Unrelated commands preserve the installed palette.
            session.run_command(b":")
            search(title.encode())
        session.run_command(b"cjshopt keybind ext clear palette:fixture")
        search(b"No matches - showing all actions")

    def test_external_modes_persist_while_editor_remains_usable(self):
        session = self.session()
        probe = self.home / "term.py"
        result = self.home / "modes"
        probe.write_text('import termios,sys,json\na=termios.tcgetattr(0)\n'
                         'json.dump([*a[:4], [v if isinstance(v,int) else v[0] for v in a[6]]], open(sys.argv[1],"w"))\n')
        def modes():
            session.run_command(shlex.join([sys.executable, str(probe), str(result)]).encode())
            return json.loads(result.read_text())
        initial = modes()
        editor = termios.tcgetattr(session.fd)
        for command, field, flag in (("stty -isig", 3, termios.ISIG),
                                      ("stty -icrnl", 0, termios.ICRNL),
                                      ("stty -opost", 1, termios.OPOST)):
            session.run_command(command.encode())
            self.assertFalse(modes()[field] & flag)
            self.assertEqual(termios.tcgetattr(session.fd), editor)
        session.run_command(b"stty intr '^]' erase '^H' eof '^F'")
        observed = modes()
        self.assertEqual(observed[4][termios.VINTR], 29)
        self.assertEqual(observed[4][termios.VERASE], 8)
        self.assertEqual(observed[4][termios.VEOF], 6)
        session.run_command(b"sh -c 'stty raw -echo; kill -KILL $$'")
        self.assertEqual(modes(), observed)
        session.run_command(b"stty sane")
        self.assertTrue(modes()[3] & termios.ICANON)
        self.assertTrue(initial[3] & termios.ICANON)

    def test_nested_suspend_login_override_and_resume(self):
        for login in (False, True):
            with self.subTest(login=login):
                session = self.session()
                nested = [self.binary, "--no-config", "--no-titleline", "--no-prompt-vars", "--no-history"]
                if login:
                    nested += ["-l"]
                start = len(session.output)
                session.write(shlex.join(nested).encode() + b"\r")
                command_start = session.wait_for(b"\x1b]133;C", start)
                session.wait_for_prompt(command_start)
                child = os.tcgetpgrp(session.fd)
                self.assertNotEqual(child, session.pid)
                if login:
                    start = session.run_command(b"suspend")
                    self.assertIn(b"cannot suspend a login shell", session.output[start:])
                start = len(session.output)
                session.write(b"suspend -f\r" if login else b"suspend\r")
                session.wait_for_prompt(start, command_completed=True)
                self.assertEqual(os.tcgetpgrp(session.fd), session.pid)
                start = session.run_command(b"echo parent-usable")
                self.assertIn(b"parent-usable", session.output[start:])
                start = len(session.output)
                session.write(b"fg\r")
                session.wait_for_prompt(start, command_completed=True)
                self.assertEqual(os.tcgetpgrp(session.fd), child)
                session.run_command(b"echo child-usable")
                start = len(session.output)
                session.write(b"exit\r")
                session.wait_for_prompt(start, command_completed=True)
                self.assertEqual(os.tcgetpgrp(session.fd), session.pid)
                session.write(b"exit\r")
                self.assertEqual(session.wait_for_exit(), 0)

    def test_history_drafts_never_overwrite_other_sessions(self):
        first = self.session(history=True)
        second = self.session(history=True)
        path = self.home / ".cache/cjsh/history.txt"
        first.write(b"unfinished-secret-draft")
        first.pump(.1)
        self.assertNotIn("unfinished", path.read_text())
        second.run_command(b"echo second-committed")
        first.write(b"\x03")
        first.wait_for_prompt(len(first.output))
        first.run_command(b"echo first-committed")
        text = path.read_text()
        self.assertIn("echo second-committed", text)
        self.assertIn("echo first-committed", text)
        self.assertNotIn("unfinished", text)
        first.write(b"exit\r")
        second.write(b"exit\r")
        self.assertEqual(first.wait_for_exit(), 0)
        self.assertEqual(second.wait_for_exit(), 0)
        text = path.read_text()
        self.assertIn("echo second-committed", text)
        self.assertIn("echo first-committed", text)

    def test_no_config_keeps_history_enabled(self):
        session = self.session(history=True)
        session.run_command(b"echo persisted-without-config")
        path = self.home / ".cache/cjsh/history.txt"
        self.assertIn("echo persisted-without-config", path.read_text())
        session.run_command(b"cjshopt set-history-max 0")
        session.run_command(b"echo not-recorded")
        self.assertEqual(path.read_text(), "")
        session.run_command(b"cjshopt set-history-max 3")
        for number in range(5):
            session.run_command(f"echo retained-{number}".encode())
        commands = [line for line in path.read_text().splitlines() if not line.startswith("#")]
        self.assertEqual(commands, ["echo retained-2", "echo retained-3", "echo retained-4"])


if __name__ == "__main__":
    InteractiveTests.binary = str(Path(sys.argv[1]).resolve())
    unittest.main(argv=[sys.argv[0]], verbosity=2)
