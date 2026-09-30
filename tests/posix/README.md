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

# POSIX mode regression tests

`test_posix_mode.py` targets POSIX.1-2024 and cjsh's additional strict-language
policy. It specifies output, exit status, and diagnostics independently of an
oracle shell. Every script fixture runs through `--posix` and a temporary `sh`
symlink, using `-c`, script files, stdin, `eval`, and dot sourcing. Invocation tests
also cover option bundles, syntax-only mode, and shared stdin with `read`.

Each check gets an isolated working directory, HOME, environment, and process
group. Timeouts terminate the group. `test_posix_interactive.py` uses PTYs for
interactive recovery, `ignoreeof`, and `notify` behavior, including wait statuses.
Existing startup coverage remains in `tests/core/test_batch2_startup.py`.

```sh
python3 tests/posix/test_posix_mode.py build/release/cjsh
python3 tests/posix/test_posix_interactive.py build/release/cjsh
ctest --test-dir build/release -L posix --output-on-failure
```

Keep portable positive cases separate from extension rejection. POSIX.1-2024
includes `pipefail`, dollar-single-quotes, and `;&`; `;;&` remains an extension.
The XSI `type` utility is supported. The optional User Portability Utilities `vi`
editing mode is outside the implemented profile. Native extensions continue to
be checked by the existing shell suites.
