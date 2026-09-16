#!/usr/bin/env python3

# test_batch2_startup.py
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

"""Batch 2 invocation, startup paths, environment and persistence contracts."""
from pathlib import Path
import os
import pwd
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from test_idle_hook_interactive import IdleHookSession


SKIP_PRELOAD_INJECTION = os.environ.get("CJSH_TEST_SKIP_PRELOAD_INJECTION") == "1"


class StartupTests(unittest.TestCase):
    binary: str
    injector: str

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="cjsh-batch2-")
        self.addCleanup(directory.cleanup)
        self.home = Path(directory.name).resolve()
        self.env = dict(os.environ, HOME=str(self.home), TERM="xterm")
        for name in ("CJSH_ENV", "CJSH_CONFIG_HOME", "CJSH_HISTORY_FILE", "ENV"):
            self.env.pop(name, None)

    def run_shell(self, *args, input=None):
        return subprocess.run([self.binary, *args], input=input, env=self.env,
                              text=True, capture_output=True, timeout=8)

    def run_with_inherited_fds(self, command, *args, argv0=None):
        # Install exact descriptor numbers in a separate process so the test runner's
        # descriptors cannot be overwritten. All four refer to the same writable file.
        launcher = (
            "import os, sys\n"
            "source = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)\n"
            "for fd in (3, 9, 19, 20):\n"
            "    os.dup2(source, fd)\n"
            "    os.set_inheritable(fd, True)\n"
            "if source not in (3, 9, 19, 20):\n"
            "    os.close(source)\n"
            "os.execv(sys.argv[2], sys.argv[3:])\n"
        )
        argv = [argv0 or self.binary, "--no-titleline", "--no-history", "--no-agent",
                *args, "-c", command]
        return subprocess.run(
            [sys.executable, "-c", launcher, str(self.home / "inherited-fds"),
             self.binary, *argv], input="", env=self.env, text=True,
            capture_output=True, timeout=8, start_new_session=True)

    def fd_probe_command(self):
        probe = (
            "import os\n"
            "opened = []\n"
            "for fd in (0, 1, 2, 3, 9, 19, 20):\n"
            "    try:\n"
            "        os.fstat(fd)\n"
            "        opened.append(str(fd))\n"
            "    except OSError:\n"
            "        pass\n"
            "print(' '.join(opened))\n"
        )
        return shlex.join([sys.executable, "-c", probe])

    def test_inherited_fds_only_cloexec_for_interactive_login(self):
        for args, argv0, sanitized in (([], None, False), (["-l"], None, False),
                                      (["-i"], None, False), (["-il"], None, True),
                                      (["--login", "--interactive"], None, True),
                                      ([], "-cjsh", False), (["-i"], "-cjsh", True)):
            for prefix in ("", "exec "):
                with self.subTest(args=args, argv0=argv0, prefix=prefix):
                    command = "echo builtin >&9; " + prefix + self.fd_probe_command()
                    result = self.run_with_inherited_fds(
                        command, "--no-config", *args, argv0=argv0)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stderr, "")
                    self.assertEqual(result.stdout, "0 1 2 20\n" if sanitized
                                     else "0 1 2 3 9 19 20\n")
                    self.assertEqual((self.home / "inherited-fds").read_text(), "builtin\n")

    def test_inherited_fds_sanitized_before_startup_files(self):
        probe = self.fd_probe_command()
        (self.home / ".cjshenv").write_text("echo env >&9; " + probe + "\n")
        (self.home / ".cjprofile").write_text(
            'exec 9>"$HOME/profile-fd"; ' + probe + "\n")
        (self.home / ".cjshrc").write_text(probe + "\n")
        result = self.run_with_inherited_fds(probe + "; echo body >&9", "-il")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, "0 1 2 20\n" + "0 1 2 9 20\n" * 3)
        self.assertEqual((self.home / "inherited-fds").read_text(), "env\n")
        self.assertEqual((self.home / "profile-fd").read_text(), "body\n")

    def test_inherited_cloexec_fds_explicit_redirections(self):
        probe = self.fd_probe_command()
        command = (f"{probe} 9>&20; {probe}; "
                   f'exec 9>"$HOME/reopened-fd"; {probe}; echo reopened >&9')
        result = self.run_with_inherited_fds(command, "--no-config", "-il")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, "0 1 2 9 20\n0 1 2 20\n0 1 2 9 20\n")
        self.assertEqual((self.home / "reopened-fd").read_text(), "reopened\n")

    def trace_files(self, root, prefix=""):
        root.mkdir(parents=True, exist_ok=True)
        for name, stage in ((".cjshenv", "env"), (".cjprofile", "profile"),
                            (".cjshrc", "rc"), (".cjlogout", "logout")):
            (root / name).write_text(f'echo {prefix}{stage} >> "$HOME/trace"\n')

    def read_trace(self):
        path = self.home / "trace"
        result = path.read_text().splitlines() if path.exists() else []
        path.unlink(missing_ok=True)
        return result

    def test_invocation_streams(self):
        for args in (("--unknown-batch2",), ("-Z",), ("-c",), ("--command",),
                     ("--config-dir",), ("--config-dir=",), ("--login-path",)):
            with self.subTest(args=args):
                r = self.run_shell(*args)
                self.assertEqual(r.returncode, 1)
                self.assertEqual(r.stdout, "")
                self.assertIn("Usage:", r.stderr)
        for arg in ("--help", "--version"):
            r = self.run_shell(arg)
            self.assertEqual(r.returncode, 0)
            self.assertTrue(r.stdout)
            self.assertEqual(r.stderr, "")
        help_text = self.run_shell("--help").stdout
        self.assertIn("--no-system-paths", help_text)
        self.assertNotIn("--login-path", help_text)

    def test_viewport_limit_commands(self):
        for command, label, default in (
            ("multiline-max-lines", "Multiline input", 15),
            ("completion-menu-max-lines", "Completion menu", 15),
            ("history-menu-max-lines", "History menu", 15),
            ("command-palette-max-lines", "Command palette", 15),
            ("custom-menu-max-lines", "Custom menu", 15),
        ):
            result = self.run_shell("-c", f"cjshopt {command} status")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, f"{label} currently shows up to {default} lines.\n")
            help_result = self.run_shell("-c", f"cjshopt {command} --help")
            self.assertEqual(help_result.returncode, 0, help_result.stderr)
            self.assertIn(f"{command} <count|status>", help_result.stdout)
            self.assertIn(f"  {command} ", self.run_shell("-c", "cjshopt --help").stdout)
            for requested, applied in ((1, 1), (8, 8), (75, 75), (300, 256)):
                with self.subTest(command=command, requested=requested):
                    result = self.run_shell("-c", f"cjshopt {command} {requested}; "
                                            f"cjshopt {command} --status")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stderr, "")
                    unit = "line" if applied == 1 else "lines"
                    self.assertEqual(result.stdout.splitlines()[-1],
                                     f"{label} currently shows up to {applied} {unit}.")
            for value in ("", "0", "-1", "+2", "1.5", "invalid", "8 extra",
                          "999999999999999999999999999"):
                with self.subTest(command=command, invalid=value):
                    result = self.run_shell("-c", f"cjshopt {command} {value}")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(command, result.stderr)

    def test_menu_limits_are_independent_and_quiet_in_rc(self):
        options = (
            ("completion-menu-max-lines", "Completion menu", 8),
            ("history-menu-max-lines", "History menu", 12),
            ("command-palette-max-lines", "Command palette", 16),
            ("custom-menu-max-lines", "Custom menu", 20),
        )
        defaults = dict(zip((command for command, _, _ in options), (15, 15, 15, 15)))
        statuses = "; ".join(f"cjshopt {command} status" for command, _, _ in options)
        for changed_command, _, limit in options:
            result = self.run_shell("-c", f"cjshopt {changed_command} {limit}; " + statuses)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines()[-4:], [
                f"{label} currently shows up to "
                f"{limit if command == changed_command else defaults[command]} lines."
                for command, label, _ in options
            ])

        (self.home / ".cjshrc").write_text("\n".join(
            f"cjshopt {command} {limit}\ncjshopt {command} status"
            for command, _, limit in options
        ) + "\n")
        result = self.run_shell("-i", "--no-titleline", "--no-history", "-c",
                                statuses + "; cjshopt multiline-max-lines status; "
                                "cjshopt multiline-bottom-lines status")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout.splitlines(), [
            f"{label} currently shows up to {limit} lines." for _, label, limit in options
        ] + ["Multiline input currently shows up to 15 lines.",
             "Multiline input currently uses a cursor margin of up to 3 content lines."])

    def test_shared_menu_limit_command_removed(self):
        for value in ("8", "status", "--help"):
            result = self.run_shell("-c", f"cjshopt menu-max-lines {value}")
            self.assertEqual(result.returncode, 1)
            self.assertIn("menu-max-lines", result.stderr)
        self.assertNotIn("  menu-max-lines ", self.run_shell("-c", "cjshopt --help").stdout)

    def test_line_wrap_marker_command(self):
        default_marker = "↵" if sys.platform == "darwin" else "←"
        result = self.run_shell("-c", "cjshopt line-wrap-marker status; "
                                "cjshopt line-wrap-marker ''; cjshopt line-wrap-marker status; "
                                "cjshopt line-wrap-marker '↪'; cjshopt line-wrap-marker --status")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual([line for line in result.stdout.splitlines() if "currently" in line], [
            f"Line wrap marker is currently '{default_marker}'.",
            "Line wrap marker is currently '' (disabled).",
            "Line wrap marker is currently '↪'.",
        ])
        for value in ("", "on", "off", "enable", "false", "ab", "'↪↪'", "'é'", "'>' extra",
                      shlex.quote("\n"), shlex.quote("\t"), shlex.quote("\x1b")):
            with self.subTest(invalid=value):
                result = self.run_shell("-c", "cjshopt line-wrap-marker '!'; "
                                        f"cjshopt line-wrap-marker {value}; result=$?; "
                                        "cjshopt line-wrap-marker status; exit $result")
                self.assertEqual(result.returncode, 1)
                self.assertIn("line-wrap-marker", result.stderr)
                self.assertEqual(result.stdout.splitlines()[-1],
                                 "Line wrap marker is currently '!'.")
        result = self.run_shell("-c", "cjshopt line-wrap-marker --help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("line-wrap-marker <marker|status>", result.stdout)

    def test_line_wrap_marker_characters_and_persistence(self):
        for marker in ("", ">", "<", "|", "&", ";", "(", ")", "é", "界", "😀", "'", '"',
                       "\\", "$", "`", "[", " ", "0", "1"):
            with self.subTest(marker=marker):
                result = self.run_shell("-c", "cjshopt line-wrap-marker " + shlex.quote(marker))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                # Reuse the printed persistence command, including shell metacharacter quoting.
                command = result.stdout.split("Add `", 1)[1].rsplit("` to your", 1)[0]
                (self.home / ".cjshrc").write_text(command + "\ncjshopt line-wrap-marker status\n")
                result = self.run_shell("-i", "--no-titleline", "--no-history", "-c",
                                        "cjshopt line-wrap-marker status")
                self.assertEqual(result.returncode, 0, result.stderr)
                quoted = "\"'\"" if marker == "'" else "'" + marker + "'"
                suffix = " (disabled).\n" if not marker else ".\n"
                self.assertEqual(result.stdout, "Line wrap marker is currently " + quoted + suffix)

    def test_line_wrap_marker_from_rc_is_quiet(self):
        (self.home / ".cjshrc").write_text(
            "cjshopt line-wrap-marker ''\ncjshopt line-wrap-marker status\n")
        result = self.run_shell("-i", "--no-titleline", "--no-history", "-c",
                                "cjshopt line-wrap-marker status")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "Line wrap marker is currently '' (disabled).\n")

    def test_noexec_sources(self):
        self.trace_files(self.home)
        marker = self.home / "executed"
        for flag in ("-n", "--no-exec"):
            for body, valid in ((f'touch {shlex.quote(str(marker))}\n', True),
                                ("if true; then\necho incomplete\n", False),
                                ("echo 'unterminated\n", False)):
                for source in ("command", "script", "stdin"):
                    with self.subTest(flag=flag, source=source, valid=valid):
                        args = [flag, "-l"]
                        if source == "command":
                            args += ["-c", body]
                        elif source == "script":
                            path = self.home / "script"
                            path.write_text(body)
                            args.append(str(path))
                        r = self.run_shell(*args, input=body if source == "stdin" else None)
                        self.assertEqual(r.returncode == 0, valid, r.stderr)
                        self.assertEqual(r.stdout, "")
                        self.assertFalse(marker.exists())
                        self.assertEqual(self.read_trace(), [])
        for flag in ("-m", "-s"):
            self.assertEqual(self.run_shell(flag, "-c", "echo executed").stdout, "executed\n")

    def test_native_configuration_precedence_and_bypass(self):
        alt = self.home / ".config/cjsh"
        override = self.home / "override root"
        cli = self.home / "cli root"
        self.trace_files(alt, "alt-")
        self.trace_files(override, "override-")
        self.trace_files(cli, "cli-")
        args = ("-il", "--no-titleline", "--no-history", "-c", 'echo body >> "$HOME/trace"')
        self.assertEqual(self.run_shell(*args).returncode, 0)
        self.assertEqual(self.read_trace(), ["alt-env", "alt-profile", "alt-rc", "body", "alt-logout"])
        self.trace_files(self.home)
        self.run_shell(*args)
        self.assertEqual(self.read_trace(), ["env", "profile", "rc", "body", "logout"])
        self.env["CJSH_CONFIG_HOME"] = str(override)
        self.run_shell(*args)
        self.assertEqual(self.read_trace(), ["override-env", "override-profile", "override-rc", "body", "override-logout"])
        self.run_shell("--config-dir", str(cli), *args)
        self.assertEqual(self.read_trace(), ["cli-env", "cli-profile", "cli-rc", "body", "cli-logout"])
        self.env["CJSH_ENV"] = str(self.home / ".cjshenv")
        self.run_shell(*args)
        self.assertEqual(self.read_trace(), ["env", "override-profile", "override-rc", "body", "override-logout"])
        self.env["CJSH_ENV"] = str(self.home / "missing")
        self.run_shell(*args)
        self.assertEqual(self.read_trace(), ["override-profile", "override-rc", "body", "override-logout"])
        for bypass in ("--no-config", "--secure"):
            self.run_shell(bypass, *args)
            self.assertEqual(self.read_trace(), ["body"])
        self.env.pop("CJSH_ENV")
        self.run_shell("--no-source", *args)
        self.assertEqual(self.read_trace(), ["override-env", "override-profile", "body", "override-logout"])
        self.run_shell("--config-dir", str(self.home / "missing-root"), *args)
        self.assertEqual(self.read_trace(), ["body"])
        self.assertFalse((self.home / "missing-root").exists())

    def run_guarded_startup(self, home):
        try:
            return subprocess.run(
                [self.binary, "-il", "--no-system-paths", "--no-titleline", "--no-history",
                 "-c", 'echo body >> "$HOME/trace"'],
                env=dict(self.env, HOME=str(home)), text=True, capture_output=True,
                timeout=3, start_new_session=True)
        except subprocess.TimeoutExpired:
            raise self.failureException(
                "automatic startup/logout file blocked shell execution for over 3 seconds") from None

    def check_special_startup_files(self, kind):
        stages = ((".cjshenv", "env"), (".cjprofile", "profile"),
                  (".cjshrc", "rc"), (".cjlogout", "logout"))
        for filename, stage in stages:
            with self.subTest(file=filename, kind=kind):
                home = self.home / stage
                self.trace_files(home)
                self.trace_files(home / ".config/cjsh", "alt-")
                path = home / filename
                path.unlink()
                if kind == "swap":
                    path.write_text('echo unexpected >> "$HOME/trace"\n')
                    self.env["CJSH_TEST_STARTUP_SWAP_PATH"] = str(path)
                elif kind == "directory":
                    path.mkdir()
                elif kind == "fifo":
                    os.mkfifo(path)
                else:
                    target = home / "startup-fifo"
                    os.mkfifo(target)
                    path.symlink_to(target.name)

                result = self.run_guarded_startup(home)
                if kind == "swap":
                    self.assertTrue(path.is_fifo(), "startup-file replacement was not injected")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")
                expected = ["env", "profile", "rc", "body", "logout"]
                expected[expected.index(stage)] = "alt-" + stage
                self.assertEqual((home / "trace").read_text().splitlines(), expected)

    def test_native_startup_skips_fifos(self):
        self.check_special_startup_files("fifo")

    def test_native_startup_skips_symlinks_to_fifos(self):
        self.check_special_startup_files("symlink")

    def test_native_startup_skips_directories(self):
        self.check_special_startup_files("directory")

    @unittest.skipIf(SKIP_PRELOAD_INJECTION, "file replacement injection requires a dynamic binary")
    def test_native_startup_rejects_file_replaced_with_fifo_during_open(self):
        self.env["DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"] = self.injector
        self.check_special_startup_files("swap")

    def test_native_startup_follows_regular_file_symlinks(self):
        targets = self.home / "targets"
        self.trace_files(targets)
        self.trace_files(self.home / ".config/cjsh", "alt-")
        for path in targets.iterdir():
            (self.home / path.name).symlink_to(path.relative_to(self.home))
        result = self.run_guarded_startup(self.home)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.read_trace(), ["env", "profile", "rc", "body", "logout"])

    def test_posix_startup_and_env_expansion(self):
        self.trace_files(self.home, "native-")
        (self.home / ".profile").write_text('echo profile >> "$HOME/trace"\n')
        (self.home / "interactive env").write_text('echo ENV >> "$HOME/trace"\n')
        self.env["ENV"] = '${HOME}/interactive env'
        system_profile = self.home / "system-profile"
        system_profile.write_text('echo system >> "$HOME/trace"\n')
        self.env["CJSH_TEST_SYSTEM_PROFILE"] = str(system_profile)
        self.env["DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"] = self.injector
        sh = self.home / "sh"
        sh.symlink_to(self.binary)
        for binary in (self.binary, str(sh)):
            for login in (False, True):
                for interactive in (False, True):
                    with self.subTest(binary=binary, login=login, interactive=interactive):
                        if login and SKIP_PRELOAD_INJECTION:
                            self.skipTest("system-profile injection requires a dynamic binary")
                        args = [binary, "--posix", "--no-sh-warning", "--no-history"]
                        args += (["-l"] if login else []) + (["-i"] if interactive else [])
                        args += ["-c", 'echo body >> "$HOME/trace"']
                        r = subprocess.run(args, env=self.env, capture_output=True, text=True, timeout=8)
                        self.assertEqual(r.returncode, 0, r.stderr)
                        self.assertEqual(self.read_trace(), (["system", "profile"] if login else []) +
                                         (["ENV"] if interactive else []) + ["body"])
        self.run_shell("--posix", "-il", "--no-config", "-c", ":")
        self.assertEqual(self.read_trace(), [])
        self.env["ENV"] = '$(touch "$HOME/unwanted")'
        self.run_shell("--posix", "-i", "-c", ":")
        self.assertFalse((self.home / "unwanted").exists())

    def test_failed_exec_contract(self):
        for mode, expected in (([], 0), (["--posix"], 127), (["--posix", "-i"], 0)):
            r = self.run_shell("--no-config", *mode, "-c", "exec /missing-batch2-command; echo survived")
            self.assertEqual(r.returncode, expected, r.stderr)
            self.assertEqual(r.stdout, "" if expected else "survived\n")
        path = self.home / "nonexecutable"
        path.write_text("echo no\n")
        r = self.run_shell("--posix", "-c", f"exec {shlex.quote(str(path))}; echo survived")
        self.assertEqual(r.returncode, 126)
        self.assertEqual(r.stdout, "")

    def child_environment(self, *args):
        r = self.run_shell(*args, "-c", "/usr/bin/env")
        self.assertEqual(r.returncode, 0, r.stderr)
        return dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)

    def test_environment_supplied_empty_absent(self):
        names = ("USER", "LOGNAME", "LANG", "PAGER", "TMPDIR", "PATH", "MANPATH")
        for value in ("batch2-inherited", "", None):
            for name in names:
                if value is None:
                    self.env.pop(name, None)
                else:
                    self.env[name] = value
            for args in (["--no-system-paths"], ["-l", "--no-system-paths"],
                         ["--no-config"], ["-l", "--no-config"],
                         ["-l", "--secure"], ["--posix"],
                         ["-m", "--no-system-paths"], ["-l", "-m", "--no-system-paths"],
                         ["-l", "--minimal", "--no-config"], ["-l", "-m", "--secure"]):
                with self.subTest(value=value, args=args):
                    child = self.child_environment(*args)
                    for name in names:
                        expected = pwd.getpwuid(os.getuid()).pw_name if value is None and name in ("USER", "LOGNAME") else value
                        self.assertEqual(child.get(name), expected, name)
        # An internal exec search fallback must not export a synthesized PATH.
        r = self.run_shell("--no-config", "-c", "env")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(any(line.startswith("PATH=") for line in r.stdout.splitlines()))

    def test_inherited_identity_still_initializes_shell_defaults(self):
        for user, logname in (("caller-user", "caller-logname"), ("", "")):
            with self.subTest(user=user, logname=logname):
                self.env.update(USER=user, LOGNAME=logname, SHLVL="6", IFS="bad")
                for name in ("HOSTNAME", "PS1", "PS2", "PS4", "CJSH_VERSION"):
                    self.env.pop(name, None)
                result = self.run_shell(
                    "--no-config", "-c",
                    'printf "%s|%s|%s|%s\\n" "$USER" "$LOGNAME" "$HOME" "$SHLVL"; '
                    'printf "IFS<%s>\\n" "$IFS"; '
                    'test -n "$HOSTNAME" && test -n "$PS1" && test -n "$PS2" && '
                    'test "$PS4" = "+ " && test -n "$CJSH_VERSION"')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, f"{user}|{logname}|{self.home}|7\nIFS< \t\n>\n")

    def test_account_defaults_for_individually_missing_identity_fields(self):
        account = pwd.getpwuid(os.getuid())
        for missing in ("USER", "LOGNAME", "HOME", "empty-HOME"):
            with self.subTest(missing=missing):
                self.env.update(USER="caller-user", LOGNAME="caller-logname", HOME=str(self.home))
                if missing == "empty-HOME":
                    self.env["HOME"] = ""
                else:
                    self.env.pop(missing)
                result = self.run_shell(
                    "--no-config", "-c", 'printf "%s|%s|%s\\n" "$USER" "$LOGNAME" "$HOME"')
                self.assertEqual(result.returncode, 0, result.stderr)
                user = account.pw_name if missing == "USER" else "caller-user"
                logname = account.pw_name if missing == "LOGNAME" else "caller-logname"
                home = account.pw_dir if missing in ("HOME", "empty-HOME") else str(self.home)
                self.assertEqual(result.stdout, f"{user}|{logname}|{home}\n")

    def test_default_system_paths(self):
        for value in ("/batch2/supplied:/batch2/supplied/bin:/batch2/supplied", "", None):
            for manpath in ("/batch2/man", "", None):
                for name, entry in (("PATH", value), ("MANPATH", manpath)):
                    if entry is None:
                        self.env.pop(name, None)
                    else:
                        self.env[name] = entry
                for args in ([], ["-l"], ["-i", "--no-titleline", "--no-history"],
                             ["-il", "--no-titleline", "--no-history"],
                             ["--minimal"], ["-l", "--minimal"],
                             ["-i", "-m", "--no-history"], ["-il", "-m", "--no-history"]):
                    with self.subTest(value=value, manpath=manpath, args=args):
                        child = self.child_environment(*args)
                        self.assertTrue(child.get("PATH"))
                        parts = child["PATH"].split(":")
                        if value and "-l" not in args and "-il" not in args:
                            self.assertEqual(child["PATH"], value)
                        else:
                            self.assertEqual(len(parts), len(set(parts)))
                        if value:
                            self.assertIn("/batch2/supplied", parts)
                            self.assertIn("/batch2/supplied/bin", parts)
                        else:
                            # This was the terminal-startup regression: commands must
                            # be found and children must receive the initialized PATH.
                            r = self.run_shell(*args, "-c", "env")
                            self.assertEqual(r.returncode, 0, r.stderr)
                            self.assertIn("PATH=" + child["PATH"], r.stdout.splitlines())
                        self.assertEqual(child.get("MANPATH"), manpath)

    def test_nonlogin_preserves_path_components(self):
        for value in (":/custom/bin::/usr/bin:/custom/bin:", ":", "::", " "):
            self.env["PATH"] = value
            for args in ([], ["-i", "--no-titleline", "--no-history"],
                         ["--minimal"], ["-i", "-m", "--no-history"]):
                with self.subTest(value=value, args=args):
                    self.assertEqual(self.child_environment(*args)["PATH"], value)

    def test_nested_shell_preserves_toolchain_precedence(self):
        toolchain = self.home / "toolchain/bin"
        toolchain.mkdir(parents=True)
        executable = toolchain / "ls"
        executable.write_text("#!/bin/sh\nprintf 'toolchain-ls\\n'\n")
        executable.chmod(0o755)
        self.env["PATH"] = "/usr/bin:/bin"
        inherited = f"{toolchain}:/usr/bin:/bin::{toolchain}:"
        command = 'printf "%s\\n" "$PATH" "$(command -v ls)" "$(ls)"'
        for args in ([], ["-i", "--no-titleline", "--no-history"]):
            with self.subTest(args=args):
                nested = shlex.join([self.binary, *args, "-c", command])
                r = self.run_shell("-c", f"PATH={shlex.quote(inherited)}; {nested}")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(r.stdout.splitlines(), [inherited, str(executable), "toolchain-ls"])

    def test_system_paths_precede_native_configuration(self):
        (self.home / ".cjshenv").write_text('PATH="/batch2/env:$PATH"\n')
        (self.home / ".cjprofile").write_text('PATH="/batch2/profile:$PATH"\n')
        for args in ([], ["--minimal"]):
            with self.subTest(args=args):
                self.env.pop("PATH", None)
                child = self.child_environment("-l", *args)
                self.assertTrue(child["PATH"].startswith("/batch2/profile:/batch2/env:"))
                self.assertIn("/usr/bin", child["PATH"].split(":"))
                self.env["PATH"] = "/batch2/inherited"
                child = self.child_environment("-l", *args, "--no-system-paths")
                self.assertEqual(child["PATH"], "/batch2/profile:/batch2/env:/batch2/inherited")

    def test_login_startup_arg_subcommand_removed(self):
        r = self.run_shell("-c", "cjshopt login-startup-arg --no-system-paths")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown subcommand 'login-startup-arg'", r.stderr)

    def test_history_path_from_rc_overrides_earlier_configuration(self):
        old = self.home / "earlier-history"
        old.write_text("echo earlier-history\n")
        selected = self.home / "selected history"
        selected.write_text("echo selected-history\n")
        (self.home / ".cjshrc").write_text('CJSH_HISTORY_FILE="$HOME/selected history"\n')
        for origin in ("default", "environment", ".cjshenv", ".cjprofile"):
            with self.subTest(origin=origin):
                self.env.pop("CJSH_HISTORY_FILE", None)
                for name in (".cjshenv", ".cjprofile"):
                    (self.home / name).unlink(missing_ok=True)
                if origin == "environment":
                    self.env["CJSH_HISTORY_FILE"] = str(old)
                elif origin != "default":
                    (self.home / origin).write_text('CJSH_HISTORY_FILE="$HOME/earlier-history"\n')
                result = self.run_shell("-il", "--no-titleline", "-c", "history; fc -ln")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertEqual([line.strip() for line in result.stdout.splitlines()],
                                 ["0  echo selected-history", "echo selected-history"])
                self.assertEqual(old.read_text(), "echo earlier-history\n")
                self.assertFalse((self.home / ".cache/cjsh/history.txt").exists())

    def test_history_storage_created_after_rc(self):
        (self.home / ".cjshrc").write_text('CJSH_HISTORY_FILE="~/history directory/new-history"\n')
        result = self.run_shell("-i", "--no-titleline", "-c", ":")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertTrue((self.home / "history directory/new-history").is_file())
        self.assertFalse((self.home / ".cache/cjsh/history.txt").exists())

    def test_startup_history_commands_follow_later_assignments(self):
        (self.home / "early-history").write_text("echo early-selection\n")
        (self.home / "late-history").write_text("echo late-selection\n")
        for command in ("history", "fc -ln"):
            with self.subTest(command=command):
                (self.home / ".cjshenv").write_text(
                    'CJSH_HISTORY_FILE="$HOME/early-history"\n'
                    f'{command} > "$HOME/early-output"\n')
                (self.home / ".cjshrc").write_text(
                    'CJSH_HISTORY_FILE="$HOME/late-history"\n'
                    f'{command} > "$HOME/late-output"\n')
                result = self.run_shell("-i", "--no-titleline", "-c", command)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertIn("echo early-selection", (self.home / "early-output").read_text())
                self.assertIn("echo late-selection", (self.home / "late-output").read_text())
                self.assertIn("echo late-selection", result.stdout)
                self.assertNotIn("echo early-selection", result.stdout)

    def test_history_storage_recovers_after_startup_path_change(self):
        fifo = self.home / "history-fifo"
        os.mkfifo(fifo)
        for path in (self.home, fifo):
            with self.subTest(path=path):
                self.env["CJSH_HISTORY_FILE"] = str(path)
                (self.home / ".cjshenv").write_text("history\n")
                (self.home / ".cjshrc").write_text('CJSH_HISTORY_FILE="$HOME/recovered/history"\n')
                result = self.run_shell("-i", "--no-titleline", "-c", "history; echo usable")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "usable\n")
                self.assertEqual(result.stderr.count("persistence unavailable"), 1, result.stderr)
                self.assertTrue((self.home / "recovered/history").is_file())

    def test_invalid_history_path_from_rc_does_not_use_earlier_storage(self):
        earlier = self.home / "earlier-history"
        self.env["CJSH_HISTORY_FILE"] = str(earlier)
        for kind in ("directory", "fifo"):
            with self.subTest(kind=kind):
                target = self.home / kind
                if kind == "directory":
                    target.mkdir()
                else:
                    os.mkfifo(target)
                (self.home / ".cjshrc").write_text(f'CJSH_HISTORY_FILE="$HOME/{kind}"\n')
                result = self.run_shell("-i", "--no-titleline", "-c",
                                        "history; fc -ln; echo usable")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "usable\n")
                self.assertEqual(result.stderr.count("persistence unavailable"), 1, result.stderr)
                self.assertFalse(earlier.exists())

    def test_history_path_is_fixed_after_interactive_startup(self):
        (self.home / "selected-history").write_text("echo selected-history\n")
        (self.home / "other-history").write_text("echo other-history\n")
        (self.home / ".cjshrc").write_text('CJSH_HISTORY_FILE="$HOME/selected-history"\n')
        result = self.run_shell("-i", "--no-titleline", "-c",
                                'CJSH_HISTORY_FILE="$HOME/other-history"; history')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("echo selected-history", result.stdout)
        self.assertNotIn("echo other-history", result.stdout)

    def test_unsetting_history_override_during_startup_restores_default(self):
        earlier = self.home / "earlier-history"
        earlier.write_text("echo earlier-history\n")
        default = self.home / ".cache/cjsh/history.txt"
        default.parent.mkdir(parents=True)
        default.write_text("echo default-history\n")
        self.env["CJSH_HISTORY_FILE"] = str(earlier)
        (self.home / ".cjshenv").write_text('history > "$HOME/early-output"\n')
        (self.home / ".cjshrc").write_text("unset CJSH_HISTORY_FILE\n")
        result = self.run_shell("-i", "--no-titleline", "-c", "history")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("echo earlier-history", (self.home / "early-output").read_text())
        self.assertIn("echo default-history", result.stdout)
        self.assertNotIn("echo earlier-history", result.stdout)

    def test_noninteractive_history_remains_lazy_and_refreshes_startup_selection(self):
        result = self.run_shell("-c", ":")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.home / ".cache/cjsh").exists())
        (self.home / "script-history").write_text("echo script-history\n")
        result = self.run_shell("-c", 'CJSH_HISTORY_FILE="$HOME/script-history"; history')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("echo script-history", result.stdout)

        (self.home / "early-history").write_text("echo early-history\n")
        (self.home / ".cjshenv").write_text(
            'CJSH_HISTORY_FILE="$HOME/early-history"\n'
            'history > "$HOME/early-output"\n'
            'CJSH_HISTORY_FILE="$HOME/script-history"\n')
        result = self.run_shell("-c", "history")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("echo script-history", result.stdout)
        self.assertNotIn("echo early-history", result.stdout)

    def test_editor_and_builtins_use_final_startup_history_path(self):
        earlier = self.home / "earlier-history"
        earlier_content = 'echo earlier-selection > "$HOME/recalled"\n'
        earlier.write_text(earlier_content)
        selected = self.home / "selected-history"
        selected.write_text('echo selected-selection > "$HOME/recalled"\n')
        self.env["CJSH_HISTORY_FILE"] = str(earlier)
        (self.home / ".cjshrc").write_text(
            'cjshopt set-history-max 50\n'
            'CJSH_HISTORY_FILE="$HOME/selected-history"\n')
        with mock.patch.dict(os.environ, self.env, clear=True):
            session = IdleHookSession(self.binary, str(self.home), argv=[
                self.binary, "--no-titleline", "--no-prompt-vars", "--no-agent",
                "--no-completions", "--no-syntax-highlighting"])
        self.addCleanup(session.close)
        session.wait_for_prompt(0)
        start = len(session.output)
        session.write(b"\x1b[A\r")
        session.wait_for_prompt(start, command_completed=True)
        self.assertEqual((self.home / "recalled").read_text(), "selected-selection\n")
        session.run_command(b"echo saved-to-selected-history")
        session.run_command(b'history > "$HOME/history-output"')
        session.run_command(b'fc -ln > "$HOME/fc-output"')
        session.write(b"exit\r")
        self.assertEqual(session.wait_for_exit(), 0)
        for path in (selected, self.home / "history-output", self.home / "fc-output"):
            content = path.read_text()
            self.assertIn("saved-to-selected-history", content)
            self.assertNotIn("earlier-selection", content)
        self.assertEqual(earlier.read_text(), earlier_content)

    def test_startup_history_limits_wait_for_editor_initialization(self):
        history = self.home / "selected-history"
        commands = [f'echo retained-{number} > "$HOME/recalled"' for number in range(4)]
        original = "\n".join(commands) + "\n"
        history.write_text(original)
        self.env["CJSH_HISTORY_FILE"] = str(history)
        for name, limits in ((".cjshenv", (1,)), (".cjprofile", (0, "default", 2)),
                             (".cjshrc", (1, 3))):
            (self.home / name).write_text(
                "".join(f"cjshopt set-history-max {limit}\n" for limit in limits) +
                f'cat "$CJSH_HISTORY_FILE" > "$HOME/{name}-snapshot"\n')
        with mock.patch.dict(os.environ, self.env, clear=True):
            session = IdleHookSession(self.binary, str(self.home), argv=[
                self.binary, "--login", "--no-titleline", "--no-prompt-vars", "--no-agent",
                "--no-completions", "--no-syntax-highlighting"])
        self.addCleanup(session.close)
        session.wait_for_prompt(0)
        for name in (".cjshenv", ".cjprofile", ".cjshrc"):
            self.assertEqual((self.home / f"{name}-snapshot").read_text(), original)
        self.assertEqual([line for line in history.read_text().splitlines()
                          if not line.startswith("#")], commands[-3:])
        start = len(session.output)
        session.write(b"\x1b[A\r")
        session.wait_for_prompt(start, command_completed=True)
        self.assertEqual((self.home / "recalled").read_text(), "retained-3\n")
        start = session.run_command(b"cjshopt set-history-max status")
        self.assertIn(b"History file retains up to 3 entries.", session.output[start:])
        session.write(b"exit\r")
        self.assertEqual(session.wait_for_exit(), 0)

    def test_startup_history_limits_apply_without_an_editor(self):
        earlier = self.home / "earlier-history"
        selected = self.home / "selected-history"
        original = "echo first\necho second\necho third\n"
        script = self.home / "script.cjsh"
        script.write_text("history\n")
        for args, startup_file in ((("-c", "history"), ".cjshenv"),
                                   ((str(script),), ".cjshenv"),
                                   (("-il", "-c", "history"), ".cjshrc"),
                                   (("-il", str(script)), ".cjshrc"),
                                   (("-il",), ".cjshrc")):
            for limit in (0, 2):
                with self.subTest(args=args, limit=limit):
                    earlier.write_text(original)
                    selected.write_text(original)
                    self.env["CJSH_HISTORY_FILE"] = str(earlier)
                    for name in (".cjshenv", ".cjshrc"):
                        (self.home / name).unlink(missing_ok=True)
                    (self.home / startup_file).write_text(
                        f'cjshopt set-history-max {limit}\n'
                        'CJSH_HISTORY_FILE="$HOME/selected-history"\n')
                    result = self.run_shell(*args, input="history\n")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stderr, "")
                    self.assertEqual(earlier.read_text(), original)
                    expected = [] if limit == 0 else ["echo second", "echo third"]
                    self.assertEqual([line for line in selected.read_text().splitlines()
                                      if not line.startswith("#")], expected)
                    self.assertEqual([line.strip() for line in result.stdout.splitlines()],
                                     [f"{index}  {command}" for index, command in enumerate(expected)])

    def test_runtime_history_limit_changes_apply_immediately(self):
        history = self.home / "selected-history"
        self.env["CJSH_HISTORY_FILE"] = str(history)
        runtime_config = self.home / "runtime.cjsh"
        runtime_config.write_text("cjshopt set-history-max 2\n")
        for args in ((), ("-i",)):
            with self.subTest(args=args):
                history.write_text("echo first\necho second\necho third\n")
                result = self.run_shell(*args, "-c",
                                        'source "$HOME/runtime.cjsh"; history; '
                                        'cjshopt set-history-max 0; history')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertEqual([line.strip() for line in result.stdout.splitlines()], [
                    "History file will retain up to 2 entries.",
                    "0  echo second", "1  echo third", "History persistence disabled."])
                self.assertEqual(history.read_text(), "")

    def test_startup_history_limit_respects_final_persistence_flags(self):
        history = self.home / "selected-history"
        self.env["CJSH_HISTORY_FILE"] = str(history)
        original = "echo preserved-history\n"
        for flag in ("--no-history", "--secure"):
            with self.subTest(flag=flag):
                history.write_text(original)
                (self.home / ".cjprofile").write_text('cjshopt set-history-max 0\n')
                result = self.run_shell(flag, "-il", "-c", "echo usable")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertEqual(result.stdout, "usable\n")
                self.assertEqual(history.read_text(), original)

    def test_unavailable_persistence(self):
        missing = self.home / "missing"
        readonly = self.home / "readonly"
        readonly.mkdir()
        readonly.chmod(0o500)
        self.addCleanup(readonly.chmod, 0o700)
        for path in (missing, readonly):
            self.env["HOME"] = str(path)
            r = self.run_shell("-i", "--no-config", "-c", "echo usable")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, "usable\n")
            if os.getuid() != 0:
                self.assertEqual(r.stderr.count("persistence unavailable"), 1, r.stderr)
        self.assertFalse(missing.exists())
        self.env["HOME"] = str(self.home)
        self.env["CJSH_HISTORY_FILE"] = str(self.home)  # directory, not a file
        r = self.run_shell("-i", "--no-config", "-c", "echo usable")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr.count("persistence unavailable"), 1)
        session = IdleHookSession(self.binary, str(missing), argv=[self.binary, "--no-config", "--no-titleline", "--no-prompt-vars"])
        self.addCleanup(session.close)
        session.wait_for_prompt(0)
        session.run_command(b"echo usable-one")
        session.run_command(b"echo usable-two")
        self.assertEqual(bytes(session.output).count(b"persistence unavailable"), 1)
        session.write(b"exit\r")
        self.assertEqual(session.wait_for_exit(), 0)

    def test_invalid_history_and_completion_storage_are_independent(self):
        fifo = self.home / "history-fifo"
        os.mkfifo(fifo)
        self.env["CJSH_HISTORY_FILE"] = str(fifo)
        r = self.run_shell("-i", "--no-config", "-c", "echo usable")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "usable\n")
        self.assertEqual(r.stderr.count("persistence unavailable"), 1)
        self.env.pop("CJSH_HISTORY_FILE")
        completion = self.home / ".cache/cjsh/generated_completions"
        completion.rmdir()
        completion.write_text("not a directory")
        r = self.run_shell("-i", "--no-config", "-c", "echo usable")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr.count("persistence unavailable"), 1)
        self.assertIn("generated_completions", r.stderr)
        self.assertTrue((self.home / ".cache/cjsh/history.txt").is_file())

    @unittest.skipIf(SKIP_PRELOAD_INJECTION, "credential injection requires a dynamic binary")
    def test_privileged_startup_refused_before_files(self):
        self.trace_files(self.home)
        self.env["DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"] = self.injector
        for kind in ("UID", "GID"):
            self.env[f"CJSH_TEST_{kind}_MISMATCH"] = "1"
            for args in ([], ["--posix"], ["--no-config"]):
                r = self.run_shell(*args, "-il", "-c", "echo executed")
                self.assertEqual(r.returncode, 1, r.stderr)
                self.assertEqual(r.stdout, "")
                self.assertIn("mismatched real and effective IDs", r.stderr)
                self.assertEqual(self.read_trace(), [])
            del self.env[f"CJSH_TEST_{kind}_MISMATCH"]


if __name__ == "__main__":
    StartupTests.binary = str(Path(sys.argv[1]).resolve())
    StartupTests.injector = str(Path(sys.argv[2]).resolve())
    unittest.main(argv=[sys.argv[0]], verbosity=2)
