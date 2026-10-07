<!--
  hooks.md

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

# Shell Hooks

CJ's Shell provides a lightweight hook system that allows you to execute custom shell functions at key points in the shell's lifecycle. This is similar to Zsh's hook system and enables powerful customizations without modifying the shell's core code.

## Available Hook Types

### `precmd`
Executed before the prompt is displayed, after the previous command has completed.
The previous command's exit status is available as `$?`, and `CJSH_COMMAND_DURATION_MS` contains
its execution time in milliseconds.

**Use cases:**
- Update dynamic prompt elements
- Display custom information before each prompt
- Log command execution times
- Update terminal title

**Example:**
```bash
function my_precmd() {
    echo "Last command finished at $(date)"
}
hook add precmd my_precmd
```

### `preexec`
Executed after you press Enter but before the command is actually executed.
The command that will execute is passed as `$1`. If history expansion changed the submitted line,
`$1` contains the expanded command.

**Use cases:**
- Log commands before execution
- Display command start time
- Validate commands before running
- Send notifications for long-running commands

**Example:**
```bash
function my_preexec() {
    echo "About to execute command..."
}
hook add preexec my_preexec
```

### `chpwd`
Executed after successfully changing directories with the `cd` command.

**Use cases:**
- Automatically activate virtual environments
- Display directory-specific information
- Update terminal title with current directory
- Load directory-specific configurations

**Example:**
```bash
function my_chpwd() {
    echo "Changed to: $PWD"
    # Auto-activate Python virtual environment if it exists
    if [ -f ".venv/bin/activate" ]; then
        source .venv/bin/activate
    fi
}
hook add chpwd my_chpwd
```

### `idle`
Executed after the interactive prompt receives no terminal input for the configured number of
seconds. Idle hooks are disabled until a timeout is set with `cjshopt idle-timeout`.

CJSH pauses the editor, restores the terminal, and runs idle hooks synchronously in the foreground.
When the hooks return, the pending input buffer and cursor position are restored. The timeout then
starts again, so a full-screen hook can return after a key press and be launched again after the
next idle period.

**Use cases:**
- Start a terminal screensaver
- Lock or dim an unattended terminal
- Refresh an interactive dashboard after inactivity

**Example:**
```bash
function idle_notice() {
    echo "Terminal was idle"
}

cjshopt idle-timeout 120
hook add idle idle_notice
```

Use `cjshopt idle-timeout off` to disable idle detection. Idle hooks do not run in non-interactive,
`--secure`, or `--posix` sessions.

### Drift terminal screensaver

Add this native integration to `~/.cjshrc`:

```bash
function _drift_idle() {
    if command -v drift >/dev/null 2>&1; then
        drift
    fi
}

cjshopt idle-timeout "${DRIFT_TIMEOUT:-120}"
hook add idle _drift_idle
```

The hook runs `drift` as a foreground job, so it can read from and draw to the terminal normally.
The Bash integration's background timer, PID tracking, monitor-mode changes, `ps` test, and
`disown` call are not needed.

## Special Function Handlers

In addition to `hook add`, CJ's Shell recognizes these function names directly:

### `command_not_found_handler`
Executed when command lookup fails for an external command name (exit `127` path). The missing
command name and its arguments are passed to the function. Returning `127` defers back to cjsh's
built-in `command not found` message and exit code; any other return code overrides the default.
This handler is ignored when cjsh starts with `--minimal`, `--secure`, or `--posix`.

```bash
function command_not_found_handler() {
    echo "Unknown command: $1"
    return 127
}
```

### `cjshexit`
Executed during shell shutdown before `trap ... EXIT` handlers.
This handler is ignored when cjsh starts with `--minimal`, `--secure`, or `--posix`.
Confirmed exits, `exit --force`, and untrapped HUP or TERM all use this ordering:
`cjshexit`, the `EXIT` trap, then login-shell logout configuration. Each runs once.
HUP and TERM traps that return normally leave the shell running; a trap may explicitly call `exit`.

```bash
function cjshexit() {
    echo "bye from cjsh"
}
```

## Hook Management Commands

### Register a Hook
```bash
hook add <hook_type> <function_name>
```

Registers a shell function to be called at the specified hook point.

**Example:**
```bash
function greet() {
    echo "Hello from hook!"
}
hook add precmd greet
```

### Remove a Hook
```bash
hook remove <hook_type> <function_name>
```

Unregisters a previously registered hook function.

**Example:**
```bash
hook remove precmd greet
```

### List Hooks
```bash
hook list [hook_type]
```

Lists all registered hooks. If `hook_type` is specified, only hooks of that type are shown.

**Examples:**
```bash
# List all hooks
hook list

# List only precmd hooks
hook list precmd
```

### Clear All Hooks
```bash
hook clear <hook_type>
```

Removes all hooks of the specified type.

**Example:**
```bash
hook clear precmd
```

## Complete Examples

### Auto-activate Python Virtual Environments

```bash
# In ~/.cjshrc

function auto_venv() {
    # Deactivate current venv if exists
    if [ ! -z "$VIRTUAL_ENV" ]; then
        deactivate 2>/dev/null
    fi
    
    # Check for virtual environment in current directory
    if [ -f ".venv/bin/activate" ]; then
        source .venv/bin/activate
        echo "✓ Activated Python virtual environment"
    elif [ -f "venv/bin/activate" ]; then
        source venv/bin/activate
        echo "✓ Activated Python virtual environment"
    fi
}

hook add chpwd auto_venv
```

### Display Git Status After Directory Change

```bash
# In ~/.cjshrc

function git_status_on_cd() {
    if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
        echo ""
        git status -sb
        echo ""
    fi
}

hook add chpwd git_status_on_cd
```

### Log All Commands

```bash
# In ~/.cjshrc

function log_command() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $PWD" >> ~/.cjsh_command_log
}

hook add preexec log_command
```

### Display Command Duration

```bash
# In ~/.cjshrc

function command_timer_start() {
    COMMAND_START_TIME=$(date +%s)
}

function command_timer_end() {
    if [ ! -z "$COMMAND_START_TIME" ]; then
        local duration=$(($(date +%s) - COMMAND_START_TIME))
        if [ $duration -gt 5 ]; then
            echo "⏱ Command took ${duration}s"
        fi
        unset COMMAND_START_TIME
    fi
}

hook add preexec command_timer_start
hook add precmd command_timer_end
```

### Dynamic Terminal Title

```bash
# In ~/.cjshrc

function update_terminal_title() {
    # Set terminal title to current directory
    echo -ne "\033]0;${PWD/#$HOME/~}\007"
}

hook add precmd update_terminal_title
hook add chpwd update_terminal_title
```

## Best Practices

1. **Keep lifecycle hooks fast**: Hooks are executed synchronously. An `idle` hook may intentionally
   stay active while it owns the terminal, but `precmd`, `preexec`, and `chpwd` should normally
   return quickly.

2. **Handle errors gracefully**: Hooks should not exit with error codes that affect subsequent commands.

3. **Use functions**: Define hooks as shell functions for better organization and reusability.

4. **Avoid infinite loops**: Be careful with hooks that might trigger themselves (e.g., `chpwd` that calls `cd`).

5. **Test hooks**: Test your hooks in a separate shell session before adding them to `~/.cjshrc`.

6. **Document your hooks**: Add comments to your `~/.cjshrc` explaining what each hook does.

## Debugging Hooks

To debug hook execution, you can temporarily add debugging output:

```bash
function my_hook() {
    echo "DEBUG: my_hook called" >&2
    # Your actual hook code here
}
```

To disable a problematic hook without editing files:

```bash
# In the shell
hook remove precmd problematic_function
```

## Hook Execution Order

When multiple hooks of the same type are registered, they are executed in the order they were registered:

```bash
hook add precmd first_function
hook add precmd second_function
hook add precmd third_function

# Execution order: first_function, second_function, third_function
```

## Differences from Zsh Hooks

CJ's Shell hooks are inspired by Zsh but have some differences:

- **Simpler API**: Uses a single `hook` command instead of `add-zsh-hook`
- **Fewer hook types**: Currently supports `precmd`, `preexec`, `chpwd`, and `idle`
- **No automatic unhooking**: Functions are not automatically removed when undefined
- **Manual management**: No automatic hook discovery from function names

## Configuration File Example

Here's a complete example of hook usage in `~/.cjshrc`:

```bash
# ~/.cjshrc - CJ's Shell Configuration with Hooks

# Function definitions
function show_git_info() {
    if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
        local branch=$(git branch --show-current)
        echo "Git branch: $branch"
    fi
}

function greet_directory() {
    local dir=$(basename "$PWD")
    echo "→ Entered: $dir"
}

function check_todos() {
    if [ -f "TODO.md" ]; then
        echo "This directory has a TODO.md file"
    fi
}

# Register hooks
hook add chpwd show_git_info
hook add chpwd greet_directory
hook add chpwd check_todos

echo "Hooks loaded successfully"
```

## See Also

- [Built-in Commands Reference](commands.md)
- [Configuration Guide](../getting-started/what-to-know.md#startup-files)
