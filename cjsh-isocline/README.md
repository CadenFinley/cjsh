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

# cjsh's isocline fork

This directory contains a substantially modified fork of
[daanx/isocline](https://github.com/daanx/isocline), used for cjsh's line editor,
terminal handling, history, and completion UI. It is built into the cjsh binary;
users do not install it separately.

The original upstream import commit is not recorded here. Do not infer an exact
upstream revision from the version string in the source. The cjsh Git history is
the revision record for this fork.

## Maintenance

- Treat this as maintained project code: changes need review and tests under
  `tests/isocline/` or the relevant interactive shell suite.
- Review applicable upstream fixes rather than replacing this directory with an
  upstream snapshot; the fork contains cjsh-specific APIs and behavior.
- When importing upstream changes, record the upstream URL and commit in the
  commit message or pull request, and preserve applicable license notices.
- Run the repository lint command and the relevant CTest suites after changes.
- Report vulnerabilities through the root [security policy](../SECURITY.md).

The original MIT notices remain in the source. Distribution notices, including
attribution for the adapted combining-character table, are collected in
[`THIRD_PARTY_NOTICES`](../THIRD_PARTY_NOTICES). Release archives ship that file
alongside `LICENSE`; CMake installations place both under `share/licenses/cjsh`
by default.
