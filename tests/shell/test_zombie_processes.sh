#!/usr/bin/env sh
# test_zombie_processes.sh
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


TOTAL=0
PASSED=0
FAILED=0

if [ -n "$CJSH" ]; then 
    CJSH_PATH="$CJSH"
else 
    CJSH_PATH="$(cd "$(dirname "$0")/../../build" && pwd)/cjsh"
fi

log_test() {
    TOTAL=$((TOTAL + 1))
    printf "Test %03d: %s... " "$TOTAL" "$1"
}

pass() {
    PASSED=$((PASSED + 1))
    printf "${GREEN}PASS${NC}\n"
}

fail() {
    FAILED=$((FAILED + 1))
    printf "${RED}FAIL${NC} - %s\n" "$1"
}

if [ ! -x "$CJSH_PATH" ]; then
    echo "${RED}Error: Shell '$CJSH_PATH' not found or not executable${NC}"
    echo "Usage: $0 [path_to_shell]"
    exit 1
fi

echo "Testing Zombie Process Handling for: $CJSH_PATH"
echo "================================================"

count_zombies() {
    if command -v ps >/dev/null 2>&1; then
        ps axo stat 2>/dev/null | awk '$1 ~ /^Z/ { count++ } END { print count+0 }'
    else
        echo 0
    fi
}

wait_for_zombie_cleanup() {
    local baseline="$1"
    local max_checks="${2:-20}"
    local delay="${3:-0.1}"
    local count=0
    local current="$baseline"
    while [ "$count" -lt "$max_checks" ]; do
        current=$(count_zombies)
        if [ "$current" -le "$baseline" ]; then
            echo "$current"
            return 0
        fi
        sleep "$delay"
        count=$((count + 1))
    done
    echo "$current"
    return 1
}

log_test "No zombies from simple commands"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "echo hello > /dev/null"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Simple command created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Background process with wait doesn't create zombies"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "sleep 0.1 & wait"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Background process with wait created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Multiple background processes cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "sleep 0.1 & sleep 0.1 & sleep 0.1 & wait"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Multiple background processes created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Pipeline processes don't create zombies"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "echo test | cat | wc -l > /dev/null"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Pipeline created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Killed background process cleanup"
ZOMBIES_BEFORE=$(count_zombies)
RESULT=$("$CJSH_PATH" -c "sleep 2 & echo \$!; kill %1; wait %1 2>/dev/null; echo done" 2>/dev/null)
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Killed background process created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Quick-exiting background processes cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "true & true & true & true & true &"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Quick-exiting processes created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Command substitution cleanup"
ZOMBIES_BEFORE=$(count_zombies)
RESULT=$("$CJSH_PATH" -c "echo \$(echo nested \$(echo double))")
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE") && [ "$RESULT" = "nested double" ]; then
    pass
else
    fail "Command substitution created zombies or failed (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER, result: '$RESULT')"
fi

log_test "Process group cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "{ sleep 0.1; echo group; } &"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Process group created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Rapid process creation/termination"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "for i in 1 2 3 4 5; do true & done; wait"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Rapid process creation created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Mixed foreground/background process cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "echo fg1; sleep 0.1 & echo fg2; wait; echo fg3"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Mixed processes created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Subshell process cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "(echo subshell; sleep 0.1) &"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Subshell created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Complex pipeline cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "echo 'line1\nline2\nline3' | grep line | wc -l > /dev/null"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Complex pipeline created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Error in background process cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "nonexistent_command_xyz123 & wait %1 2>/dev/null"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Error in background process created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Signal handling doesn't interfere with cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "sleep 2 & PID=\$!; kill -TERM \$PID; wait \$PID 2>/dev/null"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Signal handling interfered with cleanup (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

log_test "Job control commands cleanup"
ZOMBIES_BEFORE=$(count_zombies)
"$CJSH_PATH" -c "sleep 0.1 & jobs > /dev/null; wait"
if ZOMBIES_AFTER=$(wait_for_zombie_cleanup "$ZOMBIES_BEFORE"); then
    pass
else
    fail "Job control commands created zombies (before: $ZOMBIES_BEFORE, after: $ZOMBIES_AFTER)"
fi

echo ""
echo "================================================"
echo "Zombie Process Test Summary:"
echo "  Total tests: $TOTAL"
echo "  Passed:      ${GREEN}$PASSED${NC}"
echo "  Failed:      ${RED}$FAILED${NC}"
echo "================================================"

if [ $FAILED -eq 0 ]; then
    echo "${GREEN}All zombie process tests passed!${NC}"
    exit 0
else
    echo "${RED}Some zombie process tests failed.${NC}"
    exit 1
fi
