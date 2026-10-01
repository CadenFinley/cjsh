#!/usr/bin/env sh
# test_posix_mode_restrictions.sh
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

# POSIX mode restriction tests

TOTAL=0
PASSED=0
FAILED=0

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_SHELL="$SCRIPT_DIR/../../build/cjsh"

if [ -n "$1" ]; then
    SHELL_TO_TEST="$1"
elif [ -z "$SHELL_TO_TEST" ]; then
    if [ -n "$CJSH" ]; then
        SHELL_TO_TEST="$CJSH"
    else
        SHELL_TO_TEST="$DEFAULT_SHELL"
    fi
fi

if [ "${SHELL_TO_TEST#/}" = "$SHELL_TO_TEST" ]; then
    SHELL_TO_TEST="$(pwd)/$SHELL_TO_TEST"
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

if [ ! -x "$SHELL_TO_TEST" ]; then
    echo "Error: Shell '$SHELL_TO_TEST' not found or not executable"
    echo "Usage: $0 [path_to_shell]"
    exit 1
fi

tmpdir=$(mktemp -d)
if [ -z "$tmpdir" ] || [ ! -d "$tmpdir" ]; then
    echo "Error: could not create temporary directory"
    exit 1
fi
trap 'rm -rf "$tmpdir"' EXIT INT TERM

tmp_source_file="$tmpdir/source_file.sh"
printf 'echo from source\n' > "$tmp_source_file"

tmp_glob_root="$tmpdir/glob_root"
mkdir -p "$tmp_glob_root/a/b"
: > "$tmp_glob_root/a/b/deep.txt"

echo "Testing POSIX mode restrictions for: $SHELL_TO_TEST"
echo "==================================================="

run_expect_posix_code() {
    run_expect_fail "$1" "$2" "$3"
}

run_expect_fail() {
    local description=$1
    local command=$2
    local pattern=$3

    log_test "$description"
    output=$("$SHELL_TO_TEST" --posix -c "$command" 2>&1)
    status=$?

    if [ "$status" -ne 0 ] && printf "%s" "$output" | grep -Fq -- "$pattern"; then
        pass
    else
        clean_output=$(printf "%s" "$output" | tr '\n' ' ')
        fail "Expected failure with pattern '$pattern' (status=$status, output=$clean_output)"
    fi
}

run_expect_literal() {
    local description=$1
    local command=$2
    local expected=$3

    log_test "$description"
    output=$("$SHELL_TO_TEST" --posix -c "$command" 2>/dev/null)
    if [ "$output" = "$expected" ]; then
        pass
    else
        fail "Expected '$expected', got '$output'"
    fi
}

run_expect_posix_code "[[ forbidden" "[[ 1 == 1 ]]" "POSIX001"
run_expect_posix_code "function keyword disabled" "function foo { :; }; foo" "POSIX002"
run_expect_posix_code "array assignment disabled" "arr=(1 2)" "POSIX005"
run_expect_posix_code "+= assignment disabled" "x=1; x+=2" "POSIX006"
run_expect_posix_code "|& pipeline disabled" "echo hi |& cat" "POSIX007"
run_expect_posix_code "&^ auto-background disabled" "true &^" "POSIX012"
run_expect_posix_code "&^! auto-background silent disabled" "true &^!" "POSIX012"
run_expect_posix_code "&> redirection disabled" "echo hi &> '$tmpdir/out.txt'" "POSIX008"
run_expect_posix_code "&>> redirection disabled" "echo hi &>> '$tmpdir/out.txt'" "POSIX008"
run_expect_posix_code "here-string disabled" "cat <<< hi" "POSIX004"
run_expect_posix_code "process substitution input disabled" "cat <(echo hi)" "POSIX003"
run_expect_posix_code "process substitution output disabled" "echo hi > >(cat)" "POSIX003"
run_expect_posix_code "source builtin syntax disabled" "source '$tmp_source_file'" "POSIX009"
run_expect_posix_code "local builtin syntax disabled" "local foo=1" "POSIX010"
run_expect_posix_code "declare builtin syntax disabled" "declare foo=1" "POSIX011"
run_expect_posix_code "typeset builtin syntax disabled" "typeset foo=1" "POSIX011"
run_expect_literal "POSIX.1-2024 case fall-through" \
    "case x in x) printf one ;& y) printf two ;; esac" "onetwo"
run_expect_posix_code "case ;;& pattern continuation disabled" \
    "case x in x) echo one ;;& x) echo two ;; esac" "POSIX013"
run_expect_fail "source builtin disabled at runtime" \
    "if true; then source '$tmp_source_file'; fi" \
    "'source' is disabled in POSIX mode"
run_expect_fail "local builtin disabled at runtime" "f() { local foo=1; }; f" \
    "'local' is disabled in POSIX mode"
run_expect_fail "declare builtin disabled at runtime" "if true; then declare foo=1; fi" \
    "'declare' is disabled in POSIX mode"
run_expect_fail "typeset builtin disabled at runtime" "if true; then typeset foo=1; fi" \
    "'typeset' is disabled in POSIX mode"
run_expect_fail "source disabled through command builtin" "command source '$tmp_source_file'" \
    "'source' is disabled in POSIX mode"
run_expect_fail "local disabled through command builtin" "command local foo=1" \
    "'local' is disabled in POSIX mode"
run_expect_fail "declare disabled through command builtin" "command declare foo=1" \
    "'declare' is disabled in POSIX mode"
run_expect_fail "shopt -s globstar disabled" "shopt -s globstar" "not available in POSIX mode"
run_expect_literal "POSIX.1-2024 pipefail" 'set -o pipefail; false | true; echo "$?"' "1"
run_expect_fail "shopt -s huponexit disabled" "shopt -s huponexit" "not available in POSIX mode"
run_expect_fail "pushd builtin disabled" "pushd /tmp" "not available in POSIX mode"
run_expect_fail "popd builtin disabled" "popd" "not available in POSIX mode"
run_expect_fail "dirs builtin disabled" "dirs" "not available in POSIX mode"
run_expect_fail "help builtin disabled" "help" "not available in POSIX mode"
run_expect_fail "version builtin disabled" "version" "not available in POSIX mode"
run_expect_fail "history builtin disabled" "history" "not available in POSIX mode"
run_expect_fail "restart builtin disabled" "restart" "not available in POSIX mode"
run_expect_literal "XSI type builtin available" 'type echo >/dev/null; echo "$?"' "0"
run_expect_fail "which builtin disabled" "which echo" "not available in POSIX mode"
run_expect_fail "jobname builtin disabled" "jobname %1 test-name" "not available in POSIX mode"
run_expect_fail "disown builtin disabled" "disown" "not available in POSIX mode"
run_expect_fail "coproc builtin disabled" "coproc echo hi" "not available in POSIX mode"
run_expect_fail "cjshopt builtin disabled" "cjshopt" "not available in POSIX mode"
run_expect_fail "hook builtin disabled" "hook" "not available in POSIX mode"
run_expect_fail "abbr builtin disabled" "abbr" "not available in POSIX mode"
run_expect_fail "unabbr builtin disabled" "unabbr" "not available in POSIX mode"
run_expect_fail "approot builtin disabled" "approot" "not available in POSIX mode"
run_expect_fail "generate-completions builtin disabled" "generate-completions" \
    "not available in POSIX mode"
run_expect_fail "cjsh-widget builtin disabled" "cjsh-widget" "not available in POSIX mode"
run_expect_fail "quit alias disabled" "quit" "not available in POSIX mode"
run_expect_fail "bye alias disabled" "bye" "not available in POSIX mode"
run_expect_fail "builtin utility disabled" "builtin echo hi" "not available in POSIX mode"
run_expect_fail "help disabled through command builtin" "command help" \
    "not available in POSIX mode"
run_expect_fail "quit disabled through command builtin" "command quit" \
    "not available in POSIX mode"
run_expect_literal "dot builtin allowed" ". '$tmp_source_file'" "from source"

run_expect_literal "brace expansion stays literal" "echo {1..3}" "{1..3}"
run_expect_literal "tilde expands HOME" "HOME=/tmp/cjsh_posix_home; echo ~" "/tmp/cjsh_posix_home"
run_expect_literal "disabled syntax stays literal in single quotes" \
    "printf '%s' '[[ not-a-conditional ]]'" "[[ not-a-conditional ]]"
run_expect_literal "disabled syntax stays literal in double quotes" \
    "printf '%s' \"x+=2\"" "x+=2"
run_expect_literal "disabled builtin token stays literal in quotes" \
    "printf '%s' \"source $tmp_source_file\"" "source $tmp_source_file"

log_test "POSIXLY_CORRECT forced to 1"
posix_env_output=$(POSIXLY_CORRECT=0 "$SHELL_TO_TEST" --posix -c 'printf "%s" "$POSIXLY_CORRECT"' 2>/dev/null)
if [ "$posix_env_output" = "1" ]; then
    pass
else
    fail "Expected POSIXLY_CORRECT=1, got '$posix_env_output'"
fi

log_test "POSIXLY_CORRECT remains unchanged without --posix"
non_posix_env_output=$(POSIXLY_CORRECT=0 "$SHELL_TO_TEST" -c 'printf "%s" "$POSIXLY_CORRECT"' 2>/dev/null)
if [ "$non_posix_env_output" = "0" ]; then
    pass
else
    fail "Expected POSIXLY_CORRECT=0 without --posix, got '$non_posix_env_output'"
fi

log_test "globstar remains literal"
glob_output=$("$SHELL_TO_TEST" --posix -c "cd '$tmp_glob_root' && printf '%s' **/deep.txt" 2>/dev/null)
if [ "$glob_output" = "**/deep.txt" ]; then
    pass
else
    fail "Expected literal '**/deep.txt', got '$glob_output'"
fi

log_test "smart cd fallback disabled in POSIX mode"
smart_cd_output=$("$SHELL_TO_TEST" --posix -c "cd '$tmpdir'; mkdir -p exact-directory; cd exact-dir" 2>&1)
smart_cd_status=$?
if [ "$smart_cd_status" -ne 0 ] &&
   printf "%s" "$smart_cd_output" | grep -Fq -- "no such file or directory"; then
    pass
else
    clean_output=$(printf "%s" "$smart_cd_output" | tr '\n' ' ')
    fail "Expected smart cd fallback to be disabled (status=$smart_cd_status, output=$clean_output)"
fi

tmp_extension_script="$tmpdir/extension_probe.bash"
printf 'printf "extension-dispatch-ran\\n"\n' > "$tmp_extension_script"
chmod +x "$tmp_extension_script"
mkdir -p "$tmpdir/home"

log_test "extension-based script dispatch disabled"
extension_output=$(HOME="$tmpdir/home" CJSH_ENV= \
    "$SHELL_TO_TEST" --posix -c "$tmp_extension_script" 2>&1)
extension_status=$?
if [ "$extension_status" -ne 0 ] &&
   ! printf "%s" "$extension_output" | grep -Fq -- "extension-dispatch-ran"; then
    pass
else
    clean_output=$(printf "%s" "$extension_output" | tr '\n' ' ')
    fail "Expected extension dispatch to be disabled (status=$extension_status, output=$clean_output)"
fi

log_test "extension-based script dispatch enabled without --posix"
extension_non_posix_output=$(HOME="$tmpdir/home" CJSH_ENV= \
    "$SHELL_TO_TEST" -c "$tmp_extension_script" 2>&1)
extension_non_posix_status=$?
if [ "$extension_non_posix_status" -eq 0 ] &&
   [ "$extension_non_posix_output" = "extension-dispatch-ran" ]; then
    pass
else
    clean_output=$(printf "%s" "$extension_non_posix_output" | tr '\n' ' ')
    fail "Expected extension dispatch in default mode (status=$extension_non_posix_status, output=$clean_output)"
fi

tmp_home="$tmpdir/strict-posix-home"
mkdir -p "$tmp_home"
tmp_env_override="$tmpdir/cjsh_env_override.sh"
printf 'echo from-cjshenv\n' > "$tmp_home/.cjshenv"
printf 'echo from-cjprofile\n' > "$tmp_home/.cjprofile"
printf 'echo from-cjsh-env-override\n' > "$tmp_env_override"

log_test "native startup files skipped in POSIX login mode"
startup_output=$(HOME="$tmp_home" CJSH_ENV="$tmp_env_override" \
    "$SHELL_TO_TEST" --posix --login -c 'echo command-ran' 2>&1)
startup_status=$?
# POSIX login still reads /etc/profile, which may emit platform-specific diagnostics.
if [ "$startup_status" -eq 0 ] &&
   printf "%s\n" "$startup_output" | grep -Fxq -- "command-ran" &&
   ! printf "%s\n" "$startup_output" | grep -Eq '^from-(cjshenv|cjprofile|cjsh-env-override)$'; then
    pass
else
    clean_output=$(printf "%s" "$startup_output" | tr '\n' ' ')
    fail "Expected command output without native startup markers (status=$startup_status, output=$clean_output)"
fi

log_test "command_not_found_handler ignored in POSIX mode"
handler_output=$("$SHELL_TO_TEST" --posix -c '
command_not_found_handler() {
    echo handler-ran
    return 42
}
missing_posix_handler
echo "status:$?"
' 2>&1)
if ! printf "%s" "$handler_output" | grep -Fq -- "handler-ran" &&
   printf "%s" "$handler_output" | grep -Fq -- "command not found" &&
   printf "%s" "$handler_output" | grep -Fq -- "status:127"; then
    pass
else
    clean_output=$(printf "%s" "$handler_output" | tr '\n' ' ')
    fail "Expected default command-not-found behavior, got '$clean_output'"
fi

log_test "cjshexit ignored in POSIX mode"
cjshexit_output=$("$SHELL_TO_TEST" --posix -c '
cjshexit() {
    echo cjshexit-ran
}
true
' 2>&1)
if ! printf "%s" "$cjshexit_output" | grep -Fq -- "cjshexit-ran"; then
    pass
else
    clean_output=$(printf "%s" "$cjshexit_output" | tr '\n' ' ')
    fail "Expected cjshexit to be ignored, got '$clean_output'"
fi

echo "==================================================="
echo "POSIX Mode Restriction Tests Summary:"
echo "Total: $TOTAL"
echo "Passed: ${GREEN}$PASSED${NC}"
echo "Failed: ${RED}$FAILED${NC}"

if [ $FAILED -eq 0 ]; then
    exit 0
else
    exit 1
fi
