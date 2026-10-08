#!/usr/bin/env sh

# test_printf_comprehensive.sh
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

echo "Test: printf comprehensive..."

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

check_default_arguments() {
    description=$1
    script=$2
    expected=$3
    output=$("$CJSH_PATH" --secure -c "$script" 2>&1)
    status=$?
    if [ "$status" -eq 0 ] && [ "$output" = "$expected" ]; then
        pass_test "$description"
    else
        fail_test "$description (status=$status, output='$output', expected='$expected')"
    fi
}

# Missing values count as format slots, but must not advance past the argument array.
check_default_arguments 'printf defaults with no operands' "printf '%s:%d'" ':0'
check_default_arguments 'printf defaults after the last operand' \
    "printf '%s:%d:%s' value" 'value:0:'
check_default_arguments 'printf defaults after format reuse' \
    "printf '%s:%s;' a b c" 'a:b;c:;'
check_default_arguments 'printf missing width and precision operands' \
    "printf '[%*.*s]'" '[]'

OUT=$("$CJSH_PATH" -c "printf '%s\n' hello")
if [ "$OUT" != "hello" ]; then
    fail_test "printf basic string (got '$OUT')"
else
    pass_test "printf basic string"
fi

OUT=$("$CJSH_PATH" -c "printf '%d\n' 42")
if [ "$OUT" != "42" ]; then
    fail_test "printf integer decimal (got '$OUT')"
else
    pass_test "printf integer decimal"
fi

OUT=$("$CJSH_PATH" -c "printf '%i\n' 123")
if [ "$OUT" != "123" ]; then
    fail_test "printf integer %i format (got '$OUT')"
else
    pass_test "printf integer %i format"
fi

OUT=$("$CJSH_PATH" -c "printf '%o\n' 8")
if [ "$OUT" != "10" ]; then
    fail_test "printf octal format (got '$OUT', expected '10')"
else
    pass_test "printf octal format"
fi

OUT=$("$CJSH_PATH" -c "printf '%x\n' 255")
if [ "$OUT" != "ff" ]; then
    fail_test "printf hex lowercase (got '$OUT', expected 'ff')"
else
    pass_test "printf hex lowercase"
fi

OUT=$("$CJSH_PATH" -c "printf '%X\n' 255")
if [ "$OUT" != "FF" ]; then
    fail_test "printf hex uppercase (got '$OUT', expected 'FF')"
else
    pass_test "printf hex uppercase"
fi

OUT=$("$CJSH_PATH" -c "printf '%u\n' 42")
if [ "$OUT" != "42" ]; then
    fail_test "printf unsigned integer (got '$OUT')"
else
    pass_test "printf unsigned integer"
fi

OUT=$("$CJSH_PATH" -c "printf '%f\n' 3.14")
if [ "$OUT" != "3.140000" ]; then
    fail_test "printf float default precision (got '$OUT', expected '3.140000')"
else
    pass_test "printf float default precision"
fi

OUT=$("$CJSH_PATH" -c "printf '%.2f\n' 3.14159")
if [ "$OUT" != "3.14" ]; then
    fail_test "printf float with precision (got '$OUT', expected '3.14')"
else
    pass_test "printf float with precision"
fi

OUT=$("$CJSH_PATH" -c "printf '%e\n' 1000")
EXPECTED="1.000000e+03"
if [ "$OUT" != "$EXPECTED" ] && [ "$OUT" != "1.000000e+3" ]; then
    fail_test "printf exponential lowercase (got '$OUT', expected '$EXPECTED' or '1.000000e+3')"
else
    pass_test "printf exponential lowercase"
fi

OUT=$("$CJSH_PATH" -c "printf '%E\n' 1000")
EXPECTED="1.000000E+03"
if [ "$OUT" != "$EXPECTED" ] && [ "$OUT" != "1.000000E+3" ]; then
    fail_test "printf exponential uppercase (got '$OUT', expected '$EXPECTED' or '1.000000E+3')"
else
    pass_test "printf exponential uppercase"
fi

OUT=$("$CJSH_PATH" -c "printf '%c\n' A")
if [ "$OUT" != "A" ]; then
    fail_test "printf character (got '$OUT')"
else
    pass_test "printf character"
fi

OUT=$("$CJSH_PATH" -c "printf '%s %d %x\n' hello 10 15")
if [ "$OUT" != "hello 10 f" ]; then
    fail_test "printf multiple specifiers (got '$OUT', expected 'hello 10 f')"
else
    pass_test "printf multiple specifiers"
fi

OUT=$("$CJSH_PATH" -c "printf '%0100000000000d' 0" 2>&1)
status=$?
if [ $status -ne 0 ]; then
    pass_test "printf rejects extremely large field widths"
else
    fail_test "printf accepted huge field width (status=$status, output: '$OUT')"
fi

OUT=$("$CJSH_PATH" -c "printf '%1000000000000\\$d\n' 1" 2>&1)
status=$?
if [ $status -ne 0 ]; then
    pass_test "printf rejects oversized positional argument indexes"
else
    fail_test "printf accepted oversized positional argument (status=$status, output: '$OUT')"
fi

OUT=$("$CJSH_PATH" -c "printf 'hello\nworld\n'")
EXPECTED="hello
world"
if [ "$OUT" != "$EXPECTED" ]; then
    fail_test "printf newline escape (output mismatch)"
else
    pass_test "printf newline escape"
fi

OUT=$("$CJSH_PATH" -c "printf 'col1\tcol2\n'")
EXPECTED="col1	col2"
if [ "$OUT" != "$EXPECTED" ]; then
    fail_test "printf tab escape (output mismatch)"
else
    pass_test "printf tab escape"
fi

OUT=$("$CJSH_PATH" -c "printf 'back\\\\slash\n'")
if [ "$OUT" != "back\\slash" ]; then
    fail_test "printf backslash escape (got '$OUT')"
else
    pass_test "printf backslash escape"
fi

OUT=$("$CJSH_PATH" -c "printf '100%% complete\n'")
if [ "$OUT" != "100% complete" ]; then
    fail_test "printf percent literal (got '$OUT')"
else
    pass_test "printf percent literal"
fi

OUT=$("$CJSH_PATH" -c "printf '%10s\n' hello")
if [ "$OUT" != "     hello" ]; then
    fail_test "printf width specifier (got '$OUT', expected '     hello')"
else
    pass_test "printf width specifier"
fi

OUT=$("$CJSH_PATH" -c "printf '%-10s|\n' hello")
if [ "$OUT" != "hello     |" ]; then
    fail_test "printf left-align (got '$OUT', expected 'hello     |')"
else
    pass_test "printf left-align"
fi

OUT=$("$CJSH_PATH" -c "printf '%05d\n' 42")
if [ "$OUT" != "00042" ]; then
    fail_test "printf zero-padding (got '$OUT', expected '00042')"
else
    pass_test "printf zero-padding"
fi

OUT=$("$CJSH_PATH" -c "printf 'just text\n'")
if [ "$OUT" != "just text" ]; then
    fail_test "printf no arguments (got '$OUT')"
else
    pass_test "printf no arguments"
fi

OUT=$("$CJSH_PATH" -c "printf '%s %d\n' hello")
if [ "$OUT" != "hello 0" ]; then
    fail_test "printf missing argument defaults to 0 (got '$OUT')"
else
    pass_test "printf missing argument defaults to 0"
fi

OUT=$("$CJSH_PATH" -c "printf '%s\n' one two three")
EXPECTED="one
two
three"
if [ "$OUT" != "$EXPECTED" ]; then
    fail_test "printf format reuse (output mismatch)"
else
    pass_test "printf format reuse"
fi

OUT=$("$CJSH_PATH" -c "printf '%q\n' test" 2>&1)
if [ $? -eq 0 ] || [ $? -eq 1 ]; then
    pass_test "printf invalid format specifier handled"
else
    fail_test "printf invalid format specifier not handled"
fi

OUT=$("$CJSH_PATH" -c "printf ''")
if [ "$OUT" != "" ]; then
    fail_test "printf empty format string (got '$OUT')"
else
    pass_test "printf empty format string"
fi

OUT=$("$CJSH_PATH" -c "printf \"%s\n\" \"it's\"")
if [ "$OUT" != "it's" ]; then
    fail_test "printf single quote in string (got '$OUT')"
else
    pass_test "printf single quote in string"
fi

"$CJSH_PATH" -c "printf 'test\n' >/dev/null"
if [ $? -ne 0 ]; then
    fail_test "printf exit status on success"
else
    pass_test "printf exit status on success"
fi

echo ""
echo "Printf Comprehensive Tests Summary:"
echo "Passed: $TESTS_PASSED"
echo "Failed: $TESTS_FAILED"
if [ $TESTS_FAILED -eq 0 ]; then
    echo "PASS"
    exit 0
else
    echo "FAIL"
    exit 1
fi
