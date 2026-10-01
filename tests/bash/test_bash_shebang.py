#!/usr/bin/env python3

# test_bash_shebang.py
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

"""Bash shebang dispatch, temporary dialect changes, and POSIX isolation."""

import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile


def main():
    cjsh = str(Path(sys.argv[1]).resolve())
    total = failures = 0
    with tempfile.TemporaryDirectory(prefix="cjsh-bash-shebang-") as tmp:
        root = Path(tmp)
        env = {"HOME": tmp, "PATH": f"{tmp}:/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb"}

        def fixture(name, content, executable=True):
            target = root / name
            target.write_text(content)
            target.chmod(0o755 if executable else 0o644)
            return shlex.quote(str(target))

        def check(name, arguments, stdout, status=0, diagnostic=""):
            nonlocal total, failures
            total += 1
            proc = subprocess.Popen([cjsh, "--no-config", *arguments], cwd=tmp, env=env,
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, start_new_session=True)
            try:
                actual_stdout, stderr = proc.communicate(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate()
                failures += 1
                print(f"FAIL {name}: timed out")
                return
            if (proc.returncode, actual_stdout) != (status, stdout) or diagnostic not in stderr:
                failures += 1
                print(f"FAIL {name}: {(proc.returncode, actual_stdout, stderr)!r}; "
                      f"expected {(status, stdout, diagnostic)!r}")

        probe = 'cjshopt dialect status\na=(one "two words"); printf "<%s>\\n" "${a[@]}"\n'
        expected = "bash\n<one>\n<two words>\n"
        for name, shebang in (
            ("absolute", "#!/bin/bash"),
            ("env", "#!/usr/bin/env bash"),
            ("env_split", "#!/usr/bin/env -S bash"),
            ("spaced", "#! \t/usr/local/bin/bash\r"),
        ):
            path = fixture(name, shebang + "\n" + probe)
            check(name + " file", [str(root / name)], expected)
            for source in ("source", "."):
                check(name + " " + source, ["-c", f"{source} {path}; cjshopt dialect status"],
                      expected + "cjsh\n")
            check(name + " command", ["-c", f"{path}; cjshopt dialect status"], expected + "cjsh\n")
            check(name + " bash caller", ["--bash", "-c", f". {path}; cjshopt dialect status"],
                  expected + "bash\n")

        path = fixture("native.cjsh", "#!/bin/bash\n" + probe, executable=False)
        check("cjsh extension quick dispatch", ["-c", f"{path}\ncjshopt dialect status"], expected + "cjsh\n")
        check("cjsh extension pipeline parsing", ["-c", f"{path}; cjshopt dialect status"], expected + "cjsh\n")

        path = fixture("nonexecutable", "#!/bin/bash\n" + probe, executable=False)
        check("nonexecutable command", ["-c", path], "", 126)
        check("nonexecutable source", ["-c", f". {path}; cjshopt dialect status"], expected + "cjsh\n")

        path = fixture("with arguments.sh", '#!/usr/bin/env bash\ncjshopt dialect status\n'
                       'printf "%s|%s|%s|%s\\n" "$0" "$#" "$1" "$2"\n')
        arg_output = f"bash\n{root / 'with arguments.sh'}|2|two words|--bash\n"
        check("command arguments", ["-c", f"{path} 'two words' --bash"], arg_output)
        check("exec arguments", ["-c", f"exec {path} 'two words' --bash"], arg_output)
        check("PATH command", ["-c", "absolute; cjshopt dialect status"], expected + "cjsh\n")
        check("PATH exec", ["-c", "exec absolute"], expected)
        check("pipeline", ["-c", "absolute | cat; cjshopt dialect status"], expected + "cjsh\n")
        check("background", ["-c", "absolute > output & wait $!; cat output; cjshopt dialect status"],
              expected + "cjsh\n")
        check("redirection", ["-c", "absolute > output; cat output; cjshopt dialect status"], expected + "cjsh\n")
        check("extension dispatch disabled", ["--no-script-extension-interpreter", "-c", "absolute"], expected)
        check("bash command caller", ["--bash", "-c", "absolute; cjshopt dialect status"], expected + "bash\n")

        path = fixture("isolated", '#!/bin/bash\nx=child\nexit 7\n')
        check("command isolation and exit", ["-c", f"x=parent; {path}; printf '%s:%s\\n' \"$?\" \"$x\"; cjshopt dialect status"],
              "7:parent\ncjsh\n")
        path = fixture("returns", "#!/bin/bash\ncjshopt dialect status\nreturn 7\necho bad\n")
        check("source return restores dialect", ["-c", f". {path}; echo $?; cjshopt dialect status"], "bash\n7\ncjsh\n")
        path = fixture("invalid", "#!/bin/bash\nif\n")
        check("syntax failure restores dialect", ["-c", f". {path}; echo $?; cjshopt dialect status"], "2\ncjsh\n")
        check("syntax failure child", ["-c", path], "", 2)
        path = fixture("changed_dialect", "#!/bin/bash\ncjshopt dialect cjsh\n")
        check("bash caller restored after explicit change", ["--bash", "-c", f". {path}; cjshopt dialect status"], "bash\n")

        inner = fixture("inner", "#!/bin/bash\ncjshopt dialect status\nreturn 4\n")
        outer = fixture("outer", f"#!/bin/bash\ncjshopt dialect status\n. {inner}\necho $?\ncjshopt dialect status\n")
        check("nested source restores each dialect", ["-c", f". {outer}; cjshopt dialect status"], "bash\nbash\n4\nbash\ncjsh\n")
        check("native array and options restored", ["-c", "shopt -s expand_aliases inherit_errexit; . ./absolute; "
              "shopt -q expand_aliases inherit_errexit; echo $?; false | true; "
              'printf "<%s>\\n" "${PIPESTATUS[@]}"'], expected + "0\n<1 0>\n")

        for name, shebang in (("errexit", "#!/bin/bash -e"), ("split_errexit", "#!/usr/bin/env -S bash -e")):
            path = fixture(name, shebang + "\nfalse\necho bad\n")
            check(name + " command option", ["-c", path], "", 1)

        for name, content in (
            ("plain", "cjshopt dialect status\n"),
            ("sh", "#!/bin/sh\ncjshopt dialect status\n"),
            ("similar", "#!/bin/notbash\ncjshopt dialect status\n"),
            ("argument", "#!/bin/echo bash\ncjshopt dialect status\n"),
            ("late", "# comment\n#!/bin/bash\ncjshopt dialect status\n"),
        ):
            path = fixture(name, content)
            check(name + " does not switch dialect", [str(root / name)], "cjsh\n")
            check(name + " source does not switch dialect", ["-c", f". {path}"], "cjsh\n")

        check("shebang comment in command string", ["-c", "#!/bin/bash\ncjshopt dialect status"], "cjsh\n")
        fixture("empty", "#!/bin/bash", executable=False)
        check("shebang without newline", ["empty"], "")

        path = fixture("posix_probe", '#!/bin/bash\ncjshopt dialect status\nprintf "%s\\n" {one,two}\n')
        check("posix file remains posix", ["--posix", "posix_probe"], "posix\n{one,two}\n")
        check("posix source remains posix", ["--posix", "-c", f". {path}; cjshopt dialect status"],
              "posix\n{one,two}\nposix\n")
        check("runtime posix source remains posix", ["-c", f"cjshopt dialect posix; . {path}; cjshopt dialect status"],
              "posix\n{one,two}\nposix\n")
        check("posix rejects Bash syntax in file", ["--posix", "absolute"], "", 2, "POSIX")
        check("posix rejects Bash syntax in source", ["--posix", "-c", ". ./absolute"], "", 2, "POSIX")
        path = fixture("external", '#!/bin/bash\nprintf "%s\\n" external-bash\n')
        check("posix executable uses shebang interpreter", ["--posix", "-c", f"{path}; cjshopt dialect status"],
              "external-bash\nposix\n")
        check("posix exec uses shebang interpreter", ["--posix", "-c", f"exec {path}"], "external-bash\n")

    if failures:
        print(f"{failures}/{total} Bash shebang tests failed")
    else:
        print(f"All {total} Bash shebang tests passed")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
