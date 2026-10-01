#!/usr/bin/env python3

# test_bash_mode.py
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

"""Native dialect regressions and differential checks against an explicit Bash 5.3 oracle."""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile


@dataclass
class Case:
    name: str
    script: str
    diagnostic: str = ""


CASES = [
    Case("pipeline array", 'false | true; printf "<%s>\\n" "${PIPESTATUS[@]}"'),
    Case("pipeline index", 'false | true; printf "%s:%s\\n" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}"'),
    Case("array fields", 'a=("two words" "" "*"); printf "<%s>\\n" "${a[@]}"'),
    Case("array literals", "a[0]='a$b'; a[1]='[1]=literal'; printf '<%s>\\n' \"${a[@]}\""),
    Case("array concatenation", 'a=(one two); b=(three four); printf "<%s>\\n" "pre${a[@]}mid${b[@]}post"'),
    Case("empty array", 'a=(); set -- "${a[@]}"; printf "%s\\n" "$#"'),
    Case("sparse array keys", 'a[3]=three; a[8]=eight; printf "<%s>\\n" "${!a[@]}"'),
    Case("errexit substitution", 'set -e; value=$(false; printf survived); printf "%s\\n" "$value"'),
    Case("inherit errexit", 'shopt -s inherit_errexit; set -e; value=$(false; echo bad); echo bad'),
    Case("substitution isolates option", 'set -e; value=$(true); false; echo bad'),
    Case("noninteractive aliases", 'alias cjsh_bash_probe_echo=echo\ncjsh_bash_probe_echo hello', "command not found"),
    Case("explicit aliases", 'shopt -s expand_aliases\nalias cjsh_bash_probe_echo=echo\ncjsh_bash_probe_echo hello'),
    Case("regex captures", '[[ abc =~ (b) ]]; printf "<%s>\\n" "${BASH_REMATCH[@]}"'),
    Case("regex no match", '[[ abc =~ (b) ]]; [[ xyz =~ (b) ]]; printf "%s:%s\\n" "$?" "${#BASH_REMATCH[@]}"'),
    Case("regex optional capture", '[[ ac =~ a(b)?c ]]; printf "<%s>\\n" "${BASH_REMATCH[@]}"'),
    Case("regex ERE", "pattern='a|ab'; [[ ab =~ $pattern ]]; printf '<%s>\\n' \"${BASH_REMATCH[@]}\""),
    Case("regex invalid", "pattern='['; [[ a =~ $pattern ]]; printf '%s\\n' \"$?\"", "invalid regular expression"),
    Case("shopt set and print", 'shopt -s globstar extglob; shopt -p globstar extglob'),
    Case("shopt unset status", 'shopt -u extglob; shopt -p extglob'),
    Case("shopt quiet", 'shopt -s extglob; shopt -q extglob globstar'),
    Case("shopt set namespace", 'shopt -so pipefail; shopt -qo pipefail; false | true; echo "$?"'),
    Case("shopt unknown", 'shopt -s unknown_cjsh_option', "invalid shell option name"),
    Case("shopt conflicting flags", 'shopt -su extglob', "cannot set and unset"),
    Case("shopt display", 'shopt extglob'),
    Case("globstar", 'shopt -s globstar; printf "%s\\n" **/fixture.txt'),
    Case("extglob", 'shopt -s extglob\nprintf "%s\\n" @(one|two).txt'),
    Case("brace option", 'set +B; printf "%s\\n" {a,b}; set -B; printf "%s\\n" {a,b}'),
    Case("filesystem effects", 'printf before > artifact; printf after >> artifact; cat artifact'),
]


def run(shell, prefix, script, source="command", extra=()):
    with tempfile.TemporaryDirectory(prefix="cjsh-bash-test-") as tmp:
        root = Path(tmp)
        (root / "nested").mkdir()
        (root / "nested/fixture.txt").write_text("fixture\n")
        for name in ("one.txt", "two.txt", "other.log"):
            (root / name).touch()
        env = {"HOME": tmp, "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb"}
        argv = [shell, *prefix, *extra]
        data = None
        if source == "command":
            argv += ["-c", script]
        elif source == "stdin":
            data = script
        elif source == "eval":
            argv += ["-c", "eval " + shlex.quote(script)]
        else:
            (root / "script").write_text(script)
            argv += ["script"] if source == "file" else ["-c", '. ./script']
        proc = subprocess.Popen(argv, cwd=tmp, env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(data, timeout=8)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.communicate()
            raise AssertionError(f"timeout: {argv}")
        artifact = (root / "artifact").read_bytes() if (root / "artifact").exists() else None
        return proc.returncode, stdout, stderr, artifact


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cjsh")
    parser.add_argument("category", choices=("native", "differential"))
    parser.add_argument("--bash", default=os.environ.get("CJSH_BASH_REFERENCE") or shutil.which("bash"))
    parser.add_argument("--bash-version", default=os.environ.get("CJSH_BASH_VERSION", "5.3"))
    args = parser.parse_args()
    cjsh = str(Path(args.cjsh).resolve())
    failures = 0
    total = 0

    def check(name, actual, expected):
        nonlocal failures, total
        total += 1
        if actual != expected:
            failures += 1
            print(f"FAIL {name}: {actual!r}; expected {expected!r}")

    if args.category == "differential":
        if not args.bash:
            print("SKIP: set CJSH_BASH_REFERENCE to a Bash 5.3 executable")
            return 77
        version = subprocess.run([args.bash, "--noprofile", "--norc", "-c", 'printf "%s" "$BASH_VERSION"'],
                                 capture_output=True, text=True, check=True).stdout
        if not re.match(re.escape(args.bash_version) + r"(?:\.|\()", version):
            print(f"SKIP: reference is Bash {version}, expected {args.bash_version}")
            return 77
        print(f"Reference: {args.bash} ({version})")
        for case in CASES:
            for source in ("command", "file", "stdin", "eval", "source"):
                reference = run(args.bash, ["--noprofile", "--norc"], case.script, source)
                actual = run(cjsh, ["--no-config", "--bash"], case.script, source)
                if case.diagnostic:
                    check(case.name + "/" + source,
                          (actual[0], actual[1], case.diagnostic in actual[2], actual[3]),
                          (reference[0], reference[1], case.diagnostic in reference[2], reference[3]))
                else:
                    check(case.name + "/" + source, actual, reference)
    else:
        def native(name, script, stdout, status=0, flags=()):
            result = run(cjsh, ["--no-config", *flags], script)
            check(name, result[:2], (status, stdout))

        native("default dialect", "cjshopt dialect status", "cjsh\n")
        native("bash flag", "cjshopt dialect status", "bash\n", flags=("--bash",))
        native("dialect flag", "cjshopt dialect status", "bash\n", flags=("--dialect", "bash"))
        native("last dialect wins", "cjshopt dialect status", "cjsh\n", flags=("--posix", "--dialect=cjsh"))
        native("runtime switches", "cjshopt dialect bash; cjshopt dialect status; cjshopt dialect posix; cjshopt dialect status; cjshopt dialect cjsh; cjshopt dialect status", "bash\nposix\ncjsh\n")
        native("invalid dialect preserves selection", "cjshopt dialect typo 2>/dev/null; cjshopt dialect status", "cjsh\n")
        native("native pipeline representation", 'false | true; printf "<%s>\\n" "${PIPESTATUS[@]}"', "<1 0>\n")
        native("native arrays preserved", 'a=(a b); printf "<%s>\\n" "${a[@]}"', "<a b>\n")
        native("native errexit preserved", 'set -e; x=$(false; echo bad); echo bad', "", 1)
        native("native aliases preserved", 'alias cjsh_probe_echo=echo\ncjsh_probe_echo yes', "yes\n")
        native("native regex preserved", '[[ abc =~ b ]]; printf "%s\\n" "${BASH_REMATCH-unset}"', "unset\n")
        native("subshell dialect isolated", '(cjshopt dialect bash; cjshopt dialect status); cjshopt dialect status', "bash\ncjsh\n")
        native("substitution dialect isolated", 'x=$(cjshopt dialect bash; cjshopt dialect status); echo "$x"; cjshopt dialect status', "bash\ncjsh\n")
        native("native alias restored", 'alias cjsh_probe_echo=echo\ncjshopt dialect bash\ncjsh_probe_echo no 2>/dev/null\ncjshopt dialect cjsh\ncjsh_probe_echo yes', "yes\n")
        native("native pipeline restored", 'cjshopt dialect bash; false | true; cjshopt dialect cjsh; false | true; printf "<%s>\\n" "${PIPESTATUS[@]}"', "<1 0>\n")
        native("posix restores extglob", 'shopt -s extglob; cjshopt dialect posix; cjshopt dialect cjsh; shopt -q extglob', "")
        native("posix startup restores environment", 'cjshopt dialect cjsh; echo "${POSIXLY_CORRECT-unset}"', "unset\n", flags=("--posix",))
        native("posix runtime restores variable", 'POSIXLY_CORRECT=original; cjshopt dialect posix; echo "$POSIXLY_CORRECT"; cjshopt dialect cjsh; echo "$POSIXLY_CORRECT"', "1\noriginal\n")
        native("posix rejects shopt listing flag", ':', "", 2, flags=("--posix", "-O"))
        native("explicit shopt persists", 'shopt -s expand_aliases; cjshopt dialect bash; shopt -q expand_aliases', "")
        native("moved extglob interface", 'cjshopt extglob on', "", 1)
        native("moved globstar interface", 'set -o globstar', "", 1)
        native("moved huponexit interface", 'set -o huponexit', "", 1)
        for dialect in ("cjsh", "posix", "bash"):
            flags = ("--dialect", dialect)
            native(dialect + " -e", 'false; echo bad', "", 1, (*flags, "-e"))
            native(dialect + " -C", 'echo one > artifact; echo two > artifact; cat artifact', "one\n", flags=(*flags, "-C"))
            native(dialect + " -v", 'echo yes', "yes\n", flags=(*flags, "-v"))
            native(dialect + " -h", 'echo yes', "yes\n", flags=(*flags, "-h"))
        native("invocation shopt", 'shopt -q globstar', "", flags=("-O", "globstar"))
        native("command payload is data", 'printf "%s\\n" --bash', "--bash\n")
        for dialect in ("cjsh", "posix", "bash"):
            result = run(cjsh, ["--no-config", "--dialect", dialect],
                         'read -r value\ninput line\nprintf "%s\\n" "$value"\n', "stdin", ("-s",))
            check(dialect + " stdin shares read input", result[:2], (0, "input line\n"))

    print(f"Total tests: {total}\nPassed: {total - failures}\nFailed: {failures}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
