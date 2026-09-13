<!--
  editing.md

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

# Interactive Line Editing

CJ's Shell uses the [isocline](https://github.com/cadenfinley/isocline) line editor library to provide a powerful and feature-rich interactive editing experience. This document details all editing features currently available in cjsh.

## Overview

Isocline is a modern, pure C line editing library that provides advanced terminal interaction capabilities. CJ's Shell leverages and extends isocline to offer:

- Multiline editing with intelligent indentation
- Real-time syntax highlighting
- Context-aware tab completion
- Inline hints and preview suggestions
- Customizable key bindings
- Mouse-aware cursor movement and menu selection
- Line numbering for multiline input
- Optional visible markers for whitespace characters
- History search and management
- Brace matching and auto-insertion
- Spell correction
- Fish-style abbreviations (automatic expansion on word boundaries)

All of these features can be configured through the `cjshopt` command or in your `~/.cjshrc` configuration file.

## Core Editing Features

### Multiline Input

CJ's Shell supports seamless multiline input for complex commands, heredocs, and incomplete statements.

**Features:**
- Automatic continuation when lines are incomplete
- Smart indentation that aligns continuation prompts and indents automatic continuation lines
- Line numbers for easy navigation
- Both absolute and relative line numbering modes

**Configuration:**
```bash
# Enable/disable multiline input (enabled by default)
cjshopt multiline on|off|status

# Enable/disable automatic indentation (enabled by default)
cjshopt multiline-indent on|off|status

# Configure how many prompt lines are shown before typing (default: 1)
cjshopt multiline-start-lines <count|status>

# Limit visible multiline input rows (default: 15)
cjshopt multiline-max-lines <count|status>

# Limit visible menu content rows (default: 50)
cjshopt menu-max-lines <count|status>

# Configure the input and menu scroll margin (default: 3)
cjshopt multiline-bottom-lines <count|status>
```

When a command exceeds the viewport limit, the visible rows scroll with the cursor while the full
command remains available for editing and submission. Completion menus and other helper rows are
laid out separately below the input viewport. The symmetric cursor margin keeps the viewport fixed
while the cursor moves within it, uses only rows that exist in the command, and never pads the
display with blank lines.

Completion, history, command palette, and custom menus default to at most 50 content rows, including
expanded item previews, and shrink to fit the terminal. Headers and help text use separate rows.
Use `cjshopt menu-max-lines` to change the limit. The isocline API also exposes it through
`ic_set_menu_max_line_count()` and
`ic_get_menu_max_line_count()`. Menus use the same `multiline-bottom-lines` scroll margin (3 by
default) around the selected item.

**Multiline Detection:**
CJ's Shell automatically enters multiline mode when:
- A line ends with a backslash (`\`)
- Quotes are unclosed (`"`, `'`, or backticks)
- Control structures are incomplete (`if`, `while`, `for`, `case`, etc.)
- Heredocs are being entered (`<<`, `<<-`)
- Brackets/braces/parentheses are unclosed

### Line Numbers

When in multiline mode, line numbers help track your position in the input.

**Modes:**
- **Absolute numbering**: Shows actual line numbers (1, 2, 3, ...)
- **Relative numbering**: Shows distance from current line (0 for current, ±N for others)

**Configuration:**
```bash
# Enable absolute line numbers (default)
cjshopt line-numbers on
cjshopt line-numbers absolute

# Enable relative line numbers
cjshopt line-numbers relative

# Disable line numbers
cjshopt line-numbers off

# Check current status
cjshopt line-numbers status
```

> **Tip:** Custom continuation prompts normally suppress multiline line numbers. Restore them
> by running `cjshopt line-numbers-continuation on` (or calling
> `ic_enable_line_numbers_with_continuation_prompt(true)` from your startup scripts) before the
> editor initializes.
>
> Multi-line prompts that render banners or status lines can swap the final PS1 row with the
> numeric gutter so that every editable line begins with `1|`, `2|`, etc. Enable this behavior with:
>
> ```bash
> cjshopt line-numbers-replace-prompt on|off|status
> ```
>
> The replacement only applies when line numbers are visible (and, if `PS2` is set, when
> `cjshopt line-numbers-continuation on` is active).

**Current Line Highlighting:**
The line containing the cursor can be highlighted differently:
```bash
# Enable/disable current line number highlighting (enabled by default)
cjshopt current-line-number-highlight on|off|status
```

The line number styles can be customized:
- `ic-linenumbers`: Style for regular line numbers
- `ic-linenumber-current`: Style for the current line number

### Visible Whitespace Markers

You can visualize whitespace characters while editing to spot stray spaces or indentation issues. When enabled, spaces are rendered using a subtle middle-dot marker.

**Configuration:**
```bash
# Show or hide visible whitespace markers (disabled by default)
cjshopt visible-whitespace on|off|status
```

Pair this option with custom styling via `cjshopt style_def ic-whitespace-char "<style>"` to adjust the marker color.

### Line Wrap Marker

Long lines display a `↵` marker on macOS or `←` on other UTF-8 terminals where they
wrap onto another screen row by default. Customize it with one printable Unicode
character, or use an empty string to hide it.

```bash
cjshopt line-wrap-marker '↪'     # Use a custom wrap marker
cjshopt line-wrap-marker ''      # Hide the wrap marker
cjshopt line-wrap-marker status  # Show the current marker
```

Add the command to `~/.cjshrc` to persist the preference. The marker reserves its
display width, including two columns for a wide character. An empty marker lets
input use the full terminal width. Input contents and the newline after pressing
Enter are unchanged. Multi-character strings, control characters, and zero-width
characters are rejected. ASCII markers also work on non-UTF-8 terminals.

The isocline API exposes `ic_set_line_wrap_marker(const char*)` and
`ic_get_line_wrap_marker()`. The setter copies the string and returns `false`
without changing the marker for invalid input. Pass `""` to disable the marker or
`NULL` to restore the platform default. These replace the previous boolean API;
the shell command no longer accepts `on`, `off`, or other toggle words.

### Syntax Highlighting

Real-time syntax highlighting provides visual feedback as you type.

**Highlighted Elements:**
- Commands (builtins, executables, aliases)
- Keywords (`if`, `then`, `else`, `while`, `for`, etc.)
- Strings (single and double quoted)
- Variables and parameter expansions
- Operators (pipes, redirections, logical operators)
- Comments
- Errors (unmatched quotes, invalid syntax)

**Configuration:**
```bash
# Enable/disable syntax highlighting (enabled by default)
# Note: Can also be controlled with --no-syntax-highlighting flag

# Customize highlighting styles
cjshopt style_def <token_type> <style>
```

**Available Style Names:**
- `keyword`, `builtin`, `system`, `unknown-command`, `agent-prefix`, `agent-request`
- `string`, `comment`, `variable`, `number`, `operator`, `heredoc-delimiter`
- `file-argument`, `path-exists`, `path-not-exists`, `glob-pattern`, `assignment-value`
- `command-substitution`, `arithmetic`, `option`, `function-definition`, `history-expansion`
- `ic-prompt`, `ic-hint`, `ic-error`, `ic-info`, `ic-source`, `ic-diminish`, `ic-emphasis`
- `ic-linenumbers`, `ic-linenumber-current`, `ic-bracematch`, `ic-whitespace-char`

Heredoc opening markers (such as `EOF` in `cat <<EOF`) and matching closing lines use
`heredoc-delimiter` (bold yellow by default). Quoted/escaped markers, multiple heredocs,
and `<<-` tab stripping are recognized. Body text uses `string` rather than command styling.

```bash
cjshopt style_def heredoc-delimiter "bold color=#8BE9FD"
```

**Syntax Highlighting Control:**
The syntax highlighter can be temporarily disabled with the `--no-syntax-highlighting` startup flag.

### Completion System

CJ's Shell features a sophisticated completion system that provides context-aware suggestions.

**Completion Types:**
- **Command completion**: Completes commands from PATH, builtins, aliases, and functions
- **File/directory completion**: Completes paths with proper quoting and escaping
- **Variable completion**: Completes shell variable names
- **User completion**: Completes usernames (after `~`)
- **Hostname completion**: Completes hostnames (for ssh, scp, etc.)

**Features:**
- Fuzzy matching for typo tolerance
- Empty-prompt history suggestions ranked by last use, then frequency
- Source attribution (shows where completions come from)
- Preview of selected completion
- Optional expanded-by-default completion menu layout
- Automatic expansion with auto-tab

Need to author new entries or override the defaults? Check the [Completion Authoring Guide](completions.md) for cache formats, nested command support, and manual customization tips.

**Configuration:**
```bash
# Enable/disable completion preview (enabled by default)
cjshopt completion-preview on|off|status

# Open completion menus expanded by default (disabled by default)
cjshopt completion-menu-expanded on|off|status

# Control whether mouse clicks immediately accept completion entries
# (disabled by default)
cjshopt completion-click-accept on|off|status

# Syntax-highlight completion and history menu entries
# (disabled by default; reverse highlights every item except the selection)
cjshopt menu-highlighting none|single|all|reverse|status

# Enable/disable auto-tab (disabled by default)
# Auto-tab automatically completes unique prefixes
cjshopt auto-tab on|off|status

# Configure case sensitivity (disabled by default; completions are case-insensitive)
cjshopt completion-case on|off|status

# Enable/disable spell correction (enabled by default)
cjshopt completion-spell on|off|status

# Enable/disable Enter-triggered spell autocorrection (disabled by default)
cjshopt completion-spell-enter on|off|status

# Cap the number of suggestions shown per request (default: 1000, no upper limit)
cjshopt set-completion-max <number|default|status>
```

Lower or raise the completion cap (any value >= 1) to keep the menu focused when working inside
directories that contain thousands of files or deeply nested command trees.

At an empty or whitespace-only prompt, `Tab` offers unique history entries up to the
`cjshopt set-completion-max` limit (default: **1000**), newest first. Commands with the same last-use
timestamp are ordered by frequency, then by their position in the history file (later entries
first). Entries without valid timestamps follow dated entries. If history is disabled, missing,
or has no eligible entries, no completions are shown. Typing a command or path prefix uses the
usual context-aware completion sources.

**Using Completions:**
- Press `Tab` to show completions
- Press `Tab` again to cycle through options
- Use arrow keys to navigate the completion menu
- Press `Enter` to accept a completion
- Press `Esc` to cancel
- In expanded menus, use the mouse wheel to scroll and click entries to select/accept
  (`cjshopt completion-click-accept off` keeps click selection but requires Enter/Right/End to accept)
- Completion entries stay on one row. With completion preview enabled, the selected command appears
  at the prompt; oversized previews end with `...` to keep the prompt and menu controls visible.
  Accepting a completion inserts its full text.
- Use `cjshopt menu-highlighting single`, `all`, or `reverse` to render completion and
  history menu items through the same syntax highlighter used by the edit buffer.

### Hints and Inline Help

CJ's Shell provides inline hints and help to assist with command input.

**Hints:**
When there's a single possible completion, a hint is displayed inline with dimmed text.

**Features:**
- Shows the rest of the word being typed
- Appears after a configurable delay
- Can be accepted by pressing → (right arrow)

**Configuration:**
```bash
# Enable/disable hints (enabled by default)
cjshopt hint on|off|status

# Set or show the hint delay in milliseconds (0ms by default)
cjshopt hint-delay <milliseconds|status>
```

**Inline Help:**
Short help messages are displayed for certain operations:
- History search instructions
- Completion menu navigation
- Special key binding hints

**Configuration:**
```bash
# Enable/disable inline help (enabled by default)
cjshopt inline-help on|off|status
```

**Status Hint Banner:**
Configure when the underlined shortcut banner (`complete`, `history search`, `help`, etc.) shows up.

```bash
cjshopt status-hints <off|normal|transient|persistent|status>
```

- `off` – hide the banner entirely.
- `normal` (default) – display it only when both the input buffer and status line are empty.
- `transient` – show it whenever no other status message is present.
- `persistent` – always prepend it above any status output.

**Status Line Visibility:**
Hide the entire status area—syntax validation, inline diagnostics, and the hint banner—with a single toggle.

```bash
cjshopt status-line on|off|status
```

- `on` (default) – keep validation messages and banners visible.
- `off` – remove the status row entirely; `status-hints` preferences are remembered but stay hidden until you turn the line back on.

**cjsh Status Reporting:**
Prefer to keep the status line for banners but hide validation/error text? Toggle the built-in reporting channel.

```bash
cjshopt status-reporting on|off|status
```

- `on` (default) – show syntax validation results beneath the prompt.
- `off` – suppress cjsh-generated status text while still allowing `status-hints` (or `status-line-callback`) to render.

**Status Line Callback:**
Run your own shell function during status refresh and publish custom content into the status row.

```bash
cjshopt status-line-callback <function_name|off|status>
```

- The callback receives the active buffer as `$1` and via `CJSH_STATUS_INPUT`.
- Set `CJSH_STATUS_OUTPUT` inside the function to the text you want shown.
- Use `off` to disable callback output.

**Full Help:**
Press `F1` at any time to display the complete key binding cheat sheet, regardless of the inline-help setting.

### Mouse Clicking and Menu Navigation

CJ's Shell can use terminal mouse reporting for cursor placement and menu navigation.

**Per-prompt toggle:**
- Press `F2` to toggle mouse clicking for the active prompt only
- When enabled, the status row can show `Mouse clicking is enabled`

**Persistent defaults:**
```bash
# Configure prompt and menu mouse capture
cjshopt mouse-clicking all-off|off|simple|smart|status

# Show or hide the mouse status indicator line
cjshopt mouse-clicking-status-line on|off|status

# Configure click-to-accept behavior for completion hints and completion menus
cjshopt completion-click-accept on|off|status
```

**Behavior:**
- `off` is the default mode
- In the editor buffer, left-click moves the cursor to the clicked position
- In `off` mode, editing remains under terminal control while interactive menus temporarily capture
  clicks and wheel events; collapsed completion lists remain non-clickable and `all-off` prevents
  capture everywhere
- In `smart` mode, starting a selection in the prompt/gutter or status rows, or dragging with the
  left mouse button, suspends mouse capture so the terminal can highlight text. The display stays
  in place while selecting. A reported button release resumes capture without clearing the
  highlight; the next click or keyboard input resumes repainting
- Expanded completion, history, and command-palette menus temporarily capture mouse events in every
  mode except `all-off`
- Clicking the prompt, menu header/help, or any area outside the selectable menu rows temporarily
  releases mouse capture; keyboard input or returning focus to the terminal restores it
- Completion click acceptance follows `cjshopt completion-click-accept` (disabled by default)

Smart mode releases capture as soon as the terminal reports a drag into another character cell,
including inside interactive menus. Whether highlighting continues during that same drag depends
on the terminal. If the first drag only releases capture, release the button and drag again before
typing or changing focus. Terminals that report only clicks release capture when the button is
released in a different cell.

Smart mode uses standard terminal mouse reports without terminal-specific configuration. Turning
off mouse reporting also stops button-release reports, so automatic resume on release is
best-effort: it works only if a release still reaches cjsh, for example one already queued before
capture was disabled. There is no portable notification that native text selection has ended.
Keyboard input restores capture; focus-in input also restores it when supported by the terminal.
`F2` lets you toggle capture manually before or after selecting.

### Fish-Style Abbreviations

CJ's Shell supports fish-style abbreviations that expand typed shortcuts into longer phrases as soon as you type a whitespace character or submit the line. This is powered directly by the isocline editor, so expansions happen inline without disrupting your cursor position or undo history.

**Usage:**
- Define abbreviations with the `abbr` builtin: `abbr gs='git status --short --branch'`
- Remove them with `unabbr`: `unabbr gs`
- List all active abbreviations by running `abbr` with no arguments

**Behavior:**
- Triggers expand when followed by a space, tab, newline, or when you press `Enter`
- Expansion text is inserted in place of the trigger, preserving subsequent input
- Abbreviations live in the current shell session and persist across reads within that session

**Defaults:**
- `abbr` → `abbreviate`
- `unabbr` → `unabbreviate`

Add `abbr` definitions to your `~/.cjshrc` to load them automatically on startup.

### Brace Matching and Auto-Insertion

Visual feedback for matching brackets, braces, and parentheses.

**Brace Matching:**
When the cursor is next to a brace, its matching pair is highlighted. Unmatched braces are highlighted as errors.

**Default Brace Pairs:**
- Parentheses: `()`
- Square brackets: `[]`
- Curly braces: `{}`

**Auto-Insertion:**
When you type an opening brace, the closing brace is automatically inserted (enabled by default).

**Default Auto-Insertion Pairs:**
- Parentheses: `()`
- Square brackets: `[]`
- Curly braces: `{}`
- Double quotes: `""`
- Single quotes: `''`

**Note:** Brace matching and auto-insertion are currently controlled by isocline's default settings and cannot be configured via `cjshopt`, but the feature is active.

## History Management

### History Features

CJ's Shell maintains a persistent command history with advanced search capabilities.

**Features:**
- Persistent history across sessions
- Configurable maximum entries
- Duplicate suppression
- History expansion (`!`, `!!`, `!$`, etc.)
- Interactive fuzzy search

**Configuration:**
```bash
# Set maximum history entries
cjshopt set-history-max <number>

# Use default history size
cjshopt set-history-max default

# Check current setting
cjshopt set-history-max status
```

**History File:**
History is stored at `~/.cache/cjsh/history.txt`. Each entry is prefixed by a metadata line in the
form `# timestamp=<unix_timestamp> frequency=<count> code=<exit_status> ms=<elapsed_ms> cwd=<directory>`.
Metadata values are percent-encoded. `cwd` records the physical working directory before execution.

**Duplicate Handling:**
Repeated commands in the same directory update their existing entry and frequency. The same
command run in different directories keeps a separate entry for each directory.

### History Expansion

History expansion runs in interactive sessions before the command line is parsed.

```bash
!!            # rerun the previous command
!git          # most recent command that starts with "git"
!?status?     # most recent command that contains "status"
!$            # last argument from the previous command
!^            # first argument from the previous command
!*            # all arguments from the previous command
^old^new      # replace the first "old" in the previous command with "new"
```

`!?` by itself is not a complete expansion; use `!?text?` (or `!?text`) with a search string.

Press `Alt+.` or `Alt+_` to insert the last argument from the previous command. Press the binding
again to cycle through the last arguments of older history entries. You can also use `!$` when you
want history expansion to happen as part of command submission.

### History Search

**Fuzzy Search (`Ctrl+R` or `Ctrl+S`):**
1. Press `Ctrl+R` or `Ctrl+S` to open the history search menu
2. Type to filter history entries
3. Use `Up` and `Down` to select a match
4. Press `Enter` to execute the selected command, or `Tab` to load it for editing
5. Press `Esc` to cancel search

**History Navigation:**
- `↑` (Up Arrow): Previous command
- `↓` (Down Arrow): Next command
- `Alt+<`: Jump to oldest history entry
- `Alt+>`: Jump to newest history entry

Control whether the fuzzy history search menu matches case-sensitively with `cjshopt history-search-case <on|off|status>` or press `Alt+C` inside the menu to flip modes on the fly. Turning case sensitivity off lets uppercase queries (for example, `LS`) match lowercase history entries (`ls`) when filtering.

Use `cjshopt history-directory on|off|status` to scope interactive history recall and suggestions
to the current directory. Use `cjshopt history-directory-subdirs on|off|status` to also include
commands run in its nested directories. Both default to `off`. A parent includes descendants when
nested scope is on; a child does not include its parent's commands. Older entries without `cwd`
remain available with directory scope off. Add these commands to `~/.cjshrc` to persist preferences.

Inside the history menu, `Alt+D` toggles directory scope and `Alt+N` toggles nested directories for
the open menu. Both settings appear in the status line, alongside case sensitivity.

History search results are sorted newest-first by default. Press `Alt+S` inside the menu to cycle the current menu through available sort arrangements such as command text and metadata keys present in the matching history entries. This only changes the open menu; the default sort can be changed by callers through the isocline history search sort API.

## Agent-Assisted Command Writing

CJSH can pass the current editor buffer to any command-line AI executor and present the returned
commands in an isocline selection menu. CJSH does not choose a provider or store API keys. Press
`Tab` to place the chosen command in a fresh editor buffer for inspection, or `Enter` to submit it
immediately. In either case, the original natural-language request remains visible on the preceding
prompt line. If `PS1_FINAL` or `RPS1_FINAL` is configured, those final-prompt values restyle the
preserved request; the generated command still uses the normal active `PS1` and `RPS1`.

Configure an executor in `~/.cjshrc`:

```bash
cjshopt agent-mode set \
  --trigger-prefix ': ' \
  --system-prompt 'Prefer portable commands and flag destructive behavior.' \
  --command 'copilot --reasoning-effort low --prompt'
```

With that example, type `: describe what the command should do` and press `Enter`. The trigger
prefix is removed before the request is sent. While an enabled trigger prefix matches, the syntax
highlighter styles the prefix and the remaining natural-language request with `agent-prefix` and
`agent-request`; it does not interpret request punctuation as shell syntax. Both styles can be
changed with `cjshopt style_def`. A transient **Waiting for agent response.** status
animates through one, two, and three dots while the executor is running, then clears before its
results or an error are displayed.
Use `Up`/`Down` to select a suggestion, `Tab` to insert it for review, `Enter` to submit it, or `Esc`
to keep the original buffer.

Invoking the agent activation key with an empty or whitespace-only buffer, or pressing `Enter`
after typing only a configured prefix and optional whitespace, does not start the executor. It
submits an empty editor buffer and immediately advances to a fresh prompt.

The executor command is parsed into an executable and arguments; shell operators are not evaluated.
CJSH appends one final argument containing its embedded protocol and safety prompt, runtime context,
the optional user-configured system prompt as additional instructions, and the current input as the
command request. Runtime context is generated for each request and includes local and UTC time,
working directory, hostname, operating system and kernel, architecture, CJSH version and mode, and
the previous command's exit status. CJSH does not copy arbitrary environment variables or credentials
into the prompt. The embedded prompt always takes precedence and requires one to three command
suggestions. The executor must print a JSON array, although the parser tolerates surrounding
explanatory text or a Markdown fence:

```json
[
  {"command": "find . -name '*.log' -delete", "description": "Delete log files recursively"},
  {"command": "find . -name '*.log' -print", "description": "Preview matching log files"}
]
```

Omit `--trigger-prefix` to configure the fallback executor. Multiple prefix executors are supported;
the longest matching prefix wins. Press `Alt+A` to invoke agent writing on any current buffer (using
the matching prefix, then the fallback, then the first configured executor), or choose **Write
command with agent** from the command palette. Change the key with
`cjshopt agent-mode key <key>`, for example `cjshopt agent-mode key F3`.
An explicit `cjshopt keybind ext` command on the same key takes precedence.

Useful controls:

```bash
cjshopt agent-mode status
cjshopt agent-mode on
cjshopt agent-mode off
cjshopt agent-mode key default
cjshopt agent-mode key off
cjshopt agent-mode clear --trigger-prefix ': '
cjshopt agent-mode clear --default
cjshopt agent-mode clear --all
cjshopt agent-mode reset
```

Start CJSH with `--no-agent` to keep agent mode disabled even while executor definitions load from
`~/.cjshrc`. It can still be deliberately enabled later with `cjshopt agent-mode on`. Persist the
startup disable by including `--no-agent` in your launcher or shell alias.

## Open Buffer in Browser

Press `Alt+O` to search the web for the current buffer, or choose **Open buffer in browser**
from the command palette (`Alt+P`). HTTP, HTTPS, and `file://` URLs open directly; addresses
starting with `www.` open with HTTPS. Other text becomes a URL-encoded search query. Leading
and trailing whitespace is ignored, and empty buffers do nothing. The request remains visible
above a fresh, empty prompt, including when the browser command fails. Like agent-assisted
command writing, this honors `PS1_FINAL` and `RPS1_FINAL` for the preserved request. The search
text is never submitted as a shell command.

Configure the browser in `~/.cjshrc`, like your editor:

```bash
export BROWSER='firefox --new-window'
# macOS example:
# export BROWSER='open -a "Firefox"'

# Optional: replace the default Google search URL prefix.
export CJSH_BROWSER_SEARCH_URL='https://duckduckgo.com/?q='
```

When `BROWSER` is unset or empty, CJSH uses `open` on macOS and `xdg-open` on other systems.
The browser setting is parsed into an executable and optional quoted arguments, just like an
agent executor. CJSH appends the destination URL as one final argument; it does not evaluate
the buffer as a shell command. The launcher runs with normal terminal settings, so terminal
browsers work too. The prompt resumes when the launcher exits.

`Alt+O` is a CJSH-owned isocline runoff binding. It is restored after keymap resets and profile
changes, and explicit user bindings on that key take precedence. `Alt+B` still moves backward
one word. To disable the browser shortcut, add `cjshopt keybind add none alt+o` to `~/.cjshrc`.

## Key Bindings

CJ's Shell supports customizable key bindings with multiple profiles.

### Key Binding Profiles

**Available Profiles:**
- `emacs`: Emacs-inspired key bindings (default)
- `vim`: Inherits the `emacs` profile and adds Vim-inspired navigation with `Alt+H/J/K/L` and
  `Alt+W`. This is not a modal Vi/Vim editing mode.

**Set Profile:**
```bash
cjshopt keybind profile set emacs|vim
```

### Common Key Bindings (Emacs Profile)

#### Cursor Movement
- `Ctrl+A`: Move to beginning of line
- `Ctrl+E`: Move to end of line
- `Ctrl+F` / `→`: Move forward one character
- `Ctrl+B` / `←`: Move backward one character
- `Alt+F`: Move forward one word
- `Alt+B`: Move backward one word
- `↑`: Previous line (or previous history)
- `↓`: Next line (or next history)

#### Editing
- `Ctrl+D`: Delete character under cursor (or exit if line is empty)
- `Ctrl+H` / `Backspace`: Delete character before cursor
- `Ctrl+W`: Delete word before cursor
- `Alt+D`: Delete word after cursor
- `Ctrl+U`: Delete from cursor to beginning of line
- `Ctrl+K`: Delete from cursor to end of line
- `Ctrl+Z` / `Ctrl+_`: Undo
- `Ctrl+Y`: Redo
- `Ctrl+T`: Transpose characters

CJ's Shell does not currently provide a kill ring, yank-last-deletion command, or transpose-words
action.

#### History
- `Ctrl+R`: Open the fuzzy history search menu
- `Ctrl+S`: Open the fuzzy history search menu
- `Alt+C`: Toggle case sensitivity while the fuzzy history search menu is open
- `Alt+D`: Toggle directory scope while the fuzzy history search menu is open
- `Alt+N`: Toggle inclusion of nested directories while the fuzzy history search menu is open
- `Alt+S`: Cycle sort arrangements while the fuzzy history search menu is open
- `↑`: Previous history entry
- `↓`: Next history entry
- `Alt+<`: First history entry
- `Alt+>`: Last history entry

#### Completion
- `Tab`: Trigger completion / cycle through completions
- `Shift+Tab`: Cycle backward through completions

#### Special
- `Ctrl+C`: Cancel current input
- `Ctrl+D`: Exit shell (when buffer is empty)
- `Ctrl+L`: Clear screen
- `Enter`: Execute command
- `F1`: Show help / key binding cheat sheet
- `F2`: Toggle mouse clicking for the current prompt
- `Alt+A`: Invoke agent-assisted command writing (when enabled)
- `Alt+O`: Search the buffer on the web or open its URL in the browser
- `Esc`: Cancel an open menu or search; at the main prompt, clear non-empty input

### Custom Key Bindings

You can inspect and customize key bindings through the `cjshopt keybind` subcommands:

```bash
# List all current key bindings (safe at runtime)
cjshopt keybind list

# Show available key binding profiles
cjshopt keybind profile list

# Add Vim-inspired navigation bindings (add to ~/.cjshrc to persist)
cjshopt keybind profile set vim

# Replace bindings for an action (changes apply immediately; add to ~/.cjshrc to persist)
cjshopt keybind set cursor-left ctrl-h|left

# Add bindings without removing existing ones (changes apply immediately; add to ~/.cjshrc to persist)
cjshopt keybind add delete-word-end alt-d

# Remove specific key bindings (changes apply immediately; add to ~/.cjshrc to persist)
cjshopt keybind clear ctrl-h ctrl-b

# Remove all custom bindings for an action (changes apply immediately; add to ~/.cjshrc to persist)
cjshopt keybind clear-action delete-word-end

# Restore default bindings (changes apply immediately; add to ~/.cjshrc to persist)
cjshopt keybind reset
```

**Key Specification Format:**
- `ctrl-<key>`: Control key combinations (e.g., `ctrl-a`)
- `alt-<key>`: Alt/Meta key combinations (e.g., `alt-f`)
- `shift-<key>`: Shift key combinations (e.g., `shift-tab`)
- `f<N>`: Function keys (e.g., `f1`, `f12`)
- `<key>`: Regular keys (e.g., `tab`, `enter`)

## Command-Driven Key Bindings

Use the extended key binding namespace to trigger shell commands directly from key presses:

```bash
# List custom command key bindings (safe at runtime)
cjshopt keybind ext list

# Bind Ctrl+G to run a command (add to ~/.cjshrc)
cjshopt keybind ext set ctrl-g --title 'Accept line' 'cjsh-widget accept'

# Add a palette-only command with a title used for command palette lookup
cjshopt keybind ext set palette:uuid --title 'Generate UUID' 'uuidgen'

# Remove a command binding
cjshopt keybind ext clear ctrl-g

# Clear every custom command binding
cjshopt keybind ext reset
```

Command key bindings typically leverage the `cjsh-widget` builtin to read or modify editor state.
Changes apply immediately in the current shell; add the same commands to `~/.cjshrc` to persist them.

## Prompt Customization

### Prompt Spacing

Control whether cjsh inserts a blank line after command execution.

**Configuration:**
Use `cjshopt prompt-newline on|off|status`, then add the same command to your startup files (typically `~/.cjshrc`) if you want it persisted.

### Prompt Markers

The prompt system uses markers to indicate different states:

**Primary Prompt Marker:**
Displayed as part of `PS1`. The default primary template is `\S  [color=#5fd7ff]\p[/color] \g`, which abbreviates parent directories and keeps the current directory name in full. With `--minimal` or `--secure`, the default is `cjsh> `.

**Continuation Prompt Marker:**
Displayed for continuation lines in multiline input. The default `PS2` template is `[ic-hint]> [/ic-hint]`.

**Select Prompt Marker:**
Displayed by `select` before reading a menu choice. Set `PS3` to customize it; the fallback is `#? `.

**Trace Prompt Marker:**
Displayed by `set -x` before each traced command. `PS4` defaults to `+ ` and supports the same
prompt escapes as `PS1`, plus normal shell variable expansion at trace time.

**History Search Prompt Marker:**
Displayed while fuzzy-searching command history. Set `PS5` to customize it; the fallback is `history search: `.

**Command Palette Prompt Marker:**
Displayed while searching the command palette. Set `PS6` to customize it; the fallback is `command palette: `.

**Customization:**
Define these markers directly inside `PS1`, `PS2`, `PS3`, `PS4`, `PS5`, `PS6`, and any helper
functions you call from `PROMPT_COMMAND`.

**Transient Final Prompt:**
Set `PS1_FINAL` to replace the just-submitted prompt line with a different style after Enter
accepts the command buffer. Set `RPS1_FINAL` if you also want a dedicated transient right prompt
on that submitted line.

## Terminal Features

### Color Support

CJ's Shell automatically detects terminal capabilities:

**Color Depth Detection:**
- Monochrome (1-bit)
- 8 colors (3-bit) with bold for bright
- 16 colors (4-bit)
- 256 colors (8-bit)
- True color (24-bit RGB)

**Environment Variables:**
- `COLORTERM`: Indicates color capability (`truecolor`, `24bit`, `256color`, etc.)

**Disable Colors:**
Use the `--no-colors` flag or set `TERM=dumb` to disable all color output.

### BBCode Formatting

CJ's Shell supports BBCode-style formatting in prompts and output:

**Tags:**
- `[b]...[/b]`: Bold text
- `[i]...[/i]`: Italic text
- `[u]...[/u]`: Underline text
- `[r]...[/r]`: Reverse video
- `[color]...[/color]`: Color names (e.g., `[red]`, `[blue]`)
- `[#RRGGBB]...[/]`: Hex RGB colors
- `[on color]...[/]`: Background colors

**Style Combinations:**
```
[b red on blue]Bold red text on blue background[/]
[i #ff00ff]Italic magenta text[/]
```

## Advanced Features

### Spell Correction

Automatic spell correction for commands and completions.

**How It Works:**
When no exact match is found, cjsh attempts to find the closest match using edit distance algorithms.

**Configuration:**
```bash
# Enable/disable spell correction (enabled by default)
cjshopt completion-spell on|off|status

# Enable/disable Enter-triggered spell autocorrection (disabled by default)
cjshopt completion-spell-enter on|off|status
```

### Heredoc Support

Full editing capabilities when entering heredocs:

```bash
cat <<EOF
# You get full editing features here:
# - Syntax highlighting
# - Multiline editing
# - History access
# - All normal key bindings
EOF
```

**Features:**
- Each line is edited with full isocline capabilities
- Supports `<<-` (strip leading tabs)
- Delimiter detection ends input
- Ctrl+C/Ctrl+D cancels heredoc input

### Readline Integration

CJ's Shell uses isocline's unified `ic_readline()` API for input, which provides:

**Inline Right Text:**
Prompts can display additional information on the right side of the input line when you embed markup in `RPS1`/`RPROMPT` or inject text via `PROMPT_COMMAND`.

**Initial Input:**
Commands can pre-populate the input buffer (used by `fc` command for editing).

**Special Return Values:**
- `IC_READLINE_TOKEN_CTRL_C`: Returned when Ctrl+C is pressed on empty buffer
- `IC_READLINE_TOKEN_CTRL_D`: Returned when Ctrl+D is pressed on empty buffer (EOF)

### Non-TTY Mode

When standard input is not a terminal (pipe, file redirect, debugger, etc.):
- Line editing is disabled
- Input is read directly from the stream
- Prompts are still displayed but without styling
- Scripts and pipes work seamlessly

## Style Definitions

All visual aspects of the editor can be customized through style definitions.

### Available Styles

**Prompt Styles:**
- `ic-prompt`: Main prompt text
- `ic-linenumbers`: Line numbers in multiline mode
- `ic-linenumber-current`: Current line number highlight

**Syntax Highlighting Styles:**
- `keyword`: Shell keywords
- `builtin`: Builtin commands
- `system`: External commands resolved from PATH
- `unknown-command`: Unresolved command names
- `heredoc-delimiter`: Opening and closing heredoc markers
- `agent-prefix`, `agent-request`: Agent trigger prefixes and their natural-language requests
- `string`, `comment`, `variable`, `number`, `operator`
- `file-argument`, `path-exists`, `path-not-exists`, `glob-pattern`, `assignment-value`
- `command-substitution`, `arithmetic`, `option`, `function-definition`, `history-expansion`

**Interactive UI Styles:**
- `ic-hint`: Inline hints
- `ic-info`: Informational status text
- `ic-source`: Completion/history source labels
- `ic-diminish`: De-emphasized helper text
- `ic-emphasis`: Emphasized helper text
- `ic-error`: Editor/runtime error text
- `ic-bracematch`: Bracket match highlighting
- `ic-whitespace-char`: Visible whitespace markers

### Style Syntax

```bash
cjshopt style_def <style-name> "<bbcode-style>"
```

**Examples:**
```bash
# Make errors bright red and bold
cjshopt style_def ic-error "bold red"

# Use custom RGB color for commands
cjshopt style_def system "#00ff00"

# Style keywords with italic blue
cjshopt style_def keyword "italic blue"

# Combine multiple attributes
cjshopt style_def string "italic #ffaa00"
```

**Style Attributes:**
- Colors: `red`, `blue`, `green`, `yellow`, `cyan`, `magenta`, `white`, `black`
- ANSI colors: `ansi-red`, `ansi-bright-blue`, etc.
- RGB colors: `#RRGGBB` or `#RGB`
- Attributes: `bold`, `italic`, `underline`, `reverse`
- Underline color only: `underline-color=<color>` or `ansi-underline-color=<idx>` (also accepts
	shorthand `ulcolor`)
- Background: `on <color>`

## Configuration Examples

### Complete Editing Configuration

Here's a comprehensive example for `~/.cjshrc`:

```bash
# Multiline settings
cjshopt multiline on
cjshopt multiline-indent on
cjshopt line-numbers relative
cjshopt current-line-number-highlight on

# Completion settings
cjshopt completion-preview on
cjshopt completion-menu-expanded off
cjshopt auto-tab off
cjshopt completion-case off  # Case-insensitive completions
cjshopt completion-spell on
cjshopt completion-spell-enter off

# Hint settings
cjshopt hint on
cjshopt hint-delay 100  # 100ms delay before showing hints

# Help settings
cjshopt inline-help on

# History
cjshopt set-history-max 10000

# Key bindings
cjshopt keybind profile set emacs

# Syntax highlighting styles
cjshopt style_def keyword "bold blue"
cjshopt style_def system "green"
cjshopt style_def ic-error "bold red"
cjshopt style_def string "#ffaa00"
cjshopt style_def comment "italic #888888"
cjshopt style_def operator "bold"
```

### Minimal Configuration

For a minimal, fast setup (`--minimal` disables colors, completions and completion learning, syntax highlighting, smart `cd`, rc sourcing, the title line, history expansion, the status line, multiline line numbers, the startup time banner, error suggestions, prompt vars, and special lifecycle handlers):

```bash
# Use the --minimal flag at startup, or configure selectively:
cjshopt multiline on
cjshopt line-numbers off
cjshopt completion-preview off
cjshopt hint off
cjshopt inline-help off
```

## Performance Considerations

### Optimization Tips

1. **Disable unnecessary features** in resource-constrained environments
2. **Increase hint delay** if experiencing lag while typing
3. **Reduce history size** for faster search
4. **Use simpler prompt markup** to reduce rendering time

### Benchmarks

Isocline is designed to be fast and responsive:
- Completion generation: < 10ms for typical cases
- Syntax highlighting: Real-time with no noticeable lag
- History search: Fast even with large history files
- Multiline editing: Smooth for inputs up to hundreds of lines

## Troubleshooting

### Common Issues

**Completions not working:**
- Check if completions are enabled (not started with `--no-completions`)
- Verify PATH contains command directories
- Check file permissions

**Syntax highlighting not showing:**
- Ensure not started with `--no-syntax-highlighting`
- Check terminal color support
- Verify your prompt/style definitions (`cjshopt style_def`) are loaded

**Key bindings not working:**
- Check terminal emulator key sending
- Verify with `cjshopt keybind list`
- Some terminals may not support all key combinations

**Multiline issues:**
- Verify multiline is enabled: `cjshopt multiline status`
- Check for terminal compatibility
- Ensure terminal width is adequate

### Getting Help

- Press `F1` during input for interactive help
- Run `cjshopt <subcommand> --help` for command-specific help
- Check logs in debug mode
- Report issues on the cjsh GitHub repository

## External terminal settings

After each foreground command, CJSH preserves canonical external settings such as
`stty -isig`, `stty -icrnl`, `stty -opost`, and changes to interrupt, erase, and EOF
characters. These settings apply to following external commands. Echo is restored.
The editor keeps its own key bindings, input mappings and output modes, so changing
an external control character does not rebind editor keys.

Leaving noncanonical (`-icanon`/raw) mode behind is treated as an abandoned terminal
session: CJSH restores the previous external baseline. This policy also recovers the
prompt after a crash or stopped raw-mode application; `fg` restores the job's saved
modes before continuing it. Intentional persistent noncanonical mode is not supported.
