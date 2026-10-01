<!--
  posix-mode.md

  This file is part of cjsh, CJ's Shell

  MIT License

  Copyright (c) 2026 Caden Finley

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
-->

# POSIX mode

Start with `cjsh --posix` or invoke the executable with the name `sh`. The language
target is POSIX.1-2024 (Issue 8). This is a compatibility and extension-restriction
mode, not a claim of formal POSIX certification. Supplying `POSIXLY_CORRECT` alone
does not select it, and there is no runtime `set -o posix` switch.

```sh
cjsh --posix -eu -c 'printf "%s\n" "$1"' script-name argument
cjsh --posix -s -- argument < script.sh
cjsh --posix -n script.sh
```

In this mode, short invocation flags follow `sh`: `-a`, `-b`, `-C`, `-e`, `-f`,
`-h`, `-m`, `-n`, `-u`, `-v`, and `-x` set shell options; `+` clears them.
`-o NAME` and `+o NAME` select named options. `-s` reads commands from standard
input and passes remaining operands as positional parameters. `-c` uses the next
operand as its command string, followed by an optional `$0` and positional
parameters. Native conveniences remain accessible through long options such as
`--version`, `--help`, `--no-colors`, `--minimal`, and `--secure`.

Supported standard features include tilde expansion (also in assignments),
`$'…'` quoting, `pipefail`, and case fall-through with `;&`. These last three are
part of the 2024 standard. `type` is available as an XSI utility. `set +o` produces
commands that restore option state; `set`, `export -p`, and `readonly -p` quote
values for shell input. `hashall` and `nolog` accept their standardized settings;
they do not change cjsh's lookup or history implementation. The optional User
Portability Utilities `vi` line-editing option is not implemented.

`notify` (`set -b`) enables job notifications while waiting for interactive input.
Without it, background notifications wait until the next prompt. `ignoreeof`
requires `exit` to leave an interactive shell. Noninteractive background jobs do
not print interactive job notifications.

Noninteractive syntax, expansion, assignment, and special-builtin errors terminate
the current shell environment. Ordinary nonzero statuses remain ordinary command
results unless `errexit` applies. `command` removes a builtin's special status;
errors inside `eval` or a dot-sourced file still follow the rules of those commands.
Subshell errors remain isolated from the parent. Dot (`.`) searches `PATH` for
names without a slash, including readable files without execute permission.

Aliases expand before command operators are parsed, including chained aliases and
the next word after an alias ending in a blank. Function definitions retain the
aliases present when their bodies are read. A function body can be any supported
POSIX compound command; `set --` and `shift` affect its arguments until it returns.
`unset -f` removes functions. `read` preserves the remaining separators in its
last variable and assigns partial input on EOF while returning failure. `trap -p`
prints restorable condition states, and an explicit `exit` in an exit trap selects
the final exit status.

The strict language policy rejects arrays, append assignments, arithmetic commands
(`((…))`), C-style `for`, `select`, the `function` keyword, `[[…]]`, process
substitution, here-strings, `|&`, combined output redirections, `;;&`, and cjsh's
auto-background operators. Parameter slicing, replacement, indirect expansion,
and case conversion are rejected. Brace expansion stays literal; extglob and
recursive globstar are disabled. Extension builtins and options such as `source`,
`local`, `declare`, `read -n`, `test ==`, and `errexit_severity` are unavailable.
Quoted text and here-document data retain their literal meaning.

Startup uses POSIX login profiles and interactive `ENV`; see the
[startup rules](../getting-started/what-to-know.md). `--no-config` disables automatic
startup files for isolated script execution.

The dedicated tests run positive behavior, fatal errors, restrictions, builtin
semantics, and invocation options through both mode entry points. Script fixtures
run as command strings, files, standard input, `eval`, and dot sourcing. Separate
PTY tests exercise interactive error recovery, EOF handling, notification timing,
and retained wait statuses. Run them after building:

```sh
ctest --test-dir build/release -L posix --output-on-failure
```

Expectations are based on the [shell language](https://pubs.opengroup.org/onlinepubs/9799919799/utilities/V3_chap02.html),
[`sh`](https://pubs.opengroup.org/onlinepubs/9799919799/utilities/sh.html), and
[`set`](https://pubs.opengroup.org/onlinepubs/9799919799/utilities/set.html)
specifications. Passing the regression suite is not an exhaustive conformance assessment.
