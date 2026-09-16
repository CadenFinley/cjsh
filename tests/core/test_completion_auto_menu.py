#!/usr/bin/env python3

# test_completion_auto_menu.py
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

"""Ensure passive completion menus do not run expensive explicit completion work."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import sys
import tempfile

from test_idle_hook_interactive import IdleHookSession, normalize_terminal_output


def executable(path: Path, script: str = "#!/bin/sh\nexit 0\n") -> None:
    path.write_text(script, encoding="utf-8")
    path.chmod(0o755)


def cancel(session: IdleHookSession) -> None:
    start = len(session.output)
    session.write(b"\x03")
    session.wait_for_prompt(start)
    session.pump()


def type_text(session: IdleHookSession, text: str) -> int:
    start = len(session.output)
    for end, char in enumerate(text, 1):
        start = len(session.output)
        session.write(char.encode())
        session.wait_for_normalized(b"cjsh> " + text[:end].encode(), start)
    return start


def main(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cjsh-auto-menu-") as temp:
        root = Path(temp)
        home = root / "home"
        commands = root / "bin"
        home.mkdir()
        commands.mkdir()
        for number in range(1, 5):
            executable(commands / f"automenu-tool{number}")
        calls = root / "man-calls"
        man = root / "fake-man"
        executable(
            man,
            "#!/bin/sh\n"
            f"printf 'called\\n' >> {shlex.quote(str(calls))}\n"
            "printf 'NAME\\n    automenu-tool1 - fixture command\\n"
            "OPTIONS\\n    --sample    Sample option\\n'\n",
        )
        session = IdleHookSession(
            os.path.abspath(binary), str(home),
            editor_args=["--no-syntax-highlighting", "--no-history"],
            terminal_size=(24, 120),
        )
        try:
            session.wait_for_prompt(0)
            session.run_command(
                (
                    f"export PATH={shlex.quote(str(commands))} "
                    f"CJSH_MAN_PATH={shlex.quote(str(man))}; "
                    f"cd {shlex.quote(str(home))}; "
                    "PS1='cjsh> '; unset RPS1 RPROMPT PS1_FINAL RPS1_FINAL; "
                    "cjshopt status-line off; cjshopt status-hints off; "
                    "cjshopt completion-learning on; cjshopt completion-auto-menu on"
                ).encode()
            )
            session.pump()

            # First letters must not fetch documentation. The automatic list must
            # retain its menu-sized result budget rather than the two-hint limit.
            start = type_text(session, "automenu-")
            session.wait_for(b"tab:activate", start)
            for number in range(1, 5):
                session.wait_for(f"automenu-tool{number}".encode(), start)
            if calls.exists():
                raise AssertionError("typing a command launched the manual-page reader")
            cancel(session)

            # With no cached option documentation, typing a new argument leaves
            # the menu hidden and must still avoid launching the manual reader.
            type_text(session, "automenu-tool1 ")
            for prefix, char in (("-", b"-"), ("--", b"-"), ("--s", b"s")):
                start = len(session.output)
                session.write(char)
                session.wait_for_normalized(
                    f"cjsh> automenu-tool1 {prefix}".encode(), start
                )
            output = normalize_terminal_output(bytes(session.output[start:]))
            if calls.exists():
                raise AssertionError("typing an uncached argument launched the manual-page reader")
            if b"Completions" in output:
                raise AssertionError(f"uncached argument fixture unexpectedly has matches: {output!r}")

            start = len(session.output)
            session.write(b"\t")
            session.wait_for(b"--sample", start)
            if not calls.exists():
                raise AssertionError("Tab did not fetch missing completion documentation")
            fetched = calls.read_bytes()
            cancel(session)
            start = type_text(session, "automenu-tool1 --s")
            session.wait_for(b"--sample", start)
            session.wait_for(b"tab:activate", start)
            if calls.read_bytes() != fetched:
                raise AssertionError("passive completion fetched cached documentation again")
            cancel(session)

            # A command installed during editing must not trigger PATH rescans
            # on each key. An explicit Tab refresh must still discover it.
            type_text(session, "freshmenu-")
            executable(commands / "freshmenu-new")
            start = len(session.output)
            session.write(b"n")
            session.wait_for_normalized(b"cjsh> freshmenu-n", start)
            if b"freshmenu-new" in session.output[start:]:
                raise AssertionError("passive completion rebuilt the prompt's PATH snapshot")
            start = len(session.output)
            session.write(b"\t")
            session.wait_for(b"freshmenu-new", start)
            cancel(session)
        finally:
            session.close()
    print("All 4 automatic completion menu integration tests passed")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <cjsh>", file=sys.stderr)
        sys.exit(2)
    main(sys.argv[1])
