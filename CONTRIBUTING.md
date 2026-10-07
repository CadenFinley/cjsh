<!--
  CONTRIBUTING.md

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

# Contributing to CJ's Shell

Thanks for contributing to `cjsh`. The most useful pull requests are focused, tested locally, and accompanied by the documentation updates needed to explain behavior changes.

## Before You Start

- Search existing issues and pull requests before starting duplicate work.
- Open an issue before large behavioral changes, wide refactors, or compatibility-impacting work.
- Keep feature work, refactors, and formatting-only changes in separate pull requests when possible.
- Base pull requests on `master`.

## Tooling

You will need:

- C compiler
- C++ compiler
- CMake 3.25 or newer
- Ninja
- Python 3.10 or newer for lint tooling and parts of the test suite
- LLVM 23's `clang-format` and `clang-tidy` for the CI formatting and analysis baseline

## Build

From the repository root:

```bash
git clone https://github.com/CadenFinley/cjsh && cd cjsh
cmake --preset release
cmake --build --preset release --parallel
```

Useful presets:

- `release`: optimized default build
- `debug`: debug build with AddressSanitizer enabled
- `minimal`: size-focused release profile
- `relwithdebinfo`: optimized build with symbols
- `minsizerel`: CMake `MinSizeRel` profile

List presets with `cmake --list-presets`.

## Local Verification

Use CTest for all automated tests. Before opening a pull request, run the checks that match the scope of your change. For most code changes, that means:

```bash
ctest --preset release --parallel 4
```

If you touch parser, interpreter, job control, interactive input, or other memory-sensitive runtime code, also test the debug preset:

```bash
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --parallel 4
```

CTest runs all shell files and the focused C, C++, and Python suites. `--parallel 4`
runs up to four independent suites at once; adjust the number for your machine or
use `--parallel 1` for a serial run. Timing and system-wide process-count checks
run alone, and tests sharing temporary files are locked against each other.
GitHub CI uses the same four-worker configuration for each build preset.
Each CTest worker runs in a separate process session with `/dev/null` on stdin,
so tests cannot read or change the invoking terminal through stdin or `/dev/tty`.
The launcher forwards cancellation signals to the worker's process group.
Interactive suites create their own pseudoterminals.

Use `ctest --preset release --parallel 4 -L shell` for just the shell files, or
`ctest --preset release --parallel 4 -LE shell` for just the focused suites.
`ctest --preset release --rerun-failed --output-on-failure` repeats failed suites.
To run one shell file, use `ctest --preset release -R '^shell\.test_alias$'`
(using the filename without `.sh`). CTest also prints combined individual-test
counts after its suite summary.

If you configured a custom build directory, use `ctest --test-dir build --parallel 4 --output-on-failure`
instead of a preset, replacing `build` with that directory. CTest selects its built
`cjsh` binary automatically.

## Code Style

See [cjsh-core/README.md](cjsh-core/README.md) for a source map, execution entry points,
and runtime constraints, and [cjsh-isocline/README.md](cjsh-isocline/README.md) for the
vendored editor's maintenance notes.

Check all C/C++ sources and headers, including tests, from the repository root:

```bash
cmake --preset release
python3 tools/lint.py
```

The script checks formatting and analyzes every compilation unit, its project
headers, and standalone headers, treating lint warnings as failures. Included isocline
C implementation fragments are analyzed through their owning compilation units.
Sources missing from the compilation database are errors, not silent skips; configure
with `CJSH_BUILD_TESTS=ON` and `CJSH_GENERATE_COMPILE_COMMANDS=ON` (the `release` defaults).
The script does not modify sources or the build's compilation database.

It supplies the macOS SDK paths when using Homebrew LLVM. Use `--build-dir build/debug`
to analyze the debug configuration, and `--jobs 4` to limit parallel workers.
`--clang-format` and `--clang-tidy` select specific tool executables. For example:

```bash
brew install llvm
python3 tools/lint.py \
  --clang-format "$(brew --prefix llvm)/bin/clang-format" \
  --clang-tidy "$(brew --prefix llvm)/bin/clang-tidy"
```

Use LLVM 23 to match CI; a major toolchain upgrade requires reviewing formatting and
new diagnostics before updating the CI version guard. `--format-only` works without
a configured build; `--tidy-only` runs just static analysis. `--timeout 300` sets the
per-file timeout in seconds. Exit codes are 0 for success, 1 for failed checks, and 2
for setup errors. Other file types currently have no configured lint rules.

Tooling and release-notice regressions can also be run without building the shell:

```bash
python3 -m unittest discover -s tests/tooling -p 'test_*.py' -v
```

They are registered with CTest under the `tooling` label.

Keep exceptions specific and documented. C API tests deliberately exercise invalid
enum values. The Annex K replacement recommendation is disabled because the
supported POSIX libraries do not provide those optional APIs; buffer-bounds and
unbounded-copy checks remain enabled.
The include cleaner ignores private Apple SDK headers in favor of their public
C/POSIX counterparts; it still checks project and standard C++ includes.

- C sources must remain compatible with ISO C11.
- C++ sources must remain compatible with ISO C++17.
- Format touched C and C++ files with `clang-format` using the repository's `.clang-format` file.
- Use braces around `if`, `else`, `for`, `do`, and `while` bodies, including single statements. The formatter inserts missing braces automatically, except inside macro definitions or around preprocessor directives.
- Follow the existing naming, file layout, and style conventions in the area you are modifying.
- Keep changes as small as practical. Small, well-scoped patches are easier to review and safer to merge.

## Tests

Add or update tests when you change behavior. See [tests/README.md](tests/README.md)
for suite ownership, test registration, isolation requirements, and failure diagnosis.

- Use `tests/shell/` for end-to-end shell behavior and scripting regressions.
- Use the focused C, C++, and Python tests under `tests/` for subsystem-specific coverage.
- If you fix a bug, add a regression test whenever practical.

## Documentation

Update the docs when you change user-visible behavior, builtins, flags, completion behavior, or interactive features.

To preview the docs locally:

```bash
python3 -m pip install -r docs/requirements.txt
mkdocs serve --config-file docs/mkdocs.yml
```

## Pull Requests

When opening a pull request:

- Explain the user-visible change and why it is needed.
- List the commands you ran locally.
- Note any platform-specific testing you performed.
- Include screenshots or terminal recordings for prompt, editing, highlighting, or other UI-facing changes when helpful.

## Continuous Integration

Pull requests and pushes to `master` run the GitHub Actions workflows in `.github/workflows/`. Keep local verification aligned with the parts of CI your change is expected to affect.

The **C/C++ Lint (LLVM 23)** job checks formatting and static analysis on macOS, using
the same `tools/lint.py` command as local development. Any reported project warning,
formatting violation, tool failure, or timeout fails the job. Linux portability is
still exercised by the existing build/test jobs; this is not a Linux static-analysis run.
Repository administrators should make the lint job and build/test jobs required checks
in the branch ruleset; workflow files alone cannot enforce merge protection.

Administrators must also enable **Private vulnerability reporting** in GitHub's
repository security settings so the reporting link in [SECURITY.md](SECURITY.md) works.
Do not use public issues for sensitive vulnerability details.

The CI workflow also builds and runs the full CTest suite on a Windows 2025 runner
using Ubuntu 24.04 under WSL 2. It uses the `release-artifact` preset and four test
workers, with a regular Linux user and a checkout inside the Linux filesystem so
file permissions and executable bits behave as expected. To reproduce locally in
WSL, clone the repository under your Linux home directory and use the same preset.

## Releases

Stable releases are built from tags whose names use the `vX.Y.Z` format. Before creating a tag,
update the version in the root `CMakeLists.txt` and prepare its entry in `CHANGELOG.md`.
Keep upcoming changes under `## [Unreleased]`, grouped into the applicable `Added`, `Changed`,
`Deprecated`, `Removed`, `Fixed`, or `Security` sections from
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/). Omit empty change categories.

At release time, move those notes into a `## [X.Y.Z] - YYYY-MM-DD` section immediately below
`Unreleased`, using the release date. Add the version's comparison link at the bottom of the
file and advance the `Unreleased` link to compare the new tag with `HEAD`. Preview the release
notes with:

```bash
python3 .github/scripts/extract-release-notes.py v1.5.3
```

Commit the version and changelog together and ensure the normal CI run passes. Then create and
push an annotated tag:

```bash
git tag -a v1.4.5 -m "v1.4.5"
git push origin v1.4.5
```

The release workflow validates that the tag, CMake project version, checked-out commit, and built
binary all agree. Publishing also requires a dated, nonempty changelog entry with a version link;
that entry becomes the GitHub release description, including when resuming an existing draft.
It then tests and packages Intel, Apple Silicon, and Universal2 macOS builds;
glibc x86-64 and ARM64 Linux builds; and static musl x86-64 and ARM64 Linux builds. A draft GitHub
release is published only after all seven archives, their checksums, and their provenance
attestations have been created successfully.

Every archive includes `LICENSE` and `THIRD_PARTY_NOTICES`. CMake installs both to
`${CMAKE_INSTALL_DATADIR}/licenses/cjsh` (normally `share/licenses/cjsh` under the install
prefix), including for staged `DESTDIR` packaging. Preserve these notices in downstream
packages. When adding or updating vendored code, review its notices and update
`THIRD_PARTY_NOTICES`; see [the isocline fork notes](cjsh-isocline/README.md).

The **Release Binaries** workflow can also be dispatched manually. Leaving **Publish** disabled
runs a safe release dry run from the selected branch: all seven archives are built, tested,
attested, and retained as workflow artifacts, but no GitHub Release is created. Enabling
**Publish** checks out and strictly validates the requested tag before publishing it. A published
release is never overwritten by the workflow.

## Security

Report suspected vulnerabilities privately using [SECURITY.md](SECURITY.md), rather than
opening a public bug report with reproduction or exploit details.

## License

By contributing to this repository, you agree that your contributions will be licensed under the project's MIT License.
