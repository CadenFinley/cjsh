#!/usr/bin/env sh

# test_globbing.sh
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

if [ -n "$CJSH" ]; then CJSH_PATH="$CJSH"; else CJSH_PATH="$(cd "$(dirname "$0")/../../build" && pwd)/cjsh"; fi
echo "Test: globbing..."

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

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT INT TERM
touch "$TMPDIR/a.txt" "$TMPDIR/ab.txt" "$TMPDIR/b.txt"

PROBE=$("$CJSH_PATH" -c "cd '$TMPDIR'; echo *.txt")
case "$(printf %s "$PROBE" | tr -d "'\"")" in
  *\**)
    fail_test "globbing not supported by cjsh"
    exit 1
    ;;
esac
pass_test "globbing support detected"

OUT=$("$CJSH_PATH" -c "cd '$TMPDIR'; printf '%s ' *.txt | sed 's/ *$//' | tr -d '\n'")
OUT_BASE=$(echo "$OUT" | xargs -n1 basename | tr '\n' ' ' | sed 's/ *$//')

EXPECTED="a.txt ab.txt b.txt"
if [ "$OUT_BASE" != "$EXPECTED" ] && [ "$OUT_BASE" != "a.txt b.txt ab.txt" ]; then
  fail_test "globbing result '$OUT_BASE'"
  exit 1
else
  pass_test "globbing expansion"
fi

echo ""
echo "Testing globstar expansion..."
GLOB_TMP=$(mktemp -d)
mkdir -p "$GLOB_TMP/root/a/b"
touch "$GLOB_TMP/root/a/b/deep.txt"
touch "$GLOB_TMP/root/root.txt"

OUT=$("$CJSH_PATH" -c "cd '$GLOB_TMP/root'; printf '%s' **/deep.txt" 2>/dev/null)
if [ "$OUT" = "**/deep.txt" ]; then
  pass_test "globstar disabled leaves pattern literal"
else
  fail_test "expected literal pattern when globstar disabled, got '$OUT'"
  rm -rf "$GLOB_TMP"
  exit 1
fi

OUT=$("$CJSH_PATH" -c "cd '$GLOB_TMP/root'; shopt -s globstar; printf '%s' **/deep.txt" 2>/dev/null)
if [ "$OUT" = "a/b/deep.txt" ]; then
  pass_test "globstar matches nested files"
else
  fail_test "globstar lookup mismatch (got '$OUT')"
  rm -rf "$GLOB_TMP"
  exit 1
fi

OUT=$("$CJSH_PATH" -c "cd '$GLOB_TMP/root'; shopt -s globstar; printf '%s' **/root.txt" 2>/dev/null)
if [ "$OUT" = "root.txt" ]; then
  pass_test "globstar zero-depth match"
else
  fail_test "globstar zero-depth match failed (got '$OUT')"
  rm -rf "$GLOB_TMP"
  exit 1
fi

DIRS=$("$CJSH_PATH" -c "cd '$GLOB_TMP/root'; shopt -s globstar; printf '%s ' **/" 2>/dev/null)
if printf '%s' "$DIRS" | tr ' ' '\n' | grep -q "a/b/"; then
  pass_test "globstar directory-only expansion"
else
  fail_test "globstar directory-only expansion missing nested path"
  rm -rf "$GLOB_TMP"
  exit 1
fi

rm -rf "$GLOB_TMP"

echo ""
echo "Globbing Tests Summary:"
echo "Passed: $TESTS_PASSED"
echo "Failed: $TESTS_FAILED"
if [ $TESTS_FAILED -eq 0 ]; then
    echo "PASS"
    exit 0
else
    echo "FAIL"
    exit 1
fi
