#!/usr/bin/env python3

# test_syntax_regressions.py
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

"""Compare editor validation, syntax-only mode, and execution across syntax layouts.

Fixtures carry expected output and status so tests do not depend on another shell.
"""

import json
from pathlib import Path
import subprocess
import sys


def control_flow_layout_cases():
    blocks = [
        ("for", "for i in one two", "do", 'echo "$i"', "done", "one\ntwo\n", ""),
        ("arithmetic for", "for ((i=1; i<=2; i++))", "do", 'echo "$i"', "done", "1\n2\n", ""),
        ("while", "while true", "do", "echo ok; break", "done", "ok\n", ""),
        ("until", "until false", "do", "echo ok; break", "done", "ok\n", ""),
        ("select", "select i in one", "do", 'echo "$i"; break', "done", "one\n", "1\n"),
        ("if", "if true", "then", "echo ok", "fi", "ok\n", ""),
        ("case", "case x in", "x)", "echo ok;", "esac", "ok\n", ""),
    ]
    for name, header, opening, body, closing, output, stdin in blocks:
        for header_break in (False, True):
            for body_break in (False, True):
                separator = "\n" if header_break else (" " if name == "case" else "; ")
                script = header + separator + opening + "\n" + body
                script += ("\n" if body_break else "; ") + closing
                yield {
                    "name": f"{name}/mixed lines/{header_break}/{body_break}",
                    "script": script, "complete": True, "status": 0,
                    "stdout": output, "stdin": stdin,
                }
    for name, script, output in (
        ("reported loop", "for i in {1..100}; do\n echo $i; done",
         "".join(f"{i}\n" for i in range(1, 101))),
        ("nested loops", 'for i in one; do\n for j in two; do\n echo "$i $j"; done; done',
         "one two\n"),
        ("nested conditional", "for i in one; do\n if true; then\n echo ok; fi; done", "ok\n"),
        ("inline inner loop", "for i in one; do for j in two; do echo ok; done\ndone", "ok\n"),
        ("later opener", "echo first; for i in one; do\n echo second; done", "first\nsecond\n"),
        ("else", "if false; then\n echo wrong; else echo ok; fi", "ok\n"),
        ("elif", "if false; then\n echo wrong; elif true; then echo ok; fi", "ok\n"),
        ("elif and else", "if false; then\n echo wrong; elif false; then echo wrong; else echo ok; fi",
         "ok\n"),
        ("nested branches", "if true; then\n if false; then echo wrong; else echo ok; fi; fi", "ok\n"),
        ("nested case", "for i in one; do\n case x in x) echo ok;; esac; done", "ok\n"),
        ("case keywords", "case done in\n done|for|if) echo ok;; esac", "ok\n"),
        ("case pattern on a separate line", "case done in\n done )\n echo ok;; esac", "ok\n"),
        ("function body", "f() {\n for i in one; do\n echo ok; done\n}\nf", "ok\n"),
        ("loop words", "for i in do done if fi; do\n echo $i; done", "do\ndone\nif\nfi\n"),
        ("literal delimiters", 'for i in one; do\n echo "done; fi"; echo \\done; done',
         "done; fi\ndone\n"),
        ("comment", "for i in one; do # done\n echo ok; done # for", "ok\n"),
        ("substitution", 'for i in one; do\n echo "$(echo done)"; done', "done\n"),
        ("literal opener", "echo for item\necho while false", "for item\nwhile false\n"),
        ("conditional tail", "if true; then\n echo first; fi; echo second", "first\nsecond\n"),
        ("skipped loop", "false && for i in one; do\n echo wrong; done\necho ok", "ok\n"),
        ("logical conditional", "true && if false; then\n echo wrong; else echo ok; fi", "ok\n"),
        ("brace body", "for i in one; do\n { echo ok; }; done", "ok\n"),
        ("subshell body", "for i in one; do\n (echo ok); done", "ok\n"),
        ("function definition body", "for i in one; do\n f() { echo ok; }; done\nf", "ok\n"),
    ):
        yield {"name": name, "script": script, "complete": True, "status": 0, "stdout": output}
    for script in (
        "for i in one; do for j in two; do echo ok; done",
        "if true; then if true; then echo ok; fi",
        "for i in one; do\n echo done",
        "for i in one; do\n echo 'done'",
        "for i in one; do\n echo ok # done",
        "for i in one; do\n echo \"; done\"",
        "for i in one; do\n echo $(echo done)",
        "for i in one; do\n if true; then echo ok; fi",
    ):
        yield {"name": f"incomplete/{script}", "script": script, "complete": False}


def main():
    binary, validator = sys.argv[1:3]
    cases = json.loads(Path(__file__).with_name("syntax_regression_cases.json").read_text())["cases"]
    cases.extend(control_flow_layout_cases())
    failures = 0
    for case in cases:
        problems = []
        try:
            validation = subprocess.run(
                [validator, case["script"]], capture_output=True, text=True, timeout=5,
                check=True,
            ).stdout
            more = "MORE 1" in validation
            syntax_errors = [line for line in validation.splitlines()
                             if line.startswith(("ERROR SYN", "ERROR FUNC", "ERROR POSIX"))]
            if more == case["complete"]:
                problems.append(f"continuation requested: {more}")
            if case["complete"] and syntax_errors:
                problems.extend(syntax_errors)
            if case["complete"]:
                syntax_only = subprocess.run(
                    [binary, "--no-config", "-nc", case["script"]],
                    input="", capture_output=True, text=True, timeout=5,
                )
                if syntax_only.returncode != 0 or syntax_only.stdout:
                    problems.append(f"syntax-only mode rejected or executed valid input: {syntax_only!r}")
            mode = "-c" if case["complete"] else "-nc"
            actual = subprocess.run(
                [binary, "--no-config", mode, case["script"]],
                input=case.get("stdin", ""), capture_output=True, text=True, timeout=5,
            )
            if case["complete"]:
                if actual.returncode != case["status"] or actual.stdout != case["stdout"]:
                    problems.append(
                        f"expected status={case['status']}, stdout={case['stdout']!r}; "
                        f"got status={actual.returncode}, stdout={actual.stdout!r}, "
                        f"stderr={actual.stderr!r}")
            elif actual.returncode == 0:
                problems.append("syntax-only mode accepted incomplete input")
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
            problems.append(str(error))
        if problems:
            failures += 1
            print(f"FAIL: {case['name']}: " + " | ".join(problems))
    print(f"Total tests: {len(cases)}")
    print(f"Passed: {len(cases) - failures}")
    print(f"Failed: {failures}")
    if failures:
        print(f"{failures}/{len(cases)} syntax regression tests failed")
    else:
        print(f"All {len(cases)} syntax regression tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
