<!--
  features.md

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

# Features Overview

CJ's Shell (cjsh) pairs a standards-focused POSIX shell engine with a modern interactive
experience. It aims for broad portable-script compatibility while documenting the places where
POSIX, Bash, or Zsh behavior is not identical.

## Core Shell Engine

- **POSIX-first semantics** – Standards-focused behavior backed by more than 1700 shell tests.
  This is an engineering target, not a claim of complete POSIX certification; see the
  [Language Compatibility Inventory](language-compatibility.md).
- **Bourne-compatible surface** – Classic constructs (`if`, `case`, `for`, `while`, functions,
    redirections, here-documents, command substitution) behave the way portable scripts expect.
- **POSIX+ extensions** – `[[ … ]]`, arithmetic contexts, indexed and associative arrays,
    namerefs, coprocesses, brace expansion (including strides), case fall-through, here-strings,
    and process substitution are supported. Extended globs are enabled with
    `cjshopt extglob on`. History expansion stays interactive-only by default.
- **Job control** – Background jobs, `fg`, `bg`, `jobs`, `wait`, `disown`, and `trap` integrate with
    terminal process groups so `SIGTTIN`/`SIGTTOU` retain their normal kernel semantics. Monitor
    mode is enabled for interactive shells and toggled with `set -m` / `set +m`. Append `&^` to a command
    to auto-background it on `Ctrl+Z`, or `&^!` to auto-background and discard stdout/stderr after
    the suspend. `set -o huponexit` controls whether exiting shells hang up or leave running jobs
    alone (default: off, so long-lived helpers keep running until you explicitly stop them).

## Interactive Layer

Powered by the embedded [isocline](https://github.com/cadenfinley/isocline) editor:

- **Multiline editing** with automatic indentation and optional line numbers.
- **Syntax highlighting** that understands commands, keywords, paths, arguments, substitutions,
    comments, and error states.
- **Fuzzy completions** for commands, files, options, variables, users, and hosts. Completions learn
    from your `PATH` and cached man-page metadata.
- **Inline hints & preview** with configurable delays, spell correction, and case sensitivity.
- **Advanced history** – fuzzy search (`Ctrl+R`/`Ctrl+S`), deduplicated persistent history with exit codes,
    and bash-style history expansion that auto-disables in non-interactive contexts. Configure
    whether the fuzzy history menu matches case-sensitively with `cjshopt history-search-case` or
    flip it on the fly with `Alt+C` inside the menu. Press `Alt+S` to cycle sort modes for the open
    history search menu. Directory-aware recall is available with `cjshopt history-directory`,
    with optional descendants via `cjshopt history-directory-subdirs` and all ancestors via
    `cjshopt history-directory-parents`; `Alt+D`, `Alt+N`, and `Alt+P` toggle these for the open menu.
    At the end of the input buffer, `Up`/`Down` move between history entries;
    recalled entries place the cursor at the end of the buffer, including multiline commands.
    `Shift+Up`/`Shift+Down` navigate history from any cursor position.
- **Custom key bindings** – An Emacs-inspired default profile, an optional `vim` profile that adds
    `Alt+H/J/K/L/W` navigation, and fine-grained overrides via `cjshopt keybind` (including
    command-driven bindings through `cjsh-widget`). The `vim` profile is not a modal Vi/Vim mode.
- **Agent-assisted command writing** – Route editor text to user-configured AI executors through
    `cjshopt agent-mode`, select their JSON command suggestions in an isocline menu, and review the
    result before execution. CJSH does not manage provider credentials.
- **Browser shortcut** – Press `Alt+O` to search the current buffer on the web or open its URL,
    using `$BROWSER` or the system launcher, then continue at a fresh, empty prompt.
- **Mouse-aware editing** – Configure capture with `cjshopt mouse-clicking`: `all-off` disables it
    everywhere, `off` limits it to interactive menus, and `simple`/`smart` also support
    prompt cursor placement. Clicks can select completion/history entries,
    and `cjshopt completion-click-accept` controls whether
    clicks accept completion entries.
- **Automatic completion menu** – Opt in with `cjshopt completion-auto-menu on` to show passive
    suggestions while typing. Tab completes a unique match immediately or activates selection,
    scrolling, and acceptance for multiple matches. Up/Down also activate the menu; Left edits
    the command and Right accepts a suggestion. With prompt mouse clicking enabled, scrolling
    or clicking the menu activates it without accepting. After acceptance,
    the updated menu stays visible in passive mode until the next interaction.
- **Typeahead capture** – Keystrokes entered while a command runs are buffered and replayed when the prompt returns so you never lose input.
- **Abbreviations** – `abbr`/`unabbr` provide fish-style expansions for frequently typed snippets.

See the [Interactive Editing Guide](editing.md) and [Completion Authoring Guide](completions.md)
for full details.

## Prompt & Visual Styling

- **BBCode-inspired markup** inside `PS1`, `RPS1`/`RPROMPT`, `PS2`, and other prompt variables.
    Tags such as `[b]`, `[color=hotpink]`, `[ic-hint]`, and `[bgcolor=#202020]` let you mix ANSI
    styles with reusable highlight names. The full markup reference lives in
    [Prompt Markup and Styling](../themes/thedetails.md).
- **Bash-style `PS3`/`PS4` behavior** – `select` uses `PS3` for its choice prompt, and `set -x`
    uses `PS4` for trace prefixes.
- **Partial-line preservation marker** via `PROMPT_EOL_MARK` controls what appears when command
    output does not end in a newline before the next prompt.
- **Right prompt cursor tracking** – `cjshopt right-prompt-follow-cursor` keeps the inline right
    prompt aligned with the current cursor row instead of pinning it to the first line.
- **`cjshopt style_def`** redefines syntax-highlighter styles (`unknown-command`, `ic-hint`, etc.),
    instantly applying to both inline highlighting and prompt markup tags that reference them.

## Configuration Surface

- **Runtime toggles** – Every major interactive feature has a `cjshopt` command. Highlights:
    - `cjshopt multiline`, `cjshopt multiline-indent`, `cjshopt line-numbers`,
        `cjshopt multiline-start-lines`, `cjshopt multiline-max-lines`,
        `cjshopt multiline-bottom-lines`
      - `cjshopt completion-auto-menu`, `cjshopt completion-preview`,
          `cjshopt completion-click-accept`, `cjshopt menu-highlighting`,
          `cjshopt completion-case`, `cjshopt completion-spell`, `cjshopt completion-spell-enter`,
          `cjshopt completion-learning`, `cjshopt auto-tab`
     - `cjshopt hint`, `cjshopt hint-delay`, `cjshopt idle-timeout`, `cjshopt inline-help`,
         `cjshopt status-hints`, `cjshopt status-line`, `cjshopt status-reporting`,
         `cjshopt visible-whitespace`
     - `cjshopt mouse-clicking`, `cjshopt mouse-clicking-status-line`
     - `cjshopt prompt-newline`, `cjshopt right-prompt-follow-cursor`
     - `cjshopt exit-confirmation smart|always|never` controls when exiting requires a
         consecutive confirmation
     - `cjshopt agent-mode …` for user-provided command-writing executors
     - `cjshopt keybind …` and `cjshopt keybind ext …` for keymap management
    - `cjshopt set-history-max` to adjust persistent history size (0 or more entries; no upper limit)
    - `set -o huponexit` mirrors bash's option for sending SIGHUP to background jobs when the
        shell exits (off by default so long-running helpers stick around)
- **Generated config skeletons** – `cjshopt generate-env`, `cjshopt generate-profile`,
    `cjshopt generate-rc`, and `cjshopt generate-logout` create `~/.cjshenv`, `~/.cjprofile`,
    `~/.cjshrc`, and `~/.cjlogout` (or alternate locations under `~/.config/cjsh/`) with
    sensible defaults.

### Startup Files

| File | When it runs | Typical responsibilities |
| --- | --- | --- |
| `~/.cjshenv` (or `~/.config/cjsh/.cjshenv`) | Every shell start before login/interactive setup | Export environment vars shared by scripts and interactive sessions |
| `~/.cjprofile` (or `~/.config/cjsh/.cjprofile`) | Login shells before interactive setup | Export login-only environment vars and run login hooks |
| `~/.cjshrc` | Every interactive shell (unless `--no-source`) | Prompt definitions, aliases, key bindings, abbreviations |
| `~/.cjlogout` | When a login shell exits | Cleanup hooks, session summaries |

Set `CJSH_ENV` to override the `~/.cjshenv` search paths. If `CJSH_ENV` is set but empty, cjsh
falls back to the default search paths.

Persistent caches (history, generated completions, etc.) live under `~/.cache/cjsh/`.

## Command-line Flags

`cjsh` accepts these switches (short/long forms shown where available):

- `-h, --help` – usage information
- `-v, --version` – print the version banner and exit
- `-l, --login` – treat the shell as a login shell (source `~/.cjprofile`)
- `-i, --interactive` – force interactive behavior even if stdin is not a tty
- `-c, --command <string>` – execute a single command and exit (disables history expansion)
- `--no-exec` – read commands but do not execute them
- `--no-system-paths` – skip automatic PATH setup from `/etc/paths` and `/etc/paths.d`
- `--posix` – enable POSIX mode and reject non-POSIX syntax and non-POSIX builtins
- `-m, --minimal` – disable colors, completions and completion learning, syntax
  highlighting, rc sourcing, smart cd, the title line, history expansion, the status line,
  multiline line numbers, the startup time banner, error suggestions, prompt vars, and special
  lifecycle handlers; normal PATH setup and native env/profile loading still apply
- `-C, --no-colors`
- `-L, --no-titleline`
- `-U, --show-startup-time`
- `-N, --no-source`
- `-O, --no-completions`
- `--no-script-extension-interpreter`
- `--no-smart-cd`
- `--no-completion-learning` – keep completions enabled but skip on-demand man-page scraping
- `-S, --no-syntax-highlighting`
- `--no-error-suggestions`
- `--no-agent` – disable agent-assisted command writing, including configured activation keys,
  trigger prefixes, and its command-palette entry
- `--no-prompt-vars`
- `-H, --no-history-expansion`
- `--no-history` – disable history recording (also disables history expansion)
- `-W, --no-sh-warning` – suppress the reminder shown when cjsh is invoked via `sh`
- `-s, --secure` – skip `~/.cjshenv`, `~/.cjprofile`, `~/.cjshrc`, and `~/.cjlogout`, disable
  history persistence and smart cd, and ignore special lifecycle handlers

## Built-in Tooling Highlights

- `approot` – Jump straight to cjsh config/cache/history/firstboot/completion roots or the cjsh executable directory.
- `firstboot` – Suppress the welcome banner by creating its marker once.
- `restart` – Re-exec the current shell process, with an option to drop startup flags.
- `generate-completions` – Pre-populate completion caches by scraping manual pages in parallel.
- `hash` – Inspect or reset execution caches.
- `history` / `fc` – Explore, edit, and replay persistent history (exit codes are stored alongside entries).
- `hook` – Lightweight precmd/preexec/chpwd/idle hook system, including foreground terminal
  handoff and editor-state restoration for idle tools such as screensavers.
- `cjsh-widget` – Bridge between shell code and the line editor for custom key-driven behaviors.

## Performance Characteristics

- Single executable with vendored dependencies only.
- Aggressive optimization flags and caching layers (completion caches, prompt helpers, execution
    lookup cache).
- Prompt markup renders quickly because formatting is handled inside the line editor with minimal
    allocations.

## Platform & Build Support

- **Targets** – Linux, macOS, and WSL are primary; other POSIX-like systems generally work.
- **Toolchain** – Requires CMake ≥3.25 and a C++17-capable compiler (clang, GCC, or MSVC via WSL).
- **Quick build** – `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel`.
- **Package installs** – Homebrew (`brew install cjsh`) and Arch AUR (`cjsh`) are maintained.

For installation walkthroughs, see [Quick Start](../getting-started/quick-start.md). For the
interactive feature matrix, continue to the [Editing](editing.md) and
[POSIX+ Interactive Features](non-posix-features.md) documents.
