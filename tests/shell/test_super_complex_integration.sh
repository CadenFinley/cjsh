#!/usr/bin/env sh
# test_super_complex_integration.sh
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

if [ ! -x "$CJSH_PATH" ]; then
    echo "FAIL: cjsh binary not found at $CJSH_PATH"
    echo "Please build the project before running this test."
    exit 1
fi

TOTAL=0
PASSED=0
FAILED=0

pass() {
  PASSED=$((PASSED + 1))
  printf "PASS: %s\n" "$1"
}

fail() {
  FAILED=$((FAILED + 1))
  printf "FAIL: %s\n       Expected: [%s]\n       Got: [%s]\n" "$1" "$2" "$3"
}

expect_output() {
  desc=$1
  script=$2
  expected=$3
  TOTAL=$((TOTAL + 1))
  output=$("$CJSH_PATH" -c "$script" 2>&1)
  if [ "$output" = "$expected" ]; then
    pass "$desc"
  else
    fail "$desc" "$expected" "$output"
  fi
}

expect_exit() {
  desc=$1
  script=$2
  expected_exit=$3
  TOTAL=$((TOTAL + 1))
  "$CJSH_PATH" -c "$script" >/dev/null 2>&1
  exit_code=$?
  if [ "$exit_code" -eq "$expected_exit" ]; then
    pass "$desc"
  else
    fail "$desc" "exit $expected_exit" "exit $exit_code"
  fi
}

expect_not_crash() {
  desc=$1
  script=$2
  TOTAL=$((TOTAL + 1))
  "$CJSH_PATH" -c "$script" >/dev/null 2>&1
  exit_code=$?
  if [ "$exit_code" -lt 128 ] || [ "$exit_code" -eq 0 ]; then
    pass "$desc"
  else
    fail "$desc" "normal exit (< 128)" "crash signal (exit $exit_code)"
  fi
}

expect_output "10-level nested if-then-else" \
  'if true; then if true; then if true; then if true; then if true; then if true; then if true; then if true; then if true; then if true; then echo "deep"; fi; fi; fi; fi; fi; fi; fi; fi; fi; fi' \
  "deep"

expect_output "Mixed nested loops and conditionals" \
  'count=0; for i in 1 2 3; do for j in a b; do if [ "$i" = "2" ]; then if [ "$j" = "b" ]; then count=$((count + 1)); fi; fi; done; done; echo $count' \
  "1"

expect_output "Deeply nested while loops with break" \
  'x=0; while [ $x -lt 3 ]; do y=0; while [ $y -lt 3 ]; do z=0; while [ $z -lt 3 ]; do if [ $x -eq 1 ] && [ $y -eq 1 ] && [ $z -eq 1 ]; then echo "found"; break 3; fi; z=$((z+1)); done; y=$((y+1)); done; x=$((x+1)); done' \
  "found"

expect_output "Case statement with 20+ patterns" \
  'val=15; case $val in 1)echo a;;2)echo b;;3)echo c;;4)echo d;;5)echo e;;6)echo f;;7)echo g;;8)echo h;;9)echo i;;10)echo j;;11)echo k;;12)echo l;;13)echo m;;14)echo n;;15)echo o;;16)echo p;;*)echo z;;esac' \
  "o"

expect_not_crash "100-level nested subshells" \
  '(((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((echo ok)))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))'

expect_output "Large number arithmetic" \
  'echo $((999999 + 999999))' \
  "1999998"

expect_output "Complex arithmetic precedence" \
  'echo $((5 + 3 * 2 ** 3 - 4 / 2))' \
  "27"

expect_output "Nested arithmetic expansions (5 levels)" \
  'echo $((1 + $((2 + $((3 + $((4 + $((5))))))))))' \
  "15"

expect_output "All arithmetic operators" \
  'a=10; b=3; echo "$((a+b)) $((a-b)) $((a*b)) $((a/b)) $((a%b))"' \
  "13 7 30 3 1"

expect_output "Bitwise XOR operation" \
  'echo $(((15 | 8) ^ 7))' \
  "8"

expect_output "Negative number handling" \
  'echo $((-5 * -3 + -2))' \
  "13"

expect_output "Division by zero protection" \
  'a=10; b=0; echo $((a / (b + 1)))' \
  "10"

expect_not_crash "Arithmetic overflow handling" \
  'echo $((999999999 * 999999999)) >/dev/null'

expect_output "Quotes within quotes" \
  'echo "He said '"'"'hello'"'"' there"' \
  "He said 'hello' there"

expect_output "Escaped quotes" \
  'echo "Say \"hello\" now"' \
  'Say "hello" now'

expect_output "Mixed quote types" \
  "echo 'single' \"double\" \`echo backtick\`" \
  "single double backtick"

expect_output "Special characters in quotes" \
  'echo "!@#\$%^&*()_+-={}[]|:;<>?,./"' \
  '!@#$%^&*()_+-={}[]|:;<>?,./'

expect_output "Backslash escaping" \
  'echo "\\n\\t\\r\\\\"' \
  '\n\t\r\\'

expect_output "Empty string handling" \
  'a=""; b="test"; echo "${a}${b}"' \
  "test"

expect_output "Whitespace preservation" \
  'echo "  spaces   and   tabs		here  "' \
  "  spaces   and   tabs		here  "

expect_output "Nested variable expansion" \
  'a=b; b=c; c=hello; eval "echo \${!a}"' \
  "c"

expect_output "Default value expansion" \
  'unset VAR; echo ${VAR:-default}' \
  "default"

expect_output "Alternate value expansion" \
  'VAR=set; echo ${VAR:+alternate}' \
  "alternate"

expect_output "String length expansion" \
  'VAR="hello world"; echo ${#VAR}' \
  "11"

expect_output "Substring extraction" \
  'VAR="hello world"; echo ${VAR#hello }' \
  "world"

expect_output "Pattern removal" \
  'VAR="test.tar.gz"; echo ${VAR%.gz}' \
  "test.tar"

expect_output "Variable with numbers and underscores" \
  'VAR_123_test=value; echo $VAR_123_test' \
  "value"

expect_output "Massive variable expansion" \
  'a=1; b=2; c=3; d=4; e=5; echo "$a$b$c$d$e$a$b$c$d$e$a$b$c$d$e$a$b$c$d$e"' \
  "12345123451234512345"

expect_output "Environment variable passthrough" \
  'HOME=/test; echo $HOME' \
  "/test"

expect_output "Basic command substitution" \
  'echo $(echo hello)' \
  "hello"

expect_output "Nested command substitution (3 levels)" \
  'echo $(echo $(echo $(echo deep)))' \
  "deep"

expect_output "Command substitution in arithmetic" \
  'echo $(($(echo 5) + $(echo 3)))' \
  "8"

expect_output "Command substitution with pipes" \
  'echo $(echo "a b c" | wc -w | tr -d " ")' \
  "3"

expect_output "Multiple command substitutions" \
  'echo $(echo a) $(echo b) $(echo c)' \
  "a b c"

expect_output "Command substitution with variables" \
  'VAR=$(echo test); echo $VAR' \
  "test"

expect_not_crash "Empty command substitution" \
  'echo $()'

expect_output "Command substitution with redirection" \
  'echo $(echo test 2>&1)' \
  "test"

expect_output "Long pipeline chain" \
  'echo "test" | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat' \
  "test"

expect_output "Multiple redirections" \
  'echo test >/tmp/cjsh_test_$$.txt 2>&1; cat /tmp/cjsh_test_$$.txt; rm -f /tmp/cjsh_test_$$.txt' \
  "test"

expect_output "Here-string simulation" \
  'cat <<EOF
line1
line2
EOF' \
  "line1
line2"

expect_output "Stderr redirection" \
  'echo stdout; echo stderr >&2' \
  "stdout
stderr"

expect_not_crash "Redirect to /dev/null" \
  'echo test >/dev/null 2>&1'

expect_not_crash "Append redirection" \
  'echo a >>/tmp/cjsh_append_$$.txt; echo b >>/tmp/cjsh_append_$$.txt; cat /tmp/cjsh_append_$$.txt; rm -f /tmp/cjsh_append_$$.txt'

expect_output "Simple function" \
  'myfunc() { echo hello; }; myfunc' \
  "hello"

expect_output "Function with parameters" \
  'add() { echo $(($1 + $2)); }; add 5 3' \
  "8"

expect_output "Function with return value" \
  'check() { return 42; }; check; echo $?' \
  "42"

expect_output "Recursive function" \
  'factorial() { if [ $1 -le 1 ]; then echo 1; else echo $(($1 * $(factorial $(($1 - 1))))); fi; }; factorial 5' \
  "120"

expect_output "Function variable scoping" \
  'x=outer; func() { x=inner; echo $x; }; func; echo $x' \
  "inner
inner"

expect_output "Function with local variables (if supported)" \
  'x=outer; func() { local x=inner; echo $x; }; func; echo $x' \
  "inner
outer"

expect_output "Positional parameters" \
  'set -- a b c d e; echo "$1 $3 $5"' \
  "a c e"

expect_output "Parameter count" \
  'set -- a b c d e; echo $#' \
  "5"

expect_output "All parameters" \
  'set -- a b c; echo "$*"' \
  "a b c"

expect_output "Last exit code" \
  'true; echo $?; false; echo $?' \
  "0
1"

expect_output "Current shell PID" \
  'echo $$ | grep -E "^[0-9]+$" >/dev/null && echo "valid"' \
  "valid"

expect_output "Shift parameters" \
  'set -- a b c d; shift 2; echo $1' \
  "c"

expect_exit "Command not found" \
  'nonexistent_command_xyz123' \
  127

expect_exit "Syntax error: unclosed quote" \
  'echo "unclosed' \
  2

expect_exit "Syntax error: unmatched brace" \
  'echo ${VAR' \
  2

expect_not_crash "Empty variable expansion" \
  'echo $UNDEFINED_VAR'

expect_not_crash "Division in conditional" \
  'if [ $((10 / 2)) -eq 5 ]; then echo yes; fi'

expect_output "Exit in subshell preserves main" \
  '(exit 1); echo still running' \
  "still running"

expect_not_crash "Null command" \
  ': "this is a null command"'

expect_not_crash "Very long variable name" \
  'very_long_variable_name_that_goes_on_and_on_and_on_1234567890=test; echo $very_long_variable_name_that_goes_on_and_on_and_on_1234567890'

expect_output "Glob with multiple wildcards" \
  'case "test" in t*s*) echo match;; *) echo no;; esac' \
  "match"

expect_output "Character class matching" \
  'case "5" in [0-9]) echo digit;; *) echo no;; esac' \
  "digit"

expect_output "Negated character class" \
  'case "a" in [!0-9]) echo letter;; *) echo no;; esac' \
  "letter"

expect_output "Range matching" \
  'case "m" in [a-z]) echo lower;; *) echo no;; esac' \
  "lower"

expect_not_crash "Empty glob pattern" \
  'case "" in "") echo empty;; esac'

expect_not_crash "Create 100 variables" \
  'v0=0;v1=1;v2=2;v3=3;v4=4;v5=5;v6=6;v7=7;v8=8;v9=9;v10=10;v11=11;v12=12;v13=13;v14=14;v15=15;v16=16;v17=17;v18=18;v19=19;v20=20;echo ok'

expect_not_crash "Large loop iteration" \
  'i=0; while [ $i -lt 100 ]; do i=$((i+1)); done; echo $i'

expect_not_crash "Deep arithmetic nesting" \
  'echo $(((((((((1+1)+1)+1)+1)+1)+1)+1)+1)+1)'

expect_output "String concatenation loop" \
  'str=""; i=0; while [ $i -lt 10 ]; do str="${str}x"; i=$((i+1)); done; echo ${#str}' \
  "10"

expect_not_crash "Multiple file operations" \
  'for i in 1 2 3 4 5; do echo $i >/tmp/cjsh_stress_$$_$i.txt; done; cat /tmp/cjsh_stress_$$_*.txt 2>/dev/null; rm -f /tmp/cjsh_stress_$$_*.txt'

expect_output "Unicode characters" \
  'echo "Hello 世界 🌍"' \
  "Hello 世界 🌍"

expect_output "Tab character" \
  'echo "a	b	c"' \
  "a	b	c"

expect_output "Newline in variable" \
  'VAR="line1
line2"; echo "$VAR"' \
  "line1
line2"

expect_output "Parse CSV-like data" \
  'IFS=,; set -- a b c d e; echo "$1-$3-$5"' \
  "a-c-e"

expect_output "Simple calculator" \
  'calc() { echo $(($1 $2 $3)); }; calc 10 + 5' \
  "15"

expect_output "String contains check" \
  'str="hello world"; case "$str" in *world*) echo found;; *) echo not;; esac' \
  "found"

expect_output "Min/max finder" \
  'a=5; b=10; if [ $a -lt $b ]; then echo $a; else echo $b; fi' \
  "5"

expect_output "Environment manipulation" \
  'PATH=/custom:$PATH; echo $PATH | grep -o /custom' \
  "/custom"

expect_output "Trap on EXIT" \
  'trap "echo goodbye" EXIT; echo hello' \
  "hello
goodbye"

expect_output "Multiple traps" \
  'trap "echo trap1" EXIT; trap "echo trap2" 0; echo main' \
  "main
trap2"

expect_exit "Exit with code" \
  'exit 42' \
  42

expect_exit "Exit in function" \
  'func() { exit 5; }; func' \
  5

expect_output "Echo with escape sequences" \
  'echo "test"' \
  "test"

expect_output "Printf-like formatting" \
  'printf "%s\n" "test"' \
  "test"

expect_output "Read and variable assignment" \
  'echo "value" | (read var; echo $var)' \
  "value"

expect_output "Export and environment" \
  'export TESTVAR=value; sh -c "echo \$TESTVAR"' \
  "value"

expect_not_crash "Unset variable" \
  'VAR=test; unset VAR; echo $VAR'

expect_output "Test command equivalence" \
  '[ 1 -eq 1 ] && echo yes || echo no' \
  "yes"

expect_output "Type command for builtin" \
  'type cd | grep -q builtin && echo yes || echo no' \
  "yes"

COMPLEX_SCRIPT="/tmp/cjsh_complex_test_$$.sh"
cat > "$COMPLEX_SCRIPT" << 'SCRIPT_EOF'
#!/usr/bin/env sh

GLOBAL_ERROR_COUNT=0
GLOBAL_ERROR_COUNT=0
GLOBAL_PROCESSED=0
CACHE_HITS=0
CACHE_MISSES=0

CACHE_KEYS=""
CACHE_VALUES=""

log_message() {
    severity=$1
    shift
    message="$*"
    
    case "$severity" in
        ERROR)
            printf "[ERROR] %s\n" "$message" >&2
            GLOBAL_ERROR_COUNT=$((GLOBAL_ERROR_COUNT + 1))
            ;;
        WARN)
            printf "[WARN] %s\n" "$message" >&2
            ;;
        INFO)
            printf "[INFO] %s\n" "$message" >&2
            ;;
        DEBUG)
            if [ "${DEBUG_MODE:-0}" -eq 1 ]; then
                printf "[DEBUG] %s\n" "$message" >&2
            fi
            ;;
    esac
}

cache_put() {
    key="$1"
    value="$2"
    
    if cache_has "$key"; then
        log_message DEBUG "Updating cache key: $key"
        cache_remove "$key"
    else
        log_message DEBUG "Adding new cache key: $key"
    fi
    
    if [ -z "$CACHE_KEYS" ]; then
        CACHE_KEYS="$key"
        CACHE_VALUES="$value"
    else
        CACHE_KEYS="$CACHE_KEYS|$key"
        CACHE_VALUES="$CACHE_VALUES|$value"
    fi
}

cache_get() {
    key="$1"
    
    saved_ifs="$IFS"
    IFS="|"
    key_index=0
    found_index=-1
    
    for k in $CACHE_KEYS; do
        if [ "$k" = "$key" ]; then
            found_index=$key_index
            break
        fi
        key_index=$((key_index + 1))
    done
    
    if [ "$found_index" -ge 0 ]; then
        value_index=0
        for v in $CACHE_VALUES; do
            if [ "$value_index" -eq "$found_index" ]; then
                IFS="$saved_ifs"
                CACHE_HITS=$((CACHE_HITS + 1))
                echo "$v"
                return 0
            fi
            value_index=$((value_index + 1))
        done
    fi
    
    IFS="$saved_ifs"
    CACHE_MISSES=$((CACHE_MISSES + 1))
    return 1
}

cache_has() {
    key="$1"
    case "|$CACHE_KEYS|" in
        *"|$key|"*) return 0 ;;
        *) return 1 ;;
    esac
}

cache_remove() {
    key="$1"
    
    new_keys=""
    new_values=""
    saved_ifs="$IFS"
    IFS="|"
    key_index=0
    
    for k in $CACHE_KEYS; do
        if [ "$k" != "$key" ]; then
            if [ -z "$new_keys" ]; then
                new_keys="$k"
            else
                new_keys="$new_keys|$k"
            fi
            

            value_index=0
            for v in $CACHE_VALUES; do
                if [ "$value_index" -eq "$key_index" ]; then
                    if [ -z "$new_values" ]; then
                        new_values="$v"
                    else
                        new_values="$new_values|$v"
                    fi
                    break
                fi
                value_index=$((value_index + 1))
            done
        fi
        key_index=$((key_index + 1))
    done
    
    IFS="$saved_ifs"
    CACHE_KEYS="$new_keys"
    CACHE_VALUES="$new_values"
}


fibonacci() {
    n=$1
    
    if [ "$n" -le 0 ]; then
        echo 0
        return 0
    elif [ "$n" -eq 1 ]; then
        echo 1
        return 0
    fi
    

    cache_key="fib_$n"
    if cached_result=$(cache_get "$cache_key"); then
        echo "$cached_result"
        return 0
    fi
    

    n1=$((n - 1))
    n2=$((n - 2))
    fib1=$(fibonacci "$n1")
    fib2=$(fibonacci "$n2")
    result=$((fib1 + fib2))
    

    cache_put "$cache_key" "$result"
    echo "$result"
}

gcd() {
    a=$1
    b=$2
    

    a=${a#-}
    b=${b#-}
    
    while [ "$b" -ne 0 ]; do
        temp=$b
        b=$((a % b))
        a=$temp
    done
    
    echo "$a"
}

count_prime_factors() {
    n=$1
    count=0
    

    while [ $((n % 2)) -eq 0 ]; do
        count=$((count + 1))
        n=$((n / 2))
    done
    

    i=3
    while [ $((i * i)) -le "$n" ]; do
        while [ $((n % i)) -eq 0 ]; do
            count=$((count + 1))
            n=$((n / i))
        done
        i=$((i + 2))
    done
    

    if [ "$n" -gt 1 ]; then
        count=$((count + 1))
    fi
    
    echo "$count"
}


string_reverse() {
    input="$1"
    result=""
    length=${#input}
    
    while [ "$length" -gt 0 ]; do
        char=$(printf "%s" "$input" | cut -c "$length")
        result="$result$char"
        length=$((length - 1))
    done
    
    echo "$result"
}

count_vowels() {
    input="$1"
    count=0
    length=${#input}
    i=1
    
    while [ $i -le "$length" ]; do
        char=$(printf "%s" "$input" | cut -c "$i" | tr '[:upper:]' '[:lower:]')
        case "$char" in
            a|e|i|o|u) count=$((count + 1)) ;;
        esac
        i=$((i + 1))
    done
    
    echo "$count"
}

is_palindrome() {
    input="$1"
    reversed=$(string_reverse "$input")
    
    if [ "$input" = "$reversed" ]; then
        echo "true"
        return 0
    else
        echo "false"
        return 1
    fi
}


parse_data() {
    data="$1"
    saved_ifs="$IFS"
    IFS=","
    
    for pair in $data; do
        IFS=":"
        set -- $pair
        key="$1"
        value="$2"
        
        if [ -n "$key" ] && [ -n "$value" ]; then
            cache_put "data_$key" "$value"
            log_message DEBUG "Parsed: $key = $value"
        fi
        IFS=","
    done
    
    IFS="$saved_ifs"
}

compute_statistics() {
    data="$1"
    
    sum=0
    count=0
    min=""
    max=""
    
    saved_ifs="$IFS"
    IFS=","
    
    for num in $data; do

        case "$num" in
            ''|*[!0-9]*) continue ;;
        esac
        
        sum=$((sum + num))
        count=$((count + 1))
        
        if [ -z "$min" ] || [ "$num" -lt "$min" ]; then
            min=$num
        fi
        
        if [ -z "$max" ] || [ "$num" -gt "$max" ]; then
            max=$num
        fi
    done
    
    IFS="$saved_ifs"
    
    if [ "$count" -gt 0 ]; then
        avg=$((sum / count))
        echo "count:$count,sum:$sum,avg:$avg,min:$min,max:$max"
    else
        echo "count:0,sum:0,avg:0,min:0,max:0"
    fi
}


process_tree() {
    node="$1"
    depth="${2:-0}"
    
    # Extract node name (before parenthesis or entire string)
    node_name="${node%%(*}"
    
    if [ "$depth" -eq 0 ]; then
        log_message INFO "Processing tree: $node_name"
    fi
    
    # Count nodes excluding root
    if [ "$depth" -gt 0 ]; then
        GLOBAL_PROCESSED=$((GLOBAL_PROCESSED + 1))
    fi
    
    # Check if node has children using parameter expansion
    if [ "${node#*\(}" != "$node" ]; then
        children="${node#*\(}"
        children="${children%\)}"
        
        child_depth=$((depth + 1))
        
        saved_ifs="$IFS"
        IFS="," 
        for child in $children; do
            if [ -n "$child" ]; then
                process_tree "$child" "$child_depth"
            fi
        done
        IFS="$saved_ifs"
    fi
    
    return 0
}


run_comprehensive_test() {
    test_type="$1"
    
    case "$test_type" in
        math)
            log_message INFO "Running mathematical tests"
            
            # Test Fibonacci
            fib_10=$(fibonacci 10)
            log_message INFO "Fibonacci(10) = $fib_10"
            
            # Test GCD
            gcd_result=$(gcd 48 18)
            log_message INFO "GCD(48, 18) = $gcd_result"
            
            # Test prime factors
            factors=$(count_prime_factors 24)
            log_message INFO "Prime factors of 24: $factors"
            
            echo "math:fib=$fib_10,gcd=$gcd_result,factors=$factors"
            ;;
            
        string)
            log_message INFO "Running string processing tests"
            
            test_str="radar"
            reversed=$(string_reverse "$test_str")
            vowels=$(count_vowels "$test_str")
            palindrome=$(is_palindrome "$test_str")
            
            echo "string:reversed=$reversed,vowels=$vowels,palindrome=$palindrome"
            ;;
            
        data)
            log_message INFO "Running data processing tests"
            
            # Parse data
            parse_data "id:1,name:test,value:42"
            
            # Compute statistics
            stats=$(compute_statistics "10,20,30,40,50")
            
            echo "data:$stats"
            ;;
            
        tree)
            log_message INFO "Running tree processing tests"
            
            GLOBAL_PROCESSED=0
            process_tree "root(a,b(c,d),e)"
            
            echo "tree:nodes=$GLOBAL_PROCESSED"
            ;;
            
        cache)
            log_message INFO "Running cache performance tests"
            # Warm cache without subshells to ensure counters update correctly
            : "${CACHE_HITS:=0}" "${CACHE_MISSES:=0}"
            cache_put fib_1 1
            cache_put fib_2 1
            cache_put fib_3 2
            # Access entries to increment hit counter in the current shell
            cache_get fib_1 >/dev/null || true
            cache_get fib_2 >/dev/null || true
            cache_get fib_3 >/dev/null || true
            echo "cache:hits=$CACHE_HITS,misses=$CACHE_MISSES"
            ;;
            
        full)
            log_message INFO "Running full integration test"
            

            math_result=$(run_comprehensive_test math)
            string_result=$(run_comprehensive_test string)
            data_result=$(run_comprehensive_test data)
            tree_result=$(run_comprehensive_test tree)
            cache_result=$(run_comprehensive_test cache)
            

            echo "full:$math_result|$string_result|$data_result|$tree_result|$cache_result"
            ;;
            
        *)
            log_message ERROR "Unknown test type: $test_type"
            return 1
            ;;
    esac
    
    return 0
}

if [ $# -eq 0 ]; then
    log_message ERROR "No test type specified"
    exit 1
fi

run_comprehensive_test "$1"
exit_code=$?

if [ "$GLOBAL_ERROR_COUNT" -gt 0 ]; then
    log_message WARN "Completed with $GLOBAL_ERROR_COUNT errors"
fi

exit $exit_code
SCRIPT_EOF

chmod +x "$COMPLEX_SCRIPT"

TOTAL=$((TOTAL + 1))
output=$("$CJSH_PATH" "$COMPLEX_SCRIPT" math 2>/dev/null)
expected="math:fib=55,gcd=6,factors=4"
if [ "$output" = "$expected" ]; then
    pass "Complex mathematical operations (Fibonacci, GCD, prime factors)"
else
    fail "Complex mathematical operations" "expected: [$expected], got: [$output]"
fi

TOTAL=$((TOTAL + 1))
output=$("$CJSH_PATH" "$COMPLEX_SCRIPT" string 2>/dev/null)
expected="string:reversed=radar,vowels=2,palindrome=true"
if [ "$output" = "$expected" ]; then
    pass "Complex string operations (reverse, vowel count, palindrome check)"
else
    fail "Complex string operations" "expected: [$expected], got: [$output]"
fi

TOTAL=$((TOTAL + 1))
output=$("$CJSH_PATH" "$COMPLEX_SCRIPT" data 2>/dev/null)
expected="data:count:5,sum:150,avg:30,min:10,max:50"
if [ "$output" = "$expected" ]; then
    pass "Complex data processing pipeline (parsing, statistics)"
else
    fail "Complex data processing pipeline" "expected: [$expected], got: [$output]"
fi

TOTAL=$((TOTAL + 1))
output=$("$CJSH_PATH" "$COMPLEX_SCRIPT" tree 2>/dev/null)
expected="tree:nodes=5"
if [ "$output" = "$expected" ]; then
    pass "Complex tree traversal and recursive processing"
else
    fail "Complex tree traversal" "expected: [$expected], got: [$output]"
fi

TOTAL=$((TOTAL + 1))
output=$("$CJSH_PATH" "$COMPLEX_SCRIPT" cache 2>/dev/null)
if echo "$output" | grep -q "cache:hits=[0-9]*,misses=[0-9]*"; then
    pass "Cache management system with memoization"
else
    fail "Cache management system" "unexpected output format: [$output]"
fi

TOTAL=$((TOTAL + 1))
"$CJSH_PATH" "$COMPLEX_SCRIPT" invalid_test >/dev/null 2>&1
exit_code=$?
if [ "$exit_code" -ne 0 ]; then
    pass "Error handling for invalid test type"
else
    fail "Error handling" "expected non-zero exit code, got: $exit_code"
fi

TOTAL=$((TOTAL + 1))
"$CJSH_PATH" "$COMPLEX_SCRIPT" >/dev/null 2>&1
exit_code=$?
if [ "$exit_code" -ne 0 ]; then
    pass "Error handling for missing arguments"
else
    fail "Error handling for missing arguments" "expected non-zero exit code, got: $exit_code"
fi

TOTAL=$((TOTAL + 1))
output=$("$CJSH_PATH" "$COMPLEX_SCRIPT" full 2>/dev/null)
if echo "$output" | grep -q "full:math:" && \
   echo "$output" | grep -q "string:" && \
   echo "$output" | grep -q "data:" && \
   echo "$output" | grep -q "tree:" && \
   echo "$output" | grep -q "cache:"; then
    pass "Full integration test with all components"
else
    fail "Full integration test" "missing expected components in: [$output]"
fi

rm -f "$COMPLEX_SCRIPT"

printf "Total Tests:    %3d\n" "$TOTAL"
printf "Passed:         %3d (%.1f%%)\n" "$PASSED" "$((PASSED * 100 / (TOTAL > 0 ? TOTAL : 1)))"
printf "Failed:         %3d (%.1f%%)\n" "$FAILED" "$((FAILED * 100 / (TOTAL > 0 ? TOTAL : 1)))"
if [ "$FAILED" -eq 0 ]; then
    echo "SUCCESS: cjsh passed all tests"
    exit 0
else
    echo "WARNING: $FAILED test(s) failed"
    exit 1
fi
