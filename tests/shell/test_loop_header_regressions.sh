#!/usr/bin/env sh

# test_loop_header_regressions.sh
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

# Regression coverage for literal loop headers, diagnostics, and word expansion.
CJSH_PATH=${CJSH:-"$(cd "$(dirname "$0")/../../build/release" && pwd)/cjsh"}
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cjsh-loop-headers.XXXXXX") || exit 1
trap 'rm -f "$TEST_DIR/stdout" "$TEST_DIR/stderr" "$TEST_DIR/script"; rmdir "$TEST_DIR"' EXIT
PASSED=0
FAILED=0

pass() {
    printf 'PASS: %s\n' "$1"
    PASSED=$((PASSED + 1))
}

fail() {
    printf 'FAIL: %s (status=%s, stdout=%s, stderr=%s)\n' \
        "$1" "$result" "$(cat "$TEST_DIR/stdout")" "$(cat "$TEST_DIR/stderr")"
    FAILED=$((FAILED + 1))
}

check_error() {
    # Check the stream, status, useful diagnostic, and that the loop body did not run.
    if [ "$result" -eq 2 ] && [ ! -s "$TEST_DIR/stdout" ] &&
       grep -Fq 'syntax error' "$TEST_DIR/stderr" &&
       grep -Fq "$2" "$TEST_DIR/stderr"; then
        pass "$1"
    else
        fail "$1"
    fi
}

expect_error() {
    description=$1 script=$2 diagnostic=$3
    shift 3
    "$CJSH_PATH" --secure "$@" -c "$script" < /dev/null \
        > "$TEST_DIR/stdout" 2> "$TEST_DIR/stderr"
    result=$?
    check_error "$description" "$diagnostic"
}

expect_output() {
    description=$1 script=$2 expected=$3
    shift 3
    "$CJSH_PATH" --secure "$@" -c "$script" < /dev/null \
        > "$TEST_DIR/stdout" 2> "$TEST_DIR/stderr"
    result=$?
    if [ "$result" -eq 0 ] && [ "$(cat "$TEST_DIR/stdout")" = "$expected" ] &&
       [ ! -s "$TEST_DIR/stderr" ]; then
        pass "$description"
    else
        fail "$description"
    fi
}

expect_error 'original multiline typo reports the unexpected token' \
    'for i n {1..1000}; do
echo "$i"
done' "unexpected token 'n'"
expect_error 'in is rejected as a likely mistyped loop variable' \
    'for in in {1..10};
do
echo $i
done' "unexpected 'in' where a loop variable was expected"
expect_error 'select rejects in as its variable too' \
    'select in in one; do echo BODY; break; done' "unexpected 'in'"
expect_error 'nested loop rejects in before expanding its list' \
    'for outer in one; do for in in $(echo SIDE_EFFECT >&2); do echo BODY; done; done' \
    "unexpected 'in'"
expect_error 'noexec rejects in as a loop variable' \
    'for in in one; do echo BODY; done' "unexpected 'in'" --no-exec
expect_error 'malformed conditional reports the header typo' \
    'while false; do
    f [ 1 -eq 1 ]; then
        echo BODY
    fi
done' "possible typo 'f' instead of 'if'"
expect_error 'inline malformed conditional aborts before executing commands' \
    'echo BEFORE; f [ 1 -eq 1 ]; then echo BODY; fi' "'then' without matching"
expect_error 'POSIX mode reports an unexpected then' \
    'f [ 1 -eq 1 ]; then echo BODY; fi' "'then' without matching" --posix
expect_error 'noexec reports an unexpected then' \
    'f [ 1 -eq 1 ]; then echo BODY; fi' "'then' without matching" --no-exec
expect_error 'inline typo reports expected in' \
    'for i n {1..1000}; do echo BODY; done' "expected 'in'"
expect_error 'typo cannot fall back to positional parameters' \
    'set -- one two; for i n {1..3}; do echo BODY; done' "unexpected token 'n'"
expect_error 'missing in before a list is rejected' \
    'for i one two; do echo BODY; done' "unexpected token 'one'"
expect_error 'quoted in is not a keyword' \
    'for i "in" one; do echo BODY; done' 'unexpected token'
expect_error 'expanded in is not a keyword' \
    'kw=in; for i $kw one; do echo BODY; done' 'unexpected token'
expect_error 'numeric for variable is rejected' \
    'for 1i in one; do echo BODY; done' "invalid loop variable '1i'"
expect_error 'punctuation in for variable is rejected' \
    'for bad-name in one; do echo BODY; done' "invalid loop variable 'bad-name'"
expect_error 'quoted for variable is rejected' \
    'for "i" in one; do echo BODY; done' 'invalid loop variable'
expect_error 'expanded for variable is rejected' \
    'name=i; for $name in one; do echo BODY; done' 'invalid loop variable'
expect_error 'missing for variable is rejected' \
    'for ; do echo BODY; done' 'loop variable'
expect_error 'missing for variable without whitespace is rejected' \
    'for; do echo BODY; done' 'loop variable'
expect_error 'multiline header with a separate do is rejected' \
    'for i n one
do
echo BODY
done' "unexpected token 'n'"
expect_error 'function loop reports header errors' \
    'f() { for i n one; do echo BODY; done; }; f' "unexpected token 'n'"
expect_error 'nested loop reports header errors' \
    'for outer in one; do for i n one; do echo BODY; done; done' "unexpected token 'n'"
expect_error 'logical chain loop reports header errors' \
    'true && for i n one; do echo BODY; done' "unexpected token 'n'"
expect_error 'POSIX mode rejects malformed for header' \
    'for i n one; do echo BODY; done' "unexpected token 'n'" --posix
expect_error 'noexec reports malformed for header' \
    'for i n one; do echo BODY; done' "unexpected token 'n'" --no-exec

expect_error 'select typo prints a syntax error' \
    'select i n one; do echo BODY; break; done' "unexpected token 'n'"
expect_error 'select typo cannot fall back to positional parameters' \
    'set -- one two; select i n one; do echo BODY; break; done' "unexpected token 'n'"
expect_error 'select invalid variable prints a syntax error' \
    'select 1i in one; do echo BODY; break; done' "invalid loop variable '1i'"
expect_error 'select quoted variable is rejected before expansion' \
    'select "i" in one; do echo BODY; break; done' 'invalid loop variable'
expect_error 'select expanded variable is rejected' \
    'name=i; select $name in one; do echo BODY; break; done' 'invalid loop variable'
expect_error 'select missing variable prints a syntax error' \
    'select ; do echo BODY; break; done' 'loop variable'
expect_error 'select missing variable without whitespace is rejected' \
    'select; do echo BODY; break; done' 'loop variable'
expect_error 'select explicit empty list prints a syntax error' \
    'select i in; do echo BODY; break; done' "after 'in'"
expect_error 'multiline select typo reports the token' \
    'select i n one; do
echo BODY
break
done' "unexpected token 'n'"
expect_error 'select header inside function reports an error' \
    'f() { select i n one; do echo BODY; break; done; }; f' "unexpected token 'n'"
expect_error 'malformed C-style loop cannot become a positional loop' \
    'f() { for ((i=0; i<2)); do echo BODY; done; }; f' 'C-style loop header'
expect_error 'C-style loop rejects trailing header words' \
    'f() { for ((i=0; i<2; i++)) extra; do echo BODY; done; }; f' 'C-style loop header'

original='for i n {1..1000}; do
echo "$i"
done'
printf '%s\n' "$original" > "$TEST_DIR/script"
"$CJSH_PATH" --secure "$TEST_DIR/script" > "$TEST_DIR/stdout" 2> "$TEST_DIR/stderr"
result=$?
check_error 'script file reports the malformed header' "unexpected token 'n'"
"$CJSH_PATH" --secure < "$TEST_DIR/script" > "$TEST_DIR/stdout" 2> "$TEST_DIR/stderr"
result=$?
check_error 'stdin script reports the malformed header' "unexpected token 'n'"

expect_output 'for without in uses positional parameters' \
    'set -- "one two" "" three; for i; do printf "<%s>" "$i"; done' '<one two><><three>'
expect_output 'POSIX mode preserves in as a loop variable' \
    'for in in one two; do printf "<%s>" "$in"; done' '<one><two>' --posix
expect_output 'f remains a valid function and command name' \
    'f() { printf "<%s>" "$1"; }; f literal' '<literal>'
expect_output 'for without positional parameters does nothing successfully' \
    'for i; do echo BODY; done' ''
expect_output 'multiline positional loop is preserved' \
    'set -- one two
for i
do
printf "<%s>" "$i"
done' '<one><two>'
expect_output 'comments after the loop variable are allowed' \
    'set -- one two
for i # comment
do
printf "<%s>" "$i"
done' '<one><two>'
expect_output 'tab-separated header is allowed' \
    'for	i	in	one two; do printf "<%s>" "$i"; done' '<one><two>'
expect_output 'in can follow the variable on the next line' \
    'for i
in one two
do
printf "<%s>" "$i"
done' '<one><two>'
expect_output 'explicit empty for list stays empty with positional arguments' \
    'set -- one two; for i in; do echo BODY; done' ''
expect_output 'empty for list is valid in POSIX mode' \
    'for i in ; do echo BODY; done' '' --posix
expect_output 'keyword-looking words are valid iteration items' \
    'for i in do done then in; do printf "<%s>" "$i"; done' '<do><done><then><in>'
expect_output 'quoted and empty iteration words are preserved' \
    'for _i2 in "one two" "" three; do printf "<%s>" "$_i2"; done' '<one two><><three>'
expect_output 'empty list expansion is valid for select' \
    'unset empty; select i in $empty; do echo BODY; break; done' ''
expect_output 'select without positional parameters does nothing successfully' \
    'select i; do echo BODY; break; done' ''
expect_output 'ordinary C-style loops still work' \
    'for ((i=0; i<3; i++)); do printf "<%s>" "$i"; done' '<0><1><2>'
expect_output 'brace range keeps following list words' \
    'for i in {1..3} last; do printf "<%s>" "$i"; done' '<1><2><3><last>'
expect_output 'alphabetic brace range is expanded' \
    'for i in {a..c}; do printf "<%s>" "$i"; done' '<a><b><c>'
expect_output 'quoted brace range remains literal' \
    'for i in "{1..3}"; do printf "<%s>" "$i"; done' '<{1..3}>'
expect_output 'range prefix and suffix are preserved' \
    'for i in pre{1..2}post; do printf "<%s>" "$i"; done' '<pre1post><pre2post>'
expect_output 'numeric range padding is preserved' \
    'for i in {01..03}; do printf "<%s>" "$i"; done' '<01><02><03>'
expect_output 'descending range stride is preserved' \
    'for i in {5..1..2}; do printf "<%s>" "$i"; done' '<5><3><1>'
expect_output 'malformed brace range remains a literal word' \
    'for i in {a..3}; do printf "<%s>" "$i"; done' '<{a..3}>'

expect_output 'command substitution supplies the iteration list' \
    'for i in $(seq 1 13); do printf "<%s>" "$i"; done' \
    '<1><2><3><4><5><6><7><8><9><10><11><12><13>'
expect_output 'command substitution splits newlines and removes trailing newlines' \
    'for i in $(printf "one\ntwo\n\n"); do printf "<%s>" "$i"; done' '<one><two>'
expect_output 'backtick substitution supplies the iteration list' \
    'for i in `printf "one two"`; do printf "<%s>" "$i"; done' '<one><two>'
expect_output 'quoted command substitution preserves whitespace' \
    'for i in "$(printf "one  two\nthree\n\n")"; do printf "<%s>" "$i"; done' '<one  two
three>'
expect_output 'single-quoted substitution stays literal' \
    "for i in '\$(echo one two)'; do printf '<%s>' \"\$i\"; done" '<$(echo one two)>'
expect_output 'empty unquoted substitution produces no iterations' \
    'for i in $(printf ""); do echo BODY; done' ''
expect_output 'empty quoted substitution produces one empty item' \
    'for i in "$(printf "")"; do printf "<%s>" "$i"; done' '<>'
expect_output 'substitutions preserve surrounding list words and affixes' \
    'for i in first pre$(printf "one two")post last; do printf "<%s>" "$i"; done' \
    '<first><preone><twopost><last>'
expect_output 'nested command substitution can access shell variables' \
    'value="one two"; for i in $(printf "%s" "$(printf "%s" "$value")"); do printf "<%s>" "$i"; done' \
    '<one><two>'
expect_output 'list expansion happens once before the body' \
    'value="one two"; values() { printf "%s" "$value"; }; for i in $(values); do value=changed; printf "<%s>" "$i"; done' \
    '<one><two>'
expect_output 'body substitutions use the current iteration value' \
    'for i in $(printf "one two"); do printf "<%s>" "$(printf "%s" "$i")"; done' '<one><two>'
expect_output 'function calls re-expand their loop lists' \
    'f() { for i in $(printf "%s" "$1"); do printf "<%s>" "$i"; done; }; f "one two"; f three' \
    '<one><two><three>'
expect_output 'nested loops expand their lists in the outer iteration' \
    'for outer in one two; do for i in $(printf "%s" "$outer"); do printf "<%s>" "$i"; done; done' \
    '<one><two>'
expect_output 'command substitution respects custom IFS' \
    'IFS=:; for i in $(printf "one:two"); do printf "<%s>" "$i"; done' '<one><two>'
expect_output 'list substitution status does not affect assignments in the body' \
    'for i in $(printf one; false); do value=ok; printf "<%s>" "$?"; done' '<0>'
expect_output 'parameter values are not expanded twice in loop lists' \
    "value='\$other'; other=BAD; for i in \"\${value}\"; do printf '<%s>' \"\$i\"; done" '<$other>'
expect_output 'POSIX mode expands command substitutions in loop lists' \
    'for i in $(printf "one two"); do printf "<%s>" "$i"; done' '<one><two>' --posix
expect_output 'noexec does not execute loop list substitutions' \
    'for i in $(printf SIDE_EFFECT >&2; printf one); do echo BODY; done' '' --no-exec

literal_output=$(cat <<'EOF'
value=expanded
for i in $(printf '%s\n' '$value' '$(echo BAD)' 'a;b' '"quoted"' '{1..3}'); do
    printf '<%s>' "$i"
done
EOF
)
expect_output 'substitution output is not interpreted as shell syntax' \
    "$literal_output" '<$value><$(echo><BAD)><a;b><"quoted"><{1..3}>'

rainbow_loop=$(cat <<'EOF'
for i in $(seq 1 13); do
    printf '\033[38;5;%dm%s' $((196 + (i-1)%6*6)) \
        "$(echo 'Hello, World!' | cut -c$i)"
done
printf '\033[0m\n'
EOF
)
rainbow_expected=$(printf '\033[38;5;196mH\033[38;5;202me\033[38;5;208ml\033[38;5;214ml\033[38;5;220mo\033[38;5;226m,\033[38;5;196m \033[38;5;202mW\033[38;5;208mo\033[38;5;214mr\033[38;5;220ml\033[38;5;226md\033[38;5;196m!\033[0m')
expect_output 'original multiline colored greeting' "$rainbow_loop" "$rainbow_expected"
printf '%s\n' "$rainbow_loop" > "$TEST_DIR/script"
for input_mode in file stdin; do
    if [ "$input_mode" = file ]; then
        "$CJSH_PATH" --secure "$TEST_DIR/script" > "$TEST_DIR/stdout" 2> "$TEST_DIR/stderr"
    else
        "$CJSH_PATH" --secure < "$TEST_DIR/script" > "$TEST_DIR/stdout" 2> "$TEST_DIR/stderr"
    fi
    result=$?
    if [ "$result" -eq 0 ] && [ "$(cat "$TEST_DIR/stdout")" = "$rainbow_expected" ] &&
       [ ! -s "$TEST_DIR/stderr" ]; then
        pass "colored greeting from $input_mode"
    else
        fail "colored greeting from $input_mode"
    fi
done

expect_output 'select accepts an empty expanded list' \
    'select i in $(printf ""); do echo BODY; break; done' ''
printf '2\n' | "$CJSH_PATH" --secure -c \
    'PS3=""; select i in $(printf "one two") "$(printf "three four")"; do printf "<%s>" "$i"; break; done' \
    > "$TEST_DIR/stdout" 2> "$TEST_DIR/stderr"
result=$?
if [ "$result" -eq 0 ] && [ "$(cat "$TEST_DIR/stdout")" = '<two>' ] &&
   [ "$(cat "$TEST_DIR/stderr")" = '1) one
2) two
3) three four' ]; then
    pass 'select expands both unquoted and quoted command substitutions'
else
    fail 'select expands both unquoted and quoted command substitutions'
fi

printf '\nLoop header regressions: %s passed, %s failed\n' "$PASSED" "$FAILED"
[ "$FAILED" -eq 0 ]
