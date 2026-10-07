<!--
  README.md

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

# Shell core

This directory implements cjsh's language, execution runtime, and shell-facing
interactive behavior in C++17. CMake builds the internal `cjsh_core` static library
and the `cjsh` executable. The library is an implementation detail, not a stable
embedding API. The line editor and terminal primitives live in
[`cjsh-isocline`](../cjsh-isocline/README.md).

See [CONTRIBUTING.md](../CONTRIBUTING.md) for build, lint, and review instructions,
and the [test guide](../tests/README.md) for regression-test placement.

## Source map

| Path under `src/` | Responsibility |
| --- | --- |
| `cjsh.cpp` | Process entry point, startup orchestration, and invocation dispatch |
| `core/` | Shell state, main loop, environment/startup files, signals, job control, prompts, hooks, browser, and agent mode |
| `parser/` | Tokenization, quote/delimiter tracking, command and pipeline parsing, and word expansion |
| `interpreter/` | Script/control-flow evaluation, functions, variable scopes, arithmetic, substitutions, and pattern matching |
| `exec/` | Process launch, pipelines, redirections, waits, execution errors, and execution-side job handling |
| `builtin/` | Builtin implementations and their option/help handling |
| `validation/` | Syntax and semantic diagnostics, including interactive incomplete-input checks |
| `completion/` | Completion context, providers, ranking, history, and suggestions |
| `highlighter/` | Token classification and interactive syntax highlighting |
| `utils/` | Shared command lookup, redirection, history-file, status, numeric, and string helpers |

## Following a command

Start with these entry points when tracing behavior:

1. `src/cjsh.cpp` handles invocation/startup; `core/main_loop.cpp` handles interactive input.
2. `Shell::execute()` in `core/shell.cpp` uses `Parser::parse_into_lines()` and enters
   `ShellScriptInterpreter::execute_block()`. Script-file execution has a corresponding
   `Shell::execute_script_content()` path.
3. The interpreter dispatches control structures, functions, and commands, using the parser
   and expansion evaluators as needed. Parsing, expansion, and evaluation interact; this
   is not a single context-free tokenize/expand/execute pass.
4. Simple prepared commands reach `Shell::execute_prepared_command()`, which dispatches
   builtins or the execution layer. Pipeline execution also enters `Exec` from the interpreter.
5. The execution and job-control layers manage children, descriptors, wait statuses, and
   terminal ownership. The interactive loop then recovers terminal state and presents the next prompt.

For a language change, inspect the relevant evaluator and parser together. For an editor
issue, also inspect the isocline implementation and PTY tests. The
[language compatibility inventory](../docs/reference/language-compatibility.md) is the
user-facing contract; do not assume every Bash behavior is intended cjsh behavior.

## Constraints to preserve

- **Quoting and expansion context:** preserve quote provenance and the distinctions between
  assignment, argument, pattern, and redirection contexts. Lexical caches must not cache
  results that depend on changing variables, aliases, IFS, filesystem contents, or dialect.
- **Process state:** builtins/functions can run in the parent shell or a child context.
  Check environment, variable scope, working-directory, and option restoration on all exits.
- **Descriptors:** preserve redirection order, close unused pipe ends and temporary descriptors,
  and restore parent descriptors after scoped redirections, including errors and early returns.
- **Signals and jobs:** keep signal-handler work async-signal-safe; do not add allocation,
  ordinary logging, or script evaluation to handlers. Preserve child signal dispositions,
  process groups, child reaping, and foreground terminal handoff/recovery.
- **Shared state:** `g_shell` and other process-wide state mean the runtime is not a generally
  reentrant or thread-safe library. New background work must not assume independent shell instances.
- **Interactive versus scripting behavior:** validation/highlighting must not accidentally
  execute user commands. Keep startup, native/POSIX dialect, noninteractive, and syntax-only
  paths covered when changing dispatch or expansion.

Prefer small changes with focused regressions over broad rewrites. Parser, interpreter,
execution, and terminal-sensitive changes should also be tested with the Debug/ASan preset
as described in the contributor guide.
