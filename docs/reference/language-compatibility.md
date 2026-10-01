<!--
  language-compatibility.md

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

# Language Compatibility Inventory

CJSH is a POSIX-focused shell with selected Bash and Zsh conveniences. It does not claim formal
POSIX certification, complete Bash compatibility, or complete Zsh compatibility. This inventory
is the source of truth for the extensions that are intentionally supported.

## Current support

| Area | Status | CJSH behavior |
| --- | --- | --- |
| Portable shell syntax | Broad support | Functions, pipelines, redirections, substitutions, control flow, traps, job control, and the standard parameter operators are covered by the shell test suite. `--posix` disables or rejects CJSH extensions where practical. |
| Indexed arrays | Supported extension | `declare -a`, indexed assignment, `${array[index]}`, `${array[@]}`, `${!array[@]}`, and `${#array[@]}` are supported. |
| Associative arrays | Supported extension | `declare -A map=([key]=value)`, element assignment/unset, key/value expansion, length expansion, local scope, and reusable `declare -p` output are supported. |
| Namerefs | Supported extension | `declare -n`/`typeset -n`/`local -n` can reference scalars or array elements. Reads, assignments, `${!ref}`, normal `unset`, and `unset -n` follow Bash-style reference behavior. |
| Coprocesses | Supported extension | Simple commands, pipelines, unnamed compound commands, and the recommended `coproc NAME { command; }` form run asynchronously. Descriptors are published in `NAME[0]`/`NAME[1]`, the PID in `NAME_PID`, dynamic descriptor redirection is supported, and `read -u` can consume the output. |
| Extended globs | Supported, opt-in | Run `shopt -s extglob` to enable `?()`, `*()`, `+()`, `@()`, and `!()` in pathname expansion, `[[ … ]]`, `case`, and parameter patterns. It is off by default and forced off in POSIX mode. |
| Case continuation | POSIX.1-2024 and extension | `;&` executes the next clause body without testing its pattern and is available in POSIX mode. `;;&` continues testing subsequent patterns and is rejected in POSIX mode. |
| Brace range strides | Supported extension | Numeric and character ranges accept `{start..end..stride}`. Direction is inferred, and numeric zero padding is preserved. |
| Pattern replacement | Supported extension | `${parameter/pattern/replacement}` uses the leftmost-longest wildcard match; `//` replaces all matches and `/#`/`/%` anchor a single replacement. Escaped slashes, character classes, and extglobs are supported. |

`for` and `select` require a literal variable name and an unquoted `in` keyword before
an explicit list. Malformed headers such as `for i n 1 2; do ...; done` report a syntax
error with status 2. Omitting the list (`for i; do ...; done`) uses positional parameters;
an explicit empty `for` list runs no iterations. Loop lists use normal word expansion,
including quoted literals and brace ranges with surrounding or following words.

One-line function bodies are recognized by interactive validation, including CJSH's
compact `function sayhello {echo hello}` form. For conventional shell syntax, use
`function sayhello { echo hello; }`; for POSIX scripts, use
`sayhello() { echo hello; }`.

Function headers may put the opening brace on the next line. Function bodies and
command groups support mixed inline and multiline layouts, with quoted and escaped
delimiters preserved as literal text. Syntax-only mode (`-n`) rejects unfinished
function bodies and control-flow blocks.

Interactive validation treats `then`, `do`, `fi`, `done`, and `esac` as control
keywords only where a command can begin. Using those words as arguments, in
quotes, or in comments does not complete an unfinished block.

## Configuration

Language options use `shopt`; editor configuration and dialect selection use `cjshopt`:

```bash
shopt -s extglob
shopt -u extglob
shopt -p extglob
cjshopt dialect bash
```

Add the desired command to `~/.cjshrc` to apply it to future interactive sessions.

## Known boundaries

- [`--posix`](posix-mode.md) targets POSIX.1-2024, with explicit extension restrictions and dedicated mode tests. It is not a standards certification.
- Only one coprocess connection is retained by the parent shell at a time. Starting another
  coprocess closes the parent's previous coprocess descriptors.
- Associative-array enumeration is deterministic and key-sorted in CJSH. Scripts should not depend
  on Bash or Zsh producing the same enumeration order.
- The experimental [Bash dialect](bash-mode.md) implements a tested subset of Bash 5.3
  behavior. The initial `shopt` registry is deliberately limited; unknown options fail.
- Zsh-only parameter modifiers, glob qualifiers, option dialect, modules, and completion language
  are outside the Bash-style compatibility surface listed above.

For maximally portable scripts, continue to target POSIX `sh` syntax and test with `cjsh --posix`.
