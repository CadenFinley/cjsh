#!/usr/bin/env python3

# test_history_expansion_interactive.py
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

"""Exercise history expansion through the editor, persistence, parser and executor."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

from test_idle_hook_interactive import (
    COMMAND_OUTPUT_END,
    IdleHookSession,
    normalize_terminal_output,
)


COMMAND_OUTPUT_START = b"\x1b]133;C\x1b\\"


class HistoryExpansionTests(unittest.TestCase):
    binary: str

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="cjsh-history-expansion-")
        self.addCleanup(directory.cleanup)
        self.home = Path(directory.name)
        self.history = self.home / ".cache/cjsh/history.txt"

    def session(self, *extra_args):
        session = IdleHookSession(
            self.binary, str(self.home),
            editor_args=["--no-prompt-vars", "--no-completions",
                         "--no-syntax-highlighting", *extra_args],
            terminal_size=(40, 180),
        )
        self.addCleanup(session.close)
        session.wait_for_prompt(0)
        return session

    def command(self, session, command, status=0):
        start = session.run_command(command.encode())
        segment = bytes(session.output[start:])
        self.assertIn(COMMAND_OUTPUT_START, segment)
        # Only inspect execution output: editor redraws also contain the input.
        output = segment.split(COMMAND_OUTPUT_START, 1)[1]
        output, end = output.split(COMMAND_OUTPUT_END, 1)
        self.assertTrue(end.startswith(str(status).encode() + b"\x1b\\"), segment)
        return normalize_terminal_output(output).decode()

    def expansion(self, session, expression, expanded, output):
        self.assertEqual(self.command(session, expression), expanded + "\n" + output)

    def records(self):
        if not self.history.exists():
            return []
        return [line for line in self.history.read_text().splitlines()
                if line and not line.startswith("#")]

    def test_empty_history_reports_error_without_recording_input(self):
        session = self.session()
        for expression in ("!!", "!-1", "!0", "!echo", "!?echo?", "echo !$", "^old^new^"):
            with self.subTest(expression=expression):
                output = self.command(session, expression, status=1)
                self.assertIn("event not found", output)
                self.assertEqual(self.records(), [])
        self.command(session, "echo FIRST")
        self.expansion(session, "!!", "echo FIRST", "FIRST\n")

    def test_single_previous_command_is_available(self):
        session = self.session()
        self.command(session, "echo ONLY")
        self.expansion(session, "!!", "echo ONLY", "ONLY\n")
        self.assertEqual(self.records(), ["echo ONLY"])

    def test_echo_double_bang_uses_latest_command(self):
        session = self.session()
        self.command(session, "echo OLDER")
        self.command(session, "echo LATEST")
        self.expansion(session, "echo !!", "echo echo LATEST", "echo LATEST\n")
        self.assertEqual(self.records()[-1], "echo echo LATEST")
        self.expansion(session, "echo !!", "echo echo echo LATEST", "echo echo LATEST\n")

    def test_repeated_and_deduplicated_commands_keep_recency(self):
        session = self.session()
        self.command(session, "echo FIRST")
        self.command(session, "echo SECOND")
        self.command(session, "echo FIRST")
        for _ in range(3):
            self.expansion(session, "!!", "echo FIRST", "FIRST\n")
        self.assertEqual(self.records(), ["echo SECOND", "echo FIRST"])
        self.assertIn("frequency=5", self.history.read_text().splitlines()[-2])

    def test_relative_and_absolute_events_include_newest_entry(self):
        session = self.session()
        for expression, expected in (("!-1", "NEW"), ("!-2", "OLD"),
                                     ("!0", "OLD"), ("!1", "NEW")):
            with self.subTest(expression=expression):
                self.command(session, "echo OLD")
                self.command(session, "echo NEW")
                self.expansion(session, expression, "echo " + expected, expected + "\n")

    def test_search_selects_most_recent_matching_command(self):
        session = self.session()
        for expression in ("!echo", "!?MATCH?"):
            with self.subTest(expression=expression):
                self.command(session, "echo MATCH_OLD")
                self.command(session, "echo MATCH_NEW")
                self.expansion(session, expression, "echo MATCH_NEW", "MATCH_NEW\n")

    def test_word_designators_use_latest_command(self):
        session = self.session()
        for expression, expected in (
            ("!$", "beta"), ("!^", "alpha"), ("!*", "alpha beta"),
            ("!!:$", "beta"), ("!!:^", "alpha"), ("!!:*", "alpha beta"),
            ("!:0", "echo"), ("!:1-2", "alpha beta"), ("!-1:$", "beta"),
            ("!echo:^", "alpha"), ("!?alpha?:*", "alpha beta"),
        ):
            with self.subTest(expression=expression):
                self.command(session, "echo OLD_STALE")
                self.command(session, "echo alpha beta")
                self.expansion(session, "echo " + expression, "echo " + expected,
                               expected + "\n")

    def test_quick_substitution_uses_latest_command(self):
        session = self.session()
        self.command(session, "echo OLD_TOKEN")
        self.command(session, "echo NEW_TOKEN")
        self.expansion(session, "^NEW^CHANGED^ suffix",
                       "echo CHANGED_TOKEN suffix", "CHANGED_TOKEN suffix\n")
        self.assertEqual(self.records()[-1], "echo CHANGED_TOKEN suffix")

    def test_expansion_errors_preserve_last_successful_history_entry(self):
        session = self.session()
        self.command(session, "echo RETAINED")
        before = self.history.read_bytes()
        for expression in ("!missing_history_event", "!-99", "!!:99",
                           "^missing^replacement^", "^^"):
            with self.subTest(expression=expression):
                self.assertIn("history-expansion", self.command(session, expression, status=1))
                self.assertEqual(self.history.read_bytes(), before)
        self.expansion(session, "!!", "echo RETAINED", "RETAINED\n")

    def test_failed_commands_are_still_the_previous_event(self):
        session = self.session()
        self.command(session, "echo OLDER")
        self.assertEqual(self.command(session, "false", status=1), "")
        self.assertEqual(self.command(session, "!!", status=1), "false\n")
        self.assertEqual(self.records()[-1], "false")

    def test_failed_multiline_paste_is_not_skipped(self):
        session = self.session()
        self.command(session, "echo OLDER")
        missing = shlex.quote(str(self.home / "missing-command"))
        pasted = f"  {missing} FIRST_LINE\n\n   {missing} SECOND_LINE"
        original = self.command(session, pasted, status=127)
        self.assertIn("missing-command", original)
        saved = self.records()[-1]
        replay = self.command(session, "!!", status=127)
        self.assertEqual(replay, pasted + "\n" + original)
        self.assertEqual(self.records()[-1], saved)
        echoed = self.command(session, "echo !!", status=127)
        self.assertTrue(echoed.startswith("echo " + pasted + "\n"), echoed)
        self.assertNotIn("OLDER", echoed)
        self.assertEqual(self.records()[-1], "echo " + saved)

    def test_multiline_paste_preserves_blank_lines_and_indentation(self):
        session = self.session()
        pasted = "  echo FIRST_LINE\n\n    echo SECOND_LINE"
        output = "FIRST_LINE\nSECOND_LINE\n"
        self.assertEqual(self.command(session, pasted), output)
        saved = self.records()[-1]
        for _ in range(2):
            self.expansion(session, "!!", pasted, output)
            self.assertEqual(self.records(), [saved])

    def test_heredoc_replays_as_one_history_event(self):
        session = self.session()
        command = "cat <<'HISTORY_END'\nfirst line\n\nlast line\nHISTORY_END"
        output = "first line\n\nlast line\n"
        self.assertEqual(self.command(session, command), output)
        self.expansion(session, "!!", command, output)
        self.assertEqual(len(self.records()), 1)

    def test_backslashes_quotes_and_unicode_survive_replay(self):
        session = self.session()
        command = "printf '%s\\n' 'literal\\n' 'C:\\tmp' '#tag' 'café' '!!'"
        output = "literal\\n\nC:\\tmp\n#tag\ncafé\n!!\n"
        self.assertEqual(self.command(session, command), output)
        saved = self.records()[-1]
        self.expansion(session, "!!", command, output)
        self.assertEqual(self.records(), [saved])

    def test_persisted_tabs_are_decoded_before_execution(self):
        # The paste editor normalizes tabs; seed a stored entry to exercise the reader.
        self.history.parent.mkdir(parents=True)
        self.history.write_text("echo\\tTAB_ARGUMENT\n")
        session = self.session()
        self.expansion(session, "!!", "echo\tTAB_ARGUMENT", "TAB_ARGUMENT\n")
        self.assertEqual(self.records()[-1], "echo\\tTAB_ARGUMENT")

    def test_canceled_input_does_not_replace_previous_command(self):
        session = self.session()
        self.command(session, "echo RETAINED")
        before = self.history.read_bytes()
        start = len(session.output)
        session.write(b"\x1b[200~echo CANCELED\x1b[201~")
        session.wait_for_normalized(b"echo CANCELED", start)
        start = len(session.output)
        session.write(b"\x03")
        session.wait_for_prompt(start)
        self.assertEqual(self.history.read_bytes(), before)
        self.expansion(session, "!!", "echo RETAINED", "RETAINED\n")

    def test_reopened_session_decodes_persisted_multiline_history(self):
        first = self.session()
        command = "echo PERSISTED_FIRST\necho PERSISTED_SECOND"
        self.command(first, command)
        # Start another shell using the completed record, without adding an exit command.
        second = self.session()
        self.expansion(second, "!!", command, "PERSISTED_FIRST\nPERSISTED_SECOND\n")

    def test_other_sessions_committed_history_is_available(self):
        first = self.session()
        second = self.session()
        self.command(first, "echo FIRST_SESSION")
        self.command(second, "echo SECOND_SESSION")
        self.expansion(first, "!!", "echo SECOND_SESSION", "SECOND_SESSION\n")

    def test_directory_scoping_does_not_filter_expansion(self):
        first = self.session()
        second = self.session()
        other = self.home / "other"
        other.mkdir()
        self.command(first, "cjshopt history-directory on")
        self.command(second, "cd " + shlex.quote(str(other)))
        self.command(second, "echo OTHER_DIRECTORY")
        self.expansion(first, "!!", "echo OTHER_DIRECTORY", "OTHER_DIRECTORY\n")

    def test_expansion_can_be_disabled(self):
        session = self.session("--no-history-expansion")
        self.command(session, "echo PREVIOUS")
        self.assertEqual(self.command(session, "echo !!"), "!!\n")
        self.assertEqual(self.records()[-1], "echo !!")

    def test_noninteractive_input_does_not_expand_history(self):
        env = os.environ.copy()
        env["HOME"] = str(self.home)
        env["XDG_CONFIG_HOME"] = str(self.home / ".config")
        result = subprocess.run(
            [self.binary, "--no-source"], input="echo FIRST\necho !!\n",
            text=True, capture_output=True, env=env, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "FIRST\n!!\n")
        self.assertEqual(self.records(), [])

    def test_history_and_fc_decode_the_same_persisted_records(self):
        session = self.session()
        command = "echo FIRST_LINE\necho SECOND_LINE"
        self.command(session, command)
        self.assertEqual(self.command(session, "history 1"), "    0  " + command + "\n")
        self.command(session, command)
        self.assertEqual(self.command(session, "fc -s"),
                         command + "\nFIRST_LINE\nSECOND_LINE\n")


if __name__ == "__main__":
    HistoryExpansionTests.binary = str(Path(sys.argv.pop(1)).resolve())
    unittest.main(verbosity=2)
