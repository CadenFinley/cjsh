#!/usr/bin/env python3

# test_directory_history.py
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
import shlex
import sys
import tempfile
from urllib.parse import unquote

from test_idle_hook_interactive import IdleHookSession, normalize_terminal_output


def records(path: Path) -> list[tuple[str, dict[str, str]]]:
    result = []
    metadata = {}
    for line in path.read_text().splitlines():
        if line.startswith("#"):
            metadata = dict((key, unquote(value)) for key, value in
                            (field.split("=", 1) for field in line[1:].split() if "=" in field))
        elif line:
            result.append((line, metadata))
            metadata = {}
    return result


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cjsh-directory-history-") as home:
        parent = Path(home, "project space%[x]")
        child = parent / "src" / "deep"
        sibling = Path(str(parent) + "-other")
        empty = Path(home, "empty")
        child.mkdir(parents=True)
        sibling.mkdir()
        empty.mkdir()
        alias = Path(home, "alias")
        alias.symlink_to(parent, target_is_directory=True)
        history = Path(home, ".cache", "cjsh", "history.txt")
        history.parent.mkdir(parents=True)
        history.write_text("echo DH_LEGACY\n")
        session = IdleHookSession(binary, home, editor_args=["--no-prompt-vars",
                                  "--no-completions", "--no-syntax-highlighting"],
                                  terminal_size=(30, 180))
        try:
            session.wait_for_prompt(0)
            def command(text: str) -> int:
                start = session.run_command(text.encode())
                assert records(history)[-1][0] == text, (text, records(history)[-1],
                    normalize_terminal_output(bytes(session.output[start:]))[-1500:])
                return start

            command("cd " + shlex.quote(str(parent)))
            command("echo DH_PARENT")
            command("echo DH_SHARED")
            command("echo DH_SHARED")
            command("cd src/deep")
            command("echo DH_CHILD")
            command("echo DH_SHARED")
            command("false")
            command("cd " + shlex.quote(str(sibling)))
            command("echo DH_SIBLING")
            command("cd " + shlex.quote(str(alias)))
            command("cjshopt history-directory enable")
            command("cjshopt history-directory-subdirs false")

            saved = records(history)
            shared_dirs = {meta.get("cwd"): meta["frequency"] for cmd, meta in saved
                           if cmd == "echo DH_SHARED"}
            assert shared_dirs == {str(parent.resolve()): "2", str(child.resolve()): "1"}, saved
            assert any(cmd == "cd src/deep" and meta.get("cwd") == str(parent.resolve())
                       for cmd, meta in saved), saved
            assert any(cmd == "false" and meta.get("cwd") == str(child.resolve()) and
                       meta.get("code") == "1" and "ms" in meta for cmd, meta in saved), saved

            # Prefix history navigation must skip the newer child and sibling commands.
            start = len(session.output)
            session.write(b"echo DH_\x1b[A\r")
            session.wait_for_prompt(start, command_completed=True)
            assert records(history)[-1][0] == "echo DH_SHARED"
            assert records(history)[-1][1]["cwd"] == str(parent.resolve())

            start = len(session.output)
            session.write(b"\x12DH_")
            session.wait_for_normalized(b"2 matches found", start)
            session.wait_for_normalized(b"scope directory - nested off", start)
            start = len(session.output)
            session.write(b"\x1bn")
            session.wait_for_normalized(b"4 matches found", start)
            session.wait_for_normalized(b"scope directory - nested on", start)
            start = len(session.output)
            session.write(b"\x1bd")
            session.wait_for_normalized(b"6 matches found", start)
            session.wait_for_normalized(b"scope all - nested on", start)
            start = len(session.output)
            session.write(b"\x07")
            session.wait_for_normalized(b"cjsh>", start)
            start = command("cjshopt history-directory status")
            session.wait_for_normalized(b"Directory-aware history is currently enabled.", start)
            start = command("cjshopt history-directory-subdirs status")
            session.wait_for_normalized(b"History nested directories is currently disabled.", start)

            # No-result fallback remains scoped, and an empty scope can still be toggled off.
            start = len(session.output)
            session.write(b"\x12DH_MISSING")
            session.wait_for_normalized(b"No matches - showing available history", start)
            segment = normalize_terminal_output(bytes(session.output[start:]))
            assert b"DH_SIBLING" not in segment and b"DH_LEGACY" not in segment, segment
            start = len(session.output)
            session.write(b"\x07")
            session.wait_for_normalized(b"cjsh>", start)
            command("cd " + shlex.quote(str(empty)))
            start = len(session.output)
            session.write(b"\x12DH_")
            session.wait_for_normalized(b"No matches found", start)
            start = len(session.output)
            session.write(b"\x1bd")
            session.wait_for_normalized(b"6 matches found", start)
            # Tab accepts a global result, while the configured directory setting survives.
            session.write(b"\t")
            session.pump(0.2)
            session.write(b"\x03")
            session.pump(0.2)
            start = command("cjshopt history-directory status")
            session.wait_for_normalized(b"Directory-aware history is currently enabled.", start)
            # Parent scope includes ancestors and remains temporary on cancel and acceptance.
            command("cd " + shlex.quote(str(child)))
            start = len(session.output)
            session.write(b"\x12DH_")
            session.wait_for_normalized(b"2 matches found", start)
            session.wait_for_normalized(b"nested off - parents off", start)
            start = len(session.output)
            session.write(b"\x1bp")
            session.wait_for_normalized(b"4 matches found", start)
            session.wait_for_normalized(b"nested off - parents on", start)
            start = len(session.output)
            session.write(b"\x07")
            session.wait_for_normalized(b"cjsh>", start)
            start = command("cjshopt history-directory-parents status")
            session.wait_for_normalized(b"History parent directories is currently disabled.", start)

            start = len(session.output)
            session.write(b"\x12DH_")
            session.wait_for_normalized(b"2 matches found", start)
            start = len(session.output)
            session.write(b"\x1bP")
            session.wait_for_normalized(b"4 matches found", start)
            session.write(b"\t")
            session.pump(0.2)
            session.write(b"\x03")
            session.pump(0.2)
            start = command("cjshopt history-directory-parents --status")
            session.wait_for_normalized(b"History parent directories is currently disabled.", start)

            command("cjshopt history-directory-parents enable")
            start = len(session.output)
            session.write(b"\x12DH_")
            session.wait_for_normalized(b"4 matches found", start)
            start = len(session.output)
            session.write(b"\x1bp")
            session.wait_for_normalized(b"2 matches found", start)
            start = len(session.output)
            session.write(b"\x07")
            session.wait_for_normalized(b"cjsh>", start)
            start = command("cjshopt history-directory-parents status")
            session.wait_for_normalized(b"History parent directories is currently enabled.", start)
            start = len(session.output)
            session.write(b"echo DH_P\x1b[A")
            session.wait_for_normalized(b"echo DH_PARENT", start)
            session.write(b"\x03")
            session.pump(0.2)
            command("cjshopt history-directory disable")
            command("cjshopt history-directory-subdirs true")
            start = command("cjshopt history-directory-subdirs --status")
            session.wait_for_normalized(b"History nested directories is currently enabled.", start)
            start = command("cjshopt history-directory invalid")
            session.wait_for_normalized(b"Unknown option", start)
            start = command("cjshopt history-directory-parents invalid")
            session.wait_for_normalized(b"Unknown option", start)
            session.write(b"exit\r")
            session.wait_for_exit()
        finally:
            session.close()

        # A fresh shell restores per-directory records and can load preferences from a file.
        rc = Path(home, "directory-settings.cjsh")
        rc.write_text("cjshopt history-directory on\ncjshopt history-directory-subdirs on\n"
                      "cjshopt history-directory-parents on\n")
        session = IdleHookSession(binary, home, editor_args=["--no-prompt-vars",
                                  "--no-completions", "--no-syntax-highlighting"],
                                  terminal_size=(30, 180))
        try:
            session.wait_for_prompt(0)
            session.run_command(("source " + shlex.quote(str(rc))).encode())
            session.run_command(("cd " + shlex.quote(str(child))).encode())
            start = len(session.output)
            session.write(b"\x12DH_")
            session.wait_for_normalized(b"4 matches found", start)
            session.wait_for_normalized(b"scope directory - nested on - parents on", start)
            start = len(session.output)
            session.write(b"\x07")
            session.wait_for_normalized(b"cjsh>", start)
            session.write(b"exit\r")
            session.wait_for_exit()
        finally:
            session.close()


if __name__ == "__main__":
    run(os.path.abspath(sys.argv[1]))
    print("PASS: directory history persistence, navigation, menu toggles, and settings")
