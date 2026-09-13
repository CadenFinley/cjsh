#!/usr/bin/env sh

# test_language_compatibility.sh
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

echo "Test: language compatibility extensions..."

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

run_expect_output() {
    desc=$1
    command=$2
    expected=$3

    output=$("$CJSH_PATH" -c "$command" 2>/dev/null)
    status=$?
    if [ "$status" -eq 0 ] && [ "$output" = "$expected" ]; then
        pass_test "$desc"
    else
        fail_test "$desc (status=$status, output='$output', expected='$expected')"
    fi
}

run_expect_output "brace ranges support numeric strides" \
    'echo {1..10..2}; echo {10..1..3}; echo {01..10..3}' \
    '1 3 5 7 9
10 7 4 1
01 04 07 10'

run_expect_output "brace ranges support character strides" \
    'echo {a..g..2}; echo {G..A..2}' \
    'a c e g
G E C A'

run_expect_output "wildcard parameter replacement uses leftmost-longest matches" \
    'value=abcabc; printf "%s|%s|%s" "${value/?b/X}" "${value//?b/X}" "${value/a*c/X}"' \
    'Xcabc|XcXc|X'

run_expect_output "parameter replacement supports anchored patterns" \
    'value=abcabc; printf "%s|%s|%s" "${value/#a*/X}" "${value/%*c/X}" "${value//#a*/X}"' \
    'X|X|abcabc'

run_expect_output "parameter replacement handles literal pipes and POSIX character classes" \
    'value="a|b"; printf "%s|%s" "${value/a|b/X}" "${value//[[:alpha:]]/X}"' \
    'X|X|X'

run_expect_output "case ;& falls through to the next clause body" \
    'case x in x) echo first ;& y) echo second ;; z) echo third ;; esac' \
    'first
second'

run_expect_output "case ;;& resumes pattern testing" \
    'case x in x) echo first ;;& y) echo skipped ;; x) echo second ;; esac' \
    'first
second'

run_expect_output "associative arrays support keys, values, and cardinality" \
    'declare -A colors=([red]=ff0000 ["light blue"]="00 aaff"); colors[green]=00ff00; printf "%s|%s|%s" "${colors[red]}" "${colors[light blue]}" "${#colors[@]}"' \
    'ff0000|00 aaff|3'

run_expect_output "declare -p emits reusable associative declarations" \
    'declare -A data=([key]="two words"); saved=$(declare -p data); unset data; eval "$saved"; printf "%s" "${data[key]}"' \
    'two words'

run_expect_output "namerefs read, write, and expose their target" \
    'target=before; declare -n ref=target; ref=after; printf "%s|%s|%s" "$ref" "$target" "${!ref}"' \
    'after|after|target'

run_expect_output "unset -n removes a nameref without removing its target" \
    'target=value; declare -n ref=target; unset -n ref; printf "%s|%s" "${ref-unset}" "$target"' \
    'unset|value'

run_expect_output "cjshopt extglob enables extended pattern operators" \
    'cjshopt extglob on >/dev/null; [[ foo == @(foo|bar) ]] && [[ foobar == +(foo|bar) ]] && [[ baz == !(foo|bar) ]]; printf "%s" "$?"' \
    '0'

run_expect_output "extglob works in case patterns" \
    'cjshopt extglob on >/dev/null; case foobar in +(foo|bar)) echo matched ;; *) echo missed ;; esac' \
    'matched'

run_expect_output "extglob works in parameter replacement" \
    'cjshopt extglob on >/dev/null; value=foobar; printf "%s|%s" "${value/@(foo|bar)/X}" "${value//+(foo|bar)/X}"' \
    'Xbar|X'

run_expect_output "overlapping and empty extglob alternatives retain suffix matching" \
    'cjshopt extglob on >/dev/null; for value in b aab aaab aaac; do case "$value" in *(a|aa|)b) printf "yes " ;; *) printf "no " ;; esac; done' \
    'yes yes yes no '

run_expect_output "extglob frontiers preserve leftmost-longest replacement and trimming" \
    'cjshopt extglob on >/dev/null; value=aaabaa; printf "%s|%s|%s|%s" "${value//+(a|aa)/X}" "${value##*(a|aa)}" "${value%%+(a|aa)}" "${value/@(*a*a)b/X}"' \
    'XbX|baa|aaab|Xaa'

EXTGLOB_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cjsh-extglob.XXXXXX")
touch "$EXTGLOB_DIR/one.txt" "$EXTGLOB_DIR/two.txt" "$EXTGLOB_DIR/three.log" \
    "$EXTGLOB_DIR/.hidden.txt"
run_expect_output "extglob expands pathnames without implicitly matching dotfiles" \
    "cd '$EXTGLOB_DIR'; cjshopt extglob on >/dev/null; echo @(*.txt)" \
    'one.txt two.txt'
rm -rf "$EXTGLOB_DIR"

run_expect_output "coproc provides two-way descriptors and a waitable pid" \
    'coproc { read line; echo "$line"; }; echo ping >&${COPROC[1]}; read -u ${COPROC[0]} reply; wait $COPROC_PID; printf "%s|%s" "$reply" "$?"' \
    'ping|0'

run_expect_output "named compound coprocesses publish named descriptors" \
    'coproc WORKER { while read line; do echo reply:$line; break; done; }; echo ping >&${WORKER[1]}; read -u ${WORKER[0]} reply; wait $WORKER_PID; printf "%s|%s" "$reply" "$?"' \
    'reply:ping|0'

echo ""
echo "Language Compatibility Tests Summary:"
echo "Passed: $TESTS_PASSED"
echo "Failed: $TESTS_FAILED"
if [ "$TESTS_FAILED" -eq 0 ]; then
    echo "PASS"
    exit 0
fi

echo "FAIL"
exit 1
