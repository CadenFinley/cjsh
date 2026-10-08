<!--
  README.md

  This file is part of cjsh, CJ's Shell

  MIT License

  Copyright (c) 2026 Caden Finley

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
-->

# Tests

CTest is the canonical entry point for automated tests. Run commands below from the
repository root after configuring and building a preset. See
[CONTRIBUTING.md](../CONTRIBUTING.md#local-verification) for setup and Debug/ASan verification.

## Where tests belong

| Path | Use for |
| --- | --- |
| `shell/` | End-to-end scripting, builtins, expansion, redirection, signals, and regression cases; fixtures live in `shell/resources/` |
| `core/` | Focused core/parser/runtime C++ tests and Python process, startup, lifecycle, and interactive tests |
| `posix/` | POSIX-mode behavior and extension restrictions; see its [test policy](posix/README.md) |
| `isocline/` | C line-editor tests, PTY interaction, terminal contracts, and concurrent history storage |
| `completions/` | Focused completion tests |
| `highlighter/` | Syntax-highlighting tests |
| `build_system/` | CMake configuration, build metadata, and preset behavior |
| `runner/` | Test process isolation and combined-result reporting |
| `fuzz/` | Lexical parser libFuzzer driver and seed corpus; no command execution |
| `tooling/` | Lint-driver behavior and release/archive/install license-notice checks |
| `test_history_expansion.cpp` | Focused history-expansion tests |

The `time_startup_binaries.py` and `time_test_binaries.py` scripts are manual timing
utilities, not additional CTest suites. Use their `--help` output for options;
performance comparisons need controlled, otherwise-idle environments.

## Common commands

```bash
ctest --preset release --parallel 4                   # full suite
ctest --preset release -N                             # list registered tests
ctest --preset release -L shell --parallel 4           # shell scripts
ctest --preset release -L posix --parallel 4           # strict POSIX-mode suites
ctest --preset release -L tooling                     # lint and packaging regressions
ctest --preset release -R '^shell\.test_alias$'        # one shell suite
ctest --preset release --rerun-failed --output-on-failure
```

For a custom build directory, use `ctest --test-dir <build-dir> --output-on-failure`.
CTest passes the binary from that build to each suite; do not rely on an installed
`cjsh` being the version under test.

Compiled test executables and helper libraries are in `<build-dir>/tests/`
(for example, `build/release/tests/`). The shell executable is at `<build-dir>/cjsh`.

## Adding a regression

1. Prefer the existing suite that owns the behavior; use a small, deterministic reproducer.
2. Check exit status, stdout, and stderr where relevant. Include quoting, error, cleanup,
   and native/POSIX differences when those are affected.
3. New `shell/test_*.sh` files are discovered by CMake and registered as `shell.<filename>`
   without the `.sh` suffix. Reconfigure/rebuild after adding files. Other C/C++/Python
   suites require explicit targets and `cjsh_add_test()` entries in [CMakeLists.txt](CMakeLists.txt).
4. Use `cjsh_add_test()` rather than bypassing the isolation launcher. Put temporary data
   in a per-test directory, and clean up child processes even on failure or timeout.
5. Interactive tests should allocate their own PTY and wait for observable state rather
   than assuming a fixed delay is sufficient. Never manipulate the developer's terminal.
6. Report unavailable prerequisites as explicit skips with reasons; do not silently
   count an unexecuted case as passing. Record disabled/flaky cases and their re-enable criteria.

Use the [compatibility inventory](../docs/reference/language-compatibility.md) and the
POSIX specification to decide expected behavior. Bash is useful for comparison, but is
not the oracle for every native extension or POSIX.1-2024 requirement.

## Isolation and diagnosing failures

The `runner/run_test.c` launcher runs each CTest worker in a separate process session
with `/dev/null` on stdin. It forwards cancellation to the worker's process group;
interactive suites create their own pseudoterminals. Timing and system-wide process-count
checks run serially, and legacy tests sharing a temporary path use a resource lock.
New tests should avoid shared paths instead of adding more global serialization.

For failures:

- Re-run the named suite with `-R` and `--output-on-failure`; use `--parallel 1` when
  investigating timing or process-count interference.
- Inspect `<build-dir>/Testing/Temporary/LastTest.log` and any failure output from the suite.
- Repeat suspected flakes with `ctest --preset release -R '<pattern>' --repeat until-fail:10`.
- Keep platform/locale skips visible; a green run does not establish that skipped cases passed.

CTest reports registered suites, and the custom summary additionally combines individual
case counts where suites provide them. Check both failures and skipped counts; neither a
large total nor a passing summary is a substitute for relevant regression coverage.

## Sanitizers and fuzzing

CI runs the full suite with the `ci-linux-clang-debug` preset, which enables
AddressSanitizer and UndefinedBehaviorSanitizer. Reports fail the job and failure
logs are uploaded as artifacts. The ordinary `debug` preset retains ASan; add
`-DCJSH_ENABLE_UBSAN=ON` when configuring it to include UBSan.

The separate parser fuzzing job uses [LLVM libFuzzer](https://llvm.org/docs/LibFuzzer.html)
with both sanitizers and a bounded 120-second run. It exercises lexical splitting,
line preprocessing, and tokenization with arbitrary bytes, plus generated nested
arithmetic that must preserve subsequent logical operators. It never executes
commands or expands fuzzed words. Crashing inputs are retained as CI artifacts.

To build and run it locally with Clang on Linux:

```sh
cmake --preset ci-linux-clang-debug -DCJSH_BUILD_FUZZERS=ON
cmake --build --preset ci-linux-clang-debug --target parser_fuzzer --parallel 4
mkdir -p build/fuzz-corpus build/fuzz-artifacts
cp tests/fuzz/corpus/* build/fuzz-corpus/
build/ci-linux-clang-debug/tests/parser_fuzzer build/fuzz-corpus \
  -max_total_time=120 -timeout=5 -max_len=4096 \
  -rss_limit_mb=2048 -artifact_prefix=build/fuzz-artifacts/
```

The driver is compiled as an object in ordinary test builds so it remains covered
by static analysis. The libFuzzer executable requires `CJSH_BUILD_FUZZERS=ON` and
a Debug build. Keep generated corpus entries under the build directory; reduce
failures and add useful reproductions to `fuzz/corpus/` and the regression suites.
