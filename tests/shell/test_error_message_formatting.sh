#!/usr/bin/env sh
# test_error_message_formatting.sh
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

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_SHELL="$SCRIPT_DIR/../../build/cjsh"

if [ -n "${1-}" ]; then
    SHELL_TO_TEST="$1"
elif [ -n "${CJSH-}" ]; then
    SHELL_TO_TEST="$CJSH"
else
    SHELL_TO_TEST="$DEFAULT_SHELL"
fi

if [ "${SHELL_TO_TEST#/}" = "$SHELL_TO_TEST" ]; then
    SHELL_TO_TEST="$(pwd)/$SHELL_TO_TEST"
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

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

if [ ! -x "$SHELL_TO_TEST" ]; then
    echo "Error: Shell '$SHELL_TO_TEST' not found or not executable"
    exit 1
fi

TMPDIR_BASE=${TMPDIR:-/tmp}
TMPDIR_BASE=${TMPDIR_BASE%/}
WORK_DIR=$(mktemp -d "$TMPDIR_BASE/cjsh-error-format.XXXXXX")
trap 'chmod -R u+rw "$WORK_DIR" >/dev/null 2>&1; rm -rf "$WORK_DIR"' EXIT

NOPERM_FILE="$WORK_DIR/noperm_file"
printf "secret\n" > "$NOPERM_FILE"
chmod 000 "$NOPERM_FILE"

NESTED_SOURCE_LEVEL3="$WORK_DIR/nested_source_level3.cjsh"
NESTED_SOURCE_LEVEL2="$WORK_DIR/nested_source_level2.cjsh"
NESTED_SOURCE_LEVEL1="$WORK_DIR/nested_source_level1.cjsh"
cat > "$NESTED_SOURCE_LEVEL3" <<EOF
if true
echo nested
EOF
cat > "$NESTED_SOURCE_LEVEL2" <<EOF
source "$NESTED_SOURCE_LEVEL3"
EOF
cat > "$NESTED_SOURCE_LEVEL1" <<EOF
source "$NESTED_SOURCE_LEVEL2"
EOF

run_cmd() {
    cmd="$1"
    output=$("$SHELL_TO_TEST" -c "$cmd" 2>&1)
    status=$?
}

expect_nonempty_output_nonzero() {
    name="$1"
    cmd="$2"
    log_test "$name"
    run_cmd "$cmd"
    if [ -n "$output" ] && [ $status -ne 0 ]; then
        pass
    else
        fail "Expected output and non-zero status; status=$status output='$output'"
    fi
}

expect_output_contains() {
    name="$1"
    cmd="$2"
    needle="$3"
    log_test "$name"
    run_cmd "$cmd"
    if [ -n "$output" ] && printf '%s' "$output" | grep -q "$needle"; then
        pass
    else
        fail "Expected output containing '$needle'; status=$status output='$output'"
    fi
}

expect_output_not_contains() {
    name="$1"
    cmd="$2"
    needle="$3"
    log_test "$name"
    run_cmd "$cmd"
    if printf '%s' "$output" | grep -q "$needle"; then
        fail "Unexpected output containing '$needle'; status=$status output='$output'"
    else
        pass
    fi
}

expect_invalid_option_message() {
    name="$1"
    cmd="$2"
    log_test "$name"
    run_cmd "$cmd"
    if [ -n "$output" ] && [ $status -ne 0 ] && printf '%s' "$output" | grep -E -q "invalid option|invalid argument"; then
        pass
    else
        fail "Expected invalid option/argument message; status=$status output='$output'"
    fi
}

run_option_cmd() {
    output=$("$SHELL_TO_TEST" --secure -c "$1" 2>"$WORK_DIR/option.stderr")
    status=$?
    error_output=$(cat "$WORK_DIR/option.stderr")
}

expect_option_error() {
    name="$1"
    cmd="$2"
    expected_status="$3"
    needle="$4"
    log_test "$name"
    run_option_cmd "$cmd"
    if [ "$status" -eq "$expected_status" ] && [ -z "$output" ] &&
       printf '%s' "$error_output" | grep -Fq -- "$needle"; then
        pass
    else
        fail "Expected status $expected_status, empty stdout and stderr containing '$needle'; status=$status stdout='$output' stderr='$error_output'"
    fi
}

expect_option_success() {
    name="$1"
    cmd="$2"
    expected_output="$3"
    log_test "$name"
    run_option_cmd "$cmd"
    if [ "$status" -eq 0 ] && [ "$output" = "$expected_output" ] && [ -z "$error_output" ]; then
        pass
    else
        fail "Expected status 0, stdout '$expected_output' and empty stderr; status=$status stdout='$output' stderr='$error_output'"
    fi
}

expect_nonempty_output_nonzero "syntax error reports message" "if then; fi"
expect_nonempty_output_nonzero "bad parameter expansion reports message" "unset FOO; echo \${FOO?missing}"
expect_nonempty_output_nonzero "bad arithmetic expansion reports message" "echo \$((1/0))"
expect_nonempty_output_nonzero "bad test bracket reports message" "[ 1 -eq 1"
expect_nonempty_output_nonzero "bad double-bracket reports message" "[[ 1 -eq ]]"
expect_nonempty_output_nonzero "source directory reports message" "source $WORK_DIR"
expect_nonempty_output_nonzero "shift too far reports message" "set -- a b; shift 5"
expect_nonempty_output_nonzero "return outside function reports message" "return 1"
expect_nonempty_output_nonzero "break outside loop reports message" "break"
expect_nonempty_output_nonzero "continue outside loop reports message" "continue"
expect_output_contains "exit invalid numeric reports message" "exit nope" "invalid"
expect_nonempty_output_nonzero "readonly invalid name reports message" "readonly 1abc=3"
expect_nonempty_output_nonzero "unset invalid name reports message" "unset 1abc"
expect_nonempty_output_nonzero "trap invalid signal reports message" "trap 'echo hi' 99999"
expect_nonempty_output_nonzero "test syntax error reports message" "[ 1 -eq ]"
expect_nonempty_output_nonzero "if bad syntax reports message" "if true; fi"
expect_nonempty_output_nonzero "getopts illegal option reports message" "getopts a opt -z"
expect_invalid_option_message "history invalid option reports message" "history -z"
expect_invalid_option_message "kill invalid option reports message" "kill -z 1"
expect_invalid_option_message "generate-completions invalid option reports message" "generate-completions --jobs nope"
expect_output_contains "kill invalid signal identifies the signal" "kill -z 1" "invalid signal"
expect_output_contains "invalid completion job count identifies --jobs" "generate-completions --jobs nope" "jobs: nope"
expect_output_contains "source permission denied reports message" ". $NOPERM_FILE" "permission denied"
expect_output_contains "nested source reports deepest source path" "source $NESTED_SOURCE_LEVEL1" "source $NESTED_SOURCE_LEVEL3"
expect_output_contains "nested source keeps deepest line number" "source $NESTED_SOURCE_LEVEL1" "line 1, source $NESTED_SOURCE_LEVEL3"
expect_output_not_contains "history invalid option avoids interpreter error" "history -z" "unknown interpreter error"
expect_output_not_contains "kill invalid option avoids interpreter error" "kill -z 1" "unknown interpreter error"
expect_output_not_contains "generate-completions invalid option avoids interpreter error" "generate-completions --jobs nope" "unknown interpreter error"

# Keep the complete long option in diagnostics for every affected builtin.
for builtin_name in alias unalias command export hash readonly declare typeset jobs read type which umask; do
    case "$builtin_name" in
        alias|unalias|command|export|hash|readonly|declare|typeset) option_status=2 ;;
        *) option_status=1 ;;
    esac
    for invalid_option in --all --unknown=value; do
        expect_option_error "$builtin_name reports complete $invalid_option" \
            "$builtin_name $invalid_option" "$option_status" \
            "$builtin_name: invalid argument: invalid option: $invalid_option"
    done
done
for invalid_option in --all --unknown=value; do
    expect_option_error "set reports complete $invalid_option" \
        "set $invalid_option" 1 "set: invalid argument: option '$invalid_option' not supported"
done

expect_option_error "jobs reports long option after short options" "jobs -lp --all" 1 "invalid option: --all"
expect_option_error "declare reports long option after short options" "declare -x --all" 2 "invalid option: --all"
expect_option_error "typeset reports long option after plus options" "typeset +x --all" 2 "invalid option: --all"
expect_option_error "set reports long option after short options" "set -u --all" 1 "option '--all' not supported"

# Short-option clusters still identify the offending character.
expect_option_error "jobs identifies invalid clustered short option" "jobs -lz" 1 "invalid option: -z"
expect_option_error "declare identifies invalid clustered short option" "declare -xz" 2 "invalid option: -z"
expect_option_error "typeset identifies invalid clustered plus option" "typeset +xg" 2 "invalid option: +g"
expect_option_error "set identifies invalid clustered short option" "set -uz" 1 "option '-z' not supported"

# After --, option-looking tokens must reach operand handling.
expect_option_success "jobs accepts the option separator" "jobs --" "No jobs"
expect_option_error "jobs treats long option after separator as a job" "jobs -- --all" 1 "--all: no such job"
expect_option_error "declare treats long option after separator as a variable" "declare -- --all" 1 "invalid variable name: --all"
expect_option_error "typeset treats long option after separator as a variable" "typeset -- --all" 1 "invalid variable name: --all"
expect_option_success "set preserves long option after separator" \
    'set -- --all; printf "%s" "$1"' "--all"
expect_option_success "set preserves long option after an operand" \
    'set first --all; printf "%s,%s" "$1" "$2"' "first,--all"

# Supported long options and short-option values bypass invalid-option handling.
expect_option_success "pwd keeps supported long options" "pwd --physical" "$(pwd -P)"
expect_option_success "version keeps supported long options" \
    'tag=$(version --tag) && test -n "$tag"' ""
expect_option_success "set keeps supported long options with inline values" "set --errexit-severity=warning" ""
expect_option_success "set keeps supported long options with separate values" "set --errexit_severity critical" ""
expect_option_success "read accepts a separate option value beginning with --" \
    'printf "hello-world\n" | { read -d -- delimiter_value; printf "%s" "$delimiter_value"; }' "hello"
expect_option_success "read accepts an attached option value beginning with --" \
    'printf "hello-world\n" | { read -d-- delimiter_value; printf "%s" "$delimiter_value"; }' "hello"

echo ""
echo "================================================================"
echo "Error Message Formatting Test Results:"
echo "  Total tests: $TOTAL"
echo "  Passed:      ${GREEN}$PASSED${NC}"
echo "  Failed:      ${RED}$FAILED${NC}"
if [ $FAILED -eq 0 ]; then
    echo "  ${GREEN}All error message formatting tests passed!${NC}"
    exit 0
else
    echo "  ${RED}Some error message formatting tests failed.${NC}"
    exit 1
fi
