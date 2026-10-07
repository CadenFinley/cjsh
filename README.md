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

# CJ's Shell (cjsh) <a href="https://github.com/CadenFinley/cjsh/actions/workflows/ci.yml"><img src="https://github.com/CadenFinley/cjsh/actions/workflows/ci.yml/badge.svg" alt="CI"></a> <a href="https://app.codacy.com/gh/CadenFinley/cjsh/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade"><img src="https://app.codacy.com/project/badge/Grade/4e33a26accb6450da43c91c7b8e872e7" alt="Codacy Badge"></a> <a href="https://cadenfinley.github.io/cjsh/"><img src="https://img.shields.io/badge/docs-GitHub%20Pages-blue" alt="Documentation"></a> <img src="https://img.shields.io/github/repo-size/CadenFinley/cjsh" alt="Repo Size">

<p align="center"><strong>POSIX-first scripting with a modern interactive shell experience</strong></p>
<p align="center"><img src="docs/cjsh_recording.svg" alt="Terminal recording showing CJ's Shell features"></p>

`cjsh` is a POSIX-first shell with an enhanced interactive layer. It combines standards-oriented scripting behavior with modern shell features such as rich completions, customizable keybindings, syntax highlighting, multiline editing, prompt styling, spell correction, and history search.

The project builds into a single `cjsh` binary and vendors its line-editing dependency, so the shell works out of the box without an external plugin stack or framework.

> `cjsh` is under active development. For the most stable experience, prefer tagged releases or package-manager builds.

## Install

Detailed install and onboarding guides are available in the [documentation site](https://cadenfinley.github.io/cjsh/getting-started/quick-start/).

### Prebuilt binaries

Each GitHub release includes ready-to-run archives for macOS and Linux. macOS artifacts cover
Intel on macOS 12+, Apple Silicon on macOS 15+, and Universal2 on macOS 12+. Linux artifacts cover
x86-64 and ARM64 with either glibc or a fully static musl build. Every release also includes a
`SHA256SUMS` manifest, and its artifacts have GitHub build-provenance attestations.

Download the appropriate archive from the
[latest release](https://github.com/CadenFinley/cjsh/releases/latest), extract it, and place the
included `cjsh` executable somewhere on your `PATH`.

### Homebrew (macOS/Linux)
```bash
brew tap CadenFinley/tap
brew install cjsh
```

### Arch Linux (AUR)
```bash
# Using yay
yay -S cjsh

# Using paru
paru -S cjsh
```

## Build From Source

### Requirements

- C compiler
- C++ compiler
- CMake 3.25 or newer
- Ninja
- Python 3 for parts of the test suite

### Build

```bash
git clone https://github.com/CadenFinley/cjsh && cd cjsh
cmake --preset release
cmake --build --preset release --parallel
```

Run the shell with:

```bash
./build/release/cjsh
```

Install it under a prefix with:

```bash
cmake --install build/release --prefix ~/.local
```

List all configured presets with `cmake --list-presets`.

### Test

Run all automated tests through CTest from the repository root:

```bash
ctest --preset release --parallel 4
```

CTest runs the shell files and focused C, C++, and Python suites, then reports
combined individual-test counts. See [Local Verification](CONTRIBUTING.md#local-verification)
for filtering, serial runs, and repeating failures.

## Documentation

Project documentation is published at [cadenfinley.github.io/cjsh](https://cadenfinley.github.io/cjsh/).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, formatting, testing, and pull request expectations.

## Security

See [SECURITY.md](SECURITY.md) for private vulnerability reporting, supported security-fix
versions, and the security boundaries of the shell. `--secure` is not a sandbox.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
Vendored-code attributions and license terms are collected in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), which ships with release archives and is
installed alongside the project license under `share/licenses/cjsh` by default.
