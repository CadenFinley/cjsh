<!--
  non-posix-features.md

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

# POSIX+ Interactive Features

CJ's Shell aims to behave like `sh` when running scripts while offering an opinionated interactive
experience that goes well beyond the POSIX specification. This document calls out the major
non-POSIX features so you know what is different, how to tune it, and how to disable it when
necessary.

## Prompt System & Visual Enhancements

- **Markup-driven prompts** – `PS1`, `RPS1`, and `PS2` accept BBCode-style markup. Tags such as
  `[b]`, `[color=#ff69b4]`, `[ic-hint]`, and `[bgcolor=ansi-darkgray]` are specific to CJSH's
  integration with the isocline editor.
- **Prompt spacing control** – `cjshopt prompt-newline` can force a blank line after every command,
  regardless of whether the command emitted one itself.
- **Partial-line guard marker** – if output leaves the cursor mid-line, cjsh preserves that partial
  line, prints `PROMPT_EOL_MARK` (`%` for non-root, `#` for root by default), and forces terminal
  wrapping before drawing the next prompt.
- **Right-prompt cursor tracking** – `cjshopt right-prompt-follow-cursor` lets the inline right
  prompt move with the active cursor row, something stock POSIX shells do not support.
- **Dynamic title line** – The introductory banner and title-line management (enabled by default)
  can be disabled with `--no-titleline`.

See [Prompt Markup and Styling](../themes/thedetails.md) for the full markup reference.

## Interactive Editing Extensions

- **Multiline editor** – Automatic continuation detection, optional relative or absolute line
  numbers, inline hints, and visible whitespace markers are provided by isocline and configured via
  `cjshopt` commands (`multiline`, `line-numbers`, `hint`, `visible-whitespace`, etc.).
- **Syntax highlighter** – Real-time token classification and styling via `cjshopt style_def`.
  Highlight categories such as `unknown-command`, `ic-linenumber-current`, and `ic-hint` are
  non-standard.
- **Key binding profiles** – An Emacs-inspired default profile, a non-modal `vim` profile that adds
  `Alt+H/J/K/L/W` navigation, and user-defined command bindings using `cjshopt keybind` and
  `cjshopt keybind ext`.
- **Mouse-assisted editing** – `cjshopt mouse-clicking` distinguishes hard-disabled `all-off`,
  interactive-menu-only `off`, and prompt-enabled `simple`/`smart` modes;
  `cjshopt mouse-clicking-status-line` controls the status indicator. Clicking to move the cursor or
  pick menu entries is beyond POSIX scope.
- **Fish-style abbreviations** – `abbr` and `unabbr` provide inline text expansions, a feature not
  present in POSIX shells.
- **Idle hooks** – `cjshopt idle-timeout` can pause the editor after terminal inactivity and run a
  registered `idle` hook in the foreground, then restore the pending buffer and cursor.

## POSIX+ Scripting Syntax

- **Brace expansion** – Expand comma-separated terms or ranges before globbing.
  - Example: `echo {alpha,beta}` → `alpha beta`
  - Strides and zero padding are supported: `echo {01..10..3}` → `01 04 07 10`
- **Arrays and namerefs** – `declare -a`, `declare -A`, and `declare -n` provide indexed arrays,
  associative arrays, and reference variables.
- **Extended globs** – `?()`, `*()`, `+()`, `@()`, and `!()` work in pathname expansion,
  `[[ … ]]`, `case`, and parameter patterns after `cjshopt extglob on`.
- **Case fall-through** – `;&` executes the following clause body and `;;&` resumes pattern
  testing at the following clause.
- **Coprocesses** – `coproc command` and `coproc NAME { command; }` expose two-way descriptors in
  `COPROC`/`NAME` and a waitable `COPROC_PID`/`NAME_PID`. `read -u fd` reads from a descriptor.
- **Here-strings** – Feed a single string into a command's stdin.
  - Example: `grep foo <<< "foo bar"`
- **Process substitution** – Treat command output/input as a file-like path.
  - Example: `diff <(sort a.txt) <(sort b.txt)`

These syntax extensions are available in both scripts and interactive sessions. History expansion
remains interactive-only by default; see the History section below.
See the [Language Compatibility Inventory](language-compatibility.md) for exact support and known
differences from Bash and Zsh.

## Completion Engine

- **Fuzzy matching and spell correction** – Configurable through `cjshopt completion-case`,
  `cjshopt completion-spell`, `cjshopt completion-spell-enter`, `cjshopt completion-preview`,
  `cjshopt completion-click-accept`, and
  `cjshopt menu-highlighting`.
- **Man-page scraping** – `generate-completions` and on-demand parsing of `man` pages populate a
  cache under `~/.cache/cjsh/generated_completions/` for rich option and subcommand help.
- **Inline preview pane & source annotations** – Completion menus display descriptions, origins
  (history, PATH, builtin), and exit-status tags; these are CJSH-specific niceties.
- **Pointer navigation in menus** – Completion and history-search menus accept mouse-wheel
  scrolling plus click selection.

Consult the [Completion Authoring Guide](completions.md) for cache format and customization tips.

## Command Enhancements

- **Smart `cd`** – `cd` falls back to fuzzy directory matching and prioritizes frequently visited
  locations to speed up navigation.
- **`cjshopt`** – A dedicated configuration builtin that sets editor behaviour, generates config
  files, and manages key bindings/history limits. None of its subcommands exist in POSIX sh.
- **`cjsh-widget`** – Exposes the line editor to shell scripts for advanced key-driven workflows.
- **`generate-completions`** – Pre-warm the completion cache. A convenience command beyond POSIX.
- **`approot`** – Quick directory jump command for cjsh config/cache/history/firstboot/completion roots and binary location.
- **`firstboot`** – Suppress the welcome banner by creating its marker once.
- **`hook`** – Lightweight precmd/preexec/chpwd/idle hook management similar to zsh's hook system.
- **`restart`** – Re-exec the current shell process, optionally dropping startup flags with
  `restart --no-flags`.
- **Special lifecycle handlers** – `command_not_found_handler` and `cjshexit` function names are
  recognized automatically for command-miss and shell-exit customization. They are ignored in
  `--minimal`, `--secure`, and `--posix` sessions.

## History & Execution Utilities

- **History expansion** – Interactive history expansion supports `!!`, `!prefix`, `!?text?`, `!$`,
  `!^`, `!*`, and `^foo^bar`. It automatically disables in script mode, `cjsh -c`, or when stdin
  is not a tty. Use `--no-history-expansion` to turn it off.
- **History recording** – Disable persistence entirely with `--no-history` (also disables history
  expansion).
- **Persistent exit codes** – Each history entry records the command's exit status to enrich
  completions and prompts.
- **Fuzzy history case sensitivity** – `cjshopt history-search-case` toggles whether the search menu
  treats uppercase and lowercase entries as distinct (press `Alt+C` inside the menu to flip the
  setting temporarily).
- **Directory-aware history** – Records the working directory before execution. Enable scoped
  recall with `cjshopt history-directory on`, and include descendants with
  `cjshopt history-directory-subdirs on`. Both default to off. `Alt+D` and `Alt+N` toggle them
  temporarily inside the history menu.
- **Fuzzy history sorting** – History search is newest-first by default, and `Alt+S` cycles the open
  menu through command-text and metadata sort arrangements without changing the configured default.
- **Typeahead buffering** – Key presses made while a command runs are replayed automatically once
  the prompt returns.

## Additional Non-POSIX Behaviours

- **Startup diagnostics** – `--show-startup-time` prints the duration spent initializing CJSH.
- **Secure mode** – `--secure` skips environment/profile/rc/logout sourcing for hardened sessions,
  disables history persistence and smart cd, and ignores special lifecycle handlers.
- **Auto-background on suspend** – Append `&^` to a command to resume it in the background when
  you press `Ctrl+Z`. Append `&^!` to resume it and discard stdout/stderr after the suspend so the
  prompt stays clean.
- **Extension-based script dispatch** – When executing a script file without a shebang, CJSH can
  infer the interpreter from the file extension (for example, `.sh` uses `sh`, `.bash` uses `bash`).
  Disable with `--no-script-extension-interpreter` or `cjshopt script-extension-interpreter off`.
- **Consistent error output** – Interpreter failures now use the same compact `cjsh:` error_out
  format as other builtins for predictable logs.

## Disabling Enhancements

CJSH keeps POSIX-focused scripting predictable by disabling most POSIX+ features automatically in
non-interactive contexts. When you need a strictly standard shell interactively, start with
`--minimal` (disables colors, completions, completion learning, syntax highlighting, smart cd, rc
sourcing, the title line, history expansion, the status line, multiline line numbers, the startup
time banner, error suggestions, and prompt vars) and add `--secure` if you also want to skip all
login/logout dotfiles:

```bash
cjsh --minimal --secure
```

For repeatable launches, include equivalent flags in your launcher/alias.

## Summary

- Prompt markup, layout toggles, and syntax styling provide rich visual customization beyond POSIX.
- The isocline editor delivers multiline editing, hints, completions, abbreviations, and keymap
  control.
- Builtins such as `cjshopt`, `approot`, `generate-completions`, `hook`, `restart`, and
  `cjsh-widget` extend the shell's capabilities.
- History expansion, typeahead buffering, and persistent exit codes streamline interactive work.

Each enhancement is optional and either automatically disabled outside of interactive mode or
controllable through command-line flags and `cjshopt` commands, allowing you to match the behaviour
you need for any environment.
