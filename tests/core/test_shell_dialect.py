#!/usr/bin/env python3

# test_shell_dialect.py
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

"""Native and POSIX dialect selection, option behavior, and script dispatch."""

import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def run(shell, prefix, script, source="command", extra=()):
    with tempfile.TemporaryDirectory(prefix="cjsh-dialect-test-") as tmp:
        root = Path(tmp)
        (root / "nested").mkdir()
        env = {"HOME": tmp, "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb"}
        argv = [shell, *prefix, *extra]
        data = None
        if source == "command":
            argv += ["-c", script]
        elif source == "stdin":
            data = script
        else:
            name = "script.cjsh" if source.startswith("cjsh") else "script"
            (root / name).write_text(script)
            if source == "file":
                argv += [name]
            elif source == "source":
                argv += ["-c", ". ./script; cjshopt dialect status"]
            else:
                argv += ["-c", "./script.cjsh" + ("; :" if source == "cjsh_compound" else "")]
        proc = subprocess.Popen(argv, cwd=tmp, env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(data, timeout=8)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            raise AssertionError(f"timeout: {argv}")
        return proc.returncode, stdout, stderr


def main():
    cjsh = str(Path(sys.argv[1]).resolve())
    failures = total = 0

    def check(name, actual, expected):
        nonlocal failures, total
        total += 1
        if actual != expected:
            failures += 1
            print(f"FAIL {name}: {actual!r}; expected {expected!r}")

    def native(name, script, stdout, status=0, flags=()):
        result = run(cjsh, ["--no-config", *flags], script)
        check(name, result[:2], (status, stdout))

    native("default dialect", "cjshopt dialect status", "cjsh\n")
    native("last dialect wins", "cjshopt dialect status", "cjsh\n", flags=("--posix", "--dialect=cjsh"))
    native("runtime switches", "cjshopt dialect posix; cjshopt dialect status; cjshopt dialect cjsh; cjshopt dialect status", "posix\ncjsh\n")
    native("invalid dialect preserves selection", "cjshopt dialect typo 2>/dev/null; cjshopt dialect status", "cjsh\n")
    native("native pipeline representation", 'false | true; printf "<%s>\\n" "${PIPESTATUS[@]}"', "<1 0>\n")
    native("native arrays preserved", 'a=(a b); printf "<%s>\\n" "${a[@]}"', "<a b>\n")
    native("native errexit preserved", 'set -e; x=$(false; echo bad); echo bad', "", 1)
    native("native aliases preserved", 'alias cjsh_probe_echo=echo\ncjsh_probe_echo yes', "yes\n")
    native("native regex preserved", '[[ abc =~ b ]]; printf "%s\\n" "${BASH_REMATCH-unset}"', "unset\n")
    native("subshell dialect isolated", '(cjshopt dialect posix; cjshopt dialect status); cjshopt dialect status', "posix\ncjsh\n")
    native("substitution dialect isolated", 'x=$(cjshopt dialect posix; cjshopt dialect status); echo "$x"; cjshopt dialect status', "posix\ncjsh\n")
    native("posix restores extglob", 'shopt -s extglob; cjshopt dialect posix; cjshopt dialect cjsh; shopt -q extglob', "")
    native("posix startup restores environment", 'cjshopt dialect cjsh; echo "${POSIXLY_CORRECT-unset}"', "unset\n", flags=("--posix",))
    native("posix runtime restores variable", 'POSIXLY_CORRECT=original; cjshopt dialect posix; echo "$POSIXLY_CORRECT"; cjshopt dialect cjsh; echo "$POSIXLY_CORRECT"', "1\noriginal\n")
    native("posix rejects shopt listing flag", ':', "", 2, flags=("--posix", "-O"))
    native("explicit shopt persists", 'shopt -s expand_aliases; cjshopt dialect posix; cjshopt dialect cjsh; shopt -q expand_aliases', "")
    native("native autocd default", 'shopt -q autocd', "")
    native("autocd defaults follow dialect", 'shopt -q autocd; echo "$?"; cjshopt dialect posix; shopt -q autocd; echo "$?"; cjshopt dialect cjsh; shopt -q autocd', "0\n1\n")
    native("explicit autocd survives dialect changes", 'shopt -u autocd; cjshopt dialect posix; cjshopt dialect cjsh; shopt -q autocd', "", 1)
    native("posix suppresses explicit autocd", 'shopt -s autocd; cjshopt dialect posix; nested; printf "%s\\n" "$?"; cjshopt dialect cjsh; shopt -q autocd', "127\n")
    native("startup enables autocd", 'shopt -p autocd', "shopt -s autocd\n", flags=("-O", "autocd"))
    native("startup disables native autocd", 'shopt -p autocd', "shopt -u autocd\n", 1, flags=("+O", "autocd"))
    native("native interactive autocd", 'nested; printf "%s\\n" "${PWD##*/}"', "nested\n", flags=("-i",))
    native("native autocd can be disabled", 'shopt -u autocd; nested; printf "%s\\n" "$?"', "127\n", flags=("-i",))
    native("moved extglob interface", 'cjshopt extglob on', "", 1)
    native("moved globstar interface", 'set -o globstar', "", 1)
    native("moved huponexit interface", 'set -o huponexit', "", 1)
    for dialect in ("cjsh", "posix"):
        flags = ("--dialect", dialect)
        native(dialect + " -e", 'false; echo bad', "", 1, (*flags, "-e"))
        native(dialect + " -C", 'echo one > artifact; echo two > artifact; cat artifact', "one\n", flags=(*flags, "-C"))
        native(dialect + " -v", 'echo yes', "yes\n", flags=(*flags, "-v"))
        native(dialect + " -h", 'echo yes', "yes\n", flags=(*flags, "-h"))
    native("invocation shopt", 'shopt -q globstar', "", flags=("-O", "globstar"))
    native("command payload is data", 'printf "%s\\n" --bash', "--bash\n")
    for dialect in ("cjsh", "posix"):
        result = run(cjsh, ["--no-config", "--dialect", dialect],
                     'read -r value\ninput line\nprintf "%s\\n" "$value"\n', "stdin", ("-s",))
        check(dialect + " stdin shares read input", result[:2], (0, "input line\n"))

    native("posix dialect flag", "cjshopt dialect status", "posix\n", flags=("--dialect", "posix"))
    native("removed Bash flag", ":", "", 1, flags=("--bash",))
    native("removed Bash dialect flag", ":", "", 2, flags=("--dialect", "bash"))
    native("removed Bash dialect assignment", ":", "", 2, flags=("--dialect=bash",))
    native("removed runtime Bash dialect", "cjshopt dialect bash", "", 2)
    native("rejected Bash dialect preserves selection", "cjshopt dialect bash 2>/dev/null; cjshopt dialect status", "cjsh\n")
    native("removed substitution option", "shopt -s inherit_errexit", "", 1)
    for dialect in ("cjsh", "posix"):
        for shebang in ("#!/bin/bash", "#!/usr/bin/env bash", "#!/usr/bin/env -S bash -e"):
            sources = ("file", "source", "cjsh", "cjsh_compound") if dialect == "cjsh" else ("file", "source")
            for source in sources:
                result = run(cjsh, ["--no-config", "--dialect", dialect],
                             shebang + "\ncjshopt dialect status\n", source)
                expected = (dialect + "\n") * (2 if source == "source" else 1)
                check(dialect + " " + shebang + " " + source, result[:2], (0, expected))

    print(f"Total tests: {total}\nPassed: {total - failures}\nFailed: {failures}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
