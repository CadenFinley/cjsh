#!/usr/bin/env sh

# test_quote_parser_regression.sh
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

echo "Test: quote parser regression cases..."

MULTILINE_SCRIPT=$(printf '%s\n%s' 'echo "Say \"hello\" now"' 'echo done')
MULTILINE_OUTPUT=$("$CJSH_PATH" -c "$MULTILINE_SCRIPT" 2>&1)
EXPECTED_MULTILINE_OUTPUT=$(printf 'Say "hello" now\ndone')
if [ "$MULTILINE_OUTPUT" != "$EXPECTED_MULTILINE_OUTPUT" ]; then
  echo "FAIL: escaped quotes before newline"
  echo "      got: $MULTILINE_OUTPUT"
  exit 1
else
  echo "PASS: escaped quotes before newline"
fi

NESTED_SUBST_OUTPUT=$("$CJSH_PATH" -c 'value="$(command printf "%s" "$(command printf "%s" "deep")")"; printf "%s" "$value"' 2>&1)
if [ "$NESTED_SUBST_OUTPUT" != "deep" ]; then
  echo "FAIL: nested substitutions in double quotes"
  echo "      got: $NESTED_SUBST_OUTPUT"
  exit 1
else
  echo "PASS: nested substitutions in double quotes"
fi

PWD_VALUE=$(pwd)
NESTED_ARGS_OUTPUT=$("$CJSH_PATH" -c 'f() { out="$(command printf "%s|%s" "$(pwd)" "$@")"; printf "%s" "$out"; }; f "arg with space"' 2>&1)
EXPECTED_NESTED_ARGS_OUTPUT=$(printf '%s|%s' "$PWD_VALUE" 'arg with space')
if [ "$NESTED_ARGS_OUTPUT" != "$EXPECTED_NESTED_ARGS_OUTPUT" ]; then
  echo "FAIL: nested substitution with quoted positional args"
  echo "      got: $NESTED_ARGS_OUTPUT"
  exit 1
else
  echo "PASS: nested substitution with quoted positional args"
fi

TMP_DIR=$(mktemp -d)
TMP_SCRIPT="$TMP_DIR/source.sh"
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM
printf '%s\n' 'z() { __CJSH_ZOXIDE_RESULT="$(command printf "%s|%s" "$(pwd)" "$@")"; printf "%s" "$__CJSH_ZOXIDE_RESULT"; }' 'z "ok arg"' >"$TMP_SCRIPT"
SOURCE_OUTPUT=$("$CJSH_PATH" -c ". \"$TMP_SCRIPT\"" 2>&1)
EXPECTED_SOURCE_OUTPUT=$(printf '%s|%s' "$PWD_VALUE" 'ok arg')
if [ "$SOURCE_OUTPUT" != "$EXPECTED_SOURCE_OUTPUT" ]; then
  echo "FAIL: sourced zoxide-style assignment"
  echo "      got: $SOURCE_OUTPUT"
  exit 1
else
  echo "PASS: sourced zoxide-style assignment"
fi

check_escaped_words() {
  label=$1
  script=$2
  expected=$3
  output=$("$CJSH_PATH" --no-config -c "$script" 2>&1)
  status=$?
  if [ "$status" -ne 0 ] || [ "$output" != "$expected" ]; then
    echo "FAIL: $label (status $status)"
    printf '      expected: %s\n      got: %s\n' "$expected" "$output"
    exit 1
  fi
  echo "PASS: $label"
}

check_escaped_words "escaped spaces preserve argument boundaries" \
  'printf "<%s>\n" one\ two three\ \ four \ five six\ seven' \
  "$(printf '<one two>\n<three  four>\n< five>\n<six seven>')"
check_escaped_words "escaped tabs preserve argument boundaries" \
  "$(printf 'printf "<%%s>\\n" one\\\ttwo')" "$(printf '<one\ttwo>')"
check_escaped_words "escaped spaces in pipelines" \
  'printf "<%s>\n" one\ two | cat' '<one two>'
check_escaped_words "unquoted expansions still split beside escaped spaces" \
  'value="one two"; printf "<%s>\n" prefix\ $value\ suffix' \
  "$(printf '<prefix one>\n<two suffix>')"
check_escaped_words "command substitution still splits beside escaped spaces" \
  'printf "<%s>\n" prefix\ $(printf "one two")\ suffix' \
  "$(printf '<prefix one>\n<two suffix>')"
check_escaped_words "escaped spaces beside quoted text" \
  'printf "<%s>\n" one\ "two" one\ '\''two'\''' \
  "$(printf '<one two>\n<one two>')"
check_escaped_words "escaped spaces in assignments" \
  'value=one\ two; export other=three\ four; printf "<%s>\n" "$value" "$other"' \
  "$(printf '<one two>\n<three four>')"
check_escaped_words "quoted substitution preserves literal control bytes" \
  'printf "%s" "$(printf '\''\037x'\'')"' "$(printf '\037x')"

cat >"$TMP_DIR/Start VM.command" <<'EOF'
#!/bin/sh
printf '<%s>\n' "$@"
EOF
chmod +x "$TMP_DIR/Start VM.command"
check_escaped_words "escaped command path followed by exit" \
  "$TMP_DIR/Start\\ VM.command one\\ two ; exit;" '<one two>'
check_escaped_words "escaped command path in a pipeline" \
  "$TMP_DIR/Start\\ VM.command one\\ two | cat" '<one two>'
check_escaped_words "escaped command path with redirections" \
  "$TMP_DIR/Start\\ VM.command one\\ two > $TMP_DIR/output\\ file; cat < $TMP_DIR/output\\ file" \
  '<one two>'
check_escaped_words "escaped spaces in descriptor redirections" \
  "printf '<%s>\\n' one\\ two 3> $TMP_DIR/fd\\ file >&3; cat < $TMP_DIR/fd\\ file" \
  '<one two>'

touch "$TMP_DIR/match one.txt" "$TMP_DIR/match two.txt"
check_escaped_words "wildcards still expand beside escaped spaces" \
  "printf '<%s>\\n' $TMP_DIR/match\\ *.txt" \
  "$(printf '<%s>\n' "$TMP_DIR/match one.txt" "$TMP_DIR/match two.txt")"

echo "PASS"
exit 0
