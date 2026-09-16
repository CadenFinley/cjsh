#!/usr/bin/env python3

# test_job_notifications_interactive.py
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

"""PTY regressions for job notifications while the editor owns the terminal."""

from __future__ import annotations

import fcntl
import os
import re
import shlex
import signal
import struct
import sys
import tempfile
import termios

from test_idle_hook_interactive import IdleHookSession, normalize_terminal_output


class NotificationSession(IdleHookSession):
    def __init__(self, binary: str, home: str) -> None:
        super().__init__(binary, home, editor_args=["--no-syntax-highlighting"])
        self.children: set[int] = set()
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))

    def close(self) -> None:
        for pid in self.children:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        super().close()

    def launch(self, name: str, command: str = "sleep 60") -> int:
        start = self.run_command(
            f"{command} & jobname $! {shlex.quote(name)}".encode()
        )
        match = re.search(rb"\[\d+\] (\d+) ", self.output[start:])
        if match is None:
            raise AssertionError(f"missing background pid: {bytes(self.output[start:])!r}")
        pid = int(match[1])
        self.children.add(pid)
        return pid

    def notification(self, needle: bytes, start: int) -> bytes:
        index = self.wait_for(needle, start)
        self.wait_for_prompt(index)
        self.pump(0.05)
        output = bytes(self.output[start:])
        if output.count(needle) != 1:
            raise AssertionError(f"duplicate notification: {output!r}")
        # Prefix, input and persistent status rows must be erased, and the old
        # semantic input region closed before printing asynchronous output.
        before = output[:output.index(needle)]
        if before.count(b"\x1b[K") < 4 or b"\x1b]133;D\x1b\\" not in before:
            raise AssertionError(f"notification did not clear the editor: {output!r}")
        restored = output[output.index(needle) + len(needle):]
        for part in (b"NOTIFY-TOP", b"NOTIFY-MIDDLE", b"cjsh> ", b"NOTIFY-RIGHT"):
            if part not in restored:
                raise AssertionError(f"missing restored {part!r}: {output!r}")
        if b"\x1b[?2004l" in output:
            raise AssertionError(f"notification disabled bracketed paste: {output!r}")
        return output

    def cancel(self) -> None:
        start = len(self.output)
        self.write(b"\x03")
        self.wait_for_prompt(start)
        self.pump(0.05)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <cjsh>", file=sys.stderr)
        return 2
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="cjsh-job-notifications-") as home:
        session = NotificationSession(binary, home)
        try:
            session.wait_for_prompt(0)
            session.run_command(
                b"PS1='NOTIFY-TOP\\nNOTIFY-MIDDLE\\ncjsh> '; RPS1=NOTIFY-RIGHT; "
                b"unset RPROMPT PS1_FINAL RPS1_FINAL; "
                b"cjshopt status-hints persistent; cjshopt line-numbers off"
            )
            pid = session.launch("notify-[b]")
            session.write(b"echo AB\x1b[D")
            session.pump(0.1)
            start = len(session.output)
            os.kill(pid, signal.SIGSTOP)
            session.notification(b"Stopped\tnotify-[b]", start)
            start = len(session.output)
            session.write(b"X\r")
            session.wait_for_prompt(start, command_completed=True)
            if b"\nAXB\n" not in normalize_terminal_output(bytes(session.output[start:])):
                raise AssertionError("notification changed the pending input or cursor")

            start = len(session.output)
            os.kill(pid, signal.SIGKILL)
            session.notification(b"(SIGKILL)\tnotify-[b]", start)
            start = session.run_command(f"wait {pid}; echo WAIT:$?".encode())
            if f"\nWAIT:{128 + signal.SIGKILL}\n".encode() not in normalize_terminal_output(
                bytes(session.output[start:])
            ):
                raise AssertionError("asynchronous job cleanup lost the wait status")
            session.children.discard(pid)

            # A child event must not close or modify an active completion menu.
            pid = session.launch("completion-notify")
            start = len(session.output)
            session.write(b"jobs \t")
            session.wait_for(b"Completions", start)
            start = len(session.output)
            os.kill(pid, signal.SIGSTOP)
            session.pump(0.25)
            if b"Stopped" in session.output[start:]:
                raise AssertionError("notification interrupted the completion menu")
            session.write(b"\x1b")
            session.notification(b"Stopped\tcompletion-notify", start)
            session.cancel()

            # History search also owns a nested input loop.
            start = len(session.output)
            session.write(b"\x12")
            session.wait_for(b"history search:", start)
            start = len(session.output)
            os.kill(pid, signal.SIGKILL)
            session.pump(0.25)
            if b"SIGKILL" in session.output[start:]:
                raise AssertionError("notification interrupted history search")
            session.write(b"\x1b")
            session.notification(b"(SIGKILL)\tcompletion-notify", start)
            session.children.discard(pid)
            session.cancel()

            # Wake a timed hint wait promptly, preserving the unaccepted hint.
            session.run_command(b"cjshopt hint-delay 2000")
            pid = session.launch("hint-notify")
            session.write(b"ech")
            session.pump(0.1)
            start = len(session.output)
            os.kill(pid, signal.SIGSTOP)
            session.wait_for(b"Stopped\thint-notify", start, timeout_s=1.0)
            session.notification(b"Stopped\thint-notify", start)
            session.cancel()
            start = len(session.output)
            os.kill(pid, signal.SIGKILL)
            session.notification(b"(SIGKILL)\thint-notify", start)
            session.children.discard(pid)
            session.run_command(b"cjshopt hint-delay 0")

            # Complete and unsuccessful jobs must notify without another keypress.
            # A file gate makes completion deterministic after the prompt appears.
            for exit_code, state in ((0, b"Done"), (7, b"Exit 7")):
                gate = os.path.join(home, f"finish-{exit_code}")
                script = (
                    f"while [ ! -e {shlex.quote(gate)} ]; do sleep 0.02; done; "
                    f"exit {exit_code}"
                )
                pid = session.launch(f"exit-{exit_code}", f"/bin/sh -c {shlex.quote(script)}")
                start = len(session.output)
                with open(gate, "w", encoding="utf-8"):
                    pass
                session.notification(state + f"\texit-{exit_code}".encode(), start)
                start = session.run_command(f"wait {pid}; echo WAIT:$?".encode())
                if f"\nWAIT:{exit_code}\n".encode() not in normalize_terminal_output(
                    bytes(session.output[start:])
                ):
                    raise AssertionError(f"wait lost background exit status {exit_code}")
                session.children.discard(pid)

            # Notifications pending during a paste must not truncate its contents.
            pid = session.launch("paste-notify")
            start = len(session.output)
            session.write(b"\x1b[200~echo PASTE-")
            session.pump(0.05)
            os.kill(pid, signal.SIGSTOP)
            session.pump(0.1)
            if b"Stopped" in session.output[start:]:
                raise AssertionError("notification interrupted bracketed paste")
            session.write(b"PRESERVED\x1b[201~")
            session.notification(b"Stopped\tpaste-notify", start)
            start = len(session.output)
            session.write(b"\r")
            session.wait_for_prompt(start, command_completed=True)
            if b"\nPASTE-PRESERVED\n" not in normalize_terminal_output(
                bytes(session.output[start:])
            ):
                raise AssertionError("notification damaged bracketed paste")
        finally:
            session.close()
    print("Interactive job notification tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
