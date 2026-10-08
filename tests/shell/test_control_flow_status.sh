#!/usr/bin/env sh

# test_control_flow_status.sh
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

CJSH_PATH=${CJSH:-${1:-./build/release/cjsh}}
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cjsh-control-flow.XXXXXX") || exit 1
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM
TEST_SOURCE="$WORK_DIR/source.sh"
export TEST_SOURCE
PASSED=0
FAILED=0

check() {
    description=$1
    script=$2
    expected_status=$3
    expected_output=$4
    expected_error=${5:-}
    if [ "$dialect" = posix ]; then
        output=$("$CJSH_PATH" --secure --posix -c "$script" 2>"$WORK_DIR/stderr")
    else
        output=$("$CJSH_PATH" --secure -c "$script" 2>"$WORK_DIR/stderr")
    fi
    status=$?
    diagnostic=$(cat "$WORK_DIR/stderr")
    error_matches=false
    if [ -z "$expected_error" ]; then
        [ -z "$diagnostic" ] && error_matches=true
    else
        case "$diagnostic" in
            *"$expected_error"*) error_matches=true ;;
        esac
    fi
    if [ "$status" -eq "$expected_status" ] && [ "$output" = "$expected_output" ] && "$error_matches"; then
        PASSED=$((PASSED + 1))
        printf 'PASS: %s: %s\n' "$dialect" "$description"
    else
        FAILED=$((FAILED + 1))
        printf 'FAIL: %s: %s (status %s, expected %s)\n' "$dialect" "$description" "$status" "$expected_status"
        printf 'Expected stdout: <%s>\nActual stdout: <%s>\nStderr: %s\n' "$expected_output" "$output" "$diagnostic"
    fi
}

for dialect in native posix; do
    for TEST_STATUS in 253 254 255; do
        export TEST_STATUS
        check "external status $TEST_STATUS" '/bin/sh -c '\''exit "$TEST_STATUS"'\''' "$TEST_STATUS" ''
        check "external status $TEST_STATUS followed by a command" \
            '/bin/sh -c '\''exit "$TEST_STATUS"'\''; printf "%s" "$?"' 0 "$TEST_STATUS"
        check "external status $TEST_STATUS in a function body" \
            'f() { /bin/sh -c '\''exit "$TEST_STATUS"'\''; printf "%s" "$?"; }; f; printf after' \
            0 "${TEST_STATUS}after"
        check "function return $TEST_STATUS is an ordinary caller status" \
            'f() { return "$TEST_STATUS"; echo bad; }; f' "$TEST_STATUS" ''
        check "function return $TEST_STATUS participates in OR lists" \
            'f() { return "$TEST_STATUS"; }; f || printf recovered' 0 recovered
        check "implicit function status $TEST_STATUS participates in OR lists" \
            'f() { /bin/sh -c '\''exit "$TEST_STATUS"'\''; }; f || printf recovered' 0 recovered
        check "function return $TEST_STATUS honors caller errexit" \
            'set -e; f() { return "$TEST_STATUS"; echo bad; }; f; echo bad' "$TEST_STATUS" ''
        check "external status $TEST_STATUS in a loop" \
            'for i in 1 2; do /bin/sh -c '\''exit "$TEST_STATUS"'\''; printf "%s:%s " "$i" "$?"; done' \
            0 "1:$TEST_STATUS 2:$TEST_STATUS "
        check "external status $TEST_STATUS at the end of each loop iteration" \
            'for i in 1 2; do printf "%s" "$i"; /bin/sh -c '\''exit "$TEST_STATUS"'\''; done' \
            "$TEST_STATUS" 12
        check "external status $TEST_STATUS in a while loop" \
            'i=0; while [ "$i" -lt 2 ]; do i=$((i+1)); /bin/sh -c '\''exit "$TEST_STATUS"'\''; printf "%s" "$i"; done' \
            0 12
        check "external status $TEST_STATUS honors errexit" \
            'set -e; /bin/sh -c '\''exit "$TEST_STATUS"'\''; echo bad' "$TEST_STATUS" ''
        check "external status $TEST_STATUS in a pipeline" \
            ': | /bin/sh -c '\''exit "$TEST_STATUS"'\''' "$TEST_STATUS" ''
        check "external status $TEST_STATUS in a subshell" \
            '( /bin/sh -c '\''exit "$TEST_STATUS"'\'' ); printf "%s" "$?"' 0 "$TEST_STATUS"
        check "external status $TEST_STATUS in command substitution" \
            'value=$(/bin/sh -c '\''exit "$TEST_STATUS"'\''); printf "%s" "$?"' 0 "$TEST_STATUS"
        cat >"$TEST_SOURCE" <<'SOURCE'
/bin/sh -c 'exit "$TEST_STATUS"'
printf '%s' "$?"
SOURCE
        check "external status $TEST_STATUS in a sourced file" '. "$TEST_SOURCE"; printf after' \
            0 "${TEST_STATUS}after"
        cat >"$TEST_SOURCE" <<'SOURCE'
return "$TEST_STATUS"
echo bad
SOURCE
        check "source consumes explicit return $TEST_STATUS" '. "$TEST_SOURCE"; printf "%s" "$?"' \
            0 "$TEST_STATUS"
    done

    check 'return zero skips the rest of the function' \
        'f() { return 0; echo bad; }; f; printf after' 0 after
    check 'eval propagates a zero return' \
        'f() { eval "return 0; echo bad"; echo bad; }; f; printf after' 0 after
    check 'return unwinds through a loop and conditional' \
        'f() { for i in 1 2; do if true; then return 7; fi; echo bad; done; echo bad; }; f; printf "%s" "$?"' \
        0 7
    check 'return in a condition unwinds the function' \
        'f() { if return 7; then echo bad; fi; echo bad; }; f; printf "%s" "$?"' 0 7
    check 'break two unwinds both loops' \
        'for i in 1 2; do for j in a b; do printf "%s%s" "$i" "$j"; break 2; echo bad; done; echo bad; done; printf after' \
        0 1aafter
    check 'continue two resumes the outer loop' \
        'for i in 1 2; do for j in a b; do printf "%s%s" "$i" "$j"; continue 2; echo bad; done; echo bad; done; printf after' \
        0 1a2aafter
    check 'break past the outermost loop is consumed' \
        'for i in 1 2; do break 100; done; printf after' 0 after
    check 'builtin wrapper preserves break' \
        'for i in 1 2; do builtin break; echo bad; done; printf after' 0 after
    check 'loop scope ends before trailing commands' \
        'for i in 1; do :; done; break' 1 '' 'break outside loop'
    check 'loop scope ends between script lines' \
        'for i in 1; do :; done
continue' 1 '' 'continue outside loop'
    check 'break in a while condition' 'while break; do echo bad; done; printf after' 0 after
    check 'break stops case fall-through' \
        'for i in 1 2; do case x in x) break ;& *) echo bad ;; esac; echo bad; done; printf after' 0 after
    check 'control variables cannot forge an unwind' \
        'CJSH_BREAK_LEVEL=7; CJSH_CONTINUE_LEVEL=7; CJSH_RETURN_CODE=7; for i in 1 2; do /bin/sh -c "exit 255"; printf "%s" "$i"; done' \
        0 12
    check 'subshell break stays in the child' \
        'for i in 1 2; do (break); printf "%s" "$i"; done' 0 12
    check 'signal trap return can unwind its function' \
        'f() { trap '\''return 7'\'' USR1; kill -USR1 $$; echo bad; }; f; printf "%s" "$?"; trap - USR1' 0 7

done

printf 'Passed: %s\nFailed: %s\n' "$PASSED" "$FAILED"
[ "$FAILED" -eq 0 ]
