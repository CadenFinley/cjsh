<!--
  commands.md

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

# Built-in Commands Reference

CJ's Shell provides a comprehensive set of built-in commands that are available without requiring external programs. These commands are optimized for both interactive use and shell scripting.

## Navigation and File System

### cd
Change the current working directory.

```bash
cd [directory]
```

- Use `cd` without arguments to go to home directory
- Use `cd -` to switch to the previous directory
- Smart CD is enabled by default: if a single fuzzy match exists, cjsh jumps to it
- Disable smart cd with `cjshopt smart-cd off` or `cjsh --no-smart-cd`

### approot
Jump to cjsh application directories.

```bash
approot [-p|--print] [-f|--file] [target]
```

- No argument defaults to `config` (`~/.config/cjsh`)
- `-p` / `--print` prints the resolved directory instead of changing to it
- `-f` / `--file` prints file-backed target paths and implies `--print`
- `cache` jumps to `~/.cache/cjsh`
- `history` jumps to the directory containing the history file (defaults to `~/.cache/cjsh`)
- `firstboot` / `first_boot` jumps to the directory containing the first-boot marker (`~/.cache/cjsh/.first_boot`)
- `completions` jumps to `~/.cache/cjsh/generated_completions`
- `env` / `cjshenv` jumps to the directory containing `$CJSH_ENV` when that variable is set and non-empty; otherwise it prefers `~/.cjshenv` and falls back to `~/.config/cjsh/.cjshenv` when the primary file is missing
- `profile` / `cjprofile` jumps to the directory containing `~/.cjprofile`, falling back to `~/.config/cjsh/.cjprofile` when the primary file is missing
- `rc` / `cjshrc` jumps to the directory containing `~/.cjshrc`, falling back to `~/.config/cjsh/.cjshrc` when the primary file is missing
- `logout` / `cjlogout` jumps to the directory containing `~/.cjlogout`, falling back to `~/.config/cjsh/.cjlogout` when the primary file is missing
- `home` jumps to your home directory
- `cjsh` jumps to the directory containing the active `cjsh` executable (symlinks are resolved)
- With `--file`, file-backed targets print their file paths (for example `history.txt`, `~/.cjshrc`, or the `cjsh` executable path)

Examples:

```bash
approot history             # cd ~/.cache/cjsh
approot --file history      # print ~/.cache/cjsh/history.txt
approot --file rc           # print ~/.cjshrc
```

### firstboot

Suppress the welcome banner shown on a new installation.

```bash
firstboot
```

The builtin creates `~/.cache/cjsh/.first_boot` and succeeds only if that marker does not
already exist. It has no arguments.

### pushd
Push the current directory onto a stack and change directories.

```bash
pushd [directory]
```

- With no arguments, swaps the current directory with the top of the stack
- The stack is shared with `popd` and `dirs`

### popd
Pop the top directory from the stack and change to it.

```bash
popd
```

- Errors if the directory stack is empty

### dirs
Display the directory stack.

```bash
dirs
```

- Prints the current directory followed by stacked entries

### pwd
Print the current working directory.

```bash
pwd [-L|-P]
```

- `-L` uses the logical path from `PWD` and is the default
- `-P` resolves the physical path without symlinks

## Text Output

### echo
Print arguments separated by spaces.

```bash
echo [args...]
```

### printf
Format and print data using printf-style specifiers.

```bash
printf format [arguments...]
```

## Shell Control

### exit / quit / bye
Leave the shell with an optional exit status.

```bash
exit [-f|--force] [n]
quit [-f|--force] [n]
bye [-f|--force] [n]
```

- In `--posix` mode, use `exit`; `quit` and `bye` are disabled.
- By default, cjsh asks for a consecutive second exit only when running or stopped jobs exist.
- Configure that guard with `cjshopt exit-confirmation smart|always|never`; `smart` is the default,
  `always` confirms every exit, and `never` exits immediately.
- Ctrl+D on an empty command line uses the same confirmation policy as `exit`.
- Without a status, cjsh uses the last foreground command's status. Consecutive exit attempts
  preserve the status from the first attempt unless you supply a new one.
- `--force` bypasses confirmation and retains normal shutdown handlers and cleanup.
- Status operands must be signed integers and are reduced to 0–255. Invalid or oversized
  integers exit with status 2; extra operands report an error with status 1 and keep the shell running.

### help
Display the CJSH command reference.

```bash
help
```

### version
Show cjsh version information.

```bash
version
```

### restart
Re-exec the current cjsh process.

```bash
restart [-n|--no-flags]
```

- By default, reuses the original startup arguments (for example, `--posix` or `--minimal`)
- `-n` / `--no-flags` restarts as plain `cjsh` (drops original startup flags and launch arguments)
- Current working directory and exported environment are preserved across the restart
- Restart preserves `SHLVL`, since it replaces the current shell process.

## Script Execution

### source / .
Execute commands from a file in the current shell context.

```bash
source filename
. filename
```

When you execute a script file directly (for example, `./script.sh`) CJSH can infer an interpreter
from the file extension if the file has no shebang. Toggle this with
`cjshopt script-extension-interpreter` or `--no-script-extension-interpreter`.

### eval
Evaluate a string as shell code.

```bash
eval string [string...]
```

### exec
Replace the shell process with another program.

```bash
exec command [args...]
```

Replacement preserves the nesting level when the next program is a shell. External programs
receive ordinary signal dispositions, while inherited and explicitly ignored signals stay ignored.
If replacement fails, cjsh restores its signal state and `SHLVL` and reports the error.

## Variables and Environment

A failed replacement terminates a noninteractive POSIX shell (127 for a missing command,
126 for a non-executable file). Native and interactive shells recover and continue with
the failure status. Successful replacement does not run exit hooks or logout files.

### export
Set or display environment variables.

```bash
export [-p] [name[=value]...]
```

### unset
Remove environment variables.

```bash
unset [-n|-v] name [name...]
```

### local
Declare local variables inside functions.

```bash
local [-aAnrx] name[=value] [name[=value]...]
```

### declare / typeset
Set variable attributes and values.

```bash
declare [-aAnFfgprx] [name[=value] ...]
typeset [-aAnFfgprx] [name[=value] ...]
```

- `typeset` is an alias for `declare`
- `-g` forces global scope inside functions
- `-x` marks variables exported (`+x` removes export)
- `-r` marks variables readonly
- `-a` declares indexed arrays
- `-A` declares associative arrays
- `-n` declares namerefs; `unset -n name` removes the reference rather than its target
- `-f`/`-F` operate on shell functions
- `-p` prints declarations

### readonly
Mark variables as read-only.

```bash
readonly [-pf] name[=value] [name[=value]...]
```

### set
Adjust shell options or positional parameters.

```bash
set [options] [args...]
```

- `huponexit` is enabled by default for interactive shells (including `-i -c`) and disabled
  by default for noninteractive shells. When enabled, normal exit sends SIGHUP to managed jobs.
  Use `set +o huponexit` to keep the previous interactive default, or `disown`/`disown -h`
  to exempt individual jobs. Add the opt-out to your interactive rc file if you depended on
  background helpers surviving shell exit. `set -o huponexit` explicitly enables it in scripts.
- HUP cleanup resumes stopped jobs so they can handle the signal. Jobs that handle or ignore
  HUP may survive; cjsh does not escalate to SIGKILL and waits at most 100 ms for child cleanup.
- `set -m` enables monitor mode and per-job process groups; `set +m` disables it. Interactive
  shells start with monitor mode enabled.

### coproc

Run a command asynchronously with a two-way pipe.

```bash
coproc command [args...]
coproc NAME { command; }
```

The read and write descriptors are assigned to `COPROC[0]` and `COPROC[1]`, or to the named
array. The child PID is available as `COPROC_PID` or `NAME_PID`. Use `read -u fd` or dynamic
descriptor redirections such as `>&${COPROC[1]}` to communicate with it.

### shift
Rotate positional parameters to the left.

```bash
shift [n]
```

## Aliases

### alias
Create or list command aliases.

```bash
alias [-p] [name[=value]...]
```

### unalias
Remove command aliases.

```bash
unalias [-a] [name...]
```

## Abbreviations

### abbr
Create, update, or list fish-style abbreviations that expand during interactive editing.

```bash
abbr [name=expansion ...]
```

- Run without arguments to display all configured abbreviations
- Use `name=expansion` pairs to set or update entries
- Triggers that contain whitespace are rejected
- Abbreviations expand when the trigger is followed by whitespace or when the line is submitted
- Two defaults are shipped with cjsh: `abbr` → `abbreviate` and `unabbr` → `unabbreviate`

### unabbr
Remove one or more fish-style abbreviations.

```bash
unabbr name [name...]
```

- Removing a non-existent abbreviation reports an error but continues processing the rest
- Pair with `abbr` to keep a clean set of triggers in your session configuration

## Control Flow

### if
Run conditional blocks in scripts.

```bash
if condition; then
    commands
[elif condition; then
    commands]
[else
    commands]
fi
```

### test / [
Evaluate POSIX test expressions.

```bash
test expression
[ expression ]
```

### [[
Evaluate extended test expressions (bash-style).

```bash
[[ expression ]]
```

### break
Exit the current loop.

```bash
break [n]
```

### continue
Skip to the next loop iteration.

```bash
continue [n]
```

### return
Exit the current function with an optional status.

```bash
return [n]
```

### :
No-op command that always succeeds.

```bash
:
```

### true
Always succeed and return exit status 0. Handy for conditionals or resetting `$?`.

```bash
true
```

### false
Always fail and return exit status 1. Useful for testing error paths or short-circuiting scripts.

```bash
false
```

## Job Control

cjsh reclaims the terminal and restores its input settings when returning to the prompt,
including with monitor mode disabled (`set +m`). This also happens around prompt and idle
hooks, so a program that exits or stops with altered terminal settings does not leave the
next prompt using them. Pending input is preserved, and `fg` restores a stopped job's own
terminal settings before resuming it.

### jobs
List background jobs.

```bash
jobs [-lprs] [job_spec|pid...]
```

`-r` and `-s` select running and stopped jobs. `-p` prints one process-group leader per job.

While you edit a command, job stop and completion notifications appear above the prompt.
Your input, cursor position, and undo history are preserved. Notifications wait until an
open completion/search menu or bracketed paste finishes before the prompt is redrawn.

### fg
Bring a job to the foreground.

```bash
fg [job_spec|pid]
```

### bg
Resume a job in the background.

```bash
bg [job_spec|pid...]
```

### Auto-background on Ctrl+Z
Append `&^` to a command to mark it for automatic backgrounding the first time you press
`Ctrl+Z`.

Append `&^!` to auto-background on `Ctrl+Z` and discard stdout/stderr after the suspend so the
prompt stays clean. Output is restored if you bring the job back with `fg`.

```bash
long_running_task &^
long_running_task &^!
```

### wait
Wait for jobs or processes to finish.

```bash
wait [-fn] [-p variable] [pid|job_spec...]
```

`wait -n` returns when the next selected job changes state, `-f` waits through stops until
termination, and `-p` stores the selected process-group leader or PID.

### kill
Send signals to jobs or processes.

```bash
kill [-s signal|-n signum|-signal] pid|job_spec [...]
kill -l
```

### disown
Detach jobs from the shell so they are no longer listed or sent hangup signals when cjsh exits.

```bash
disown [-arh] [job_spec|pid...]
```

- With no arguments, the current job is disowned
- `-a/--all` selects every tracked job; `-r/--running` restricts the selection to running jobs
- `-h` leaves selected jobs in the table but excludes them from SIGHUP propagation
- Job specs include `%N`, `%+`, `%-`, `%prefix`, and `%?substring`; a tracked PID is also accepted
- Disowned jobs continue running even if `set -o huponexit` is enabled later in the session

### suspend
Suspend the current interactive shell until its parent resumes it with `fg`.

```bash
suspend [-f]
```

Login shells require `-f`. The command requires an owned controlling terminal and is
unavailable in POSIX mode. It restores external terminal modes before stopping the shell;
a background continuation waits for foreground ownership before returning to the prompt.
The parent can use the terminal while the nested shell is stopped. It does not suspend
or resume unrelated jobs.

### jobname
Assign or update a friendly display name for a tracked job. The name shows up in `jobs`, `fg`, `bg`,
and completions while the job remains in the table.

```bash
jobname JOB_SPEC NEW_NAME
jobname JOB_SPEC [-c|--clear]
```

- `JOB_SPEC` can be a `%job_id`, a PID, or any command prefix that normally resolves jobs (`fg`
and `bg`-style matching)
- `NEW_NAME` is treated as the rest of the command line after `JOB_SPEC`, so spaces are allowed
- Names must contain at least one non-whitespace character; use `-c` or `--clear` to revert

## Signal Handling

### trap
Set signal handlers or list existing traps.

```bash
trap [action] [signal...]
```

## Command Information

### type
Explain how a command name will be resolved.

```bash
type name [name...]
```

### which
Locate commands, including aliases, builtins, and executables in PATH.

```bash
which name [name...]
```

### hash
Cache command lookups or display the cache.

```bash
hash [-r] [name...]
```

### generate-completions
Regenerate cached completion metadata for external commands.

```bash
generate-completions [--quiet] [--no-force] [-j jobs] [command ...]
```

- Runs against every executable discoverable in `PATH` when no command list is supplied
- `--quiet` suppresses per-command status messages and prints only a summary (failures are listed)
- `--no-force` keeps existing cache files and only generates data for missing entries
- `-j/--jobs` limits the number of commands processed simultaneously (defaults to CPU count)
- Interactive terminal runs redraw a progress bar beneath live status lines; redirected output
  keeps plain line-by-line status for scripting
- Summary output includes total elapsed time for the run
- Use `--` to end option parsing when processing command names that start with a dash

### builtin
Run a builtin directly, bypassing functions and PATH.

```bash
builtin command [args...]
```

### command
Execute a command while bypassing shell functions or print information about it.

```bash
command [-pVv] COMMAND [ARG...]
```

**Options:**
- `-p` – Temporarily use the default `PATH` of `/usr/bin:/bin` when resolving `COMMAND`
- `-v` – Print a short description of how `COMMAND` would be resolved
- `-V` – Print a verbose description (builtin, full path, or not found)

When no inspection flags are supplied, `command` runs the target using the shell's execution
engine, allowing you to bypass shell functions that shadow external commands. The command returns
the exit status of the invoked program.

## Hook System

### hook
Manage shell hooks that run at key lifecycle moments.

```bash
hook <add|remove|list|clear> [hook_type] [function_name]
```

Hook types include `precmd`, `preexec`, `chpwd`, and `idle`. Idle hooks require a timeout configured
with `cjshopt idle-timeout`; they run in the foreground and preserve pending editor input. Use
`hook add` inside configuration files to register functions and `hook list` to inspect active hooks. See
[`hooks.md`](hooks.md) for complete examples and best practices.

## Input/Output

### read
Read user input into variables.

```bash
read [options] name [name...]
```

Use `read -u fd name` to read from a numeric file descriptor, including a descriptor published by
`coproc`.

### getopts
Parse positional parameters as short options.

```bash
getopts optstring name [args...]
```

## History

### history
Display command history.

```bash
history [n]
```

- Without arguments, displays all history entries
- With a number `n`, displays the last `n` entries
- History is stored in `~/.cache/cjsh/history.txt`

### fc
Fix Command - edit and re-execute commands from history (POSIX-compliant).

```bash
fc [-e editor] [-lnr] [first [last]]
fc -s [old=new] [command]
fc (-c|--command) command_string
```

**Options:**
- `-e editor` - Use specified editor (default: `$FCEDIT`, `$EDITOR`, or `nano`)
- `-l` - List commands instead of editing
- `-n` - Suppress line numbers when listing
- `-r` - Reverse order of commands when listing
- `-s` - Re-execute a matching command, with optional `old=new` substitution
- `-c string`, `--command string` - Open the editor with `string` as initial content

**Arguments:**
- `first` - First command to edit/list (default: previous command)
- `last` - Last command to edit/list (default: same as first)

**Examples:**

```bash
# Edit the previous command in your editor
fc

# Edit command number 53
fc 53

# Edit commands 10 through 15
fc 10 15

# List last 16 commands
fc -l

# List commands 10 through 20
fc -l 10 20

# Edit with a specific editor
fc -e nano

# Use with environment variables
export FCEDIT=nano
fc  # Will use nano as the editor
```

**How it works:**
1. Opens the specified command(s) in your editor
2. When you save and exit the editor, the modified command is displayed
3. The modified command is automatically executed
4. The result is added to history

**Environment Variables:**
- `FCEDIT` - Preferred editor for `fc` (checked first)
- `EDITOR` - Fallback editor if `FCEDIT` is not set
- Default is `nano` if neither variable is set

**Tip:** For a better editing experience, set your preferred editor:
```bash
export FCEDIT=vi     # or nano, emacs, etc.
export EDITOR=vi     # fallback for other tools too
```

## System Information

### times
Show CPU usage for the shell and its children.

```bash
times
```

### umask
Show or set the file creation mask.

```bash
umask [mode]
```

### ulimit
Inspect or adjust resource limits for the current shell and any processes it launches.

```bash
ulimit [-HS] [-a] [-f | -n | -t | ...] [limit]
```

- `-a` lists every supported limit along with the active hard/soft values.
- Use `-H` or `-S` to operate on hard or soft limits respectively. With no selector flags, reads default to soft and writes update both hard and soft limits.
- Resource selectors mirror the underlying OS (`-f` file size, `-n` open files, `-p` pipe size, `-t` CPU time, `-v` virtual memory, etc.). Unsupported selectors print a descriptive error.
- Limits accept numeric values, or the keywords `unlimited`, `hard`, or `soft`.

Run `ulimit --help` for the full selector table, including unsupported entries on your current platform.

## Theming and Customization


### Prompt Styling
cjsh reads the standard prompt variables, so customizing the look is as simple as exporting new values inside your configuration files:

```bash
export PS1='[b]\u[/b] [color=#87ceeb]\w[/color]\n$ '
export RPS1='[ic-hint]\A[/ic-hint]'
export PROMPT_COMMAND='__update_git_info'
export PROMPT_EOL_MARK='[b reverse][color=ansi-yellow]![/color][/b]'
```

Add snippets like these to `~/.cjshrc` (or any sourced config) to persist them across sessions. See [Prompt Markup and Styling](../themes/thedetails.md) for the full markup reference plus additional examples.

`PROMPT_EOL_MARK` controls the marker shown when command output does not end with a newline and the
next prompt needs to preserve a partial line. By default cjsh shows a reverse+bold `%` for normal
users and `#` for root, then forces a terminal wrap with padding/carriage returns (zsh-style,
without printing a literal newline).

### cjshopt
Generate config files and adjust cjsh options.

```bash
cjshopt <subcommand> [options]
```

Available subcommands:
- `style_def` - Define or redefine syntax highlighting styles
- `completion-case` - Configure completion case sensitivity
- `history-search-case` - Configure fuzzy history case sensitivity
- `history-directory` - Scope interactive history to the current directory
- `history-directory-subdirs` - Include nested directories in history scope
- `completion-spell` - Toggle spell correction suggestions in completions
- `completion-spell-enter` - Toggle Enter-triggered spell autocorrection when exactly one spell match exists
- `completion-learning` - Toggle automatic completion learning from man pages
- `exit-confirmation` - Configure when `exit`, `quit`, and `bye` require confirmation
- `smart-cd` - Toggle fuzzy auto-jumps for `cd`
- `extglob` - Toggle Bash-style extended glob patterns (`?()`, `*()`, `+()`, `@()`, `!()`)
- `script-extension-interpreter` - Toggle extension-based script runners
- `line-numbers` - Configure line numbers in multiline input (on/off/relative/absolute)
- `line-numbers-continuation` - Keep line numbers when a continuation prompt is set
- `line-numbers-replace-prompt` - Replace the final prompt line with the line-number gutter
- `current-line-number-highlight` - Toggle highlighting of the current line number
- `multiline-start-lines` - Configure how many prompt lines are preallocated in multiline mode
- `multiline-max-lines` - Limit how many multiline input rows are visible at once
- `menu-max-lines` - Limit visible content rows in all isocline menus
- `multiline-bottom-lines` - Configure the input and menu scroll margin
- `hint-delay` - Set hint display delay in milliseconds
- `idle-timeout` - Run idle hooks after a period without terminal input
- `completion-preview` - Configure completion preview
- `completion-click-accept` - Configure whether click interactions accept completion candidates
- `menu-highlighting` - Syntax-highlight completion and history menu items
- `visible-whitespace` - Toggle visible whitespace characters in the editor
- `line-wrap-marker` - Customize the character at wrapped line ends
- `hint` - Configure inline hints
- `multiline-indent` - Configure auto-indent in multiline input
- `multiline` - Configure multiline input mode
- `inline-help` - Configure inline help messages
- `status-hints` - Configure the status hint banner visibility
- `status-line` - Toggle the entire status row beneath the prompt
- `status-reporting` - Toggle cjsh validation output in the status row
- `status-line-callback` - Run a shell function that supplies custom status-line text
- `mouse-clicking` - Configure mouse capture for prompt editing and interactive menus
- `mouse-clicking-status-line` - Show or hide the mouse-clicking status indicator text
- `auto-tab` - Configure automatic tab completion
- `prompt-newline` - Force a blank line after each command
- `right-prompt-follow-cursor` - Keep the inline right prompt aligned with the active cursor row
- `agent-mode` - Configure user-provided executors for agent-assisted command writing
- `keybind` - Inspect or modify key bindings (changes apply immediately; add to `~/.cjshrc` to persist)
- `generate-env` - Create or overwrite ~/.cjshenv (use `--alt` for `~/.config/cjsh/.cjshenv`)
- `generate-profile` - Create or overwrite ~/.cjprofile (use `--alt` for `~/.config/cjsh/.cjprofile`)
- `generate-rc` - Create or overwrite ~/.cjshrc (use `--alt` for `~/.config/cjsh/.cjshrc`)
- `generate-logout` - Create or overwrite ~/.cjlogout (use `--alt` for `~/.config/cjsh/.cjlogout`)
- `set-history-max` - Configure history persistence limits
- `set-completion-max` - Limit the maximum number of completion suggestions shown

#### exit-confirmation

Control whether an exit request must be repeated on the next command line before cjsh exits.

```bash
cjshopt exit-confirmation smart   # Confirm only when active jobs exist (default)
cjshopt exit-confirmation always  # Confirm every exit request
cjshopt exit-confirmation never   # Never require confirmation
cjshopt exit-confirmation status  # Show the current mode
```

Add the selected command to `~/.cjshrc` to persist it. `exit --force` bypasses every mode.
In `always` mode, an active-job warning counts as the confirmation request, so the next
consecutive exit proceeds without another prompt.

#### agent-mode

Configure user-provided AI executors without giving CJSH provider credentials. See
[Agent-Assisted Command Writing](editing.md#agent-assisted-command-writing) for the executor JSON
protocol, trigger prefixes, menu behavior, and complete examples.

```bash
cjshopt agent-mode set --command <command> [--system-prompt <text>] [--trigger-prefix <prefix>]
cjshopt agent-mode status
cjshopt agent-mode on|off
cjshopt agent-mode key <key|default|off|status>
cjshopt agent-mode clear [--default|--trigger-prefix <prefix>|--all]
cjshopt agent-mode reset
```

### cjsh-widget
Interact with the embedded line editor (isocline) to drive advanced key bindings.

```bash
cjsh-widget <subcommand> [...]
```

Available subcommands include:
- `get-buffer` / `set-buffer` – Read or replace the active input buffer
- `get-cursor` / `set-cursor` – Inspect or move the cursor (byte offsets)
- `insert` / `append` / `clear` – Modify buffer contents near the cursor or reset the line
- `action <name>` – Execute a built-in editor action such as `cursor-up`
- `accept` – Simulate pressing Enter to submit the current buffer

These commands are primarily used from custom key bindings and widgets rather than typed
interactively. Combine them with `cjshopt keybind ext` inside `~/.cjshrc` to create bespoke
editing behaviors.

#### completion-case

Toggle whether tab completions treat case as significant. Synonyms such as `enable`, `disable`, `true`, and `false` are also accepted for convenience.

```bash
cjshopt completion-case <on|off|status>
```

Examples:

```bash
cjshopt completion-case on      # Case-sensitive matching
cjshopt completion-case off     # Case-insensitive matching (default)
cjshopt completion-case status  # Show the current mode
```

Add the command to `~/.cjshrc` if you want the preference remembered across sessions.

#### history-search-case

Control whether the fuzzy history search menu (`Ctrl+R`/`Ctrl+S`) treats letter case as significant. When case sensitivity is enabled (the default), `make` and `Make` are treated as different queries. Turning it off allows uppercase queries to match lowercase history entries and vice versa. Synonyms such as `enable`, `disable`, `true`, `false`, and `--status` are supported for convenience, and you can always press `Alt+C` while the menu is open to flip the setting temporarily.

```bash
cjshopt history-search-case <on|off|status>
```

Examples:

```bash
cjshopt history-search-case on      # Require exact casing in the fuzzy history menu (default)
cjshopt history-search-case off     # Match history entries case insensitively
cjshopt history-search-case status  # Show the current setting
```

Add the command to `~/.cjshrc` to persist the preference.

#### history-directory / history-directory-subdirs

Scope arrow-key recall, fuzzy history search, and history completions to the current working
directory. Both settings default to `off` and accept `on`, `off`, `status`, and the same synonyms
as `history-search-case`.

```bash
cjshopt history-directory on             # Use commands run in the current directory
cjshopt history-directory off            # Use history from all directories (default)
cjshopt history-directory status         # Show the current setting
cjshopt history-directory-subdirs on     # Also include commands from nested directories
cjshopt history-directory-subdirs off    # Match only the current directory (default)
cjshopt history-directory-subdirs status # Show the nested-directory setting
```

With both options enabled in `/work/project`, commands from `/work/project/src` are included.
Commands from `/work/project-other` are excluded. In `/work/project/src`, commands recorded in
`/work/project` are excluded. The nested-directory setting takes effect when directory scope is on.

Inside the fuzzy history menu, `Alt+D` toggles directory scope and `Alt+N` toggles nested directories
for that menu only. Its status line shows both settings. Add the `cjshopt` commands to `~/.cjshrc`
to apply your preferences in future sessions.

New history records always capture the physical working directory before command execution,
including when filtering is off. Symlink paths to the same directory share a scope. Older records
without directory metadata remain available in global history and are excluded from directory
scope. `history`, `fc`, and `!` expansion continue to use the full history.

#### completion-spell

Enable, disable, or inspect spell correction inside the completion engine. When enabled, cjsh will try to fix minor typos before offering suggestions. The subcommand also accepts synonyms such as `enable`, `disable`, `true`, `false`, and `--status`.

```bash
cjshopt completion-spell <on|off|status>
```

Examples:

```bash
cjshopt completion-spell on       # Turn on spell correction
cjshopt completion-spell status   # Display the current state
```

Persist the choice by placing the command in `~/.cjshrc`.

#### completion-spell-enter

Enable, disable, or inspect Enter-triggered spell autocorrection. When enabled, pressing Enter will
apply a spell correction before submission if there is exactly one spell-sourced correction
candidate. The subcommand accepts synonyms such as `enable`, `disable`, `true`, `false`, and
`--status`.

```bash
cjshopt completion-spell-enter <on|off|status>
```

Examples:

```bash
cjshopt completion-spell-enter on       # Auto-apply a single spell correction on Enter
cjshopt completion-spell-enter status   # Show the current setting
```

Persist the choice by placing the command in `~/.cjshrc`.

#### completion-learning

Control whether cjsh scrapes man pages on demand to learn completions for external commands. When disabled, cjsh uses cached completion files and builtin metadata only. The subcommand accepts synonyms such as `enable`, `disable`, `true`, `false`, and `--status`.

```bash
cjshopt completion-learning <on|off|status>
```

Examples:

```bash
cjshopt completion-learning on       # Allow on-demand man-page scraping (default)
cjshopt completion-learning off      # Disable new man-page lookups
cjshopt completion-learning status   # Show the current setting
```

Add the command to `~/.cjshrc` to persist the preference.

#### smart-cd

Enable, disable, or inspect smart cd auto-jumps. When enabled, `cd` will auto-jump to a single
fuzzy match (e.g., a nearby directory with a close name). Synonyms such as `enable`, `disable`,
`true`, `false`, and `--status` are supported.

```bash
cjshopt smart-cd <on|off|status>
```

Examples:

```bash
cjshopt smart-cd on       # Enable smart cd auto-jumps
cjshopt smart-cd off      # Disable smart cd auto-jumps
cjshopt smart-cd status   # Show the current setting
```

Add the command to `~/.cjshrc` to persist the preference.

#### script-extension-interpreter

Enable, disable, or inspect extension-based script runners. When enabled, cjsh can infer the
interpreter from a script's file extension when there is no shebang (for example, `.sh` → `sh`,
`.bash` → `bash`, `.zsh` → `zsh`, `.ksh` → `ksh`). Synonyms such as `enable`, `disable`, `true`,
`false`, and `--status` are supported.

```bash
cjshopt script-extension-interpreter <on|off|status>
```

Examples:

```bash
cjshopt script-extension-interpreter on       # Enable extension-based script runners
cjshopt script-extension-interpreter off      # Disable extension-based script runners
cjshopt script-extension-interpreter status   # Show the current setting
```

Add the command to `~/.cjshrc` to persist the preference.

#### line-numbers

Enable, disable, or inspect line numbers in multiline input mode. When enabled, cjsh will display numbers on the left side of multiline input, making it easier to navigate and edit multi-line commands or scripts. You can choose between absolute numbering (the default) or relative numbering, which shows the distance to the active cursor line.

```bash
cjshopt line-numbers <on|off|relative|absolute|status>
```

Examples:

```bash
cjshopt line-numbers on       # Enable line numbers in multiline input
cjshopt line-numbers relative # Switch to relative numbering
cjshopt line-numbers off      # Disable line numbers in multiline input
cjshopt line-numbers status   # Show the current setting
```

Add the command to `~/.cjshrc` to persist the setting across sessions. The subcommand also accepts synonyms such as `enable`, `disable`, `true`, `false`, `absolute`, and `rel`/`relative`.

> **Tip:** Style the line numbers themselves with `cjshopt style_def ic-linenumbers "color=#FFB86C"` (or any other style). See `cjshopt style_def` for the full list of supported style directives.

#### line-numbers-continuation

Control whether multiline line numbers remain visible when a continuation prompt marker (PS2) is configured. By default, configuring a continuation prompt hides the numbers; enabling this option keeps them aligned with the continuation marker instead.

```bash
cjshopt line-numbers-continuation <on|off|status>
```

Examples:

```bash
cjshopt line-numbers-continuation on      # Keep line numbers even with custom continuation prompts
cjshopt line-numbers-continuation off     # Hide line numbers whenever a continuation prompt exists
cjshopt line-numbers-continuation status  # Show the current setting
```

Add the command to `~/.cjshrc` to persist the setting across sessions. Synonyms such as `enable`, `disable`, `true`, `false`, and `--status` are also accepted.

#### line-numbers-replace-prompt

Swap the final line of a multi-line prompt (the row that normally precedes your input) with the line-number gutter. This option only takes effect when line numbers are active and either no custom `PS2` is configured or `cjshopt line-numbers-continuation on` is in effect.

```bash
cjshopt line-numbers-replace-prompt <on|off|status>
```

Examples:

```bash
cjshopt line-numbers-replace-prompt on      # Replace the final prompt line with the numeric gutter
cjshopt line-numbers-replace-prompt off     # Keep the final prompt line visible
cjshopt line-numbers-replace-prompt status  # Show the current setting
```

Add the command to `~/.cjshrc` to persist the setting across sessions.

#### current-line-number-highlight

Enable or disable highlighting of the current line number in multiline input mode. When enabled (default), the line number for the line containing the cursor is displayed in a different style than other line numbers.

```bash
cjshopt current-line-number-highlight <on|off|status>
```

Examples:

```bash
cjshopt current-line-number-highlight on      # Enable current line highlighting
cjshopt current-line-number-highlight off     # Disable current line highlighting
cjshopt current-line-number-highlight status  # Show the current setting
```

Add the command to `~/.cjshrc` to persist the setting across sessions. Accepts synonyms like `enable`, `disable`, `true`, and `false`.

> **Tip:** Customize the current line number style with `cjshopt style_def ic-linenumber-current "bold color=#FFB86C"` to make it stand out from regular line numbers styled with `ic-linenumbers`.

#### multiline-start-lines

Configure how many prompt lines are preallocated when multiline input is enabled. This controls how many rows are shown before you start typing.

```bash
cjshopt multiline-start-lines <count|status>
```

Examples:

```bash
cjshopt multiline-start-lines 3      # Reserve three prompt lines in multiline mode
cjshopt multiline-start-lines 1      # Restore the default
cjshopt multiline-start-lines status # Show the current setting
```

Add the command to `~/.cjshrc` to persist the setting across sessions.

#### multiline-max-lines

Limit the multiline input viewport. Commands longer than the configured number of display rows
scroll to keep the cursor visible; the complete command remains editable and is submitted unchanged.
The default is 15 rows. Completion menus, status text, and other helper rows use separate screen
space and do not count toward this limit.

```bash
cjshopt multiline-max-lines <count|status>
```

Examples:

```bash
cjshopt multiline-max-lines 10     # Show up to ten input rows
cjshopt multiline-max-lines 15     # Restore the default
cjshopt multiline-max-lines status # Show the current limit
```

Values are clamped to the supported range of 1 through 256. Add the command to `~/.cjshrc` to
persist the setting across sessions.

#### menu-max-lines

Limit visible content rows in completion, history, command palette, and custom menus. The default
is 50 rows, including expanded item previews. Headers and help text use separate rows, and menus
shrink to fit the terminal.

```bash
cjshopt menu-max-lines 8       # Show up to eight menu content rows
cjshopt menu-max-lines 50      # Restore the default
cjshopt menu-max-lines status  # Show the current limit
```

The count must be a positive integer; values above 256 are clamped to 256. Add the command to
`~/.cjshrc` to persist the setting. Use `cjshopt multiline-bottom-lines` to adjust the shared
scroll margin, which defaults to 3 rows.

#### multiline-bottom-lines

Configure a symmetric cursor margin within the multiline viewport. The editor keeps up to this many
existing input rows visible below the cursor when moving down and above it when moving up. The
viewport stays fixed while the cursor is within those margins. It does not add blank rows when the
command has less remaining content. The default is 3. This setting also controls the scroll margin
around the selected item in completion, history, command palette, and custom menus.

```bash
cjshopt multiline-bottom-lines <count|status>
```

Examples:

```bash
cjshopt multiline-bottom-lines 3      # Keep a three-row cursor margin
cjshopt multiline-bottom-lines 0      # Disable the cursor margin
cjshopt multiline-bottom-lines status # Show the current margin
```

Values are clamped to the supported range of 0 through 256. Add the command to `~/.cjshrc` to
persist the setting across sessions.

#### hint-delay

Configure the delay (in milliseconds) before inline hints are displayed. This controls how quickly the shell shows suggestions and hints as you type.

```bash
cjshopt hint-delay <milliseconds|status>
```

Examples:

```bash
cjshopt hint-delay 100     # Set hint delay to 100 milliseconds
cjshopt hint-delay 0       # Show hints immediately
cjshopt hint-delay status  # Show the current delay
```

- Accepted input: any non-negative integer
- Effective editor range: **0-5000 ms** (values above 5000 are clamped)
- Default: **0 ms** (hints can appear immediately)

Place the command in `~/.cjshrc` to keep the delay setting between sessions.

#### idle-timeout

Configure how many seconds the interactive prompt waits without terminal input before running
registered `idle` hooks. The feature is disabled by default.

```bash
cjshopt idle-timeout <seconds|off|status>
```

Examples:

```bash
cjshopt idle-timeout 120     # Run idle hooks after two minutes
cjshopt idle-timeout off     # Disable idle detection
cjshopt idle-timeout status  # Show the current setting
```

The editor restores canonical terminal state before the hooks run, gives foreground terminal
control to commands they launch, and restores pending input and its cursor position afterward.
The timeout is active only when at least one `idle` hook is registered, and it is ignored in
non-interactive, `--secure`, and `--posix` sessions. See [Shell Hooks](hooks.md) for a Drift
screensaver example.

#### completion-preview

Toggle the completion preview feature, which shows a preview of the selected completion as you navigate through completion options.

```bash
cjshopt completion-preview <on|off|status>
```

Examples:

```bash
cjshopt completion-preview on      # Enable completion preview
cjshopt completion-preview off     # Disable completion preview
cjshopt completion-preview status  # Show the current setting
```

The subcommand accepts synonyms such as `enable`, `disable`, `true`, and `false`. Add to `~/.cjshrc` to persist the preference.

#### completion-click-accept

Control whether mouse clicks immediately accept completion hints and completion-menu entries.

```bash
cjshopt completion-click-accept <on|off|status>
```

Examples:

```bash
cjshopt completion-click-accept on       # Always accept completion entries on click
cjshopt completion-click-accept off      # Click selects entries without accepting (default)
cjshopt completion-click-accept status   # Show the current setting
```

`off` is useful when you still want mouse selection/highlighting but prefer confirming with
`Enter`, `Right`, or `End`. Add the command to `~/.cjshrc` to persist the preference.

#### menu-highlighting

Control syntax highlighting for entries rendered in completion and history search menus. The menu
uses the same syntax highlighter and styles as the edit buffer. **Disabled by default.**

```bash
cjshopt menu-highlighting <none|single|all|reverse|status>
```

Examples:

```bash
cjshopt menu-highlighting none    # Keep menu items unhighlighted (default)
cjshopt menu-highlighting single  # Highlight only the cursor-selected item
cjshopt menu-highlighting all     # Highlight every rendered item
cjshopt menu-highlighting reverse # Highlight every item except the cursor-selected item
cjshopt menu-highlighting status  # Show the current mode
```

`none` preserves the legacy menu appearance, `single` highlights only the current selection, `all`
highlights every visible completion/history item, and `reverse` highlights every visible item except
the current selection. Add the command to `~/.cjshrc` to persist the preference.

#### visible-whitespace

Show or hide visible markers for whitespace characters (such as spaces) while editing commands. When enabled, spaces are rendered with a subtle middle-dot marker so you can spot trailing or double spacing issues.

```bash
cjshopt visible-whitespace <on|off|status>
```

Examples:

```bash
cjshopt visible-whitespace on      # Show whitespace markers while editing
cjshopt visible-whitespace off     # Hide whitespace markers (default)
cjshopt visible-whitespace status  # Show the current setting
```

Add the command to `~/.cjshrc` to keep the preference across sessions. Synonyms like `enable`, `disable`, `true`, and `false` are accepted.

#### line-wrap-marker

Set the character at the end of wrapped editor rows. The default is `↵` on macOS
or `←` on other UTF-8 terminals. Use one printable Unicode character, or an empty
string to hide the marker.

```bash
cjshopt line-wrap-marker '↪'     # Use a custom wrap marker
cjshopt line-wrap-marker ''      # Hide the wrap marker
cjshopt line-wrap-marker status  # Show the current marker
```

The marker reserves its display width (one or two columns). Disabling it lets
input use the full terminal width before wrapping. Enter still moves output onto
the next line. Multi-character strings, control characters, and zero-width
characters are rejected. ASCII markers also work on non-UTF-8 terminals.
Add the command to `~/.cjshrc` to persist the preference.

This replaces the old toggle syntax: use `''` instead of `off`, and set a character
instead of `on`. Toggle words such as `enable`, `disable`, `true`, and `false` are
no longer accepted; `0` and `1` are literal marker characters.

#### hint

Enable, disable, or inspect inline hints that appear as you type commands. Hints can include suggestions, command completions, and other helpful information.

```bash
cjshopt hint <on|off|status>
```

Examples:

```bash
cjshopt hint on       # Enable inline hints
cjshopt hint off      # Disable inline hints
cjshopt hint status   # Show the current setting
```

Synonyms like `enable`, `disable`, `true`, and `false` are supported. Persist the setting by adding the command to `~/.cjshrc`.

#### multiline-indent

Configure automatic indentation in multiline input mode. When enabled, the shell will automatically indent continuation lines based on the context (e.g., after opening braces, parentheses, or control structures).

```bash
cjshopt multiline-indent <on|off|status>
```

Examples:

```bash
cjshopt multiline-indent on       # Enable automatic indentation
cjshopt multiline-indent off      # Disable automatic indentation
cjshopt multiline-indent status   # Show the current setting
```

This is particularly useful when writing shell scripts or complex commands directly in the shell. Add to `~/.cjshrc` to keep the setting. Accepts synonyms such as `enable`, `disable`, `true`, and `false`.

#### multiline

Enable or disable multiline input mode entirely. When enabled, you can enter commands that span multiple lines. When disabled, the shell treats each line as a separate command.

```bash
cjshopt multiline <on|off|status>
```

Examples:

```bash
cjshopt multiline on       # Enable multiline input
cjshopt multiline off      # Disable multiline input
cjshopt multiline status   # Show the current setting
```

Disabling multiline mode may be useful for simple command execution or when working with scripts that don't require multi-line editing. Accepts synonyms like `enable`, `disable`, `true`, and `false`. Persist by adding to `~/.cjshrc`.

#### inline-help

Toggle inline help messages that appear as you type commands. These messages can provide quick information about command syntax, options, and usage.

```bash
cjshopt inline-help <on|off|status>
```

Examples:

```bash
cjshopt inline-help on       # Enable inline help messages
cjshopt inline-help off      # Disable inline help messages
cjshopt inline-help status   # Show the current setting
```

Supports synonyms such as `enable`, `disable`, `true`, and `false`. Add the command to `~/.cjshrc` to make the setting permanent.

#### status-hints

Control the underlined status hint banner (the line that lists keys like `complete`, `history search`, and `help`). Pick when it appears using one of four modes:

- `off` – never display the banner.
- `normal` – only show the banner when both the input buffer and status area are empty (default).
- `transient` – show the banner whenever the status area has no other content.
- `persistent` – always prepend the banner above any other status message.

```bash
cjshopt status-hints <off|normal|transient|persistent|status>
```

Examples:

```bash
cjshopt status-hints normal      # Only surface the hints on an empty prompt
cjshopt status-hints persistent  # Keep the banner visible at all times
cjshopt status-hints status      # Show the current mode
```

Add the command to `~/.cjshrc` to persist the mode across sessions.

#### status-line

Remove or restore the entire status row beneath the prompt, including syntax feedback and the hint banner.

```bash
cjshopt status-line <on|off|status>
```

- `on` (default) – allow syntax validation text and `status-hints` output to appear.
- `off` – suppress the status row completely; the last `status-hints` mode is remembered for when you turn it back on.

Examples:

```bash
cjshopt status-line off     # Hide validation output and the hint banner
cjshopt status-line on      # Restore the status row using the last hint mode
cjshopt status-line status  # Display the current setting
```

Add the command to `~/.cjshrc` to persist the preference.

#### status-reporting

Keep the status line visible for banners and custom output, but disable cjsh’s built-in syntax/error reporting.

```bash
cjshopt status-reporting <on|off|status>
```

- `on` (default) – run cjsh’s validation pipeline and print its summary below the prompt.
- `off` – suppress cjsh-generated status messages while leaving `status-hints` (and any custom callbacks) intact.

Examples:

```bash
cjshopt status-reporting off     # Keep hint banners but hide validation output
cjshopt status-reporting on      # Re-enable syntax feedback
cjshopt status-reporting status  # Display the current setting
```

Persist preferences by adding the command to `~/.cjshrc`.

#### status-line-callback

Register a shell function that runs before each status-line refresh and can publish custom text.

```bash
cjshopt status-line-callback <function_name|off|status>
```

- `<function_name>` – invoke that shell function on every status refresh.
- `off` – disable custom callback output.
- `status` – print the current callback setting.

Callback contract:

- `$1` and `CJSH_STATUS_INPUT` both contain the current input buffer.
- Set `CJSH_STATUS_OUTPUT` inside your function to the text you want rendered.
- Leave `CJSH_STATUS_OUTPUT` empty to hide callback output for that refresh.

Example:

```bash
function my_status_banner() {
    local buf="$1"
    if [ -z "$buf" ]; then
        CJSH_STATUS_OUTPUT="[ic-hint]ready[/]"
        return
    fi

    CJSH_STATUS_OUTPUT="[ic-hint]chars:[/] ${#buf}"
}

cjshopt status-line-callback my_status_banner
```

Use `cjshopt status-reporting off` if you want only your callback text without cjsh validation output. Keep callback code fast because it runs frequently while editing.

#### mouse-clicking

Configure mouse capture separately for prompt editing and interactive menus.
The default is `off`, which leaves ordinary prompt interaction to the terminal while retaining
mouse support inside completion, history, and command-palette menus.

```bash
cjshopt mouse-clicking <all-off|off|simple|smart|status>
```

Examples:

```bash
cjshopt mouse-clicking all-off # Never capture mouse events, including inside menus
cjshopt mouse-clicking off     # Keep editing native; capture only in interactive menus
cjshopt mouse-clicking simple  # Capture mouse events until manually toggled
cjshopt mouse-clicking smart   # Auto-suspend for terminal selection and resume for editing
cjshopt mouse-clicking status  # Show the current setting
```

`F2` (or any key bound to `toggle-mouse-reporting`) still toggles mouse clicking for the current
prompt at runtime in `simple` and `smart` modes. In `smart` mode, wheel input and selections started
above the editor, in the prompt or continuation gutter, or on status/helper rows suspend capture.
Dragging with the left mouse button also suspends capture, including inside menus, and preserves
the display. A reported button release restores capture without clearing the highlight, but
disabling mouse reporting also stops release reports, so this is best-effort. Keyboard input
restores capture; focus-in input also restores it when supported by the terminal. No terminal-specific
configuration is required. Some terminals require releasing the button and dragging again after
capture is suspended to begin native highlighting. If motion reports are unavailable, a press and
release in different cells still suspend capture.
`disabled` remains an alias for `all-off`; `menu-only` and `menus` are aliases for `off`.

While an interactive menu has mouse capture, clicking the prompt or anywhere outside its selectable
rows temporarily releases capture to the terminal. Keyboard input or a terminal focus-in event
restores menu clicking.

#### mouse-clicking-status-line

Show or hide the status-row indicator text that appears when mouse clicking is active.

```bash
cjshopt mouse-clicking-status-line <on|off|status>
```

Examples:

```bash
cjshopt mouse-clicking-status-line on      # Show the "Mouse clicking is enabled" indicator
cjshopt mouse-clicking-status-line off     # Hide the indicator text
cjshopt mouse-clicking-status-line status  # Show the current setting
```

This toggle affects only the indicator text. It does not enable or disable mouse clicking itself.

#### auto-tab

Configure automatic tab completion behavior. When enabled, the shell may automatically complete commands or show completions without requiring explicit tab key presses. **Disabled by default.**

```bash
cjshopt auto-tab <on|off|status>
```

Examples:

```bash
cjshopt auto-tab on       # Enable automatic tab completion
cjshopt auto-tab off      # Disable automatic tab completion (default)
cjshopt auto-tab status   # Show the current setting
```

Accepts synonyms including `enable`, `disable`, `true`, and `false`. Place in `~/.cjshrc` to persist the preference across sessions.

#### keybind

Inspect or customize isocline key bindings. Changes apply immediately in the current shell, and you can add the same command to `~/.cjshrc` to persist them for future sessions.

```bash
cjshopt keybind <subcommand> [...]
```

Key subcommands include:

- `list` - Show the active profile plus default vs. custom bindings
- `set <action> <keys...>` - Replace bindings for an action
- `add <action> <keys...>` - Add additional bindings for an action
- `clear <keys...>` - Remove the provided key specifications
- `clear-action <action>` - Remove all custom bindings for an action
- `reset` - Drop every custom binding and restore defaults
- `profile list` - List available key binding profiles
- `profile set <name>` - Activate the named profile

Key specifications accept pipe (`|`) separated alternatives, so `Ctrl+K|Ctrl+X` is a single argument covering both bindings. Place commands like `cjshopt keybind set cursor-left "Ctrl+H"` in `~/.cjshrc` to keep them between sessions.

Mouse toggle actions use `toggle-mouse-reporting` (aliases: `mouse-reporting-toggle`,
`toggle-mouse`). The default binding is `F2`.

#### set-history-max

Adjust the number of entries stored in the persistent history file.

```bash
cjshopt set-history-max <number|default|status>
```

- Provide any non-negative number (0 disables history persistence entirely)
- Use `default` to restore the built-in limit of **1000** entries
- Use `status` (or `--status`) to display the current setting

Examples:

```bash
cjshopt set-history-max 0        # Disable history persistence
cjshopt set-history-max 500      # Retain the latest 500 commands
cjshopt set-history-max default  # Go back to the default limit
cjshopt set-history-max status   # Show the current limit
```

In startup files, limit changes are deferred until startup configuration finishes,
so history is loaded once using the final limit and history path. Commands and
scripts that do not start the editor apply any pending limit before their body runs.
Changes made during a running session take effect immediately.

#### set-completion-max

Limit how many completion entries are generated and displayed inside the menu each time you
press `Tab`.

```bash
cjshopt set-completion-max <number|default|status>
```

- Provide any number greater than or equal to **1**
- Use `default` to restore the built-in limit of **1000** entries
- Use `status` (or `--status`) to inspect the current setting

Examples:

```bash
cjshopt set-completion-max 50        # Show at most 50 suggestions
cjshopt set-completion-max default   # Restore the default cap
cjshopt set-completion-max status    # Display the current limit
```

Lowering the cap trims visual noise and speeds up completion-heavy commands, especially when
thousands of filesystem matches would otherwise be generated. Add the command to `~/.cjshrc`
to persist the preference.

At an empty or whitespace-only prompt, `Tab` offers unique history entries up to this same cap,
ranked by most recent use and then frequency. For example, `cjshopt set-completion-max 50` allows
up to 50 history suggestions.

#### prompt-newline

Control whether cjsh prints a blank line after every command, regardless of what your prompt already emitted.

```bash
cjshopt prompt-newline on
cjshopt prompt-newline off
cjshopt prompt-newline status
```

Enable the toggle to keep transcripts readable when prompts have dense information; disable it for a compact display.

#### right-prompt-follow-cursor

Make the inline right prompt (`RPS1`/`RPROMPT`) follow the cursor row while editing multi-line input. When disabled (default), the right prompt always renders on the first line even if the cursor moves down the buffer.

```bash
cjshopt right-prompt-follow-cursor on
cjshopt right-prompt-follow-cursor off
cjshopt right-prompt-follow-cursor status
```

Enabling the toggle keeps status blocks such as clocks or Git metadata aligned with the active input line, which is especially helpful for long pipelines that wrap across multiple rows.
