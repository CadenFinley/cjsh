#!/usr/bin/env python3
# time_test_binaries.py
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


import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import os
import shlex
import statistics
import subprocess
import sys
import time
from typing import List, Tuple, Dict, Optional

RUNS = 50

SHELL_COMMANDS = {
    "posix": {
        "loop": "-c 'i=1; while [ $i -le 5000 ]; do echo $i; i=$((i+1)); done'",
        "loop_even": "-c 'i=1; while [ $i -le 5000 ]; do if [ $((i % 2)) -eq 0 ]; then echo $i; fi; i=$((i+1)); done'",
        "prime_sieve": '-c \'MAX=1000; primes=""; i=2; while [ $i -le $MAX ]; do is_prime=1; for p in $primes; do if [ $((p*p)) -gt $i ]; then break; fi; if [ $((i % p)) -eq 0 ]; then is_prime=0; break; fi; done; if [ $is_prime -eq 1 ]; then echo $i; primes="$primes $i"; fi; i=$((i+1)); done\'',
        "branching": "-c 'count=0; i=1; while [ $i -le 2000 ]; do if [ $((i % 15)) -eq 0 ]; then count=$((count+1)); elif [ $((i % 3)) -eq 0 ]; then :; elif [ $((i % 5)) -eq 0 ]; then :; fi; i=$((i+1)); done; echo $count'",
        "function_calls": '-c \'sum(){ out=0; for n in "$@"; do out=$((out+n)); done; echo "$out"; }; i=1; while [ $i -le 400 ]; do sum 1 2 3 4 5 >/dev/null; i=$((i+1)); done\'',
        "subshell_traversal": '-c \'for dir in /bin /usr/bin /usr/sbin; do if [ -d "$dir" ]; then (cd "$dir" && ls >/dev/null); fi; done\'',
    },
    "fish": {
        "loop": "-c 'for i in (seq 5000); echo $i; end'",
        "loop_even": "-c 'for i in (seq 5000); if test (math \"$i % 2\") -eq 0; echo $i; end; end'",
        "prime_sieve": '-c \'set MAX 1000; set primes; set i 2; while test $i -le $MAX; set is_prime 1; for p in $primes; if test (math "$p * $p") -gt $i; break; end; if test (math "$i % $p") -eq 0; set is_prime 0; break; end; end; if test $is_prime -eq 1; echo $i; set primes $primes $i; end; set i (math "$i + 1"); end\'',
        "branching": '-c \'set count 0; for i in (seq 1 2000); if test (math "$i % 15") -eq 0; set count (math "$count + 1"); else if test (math "$i % 3") -eq 0; math "$i + 0" >/dev/null; else if test (math "$i % 5") -eq 0; math "$i + 0" >/dev/null; end; end; echo $count\'',
        "function_calls": "-c 'function sum; set out 0; for n in $argv; set out (math \"$out + $n\"); end; echo $out; end; for i in (seq 1 400); sum 1 2 3 4 5 >/dev/null; end'",
        "subshell_traversal": "-c 'for dir in /bin /usr/bin /usr/sbin; if test -d $dir; pushd $dir >/dev/null; ls >/dev/null; popd >/dev/null; end; end'",
    },
    "nu": {
        "loop": "-c '1..5000 | each { |i| echo $i }'",
        "loop_even": "-c '1..5000 | where { |i| $i mod 2 == 0 } | each { |i| echo $i }'",
        "prime_sieve": "-c 'let MAX = 1000; mut primes = []; mut i = 2; while $i <= $MAX { mut is_prime = true; for p in $primes { if ($p * $p) > $i { break }; if ($i mod $p) == 0 { $is_prime = false; break } }; if $is_prime { echo $i; $primes = ($primes | append $i) }; $i = $i + 1 }'",
        "branching": "-c 'mut count = 0; for i in 1..2000 { if (($i mod 15) == 0) { $count += 1 } else if (($i mod 3) == 0) { } else if (($i mod 5) == 0) { } }; echo $count'",
        "function_calls": "-c 'def sum [values: list<int>] { mut out = 0; for v in $values { $out += $v }; $out }; for _ in 1..400 { sum [1 2 3 4 5] | ignore }'",
        "subshell_traversal": "-c 'for dir in [/bin /usr/bin /usr/sbin] { if ($dir | path exists) { cd $dir; ls | ignore } }'",
    },
    "elvish": {
        "loop": "-c \"sh -c 'i=1; while [ $i -le 5000 ]; do echo $i; i=$((i+1)); done'\"",
        "loop_even": "-c \"sh -c 'i=1; while [ $i -le 5000 ]; do if [ $((i % 2)) -eq 0 ]; then echo $i; fi; i=$((i+1)); done'\"",
        "prime_sieve": "-c \"sh -c 'MAX=1000; i=2; while [ $i -le $MAX ]; do is_prime=1; j=2; while [ $((j*j)) -le $i ]; do if [ $((i % j)) -eq 0 ]; then is_prime=0; break; fi; j=$((j+1)); done; if [ $is_prime -eq 1 ]; then echo $i; fi; i=$((i+1)); done'\"",
        "branching": "-c \"sh -c 'count=0; i=1; while [ $i -le 2000 ]; do if [ $((i % 15)) -eq 0 ]; then count=$((count+1)); elif [ $((i % 3)) -eq 0 ]; then :; elif [ $((i % 5)) -eq 0 ]; then :; fi; i=$((i+1)); done; echo $count'\"",
        "function_calls": "-c \"sh -c 'sum(){ out=0; for n in $@; do out=$((out+n)); done; echo $out; }; i=1; while [ $i -le 400 ]; do sum 1 2 3 4 5 >/dev/null; i=$((i+1)); done'\"",
        "subshell_traversal": "-c \"sh -c 'for dir in /bin /usr/bin /usr/sbin; do if [ -d $dir ]; then (cd $dir && ls >/dev/null); fi; done'\"",
    },
    "ion": {
        "loop": "-c \"sh -c 'i=1; while [ $i -le 5000 ]; do echo $i; i=$((i+1)); done'\"",
        "loop_even": "-c \"sh -c 'i=1; while [ $i -le 5000 ]; do if [ $((i % 2)) -eq 0 ]; then echo $i; fi; i=$((i+1)); done'\"",
        "prime_sieve": "-c \"sh -c 'MAX=1000; i=2; while [ $i -le $MAX ]; do is_prime=1; j=2; while [ $((j*j)) -le $i ]; do if [ $((i % j)) -eq 0 ]; then is_prime=0; break; fi; j=$((j+1)); done; if [ $is_prime -eq 1 ]; then echo $i; fi; i=$((i+1)); done'\"",
        "branching": "-c \"sh -c 'count=0; i=1; while [ $i -le 2000 ]; do if [ $((i % 15)) -eq 0 ]; then count=$((count+1)); elif [ $((i % 3)) -eq 0 ]; then :; elif [ $((i % 5)) -eq 0 ]; then :; fi; i=$((i+1)); done; echo $count'\"",
        "function_calls": "-c \"sh -c 'sum(){ out=0; for n in $@; do out=$((out+n)); done; echo $out; }; i=1; while [ $i -le 400 ]; do sum 1 2 3 4 5 >/dev/null; i=$((i+1)); done'\"",
        "subshell_traversal": "-c \"sh -c 'for dir in /bin /usr/bin /usr/sbin; do if [ -d $dir ]; then (cd $dir && ls >/dev/null); fi; done'\"",
    },
}

COMMAND_PLAN = [
    {"key": "loop", "description": "High-iteration loop with stdout output"},
    {"key": "loop_even", "description": "Loop with conditional filtering"},
    {
        "key": "prime_sieve",
        "description": "Generate primes up to 1000 using incremental divisibility checks",
    },
    {"key": "branching", "description": "Nested conditionals with arithmetic checks"},
    {"key": "function_calls", "description": "Define and repeatedly invoke a function"},
    {
        "key": "subshell_traversal",
        "description": "Traverse directories using subshells or directory stack",
    },
]
BASELINE_SHELLS = ["cjsh", "bash", "zsh", "fish", "nu", "osh", "yash", "dash", "elvish", "ion"]
CJSH_BINARY_TYPES = [""]

ENABLE_BASELINE_TESTS = True


@dataclass
class RunMetrics:
    elapsed_ms: float
    user_cpu_ms: float
    system_cpu_ms: float
    peak_rss_mib: float

    @property
    def cpu_percent(self) -> float:
        if self.elapsed_ms <= 0:
            return 0.0
        return (self.user_cpu_ms + self.system_cpu_ms) / self.elapsed_ms * 100


@dataclass
class ShellResult:
    shell: str
    runs: List[RunMetrics]

    @property
    def average_time(self) -> float:
        return statistics.mean(run.elapsed_ms for run in self.runs)


all_results: List[List[ShellResult]] = []
all_commands: List[Dict[str, str]] = []


EXPECTED_OUTPUTS = {
    "hello": {"contains": "hello world", "exact": False},
    "loop": {"line_count": 5000, "tolerance": 0},
    "loop_even": {"line_count": 2500, "tolerance": 0},
    "prime_sieve": {"line_count": 168, "tolerance": 0, "contains": "997", "exact": False},
    "branching": {"contains": "133", "exact": True},
    "function_calls": {"stdout": ""},
    "exit": {"returncode": 0},
}


def validate_command_output(
    shell_cmd: str, command: str, command_key: str
) -> Tuple[bool, str]:
    """
    Validate that a command produces expected output.
    Returns (is_valid, error_message)
    """
    expected = EXPECTED_OUTPUTS.get(command_key, {})

    try:
        result = subprocess.run(
            [shell_cmd, *shlex.split(command)],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return (False, "Command timed out (>30s)")
    except Exception as e:
        return (False, f"Command execution failed: {str(e)}")

    expected_returncode = expected.get("returncode", 0)
    if result.returncode != expected_returncode:
        return (
            False,
            f"Expected return code {expected_returncode}, got {result.returncode}: "
            f"{result.stderr.strip()[:200]}",
        )

    if "stdout" in expected and result.stdout != expected["stdout"]:
        return (
            False,
            f"Expected stdout {expected['stdout']!r}, got: {result.stdout[:100]!r}",
        )

    if "contains" in expected:
        if expected.get("exact", False):
            if expected["contains"].strip() not in result.stdout.strip():
                return (
                    False,
                    f"Expected output to contain exactly '{expected['contains']}', got: {result.stdout.strip()[:100]}",
                )
        else:
            if expected["contains"] not in result.stdout:
                return (
                    False,
                    f"Expected output to contain '{expected['contains']}', got: {result.stdout[:100]}",
                )

    if "line_count" in expected:
        actual_lines = len(
            [line for line in result.stdout.splitlines() if line.strip()]
        )
        expected_lines = expected["line_count"]
        tolerance = expected.get("tolerance", 0)

        if abs(actual_lines - expected_lines) > tolerance:
            return (
                False,
                f"Expected {expected_lines} lines of output (±{tolerance}), got {actual_lines}",
            )

    return (True, "")


def peak_rss_to_mib(peak_rss: int) -> float:
    # macOS reports bytes; Linux and the other BSDs report KiB.
    return peak_rss / (1024 * 1024 if sys.platform == "darwin" else 1024)


def run_command_with_timing(shell_cmd: str, command: str) -> RunMetrics:
    """Measure one shell invocation using its own wait4 resource counters."""
    args = [shell_cmd, *shlex.split(command)]
    start_time = time.perf_counter()
    # Launch the target directly so an extra /bin/sh is not measured. Output
    # was checked during validation; discard it here to avoid full pipe buffers
    # blocking the child while wait4 waits for it to finish.
    with subprocess.Popen(
        args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    ) as process:
        _, status, usage = os.wait4(process.pid, 0)
        elapsed_ms = (time.perf_counter() - start_time) * 1000
        # wait4 already reaped the child; prevent Popen from waiting again.
        process.returncode = os.waitstatus_to_exitcode(status)
        if process.returncode != 0:
            raise subprocess.CalledProcessError(process.returncode, args)

    return RunMetrics(
        elapsed_ms=elapsed_ms,
        user_cpu_ms=usage.ru_utime * 1000,
        system_cpu_ms=usage.ru_stime * 1000,
        peak_rss_mib=peak_rss_to_mib(usage.ru_maxrss),
    )


def get_shell_command(shell: str, command_key: str) -> Optional[str]:
    if shell in ["bash", "zsh", "ksh", "osh", "yash", "dash"] or shell.startswith("./cjsh"):
        return SHELL_COMMANDS["posix"].get(command_key)
    if shell in ["fish", "../fish-shell/build/fish"]:
        return SHELL_COMMANDS["fish"].get(command_key)
    if shell == "nu":
        return SHELL_COMMANDS["nu"].get(command_key)
    if shell == "elvish":
        return SHELL_COMMANDS["elvish"].get(command_key)
    if shell == "ion":
        return SHELL_COMMANDS["ion"].get(command_key)
    if shell == "xonsh":
        return SHELL_COMMANDS["xonsh"].get(command_key)
    if shell in ["tcsh", "csh"]:
        return SHELL_COMMANDS["csh"].get(command_key)
    return SHELL_COMMANDS["posix"].get(command_key)


def measure_shell(shell_name: str, shell_path: str, command: str, runs: int) -> ShellResult:
    # Keep repetitions sequential within a shell; concurrency is across shells.
    return ShellResult(
        shell_name, [run_command_with_timing(shell_path, command) for _ in range(runs)]
    )


def test_command(command_spec: Dict[str, str], runs: int = RUNS, jobs: int = 1) -> None:
    command_key = command_spec["key"]
    results: List[ShellResult] = []

    print("----------------------------------------------------------------------")
    print(f"Testing command: {command_key}")
    description = command_spec.get("description", "")
    if description:
        print(description)
    print("----------------------------------------------------------------------")

    shells = [
        (f"./cjsh{binary_type}", f"./build/release/cjsh{binary_type}")
        for binary_type in CJSH_BINARY_TYPES
    ]
    if ENABLE_BASELINE_TESTS:
        shells.extend((shell, shell) for shell in BASELINE_SHELLS)

    # Finish validation before measuring so captured-output validation runs do
    # not compete with the measured workloads.
    validated_shells = []
    for shell_name, shell_path in shells:
        command = get_shell_command(shell_name, command_key)

        if command is None:
            print(f"Skipping {shell_name}: command '{command_key}' not defined")
            continue

        print()
        print(f"Validating {shell_name} {command}")
        is_valid, error_msg = validate_command_output(shell_path, command, command_key)
        if not is_valid:
            print(f"  VALIDATION FAILED: {error_msg}")
            print(f"  Skipping performance test for {shell_name}")
            continue
        print("  Validation passed")
        validated_shells.append((shell_name, shell_path, command))

    if validated_shells:
        workers = min(jobs, len(validated_shells))
        print(f"Measuring time and resource usage with up to {workers} shells at a time...")
        print()

        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = {
                executor.submit(measure_shell, shell_name, shell_path, command, runs): shell_name
                for shell_name, shell_path, command in validated_shells
            }
            # Only the main thread prints and updates shared results.
            for future in as_completed(futures):
                shell_name = futures[future]
                try:
                    results.append(future.result())
                except (OSError, subprocess.CalledProcessError) as exc:
                    print(f"  MEASUREMENT FAILED for {shell_name}: {exc}")
                    print(f"  Skipping performance results for {shell_name}")
                    continue
                print(f"  Completed {shell_name}: {runs} runs")

    print("----------------------------------------------------------------------")

    results.sort(key=lambda result: result.average_time)

    all_commands.append(command_spec)
    all_results.append(results)

    print(f"Completed testing: {command_key}")
    print()


def get_cjsh_version() -> str:
    try:
        result = subprocess.run(
            "./build/release/cjsh --version",
            shell=True,
            capture_output=True,
            text=True,
            check=False,
        )
        return result.stdout.strip() if result.stdout else "Version unavailable"
    except Exception:
        return "Version unavailable"


def print_summary(runs: int = RUNS, jobs: int = 1) -> None:
    print("======================================================================")
    print("                           FINAL RESULTS SUMMARY")
    print("======================================================================")
    print(f"Total runs per command: {runs}")
    print(f"Maximum concurrent shells: {jobs}")
    if jobs > 1:
        print("Concurrent workloads share system resources and can affect timing comparisons.")
    print(get_cjsh_version())
    print("CPU usage: mean of (user + system CPU time) / wall time per run; 100% = one core.")
    print("Resources include child processes accounted for by the OS when the shell waits.")
    print("Memory: per-run peak resident set size (RSS), in MiB; not summed process-tree memory.")
    print("Measured runs launch shells directly with stdout/stderr discarded.")
    print("======================================================================")

    for i, command_spec in enumerate(all_commands):
        command_key = command_spec["key"]
        print()
        description = command_spec.get("description", "")
        if description:
            print(f"Command: {command_key} - {description}")
        else:
            print(f"Command: {command_key}")
        print("----------------------------------------------------------------------")

        if not all_results[i]:
            print("No successful measurements for this command.")
        else:
            for result in all_results[i]:
                shell = result.shell
                times = [run.elapsed_ms for run in result.runs]
                user_cpu = statistics.mean(run.user_cpu_ms for run in result.runs)
                system_cpu = statistics.mean(run.system_cpu_ms for run in result.runs)
                cpu_percent = statistics.mean(run.cpu_percent for run in result.runs)
                peak_rss = [run.peak_rss_mib for run in result.runs]
                actual_command = get_shell_command(shell, command_key) or "N/A"
                print(f"{shell} ({actual_command}):")
                print(f"  Avg time: {result.average_time:.3f} ms")
                print(f"  Min time: {min(times):.3f} ms")
                print(f"  Max time: {max(times):.3f} ms")
                print(f"  Avg user CPU time: {user_cpu:.3f} ms")
                print(f"  Avg system CPU time: {system_cpu:.3f} ms")
                print(f"  Avg CPU usage: {cpu_percent:.2f}%")
                print(f"  Avg peak RSS: {statistics.mean(peak_rss):.3f} MiB")
                print(f"  Max peak RSS: {max(peak_rss):.3f} MiB")

        print("----------------------------------------------------------------------")

    print("======================================================================")


def check_binaries_exist() -> bool:
    missing_binaries = []

    for binary_type in CJSH_BINARY_TYPES:
        binary_path = f"./build/release/cjsh{binary_type}"
        if not os.path.isfile(binary_path):
            missing_binaries.append(binary_path)
        elif not os.access(binary_path, os.X_OK):
            print(f"Warning: {binary_path} exists but is not executable")

    if ENABLE_BASELINE_TESTS:
        for shell in BASELINE_SHELLS:
            try:
                result = subprocess.run(
                    f"which {shell}", shell=True, capture_output=True, check=False
                )
                if result.returncode != 0:
                    missing_binaries.append(shell)
            except Exception:
                missing_binaries.append(shell)

    if missing_binaries:
        print("Error: The following required binaries are missing or not accessible:")
        for binary in missing_binaries:
            if binary.startswith("./build/"):
                print(f"  {binary} (build the project first)")
            else:
                print(f"  {binary} (install or check PATH)")
        print()
        print("Please ensure all required binaries are available before running tests.")
        return False

    return True


def main(argv: Optional[List[str]] = None) -> None:
    parser = argparse.ArgumentParser(description="Compare shell timing, CPU usage, and peak memory.")
    parser.add_argument(
        "-j", "--jobs", type=int, default=1,
        help="Maximum shells to measure concurrently (default: %(default)s); shared load affects timing",
    )
    parser.add_argument(
        "--runs", type=int, default=RUNS,
        help="Measured runs per shell and command (default: %(default)s)",
    )
    args = parser.parse_args(argv)
    if args.jobs <= 0:
        parser.error("--jobs must be greater than 0")
    if args.runs <= 0:
        parser.error("--runs must be greater than 0")

    if not hasattr(os, "wait4"):
        print("Resource measurement requires os.wait4 (macOS, Linux, or BSD).", file=sys.stderr)
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    parent_dir = os.path.dirname(script_dir)
    os.chdir(parent_dir)

    if not check_binaries_exist():
        sys.exit(1)

    print("All required binaries found. Starting performance tests...")
    print(f"Maximum concurrent shells: {args.jobs}")
    print()

    for command_spec in COMMAND_PLAN:
        test_command(command_spec, runs=args.runs, jobs=args.jobs)

    print_summary(runs=args.runs, jobs=args.jobs)


if __name__ == "__main__":
    main()
