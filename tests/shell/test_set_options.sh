#!/usr/bin/env sh
# test_set_options.sh
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

echo "Test: Set command options (POSIX compliance gaps)..."

TESTS_PASSED=0
TESTS_FAILED=0

pass_test() {
    echo "PASS: $1"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

fail_test() {
    echo "FAIL: $1"
    TESTS_FAILED=$((TESTS_FAILED + 1))
}

echo "Test set -e (errexit) option"
output=$("$CJSH_PATH" -c "set -e; false; echo should_not_print" 2>/dev/null)
if [ -z "$output" ]; then
    pass_test "set -e (errexit) - exits on error"
else
    fail_test "set -e (errexit) - did not exit on error, got: '$output'"
fi

echo "Test set +e (disable errexit) option"
output=$("$CJSH_PATH" -c "set -e; set +e; false; echo should_print" 2>/dev/null)
if [ "$output" = "should_print" ]; then
    pass_test "set +e (disable errexit)"
else
    fail_test "set +e (disable errexit) - got: '$output'"
fi

echo "Test set -u (nounset) option"
output=$("$CJSH_PATH" -c "set -u; echo \$UNDEFINED_VAR" 2>&1)
exit_code=$?
if [ $exit_code -ne 0 ]; then
    pass_test "set -u (nounset) - errors on undefined variable"
else
    fail_test "set -u (nounset) - should error on undefined variable, got: '$output'"
fi

echo "Test set +u (allow unset) option"
output=$("$CJSH_PATH" -c "set -u; set +u; echo \$UNDEFINED_VAR" 2>/dev/null)
exit_code=$?
if [ $exit_code -eq 0 ]; then
    pass_test "set +u (allow unset) - allows undefined variables"
else
    fail_test "set +u (allow unset) - should allow undefined variables"
fi

echo "Test set -x (xtrace) option"
output=$("$CJSH_PATH" -c "set -x; echo hello" 2>&1)
if echo "$output" | grep -q "echo hello"; then
    pass_test "set -x (xtrace) - prints commands"
else
    fail_test "set -x (xtrace) - should print commands, got: '$output'"
fi

echo "Test set +x (disable xtrace) option"
output=$("$CJSH_PATH" -c "set -x; set +x; echo hello" 2>&1)
if [ "$output" = "hello" ]; then
    pass_test "set +x (disable xtrace)"
else
    if echo "$output" | tail -1 | grep -q "^hello$"; then
        pass_test "set +x (disable xtrace)"
    else
        fail_test "set +x (disable xtrace) - got: '$output'"
    fi
fi

echo "Test set -v (verbose) option"
output=$("$CJSH_PATH" -c "set -v; echo test" 2>&1)
if echo "$output" | grep -q "echo test"; then
    pass_test "set -v (verbose) - prints input lines"
else
    fail_test "set -v (verbose) - should print input lines, got: '$output'"
fi

echo "Test set -n (noexec) option"
output=$("$CJSH_PATH" -c "set -n; echo should_not_execute" 2>/dev/null)
if [ -z "$output" ]; then
    pass_test "set -n (noexec) - doesn't execute commands"
else
    fail_test "set -n (noexec) - should not execute, got: '$output'"
fi

echo "Test set -f (noglob) option"
TEST_DIR="/tmp/cjsh_glob_test_$$"
mkdir -p "$TEST_DIR"
touch "$TEST_DIR/file1.txt" "$TEST_DIR/file2.txt"

output=$("$CJSH_PATH" -c "cd $TEST_DIR && set -f; echo *.txt" 2>/dev/null)
if [ "$output" = "*.txt" ]; then
    pass_test "set -f (noglob) - disables globbing"
else
    fail_test "set -f (noglob) - should not expand glob, got: '$output'"
fi

rm -rf "$TEST_DIR"

echo "Test set +f (enable glob) option"
TEST_DIR="/tmp/cjsh_glob_test2_$$"
mkdir -p "$TEST_DIR"
touch "$TEST_DIR/file1.txt" "$TEST_DIR/file2.txt"

output=$("$CJSH_PATH" -c "cd $TEST_DIR && set -f; set +f; echo *.txt" 2>/dev/null)
if echo "$output" | grep -q "file1.txt" && echo "$output" | grep -q "file2.txt"; then
    pass_test "set +f (enable glob) - re-enables globbing"
else
    fail_test "set +f (enable glob) - should expand globs, got: '$output'"
fi

rm -rf "$TEST_DIR"

echo "Test set -C (noclobber) option"
TEST_FILE="/tmp/cjsh_noclobber_$$"
echo "original" > "$TEST_FILE"
"$CJSH_PATH" -c "set -C; echo new > $TEST_FILE" 2>/dev/null
content=$(cat "$TEST_FILE" 2>/dev/null)
if [ "$content" = "original" ]; then
    pass_test "set -C (noclobber) - prevents overwrite"
else
    fail_test "set -C (noclobber) - should prevent overwrite, got: '$content'"
fi
rm -f "$TEST_FILE"

echo "Test set +C (allow clobber) option"
TEST_FILE="/tmp/cjsh_clobber_$$"
echo "original" > "$TEST_FILE"
"$CJSH_PATH" -c "set -C; set +C; echo new > $TEST_FILE" 2>/dev/null
content=$(cat "$TEST_FILE" 2>/dev/null)
if [ "$content" = "new" ]; then
    pass_test "set +C (allow clobber) - allows overwrite"
else
    fail_test "set +C (allow clobber) - should allow overwrite, got: '$content'"
fi
rm -f "$TEST_FILE"

echo "Test set -o errexit (long form)"
output=$("$CJSH_PATH" -c "set -o errexit; false; echo should_not_print" 2>/dev/null)
if [ -z "$output" ]; then
    pass_test "set -o errexit (long form)"
else
    fail_test "set -o errexit (long form) - got: '$output'"
fi

echo "Test set -o (show all options)"
output=$("$CJSH_PATH" -c "set -o" 2>/dev/null)
if [ -n "$output" ]; then
    pass_test "set -o (show options) - displays option status"
else
    fail_test "set -o (show options) - should display options"
fi

echo "Test set +o (show options with plus form)"
output=$("$CJSH_PATH" -c "set +o" 2>/dev/null)
if echo "$output" | grep -q "errexit_severity"; then
    pass_test "set +o (show options) - displays errexit_severity"
else
    fail_test "set +o (show options) - should display errexit_severity"
fi

echo "Test set combined short options (-ex)"
output=$("$CJSH_PATH" -c 'set -ex; printf "%s" "$-"' 2>/dev/null)
if echo "$output" | grep -q "e" && echo "$output" | grep -q "x"; then
    pass_test "set -ex enables both errexit and xtrace"
else
    fail_test "set -ex should enable both options, got: '$output'"
fi

echo "Test set combined disable (+ex)"
output=$("$CJSH_PATH" -c 'set -ex; set +ex; printf "%s" "$-"' 2>/dev/null)
if echo "$output" | grep -q "[ex]"; then
    fail_test "set +ex should disable errexit and xtrace, got: '$output'"
else
    pass_test "set +ex disables both errexit and xtrace"
fi

echo "Test set -oxtrace (inline long option)"
output=$("$CJSH_PATH" -c 'set -oxtrace; printf "%s" "$-"' 2>/dev/null)
if echo "$output" | grep -q "x"; then
    pass_test "set -oxtrace enables tracing via long form"
else
    fail_test "set -oxtrace should enable tracing, got: '$output'"
fi

echo "Test set +oxtrace (inline disable)"
output=$("$CJSH_PATH" -c 'set -oxtrace; set +oxtrace; printf "%s" "$-"' 2>/dev/null)
if echo "$output" | grep -q "x"; then
    fail_test "set +oxtrace should disable tracing, got: '$output'"
else
    pass_test "set +oxtrace disables tracing via long form"
fi

echo "Test shopt -s huponexit"
output=$("$CJSH_PATH" -c 'shopt -s huponexit; shopt huponexit' 2>/dev/null)
if echo "$output" | grep -q "huponexit" && echo "$output" | grep -q "huponexit[[:space:]]*on"; then
    pass_test "shopt -s huponexit enables huponexit"
else
    fail_test "shopt -s huponexit should enable huponexit, got: '$output'"
fi

echo "Test shopt -u huponexit"
output=$("$CJSH_PATH" -c 'shopt -s huponexit; shopt -u huponexit; shopt huponexit' 2>/dev/null)
if echo "$output" | grep -q "huponexit[[:space:]]*off"; then
    pass_test "shopt -u huponexit disables huponexit"
else
    fail_test "shopt -u huponexit should disable huponexit, got: '$output'"
fi

echo "Test set -o pipefail"
output=$("$CJSH_PATH" -c 'set -o pipefail; false | true; echo $?' 2>/dev/null)
if [ "$output" = "1" ]; then
    pass_test "set -o pipefail returns last non-zero status"
else
    fail_test "set -o pipefail should return 1, got: '$output'"
fi

echo "Test set +o pipefail"
output=$("$CJSH_PATH" -c 'set -o pipefail; set +o pipefail; false | true; echo $?' 2>/dev/null)
if [ "$output" = "0" ]; then
    pass_test "set +o pipefail restores last-command status"
else
    fail_test "set +o pipefail should return 0, got: '$output'"
fi

echo "Test pipefail with multi-step pipeline"
output=$("$CJSH_PATH" -c 'set -o pipefail; true | false | true; echo $?' 2>/dev/null)
if [ "$output" = "1" ]; then
    pass_test "pipefail reports failure in middle of pipeline"
else
    fail_test "pipefail middle failure expected 1, got: '$output'"
fi

echo "Test set -o errexit_severity=warning"
output=$("$CJSH_PATH" -c 'set -o errexit_severity=warning; set -o' 2>/dev/null)
if echo "$output" | grep -q "errexit_severity" && echo "$output" | grep -q "warning"; then
    pass_test "set -o errexit_severity=VALUE updates severity"
else
    fail_test "set -o errexit_severity=VALUE should update severity, got: '$output'"
fi

echo "Test set -o errexit-severity critical"
output=$("$CJSH_PATH" -c 'set -o errexit-severity critical; set -o' 2>/dev/null)
if echo "$output" | grep -q "errexit_severity" && echo "$output" | grep -q "critical"; then
    pass_test "set -o errexit-severity VALUE accepts hyphenated name"
else
    fail_test "set -o errexit-severity VALUE should work, got: '$output'"
fi

echo "Test long option --errexit-severity=info"
output=$("$CJSH_PATH" -c 'set --errexit-severity=info; set -o' 2>/dev/null)
if echo "$output" | grep -q "errexit_severity" && echo "$output" | grep -q "info"; then
    pass_test "--errexit-severity=VALUE updates severity"
else
    fail_test "--errexit-severity=VALUE should update severity, got: '$output'"
fi

echo "Test long option --errexit_severity warning"
output=$("$CJSH_PATH" -c 'set --errexit_severity warning; set -o' 2>/dev/null)
if echo "$output" | grep -q "errexit_severity" && echo "$output" | grep -q "warning"; then
    pass_test "--errexit_severity VALUE accepts space-separated form"
else
    fail_test "--errexit_severity VALUE should work, got: '$output'"
fi

echo "Test set -o errexit_severity critical (space after option)"
output=$("$CJSH_PATH" -c 'set -o errexit_severity critical; set -o' 2>/dev/null)
if echo "$output" | grep -q "errexit_severity" && echo "$output" | grep -q "critical"; then
    pass_test "set -o errexit_severity VALUE uses next argument"
else
    fail_test "set -o errexit_severity VALUE should use next argument, got: '$output'"
fi

echo "Test set -e with positional args after --"
output=$("$CJSH_PATH" -c 'set -e -- first second; echo "$1,$2,$#"' 2>/dev/null)
if [ "$output" = "first,second,2" ]; then
    pass_test "set -e -- ARGS preserves positional parameters"
else
    fail_test "set -e -- ARGS should set positional params, got: '$output'"
fi

echo "Test set -e positional args without --"
output=$("$CJSH_PATH" -c 'set -e alpha beta; echo "$1,$2,$#"' 2>/dev/null)
if [ "$output" = "alpha,beta,2" ]; then
    pass_test "set -e ARGS (no --) sets positional params"
else
    fail_test "set -e ARGS should set positional params, got: '$output'"
fi

echo "Test set positional args without any options"
output=$("$CJSH_PATH" -c 'set uno dos tres; echo "$1,$2,$3,$#"' 2>/dev/null)
if [ "$output" = "uno,dos,tres,3" ]; then
    pass_test "set ARGS sets positional parameters"
else
    fail_test "set ARGS should set positional params, got: '$output'"
fi

echo ""
echo "================================"
echo "Set Options Summary:"
echo "  PASSED: $TESTS_PASSED"
echo "  FAILED: $TESTS_FAILED"
echo "================================"

if [ $TESTS_FAILED -gt 0 ]; then
    exit 1
fi

exit 0
