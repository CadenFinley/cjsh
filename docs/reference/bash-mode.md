<!--
  bash-mode.md

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

# Bash dialect

The experimental Bash dialect targets Bash 5.3 script behavior. It is an incremental
compatibility mode, not a claim that arbitrary Bash scripts or `.bashrc` files work.

```sh
cjsh --bash script.sh
cjsh --dialect bash -c 'false | true; printf "%s\n" "${PIPESTATUS[@]}"'
cjshopt dialect bash
cjshopt dialect status
cjshopt dialect cjsh
```

`cjshopt dialect cjsh|posix|bash` and the invocation flags share one setting. The
default is `cjsh`; invocation as `sh` selects `posix`. Explicit flags are processed
in order, so the last dialect flag wins. Runtime changes affect subsequent execution
and word expansion; they do not reparse an already-read compound command or reload
startup files. Use a separate command or `eval` when newly selected syntax needs to
be parsed. Subshells inherit the selection and changes there do not affect the parent.

Explicit shell-option choices survive dialect changes. Unspecified defaults follow
the dialect. Leaving POSIX mode restores the native feature settings that it disabled.
`cjshopt dialect` is available in POSIX mode so the selection remains reversible.

Files with a Bash shebang, such as `#!/bin/bash` or `#!/usr/bin/env bash`, automatically
use the Bash dialect when cjsh reads or sources them. The previous dialect is restored
when the file finishes, including after `return` or a syntax error. Bash scripts dispatched
as external commands run in a separate cjsh process, preserving their arguments,
redirections, and exit status. `#!/usr/bin/env -S bash` is also recognized; interpreter
options such as `-e` are passed to cjsh for executable scripts. This behavior does not
depend on extension-based interpreter selection.

POSIX mode never switches dialect based on a shebang. Files read by `cjsh --posix` or
`.` retain POSIX syntax and restrictions. Executable commands continue to use their
shebang interpreter through normal external execution.

## Options and flag migration

Short flags now have consistent meanings in every dialect:

| Flag | Meaning | Former native shortcut |
| --- | --- | --- |
| `-C` | noclobber | Use `--no-colors` |
| `-v` | verbose | Use `--version` |
| `-h` | hashall | Use `--help` |
| `-m` | monitor | Use `--minimal` |
| `-s` | read stdin | Use `--secure` |
| `-O NAME` / `+O NAME` | enable/disable a shopt option | Use `--no-completions` |
| `-H` / `+H` | enable/disable history expansion | Use `--no-history-expansion` |

Use `set` for execution options and `shopt` for extended language options:

```sh
set -o pipefail
shopt -s extglob globstar
shopt -u huponexit
shopt -p extglob
shopt -q globstar
```

The `shopt` registry contains `autocd`, `extglob`, `globstar`, `huponexit`,
`expand_aliases`, and `inherit_errexit`. `-s`, `-u`, `-p`, `-q`, and `-o` are supported;
`shopt -o` selects the `set` option namespace. Unknown options fail. `shopt` is
available in the cjsh and Bash dialects and rejected in strict POSIX mode.

Replace `cjshopt extglob on|off|status` with `shopt -s|-u|-p extglob`, and replace
`set -o/+o globstar` and `set -o/+o huponexit` with `shopt -s/-u`. Editor and UI
configuration continues to use `cjshopt`.

`shopt -s autocd` lets an interactive shell treat a directory name as an implicit
`cd` command. It defaults to off in Bash mode and on in the native dialect;
`shopt -u autocd` disables it. Bash mode also accepts explicit relative directory
paths such as `./projects`. Builtins, functions, aliases, and executable commands
take precedence over a directory with the same name. Enabling the option in a
noninteractive shell does not make directory names executable.

## Initial behavior differences

Only the Bash dialect enables the following compatibility changes:

- `PIPESTATUS` is an indexed array rather than cjsh's space-separated scalar.
- Quoted array `[@]` expansion preserves individual and empty arguments.
- Command substitutions clear `errexit` unless `shopt -s inherit_errexit` is set.
- Noninteractive alias expansion defaults to off; `shopt -s expand_aliases` enables it.
- `[[ value =~ expression ]]` uses POSIX extended regular expressions and publishes
  captures in `BASH_REMATCH`. Invalid expressions return status 2.
- Native errexit severity settings do not change Bash-mode exit decisions.
- Automatic directory changes default to off; `shopt -s autocd` enables them interactively.
- Extension-based interpreter selection is disabled.
- `huponexit` defaults to off.

The native dialect retains its previous defaults and language behavior. The flag
standardization and option-interface migration apply to both dialects.

## Startup and boundaries

Bash mode uses cjsh's native startup files and `CJSH_ENV`. It does not automatically
load `.bashrc`, `.bash_profile`, or `BASH_ENV`, and changing dialect at runtime does
not source additional files. POSIX mode continues to use `/etc/profile` and
`~/.profile` for login shells and `ENV` for interactive shells.

Further work includes the wider `shopt` matrix (`nullglob`, `failglob`, `dotglob`,
`lastpipe`, etc.), Bash's `set -o posix` behavior, parse-time alias/extglob rules,
remaining array and quoting edge cases, special Bash variables, trap inheritance,
programmable completion, and Readline bindings. The existing strict POSIX dialect
is distinct from Bash's own POSIX option. Bash version variables are not fabricated.

## Verification

`tests/bash/test_bash_mode.py` compares the same fixtures through `-c`, files, stdin,
`eval`, and sourcing. It checks stdout, status, diagnostics, and filesystem effects
in isolated environments. Native-mode tests protect existing cjsh behavior and
exercise runtime mode changes. The differential suite requires a Bash 5.3 reference;
other versions produce an explicit skip rather than silently changing the oracle.

```sh
CJSH_BASH_REFERENCE=/path/to/bash-5.3 ctest --test-dir build/release -L bash --output-on-failure
```

`CJSH_BASH_VERSION` can pin a patch level, such as `5.3.20`. Linux x86_64 CI builds
the Bash 5.3 release as its reference.
