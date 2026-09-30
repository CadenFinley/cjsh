#!/usr/bin/env python3

# test_posix_mode.py
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

"""POSIX.1-2024 behavior and cjsh's additional strict-language restrictions.

Expected results are specified here, not copied from another shell. Run each
language fixture through both mode entry points and independent execution paths.
References: XCU 2.2, 2.6, 2.8, 2.9, 2.15, and the sh/set/dot utility pages:
https://pubs.opengroup.org/onlinepubs/9799919799/utilities/V3_chap02.html
"""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile


@dataclass
class Case:
    name: str
    script: str
    stdout: str = ""
    status: int | None = 0  # None requires a nonzero status and a diagnostic.
    diagnostic: str = ""


CORE = [
    Case("assignments", 'x=one; x=two; printf "%s\\n" "$x"', "two\n"),
    Case("temporary assignments", 'unset x; x=one true; echo "${x-unset}"', "unset\n"),
    Case("special assignments", 'unset x; x=one :; echo "$x"', "one\n"),
    Case("command assignments", 'unset x; x=one command :; echo "${x-unset}"', "unset\n"),
    Case("and or", 'false && echo bad; true || echo bad; false || echo yes', "yes\n"),
    Case("if elif", 'if false; then :; elif true; then echo yes; else :; fi', "yes\n"),
    Case("for positional", 'set -- "a b" c; for x; do printf "<%s>\\n" "$x"; done', "<a b>\n<c>\n"),
    Case("for empty", 'for x in; do echo bad; done'),
    Case("while", 'x=0; while [ "$x" -lt 2 ]; do echo "$x"; x=$((x+1)); done', "0\n1\n"),
    Case("until", 'x=0; until [ "$x" -eq 2 ]; do x=$((x+1)); done; echo "$x"', "2\n"),
    Case("break", 'for x in a b; do echo "$x"; break; done', "a\n"),
    Case("continue", 'for x in a b; do continue; echo bad; done; echo yes', "yes\n"),
    Case("case", 'case abc in z*) echo bad;; a*) echo yes;; esac', "yes\n"),
    Case("case fallthrough 2024", 'case a in a) echo one ;& b) echo two ;; esac', "one\ntwo\n"),
    Case("function arguments", 'f() { printf "%s:%s\\n" "$#" "$1"; }; f "a b"', "1:a b\n"),
    Case("function return", 'f() { return 7; }; f; echo "$?"', "7\n"),
    Case("subshell environment", 'x=outer; (x=inner); echo "$x"', "outer\n"),
    Case("group environment", 'x=outer; { x=inner; }; echo "$x"', "inner\n"),
    Case("pipeline", 'printf "hi\\n" | cat', "hi\n"),
    Case("pipeline status", 'false | true; echo "$?"', "0\n"),
    Case("pipeline negation", '! true; echo "$?"', "1\n"),
    Case("pipefail 2024", 'set -o pipefail; false | true; echo "$?"', "1\n"),
    Case("pipefail rightmost", 'set -o pipefail; (exit 3) | (exit 7) | true; echo "$?"', "7\n"),
    Case("pipefail negation", 'set -o pipefail; ! false | true; echo "$?"', "0\n"),
    Case("redirections", 'printf hi > output; cat < output; printf bye >> output; cat output', "hihibye"),
    Case("here document", 'cat <<EOF\n$x\nEOF', "\n"),
    Case("quoted here document", "cat <<'EOF'\n$x [[ a+=b ${x//a/b}\nEOF", "$x [[ a+=b ${x//a/b}\n"),
    Case("exit trap", "trap 'echo done' 0; :", "done\n"),
    Case("wait", '(exit 7) & p=$!; wait "$p"; echo "$?"', "7\n"),
    Case("background pipeline", 'printf hi | cat > output & p=$!; wait "$p"; cat output', "hi"),
    Case("empty here document", 'cat <<EOF\nEOF\necho yes', "yes\n"),
    Case("blank here document lines", 'cat <<EOF\n\nx\n\nEOF', "\nx\n\n"),
]

EXPANSION = [
    Case("tilde", 'printf "%s\\n" ~', "@HOME@\n"),
    Case("tilde suffix", 'printf "%s\\n" ~/file', "@HOME@/file\n"),
    Case("quoted tilde", 'printf "%s\\n" "~" \'~\'', "~\n~\n"),
    Case("tilde assignment", 'x=~; printf "%s\\n" "$x"', "@HOME@\n"),
    Case("tilde assignment colon", 'x=~/a:~/b; printf "%s\\n" "$x"', "@HOME@/a:@HOME@/b\n"),
    Case("tilde export", 'export x=~/a; printf "%s\\n" "$x"', "@HOME@/a\n"),
    Case("tilde quoted assignment", 'x="~"; echo "$x"', "~\n"),
    Case("tilde field preservation", 'HOME="/a b/*"; set -- ~; printf "%s:%s\\n" "$#" "$1"', "1:/a b/*\n"),
    Case("argument plus equal", "printf '%s\\n' a+=b", "a+=b\n"),
    Case("argument double bracket", "printf '%s\\n' a[[b", "a[[b\n"),
    Case("quoted extensions", "printf '%s\\n' '[[ ]]' 'x+=2' '${x//a/b}'", "[[ ]]\nx+=2\n${x//a/b}\n"),
    Case("arithmetic", 'x=1; printf "%s:%s\\n" "$((x+=2))" "$x"', "3:3\n"),
    Case("arithmetic precedence", 'echo "$((2+3*4))"', "14\n"),
    Case("default", 'unset x; echo "${x:-default}"', "default\n"),
    Case("default empty", 'x=; echo "${x-default}:${x:-default}"', ":default\n"),
    Case("assignment expansion", 'unset x; echo "${x:=value}:$x"', "value:value\n"),
    Case("alternate", 'x=one; echo "${x:+yes}:${missing+no}"', "yes:\n"),
    Case("length", 'x=abcdef; echo "${#x}"', "6\n"),
    Case("prefix suffix", 'x=abcabc; echo "${x#a*}:${x##a*}:${x%c*}:${x%%c*}"', "bcabc::abcab:ab\n"),
    Case("operand extension text", 'unset x; echo "${x:-a+=b}"', "a+=b\n"),
    Case("quoted at", 'set -- "a b" "" c; printf "<%s>\\n" "$@"', "<a b>\n<>\n<c>\n"),
    Case("quoted star", 'IFS=:; set -- a b; printf "%s\\n" "$*"', "a:b\n"),
    Case("splitting", 'x="a b"; set -- $x; echo "$#:$1:$2"', "2:a:b\n"),
    Case("command substitution", 'x=$(printf "a\\n\\n"); echo "$x"', "a\n"),
    Case("backquotes", 'echo "`printf hi`"', "hi\n"),
    Case("dollar single quotes", "printf '%s' $'a\\tb\\nc'", "a\tb\nc"),
    Case("dollar quote literals", "printf '%s' $'$x `echo bad` \\n'", "$x `echo bad` \n"),
    Case("dollar quote apostrophe", "printf '%s' $'it\\'s'", "it's"),
    Case("dollar quote joined", "printf '%s' pre$'a\\tb'post", "prea\tbpost"),
    Case("dollar quote assignment", "x=$'a\\tb'; printf '%s' \"$x\"", "a\tb"),
    Case("dollar quote octal hex", "printf '%s' $'\\101\\x42'", "AB"),
    Case("dollar quote double quoted literal", '''printf '%s' "$'a\\tb'"''', "$'a\\tb'"),
    Case("brace literal", "printf '%s\\n' {a,b} {1..3}", "{a,b}\n{1..3}\n"),
    Case("tilde not from parameter", 'x="~"; printf "%s\\n" $x', "~\n"),
    Case("tilde quoted suffix", 'printf "%s\\n" ~/"a b"', "@HOME@/a b\n"),
    Case("tilde variable suffix", 'x=tail; printf "%s\\n" ~/$x', "@HOME@/tail\n"),
    Case("tilde assignment variable suffix", 'x=tail; y=~/$x; echo "$y"', "@HOME@/tail\n"),
    Case("tilde reflects HOME changes", 'for HOME in /one /two; do echo ~; done', "/one\n/two\n"),
    Case("tilde allows suffix glob", 'touch "$HOME/a"; printf "%s\\n" ~/*', "@HOME@/a\n"),
    Case("assignment parameter special", 'x=$#; echo "$x"', "0\n"),
    Case("mixed quote expansion", "x=value; printf '%s\\n' 'literal '$x", "literal value\n"),
    Case("single quote dollar literal", "x=value; printf '%s\\n' 'a$x'\"$x\"", "a$xvalue\n"),
]

ERRORS = [Case(name, code + '; echo survived', status=None) for name, code in [
    ("nounset", 'set -u; echo "$missing"'),
    ("nounset braced", 'set -u; echo "${missing}"'),
    ("nounset one letter braced", 'set -u; echo "${x}"'),
    ("nounset length", 'set -u; echo "${#missing}"'),
    ("nounset positional", 'set -u; echo "$1"'),
    ("nounset redirected", 'set -u; echo "$missing" > output'),
    ("readonly redirected", 'readonly x=one; x=two > output'),
    ("required expansion", 'unset x; echo "${x?required}"'),
    ("readonly assignment", 'readonly x=one; x=two'),
    ("readonly temporary assignment", 'readonly x=one; x=two echo bad'),
    ("readonly parameter assignment", 'readonly x; echo "${x:=value}"'),
    ("readonly arithmetic assignment", 'readonly x=1; echo "$((x=2))"'),
    ("special option", 'set -o no_such_option'),
    ("special export", 'export 1bad=value'),
    ("special readonly", 'readonly 1bad=value'),
    ("special unset", 'unset -Q x'),
    ("special break error", 'for x in a; do break bad; done'),
    ("special return error", 'f() { return bad; }; f'),
    ("missing dot", '. ./does-not-exist'),
    ("special redirection", 'export x=one > missing-directory/file'),
    ("colon redirection", ': > missing-directory/file'),
    ("eval syntax", "eval 'if'"),
    ("failed exec", 'exec ./does-not-exist'),
]] + [
    Case("command suppresses special error", 'command set -o no_such_option; echo survived', "survived\n", diagnostic="option"),
    Case("command export error", 'command export 1bad=value; echo survived', "survived\n", diagnostic="identifier"),
    Case("command dot error", 'command . ./does-not-exist; echo survived', "survived\n", diagnostic="file"),
    Case("eval ordinary nonzero", "eval 'false'; echo survived", "survived\n"),
    Case("nounset empty at", 'set -u; set --; printf "%s\\n" "$@"', "\n"),
    Case("nounset default allowed", 'set -u; echo "${missing-default}"', "default\n"),
    Case("nounset braced special", 'set -u; echo "${#}:${?}"; case ${-} in *u*) echo yes;; esac', "0:0\nyes\n"),
    Case("ordinary error continues", 'false; echo survived', "survived\n"),
    Case("ordinary redirection continues", 'echo bad > missing-directory/file; echo survived', "survived\n", diagnostic="file"),
    Case("fatal subshell isolated", '(set -u; echo "$missing"; echo bad); echo outer', "outer\n", diagnostic="parameter"),
    Case("conditional does not suppress nounset", 'if echo "$missing"; then :; fi; echo survived', "\nsurvived\n"),
    Case("fatal under conditional", 'set -u; if echo "$missing"; then :; fi; echo survived', status=None),
    Case("fatal under or", 'set -u; echo "$missing" || echo survived', status=None),
    Case("errexit", 'set -e; false; echo survived', status=1),
    Case("errexit conditional", 'set -e; if false; then :; fi; echo survived', "survived\n"),
    Case("errexit or", 'set -e; false || echo survived', "survived\n"),
    Case("errexit substitution", 'set -e; x=$(false; echo bad); echo survived', status=1),
]

RESTRICTIONS = [Case(name, code, status=None, diagnostic="POSIX") for name, code in [
    ("double bracket", '[[ 1 == 1 ]]'),
    ("function keyword", 'function f { :; }; f'),
    ("array literal", 'a=(one two)'),
    ("array element", 'a[0]=one'),
    ("array reference", 'a=one; echo "${a[0]}"'),
    ("array length", 'a=one; echo "${#a[0]}"'),
    ("append assignment", 'x=one; x+=two'),
    ("c style loop", 'for ((i=0;i<1;i++)); do :; done'),
    ("arithmetic command", '((1))'),
    ("select", 'select x in a b; do break; done'),
    ("substring", 'x=abc; echo "${x:1:2}"'),
    ("replacement", 'x=abc; echo "${x/a/z}"'),
    ("replace all", 'x=abc; echo "${x//a/z}"'),
    ("indirect", 'x=y; y=one; echo "${!x}"'),
    ("uppercase", 'x=abc; echo "${x^^}"'),
    ("lowercase", 'x=ABC; echo "${x,,}"'),
    ("here string", 'cat <<< hi'),
    ("process substitution", 'cat <(echo hi)'),
    ("pipe stderr", 'echo hi |& cat'),
    ("combined redirection", 'echo hi &> output'),
    ("auto background", 'true &^'),
    ("case retest", 'case a in a) : ;;& a) : ;; esac'),
    ("source", 'source ./fixture'),
    ("local", 'f() { local x=one; }; f'),
    ("declare", 'declare x=one'),
    ("typeset", 'typeset x=one'),
    ("builtin", 'builtin echo hi'),
    ("pushd", 'pushd .'),
    ("cjshopt", 'cjshopt'),
    ("globstar option", 'set -o globstar'),
    ("severity option", 'set -o errexit_severity warning'),
    ("severity long option", 'set --errexit-severity=warning'),
    ("read option", 'read -n 1 x < input'),
    ("test operator", '[ a == a ]'),
    ("array parameter redirected", 'echo "${x[0]}" > output'),
    ("parameter extension redirected", 'x=abc; echo "${x:1}" > output'),
    ("quoted double bracket command", "'[[' 1 == 1 ']]'"),
]]

BUILTINS = [
    Case("dot PATH", 'PATH="$PWD/bin:/usr/bin:/bin"; . fixture', "from-path\n"),
    Case("dot current directory excluded", 'PATH=/usr/bin:/bin; . local-fixture; echo bad', status=None),
    Case("dot explicit relative", '. ./local-fixture', "from-local\n"),
    Case("dot empty PATH component", 'PATH=:/usr/bin:/bin; . local-fixture', "from-local\n"),
    Case("dot return", '. ./return-fixture; echo "$?"', "7\n"),
    Case("type", 'type printf >/dev/null; echo "$?"', "0\n"),
    Case("command v", 'command -v printf', "printf\n"),
    Case("option restoration", 'set -fu; saved=$(set +o); set +fu; eval "$saved"; case $- in *f*u*|*u*f*) echo restored;; esac', "restored\n"),
    Case("hashall option", 'set -h; case $- in *h*) echo yes;; esac', "yes\n"),
    Case("notify option", 'set -b; case $- in *b*) echo yes;; esac', "yes\n"),
    Case("ignoreeof option", 'set -o ignoreeof; set +o ignoreeof'),
    Case("nolog option", 'set -o nolog; set +o nolog'),
    Case("read raw", 'read -r x < input; printf "%s\\n" "$x"', "a\\b c\n"),
    Case("test equality", '[ a = a ]; echo "$?"', "0\n"),
    Case("read fields", 'read -r x y < input; printf "%s:%s\\n" "$x" "$y"', "a\\b:c\n"),
    Case("export unset value", 'unset x; export x; echo "${x-unset}"', "unset\n"),
    Case("readonly", 'readonly x=one; echo "$x"', "one\n"),
    Case("unset", 'x=one; unset x; echo "${x-unset}"', "unset\n"),
    Case("shift", 'set -- a b c; shift 2; echo "$#:$1"', "1:c\n"),
    Case("getopts", 'set -- -a value; getopts a: x; echo "$x:$OPTARG:$OPTIND"', "a:value:3\n"),
    Case("unexported variable", "unset x; x=value; sh -c 'echo ${x-unset}'", "unset\n"),
    Case("export preserves attribute", "unset x; export x; x=value; sh -c 'echo $x'", "value\n"),
    Case("export output quoting", "x=\"a'b c\"; export x; saved=$(export -p | sed -n '/^export x=/p'); unset x; eval \"$saved\"; echo \"$x\"", "a'b c\n"),
    Case("export output unset", "unset x; export x; export -p | sed -n '/^export x$/p'", "export x\n"),
    Case("readonly output quoting", "readonly x=\"a'b c\"; readonly -p", "readonly x='a'\\''b c'\n"),
    Case("unset removes export attribute", "export x=one; unset x; x=two; sh -c 'echo ${x-unset}'", "unset\n"),
    Case("set includes local variables", "x='a b'; set | sed -n '/^x=/p'", "x='a b'\n"),
    Case("set severity text operand", 'set errexit_severity; echo "$1"', "errexit_severity\n"),
    Case("hashall disabled", 'set +h; case $- in *h*) echo bad;; *) echo yes;; esac', "yes\n"),
    Case("native flags absent", 'case $- in *B*) echo bad;; *) echo yes;; esac', "yes\n"),
]


class Runner:
    def __init__(self, binary, directory):
        self.binary = str(Path(binary).resolve())
        self.root = Path(directory)
        self.sh = self.root / "sh"
        self.sh.symlink_to(self.binary)
        self.total = self.failed = 0

    def check(self, name, entry, case, route="command", flags=(), stdin="", config=False):
        self.total += 1
        work = self.root / str(self.total)
        work.mkdir()
        home = work / "home"
        home.mkdir()
        (work / "bin").mkdir()
        (work / "bin" / "fixture").write_text('echo from-path\n')
        (work / "local-fixture").write_text('echo from-local\n')
        (work / "return-fixture").write_text('return 7\n')
        (work / "input").write_text('a\\b c\n')
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home),
               "LC_ALL": "C", "TERM": "dumb", "XDG_CACHE_HOME": str(work / "cache"),
               "XDG_CONFIG_HOME": str(work / "config")}
        argv = [self.binary, "--posix"] if entry == "flag" else [str(self.sh)]
        argv += ["--no-sh-warning"]
        if not config:
            argv += ["--no-config"]
        argv += list(flags)
        if route == "command":
            argv += ["-c", case.script]
        elif route == "file":
            script = work / "script"
            script.write_text(case.script + "\n")
            argv += [str(script)]
        elif route == "stdin":
            stdin = case.script + "\n"
        elif route == "eval":
            argv += ["-c", "eval " + shlex.quote(case.script)]
        elif route == "dot":
            (work / "script").write_text(case.script + "\n")
            argv += ["-c", ". ./script"]
        elif route == "argv":
            pass
        else:
            raise ValueError(route)
        expected = case.stdout.replace("@HOME@", str(home))
        try:
            process = subprocess.Popen(argv, cwd=work, env=env, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True, start_new_session=True)
            try:
                out, err = process.communicate(stdin, timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
                raise
            status_ok = (process.returncode != 0 and bool(err)) if case.status is None else process.returncode == case.status
            diagnostic_ok = case.diagnostic in err if case.diagnostic else (bool(err) if case.status is None else not err)
            if out != expected or not status_ok or not diagnostic_ok:
                raise AssertionError(f"status={process.returncode}, stdout={out!r}, stderr={err!r}; expected status={case.status}, stdout={expected!r}, diagnostic={case.diagnostic!r}")
        except (AssertionError, subprocess.TimeoutExpired) as error:
            self.failed += 1
            print(f"FAIL {entry}/{route}/{name}: {error}", flush=True)


def invocation(runner):
    cases = [
        ("errexit", ("-e", "-c", "false; echo bad"), "", Case("", "", status=1)),
        ("nounset", ("-u", "-c", 'echo "$missing"; echo bad'), "", Case("", "", status=None)),
        ("noglob", ("-f", "-c", "echo *"), "", Case("", "", "*\n")),
        ("allexport", ("-a", "-c", "x=value; sh -c 'echo $x'"), "", Case("", "", "value\n")),
        ("noclobber", ("-C", "-c", 'echo yes > output; echo bad > output; cat output'), "", Case("", "", "yes\n", diagnostic="output")),
        ("noexec", ("-n", "-c", "echo bad"), "", Case("", "")),
        ("long noexec after short disable", ("+n", "--no-exec", "-c", "echo bad"), "", Case("", "")),
        ("short disable after long noexec", ("--no-exec", "+n", "-c", "echo yes"), "", Case("", "", "yes\n")),
        ("long noexec after named disable", ("+o", "noexec", "--no-exec", "-c", "echo bad"), "", Case("", "")),
        ("named disable after long noexec", ("--no-exec", "+o", "noexec", "-c", "echo yes"), "", Case("", "", "yes\n")),
        ("verbose", ("-v", "-c", "echo body"), "", Case("", "", "body\n", diagnostic="echo body")),
        ("xtrace", ("-x", "-c", "echo body"), "", Case("", "", "body\n", diagnostic="echo body")),
        ("named option", ("-o", "errexit", "-c", "false; echo bad"), "", Case("", "", status=1)),
        ("plus options", ("-e", "+e", "-c", "false; echo yes"), "", Case("", "", "yes\n")),
        ("bundle", ("-ef", "-c", "echo *"), "", Case("", "", "*\n")),
        ("command inside bundle", ("-ceu", "false; echo bad"), "", Case("", "", status=1)),
        ("command identity", ("-c", 'echo "$0:$1:$#"', "name", "arg"), "", Case("", "", "name:arg:1\n")),
        ("command identity starts with flag", ("-c", 'echo "$0:$1:$#"', "-x", "arg"), "", Case("", "", "-x:arg:1\n")),
        ("long command identity", ("--command", 'echo "$0:$1:$#"', "-x", "arg"), "", Case("", "", "-x:arg:1\n")),
        ("stdin arguments", ("-s", "--", "arg"), 'echo "$1:$#"\n', Case("", "", "arg:1\n")),
        ("stdin read shares input", ("-s",), 'read -r x\ndata line\necho "$x"\n', Case("", "", "data line\n")),
        ("stdin errexit", ("-es",), 'false\necho bad\n', Case("", "", status=1)),
        ("stdin conditional errexit", ("-es",), 'false && echo bad\necho yes\n', Case("", "", "yes\n")),
        ("stdin blank preserves status", ("-s",), 'false\n\n# comment\n', Case("", "", status=1)),
        ("stdin continuation", ("-s",), 'printf "%s\\n" \\\nhello\n', Case("", "", "hello\n")),
        ("stdin flag-looking argument", ("-s", "--", "-e"), 'echo "$1:$#"\n', Case("", "", "-e:1\n")),
        ("unknown option", ("-Q",), "", Case("", "", status=None)),
        ("missing command", ("-c",), "", Case("", "", status=None)),
        ("empty command", ("-c", ""), "", Case("", "")),
        ("named noexec", ("-o", "noexec", "-c", "echo bad"), "", Case("", "")),
        ("noexec avoids expansion", ("-un", "-c", 'echo "$missing"'), "", Case("", "")),
        ("interactive special error", ("-i", "-c", "set -o no_such_option; echo survived"), "", Case("", "", "survived\n", diagnostic="option")),
    ]
    for entry in ("flag", "sh"):
        for name, flags, stdin, case in cases:
            runner.check(name, entry, case, "argv", flags, stdin)
        for case in RESTRICTIONS:
            if case.name in {"double bracket", "function keyword", "array literal", "c style loop", "arithmetic command", "select", "array reference", "substring", "replacement"}:
                runner.check("syntax only " + case.name, entry, case, "argv", ("-n", "-c", case.script))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("category", choices=("core", "expansion", "errors", "restrictions", "builtins", "invocation", "all"), default="all", nargs="?")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cjsh-posix-") as directory:
        runner = Runner(args.binary, directory)
        categories = {"core": CORE, "expansion": EXPANSION, "errors": ERRORS,
                      "restrictions": RESTRICTIONS, "builtins": BUILTINS}
        for category, cases in categories.items():
            if args.category not in (category, "all"):
                continue
            for entry in ("flag", "sh"):
                for route in ("command", "file", "stdin", "eval", "dot"):
                    for case in cases:
                        runner.check(case.name, entry, case, route)
        if args.category in ("invocation", "all"):
            invocation(runner)
        print(f"Total tests: {runner.total}\nPassed: {runner.total-runner.failed}\nFailed: {runner.failed}")
        if runner.failed:
            print(f"{runner.failed}/{runner.total} POSIX tests failed")
        else:
            print(f"All {runner.total} POSIX tests passed")
        return bool(runner.failed)


if __name__ == "__main__":
    raise SystemExit(main())
