#!/usr/bin/env sh
# process_cleanup_helpers.sh
#
# This file is part of cjsh, CJ's Shell
#
# MIT License
#
# Copyright (c) 2026 Caden Finley
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.


# Each check runs in a subshell so its cleanup trap and temporary state are local.
check_process_cleanup() (
    cleanup_binary=$1
    cleanup_signal=$2
    cleanup_background=${3:-0}
    cleanup_mode=${4:-command}
    cleanup_dir=$(mktemp -d /tmp/cjsh_process_cleanup.XXXXXX) || exit 1
    cleanup_shell_pid=

    cleanup_test_processes() {
        for cleanup_pid_file in "$cleanup_dir"/*.pid; do
            [ -s "$cleanup_pid_file" ] || continue
            read -r cleanup_pid < "$cleanup_pid_file"
            kill -KILL "$cleanup_pid" 2>/dev/null || :
        done
        if [ -n "$cleanup_shell_pid" ]; then
            kill -KILL "$cleanup_shell_pid" 2>/dev/null || :
            wait "$cleanup_shell_pid" 2>/dev/null || :
        fi
        rm -rf "$cleanup_dir"
    }
    trap cleanup_test_processes 0
    trap 'exit 1' 1 2 15

    cat > "$cleanup_dir/script" <<'EOF'
if [ "$CJSH_CLEANUP_SIGNAL" = KILL ] || [ "$CJSH_CLEANUP_MODE" = loop ]; then
    echo $$ > "$CJSH_CLEANUP_DIR/ready.pid"
    while :; do :; done
fi
if [ "$CJSH_CLEANUP_SIGNAL" = force ]; then
    shopt -s huponexit
    sleep 30 &
    echo $! > "$CJSH_CLEANUP_DIR/ready.pid"
    exit --force
fi
if [ "$CJSH_CLEANUP_BACKGROUND" = 1 ]; then
    sleep 30 &
    echo $! > "$CJSH_CLEANUP_DIR/background.pid"
fi
if [ "$CJSH_CLEANUP_MODE" = read ]; then
    echo $$ > "$CJSH_CLEANUP_DIR/ready.pid"
    read value
    exit
fi
if [ "$CJSH_CLEANUP_MODE" = timed-read ]; then
    echo $$ > "$CJSH_CLEANUP_DIR/ready.pid"
    read -t 30 value
    exit
fi
if [ "$CJSH_CLEANUP_MODE" = redirected ]; then
    sh -c 'echo $$ > "$CJSH_CLEANUP_DIR/ready.pid"; exec sleep 30' < /dev/null
else
    sh -c 'echo $$ > "$CJSH_CLEANUP_DIR/ready.pid"; exec sleep 30'
fi
EOF
    export CJSH_CLEANUP_DIR="$cleanup_dir"
    export CJSH_CLEANUP_SIGNAL="$cleanup_signal"
    export CJSH_CLEANUP_BACKGROUND="$cleanup_background"
    export CJSH_CLEANUP_MODE="$cleanup_mode"
    cleanup_input=/dev/null
    case "$cleanup_mode" in
        read|timed-read)
            cleanup_input="$cleanup_dir/input"
            mkfifo "$cleanup_input" || exit 1
            # Keep the pipe open without providing input, so read must block.
            exec 3<> "$cleanup_input"
            ;;
    esac
    if [ "$cleanup_mode" = script ]; then
        "$cleanup_binary" < "$cleanup_dir/script" > "$cleanup_dir/output" 2>&1 &
    else
        "$cleanup_binary" -c "$(cat "$cleanup_dir/script")" < "$cleanup_input" > "$cleanup_dir/output" 2>&1 &
    fi
    cleanup_shell_pid=$!

    # kill -0 only proves that fork succeeded. Wait until the command is running
    # and has recorded the exact PID whose termination this check will verify.
    cleanup_attempt=0
    while [ ! -s "$cleanup_dir/ready.pid" ] && [ "$cleanup_attempt" -lt 100 ]; do
        sleep 0.02
        cleanup_attempt=$((cleanup_attempt + 1))
    done
    if [ ! -s "$cleanup_dir/ready.pid" ]; then
        echo "Child did not become ready"
        cat "$cleanup_dir/output"
        exit 1
    fi

    case "$cleanup_signal" in
        force) ;;
        multiple)
            kill -TERM "$cleanup_shell_pid" 2>/dev/null || :
            kill -HUP "$cleanup_shell_pid" 2>/dev/null || :
            kill -TERM "$cleanup_shell_pid" 2>/dev/null || :
            ;;
        *) kill -"$cleanup_signal" "$cleanup_shell_pid" || exit 1 ;;
    esac

    cleanup_attempt=0
    while kill -0 "$cleanup_shell_pid" 2>/dev/null && [ "$cleanup_attempt" -lt 100 ]; do
        sleep 0.02
        cleanup_attempt=$((cleanup_attempt + 1))
    done
    if kill -0 "$cleanup_shell_pid" 2>/dev/null; then
        echo "Shell did not exit after $cleanup_signal"
        exit 1
    fi
    wait "$cleanup_shell_pid" 2>/dev/null
    cleanup_status=$?
    cleanup_shell_pid=
    case "$cleanup_signal:$cleanup_status" in
        TERM:143|HUP:129|KILL:137|force:0|multiple:129|multiple:143) ;;
        *) echo "Unexpected exit status after $cleanup_signal: $cleanup_status"; exit 1 ;;
    esac

    # Check immediately after reaping cjsh: live children and zombies both count
    # as leaks. Unrelated processes elsewhere on the runner cannot affect this.
    for cleanup_pid_file in "$cleanup_dir"/*.pid; do
        read -r cleanup_pid < "$cleanup_pid_file"
        if kill -0 "$cleanup_pid" 2>/dev/null; then
            echo "Child $cleanup_pid remains after $cleanup_signal"
            ps -p "$cleanup_pid" -o pid=,ppid=,stat=,comm= 2>/dev/null || :
            exit 1
        fi
    done
)
