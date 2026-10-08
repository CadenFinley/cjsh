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

## Buffer formatting

Register a formatter with `ic_set_default_formatter(callback, arg)`.
The default mode calls the formatter on Return and explicit formatting requests.
The callback receives the full input, the cursor byte offset, and the registered argument.
Call `ic_set_formatted_input()` inside the callback to supply replacement text and its cursor position.
Isocline copies the text immediately, so the callback can release its result before returning.

This example replaces each tab with one space:

```c
#include <isocline.h>
#include <string.h>

static void format_tabs(ic_format_env_t* fenv, const char* input,
                        size_t cursor_pos, void* arg) {
    (void)arg;
    size_t len = strlen(input);
    char* text = (char*)ic_malloc(len + 1);
    if (text == NULL) {
        return;
    }
    for (size_t i = 0; i <= len; ++i) {
        text[i] = (input[i] == '\t' ? ' ' : input[i]);
    }
    (void)ic_set_formatted_input(fenv, text, cursor_pos);
    ic_free(text);
}

void configure_formatter(void) {
    ic_set_default_formatter(format_tabs, NULL);
    ic_set_format_mode(IC_FORMAT_MODE_REGULAR);
    ic_set_format_delay(0);
}
```

Select the triggers with `ic_set_format_mode(mode)`. The function returns the previous mode.
`ic_get_format_mode()` returns the current mode.

| Mode | Triggers |
| --- | --- |
| `IC_FORMAT_MODE_REGULAR` (default) | Return, the format keybinding, and explicit API requests. |
| `IC_FORMAT_MODE_SMART` | Regular triggers, inserted spaces, inserted newlines, and new visual line wraps. |
| `IC_FORMAT_MODE_EVERY_KEYSTROKE` | Every keystroke, except undo and redo, plus explicit API requests. |
| `IC_FORMAT_MODE_OFF` | None. This mode also blocks the format keybinding and explicit API requests. |

Smart mode detects wraps with the same row layout that the editor displays.
Moving the cursor across an existing wrap does not trigger formatting.

`ic_set_format_delay(milliseconds)` sets a delay for Smart and Every Keystroke modes.
Zero formats immediately after a matching trigger.
A positive delay waits until typing stops for that duration. Later keystrokes restart a pending delay.
Return and explicit requests format immediately in every mode except Off.
`ic_get_format_delay()` returns the current delay.

Call `ic_format_buffer()` for an immediate request, or bind a key with
`ic_bind_key_named("f4", "format-buffer")`.
The command palette also offers **Format Buffer** when a formatter exists and the mode permits formatting.
Pass `NULL` to `ic_set_default_formatter()` to remove the formatter.

Automatic formatting shares the triggering edit's undo step when that edit records an undo state.
Explicit formatting creates one undoable edit. An unchanged result preserves undo and redo history.
Undo and redo skip automatic formatting for that keystroke.
Isocline evaluates pasted text after the paste ends and defers formatting while a menu is open.
Smart mode formats a paste if it adds spaces, newlines, or visual wraps.
Every Keystroke mode formats each paste once. Regular mode waits for Return or an explicit request.
Terminal events do not trigger formatting.

The callback can return without a result to preserve the buffer.
An allocation failure also preserves the buffer. An empty result clears it.
Isocline clamps the result's cursor offset to its length and the preceding UTF-8 character boundary.
The callback must run synchronously and must not change editor state or call readline.

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
