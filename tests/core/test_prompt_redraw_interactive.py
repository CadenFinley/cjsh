#!/usr/bin/env python3

# test_prompt_redraw_interactive.py
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

import shlex
import sys
import tempfile
from pathlib import Path

from test_agent_mode_interactive import PROMPT_INPUT_START, Session


def assert_synchronized_redraw(output: bytes, text: bytes) -> None:
    repaint = output.find(text)
    begin = output.rfind(b"\x1b[?2026h", 0, repaint)
    clear = output.find(b"\r\x1b[K", begin, repaint)
    end = output.find(b"\x1b[?2026l", begin)
    if not 0 <= begin < clear < repaint < end:
        raise AssertionError(f"The terminal can show a blank frame before {text!r}: {output!r}")


def check_git_prompt(binary: str, root: Path, dirty: bool) -> None:
    repo = root / "repo"
    (repo / ".git").mkdir(parents=True)
    (repo / ".git" / "HEAD").write_text("ref: refs/heads/redraw-test\n")
    git = root / "git"
    release = root / "release-status"
    done = root / "status-done"
    git.write_text(
        "#!/bin/sh\n"
        'test "$3" = status || exit 1\n'
        "attempt=0\n"
        f"while [ ! -f {shlex.quote(str(release))} ] && [ \"$attempt\" -lt 300 ]; do\n"
        "  attempt=$((attempt + 1))\n"
        "  sleep 0.01\n"
        "done\n"
        + ("printf '?? probe\n'\n" if dirty else "")
        + f"touch {shlex.quote(str(done))}\n"
    )
    git.chmod(0o755)
    (root / ".cjshrc").write_text(
        f"export PATH={shlex.quote(str(root))}:$PATH\n"
        "PS1='GIT-HEADER\\ngit \\g> '\n"
    )
    session = Session(binary, str(root), cwd=str(repo), prompt_vars=True)
    try:
        session.wait_for(PROMPT_INPUT_START)
        session.wait_for_normalized(b"(redraw-test)")
        session.pump(0.1)
        start = len(session.output)
        release.touch()
        session.wait_for_file(str(done))
        if dirty:
            mark = "✗".encode()
            session.wait_for(mark, start=start)
            assert_synchronized_redraw(bytes(session.output[start:]), mark)
        else:
            session.pump(0.25)
            if b"GIT-HEADER" in session.output[start:] or b"\r\x1b[K" in session.output[start:]:
                raise AssertionError("A clean Git status cleared or repainted the prompt.")
    finally:
        session.close()


def check_menu_and_final_prompts(binary: str, root: Path) -> None:
    executor = root / "agent"
    executor.write_text(
        "#!/bin/sh\n"
        """printf '%s\\n' '[{"command":"echo suggestion","description":"An example"}]'\n"""
    )
    executor.chmod(0o755)
    (root / ".cjshrc").write_text(
        "PS1='PROMPT-HEADER\\nready> '\n"
        "PS1_FINAL='final> '\n"
        "RPS1_FINAL='FINAL-RIGHT'\n"
        f"cjshopt agent-mode set --trigger-prefix 'agent: ' --command {shlex.quote(str(executor))}\n"
    )
    session = Session(binary, str(root), cwd=str(root), prompt_vars=True)
    try:
        session.wait_for(PROMPT_INPUT_START)
        session.run_command(b"echo history entry")
        for key, menu in (
            (b"\x12", b"history search:"),
            (b"\x1bp", b"command palette:"),
            (b"\r", b"agent command:"),
        ):
            if menu == b"agent command:":
                session.paste(b"agent: write a command")
                session.wait_for_normalized(b"ready> agent: write a command")
            start = len(session.output)
            session.write(key)
            session.wait_for(menu, start=start)
            assert_synchronized_redraw(bytes(session.output[start:]), menu)
            start = len(session.output)
            session.write(b"\x1b")
            session.wait_for(b"PROMPT-HEADER", start=start)
            assert_synchronized_redraw(bytes(session.output[start:]), b"PROMPT-HEADER")
        session.write(b"\x15")
        session.paste(b"echo final-check")
        session.wait_for_normalized(b"ready> echo final-check")
        start = len(session.output)
        session.write(b"\r")
        session.wait_for_normalized(b"final> echo final-check", start=start)
        session.wait_for(b"FINAL-RIGHT", start=start)
        assert_synchronized_redraw(bytes(session.output[start:]), b"FINAL-RIGHT")
    finally:
        session.close()


def main() -> int:
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="cjsh-redraw-") as directory:
        root = Path(directory)
        check_git_prompt(binary, root / "clean", dirty=False)
        check_git_prompt(binary, root / "dirty", dirty=True)
        menu_root = root / "menu"
        menu_root.mkdir()
        check_menu_and_final_prompts(binary, menu_root)
    print("Prompt redraw tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
