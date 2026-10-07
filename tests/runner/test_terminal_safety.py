#!/usr/bin/env python3

# test_terminal_safety.py
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

"""Check test execution from a terminal, including its foreground process group."""

from __future__ import annotations

import errno
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import termios
import time
import unittest


def run_in_terminal(command: list[str], timeout: float = 10.0) -> tuple[int, str]:
    # Keep a caller in the original process group. Running the test as the PTY's
    # session leader would hide attempts to take foreground control from CTest.
    ready_read, ready_write = os.pipe()
    pid, fd = pty.fork()
    if pid == 0:
        os.close(ready_write)
        os.read(ready_read, 1)
        os.close(ready_read)
        try:
            result = subprocess.run(command)
            code = result.returncode
            os._exit(code if code >= 0 else 128 - code)
        except BaseException:
            os._exit(125)

    os.close(ready_read)
    original_modes = termios.tcgetattr(fd)
    os.set_blocking(fd, False)
    os.write(ready_write, b"1")
    os.close(ready_write)
    output = bytearray()
    status = None
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            if select.select([fd], [], [], 0.02)[0]:
                try:
                    output.extend(os.read(fd, 65536))
                except OSError as exc:
                    if exc.errno not in (errno.EIO, errno.EAGAIN):
                        raise

            foreground = os.tcgetpgrp(fd)
            if foreground not in (0, pid):
                owner = subprocess.run(
                    ["ps", "-p", str(foreground), "-o", "pid,ppid,pgid,tty,command"],
                    capture_output=True, text=True,
                ).stdout
                raise AssertionError(
                    f"test stole terminal control: {foreground} != {pid}\n"
                    + owner
                    + output.decode(errors="replace")[-4000:]
                )
            child, state = os.waitpid(pid, os.WNOHANG | os.WUNTRACED)
            if child:
                if os.WIFSTOPPED(state):
                    raise AssertionError(f"caller was stopped by signal {os.WSTOPSIG(state)}")
                status = state
                break
        else:
            raise AssertionError(f"test timed out with a terminal attached: {command}")

        if termios.tcgetattr(fd) != original_modes:
            raise AssertionError("test changed the caller's terminal modes")
        while True:
            try:
                chunk = os.read(fd, 65536)
            except OSError as exc:
                if exc.errno in (errno.EIO, errno.EAGAIN):
                    break
                raise
            if not chunk:
                break
            output.extend(chunk)
        return os.waitstatus_to_exitcode(status), output.decode(errors="replace")
    finally:
        # Only groups belonging to this private terminal are touched. A broken
        # test may have moved itself into a new foreground process group.
        foreground = os.tcgetpgrp(fd)
        groups = {pid}
        if foreground > 0 and foreground != os.getpgrp():
            groups.add(foreground)
        for group in groups:
            try:
                os.killpg(group, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                # Cleanup must not mask the original terminal-safety failure.
                pass
        os.close(fd)
        if status is None:
            os.waitpid(pid, 0)


class TerminalSafetyTests(unittest.TestCase):
    runner: str
    function_tests: str
    cjsh: str

    def test_runner_disconnects_stdin_and_preserves_arguments(self) -> None:
        code, output = run_in_terminal(
            [
                self.runner, sys.executable, "-c",
                "import os,sys; assert not os.isatty(0); "
                "assert os.getsid(0) == os.getpid(); "
                "assert sys.stdin.read() == ''; print(repr(sys.argv[1]))",
                "spaces 'quotes' $literal; text",
            ]
        )
        self.assertEqual(code, 0, output)
        self.assertIn(repr("spaces 'quotes' $literal; text"), output)

    def test_runner_preserves_failure_status(self) -> None:
        code, output = run_in_terminal([self.runner, "sh", "-c", "exit 37"])
        self.assertEqual(code, 37, output)

    def test_runner_preserves_signal_status(self) -> None:
        code, output = run_in_terminal(
            [
                self.runner, sys.executable, "-c",
                "import os,signal; os.kill(os.getpid(), signal.SIGTERM)",
            ]
        )
        self.assertEqual(code, 128 + signal.SIGTERM, output)

    def test_function_syntax_does_not_claim_callers_terminal(self) -> None:
        code, output = run_in_terminal([self.function_tests])
        self.assertEqual(code, 0, output)
        self.assertRegex(output, r"All \d+ function syntax tests passed")

    def test_python_pty_child_has_its_own_session(self) -> None:
        session_module = Path(__file__).resolve().parents[1] / "core/test_agent_mode_interactive.py"
        probe = """
import importlib.util
import os
import sys
import tempfile
spec = importlib.util.spec_from_file_location("agent_tests", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix="cjsh-terminal-safety-") as home:
    session = module.Session(sys.argv[2], home)
    try:
        assert os.getsid(session.process.pid) == session.process.pid
    finally:
        session.close()
"""
        code, output = run_in_terminal(
            [sys.executable, "-c", probe, str(session_module), self.cjsh]
        )
        self.assertEqual(code, 0, output)

    def test_runner_cannot_reopen_callers_terminal(self) -> None:
        code, output = run_in_terminal([
            self.runner, sys.executable, "-c",
            "import os\n"
            "try:\n    os.open('/dev/tty', os.O_RDWR)\n"
            "except OSError:\n    pass\n"
            "else:\n    raise AssertionError('inherited controlling terminal')\n",
        ])
        self.assertEqual(code, 0, output)

    def test_runner_forwards_cancellation_to_worker(self) -> None:
        probe = """
import os
import signal
import subprocess
import sys
worker = subprocess.Popen(
    [sys.argv[1], sys.executable, '-c',
     'import os,time; print(os.getpid(), flush=True); time.sleep(30)'],
    stdout=subprocess.PIPE, text=True,
)
worker_pid = int(worker.stdout.readline())
try:
    worker.terminate()
    assert worker.wait(timeout=3) == 128 + signal.SIGTERM
    try:
        os.kill(worker_pid, 0)
    except ProcessLookupError:
        pass
    else:
        raise AssertionError('cancelled worker is still running')
finally:
    try:
        os.killpg(worker_pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if worker.poll() is None:
        worker.kill()
        worker.wait()
"""
        code, output = run_in_terminal([sys.executable, "-c", probe, self.runner])
        self.assertEqual(code, 0, output)


if __name__ == "__main__":
    TerminalSafetyTests.runner = str(Path(sys.argv[1]).resolve())
    TerminalSafetyTests.function_tests = str(Path(sys.argv[2]).resolve())
    TerminalSafetyTests.cjsh = str(Path(sys.argv[3]).resolve())
    unittest.main(argv=[sys.argv[0]], verbosity=2)
