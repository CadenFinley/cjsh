<!--
  what-to-know.md

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

# What You Need to Know

Welcome to CJ's Shell (cjsh)! This guide highlights the interactive features that ship enabled by default and explains how to tailor them to your workflow.

## Visual & Interface Features

### Prompt Styling & Themes
**Status:** Enabled  
**Configure:** Edit `PS1`, `RPS1`, `PS2`, `PROMPT_COMMAND`, and `PROMPT_EOL_MARK` directly in your config using the BBCode-style markup described in [Prompt Markup and Styling](../themes/thedetails.md). Use `cjshopt style_def <token_type> <style>` to redefine highlight palettes that are shared between syntax highlighting and prompt tags.  
**Disable:** Start cjsh with `--minimal` (turns off colors, completions and completion learning, syntax highlighting, smart `cd`, rc sourcing, the title line, history expansion, the status line, multiline line numbers, the startup time banner, error suggestions, prompt vars, and special lifecycle handlers), or disable smart cd directly with `cjsh --no-smart-cd` / `cjshopt smart-cd off`.

All prompt styling now lives inside your dotfiles—no external theme DSL or bundled theme directory is required. Share a prompt by exporting new variables or sourcing a file that sets them, just like any other shell configuration.

### True Color Support
**Status:** Enabled when the terminal advertises 24-bit color.  
**Configure:** Automatically detected; adjust styling through your prompt definitions or `cjshopt style_def`.  
**Disable:** Launch with `cjsh --no-colors` (or include `--no-colors` in your launcher/alias).

### Prompt Layout
**Status:** `prompt-newline` defaults to off so prompts stay compact unless you explicitly add spacing.  
**Configure:**

```bash
cjshopt prompt-newline on|off|status
```

Enable `prompt-newline` when you want a visual spacer between commands in long transcripts. Keep it off for a denser prompt layout.

### Syntax Highlighting
**Status:** Enabled  
**Configure:** Change token styles with `cjshopt style_def <token_type> <style>`.  
**Disable:** Start cjsh with `--no-syntax-highlighting`.

---

## Completion & Line Editing

### Fuzzy Completions
**Status:** Enabled (always on)  
Fuzzy matching powers command, path, and argument completions without additional configuration.

### Completion Preview
**Status:** Enabled  
**Configure:** `cjshopt completion-preview on|off|status`

### Completion Menu Layout
Menus always open as a full single-column list with scrolling and paging. Use `PgUp`/`PgDn`,
`Shift+Up`/`Shift+Down`, or the mouse wheel to navigate.

### Menu Syntax Highlighting
**Status:** Disabled
**Configure:** `cjshopt menu-highlighting none|single|all|reverse|status`

### Spell Correction
**Status:** Enabled  
**Configure:** `cjshopt completion-spell on|off|status`

### Enter Spell Autocorrection
**Status:** Disabled  
**Configure:** `cjshopt completion-spell-enter on|off|status`

### Completion Case Sensitivity
**Status:** Disabled (completions are case-insensitive)  
**Configure:** `cjshopt completion-case on|off|status`

### Auto-Tab Expansion
**Status:** Disabled  
**Configure:** `cjshopt auto-tab on|off|status` (auto-inserts a completion when the match is unique)

### Smart cd
**Status:** Enabled  
**Configure:** `cjshopt smart-cd on|off|status`  
**Disable:** Launch with `cjsh --no-smart-cd`.

### Inline Hints & Delay
**Status:** Enabled  
**Configure:**

```bash
cjshopt hint on|off|status
cjshopt hint-delay <milliseconds|status>
```

### Inline Help Prompts
**Status:** Enabled  
**Configure:** `cjshopt inline-help on|off|status`

### Status Hint Banner
**Status:** `normal` (only shows when both the buffer and status line are empty)  
**Configure:** `cjshopt status-hints <off|normal|transient|persistent|status>`

### Status Line Visibility
**Status:** Enabled  
**Configure:** `cjshopt status-line on|off|status` (turn it off to hide both syntax feedback and the hint banner entirely)

### Status Reporting
**Status:** Enabled  
**Configure:** `cjshopt status-reporting on|off|status` (leave the status line visible but mute cjsh’s validation/error summaries)

### Mouse Clicking
**Status:** `off` by default: prompt editing stays native while menus temporarily capture the mouse

**Configure:**

```bash
cjshopt mouse-clicking all-off|off|simple|smart|status
cjshopt mouse-clicking-status-line on|off|status
```

Use `all-off` to prevent mouse capture everywhere, or `off` to leave prompt editing under terminal
control while allowing completion/history menus to capture clicks and wheel events temporarily.
`simple` and `smart` also enable
click-to-position editing. `mouse-clicking-status-line` only controls whether the `Mouse clicking
is enabled` indicator is shown.

### Visible Whitespace Markers
**Status:** Disabled  
**Configure:** `cjshopt visible-whitespace on|off|status`

### Multiline Editing
**Status:** Enabled  
**Configure:**

```bash
cjshopt multiline on|off|status
cjshopt multiline-indent on|off|status
cjshopt multiline-start-lines <count|status>
cjshopt multiline-max-lines <count|status>
cjshopt multiline-bottom-lines <count|status>
cjshopt line-numbers <absolute|relative|off|status>
cjshopt line-numbers-continuation on|off|status
cjshopt line-numbers-replace-prompt on|off|status
cjshopt current-line-number-highlight on|off|status
```

---

## Navigation Features

### Smart `cd`
**Status:** Enabled  
Smart `cd` adds fuzzy directory matching for quick navigation between directories.

### Directory Listings
cjsh leaves directory listing behavior up to your configuration. Add an `ls` wrapper, hook, or alias in `~/.cjshrc` if you prefer automatic listings after `cd`.

---

## History Features

- **History expansions:** Enabled in interactive sessions (`!!`, `!git`, `!?text?`, `!$`, `!^`, `!*`, `^old^new`). `!?` by itself is not valid; include a search string. Disable with `cjsh --no-history-expansion`.
- **History recording:** Disable persistence with `cjsh --no-history` (also disables history expansion). Secure mode also disables history persistence.
- **History search:** Press `Ctrl+R` or `Ctrl+S` for the fuzzy history search menu (use `Alt+C` inside it to toggle case sensitivity).
- **Directory-aware history:** Use `cjshopt history-directory on` to recall commands from the current directory, `cjshopt history-directory-subdirs on` to include nested directories, and `cjshopt history-directory-parents on` to include all ancestors up to `/`. All three default to off; subdirs and parents work independently and exclude sibling branches. In the history menu, `Alt+D`, `Alt+N`, and `Alt+P` toggle these settings temporarily.
- **History search case sensitivity:** Matching is case-sensitive by default; adjust with `cjshopt history-search-case on|off|status` to set the default for every session.
- **Persistence:** History entries are stored in `~/.cache/cjsh/history.txt`; duplicate commands are suppressed by default. Concurrent writers lock a sibling `.lock` file and atomically replace complete snapshots, preserving command metadata and frequency counts. Unsubmitted input stays private to its session. Existing history-file symlinks retain their targets and share the target lock. A killed writer can leave a private `.tmp.*` file; readers ignore it.
- **Retention:** Adjust limits with `cjshopt set-history-max <number|default|status>` (any non-negative value; default 1000 entries).

---

## Configuration & Compatibility

### Inherited File Descriptors

Interactive login shells mark inherited file descriptors 3 through 19 close-on-exec
before reading startup files, matching Bash's compatibility range. Builtins can still
use these descriptors, but external programs do not inherit them automatically.
Explicit redirections, including descriptors opened in startup files with `exec`, can
pass descriptors to external programs. Standard input/output/error and descriptors
20 and above retain their inherited flags. Noninteractive shells and interactive
shells without login mode also retain inherited descriptor flags.

### Startup Files
- `~/.cjshenv` – Sourced for every shell start (before login/interactive setup).
- `~/.cjprofile` – Executed for login shells before interactive setup.
- `~/.cjshrc` – Interactive configuration (aliases, prompt definitions, hooks, etc.).
- `~/.cjlogout` – Optional cleanup script sourced when a login shell exits.

During interactive startup, Ctrl-C interrupts the active startup command and skips
the remaining startup files. cjsh finishes initializing the prompt with status 130;
settings already applied by startup files remain in effect. An interrupted interactive
`-c` or script invocation exits with status 130 before running its body. Ctrl-D keeps
its normal end-of-input behavior and does not cancel startup. Programs or traps that
explicitly ignore SIGINT retain that behavior.

Set `CJSH_ENV` to override the `~/.cjshenv` search paths. If `CJSH_ENV` is set but empty, cjsh
falls back to the default search paths. Each native stage uses the home dotfile first,
then the same filename under `~/.config/cjsh/` if the home file is missing or unusable.
Automatic startup and logout files must be readable regular files; symlinks to regular
files are supported. Directories, FIFOs, and other special files are skipped.

Background interactive shells normally stop until their parent places them in the
foreground. If repeated stop attempts cannot establish foreground ownership (for
example, in an orphaned process group), cjsh warns and continues with terminal job
control disabled, leaving the foreground process group unchanged.

`--config-dir DIR` redirects **all four native startup files** to that directory, with
no fallback to home files. A nonempty `CJSH_CONFIG_HOME` provides the same override when
the command-line option is absent. Precedence is `--config-dir` > `CJSH_CONFIG_HOME` >
default locations. `CJSH_ENV` still overrides the environment stage alone, including
when the root is overridden; a missing nonempty override is skipped without fallback.
Paths accept a leading `~/`; relative overrides resolve against the invocation directory.
The root is selected before startup files run. These controls do not relocate history
or generated completions; `CJSH_HISTORY_FILE` continues to override history separately.

Set `CJSH_HISTORY_FILE` in `~/.cjshrc` to select a custom history file for the session.
It overrides a value inherited from the environment or set in earlier startup files;
unsetting it restores the default `~/.cache/cjsh/history.txt`. History storage is normally
prepared after `.cjshrc`. Explicit history commands during startup use the setting at
that point, and a later assignment can still change the selection. Once interactive
startup finishes, the selected path stays fixed for the editor and history commands.

`--no-config` skips all automatic native startup and logout files, POSIX profiles and
`ENV`, and system PATH setup. Explicit `source`/`.` commands still work. UI
features, hooks defined by commands, and history preferences keep their normal behavior.
`--no-source` (`-N`) retains its narrower meaning: skip the native interactive rc file.
`--minimal` (`-m`) retains its feature-reduction scope, follows the normal system PATH
setup rules, and still reads native env/profile files. `--secure` (`-s`) skips automatic
configuration and disables history and smart cd.

For `--posix` and invocation as `sh`, login startup reads `/etc/profile`, then
`$HOME/.profile`, then (only when interactive) the file named by `ENV`. Non-login
interactive shells read only `ENV`; ordinary scripts do not read it. Native startup
files and native root overrides do not participate. `ENV` receives parameter and
arithmetic expansion as one pathname, without field splitting, globbing, tilde
expansion, command-string evaluation, or PATH search. A relative pathname is relative
to the current directory after profiles. Missing files are skipped. This follows the
[POSIX interactive ENV rule](https://pubs.opengroup.org/onlinepubs/9799919799/utilities/V3_chap02.html)
and the traditional [sh login profile sequence](https://www.gnu.org/software/bash/manual/html_node/Bash-Startup-Files.html).

CJSH refuses execution with status 1 when real/effective user IDs or group IDs differ,
before initializing subsystems or consulting environment-derived startup paths. It does
not offer a flag to retain set-user-ID or set-group-ID privileges. Help and version are
available without shell initialization.

### Invocation and environment policy

`-n` and `--no-exec` check syntax without executing commands or loading startup files.
They accept `-c` strings, script files, and standard input. Invalid syntax returns a
nonzero status; valid syntax returns 0. `-m` remains minimal mode and `-s` remains secure
mode; they are not monitor mode or stdin selectors. Use `set -m` for monitor mode and
omit a script/`-c` to read stdin. Options precede the script or command operands; `--`
ends option parsing. `--help` lists all invocation flags. Invocation errors return 1
and send diagnostics and usage to stderr; explicit help and version return 0 on stdout.

Native login shells initialize `PATH` automatically before startup files run. Non-login
shells do this only when PATH is missing or empty; a nonempty inherited PATH is preserved
exactly, including its order, duplicates, and empty components. This preserves virtual
environments and custom toolchains when launching nested shells.

When initialization is needed, CJSH reads `/etc/paths`, then non-hidden files in
`/etc/paths.d` in filename order, then merges inherited PATH entries. The first occurrence
of each nonempty entry wins. Files contain one path per line (colon-separated entries also work);
blank lines, surrounding whitespace, and comment lines beginning with `#` are ignored.
Paths are literal: spaces inside paths are preserved, and variables and commands are not
expanded. Missing or unreadable files and non-file entries are skipped.

If neither the files nor the inherited PATH supply any entries, CJSH uses
`/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin`. This also provides a working default on
systems without `/etc/paths` or `/etc/paths.d`. Startup files can then override PATH.
`MANPATH` is preserved, including empty and absent values.

Pass `--no-system-paths` to disable this setup and preserve the inherited PATH exactly,
including an empty or absent value. POSIX, secure, syntax-only, and `--no-config`
invocations also skip it. These startup-only options (`--no-system-paths`, `--config-dir`,
`--no-config`) are invocation-only; select them in your launcher. The former `--login-path` flag has been removed. Remove it from existing launch
commands to use the default setup. For reproducible scripts, supply PATH explicitly and
use `--no-system-paths` or `--no-config`.

Inherited `USER` and `LOGNAME` are preserved even when empty; missing values are filled
from the real user's account record. CJSH does not supply or export defaults for
`LANG`, `PAGER`, or `TMPDIR`. Children keep the caller's locale selection.

### Unavailable persistence

A missing home directory, unwritable cache, or unusable custom history file does not
prevent command execution or an interactive prompt. CJSH does not create a missing HOME.
It disables the affected history or completion storage and reports each unavailable
storage area once. A failed cache also suppresses its first-boot banner. Independent
usable storage can still work (for example, custom history outside an unavailable home).
Configuration directories are created only by explicit configuration-writing commands.
History requires a writable parent directory for its lock and atomic replacement;
later history write failures disable further writes for the session after one diagnostic.


### POSIX & Bash Compatibility
cjsh targets broad POSIX compatibility for scripting while providing POSIX+ extensions such as `[[ ... ]]`, arrays, namerefs, coprocesses, extended globs, brace expansion, here-strings, process substitution, and rich redirection semantics. This is not a formal conformance or complete Bash/Zsh-emulation claim; consult the [Language Compatibility Inventory](../reference/language-compatibility.md) for precise support. Syntax extensions are available in scripts and interactive sessions; interactive-only features like history expansion, completions, and prompt styling disable themselves automatically when stdin is not a tty. Use `--minimal` or `--secure` when you want fewer extras in interactive shells.

### Completion Learning Paths
When cjsh scrapes man pages for completions, it uses `man` from `PATH` by default. Set
`CJSH_MAN_PATH` to force a specific `man` binary. In secure mode (`--secure`), cjsh only uses
`CJSH_MAN_PATH` and skips scraping if it is not set or invalid.

When `cjsh` is symlinked or launched as `sh`, interactive sessions print a reminder that cjsh is not a drop-in 100% POSIX shell. Suppress this notice with `cjsh --no-sh-warning`.

---

## Extended Interactive Tools

### Abbreviations
Define inline expansions with the `abbr` builtin:

```bash
abbr gs='git status --short --branch'
abbr                 # list abbreviations
unabbr gs            # remove an abbreviation
```

### Typeahead
Keystrokes are buffered while commands run so no input is lost. This is always enabled.

### Key Bindings
Inspect or tweak key bindings with:

```bash
cjshopt keybind list                   # safe at runtime
cjshopt keybind profile list           # show available profiles
cjshopt keybind profile set vim        # add Vim-inspired navigation (persist in ~/.cjshrc)
cjshopt keybind set <action> <keys>    # redefine bindings (run from config files)
cjshopt keybind add <action> <keys>    # append bindings (run from config files)
```

Use `cjshopt keybind --help` for the full action catalog. For custom widgets, see the `cjsh-widget` builtin in the reference documentation.

---

## Getting Help

- `help` – Overview of built-in commands.
- `help <builtin>` – Detailed usage for a specific builtin.
- `restart` – Re-exec the running shell (`restart --no-flags` for a clean relaunch).
- `cjsh --help` – Command-line usage and startup flags.
- `cjshopt --help` and `cjshopt <subcommand> --help` – Configuration guidance.
- Documentation lives under `docs/reference/` for deeper dives into editing, scripting, hooks, and prompt styling.

---

## Quick Configuration Examples

```bash
# Toggle completion preview for the current session
cjshopt completion-preview off

# Enable inline whitespace markers
cjshopt visible-whitespace on

# Add Vim-inspired navigation bindings (add to ~/.cjshrc to persist)
cjshopt keybind profile set vim

# Increase history retention
cjshopt set-history-max 20000
```

Run `cjshopt --help` for a complete list of interactive toggles and their detailed help screens.
