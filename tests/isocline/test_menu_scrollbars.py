#!/usr/bin/env python3

# test_menu_scrollbars.py
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

import re
import sys

import test_isocline_pty as pty_tests


OPENINGS = {
    "completion": b"entry\t",
    "history": b"\x12entry",
    "palette": pty_tests.ALT_P + b"zzviewport",
    "custom": pty_tests.F3,
}


def check_scrollbars(binary: str) -> None:
    rows, cols = 24, 160

    def screen(output):
        return pty_tests.terminal_screen(output, rows, cols)

    def bar_cells(output):
        return [(row + 1, line[cols - 2]) for row, line in enumerate(screen(output))
                if len(line) >= cols - 1 and line[cols - 2] in "█│"]

    def expect(first, count=8, selected=None):
        def check(output):
            lines = screen(output)
            entries = [int(match.group(1)) for line in lines
                       if (match := re.match(r"[ →>]+entry(\d{3})", line))]
            if entries != list(range(first, first + count)):
                raise AssertionError(f"expected page {first}/{count}, got {lines!r}")
            if selected is not None and not any(
                re.match(rf"[→>] entry{selected:03d}\b", line) for line in lines
            ):
                raise AssertionError(f"expected selection {selected}, got {lines!r}")
            cells = bar_cells(output)
            if len(cells) != 8 or sum(char == "█" for _, char in cells) != 1:
                raise AssertionError(f"expected an eight-row track and one-row thumb: {lines!r}")
        return check

    def click_track(bottom):
        def keys(output):
            cells = bar_cells(output)
            row = cells[-1 if bottom else 0][0]
            # Release over an entry, outside the track. This must not accept that entry.
            return (pty_tests.mouse_left_press(cols - 1, row)
                    + pty_tests.mouse_left_release(4, row))
        return keys

    def click_thumb(output):
        row = next(row for row, char in bar_cells(output) if char == "█")
        return pty_tests.mouse_left_click(cols - 1, row)

    def drag_thumb(bottom):
        def keys(output):
            cells = bar_cells(output)
            row = next(row for row, char in cells if char == "█")
            target = cells[-1][0] + 2 if bottom else 1
            return (pty_tests.mouse_left_press(cols - 1, row)
                    + pty_tests.mouse_left_drag(3, target)
                    + pty_tests.mouse_left_release(3, target))
        return keys

    def observe(kind, suffix, actions):
        return pty_tests.run_resize_case(
            binary, "menu_viewport_" + kind + "_limit" + suffix,
            [("send", OPENINGS[kind]), ("idle", 0.15)] + actions,
            initial_rows=rows, initial_cols=cols, return_after_actions=True,
            respond_to_cursor_queries=True,
        )

    for kind in OPENINGS:
        output = observe(kind, "", [("check", expect(0, selected=0)),
                                   ("send", pty_tests.DOWN * 119), ("idle", 0.15),
                                   ("check", expect(112, selected=119))])
        cells = bar_cells(output)
        if cells[-1][1] != "█" or "\x1b[?1000h" in output:
            raise AssertionError("keyboard scrolling must move the thumb without mouse capture")

        for mode in ("_mouse", "_smart"):
            observe(kind, mode, [
                ("check", expect(0, selected=0)),
                ("send", click_track(True)), ("idle", 0.15),
                ("check", expect(8, selected=11)),
                # Clicking a thumb at a rounded position must not change the scroll offset.
                ("send", click_thumb), ("idle", 0.15),
                ("check", expect(8, selected=11)),
                ("send", click_track(True)), ("idle", 0.15),
                ("check", expect(16, selected=19)),
                ("send", click_track(False)), ("idle", 0.15),
                ("check", expect(8, selected=11)),
                ("send", drag_thumb(True)), ("idle", 0.15),
                ("check", expect(112, selected=115)),
                ("send", drag_thumb(False)), ("idle", 0.15),
                ("check", expect(0, selected=3)),
                ("send", pty_tests.DOWN), ("idle", 0.15),
                ("check", expect(0, selected=4)),
            ])

        # Fitting the entire list removes the scrollbar, including after a resize.
        fit = pty_tests.run_resize_case(
            binary, "menu_viewport_" + kind + "_limit",
            [("send", OPENINGS[kind]), ("idle", 0.15),
             ("resize", (160, cols)), ("send", b"\n"), ("idle", 0.15)],
            initial_rows=rows, initial_cols=cols, return_after_actions=True,
        )
        if any("█" in line or "│" in line for line in pty_tests.terminal_screen(fit, 160, cols)):
            raise AssertionError(f"{kind}: scrollbar must disappear when all items fit")

    observe("custom", "_preview_mouse", [
        ("check", expect(0, count=6, selected=0)),
        ("send", drag_thumb(True)), ("idle", 0.15),
        ("check", expect(112, selected=117)),
    ])

    # A passive completion menu displays the same track without capturing mouse input.
    pty_tests.run_resize_case(
        binary, "menu_viewport_completion_limit_passive",
        [("send", b"entry"), ("idle", 0.15), ("check", expect(0))],
        initial_rows=rows, initial_cols=cols, return_after_actions=True,
    )

    # Filtering to a single match must remove both the track and its hit area.
    filtered = observe("custom", "_mouse", [
        ("send", b"entry119"), ("idle", 0.15),
    ])
    if bar_cells(filtered):
        raise AssertionError("filtering to one result left a stale scrollbar")

    def press_thumb(output):
        row = next(row for row, char in bar_cells(output) if char == "█")
        return pty_tests.mouse_left_press(cols - 1, row)

    # Focus loss, keyboard input, resize and dismissal all end temporary motion capture.
    for ending in (pty_tests.FOCUS_OUT, pty_tests.DOWN, b"\x1b"):
        output = observe("custom", "_mouse", [
            ("send", press_thumb), ("idle", 0.15),
            ("send", ending), ("idle", 0.25),
        ])
        if output.rfind("\x1b[?1002l") < output.rfind("\x1b[?1002h"):
            raise AssertionError("temporary drag reporting leaked after focus/key/cancel")
        if ending == b"\x1b" and bar_cells(output):
            raise AssertionError("dismissal left a scrollbar on the prompt")

    resized = observe("custom", "_mouse", [
        ("send", press_thumb), ("idle", 0.15),
        ("resize", (12, 60)), ("idle", 0.15),
    ])
    if resized.rfind("\x1b[?1002l") < resized.rfind("\x1b[?1002h"):
        raise AssertionError("resizing must end temporary motion capture")
    narrow = pty_tests.terminal_screen(resized, 12, 60)
    if not any(len(line) == 59 and line[-1] in "█│" for line in narrow):
        raise AssertionError(f"scrollbar must follow the terminal width: {narrow!r}")


if __name__ == "__main__":
    check_scrollbars(sys.argv[1])
    print(f"Total tests: {pty_tests.PTY_CASE_COUNT}")
    print(f"Passed: {pty_tests.PTY_CASE_COUNT}")
    print("Failed: 0")
