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

# cjsh-isocline <a href="https://github.com/CadenFinley/cjsh/actions/workflows/ci.yml"><img src="https://github.com/CadenFinley/cjsh/actions/workflows/ci.yml/badge.svg" alt="CI"></a> <a href="https://app.codacy.com/gh/CadenFinley/cjsh/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade"><img src="https://app.codacy.com/project/badge/Grade/4e33a26accb6450da43c91c7b8e872e7" alt="Codacy Badge"></a> <a href="https://cadenfinley.github.io/cjsh/"><img src="https://img.shields.io/badge/docs-GitHub%20Pages-blue" alt="Documentation"></a> <img src="https://img.shields.io/github/repo-size/CadenFinley/cjsh" alt="Repo Size">

<p align="center"><strong>Vendored and extended isocline engine for CJ's Shell</strong></p>

This directory contains the `cjsh_isocline` static library used by `cjsh` for interactive terminal input. It is based on isocline and has been significantly adapted for CJ's Shell features such as advanced keybinding hooks, status messaging, prompt-line behaviors, and shell-specific integration paths.

The target is built as ISO C11, compiles with `IC_SEPARATE_OBJS=1`, and exports headers (including a generated compatibility include path) for the shell runtime.

> **WARNING** this fork intentionally diverges from upstream isocline to support cjsh behavior. prefer tagged CJ's Shell releases for stability.

## Build this module through the root project

This module is expected to be built from the repository root so shared build flags, metadata, and threading configuration are applied.

```bash
git clone https://github.com/CadenFinley/cjsh && cd cjsh
cmake --preset release
cmake --build --preset release --parallel
```

## Module layout

- `src/isocline.h` - public API surface consumed by `cjsh`
- `src/core/` - aggregation unit, runtime environment lifecycle, and shared core helpers
- `src/edit/` - readline entry points, interactive editing flow, typeahead, history, and undo
- `src/completion/` - completion and highlighting engine internals
- `src/keybinding/` - keycode tables and keybinding registration/runtime behavior
- `src/terminal/` - terminal rendering, bbcode/attributes, TTY I/O, and Unicode helpers
- `src/utils/` - reusable utilities shared across components

## Custom menus from keybindings

Applications can bind any key to the `runoff` action and open a searchable Isocline menu from the
unhandled-key callback. `ic_show_menu()` returns the selected index from the original item array and
restores the current readline buffer when the menu closes.

```c
static bool handle_custom_key(ic_keycode_t key, void* arg) {
    (void)arg;
    if (key != IC_KEY_F3) return false;

    static const ic_menu_item_t items[] = {
        { "Show status", "inspect the working tree", "git changes" },
        { "Restart service", "restart the background worker", "reload daemon" },
        { "Open logs", "view recent service output", "tail diagnostics" },
    };

    size_t selected = 0;
    if (ic_show_menu("actions: ", items, 3, &selected)) {
        /* Execute the application action identified by selected. */
    }
    return true; /* The F3 key itself was handled, including menu cancellation. */
}

ic_bind_key(IC_KEY_F3, IC_KEY_ACTION_RUNOFF);
ic_set_unhandled_key_handler(handle_custom_key, NULL);
```

Users can type to fuzzy-filter labels, descriptions, and keywords. Up/Down, Ctrl+P/Ctrl+N,
Shift+Up/Down, Enter/Tab, Escape, Alt+C, and menu mouse interactions follow the existing Isocline
menu controls.

Completion menus default to a maximum of 15 visible content rows; history, command palette, and
custom menus default to 30. These limits include expanded previews, and menus shrink to fit the terminal. Each menu has independent setters/getters (limits clamp to 1 through 256):

| Menu | Setter | Getter |
| --- | --- | --- |
| Completion (active and passive) | `ic_set_completion_menu_max_line_count(count)` | `ic_get_completion_menu_max_line_count()` |
| History | `ic_set_history_menu_max_line_count(count)` | `ic_get_history_menu_max_line_count()` |
| Command palette | `ic_set_command_palette_max_line_count(count)` | `ic_get_command_palette_max_line_count()` |
| Custom | `ic_set_custom_menu_max_line_count(count)` | `ic_get_custom_menu_max_line_count()` |

Each setter returns its previous limit. Ctrl+J toggles the open menu between its configured height
and all available terminal space. This does not change the configured limit and resets when the
menu closes. Headers and help text use separate rows. Menus share the multiline scroll margin
configured by `ic_set_multiline_bottom_line_count()`, which defaults to 3 rows around the selection.

## Testing

Run all automated tests through CTest from the repository root:

```bash
ctest --preset release --parallel 4
```

This includes isocline behavior and PTY integration coverage alongside the shell
and other focused suites. Configure the build with Python 3 installed to include
the Python suites. See
[Local Verification](../CONTRIBUTING.md#local-verification) for selecting suites
and repeating failures.

## Upstream and license

This module is distributed under the MIT License as part of CJ's Shell.
It includes work from the original isocline project by Daan Leijen, with extensive cjsh-focused modifications.

**Caden Finley** @ Abilene Christian University
© 2026
