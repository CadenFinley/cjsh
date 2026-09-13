<!--
  thedetails.md

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

# Prompt Markup and Styling

CJ's Shell no longer relies on an external theme DSL. Prompt styling is handled directly through
the standard shell prompt variables (`PS1`, `RPS1`/`RPROMPT`, `PROMPT_COMMAND`, and
`PROMPT_EOL_MARK`) combined with inline BBCode-style markup. This page explains how the markup
works, which escape sequences are available, and how to persist your preferred prompt layout.

## Quick Start

- Set `PS1` to control the primary prompt. The default template shows a status arrow, an
  abbreviated working directory, and Git information:
  ```bash
  PS1='\S  [color=#5fd7ff]\p[/color] \g'
  ```
  Parent directories shorten to their first character while the final directory stays in full:
  `~/Documents/Github` becomes `~/D/Github`. Hidden parents keep their leading dot, so
  `~/.config/tmux` becomes `~/.c/tmux`.
  If an older cjsh process exports the previous built-in `\W` template, startup refreshes it to
  the current default. Custom inherited prompts and prompts set in startup files take precedence.
- Set `RPS1` (or `RPROMPT`) to control the right-aligned prompt. It is unset by default, so nothing
  renders on the right until you export one.
- Use `PROMPT_COMMAND` for commands that should run before each prompt.
- Use `PROMPT_EOL_MARK` to customize the marker shown when output leaves the cursor mid-line right
  before the next prompt.
- Apply markup directly inside these variables to style text, add colors, and align sections.

```bash
export PS1='[b hotpink]\u[/b] in [color=#87ceeb]\w[/color]\n$ '
export RPS1='[dim]\A[/dim]'
export PROMPT_COMMAND='__update_git_info'
export PROMPT_EOL_MARK='[b reverse][color=ansi-yellow]![/color][/b]'
```

> Markup is parsed by the isocline line editor. Invalid tags are ignored; run with
> `ISOCLINE_BBCODE_DEBUG=1` to print debugging information if you are experimenting.

## Markup Building Blocks

### Inline Tags

- `[b]…[/b]` – bold text
- `[i]…[/i]` – italics
- `[u]…[/u]` – underline
- `[r]…[/r]` – reverse video
- `[dim]…[/dim]`, `[strike]…[/strike]`, `[blink]…[/blink]`, `[hidden]…[/hidden]`
- `[color=<name|#RRGGBB>]…[/color]` – foreground color
- `[bgcolor=<name|#RRGGBB>]…[/bgcolor]` – background color
- `underline-color=<name|#RRGGBB>` or `ansi-underline-color=<idx>` – attribute value that recolors
  just the underline stroke (combine with any tag, e.g. `[u underline-color=#ffaa00]...[/u]`)
- `[ansi-sgr=sequence]…[/]` – inject raw SGR codes (advanced)
- `[width=columns;<align>;<fill>;dots]…[/]` – restrict content width (align is `left|center|right`)

Color names include the full HTML color table plus `ansi-` variants such as `ansi-red`,
`ansi-lightgray`, and `ansi-teal`. Hex values (`#ff69b4`) are also supported.

Tag attributes can be combined inline: `[b color=hotpink]…[/]` applies both bold and the color.

### Named Styles

The syntax highlighter exposes reusable style names that can be used as tags. The defaults come
from `token_constants::default_styles` and include:

```
unknown-command  agent-prefix  agent-request  colon  file-argument  path-exists  path-not-exists
glob-pattern     operator  keyword  builtin  system
variable         assignment-value  string  comment  heredoc-delimiter
command-substitution  arithmetic  option  number
function-definition  history-expansion
ic-prompt  ic-hint  ic-error  ic-info  ic-emphasis
ic-source  ic-diminish  ic-bracematch  ic-whitespace-char
ic-linenumbers  ic-linenumber-current
```

Use them as simple tags: `[ic-hint]⌛[/ic-hint]`. Redefine a style with
`cjshopt style_def <name> <style>` to change both the syntax highlighter and any markup that uses
that tag.

### Closing Tags

Use `[/tag]` to end a specific style or `[/]` to end the most recent open tag. You can also nest
tags freely; the parser keeps a stack so `[b][color=hotpink]...[/color][/b]` works as expected.

## Prompt Escape Sequences

Prompt templates use familiar POSIX/Bash escapes. CJ's Shell expands the following sequences:

- `\a` bell character
- `\d` locale-specific date (e.g., `Tue Mar 04`)
- `\D{fmt}` date formatted with `strftime`
- `\e` / `\E` escape character (`\033`)
- `\h` short hostname, `\H` full hostname
- `\j` number of background jobs
- `\l` current tty
- `\n` newline, `\r` carriage return
- `\s` shell name (`cjsh` by default)
- `\t` current time `HH:MM:SS`, `\T` 12-hour, `\@` 12-hour + AM/PM, `\A` `HH:MM`
- `\u` username
- `\v` short cjsh version, `\V` full version string
- `\w` working directory with `$HOME` shortened to `~`, `\W` basename of working directory
- `\p` abbreviated working directory: `$HOME` becomes `~`, parent directories shorten to their
  first character (after any leading dot), and the final directory remains in full
- `\S` status arrow used by the default theme (green when `$?` is zero, red otherwise)
- `\g` Git segment that renders `git:(branch)` plus a dirty marker; empty outside Git repos
- `\$` `#` for root, otherwise `$`
- `\?` exit status of last command
- `\\` literal backslash, `\[` and `\]` for zero-width control regions
- Octal escapes (`\033`) for arbitrary characters

All escape processing happens before markup is interpreted.

## Right-Aligned Prompt and Continuation Lines

- `RPS1` (preferred) or `RPROMPT` controls the inline right prompt. Markup and escapes work the
  same way as `PS1`.
- `PS1_FINAL` swaps the submitted prompt line after Enter accepts input. Pair it with
  `RPS1_FINAL` to style (or clear) the submitted line's right prompt independently.
- `PS2` is used automatically for continuation lines by the line editor. Set it if you want a
  custom secondary prompt, e.g. `export PS2='[dim]> [/dim]'`.
- `PS5` controls the fuzzy history search prompt, and `PS6` controls the command palette prompt.
  Both accept the same markup and escapes as `PS1`.
- `PROMPT_COMMAND`, when set, runs before CJ's Shell generates `PS1`/`RPS1`. Use it to refresh
  environment variables, collect Git metadata, or update the terminal title.

## Partial-Line Guard Marker (`PROMPT_EOL_MARK`)

- If a command prints output without a trailing newline and leaves the cursor mid-line, cjsh
  preserves that partial line before drawing the next prompt.
- To make the partial line boundary obvious, cjsh prints `PROMPT_EOL_MARK` and then forces a wrap
  using terminal padding and carriage returns (zsh-style behavior; not a literal newline write).
- Default marker: reverse+bold `%` for normal users, reverse+bold `#` for root.
- `PROMPT_EOL_MARK` supports prompt escapes and BBCode markup, so you can style it like any other
  prompt fragment.

```bash
# Use a custom visible marker
export PROMPT_EOL_MARK='[b reverse]![/]'

# Hide the marker text while still preserving the partial line
export PROMPT_EOL_MARK=''
```

## Prompt Layout Options

Use `cjshopt` to control how the prompt behaves after command execution:

```bash
cjshopt prompt-newline on|off                 # Always print a blank line after commands
cjshopt right-prompt-follow-cursor on|off     # Make the inline right prompt follow the cursor row
```

Each toggle prints guidance on how to persist the setting. Add the chosen command to `~/.cjshrc`
to make it stick.

## Examples

```bash
# Minimal prompt that still highlights errors
export PS1='[ic-error]\$[/ic-error] '

# Git-aware prompt via PROMPT_COMMAND
prompt_git_info() {
  if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    local branch
    branch=$(git branch --show-current 2>/dev/null)
    export GIT_SEG="[color=gold](${branch:-detached})[/color] "
  else
    unset GIT_SEG
  fi
}
export PROMPT_COMMAND=prompt_git_info
export PS1='[b]\u[/b] [color=#6cb6ff]\w[/color] ${GIT_SEG}\$ '

# Right prompt that shows the time and exit code when non-zero
export RPS1='[dim]\A[/dim][if $?>0][space][ic-error]✗ $?[ic-error][/if]'
```

The last example demonstrates mixing markup with shell substitutions. Because prompt evaluation
occurs inside CJ's Shell, you can use regular shell parameter expansion to build dynamic strings
before markup is parsed.

## Persisting Configuration

- Add `export` statements to `~/.cjshrc` for prompt variables.
- Use `cjshopt style_def` in the same file to adjust highlight palettes.
- When creating configuration files through `cjshopt generate-rc` or `cjshopt generate-profile`,
  you can drop your prompt definitions into the generated files directly.

That is all you need to theme CJ's Shell now—no external DSL, no extra tooling. Compose the prompt
you want with markup, save it in your rc files, and CJSH will handle the rest.
