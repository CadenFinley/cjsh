#!/usr/bin/env sh

# test_gibberish_error_handling.sh
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
. "$(dirname "$0")/process_cleanup_helpers.sh"
echo "Test: gibberish script error handling..."

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

TEST_TMP_DIR="/tmp/cjsh_gibberish_test_$$"
mkdir -p "$TEST_TMP_DIR"

cleanup() {
    rm -rf "$TEST_TMP_DIR"
}
trap cleanup EXIT

cat > "$TEST_TMP_DIR/gibberish1.sh" << 'EOF'
#!/bin/sh
nonexistent_command_xyz --with --random --flags
exit 1  # Force script to fail after error
EOF

cat > "$TEST_TMP_DIR/gibberish2.sh" << 'EOF'
#!/bin/sh
echo "This line is valid"
if [ $nonexistent_var -eq ]; then
    echo "incomplete condition"
fi
cd /this/path/does/not/exist/anywhere
ls -z --invalid-flag
echo "another valid line" | invalid_command --with --flags
for i in 1 2 3; do
    echo $((i + undefined_var))
    break invalid  # nonnumeric break level
done
EOF

cat > "$TEST_TMP_DIR/gibberish3.sh" << 'EOF'
#!/bin/sh
@#$%^&*()_+{}|:"<>?
if then else fi while do done
echo echo echo echo echo
$$$$$$$$$$$$$$$$$$$$
[[[[[[]]]]]]
for for for in in in do do do
function function() function
random_gibberish_text_that_looks_like_code
EOF

cat > "$TEST_TMP_DIR/gibberish4.sh" << 'EOF'
#!/bin/sh
This file contains simulated binary content:
ÿþÿþÿþÿþ garbage binary data ÿþÿþÿþÿþ
More random bytes: àáâãäåæçèéêëìíîïðñòó
And some mixed content: echo "test" ÿþÿþ exit 1
EOF

OUTPUT=$("$CJSH_PATH" "$TEST_TMP_DIR/gibberish1.sh" 2>&1)
EXIT_CODE=$?
if [ "$EXIT_CODE" -gt 0 ] && [ "$EXIT_CODE" -lt 128 ]; then
    pass_test "gibberish script 1 properly failed with exit code $EXIT_CODE"
else
    fail_test "gibberish script 1 expected an error without crashing, got $EXIT_CODE"
fi

"$CJSH_PATH" "$TEST_TMP_DIR/gibberish2.sh" 2>/dev/null
EXIT_CODE=$?
if [ "$EXIT_CODE" -gt 0 ] && [ "$EXIT_CODE" -lt 128 ]; then
    pass_test "gibberish script 2 properly failed with exit code $EXIT_CODE"
else
    fail_test "gibberish script 2 expected an error without crashing, got $EXIT_CODE"
fi

"$CJSH_PATH" "$TEST_TMP_DIR/gibberish3.sh" 2>/dev/null
EXIT_CODE=$?
if [ "$EXIT_CODE" -gt 0 ] && [ "$EXIT_CODE" -lt 128 ]; then
    pass_test "gibberish script 3 properly failed with exit code $EXIT_CODE"
else
    fail_test "gibberish script 3 expected an error without crashing, got $EXIT_CODE"
fi

"$CJSH_PATH" "$TEST_TMP_DIR/gibberish4.sh" 2>/dev/null
EXIT_CODE=$?
if [ "$EXIT_CODE" -gt 0 ] && [ "$EXIT_CODE" -lt 128 ]; then
    pass_test "gibberish script 4 properly failed with exit code $EXIT_CODE"
else
    fail_test "gibberish script 4 expected an error without crashing, got $EXIT_CODE"
fi

if echo "$OUTPUT" | grep -E "(ERROR|error|command not found|Suggestion)" > /dev/null; then
    pass_test "cjsh produced error output for gibberish script"
else
    fail_test "cjsh produced no recognizable error output for gibberish script (got: '$(echo "$OUTPUT" | tr '\n' ' ')') "
fi

"$CJSH_PATH" "$TEST_TMP_DIR/nonexistent_file.sh" 2>/dev/null
EXIT_CODE=$?
if [ "$EXIT_CODE" -gt 0 ] && [ "$EXIT_CODE" -lt 128 ]; then
    pass_test "nonexistent file properly failed with exit code $EXIT_CODE"
else
    fail_test "nonexistent file expected an error without crashing, got $EXIT_CODE"
fi

# macOS has no GNU timeout. Verify a ready loop exits on SIGTERM using the
# portable helper, which checks the exact status and bounds the shutdown wait.
if cleanup_output=$(check_process_cleanup "$CJSH_PATH" TERM 0 loop); then
    pass_test "cjsh infinite loop exits on SIGTERM"
else
    fail_test "cjsh infinite loop did not exit cleanly on SIGTERM: $cleanup_output"
fi

touch "$TEST_TMP_DIR/empty.sh"
chmod +x "$TEST_TMP_DIR/empty.sh"
"$CJSH_PATH" "$TEST_TMP_DIR/empty.sh"
EXIT_CODE=$?
if [ $EXIT_CODE -eq 0 ]; then
    pass_test "empty script executed successfully"
else
    fail_test "empty script expected status 0, got $EXIT_CODE"
fi

cat > "$TEST_TMP_DIR/comments_only.sh" << 'EOF'
#!/bin/sh



   
EOF
"$CJSH_PATH" "$TEST_TMP_DIR/comments_only.sh"
EXIT_CODE=$?
if [ $EXIT_CODE -eq 0 ]; then
    pass_test "comments-only script executed successfully"
else
    fail_test "comments-only script expected status 0, got $EXIT_CODE"
fi

echo ""
echo "Gibberish Error Handling Tests Summary:"
echo "Passed: $TESTS_PASSED"
echo "Failed: $TESTS_FAILED"
if [ $TESTS_FAILED -eq 0 ]; then
    echo "PASS"
    exit 0
else
    echo "FAIL"
    exit 1
fi
