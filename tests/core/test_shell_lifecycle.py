#!/usr/bin/env python3

# test_shell_lifecycle.py
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

"""Batch 1 observable startup, shutdown, exec, signal and terminal contracts."""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import time
import unittest

from test_idle_hook_interactive import IdleHookSession


class ShellLifecycleTests(unittest.TestCase):
    binary: str
    probe: str

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="cjsh-lifecycle-")
        self.addCleanup(directory.cleanup)
        self.home = Path(directory.name).resolve()
        self.env = os.environ.copy()
        self.env.update(HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / ".config"), SHLVL="4")
        self.env.pop("CJSH_ENV", None)
        self.env.pop("EXIT_CODE", None)

    def run_shell(self, command: str, *args: str, **kwargs) -> subprocess.CompletedProcess:
        return subprocess.run([self.binary, "--no-source", *args, "-c", command],
                              env=self.env, text=True, capture_output=True, timeout=5, **kwargs)

    def session(self, *args: str, source: bool = False) -> IdleHookSession:
        argv = [self.binary, "--no-titleline", "--no-prompt-vars", "--no-history",
                "--no-completions", "--no-syntax-highlighting", *args]
        if not source:
            argv.append("--no-source")
        session = IdleHookSession(self.binary, str(self.home), argv=argv)
        self.addCleanup(session.close)
        session.wait_for_prompt(0)
        return session

    def wait_file(self, path: Path, token: str | None = None) -> str:
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            text = path.read_text() if path.exists() else ""
            if text and (token is None or token in text):
                return text
            time.sleep(.01)
        self.fail(f"missing {token!r} in {path}")

    def track_job(self, path: Path) -> int:
        pid = int(self.wait_file(path).splitlines()[0])
        def cleanup():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.addCleanup(cleanup)
        return pid

    def probe_command(self, mode: str, path: Path) -> str:
        return shlex.join([self.probe, mode, str(path)])

    def install_hooks(self) -> None:
        (self.home / ".cjshrc").write_text(
            "cjshexit() { echo hook >> \"$HOME/hooks\"; echo hook-end >> \"$HOME/hooks\"; }\n"
            "trap 'echo trap >> \"$HOME/hooks\"; echo trap-end >> \"$HOME/hooks\"' EXIT\n")
        (self.home / ".cjlogout").write_text(
            'echo logout >> "$HOME/hooks"\necho logout-end >> "$HOME/hooks"\n')

    def assert_hooks(self) -> None:
        self.assertEqual((self.home / "hooks").read_text().splitlines(),
                         ["hook", "hook-end", "trap", "trap-end", "logout", "logout-end"])

    def test_startup_matrix(self) -> None:
        trace = self.home / "trace"
        record = 'printf "%s:%s:%s:%s:%s\\n" {stage} "$0" "$1" "$#" "$-" >> "$HOME/trace"\n'
        for filename, stage in ((".cjshenv", "env"), (".cjprofile", "profile"), (".cjshrc", "rc")):
            (self.home / filename).write_text(record.format(stage=stage))
        script = self.home / "body"
        script.write_text(record.format(stage="body"))
        for login in (False, True):
            for interactive in (False, True):
                for terminal in (False, True):
                    for source in ("script", "command", "stdin"):
                        with self.subTest(login=login, interactive=interactive, terminal=terminal, source=source):
                            trace.unlink(missing_ok=True)
                            args = [self.binary, "--no-titleline", "--no-history", "--no-prompt-vars"]
                            if login:
                                args.append("-l")
                            if interactive:
                                args.append("-i")
                            body = record.format(stage="body")
                            if source == "command":
                                args += ["-c", body, "chosen-zero", "first"]
                            elif source == "script":
                                args += [str(script), "first"]
                            expected_zero = "chosen-zero" if source == "command" else str(script) if source == "script" else self.binary
                            expected_interactive = interactive or (terminal and source == "stdin")
                            if terminal:
                                session = IdleHookSession(self.binary, str(self.home), argv=args)
                                try:
                                    if source == "stdin":
                                        session.wait_for_prompt(0)
                                        session.run_command(body.strip().encode())
                                        session.write(b"exit\r")
                                    self.assertEqual(session.wait_for_exit(), 0)
                                finally:
                                    session.close()
                            else:
                                result = subprocess.run(args, input=body if source == "stdin" else "",
                                                        capture_output=True, text=True, env=self.env, timeout=5)
                                self.assertEqual(result.returncode, 0, result.stderr)
                            rows = [line.split(":") for line in trace.read_text().splitlines()]
                            stages = ["env"] + (["profile"] if login else []) + (["rc"] if expected_interactive else []) + ["body"]
                            self.assertEqual([row[0] for row in rows], stages)
                            for row in rows:
                                self.assertEqual(row[1:4], [expected_zero, "" if source == "stdin" else "first", "0" if source == "stdin" else "1"])
                                self.assertEqual("i" in row[4], expected_interactive, row)

    def test_eof_and_exit_share_status_confirmation_and_hooks(self) -> None:
        for job in ("none", "running", "stopped"):
            for first, second, expected in ((b"\x04", b"\x04", 1),
                                             (b"exit 37\r", b"\x04", 37),
                                             (b"\x04", b"exit\r", 1)):
                with self.subTest(job=job, first=first, second=second):
                    (self.home / "hooks").unlink(missing_ok=True)
                    self.install_hooks()
                    session = self.session("-l", source=True)
                    if job != "none":
                        path = self.home / f"job-{session.pid}"
                        session.run_command((self.probe_command("stop" if job == "stopped" else "default", path) + " &").encode())
                        self.track_job(path)
                        if job == "stopped":
                            session.run_command(b"sleep .05; jobs")
                    session.run_command(b"false")
                    start = len(session.output)
                    session.write(first)
                    if job != "none":
                        # Typing exit can redraw the prompt before Return is
                        # processed. Wait for the warning before its new prompt.
                        warning = session.wait_for(f"There are {job} jobs.".encode(), start)
                        session.wait_for_prompt(warning)
                        self.assertFalse((self.home / "hooks").exists())
                        session.write(second)
                    self.assertEqual(session.wait_for_exit(), expected)
                    self.assert_hooks()

    def test_force_exit_keeps_hooks(self) -> None:
        self.install_hooks()
        session = self.session("-l", source=True)
        path = self.home / "force-job"
        session.run_command((self.probe_command("default", path) + " &").encode())
        self.track_job(path)
        session.write(b"exit --force 29\r")
        self.assertEqual(session.wait_for_exit(), 29)
        self.assert_hooks()

    def test_subshell_exit_after_external_commands(self) -> None:
        for status in (0, 7):
            for body in ("sleep 0", "printf payload | cat"):
                for command in (f"({body}; exit {status})",
                                f"f() ({body}; exit {status}); f",
                                f"f() {{ ({body}; exit {status}); }}; f"):
                    with self.subTest(command=command):
                        result = self.run_shell(command)
                        self.assertEqual(result.returncode, status, result.stderr)
                        self.assertEqual(result.stdout, "payload" if "printf" in body else "")
                        self.assertEqual(result.stderr, "")

    def test_subshell_exit_preserves_hooks_and_status(self) -> None:
        result = self.run_shell(
            "f() ("
            "cjshexit() { printf 'hook:%s\\n' \"$?\"; }; "
            "trap 'printf \"trap:%s\\n\" \"$?\"' EXIT; "
            "sleep 0; exit 7); f"
        )
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["hook:7", "trap:7"])
        self.assertEqual(result.stderr, "")

    def test_subshell_from_exit_hook_does_not_repeat_hooks(self) -> None:
        result = self.run_shell(
            "cjshexit() { (sleep 0; printf 'hook\\n'); }; "
            "(sleep 0; exit 7)"
        )
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["hook", "hook"])
        self.assertEqual(result.stderr, "")

    def test_hup_cleanup_respects_handlers_ignores_and_disown(self) -> None:
        for mode, protection in (("default", ""), ("exit", ""), ("handle", ""),
                                 ("ignore", ""), ("stop", ""), ("handle", "disown"),
                                 ("handle", "disown -h")):
            with self.subTest(mode=mode, protection=protection):
                path = self.home / f"job-{mode}-{protection}"
                command = (self.probe_command(mode, path) + " >/dev/null 2>&1 & "
                           f"while [ ! -s {shlex.quote(str(path))} ]; do sleep .01; done; sleep .05; "
                           + (protection + "; " if protection else "") + "exit --force")
                # Survivors can retain inherited descriptors. Wait for the shell
                # itself, with regular files for output instead of pipe EOF.
                try:
                    with (self.home / "cleanup-output").open("w") as output:
                        result = subprocess.run([self.binary, "--no-source", "-i", "-c", command],
                                                env=self.env, stdin=subprocess.DEVNULL,
                                                stdout=output, stderr=output, timeout=5)
                finally:
                    pid = self.track_job(path)
                self.assertEqual(result.returncode, 0)
                if mode in ("default", "exit"):
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                else:
                    os.kill(pid, 0)
                text = path.read_text()
                if mode in ("handle", "stop", "exit") and not protection:
                    self.assertEqual(text.count("HUP"), 1, text)
                else:
                    self.assertNotIn("HUP", text)
                if mode == "stop":
                    self.assertIn("CONT", text)

    def test_hup_optout_and_noninteractive_default(self) -> None:
        for interactive, policy, hup in ((True, "shopt -u huponexit; ", False),
                                         (False, "", False),
                                         (False, "shopt -s huponexit; ", True)):
            with self.subTest(interactive=interactive, policy=policy):
                path = self.home / f"optout-{interactive}-{hup}"
                command = (policy + self.probe_command("handle", path) + " >/dev/null 2>&1 & "
                           f"while [ ! -s {shlex.quote(str(path))} ]; do sleep .01; done; "
                           "sleep .05; exit --force")
                with (self.home / "output").open("w") as output:
                    result = subprocess.run([self.binary, "--no-config", *(["-i"] if interactive else []),
                                             "-c", command], env=self.env, stdin=subprocess.DEVNULL,
                                            stdout=output, stderr=output, timeout=5)
                pid = self.track_job(path)
                self.assertEqual(result.returncode, 0)
                os.kill(pid, 0)
                self.assertEqual("HUP" in path.read_text(), hup)

    def test_signal_traps_return_and_can_exit(self) -> None:
        for signum in (signal.SIGHUP, signal.SIGTERM):
            with self.subTest(signum=signum):
                session = self.session()
                path = self.home / f"trapped-job-{signum}"
                session.run_command((self.probe_command("handle", path) + " &").encode())
                pid = self.track_job(path)
                session.run_command(f"trap 'echo handled' {signum}".encode())
                start = len(session.output)
                os.kill(session.pid, signum)
                session.wait_for(b"handled\r\n", start)
                session.wait_for_prompt(start)
                os.kill(pid, 0)
                self.assertNotIn("HUP", path.read_text())
                session.run_command(b"echo still-usable")
                session.run_command(f"trap 'exit 43' {signum}".encode())
                os.kill(session.pid, signum)
                self.assertEqual(session.wait_for_exit(), 43)

    def test_terminating_signals_run_hooks_and_preserve_wait_status(self) -> None:
        for signum in (signal.SIGHUP, signal.SIGTERM):
            for foreground in (False, True):
                with self.subTest(signum=signum, foreground=foreground):
                    (self.home / "hooks").unlink(missing_ok=True)
                    self.install_hooks()
                    session = self.session("-l", source=True)
                    if foreground:
                        path = self.home / f"foreground-{session.pid}"
                        session.write(b"\x1b[200~" + self.probe_command("default", path).encode() + b"\x1b[201~\r")
                        deadline = time.monotonic() + 3
                        while not path.exists() and time.monotonic() < deadline:
                            session.pump()
                        self.track_job(path)
                    os.kill(session.pid, signum)
                    self.assertEqual(session.wait_for_exit(), -signum)
                    self.assert_hooks()
                    self.assertTrue(termios.tcgetattr(session.fd)[3] & termios.ICANON)

    def test_signal_during_shutdown_does_not_repeat_handlers(self) -> None:
        self.install_hooks()
        session = self.session("-l", source=True)
        session.run_command(b"cjshexit() { echo hook >> \"$HOME/hooks\"; kill -TERM $$; echo hook-end >> \"$HOME/hooks\"; }")
        session.write(b"exit 7\r")
        self.assertEqual(session.wait_for_exit(), -signal.SIGTERM)
        self.assert_hooks()

    def test_exec_dispositions_and_failed_exec_restoration(self) -> None:
        expected = "".join(f"{name}:default:0\n" for name in ("HUP", "QUIT", "TSTP", "TTIN", "TTOU", "PIPE"))
        for interactive in (False, True):
            for replacement in (False, True):
                for ignored_hup in (False, True):
                    with self.subTest(interactive=interactive, replacement=replacement, ignored_hup=ignored_hup):
                        path = self.home / "signals"
                        command = ("trap '' HUP; " if ignored_hup else "") + ("exec " if replacement else "") + self.probe_command("signals", path)
                        result = self.run_shell(command, *(["-i"] if interactive else []))
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(path.read_text(), expected.replace("HUP:default", "HUP:ignored") if ignored_hup else expected)
        session = self.session()
        session.run_command(b"trap 'echo restored' TERM; exec /cjsh-missing-executable")
        start = len(session.output)
        os.kill(session.pid, signal.SIGTERM)
        session.wait_for(b"restored\r\n", start)
        session.wait_for_prompt(start)
        session.write(b"exit\r")
        self.assertEqual(session.wait_for_exit(), 127)

    def test_inherited_ignored_hup_in_shell_and_children(self) -> None:
        path = self.home / "inherited"
        def ignore_hup():
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
        for interactive in (False, True):
            for prefix in ("", "exec "):
                result = self.run_shell("kill -HUP $$; " + prefix + self.probe_command("signals", path),
                                        *(["-i"] if interactive else []), preexec_fn=ignore_hup)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("HUP:ignored:0", path.read_text())

    def test_traps_during_foreground_wait(self) -> None:
        for signum in (signal.SIGHUP, signal.SIGTERM):
            for redirected in (False, True):
                for exiting in (False, True):
                    with self.subTest(signum=signum, redirected=redirected, exiting=exiting):
                        path = self.home / f"wait-{signum}-{redirected}-{exiting}"
                        handled = path.with_suffix(".handled")
                        handler = "exit 43" if exiting else "echo handled > " + shlex.quote(str(handled))
                        command = f"trap {shlex.quote(handler)} {signum}; " + self.probe_command("default", path)
                        if redirected:
                            command += " </dev/null"
                        command += "; echo continued"
                        with (self.home / "wait-output").open("w+") as output:
                            process = subprocess.Popen([self.binary, "--no-source", "-c", command],
                                                       env=self.env, stdin=subprocess.DEVNULL,
                                                       stdout=output, stderr=output)
                            try:
                                job_pid = self.track_job(path)
                                os.kill(process.pid, signum)
                                if exiting:
                                    self.assertEqual(process.wait(timeout=3), 43)
                                else:
                                    self.wait_file(handled)
                                    self.assertIsNone(process.poll())
                                    os.kill(job_pid, 0)
                                    os.kill(job_pid, signal.SIGTERM)
                                    self.assertEqual(process.wait(timeout=3), 0)
                                    output.seek(0)
                                    self.assertIn("continued", output.read())
                            finally:
                                if process.poll() is None:
                                    process.kill()
                                    process.wait()

    def test_trap_during_stdin_loading_keeps_shell_usable(self) -> None:
        for signum in (signal.SIGHUP, signal.SIGTERM):
            for interactive in (False, True):
                with self.subTest(signum=signum, interactive=interactive):
                    ready = self.home / "stdin-ready"
                    handled = self.home / "stdin-handled"
                    ready.unlink(missing_ok=True)
                    handled.unlink(missing_ok=True)
                    (self.home / ".cjshenv").write_text(
                        f"trap 'echo handled > {shlex.quote(str(handled))}' {signum}\n"
                        f"echo ready > {shlex.quote(str(ready))}\n")
                    args = [self.binary, *(["-i"] if interactive else [])]
                    process = subprocess.Popen(args, env=self.env, stdin=subprocess.PIPE,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        self.wait_file(ready)
                        os.kill(process.pid, signum)
                        self.wait_file(handled)
                        self.assertIsNone(process.poll())
                        stdout, stderr = process.communicate("echo usable\n", timeout=3)
                        self.assertEqual(process.returncode, 0, stderr)
                        self.assertEqual(stdout.strip(), "usable")
                    finally:
                        if process.poll() is None:
                            process.kill()
                        process.communicate()

    def test_terminal_closure_runs_hooks(self) -> None:
        self.install_hooks()
        session = self.session("-l", source=True)
        os.close(session.fd)
        session.fd = -1
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            waited, status = os.waitpid(session.pid, os.WNOHANG)
            if waited:
                session.pid = -1
                self.assertTrue(os.WIFSIGNALED(status))
                self.assertEqual(os.WTERMSIG(status), signal.SIGHUP)
                self.assert_hooks()
                return
            time.sleep(.01)
        self.fail("shell did not exit after controlling terminal closure")

    def test_failed_restart_restores_level_and_handlers(self) -> None:
        copy = self.home / "restartable"
        shutil.copy2(self.binary, copy)
        command = 'trap "echo restored" TERM; chmod -x "$0"; restart; echo "$SHLVL"; kill -TERM $$'
        result = subprocess.run([str(copy), "--no-source", "-c", command],
                                env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["5", "restored"])

    def test_logical_pwd_validation_and_traversal(self) -> None:
        target = self.home / "physical" / "child"
        target.mkdir(parents=True)
        link = self.home / "logical"
        link.symlink_to(target, target_is_directory=True)
        for inherited in (str(link), "/incorrect", "relative", "", None):
            for interactive in (False, True):
                with self.subTest(inherited=inherited, interactive=interactive):
                    if inherited is None:
                        self.env.pop("PWD", None)
                    else:
                        self.env["PWD"] = inherited
                    result = self.run_shell('printf "%s\\n" "$PWD"; pwd; cd ..; pwd',
                                            *(["-i"] if interactive else []), cwd=target)
                    logical = inherited == str(link)
                    expected = [str(link if logical else target)] * 2 + [str(self.home if logical else target.parent)]
                    self.assertEqual(result.stdout.splitlines(), expected)

    def test_shlvl_child_exec_failure_and_restart(self) -> None:
        child = shlex.join([self.binary, "--no-source", "-c", 'echo "$SHLVL"'])
        self.assertEqual(self.run_shell(child).stdout.strip(), "6")
        self.assertEqual(self.run_shell("exec " + child).stdout.strip(), "5")
        result = self.run_shell('exec /cjsh-missing-executable; echo "$SHLVL"')
        self.assertEqual(result.stdout.strip(), "5")
        result = self.run_shell('if [ -z "$RESTARTED" ]; then export RESTARTED=1; restart; fi; echo "$SHLVL"')
        self.assertEqual(result.stdout.strip(), "5")

    def test_exit_operands(self) -> None:
        for argument, expected in (("7", 7), ("-1", 255), ("+257", 1), ("2147483648", 0),
                                   ("bad", 2), ("1.2", 2), ("999999999999999999999999", 2)):
            for interactive in (False, True):
                with self.subTest(argument=argument, interactive=interactive):
                    result = self.run_shell("exit " + argument, *(["-i"] if interactive else []))
                    self.assertEqual(result.returncode, expected, result.stderr)
        result = self.run_shell('exit 7 8; printf "continued:%s\\n" "$?"')
        self.assertEqual(result.stdout.strip(), "continued:1")
        self.assertIn("too many arguments", result.stderr)
        session = self.session()
        session.run_command(b"exit 7 8")
        session.write(b"exit\r")
        self.assertEqual(session.wait_for_exit(), 1)

    def test_redirected_stdin_foreground_and_noninteractive_stty(self) -> None:
        session = self.session()
        path = self.home / "foreground"
        for redirections in ("</dev/null", "</dev/null >/dev/null 2>&1"):
            with self.subTest(redirections=redirections):
                path.unlink(missing_ok=True)
                inner = "exec " + shlex.join([self.binary, "--no-source", "-i", "-c", self.probe_command("foreground", path)]) + " " + redirections
                session.run_command(("sh -c " + shlex.quote(inner)).encode())
                self.assertEqual(path.read_text().strip(), "1")
                self.assertEqual(os.tcgetpgrp(session.fd), session.pid)
        # The supervisor observes the nested shell's terminal change before the
        # interactive parent/editor gets its opportunity to recover the terminal.
        path = self.home / "modes"
        inner = shlex.join([self.binary, "--no-source", "-c", "stty -echo; true"]) + "; stty -a >" + shlex.quote(str(path)) + "; stty echo"
        session.run_command(("sh -c " + shlex.quote(inner)).encode())
        self.assertRegex(path.read_text(), r"(?:^|[ ;\n])-echo(?:[ ;\n]|$)")


if __name__ == "__main__":
    ShellLifecycleTests.binary = os.path.abspath(sys.argv.pop(1))
    ShellLifecycleTests.probe = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
