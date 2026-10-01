#!/usr/bin/env bash
# test_job_control.sh
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


if [ -n "$CJSH" ]; then
    CJSH_PATH="$CJSH"
else
    CJSH_PATH="$(cd "$(dirname "$0")/../../build" && pwd)/cjsh"
fi

status=0

log() {
    echo "$1"
}

cleanup_pid() {
    local pid="$1"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
    fi
}

wait_for_pid_file() {
    local pid_file="$1"
    local retries=0
    local pid=""

    while [ $retries -lt 60 ]; do
        if [ -s "$pid_file" ]; then
            pid="$(cat "$pid_file" 2>/dev/null)"
            if [ -n "$pid" ]; then
                echo "$pid"
                return 0
            fi
        fi
        sleep 0.05
        retries=$((retries + 1))
    done

    return 1
}

wait_for_process_exit() {
    local pid="$1"
    local retries=0

    while kill -0 "$pid" 2>/dev/null; do
        if [ $retries -ge 200 ]; then
            return 1
        fi
        sleep 0.05
        retries=$((retries + 1))
    done

    return 0
}

test_background_persists() {
    log "Test: background job survives without huponexit"
    local pid_file
    pid_file="$(mktemp /tmp/cjsh_job_pid.XXXXXX)"

    "$CJSH_PATH" -c "sleep 5 & echo \$! > $pid_file; sleep 0.2" >/dev/null 2>&1

    local pid
    pid="$(cat "$pid_file" 2>/dev/null)"
    rm -f "$pid_file"

    if [ -z "$pid" ]; then
        echo "FAIL: no PID recorded"
        return 1
    fi

    if kill -0 "$pid" 2>/dev/null; then
        cleanup_pid "$pid"
        echo "PASS"
        return 0
    else
        echo "FAIL: background PID $pid not running"
        return 1
    fi
}

test_huponexit_kills_jobs() {
    log "Test: shopt -s huponexit terminates jobs on exit"
    local pid_file
    pid_file="$(mktemp /tmp/cjsh_job_pid.XXXXXX)"

    "$CJSH_PATH" -c "shopt -s huponexit; sleep 5 & echo \$! > $pid_file; sleep 0.2" >/dev/null 2>&1

    local pid
    pid="$(cat "$pid_file" 2>/dev/null)"
    rm -f "$pid_file"

    if [ -z "$pid" ]; then
        echo "FAIL: no PID recorded"
        return 1
    fi

    if kill -0 "$pid" 2>/dev/null; then
        cleanup_pid "$pid"
        echo "FAIL: PID $pid should have been terminated"
        return 1
    fi

    echo "PASS"
    return 0
}

test_disown_removes_job() {
    log "Test: disown detaches current job"
    local pid_file log_file
    pid_file="$(mktemp /tmp/cjsh_job_pid.XXXXXX)"
    log_file="$(mktemp /tmp/cjsh_jobs.XXXXXX)"

    "$CJSH_PATH" -c "sleep 5 & echo \$! > $pid_file; sleep 0.2; disown; jobs" >"$log_file" 2>&1

    local pid
    pid="$(cat "$pid_file" 2>/dev/null)"
    rm -f "$pid_file"

    if [ -z "$pid" ]; then
        rm -f "$log_file"
        echo "FAIL: no PID recorded"
        return 1
    fi

    local result=0
    local remaining
    remaining=$(grep -v '^\[' "$log_file" | sed '/^[[:space:]]*$/d')

    if [ "$remaining" != "No jobs" ]; then
        echo "FAIL: expected 'No jobs' after disown, got: $remaining"
        result=1
    elif ! kill -0 "$pid" 2>/dev/null; then
        echo "FAIL: disowned PID $pid was not running"
        result=1
    else
        echo "PASS"
    fi

    rm -f "$log_file"
    cleanup_pid "$pid"
    return $result
}

test_disown_pid_and_hup_mark() {
    log "Test: disown accepts PID and -h protects from huponexit"
    local pid_file log_file
    pid_file="$(mktemp /tmp/cjsh_disown_pid.XXXXXX)"
    log_file="$(mktemp /tmp/cjsh_disown_jobs.XXXXXX)"

    "$CJSH_PATH" -c "shopt -s huponexit; sleep 5 & p=\$!; echo \$p > $pid_file; disown -h \$p; jobs -l" >"$log_file" 2>&1

    local pid
    pid="$(cat "$pid_file" 2>/dev/null)"
    local result=0
    if [ -z "$pid" ]; then
        echo "FAIL: no PID recorded"
        result=1
    elif ! grep -q "Running" "$log_file"; then
        echo "FAIL: disown -h removed the job from jobs output"
        result=1
    elif ! kill -0 "$pid" 2>/dev/null; then
        echo "FAIL: disown -h job did not survive huponexit"
        result=1
    else
        echo "PASS"
    fi

    cleanup_pid "$pid"
    rm -f "$pid_file" "$log_file"
    return $result
}

test_wait_next_and_pipeline_lifetime() {
    log "Test: wait -n and pipeline lifetime/status tracking"
    local output gate_dir
    gate_dir=$(mktemp -d) || return 1
    # Keep the first stage alive until jobs has inspected the pipeline, even
    # when sanitizer overhead or a busy runner delays the parent shell.
    output=$(CJSH_TEST_PIPELINE_GATE="$gate_dir/release" "$CJSH_PATH" -c '
        sh -c '\''while [ ! -e "$CJSH_TEST_PIPELINE_GATE" ]; do sleep 0.01; done'\'' | sh -c '\''exit 7'\'' & p=$!
        sleep 0.05; jobs -r; touch "$CJSH_TEST_PIPELINE_GATE"
        wait $p; pipeline=$?
        sleep 0.01 & wait -n -p winner
        printf "%s|%s|%s" "$pipeline" "$?" "$winner"
    ' 2>/dev/null)
    rm -f "$gate_dir/release"
    rmdir "$gate_dir"

    if echo "$output" | grep -q "Running" && echo "$output" | grep -Eq '7\|0\|[0-9]+'; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected running pipeline, status 7, and wait -n PID; got: $output"
    return 1
}

test_jobs_options_and_jobspecs() {
    log "Test: jobs option filters and relative/substring job specs"
    local output ids first second
    output=$("$CJSH_PATH" -c "set -m; \
        sh -c 'exec sleep 2' alpha-jobs-token & first=\$!; \
        sh -c 'exec sleep 2' beta-jobs-token & second=\$!; \
        kill -STOP \$second; sleep 0.1; \
        printf 'ids=%s,%s\\n' \"\$first\" \"\$second\"; \
        printf 'plus='; jobs -p %+; \
        printf 'minus='; jobs -p %-; \
        printf 'substring='; jobs -p %?alpha-jobs-token; \
        printf 'long='; jobs -l %?beta-jobs-token; \
        printf 'running='; jobs -r %?alpha-jobs-token; \
        printf 'stopped='; jobs -s %?beta-jobs-token; \
        kill \$first; kill -CONT \$second; kill \$second; \
        wait \$first \$second 2>/dev/null || true" 2>&1)

    ids=$(printf '%s\n' "$output" | sed -n 's/^ids=//p' | tail -n 1)
    first=${ids%,*}
    second=${ids#*,}

    if [ -z "$ids" ] || [ "$first" = "$ids" ] || [ "$second" = "$ids" ]; then
        echo "FAIL: could not recover job PIDs: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -q "^plus=$second$"; then
        echo "FAIL: jobs -p %+ did not select the current job: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -q "^minus=$first$"; then
        echo "FAIL: jobs -p %- did not select the previous job: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -q "^substring=$first$"; then
        echo "FAIL: jobs did not resolve %?substring: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -Eq "^long=\\[2\\][+-]?[[:space:]]+$second[[:space:]]+Stopped.*beta-jobs-token"; then
        echo "FAIL: jobs -l did not show the stopped job and process group: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -Eq '^running=\[1\][+-]?[[:space:]]+Running.*alpha-jobs-token'; then
        echo "FAIL: jobs -r did not select the running job: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -Eq '^stopped=\[2\][+-]?[[:space:]]+Stopped.*beta-jobs-token'; then
        echo "FAIL: jobs -s did not select the stopped job: $output"
        return 1
    fi

    echo "PASS"
    return 0
}

test_disown_all_and_running_filters() {
    log "Test: disown -r removes running jobs and disown -a removes the rest"
    local output ids running stopped
    output=$("$CJSH_PATH" -c "set -m; \
        sh -c 'exec sleep 2' running-disown-token & running=\$!; \
        sh -c 'exec sleep 2' stopped-disown-token & stopped=\$!; \
        kill -STOP \$stopped; sleep 0.1; \
        printf 'ids=%s,%s\\n' \"\$running\" \"\$stopped\"; \
        disown -r; printf 'after-r='; jobs -p; \
        disown -a; printf 'after-a='; jobs -p; printf 'done\\n'; \
        kill \$running; kill -CONT \$stopped; kill \$stopped; \
        wait \$running \$stopped 2>/dev/null || true" 2>&1)

    ids=$(printf '%s\n' "$output" | sed -n 's/^ids=//p' | tail -n 1)
    running=${ids%,*}
    stopped=${ids#*,}

    if [ -z "$ids" ] || [ "$running" = "$ids" ] || [ "$stopped" = "$ids" ]; then
        echo "FAIL: could not recover disowned job PIDs: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -q "^after-r=$stopped$"; then
        echo "FAIL: disown -r did not leave only the stopped job: $output"
        return 1
    fi
    if ! printf '%s\n' "$output" | grep -q '^after-a=done$'; then
        echo "FAIL: disown -a did not remove all remaining jobs: $output"
        return 1
    fi

    echo "PASS"
    return 0
}

test_wait_multi_job_and_jobspec_matrix() {
    log "Test: multi-job wait -n, operand-less wait, and substring job specs"
    local output tmp_dir shell_status
    tmp_dir=$(mktemp -d /tmp/cjsh_wait_matrix.XXXXXX) || return 1
    if ! mkfifo "$tmp_dir/release"; then
        rmdir "$tmp_dir"
        echo "FAIL: could not create wait matrix release FIFO"
        return 1
    fi

    # Keep the first operand running until wait -n selects the second, regardless
    # of how long CI takes to launch either child or reach the wait command.
    output=$("$CJSH_PATH" -c "set -m; \
        sh -c 'read release < \"\$1\"; exit 8' slow-wait-token '$tmp_dir/release' & slow=\$!; \
        sh -c 'exit 3' fast-wait-token & fast=\$!; \
        wait -n -p winner \$slow \$fast; first=\$?; \
        printf 'release\\n' > '$tmp_dir/release'; \
        wait \$slow; slow_status=\$?; \
        sh -c 'exit 7' failed-wait-token & wait; all_status=\$?; \
        sh -c 'exit 6' substring-wait-token & spec=\$!; \
        wait %?substring-wait-token; spec_status=\$?; \
        printf 'wait-results=%s,%s,%s,%s,%s,%s\\n' \
            \"\$first\" \"\$winner\" \"\$fast\" \"\$slow_status\" \
            \"\$all_status\" \"\$spec_status\"" 2>&1)
    shell_status=$?
    rm -f "$tmp_dir/release"
    rmdir "$tmp_dir"

    if [ "$shell_status" -eq 0 ] && \
        printf '%s\n' "$output" | grep -Eq '^wait-results=3,([0-9]+),\1,8,0,6$'; then
        echo "PASS"
        return 0
    fi

    # The suite runner only retains lines containing FAIL; preserve the statuses
    # and any shell diagnostics on that same line.
    echo "FAIL: wait matrix returned unexpected statuses or selected PID (shell status $shell_status): $(printf '%s\n' "$output" | tr '\n' ' ')"
    return 1
}

jobs_reports_when_empty() {
    log "Test: jobs reports when no jobs exist"
    local output
    output=$("$CJSH_PATH" -c "jobs" 2>&1)

    if echo "$output" | grep -q "No jobs"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected 'No jobs' message, got: $output"
    return 1
}

jobs_p_option_stays_silent_when_empty() {
    log "Test: jobs -p stays silent when empty"
    local output
    output=$("$CJSH_PATH" -c "jobs -p" 2>&1)

    if [ -n "$output" ]; then
        echo "FAIL: jobs -p should produce no output when empty, got: $output"
        return 1
    fi

    echo "PASS"
    return 0
}

fg_command_name_resolves_job() {
    log "Test: fg resolves job by command name"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 1 & fg sleep" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: fg sleep via command name failed (exit $exit_code): $output"
        return 1
    fi

    echo "PASS"
    return 0
}

fg_without_arg_resolves_only_job() {
    log "Test: fg without args resolves the only job"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 0.2 & fg" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: fg without args failed with one job (exit $exit_code): $output"
        return 1
    fi

    echo "PASS"
    return 0
}

bg_without_arg_resolves_only_stopped_job() {
    log "Test: bg without args resolves the only stopped job"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 5 & pid=\$!; kill -STOP \$pid; sleep 0.05; bg; bg_status=\$?; kill \$pid 2>/dev/null; wait \$pid 2>/dev/null || true; exit \$bg_status" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: bg without args failed with one stopped job (exit $exit_code): $output"
        return 1
    fi

    echo "PASS"
    return 0
}

fg_command_prefix_resolves_job() {
    log "Test: fg resolves job by command prefix"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 0.2 & fg sl" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: fg sl via prefix failed (exit $exit_code): $output"
    return 1
}

fg_command_name_requires_disambiguation() {
    log "Test: fg requires job id when command name is ambiguous"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 5 & pid1=\$!; sleep 5 & pid2=\$!; fg sleep; fg_status=\$?; kill \$pid1 \$pid2 2>/dev/null; wait \$pid1 \$pid2 2>/dev/null; exit \$fg_status" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 1 ] && echo "$output" | grep -q "multiple jobs match command"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: fg sleep should require disambiguation (exit $exit_code): $output"
    return 1
}

fg_command_prefix_requires_disambiguation() {
    log "Test: fg requires job id when command prefix is ambiguous"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 5 & pid1=\$!; sleep 5 & pid2=\$!; fg sl; fg_status=\$?; kill \$pid1 \$pid2 2>/dev/null; wait \$pid1 \$pid2 2>/dev/null; exit \$fg_status" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 1 ] && echo "$output" | grep -q "multiple jobs match command"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: fg sl should require disambiguation (exit $exit_code): $output"
    return 1
}

kill_command_name_resolves_job() {
    log "Test: kill resolves job by command name"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 5 & pid=\$!; kill sleep; kill_status=\$?; kill \$pid 2>/dev/null; wait \$pid 2>/dev/null; exit \$kill_status" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: kill sleep via command name failed (exit $exit_code): $output"
    return 1
}

kill_command_prefix_resolves_job() {
    log "Test: kill resolves job by command prefix"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 5 & pid=\$!; kill sl; kill_status=\$?; wait \$pid 2>/dev/null; exit \$kill_status" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: kill sl via prefix failed (exit $exit_code): $output"
    return 1
}

kill_command_name_requires_disambiguation() {
    log "Test: kill requires job id when command name is ambiguous"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 5 & pid1=\$!; sleep 5 & pid2=\$!; kill sleep; kill_status=\$?; kill \$pid1 \$pid2 2>/dev/null; wait \$pid1 \$pid2 2>/dev/null; exit \$kill_status" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 1 ] && echo "$output" | grep -q "multiple jobs match command"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: kill sleep should require disambiguation (exit $exit_code): $output"
    return 1
}

kill_command_prefix_requires_disambiguation() {
    log "Test: kill requires job id when command prefix is ambiguous"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 5 & pid1=\$!; sleep 5 & pid2=\$!; kill sl; kill_status=\$?; kill \$pid1 \$pid2 2>/dev/null; wait \$pid1 \$pid2 2>/dev/null; exit \$kill_status" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 1 ] && echo "$output" | grep -q "multiple jobs match command"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: kill sl should require disambiguation (exit $exit_code): $output"
    return 1
}

background_failure_prints_exit_code() {
    log "Test: background command failure reports exit status"
    local output
    # Keep personal command_not_found_handler functions from replacing the background job.
    # Poll for completion so a slow command lookup cannot be killed at shutdown.
    # Waiting with the wait builtin while the job is running suppresses its notification.
    output=$("$CJSH_PATH" -i -N -c '
        slepp 0.01 & failed_pid=$!
        attempts=0
        while kill -0 "$failed_pid" 2>/dev/null; do
            if [ "$attempts" -ge 200 ]; then
                exit --force 1
            fi
            sleep 0.05
            attempts=$((attempts + 1))
        done
        wait "$failed_pid"
        test "$?" -eq 127
    ' 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: cjsh exited with $exit_code"
        return 1
    fi

    if echo "$output" | grep -q "Exit 127"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected Exit 127 notification, got: $output"
    return 1
}

jobname_updates_jobs_output() {
    log "Test: jobname changes display in jobs"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 2 & pid=\$!; if ! jobname \$pid renamed-job; then exit \$?; fi; jobs; kill \$pid 2>/dev/null; wait \$pid 2>/dev/null || true" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: jobname command failed (exit $exit_code): $output"
        return 1
    fi

    if echo "$output" | grep -q "Running[[:space:]]\+renamed-job"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected jobs output to include renamed job, got: $output"
    return 1
}

jobname_affects_command_matching() {
    log "Test: jobname affects fg command resolution"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 0.2 & pid=\$!; if ! jobname \$pid special-name; then exit \$?; fi; fg special-name" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: fg special-name failed (exit $exit_code): $output"
    return 1
}

jobname_rejects_empty_name() {
    log "Test: jobname rejects empty rename"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 1 & pid=\$!; jobname \$pid ' '; status=\$?; kill \$pid 2>/dev/null; wait \$pid 2>/dev/null || true; exit \$status" 2>&1)
    local exit_code=$?

    if [ $exit_code -eq 1 ] && echo "$output" | grep -qi "cannot be empty"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: jobname should reject empty names (exit $exit_code): $output"
    return 1
}

jobname_clear_restores_original_name() {
    log "Test: jobname --clear restores original name"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 2 & pid=\$!; jobname \$pid custom-name >/dev/null; jobname \$pid --clear >/dev/null; jobs; kill \$pid 2>/dev/null; wait \$pid 2>/dev/null || true" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: jobname --clear failed (exit $exit_code): $output"
        return 1
    fi

    if echo "$output" | grep -q "sleep 2"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: jobs output missing original command after clear: $output"
    return 1
}

jobname_clear_short_flag() {
    log "Test: jobname -c clears name"
    local output
    output=$("$CJSH_PATH" -i -c "sleep 2 & pid=\$!; jobname \$pid renamed >/dev/null; jobname \$pid -c >/dev/null; jobs; kill \$pid 2>/dev/null; wait \$pid 2>/dev/null || true" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: jobname -c failed (exit $exit_code): $output"
        return 1
    fi

    if echo "$output" | grep -q "sleep 2"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: jobs output missing original command after -c: $output"
    return 1
}

auto_background_on_stop() {
    log "Test: &^ auto-backgrounds on SIGTSTP"
    local pid_file output_file
    pid_file="$(mktemp /tmp/cjsh_autobg_pid.XXXXXX)"
    output_file="$(mktemp /tmp/cjsh_autobg_out.XXXXXX)"

    "$CJSH_PATH" -i -c "sh -c 'echo \$\$ > $pid_file; sleep 5' &^; jobs; kill -9 %1 2>/dev/null; wait %1 2>/dev/null || true" >"$output_file" 2>&1 &
    local cjsh_pid=$!

    local target_pid
    target_pid="$(wait_for_pid_file "$pid_file")"

    if [ -z "$target_pid" ]; then
        kill "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file"
        echo "FAIL: no foreground PID recorded"
        return 1
    fi

    kill -TSTP "-$target_pid" 2>/dev/null || kill -TSTP "$target_pid" 2>/dev/null

    if ! wait_for_process_exit "$cjsh_pid"; then
        kill -CONT "$cjsh_pid" 2>/dev/null || true
        kill -9 "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file"
        echo "FAIL: cjsh did not exit after SIGTSTP"
        return 1
    fi

    wait "$cjsh_pid"
    local exit_code=$?
    local output
    output="$(cat "$output_file" 2>/dev/null)"

    rm -f "$pid_file" "$output_file"

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: cjsh exited with $exit_code: $output"
        return 1
    fi

    if echo "$output" | grep -q "Stopped"; then
        echo "FAIL: job reported stopped instead of running: $output"
        return 1
    fi

    if echo "$output" | grep -q "Running"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected Running job output, got: $output"
    return 1
}

auto_background_no_caret_command() {
    log "Test: &^ does not spawn caret command"
    local output
    output=$("$CJSH_PATH" -c "sleep 0.05 &^; sleep 0.05 &^ && echo ok" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: cjsh exited with $exit_code: $output"
        return 1
    fi

    if echo "$output" | grep -q "cjsh: \^"; then
        echo "FAIL: caret command spawned: $output"
        return 1
    fi

    if echo "$output" | grep -q "command not found"; then
        echo "FAIL: unexpected command not found: $output"
        return 1
    fi

    if echo "$output" | grep -q "ok"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected ok output, got: $output"
    return 1
}

auto_background_literal_amp_caret() {
    log "Test: quoted &^ stays literal"
    local output
    output=$("$CJSH_PATH" -c "echo '&^'; echo \\&^" 2>&1)
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: cjsh exited with $exit_code: $output"
        return 1
    fi

    if echo "$output" | grep -q "command not found"; then
        echo "FAIL: unexpected command not found: $output"
        return 1
    fi

    local count
    count=$(echo "$output" | grep -c "&^")
    if [ "$count" -ge 2 ]; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected literal &^ output, got: $output"
    return 1
}

auto_background_ignores_sigstop() {
    log "Test: &^ does not auto-background on SIGSTOP"
    local pid_file output_file
    pid_file="$(mktemp /tmp/cjsh_autobg_pid.XXXXXX)"
    output_file="$(mktemp /tmp/cjsh_autobg_out.XXXXXX)"

    "$CJSH_PATH" -i -c "sh -c 'echo \$\$ > $pid_file; sleep 5' &^; jobs; kill -9 %1 2>/dev/null; wait %1 2>/dev/null || true" >"$output_file" 2>&1 &
    local cjsh_pid=$!

    local target_pid
    target_pid="$(wait_for_pid_file "$pid_file")"
    if [ -z "$target_pid" ]; then
        kill "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file"
        echo "FAIL: no foreground PID recorded"
        return 1
    fi

    kill -STOP "-$target_pid" 2>/dev/null || kill -STOP "$target_pid" 2>/dev/null

    if ! wait_for_process_exit "$cjsh_pid"; then
        kill -CONT "$cjsh_pid" 2>/dev/null || true
        kill -9 "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file"
        echo "FAIL: cjsh did not exit after SIGSTOP"
        return 1
    fi

    wait "$cjsh_pid"
    local exit_code=$?
    local output
    output="$(cat "$output_file" 2>/dev/null)"

    rm -f "$pid_file" "$output_file"

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: cjsh exited with $exit_code: $output"
        return 1
    fi

    if echo "$output" | grep -q "Running"; then
        echo "FAIL: job reported running after SIGSTOP: $output"
        return 1
    fi

    if echo "$output" | grep -q "Stopped"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected Stopped job output, got: $output"
    return 1
}

auto_background_pipeline() {
    log "Test: &^ auto-backgrounds pipeline"
    local pid_file output_file
    pid_file="$(mktemp /tmp/cjsh_autobg_pid.XXXXXX)"
    output_file="$(mktemp /tmp/cjsh_autobg_out.XXXXXX)"

    "$CJSH_PATH" -i -c "sh -c 'echo \$\$ > $pid_file; sleep 5' | cat &^; jobs; kill -9 %1 2>/dev/null; wait %1 2>/dev/null || true" >"$output_file" 2>&1 &
    local cjsh_pid=$!

    local target_pid
    target_pid="$(wait_for_pid_file "$pid_file")"
    if [ -z "$target_pid" ]; then
        kill "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file"
        echo "FAIL: no foreground PID recorded"
        return 1
    fi

    kill -TSTP "-$target_pid" 2>/dev/null || kill -TSTP "$target_pid" 2>/dev/null

    if ! wait_for_process_exit "$cjsh_pid"; then
        kill -CONT "$cjsh_pid" 2>/dev/null || true
        kill -9 "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file"
        echo "FAIL: cjsh did not exit after SIGTSTP"
        return 1
    fi

    wait "$cjsh_pid"
    local exit_code=$?
    local output
    output="$(cat "$output_file" 2>/dev/null)"

    rm -f "$pid_file" "$output_file"

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: cjsh exited with $exit_code: $output"
        return 1
    fi

    if echo "$output" | grep -q "Stopped"; then
        echo "FAIL: job reported stopped instead of running: $output"
        return 1
    fi

    if echo "$output" | grep -q "Running"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected Running job output, got: $output"
    return 1
}

auto_background_with_redirection() {
    log "Test: &^ works with redirection"
    local pid_file output_file redir_file
    pid_file="$(mktemp /tmp/cjsh_autobg_pid.XXXXXX)"
    output_file="$(mktemp /tmp/cjsh_autobg_out.XXXXXX)"
    redir_file="$(mktemp /tmp/cjsh_autobg_redir.XXXXXX)"

    "$CJSH_PATH" -i -c "sh -c 'echo \$\$ > $pid_file; sleep 5' > $redir_file &^; jobs; kill -9 %1 2>/dev/null; wait %1 2>/dev/null || true" >"$output_file" 2>&1 &
    local cjsh_pid=$!

    local target_pid
    target_pid="$(wait_for_pid_file "$pid_file")"
    if [ -z "$target_pid" ]; then
        kill "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file" "$redir_file"
        echo "FAIL: no foreground PID recorded"
        return 1
    fi

    kill -TSTP "-$target_pid" 2>/dev/null || kill -TSTP "$target_pid" 2>/dev/null

    if ! wait_for_process_exit "$cjsh_pid"; then
        kill -CONT "$cjsh_pid" 2>/dev/null || true
        kill -9 "$cjsh_pid" 2>/dev/null || true
        wait "$cjsh_pid" 2>/dev/null || true
        rm -f "$pid_file" "$output_file" "$redir_file"
        echo "FAIL: cjsh did not exit after SIGTSTP"
        return 1
    fi

    wait "$cjsh_pid"
    local exit_code=$?
    local output
    output="$(cat "$output_file" 2>/dev/null)"

    rm -f "$pid_file" "$output_file" "$redir_file"

    if [ $exit_code -ne 0 ]; then
        echo "FAIL: cjsh exited with $exit_code: $output"
        return 1
    fi

    if echo "$output" | grep -q "Stopped"; then
        echo "FAIL: job reported stopped instead of running: $output"
        return 1
    fi

    if echo "$output" | grep -q "Running"; then
        echo "PASS"
        return 0
    fi

    echo "FAIL: expected Running job output, got: $output"
    return 1
}

if ! test_background_persists; then
    status=1
fi

if ! test_huponexit_kills_jobs; then
    status=1
fi

if ! test_disown_removes_job; then
    status=1
fi

if ! test_disown_pid_and_hup_mark; then
    status=1
fi

if ! test_wait_next_and_pipeline_lifetime; then
    status=1
fi

if ! test_jobs_options_and_jobspecs; then
    status=1
fi

if ! test_disown_all_and_running_filters; then
    status=1
fi

if ! test_wait_multi_job_and_jobspec_matrix; then
    status=1
fi

if ! jobs_reports_when_empty; then
    status=1
fi

if ! jobs_p_option_stays_silent_when_empty; then
    status=1
fi

if ! fg_command_name_resolves_job; then
    status=1
fi

if ! fg_without_arg_resolves_only_job; then
    status=1
fi

if ! bg_without_arg_resolves_only_stopped_job; then
    status=1
fi

if ! fg_command_prefix_resolves_job; then
    status=1
fi

if ! fg_command_name_requires_disambiguation; then
    status=1
fi

if ! fg_command_prefix_requires_disambiguation; then
    status=1
fi

if ! kill_command_name_resolves_job; then
    status=1
fi

if ! kill_command_prefix_resolves_job; then
    status=1
fi

if ! kill_command_name_requires_disambiguation; then
    status=1
fi

if ! kill_command_prefix_requires_disambiguation; then
    status=1
fi

if ! background_failure_prints_exit_code; then
    status=1
fi

if ! jobname_updates_jobs_output; then
    status=1
fi

if ! jobname_affects_command_matching; then
    status=1
fi

if ! jobname_rejects_empty_name; then
    status=1
fi

if ! jobname_clear_restores_original_name; then
    status=1
fi

if ! jobname_clear_short_flag; then
    status=1
fi

if ! auto_background_on_stop; then
    status=1
fi

if ! auto_background_no_caret_command; then
    status=1
fi

if ! auto_background_literal_amp_caret; then
    status=1
fi

if ! auto_background_ignores_sigstop; then
    status=1
fi

if ! auto_background_pipeline; then
    status=1
fi

if ! auto_background_with_redirection; then
    status=1
fi

exit $status
