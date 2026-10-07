#!/usr/bin/env python3

# test_input_scrollbars.py
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


BUFFER = "\n".join(f"input-line-{i:02d}" for i in range(20))


def check_scrollbars(binary: str) -> None:
    rows, cols = 24, 80

    def screen(output):
        return pty_tests.terminal_screen(output, rows, cols)

    def cells(output, column=cols - 2):
        return [(i + 1, line[column]) for i, line in enumerate(screen(output))
                if len(line) > column and line[column] in "█│"]

    def expect(first, count=8, bar=True):
        def check(output):
            lines = screen(output)
            entries = [int(match.group(1)) for line in lines
                       if (match := re.search(r"input-line-(\d{2})", line))]
            if entries != list(range(first, first + count)):
                raise AssertionError(f"expected input page {first}/{count}, got {lines!r}")
            input_rows = [i for i, line in enumerate(lines) if "input-line-" in line]
            if input_rows != list(range(count)):
                raise AssertionError(f"input viewport moved from its screen origin: {lines!r}")
            if any(line.strip() for line in lines[count:]):
                raise AssertionError(f"redraw left content below the input viewport: {lines!r}")
            track = cells(output)
            if bar and (len(track) != count or sum(char == "█" for _, char in track) < 1):
                raise AssertionError(f"expected scrollbar beside every input row: {lines!r}")
            if not bar and track:
                raise AssertionError(f"unexpected scrollbar: {lines!r}")
        return check

    def observe(suffix, actions):
        return pty_tests.run_resize_case(
            binary, "input_scrollbar_" + suffix,
            [("idle", 0.15)] + actions, initial_rows=rows, initial_cols=cols,
            return_after_actions=True, respond_to_cursor_queries=True,
        )

    output = observe("limit", [
        ("check", expect(12)), ("send", pty_tests.CTRL_HOME), ("idle", 0.15),
        ("check", expect(0)), ("send", pty_tests.CTRL_END), ("idle", 0.15),
        ("check", expect(12)),
    ])
    if cells(output)[-1][1] != "█" or "\x1b[?1000h" in output:
        raise AssertionError("keyboard scrolling must move the thumb without capturing the mouse")

    observe("limit_off", [("check", expect(12, bar=False)),
                           ("send", pty_tests.CTRL_HOME), ("idle", 0.15),
                           ("check", expect(0, bar=False))])
    observe("limit", [("send", pty_tests.F3), ("idle", 0.15),
                       ("check", expect(12, bar=False)),
                       ("send", pty_tests.F3), ("idle", 0.15),
                       ("check", expect(12))])

    def wheel(up, row=4, column=10):
        return f"\x1b[<{64 if up else 65};{column};{row}M".encode("ascii")

    def check_capture(output, enabled):
        active = output.rfind("\x1b[?1000h") > output.rfind("\x1b[?1000l")
        if active != enabled:
            raise AssertionError(f"expected mouse capture {'on' if enabled else 'off'}")

    for mode in ("mouse", "smart"):
        for margin in ("", "_margin"):
            output = observe("limit" + margin + "_" + mode, [
                ("send", wheel(True)), ("idle", 0.15), ("check", expect(11)),
                ("send", wheel(True) * 4), ("idle", 0.15), ("check", expect(7)),
                ("send", wheel(False) * 3), ("idle", 0.15), ("check", expect(10)),
                ("send", wheel(False) * 30), ("idle", 0.15), ("check", expect(12)),
                ("send", wheel(True) * 19), ("idle", 0.15), ("check", expect(0)),
            ])
            if "\x1b[?1000l" in output:
                raise AssertionError("scrolling an input viewport released mouse capture")

        observe("limit_" + mode, [
            ("send", pty_tests.F2), ("idle", 0.15),
            ("send", wheel(True) * 4), ("idle", 0.15), ("check", expect(12)),
            ("send", pty_tests.F2), ("idle", 0.15),
            ("send", wheel(True)), ("idle", 0.15), ("check", expect(11)),
        ])

    for mode in ("menu_only", "all_off"):
        output = observe("limit_" + mode, [
            ("send", wheel(True) * 3), ("idle", 0.15), ("check", expect(12)),
        ])
        if "\x1b[?1000h" in output:
            raise AssertionError(f"{mode}: wheel scrolling enabled prompt mouse capture")

    observe("limit_off_mouse", [
        ("send", wheel(True) * 3), ("idle", 0.15), ("check", expect(12, bar=False)),
    ])
    output = observe("limit_off_smart", [
        ("send", wheel(True)), ("idle", 0.15), ("check", expect(12, bar=False)),
    ])
    if "\x1b[?1000l" not in output:
        raise AssertionError("smart mode must hand wheel input to the terminal without a scrollbar")

    output = observe("limit_smart", [
        ("send", wheel(True, row=12)), ("idle", 0.15), ("check", expect(12)),
    ])
    if "\x1b[?1000l" not in output:
        raise AssertionError("smart mode must hand wheel input outside the viewport to the terminal")

    result = pty_tests.run_resize_case(
        binary, "input_scrollbar_limit_margin_mouse", [
            ("send", pty_tests.CTRL_HOME + pty_tests.DOWN * 4 + pty_tests.END), ("idle", 0.15),
            ("send", wheel(False) * 2), ("idle", 0.15), ("check", expect(2)),
            ("send", b"X\r")], initial_rows=rows, initial_cols=cols,
        respond_to_cursor_queries=True,
    )
    if result != BUFFER.replace("input-line-06", "input-line-06X"):
        raise AssertionError("wheel scrolling failed to move the cursor with the viewport")

    result = pty_tests.run_resize_case(
        binary, "input_scrollbar_limit_margin_mouse", [
            ("send", wheel(True)), ("idle", 0.15), ("check", expect(11)),
            ("send", b"X\r")], initial_rows=rows, initial_cols=cols,
        respond_to_cursor_queries=True,
    )
    if result != BUFFER.replace("input-line-18", "input-line-18X"):
        raise AssertionError("wheel scrolling failed to move the cursor up one row")

    for mode in ("mouse", "smart"):
        # At either scrollbar limit, wheel events must still move the editing cursor.
        for start, direction, expected_row, first in (
            (pty_tests.CTRL_HOME + pty_tests.DOWN * 4 + pty_tests.END, True, 2, 0),
            (pty_tests.CTRL_END + pty_tests.LEFT + pty_tests.UP * 4 + pty_tests.END,
             False, 17, 12),
        ):
            result = pty_tests.run_resize_case(
                binary, "input_scrollbar_limit_" + mode, [
                    ("send", start), ("idle", 0.15), ("check", expect(first)),
                    ("send", wheel(direction) * 2), ("idle", 0.15),
                    ("check", expect(first)), ("send", b"X\r")],
                initial_rows=rows, initial_cols=cols, respond_to_cursor_queries=True,
            )
            line = f"input-line-{expected_row:02d}"
            if result != BUFFER.replace(line, line + "X"):
                raise AssertionError(f"{mode}: a clamped scrollbar prevented cursor movement")

        observe("limit_" + mode, [
            ("send", wheel(True) * 19), ("idle", 0.15), ("check", expect(0)),
            ("check", lambda output: check_capture(output, True)),
            ("send", wheel(True)), ("idle", 0.15), ("check", expect(0)),
            ("check", lambda output: check_capture(output, mode != "smart")),
            ("send", pty_tests.RIGHT), ("idle", 0.15),
            ("check", lambda output: check_capture(output, True)),
            ("send", wheel(False)), ("idle", 0.15), ("check", expect(1)),
        ])

    # Smart handoff leaves the cursor on the first line and never navigates history.
    result = pty_tests.run_resize_case(
        binary, "input_scrollbar_limit_smart", [
            ("send", wheel(True) * 25), ("idle", 0.15), ("check", expect(0)),
            ("send", b"X\r")], initial_rows=rows, initial_cols=cols,
        respond_to_cursor_queries=True,
    )
    if result != BUFFER.replace("input-line-00", "input-line-00X"):
        raise AssertionError("smart handoff moved the cursor or navigated history")

    def click_track(bottom):
        def keys(output):
            row = cells(output)[-1 if bottom else 0][0]
            return (pty_tests.mouse_left_press(cols - 1, row)
                    + pty_tests.mouse_left_release(4, row))
        return keys

    def drag_thumb(bottom):
        def keys(output):
            track = cells(output)
            row = next(row for row, char in track if char == "█")
            target = track[-1][0] + 2 if bottom else 1
            return (pty_tests.mouse_left_press(cols - 1, row)
                    + pty_tests.mouse_left_drag(3, target)
                    + pty_tests.mouse_left_release(3, target))
        return keys

    for mode in ("mouse", "smart"):
        observe("limit_" + mode, [
            ("send", pty_tests.CTRL_HOME), ("idle", 0.15), ("check", expect(0)),
            ("send", click_track(True)), ("idle", 0.15), ("check", expect(8)),
            ("send", drag_thumb(True)), ("idle", 0.15), ("check", expect(12)),
            ("send", drag_thumb(False)), ("idle", 0.15), ("check", expect(0)),
        ])

        # Moving the cursor inside a page must not move the redraw origin when the thumb moves.
        for cursor_offset in (1, 4, 6):
            observe("limit_" + mode, [
                ("send", pty_tests.CTRL_HOME + pty_tests.DOWN * cursor_offset), ("idle", 0.15),
                ("check", expect(0)),
                ("send", drag_thumb(True)), ("idle", 0.15), ("check", expect(12)),
                ("send", drag_thumb(False)), ("idle", 0.15), ("check", expect(0)),
                ("send", click_track(True)), ("idle", 0.15), ("check", expect(8)),
                ("send", click_track(False)), ("idle", 0.15), ("check", expect(0)),
            ])

        observe("limit_margin_" + mode, [
            ("send", pty_tests.CTRL_HOME + pty_tests.DOWN * 4), ("idle", 0.15),
            ("check", expect(0)),
            ("send", drag_thumb(True)), ("idle", 0.15), ("check", expect(12)),
            ("send", drag_thumb(False)), ("idle", 0.15), ("check", expect(0)),
        ])

    def drag_one_row(output):
        row = next(row for row, char in cells(output) if char == "█")
        return (pty_tests.mouse_left_press(cols - 1, row)
                + pty_tests.mouse_left_drag(cols - 1, row + 1)
                + pty_tests.mouse_left_release(cols - 1, row + 1))

    # A thumb move can change the viewport without changing the cursor's logical row.
    observe("limit_mouse", [
        ("send", pty_tests.CTRL_HOME + pty_tests.DOWN * 2), ("idle", 0.15),
        ("send", drag_one_row), ("idle", 0.15), ("check", expect(2)),
    ])

    def press_thumb(output):
        row = next(row for row, char in cells(output) if char == "█")
        return pty_tests.mouse_left_press(cols - 1, row)

    def finish_drag(bottom):
        def keys(output):
            target = cells(output)[-1][0] + 2 if bottom else 1
            return (pty_tests.mouse_left_drag(3, target)
                    + pty_tests.mouse_left_release(3, target))
        return keys

    console_rows = []

    def remember_console(output):
        lines = screen(output)
        console_rows[:] = lines[:rows - 8]
        if not all(line.startswith("CONSOLE-LINE-") for line in console_rows):
            raise AssertionError(f"fixture failed to put the prompt at the bottom: {lines!r}")

    def expect_at_bottom(first):
        def check(output):
            lines = screen(output)
            if lines[:rows - 8] != console_rows:
                raise AssertionError(f"scrollbar redraw overwrote console output: {lines!r}")
            entries = [int(match.group(1)) for line in lines[rows - 8:]
                       if (match := re.search(r"input-line-(\d{2})", line))]
            if entries != list(range(first, first + 8)) or len(cells(output)) != 8:
                raise AssertionError(f"scrollbar redraw shifted the bottom viewport: {lines!r}")
        return check

    for mode in ("mouse", "smart"):
        # The first mouse input may already contain several wheel events at the terminal bottom.
        observe("limit_screen_bottom_margin_" + mode, [
            ("check", remember_console), ("check", expect_at_bottom(12)),
            ("send", wheel(True, row=24) * 4), ("idle", 0.15),
            ("check", expect_at_bottom(8)),
            ("send", wheel(False, row=24) * 4), ("idle", 0.15),
            ("check", expect_at_bottom(12)),
        ])

    observe("limit_screen_bottom_margin_mouse", [
        ("send", pty_tests.CTRL_HOME + pty_tests.DOWN * 4), ("idle", 0.15),
        ("check", remember_console), ("check", expect_at_bottom(0)),
        ("send", press_thumb), ("wait", b"\x1b[?1002h"),
        ("send", finish_drag(True)), ("idle", 0.15), ("check", expect_at_bottom(12)),
        ("send", press_thumb), ("wait", b"\x1b[?1002h"),
        ("send", finish_drag(False)), ("idle", 0.15), ("check", expect_at_bottom(0)),
        ("send", wheel(False, row=24) * 10), ("idle", 0.15), ("check", expect_at_bottom(10)),
        ("send", wheel(True, row=24) * 10), ("idle", 0.15), ("check", expect_at_bottom(0)),
    ])

    def expect_no_old_tail(output):
        expect(12)(output)
        if "LONG-" in "\n".join(screen(output)):
            raise AssertionError(f"short rows retained old input text: {screen(output)!r}")

    observe("limit_uneven_mouse", [
        ("send", pty_tests.CTRL_HOME), ("idle", 0.15),
        ("send", drag_thumb(True)), ("idle", 0.15), ("check", expect_no_old_tail),
    ])

    # Temporary motion capture ends on focus loss, keyboard input, disable, and resize.
    for ending in (b"\x1b[O", pty_tests.LEFT, pty_tests.F3):
        output = observe("limit_mouse", [
            ("send", press_thumb), ("wait", b"\x1b[?1004h"), ("wait", b"\x1b[?1002h"),
            ("send", ending), ("wait", b"\x1b[?1002l"), ("idle", 0.15),
        ])
        if output.rfind("\x1b[?1002l") < output.rfind("\x1b[?1002h"):
            raise AssertionError("input scrollbar left drag reporting enabled")
        if output.rfind("\x1b[?1004l") < output.rfind("\x1b[?1004h"):
            raise AssertionError("input scrollbar left temporary focus reporting enabled")
        if ending == pty_tests.F3 and cells(output):
            raise AssertionError("disabling a pressed scrollbar left its track visible")

    pty_tests.run_resize_case(
        binary, "input_scrollbar_limit_mouse", [("idle", 0.15),
        ("send", press_thumb), ("wait", b"\x1b[?1002h"),
        ("resize", (12, 60)), ("send", pty_tests.mouse_left_drag(59, 3)),
        ("wait", b"\x1b[?1002l")], initial_rows=rows, initial_cols=cols,
        return_after_actions=True, respond_to_cursor_queries=True,
    )

    result = pty_tests.run_resize_case(
        binary, "input_scrollbar_limit_mouse", [("send", pty_tests.CTRL_HOME),
        ("idle", 0.15), ("send", click_track(True)), ("idle", 0.15),
        ("send", b"X\r")], initial_rows=rows, initial_cols=cols,
        respond_to_cursor_queries=True,
    )
    if result != BUFFER.replace("input-line-08", "Xinput-line-08"):
        raise AssertionError("editing after a scrollbar click changed hidden input rows")

    # Deleting hidden rows removes the track and clears rows from the old viewport.
    observe("limit", [("send", pty_tests.CTRL_HOME), ("idle", 0.15),
                       ("send", (b"\x0b\x1b[3~") * 15), ("idle", 0.15),
                       ("check", expect(15, count=5, bar=False))])

    # Terminal height and prompt prefixes can clip input even with a generous configured limit.
    for suffix, count in (("terminal", 6), ("terminal_prefix", 4)):
        output = pty_tests.run_resize_case(
            binary, "input_scrollbar_" + suffix, [("idle", 0.15)],
            initial_rows=6, initial_cols=cols, return_after_actions=True,
        )
        lines = pty_tests.terminal_screen(output, 6, cols)
        track = [line for line in lines if len(line) == cols - 1 and line[-1] in "█│"]
        if len(track) != count:
            raise AssertionError(f"terminal-sized input scrollbar has wrong height: {lines!r}")

    for marker, margin in (("wide_marker", 2), ("marker_off", 1)):
        output = observe("limit_" + marker, [])
        if len(cells(output, cols - margin - 1)) != 8:
            raise AssertionError(f"{marker}: scrollbar moved off the input rows")

    output = observe("limit_right", [])
    if not any("[12:34:56]" in line and line.endswith("█") for line in screen(output)):
        raise AssertionError(f"inline right prompt overlaps the scrollbar: {screen(output)!r}")

    # Resize removes a scrollbar when every row fits and restores it when rows become hidden.
    resized = pty_tests.run_resize_case(
        binary, "input_scrollbar_terminal", [("idle", 0.15),
        ("resize", (6, 40)), ("send", pty_tests.LEFT), ("idle", 0.15)],
        initial_rows=rows, initial_cols=cols, return_after_actions=True,
    )
    if not all(len(line) == 39 and line[-1] in "█│"
               for line in pty_tests.terminal_screen(resized, 6, 40)):
        raise AssertionError("input scrollbar failed to follow resized terminal dimensions")
    fit = pty_tests.run_resize_case(
        binary, "input_scrollbar_terminal", [("idle", 0.15),
        ("resize", (rows, cols)), ("send", pty_tests.LEFT), ("idle", 0.15)],
        initial_rows=6, initial_cols=cols, return_after_actions=True,
    )
    if cells(fit):
        raise AssertionError("fitting all input rows left a stale scrollbar")

    # Wrapped input also uses a row-based thumb and preserves the complete submitted buffer.
    typed = b"x" * 180
    result, output = pty_tests.run_case(
        binary, "input_scrollbar_limit", typed + b"\r",
        capture_output=True, initial_rows=rows, initial_cols=40,
    )
    if result != BUFFER + typed.decode() or "█" not in output:
        raise AssertionError("wrapped input lost content or failed to render its scrollbar")
    final = output.split("[IC_RESULT_BEGIN]", 1)[0]
    if any("█" in line or "│" in line for line in pty_tests.terminal_screen(final, rows, 40)):
        raise AssertionError("submission left a scrollbar in the terminal")

    result = pty_tests.run_case_timed(
        binary, "input_scrollbar_limit_mouse",
        [pty_tests.CTRL_HOME, pty_tests.END + pty_tests.DOWN * 10 + b"X\r"],
    )
    if result != BUFFER.replace("input-line-10", "input-line-10X"):
        raise AssertionError(f"scrolling changed the input cursor or content: {result!r}")


if __name__ == "__main__":
    check_scrollbars(sys.argv[1])
    print(f"All {pty_tests.PTY_CASE_COUNT} input scrollbar tests passed")
