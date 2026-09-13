#!/bin/sh

# test_extglob_endpoint_regressions.sh
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

# Behavioral checks only: performance evidence lives in the external audit.
CJSH=${1:-./build/release/cjsh}
passed=0
failed=0
check() {
    name=$1 expected=$2 script=$3
    actual=$("$CJSH" --no-config --no-system-paths -c "cjshopt extglob on > /dev/null
$script")
    status=$?
    if [ "$status" -eq 0 ] && [ "$actual" = "$expected" ]; then
        passed=$((passed + 1))
    else
        printf 'FAIL: %s (status %s)\nexpected: <%s>\nactual: <%s>\n' "$name" "$status" "$expected" "$actual"
        failed=$((failed + 1))
    fi
}
check 'global group replacement' 'XXXXXX' 'v=abbaab; printf "%s" "${v//@(a|b)/X}"'
check 'unsuccessful group replacement' 'aaaa' 'v=aaaa; printf "%s" "${v//@(b|c)/X}"'
check 'leftmost longest replacement' 'Xa' 'v=aba; printf "%s" "${v/@(a|ab)/X}"'
check 'global deletion' 'cc' 'v=cabbac; printf "%s" "${v//@(a|b)/}"'
check 'shortest and longest prefix' 'ba|a' 'v=aba; printf "%s|%s" "${v#@(a|ab)}" "${v##@(a|ab)}"'
check 'shortest and longest suffix' 'ab|a' 'v=aba; printf "%s|%s" "${v%@(a|ba)}" "${v%%@(a|ba)}"'
check 'continuation after group' 'X' 'v=ab; printf "%s" "${v/@(a|ab)b/X}"'
check 'nested optional groups' 'XXX' 'v=aba; printf "%s" "${v//@(a|?(b))/X}"'
check 'empty optional match advances' 'XbXbX' 'v=bb; printf "%s" "${v//?(a)/X}"'
check 'anchored replacement' 'Xc|cX' 'v=abc; w=cab; printf "%s|%s" "${v/#@(a|ab)/X}" "${w/%@(b|ab)/X}"'
check 'repetition fallback' 'Xc' 'v=abbac; printf "%s" "${v/+(a|b)/X}"'
check 'disabled extended globs stay literal' 'a|X' 'cjshopt extglob off > /dev/null; v=a; w="@(a|b)"; printf "%s|%s" "${v//@(a|b)/X}" "${w//@(a|b)/X}"'
printf 'Tests passed: %s\nTests failed: %s\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
