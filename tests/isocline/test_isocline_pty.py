#!/usr/bin/env python3

# test_isocline_pty.py
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

import os
import platform
import pty
import re
import select
import signal
import struct
import sys
import termios
import time
import fcntl


RESULT_RE = re.compile(r"\[IC_RESULT_BEGIN\](.*?)\[IC_RESULT_END\]", re.S)
ANSI_CSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
ANSI_OSC_RE = re.compile(r"\x1b\].*?(?:\x07|\x1b\\)", re.S)
PROMPT_GUARD_RE = re.compile(r"[%#][ ]+pty> ")
PROMPT_LINE_RE = re.compile(r"(?m)^pty> ")
PTY_CASE_COUNT = 0
IS_DARWIN = platform.system() == "Darwin"
READLINE_STEP_MARKER = b"[IC_READLINE_STEP_DONE]"
TYPEAHEAD_CAPTURE_READY_MARKER = b"[IC_TYPEAHEAD_CAPTURE_READY]"

LEFT = b"\x1b[D"
RIGHT = b"\x1b[C"
UP = b"\x1b[A"
DOWN = b"\x1b[B"
HOME = b"\x1b[H"
END = b"\x1b[F"
PAGEUP = b"\x1b[5~"
PAGEDOWN = b"\x1b[6~"
CTRL_HOME = b"\x1b[1;5H"
CTRL_END = b"\x1b[1;5F"
SHIFT_HOME = b"\x1b[1;2H"
SHIFT_END = b"\x1b[1;2F"
SHIFT_UP = b"\x1b[1;2A"
SHIFT_DOWN = b"\x1b[1;2B"
SHIFT_TAB = b"\x1b[Z"
F1 = b"\x1bOP"
F2 = b"\x1bOQ"
F3 = b"\x1bOR"
FOCUS_IN = b"\x1b[I"
ALT_LT = b"\x1b<"
ALT_GT = b"\x1b>"
ALT_P = b"\x1bp"
ALT_S = b"\x1bs"
ALT_DELETE = b"\x1b[3;3~"
CTRL_ENTER = b"\x1b[13;5u"
WORD_PREV = b"\x1b[1;2D" if IS_DARWIN else b"\x1b[1;5D"
WORD_NEXT = b"\x1b[1;2C" if IS_DARWIN else b"\x1b[1;5C"


def mouse_left_click(column: int, row: int) -> bytes:
    press = f"\x1b[<0;{column};{row}M".encode("ascii")
    release = f"\x1b[<0;{column};{row}m".encode("ascii")
    return press + release


def mouse_left_press(column: int, row: int) -> bytes:
    return f"\x1b[<0;{column};{row}M".encode("ascii")


def mouse_left_drag(column: int, row: int) -> bytes:
    return f"\x1b[<32;{column};{row}M".encode("ascii")


def mouse_left_release(column: int, row: int) -> bytes:
    return f"\x1b[<0;{column};{row}m".encode("ascii")


def assert_smart_mouse_capture_handoff(output: str, scenario: str) -> None:
    enable = "\x1b[?1000h\x1b[?1006h\x1b[?1002h"
    disable = "\x1b[?1002l\x1b[?1000l\x1b[?1006l"
    initial_enable = output.find(enable)
    selection_disable = output.find(disable, initial_enable + len(enable))
    resume = output.find(enable, selection_disable + len(disable))
    if min(initial_enable, selection_disable, resume) < 0:
        raise AssertionError(
            f"{scenario} should suspend capture for selection and resume on release/key/focus input: "
            f"output={output!r}"
        )
    during_selection = output[selection_disable + len(disable) : resume]
    if during_selection:
        raise AssertionError(
            f"{scenario} must preserve the display while selecting: {during_selection!r}"
        )
    if output.rfind(disable) < output.rfind(enable):
        raise AssertionError(f"{scenario} must disable motion reporting when readline ends")
    if output.rfind("\x1b[?1006l") < output.rfind(enable):
        raise AssertionError(f"{scenario} must disable SGR reporting when readline ends")


def assert_smart_mouse_selection_suspends(
    binary: str, scenario: str, column: int, row: int, expected: str
) -> None:
    result, output = run_case(
        binary,
        scenario,
        mouse_left_press(column, row) + b"\r",
        capture_output=True,
    )
    if result != expected:
        raise AssertionError(f"{scenario} expected {expected!r}, got {result!r}")

    assert_smart_mouse_capture_handoff(output, scenario)


def assert_smart_mouse_drag_cases(binary: str) -> None:
    press = mouse_left_press(6, 1)
    drag = mouse_left_drag(7, 1)
    release = mouse_left_release(7, 1)
    for label, keys, expected in [
        ("drag_before_release", press + drag + b"X\r", "abcX"),
        ("drag_queued_events", press + drag + mouse_left_drag(8, 1) + release + b"X\r", "abcX"),
        ("drag_release_fallback", press + release + b"X\r", "abcX"),
        ("drag_backward", mouse_left_press(7, 1) + mouse_left_drag(6, 1) + b"X\r", "abcX"),
        ("drag_between_rows", press + mouse_left_drag(6, 2) + b"X\r", "abcX"),
        ("drag_without_press", drag + b"X\r", "abcX"),
        ("drag_with_modifiers", press + b"\x1b[<52;7;1M" + b"X\r", "abcX"),
        ("drag_legacy", press + b"\x1b[M@'!" + b"X\r", "abcX"),
        (
            "drag_focus_resume_without_release",
            press + drag + FOCUS_IN + mouse_left_click(6, 1) + b"X\r",
            "Xabc",
        ),
        ("drag_interrupt_cleanup", press + drag + b"\x03", "<CTRL+C>"),
        (
            "drag_release_resume_click",
            press + drag + release + mouse_left_click(6, 1) + b"X\r",
            "Xabc",
        ),
        (
            "drag_duplicate_release",
            press + drag + release + release + b"X\r",
            "abcX",
        ),
        (
            "drag_release_ignores_trailing_motion",
            press + drag + release + mouse_left_drag(8, 1) + b"X\r",
            "abcX",
        ),
        (
            "drag_legacy_release_resume",
            press + drag + b"\x1b[M#'!" + mouse_left_click(6, 1) + b"X\r",
            "Xabc",
        ),
    ]:
        result, output = run_case(binary, "smart_mouse_input_click", keys, capture_output=True)
        if result != expected:
            raise AssertionError(f"{label} expected {expected!r}, got {result!r}")
        assert_smart_mouse_capture_handoff(output, label)

    # A click or a report within the original cell should still position the cursor.
    # Other buttons and passive motion must not be interpreted as left dragging.
    for label, keys, expected in [
        ("click", mouse_left_click(6, 1) + b"X\r", "Xabc"),
        (
            "same_cell_motion",
            press + mouse_left_drag(6, 1) + mouse_left_release(6, 1) + b"X\r",
            "Xabc",
        ),
        ("other_motion", b"\x1b[<34;7;1M\x1b[<35;7;1M" + b"X\r", "abcX"),
        ("stale_press", press + b"X" + release + b"\r", "abcX"),
    ]:
        result, output = run_case(binary, "smart_mouse_input_click", keys, capture_output=True)
        if result != expected:
            raise AssertionError(f"{label} expected {expected!r}, got {result!r}")
        if output.count("\x1b[?1002h") != 1:
            raise AssertionError(f"{label} must keep mouse capture enabled: {output!r}")

    result, output = run_case(
        binary, "simple_mouse_input_click", press + release + b"X\r", capture_output=True
    )
    if result != "aXbc" or "\x1b[?1002h" in output:
        raise AssertionError(f"simple mode must retain click-only reporting: {result!r}, {output!r}")

    for menu_keys in [b"s\t", b"s\t" + PAGEDOWN]:
        result, output = run_case(
            binary,
            "completion_many_menu_smart",
            menu_keys
            + mouse_left_press(6, 4)
            + mouse_left_drag(7, 4)
            + mouse_left_release(7, 4)
            + b"X\r",
            capture_output=True,
        )
        if result != "sX":
            raise AssertionError(f"dragging a completion must not accept it: {result!r}")
        assert_smart_mouse_capture_handoff(output, "completion_menu_drag")


def assert_menu_mouse_suspends_until_focus(output: str, scenario: str) -> None:
    enable = "\x1b[?1000h\x1b[?1006h"
    disable = "\x1b[?1000l\x1b[?1006l"
    initial_enable = output.find(enable)
    outside_click_disable = output.find(disable, initial_enable + len(enable))
    focus_resume = output.find(enable, outside_click_disable + len(disable))
    if min(initial_enable, outside_click_disable, focus_resume) < 0:
        raise AssertionError(
            f"{scenario} should suspend menu capture outside its items and resume on focus-in: "
            f"output={output!r}"
        )


def normalize_terminal_output(text: str) -> str:
    normalized = text.replace("\r", "")
    normalized = ANSI_OSC_RE.sub("", normalized)
    normalized = ANSI_CSI_RE.sub("", normalized)
    return normalized


def assert_completion_footer_spacing(output: str, rows: int, cols: int, footer: str) -> None:
    screen = terminal_screen(output, rows, cols)
    footer_row = next((i for i, line in enumerate(screen) if footer in line), -1)
    if footer_row < 1 or not screen[footer_row - 1].strip():
        raise AssertionError(
            f"completion footer should directly follow the list or scroll hint: {screen!r}"
        )


def terminal_state(output: str, rows: int, cols: int) -> tuple[list[str], tuple[int, int]]:
    """Replay the cursor/erase controls used by these single-column menu fixtures."""
    cells = [[" "] * cols for _ in range(rows)]
    row = col = 0
    saved_cursor = (0, 0)

    def linefeed() -> None:
        nonlocal row
        row += 1
        if row == rows:
            cells.pop(0)
            cells.append([" "] * cols)
            row -= 1

    output = ANSI_OSC_RE.sub("", output)
    tokens = re.finditer(r"\x1b\[[0-?]*[ -/]*[@-~]|\x1b.|[^\x1b]", output, re.S)
    for match in tokens:
        token = match.group()
        if token.startswith("\x1b["):
            command = token[-1]
            params = token[2:-1]
            if command in "mn" or (params.startswith("?") and command in "hl"):
                continue  # Styling, queries, and input modes do not change cells.
            values = [int(value or "0") for value in params.split(";")]
            amount = values[0] or 1
            if command == "A":
                row = max(0, row - amount)
            elif command == "B":
                row = min(rows - 1, row + amount)
            elif command == "C":
                col = min(cols - 1, col + amount)
            elif command == "D":
                col = max(0, col - amount)
            elif command == "G":
                col = min(cols - 1, amount - 1)
            elif command in "Hf":
                row = min(rows - 1, amount - 1)
                col = min(cols - 1, (values[1] or 1) - 1) if len(values) > 1 else 0
            elif command == "K":
                start = 0 if values[0] in (1, 2) else min(col, cols - 1)
                end = min(col + 1, cols) if values[0] == 1 else cols
                cells[row][start:end] = [" "] * (end - start)
            elif command == "J":
                for y in range(rows):
                    for x in range(cols):
                        if (
                            values[0] == 2
                            or (values[0] == 0 and (y, x) >= (row, col))
                            or (values[0] == 1 and (y, x) <= (row, col))
                        ):
                            cells[y][x] = " "
            elif command == "s":
                saved_cursor = (row, col)
            elif command == "u":
                row, col = saved_cursor
            else:
                raise AssertionError(
                    f"unhandled terminal control in menu fixture: {token!r}"
                )
        elif token == "\r":
            col = 0
        elif token == "\n":
            linefeed()
        elif token == "\b":
            col = max(0, col - 1)
        elif token == "\t":
            col = min(cols - 1, (col // 8 + 1) * 8)
        elif token == "\x07":
            continue
        elif token.startswith("\x1b"):
            raise AssertionError(
                f"unhandled terminal escape in menu fixture: {token!r}"
            )
        elif token >= " ":
            if col == cols:
                col = 0
                linefeed()
            cells[row][col] = token
            col += 1
    return ["".join(line).rstrip() for line in cells], (row, min(col, cols - 1))


def terminal_screen(output: str, rows: int, cols: int) -> list[str]:
    return terminal_state(output, rows, cols)[0]


def count_prompt_lines(output_text: str) -> int:
    return len(PROMPT_LINE_RE.findall(normalize_terminal_output(output_text)))


def read_pending_output(fd: int, output: bytearray) -> bool:
    received = False
    while True:
        try:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            output.extend(chunk)
            received = True
        except BlockingIOError:
            break
        except OSError:
            break
    return received


def drain_remaining_output(
    fd: int,
    output: bytearray,
    idle_timeout_s: float = 0.1,
    poll_interval_s: float = 0.01,
) -> None:
    # PTYs can still have unread output queued after the child has exited.
    idle_deadline = time.monotonic() + idle_timeout_s
    while time.monotonic() < idle_deadline:
        if read_pending_output(fd, output):
            idle_deadline = time.monotonic() + idle_timeout_s
            continue
        time.sleep(poll_interval_s)


def assert_prompt_guard_marker(
    scenario: str, output_text: str, expect_marker: bool
) -> None:
    normalized = normalize_terminal_output(output_text)
    marker_count = len(PROMPT_GUARD_RE.findall(normalized))
    expected_count = 1 if expect_marker else 0
    if marker_count != expected_count:
        raise AssertionError(
            f"{scenario} expected marker_count={expected_count}, got {marker_count}, "
            f"normalized_output={normalized!r}"
        )


def assert_case(
    binary: str, label: str, scenario: str, key_bytes: bytes, expected: str
) -> None:
    actual = run_case(binary, scenario, key_bytes)
    if actual != expected:
        raise AssertionError(f"{label} expected {expected!r}, got {actual!r}")


def terminal_check_ready(check):
    def ready(output: str) -> bool:
        try:
            check(output)
        except AssertionError:
            return False
        return True

    return ready


def completion_menu_closed_ready(expected_input: str):
    def ready(output: str) -> bool:
        screen = terminal_screen(output, 24, 100)
        return (
            screen[0].rstrip() == f"pty> {expected_input}".rstrip()
            and not any(line.strip() for line in screen[1:])
        )

    return ready


def assert_completion_auto_menu_cases(binary: str) -> None:
    wheel_down = b"\x1b[<65;1;1M"
    click_second = mouse_left_click(5, 4)
    for label, scenario, keys, expected in [
        ("passive_enter", "completion_auto_menu", b"s\r", "s"),
        ("passive_digits", "completion_auto_menu", b"s02\r", "s02"),
        ("passive_right_accepts", "completion_auto_menu", b"s" + RIGHT + b"\r", "s01"),
        ("passive_left_edits", "completion_auto_menu", b"s" + LEFT + b"X\r", "Xs"),
        ("passive_end", "completion_auto_menu", b"s" + END + b"\r", "s"),
        ("passive_ctrl_f", "completion_auto_menu", b"s\x06\r", "s"),
        ("passive_down_activates", "completion_auto_menu", b"s" + DOWN + b"\r\r", "s01"),
        ("passive_up_activates", "completion_auto_menu", b"s" + UP + b"\r\r", "s01"),
        ("passive_down_then_navigate", "completion_auto_menu", b"s" + DOWN + DOWN + b"\r\r", "s02"),
        ("active_left_edits", "completion_auto_menu", b"s" + DOWN + LEFT + b"X\r", "Xs"),
        ("passive_wheel", "completion_auto_menu", b"s" + wheel_down + b"\r", "s"),
        ("passive_click_disabled", "completion_auto_menu", b"s" + click_second + b"\r", "s"),
        ("passive_click_activates", "completion_auto_menu_mouse", b"s" + click_second + b"\r\r", "s02"),
        ("passive_single", "completion_auto_menu_single_hints", b"hel\r", "hel"),
        ("passive_spell", "completion_auto_menu_spell", b"hlelo\r", "hlelo"),
        ("first_tab_selects_first", "completion_auto_menu", b"s\t\r\r", "s01"),
        ("active_navigation", "completion_auto_menu", b"s\t" + DOWN + b"\r\r", "s02"),
        ("active_wheel", "completion_auto_menu", b"s\t" + wheel_down + b"\r\r", "s02"),
        ("active_click", "completion_auto_menu", b"s\t" + click_second + b"\r", "s02"),
        ("first_tab_single", "completion_auto_menu_single", b"hel\t\r", "hello"),
        ("first_tab_single_then_type", "completion_auto_menu_single", b"hel\t!\r", "hello!"),
        ("first_tab_single_undo", "completion_auto_menu_single", b"hel\t\x1f\r", "hel"),
        ("right_single", "completion_auto_menu_single", b"hel" + RIGHT + b"\r", "hello"),
        ("right_single_undo", "completion_auto_menu_single", b"hel" + RIGHT + b"\x1f\r", "hel"),
        ("first_tab_spell", "completion_auto_menu_spell", b"hlelo\t\r", "hello"),
        ("second_tab_single", "completion_auto_menu_single", b"hel\t\t\r", "hello"),
        ("active_filter", "completion_auto_menu", b"s\t02\r\r", "s02"),
        ("active_single_tab", "completion_auto_menu", b"s\t02\t\r", "s02"),
        ("passive_backspace", "completion_auto_menu", b"s02\x7f\t\r\r", "s01"),
        ("passive_delete", "completion_auto_menu", b"s02" + LEFT + b"\x1b[3~\t\r\r", "s01"),
        ("no_match_recovery", "completion_auto_menu", b"sx\x7f\t\r\r", "s01"),
        ("clear_buffer", "completion_auto_menu", b"s\x15\r", ""),
        ("undo_does_not_apply_preview", "completion_auto_menu", b"s0\x1f\r", "s"),
        ("passive_paste", "completion_auto_menu", b"\x1b[200~s02\x1b[201~\r", "s02"),
        ("passive_interrupt", "completion_auto_menu", b"s\x03", "<CTRL+C>"),
    ]:
        assert_case(binary, label, scenario, keys, expected)

    for scenario, prefix, footer in [
        ("completion_auto_menu", b"s", "tab:activate completions"),
        ("completion_auto_menu_single_hints", b"hel", "tab:complete"),
        ("completion_auto_menu_dual", b"pla", "tab:activate completions"),
    ]:
        result, output = run_case(binary, scenario, prefix + b"\r", capture_output=True)
        if result != prefix.decode() or footer not in output:
            raise AssertionError(f"{scenario} should show passive suggestions without Tab: {output!r}")
        if "→" in output or "enter/right:accept" in output or "\x1b[?1000h" in output:
            raise AssertionError(f"{scenario} must not select, preview, or capture the mouse: {output!r}")

    _, output = run_case(binary, "completion_auto_menu_off", b"s\r", capture_output=True)
    if "tab:activate completions" in output or "Completions" in output:
        raise AssertionError("disabled automatic menu must not open while typing")

    _, output = run_case(binary, "completion_auto_menu_limit", b"s\r", capture_output=True)
    if "(9 more below)" not in output or "s03" not in output or "s04" in output:
        raise AssertionError(f"passive menu must respect its completion row limit: {output!r}")

    def check_passive_height(count: int):
        def check(output: str) -> None:
            screen = "\n".join(terminal_screen(output, 24, 100))
            entries = re.findall(r"^  s\d{2}\b", screen, re.M)
            if "Completions" not in screen or len(entries) != count:
                raise AssertionError(f"expected {count} passive content rows: {screen!r}")
            if count < 12 and f"({12 - count} more below)" not in screen:
                raise AssertionError(f"passive menu should show its hidden item count: {screen!r}")
            if count == 12 and "more below" in screen:
                raise AssertionError(f"fitting all items should clear the scroll hint: {screen!r}")
            if "pty> s" not in screen or "→" in screen or "ctrl+j:resize" not in screen:
                raise AssertionError(f"Ctrl+J must not activate or edit the passive menu: {screen!r}")
        return check

    def passive_height_ready(count: int):
        return terminal_check_ready(check_passive_height(count))

    # A quiet PTY is not necessarily a finished redraw: macOS CI can pause the
    # driver for longer than the idle interval, even just after the first prompt.
    # Wait for the full expected screen before settling and asserting it. The
    # resize runner's deadline still fails missing or incorrect menu states.
    assert_resize_case(
        binary, "passive_height_toggle", "completion_auto_menu_limit",
        [("send", b"s"), ("wait_until", passive_height_ready(3)),
         ("idle", 0.1), ("check", check_passive_height(3)),
         ("send", b"\n"), ("wait_until", passive_height_ready(12)),
         ("idle", 0.1), ("check", check_passive_height(12)),
         ("send", b"\n"), ("wait_until", passive_height_ready(3)),
         ("idle", 0.1), ("check", check_passive_height(3)),
         ("send", b"\n"), ("wait_until", passive_height_ready(12)),
         ("idle", 0.1), ("send", b"\x1b"),
         ("wait_until", completion_menu_closed_ready("s")),
         ("send", b"x\x7f"), ("wait_until", passive_height_ready(3)),
         ("idle", 0.1), ("check", check_passive_height(3)),
         ("send", b"\r")], "s", initial_cols=100,
    )

    def check_passive(expected_input: str, count: int):
        def check(output: str) -> None:
            rows = terminal_screen(output, 24, 100)
            screen = "\n".join(rows)
            if rows[0].rstrip() != f"pty> {expected_input}".rstrip() or "→" in screen:
                raise AssertionError(f"passive menu must preserve input and clear selection: {screen!r}")
            if count > 0:
                entries = re.findall(r"^  (?:s\d{2}|hello)\b", screen, re.M)
                footer = "tab:complete" if count == 1 else "tab:activate completions"
                if footer not in screen or len(entries) != count:
                    raise AssertionError(f"expected {count} live passive completions: {screen!r}")
                assert_completion_footer_spacing(output, 24, 100, footer)
            elif "Completions" in screen:
                raise AssertionError(f"empty/no-match input must remove the menu: {screen!r}")
        return check

    def passive_ready(expected_input: str, count: int):
        return terminal_check_ready(check_passive(expected_input, count))

    assert_resize_case(
        binary, "live_passive_filtering", "completion_auto_menu",
        [("send", b"s"), ("wait_until", passive_ready("s", 12)),
         ("idle", 0.1), ("check", check_passive("s", 12)),
         ("send", b"02"), ("wait_until", passive_ready("s02", 1)),
         ("idle", 0.1), ("check", check_passive("s02", 1)),
         ("send", b"x"), ("wait_until", passive_ready("s02x", 0)),
         ("idle", 0.1), ("check", check_passive("s02x", 0)),
         ("send", b"\x7f"), ("wait_until", passive_ready("s02", 1)),
         ("idle", 0.1), ("check", check_passive("s02", 1)),
         ("send", b"\x15"), ("wait_until", passive_ready("", 0)),
         ("idle", 0.1), ("check", check_passive("", 0)),
         ("send", b"\r")], "", initial_cols=100,
    )

    # Accepting returns directly to a refreshed passive menu without another typed character.
    # The subsequent Enter submits the input rather than accepting a completion again.
    # Mouse press can redraw the active selection before release is processed, so
    # wait for the passive state instead of treating a quiet PTY as completion.
    for scenario, prefix, accept_key, expected in [
        ("completion_auto_menu", b"s", b"\r", "s01"),
        ("completion_auto_menu", b"s", RIGHT, "s01"),
        ("completion_auto_menu", b"s", click_second, "s02"),
    ]:
        assert_resize_case(
            binary, "accept_returns_to_passive", scenario,
            [("send", prefix + b"\t"), ("wait", "enter/right:accept"), ("idle", 0.1),
             ("send", accept_key), ("wait_until", passive_ready(expected, 1)),
             ("idle", 0.1), ("check", check_passive(expected, 1)),
             ("send", b"\t"), ("wait_until", passive_ready(expected, 1)), ("idle", 0.1),
             ("check", check_passive(expected, 1)), ("send", b"\r")],
            expected, initial_cols=100,
        )

    # A unique match completes on the first Tab, including no-op and spell candidates.
    # Observe the passive state before Enter so a preview cannot masquerade as acceptance.
    for scenario, prefix, accept, remaining in [
        ("completion_auto_menu_single", b"hel", b"\t", 1),
        ("completion_auto_menu_single", b"hello", b"\t", 1),
        ("completion_auto_menu_single_nopreview", b"hel", b"\t", 1),
        ("completion_auto_menu_single_hints", b"hel", b"\t", 1),
        ("completion_auto_menu_single_autotab", b"hel", b"\t", 1),
        ("completion_auto_menu_spell", b"hlelo", b"\t", 0),
        ("completion_auto_menu_single", b"hel", RIGHT, 1),
        ("completion_auto_menu_single_autotab", b"hel", RIGHT, 1),
    ]:
        assert_resize_case(
            binary, "complete_without_activation", scenario,
            [("send", prefix), ("wait", "tab:complete"), ("send", accept),
             ("wait_until", passive_ready("hello", remaining)), ("idle", 0.1),
             ("check", check_passive("hello", remaining)), ("send", b"\r")],
            "hello", initial_cols=100,
        )

    # Navigation activates even a unique candidate without accepting it. A later Right
    # accepts the selection; mouse wheel activation also works in smart mouse mode.
    wheel_up = b"\x1b[<64;5;3M"
    for scenario, prefix, activate, expected in [
        ("completion_auto_menu", b"s", DOWN, "s01"),
        ("completion_auto_menu", b"s", UP, "s01"),
        ("completion_auto_menu_single", b"hel", DOWN, "hello"),
        ("completion_auto_menu_single", b"hel", UP, "hello"),
        ("completion_auto_menu_mouse", b"s", wheel_down, "s01"),
        ("completion_auto_menu_mouse", b"s", wheel_up, "s01"),
        ("completion_auto_menu_mouse_smart", b"s", wheel_down, "s01"),
        ("completion_auto_menu_mouse_smart", b"s", wheel_up, "s01"),
        ("completion_auto_menu_mouse_single", b"hel", wheel_down, "hello"),
    ]:
        def check_active(output: str) -> None:
            screen = "\n".join(terminal_screen(output, 24, 120))
            if f"→ {expected}" not in screen or "enter/right:accept" not in screen:
                raise AssertionError(f"navigation should activate the first completion: {screen!r}")

        result = run_resize_case(
            binary, scenario,
            [("send", prefix), ("wait", "up/down:activate"), ("send", activate),
             ("wait_until", terminal_check_ready(check_active)),
             ("send", RIGHT + b"\r")],
            initial_cols=120,
        )
        if result != expected:
            raise AssertionError(f"{scenario}: accepting after activation got {result!r}")

    assert_resize_case(
        binary, "passive_menu_resize", "completion_auto_menu",
        [("send", b"s"), ("wait", "  s12"),
         ("resize", (8, 100)), ("send", FOCUS_IN),
         ("wait", "(8 more below)"), ("idle", 0.1),
         ("send", b"\r")], "s", initial_cols=100,
    )

    assert_completion_auto_menu_whitespace_cases(binary)
    assert_completion_auto_menu_mouse_cases(binary)

    # Observe cancellation before sending more bytes: elapsed time alone cannot
    # keep Escape separate from an Alt sequence when the driver is descheduled.
    for scenario, prefix, footer, restored, next_keys, expected in [
        ("completion_auto_menu", b"s", "tab:activate completions", "s", b"\r", "s"),
        ("completion_auto_menu", b"s", "tab:activate completions", "s", b"0\t\r\r", "s01"),
        ("completion_auto_menu_single", b"hel", "tab:complete", "hel", b"\r", "hel"),
        ("completion_auto_menu_dual", b"pla\t", "enter/right:accept", "pla", b"\r", "pla"),
    ]:
        assert_resize_case(
            binary, "auto_menu_cancel", scenario,
            [("send", prefix), ("wait", footer), ("send", b"\x1b"),
             ("wait_until", completion_menu_closed_ready(restored)), ("send", next_keys)],
            expected, initial_cols=100,
        )


def assert_completion_auto_menu_whitespace_cases(binary: str) -> None:
    def check_menu(expected_input: str, mode: str):
        def check(output: str) -> None:
            screen = terminal_screen(output, 24, 100)
            if screen[0].rstrip() != f"pty> {expected_input}".rstrip():
                raise AssertionError(f"{label}: whitespace edits must preserve input: {screen!r}")
            menu = "\n".join(screen[1:]).strip()
            if mode == "hidden":
                if menu:
                    raise AssertionError(
                        f"{label}: whitespace before the cursor must hide the menu: {screen!r}"
                    )
            else:
                footers = (
                    ("tab:activate completions", "tab:complete")
                    if mode == "passive" else ("enter/right:accept",)
                )
                if "Completions" not in menu or not any(footer in menu for footer in footers):
                    raise AssertionError(f"{label}: expected a {mode} completion menu: {screen!r}")
        return check

    for label, steps, expected in [
        ("space_and_backspace", [
            (b"  ", "  ", "hidden"),
            (b"\x15", "", "hidden"),
            (b"s", "s", "passive"),
            (b" ", "s ", "hidden"),
            (b" ", "s  ", "hidden"),
            (b"s", "s  s", "passive"),
            (b"\x7f", "s  ", "hidden"),
        ], "s  "),
        ("cursor_between_arguments", [
            (b"s  s", "s  s", "passive"),
            (LEFT, "s  s", "hidden"),
            (LEFT, "s  s", "hidden"),
            (RIGHT, "s  s", "hidden"),
            (RIGHT, "s  s", "passive"),
            (HOME, "s  s", "hidden"),
        ], "s  s"),
        ("click_between_arguments", [
            (b"s  s", "s  s", "passive"),
            (mouse_left_click(8, 1), "s  s", "hidden"),
            (END, "s  s", "passive"),
        ], "s  s"),
        ("paste_whitespace", [
            (b"s", "s", "passive"),
            (b"\x1b[200~  \x1b[201~", "s  ", "hidden"),
            (b"s", "s  s", "passive"),
        ], "s  s"),
        ("explicit_tab_after_space", [
            (b"s ", "s ", "hidden"),
            (b"\t", "s ", "active"),
            (b"\r", "s s01", "passive"),
        ], "s s01"),
        ("space_in_active_menu", [
            (b"s\t", "s", "active"),
            (b" ", "s ", "hidden"),
            (b"s", "s s", "passive"),
        ], "s s"),
        ("backspace_in_active_menu", [
            (b"s s\t", "s s", "active"),
            (b"\x7f", "s ", "hidden"),
            (b"s", "s s", "passive"),
        ], "s s"),
    ]:
        actions = []
        for keys, expected_input, mode in steps:
            check = check_menu(expected_input, mode)
            actions.extend([
                ("send", keys), ("wait_until", terminal_check_ready(check)),
                ("idle", 0.1), ("check", check),
            ])
        actions.append(("send", b"\r"))
        actual = run_resize_case(
            binary, "completion_auto_menu_mouse_nopreview", actions,
            initial_cols=100, respond_to_cursor_queries=True,
        )
        if actual != expected:
            raise AssertionError(f"{label} expected {expected!r}, got {actual!r}")


def assert_completion_auto_menu_mouse_cases(binary: str) -> None:
    def click_fragment(fragment: str, rows: int, cols: int):
        def click(output: str) -> bytes:
            screen = terminal_screen(output, rows, cols)
            for row, line in enumerate(screen):
                if fragment in line:
                    return mouse_left_click(5, row + 1)
            raise AssertionError(f"missing click target {fragment!r}: {screen!r}")
        return click

    # Inspect the screen before sending acceptance: a click-to-accept configuration must not
    # cause the activating press/release pair to accept, even when preview changes menu geometry.
    for scenario, target, selected, expected, rows, cols in [
        ("completion_auto_menu_mouse", "  s02", "s02", "s02", 24, 100),
        ("completion_auto_menu_mouse_smart", "  s02", "s02", "s02", 24, 100),
        ("completion_auto_menu_mouse_selectonly", "  s02", "s02", "s02", 24, 100),
        ("completion_auto_menu_mouse_nopreview", "  s02", "s02", "s02", 24, 100),
        ("completion_auto_menu_mouse", "Completions", "s01", "s01", 24, 100),
        ("completion_auto_menu_mouse", "tab:activate", "s01", "s01", 24, 100),
        ("completion_auto_menu_mouse_limit", "  s03", "s03", "s03", 24, 100),
        ("completion_auto_menu_mouse_limit", "tab:activate", "s01", "s01", 24, 100),
        ("completion_auto_menu_mouse", "  s12", "s12", "s12", 24, 100),
        ("completion_auto_menu_mouse_single", "  hello", "hello", "hello", 24, 100),
        ("completion_auto_menu_mouse_multiline", "  s02", "s02", "echo\ns02", 24, 100),
        ("completion_auto_menu_mouse_prefix", "  s02", "s02", "s02", 24, 100),
        ("completion_auto_menu_mouse", "  s02", "s02", "s02", 12, 30),
        ("completion_auto_menu_mouse", "  s02", "s02", "s02", 16, 20),
    ]:
        prefix = b"hel" if "_single" in scenario else b"s"
        footer = "tab:complete" if "_single" in scenario else "tab:activate"
        if "_multiline" in scenario:
            prefix = b"\x7fs"  # replace the seeded final character without submitting a newline

        def check_active(output: str) -> None:
            screen = "\n".join(terminal_screen(output, rows, cols))
            if f"→ {selected}" not in screen or footer in screen:
                raise AssertionError(f"activating click must only select {selected!r}: {screen!r}")
            if "_nopreview" in scenario and "pty> s02" in screen:
                raise AssertionError(f"activating click must not modify input: {screen!r}")

        result = run_resize_case(
            binary, scenario,
            [("send", prefix), ("wait", footer), ("idle", 0.1),
             ("send", click_fragment(target, rows, cols)),
             ("wait_until", terminal_check_ready(check_active)), ("idle", 0.1),
             ("check", check_active), ("send", b"\r\r")],
            initial_rows=rows, initial_cols=cols, respond_to_cursor_queries=True,
        )
        if result != expected:
            raise AssertionError(f"click activation expected {expected!r}, got {result!r}")

    for label, scenario, keys, expected in [
        ("outside_passive_menu", "completion_auto_menu_mouse",
         b"s" + mouse_left_click(5, 20) + b"\r", "s"),
        ("input_click_still_edits", "completion_auto_menu_mouse",
         b"s" + mouse_left_click(6, 1) + b"X\r", "Xs"),
        ("mouse_disabled_at_prompt", "completion_auto_menu_mouse",
         F2 + b"s" + mouse_left_click(5, 4) + b"\r", "s"),
        ("passive_right_click", "completion_auto_menu_mouse",
         b"s\x1b[<2;5;4M\x1b[<2;5;4m\r", "s"),
        ("active_wheel_after_click", "completion_auto_menu_mouse",
         b"s" + mouse_left_click(5, 4) + b"\x1b[<65;1;1M\r\r", "s03"),
        ("active_click_accept_after_activation", "completion_auto_menu_mouse",
         b"s" + mouse_left_click(5, 4) + mouse_left_click(5, 5) + b"\r", "s03"),
        ("smart_drag_stays_passive", "completion_auto_menu_mouse_smart",
         b"s" + mouse_left_press(5, 4) + mouse_left_drag(6, 4) +
         mouse_left_release(6, 4) + b"\r", "s"),
    ]:
        assert_case(binary, label, scenario, keys, expected)

    assert_resize_case(
        binary, "cancel_mouse_activated_menu", "completion_auto_menu_mouse",
        [("send", b"s"), ("wait", "tab:activate completions"),
         ("send", mouse_left_click(5, 4)), ("wait", "enter/right:accept"),
         ("send", b"\x1b"), ("wait_until", completion_menu_closed_ready("s")),
         ("send", b"\r")], "s", initial_cols=100,
    )


def assert_timed_case(
    binary: str,
    label: str,
    scenario: str,
    chunks: list[bytes],
    expected: str,
    initial_delay_s: float = 0.08,
    step_delay_s: float = 0.06,
    poll_interval_s: float = 0.01,
    wait_for_reprompt: bool = False,
) -> None:
    actual = run_case_timed(
        binary,
        scenario,
        chunks,
        initial_delay_s=initial_delay_s,
        step_delay_s=step_delay_s,
        poll_interval_s=poll_interval_s,
        wait_for_reprompt=wait_for_reprompt,
    )
    if actual != expected:
        raise AssertionError(f"{label} expected {expected!r}, got {actual!r}")


def assert_resize_case(
    binary: str,
    label: str,
    scenario: str,
    actions: list[tuple[str, object]],
    expected: str,
    initial_rows: int = 24,
    initial_cols: int = 40,
    timeout_s: float = 8.0,
    poll_interval_s: float = 0.01,
) -> None:
    actual = run_resize_case(
        binary,
        scenario,
        actions,
        initial_rows=initial_rows,
        initial_cols=initial_cols,
        timeout_s=timeout_s,
        poll_interval_s=poll_interval_s,
    )
    if actual != expected:
        raise AssertionError(f"{label} expected {expected!r}, got {actual!r}")


def assert_prompt_guard_case(binary: str, scenario: str, expect_marker: bool) -> None:
    result, output_text = run_case(binary, scenario, b"ok\r", capture_output=True)
    if result != "ok":
        raise AssertionError(f"{scenario} expected 'ok', got {result!r}")
    assert_prompt_guard_marker(scenario, output_text, expect_marker)


def assert_menu_replaces_multiline_prompt(output_text: str, menu_prompt: str) -> None:
    menu_index = output_text.find(menu_prompt)
    if menu_index < 0:
        raise AssertionError(
            f"missing menu prompt {menu_prompt!r}: output={output_text!r}"
        )

    clear_index = output_text.rfind("\x1b[2A", 0, menu_index)
    if clear_index < 0:
        raise AssertionError(
            "menu did not move above both multi-line prompt prefix rows before rendering: "
            f"output={output_text!r}"
        )

    replacement_output = output_text[clear_index:menu_index]
    if replacement_output.count("\x1b[K") < 3:
        raise AssertionError(
            "menu did not clear the two prompt-prefix rows and editable prompt row: "
            f"replacement_output={replacement_output!r}"
        )
    if "MENU-BASE-TOP" in replacement_output or "MENU-BASE-MIDDLE" in replacement_output:
        raise AssertionError(
            "menu redrew the primary multi-line prompt while installing its own prompt: "
            f"replacement_output={replacement_output!r}"
        )
    if "MENU-BASE-RIGHT" in output_text[clear_index:]:
        raise AssertionError(
            "menu redrew the primary right prompt instead of showing only its menu prompt: "
            f"output={output_text!r}"
        )


def assert_final_prompt_replaces_multiline_prompt(output_text: str) -> None:
    final_index = output_text.find("FINAL-TOP")
    if final_index < 0:
        raise AssertionError(f"missing multi-line final prompt: output={output_text!r}")

    clear_index = output_text.rfind("\x1b[2A", 0, final_index)
    if clear_index < 0:
        raise AssertionError(
            "final prompt did not move above both original prompt-prefix rows before clearing: "
            f"output={output_text!r}"
        )

    replacement_output = output_text[clear_index:final_index]
    if replacement_output.count("\x1b[K") < 3:
        raise AssertionError(
            "final prompt did not clear the two original prompt-prefix rows and editable row: "
            f"replacement_output={replacement_output!r}"
        )

    final_middle_index = output_text.find("FINAL-MIDDLE", final_index)
    final_prompt_index = output_text.find("final> ", final_middle_index)
    if final_middle_index < 0 or final_prompt_index < 0:
        raise AssertionError(
            "multi-line final prompt was not rendered in order after clearing: "
            f"output={output_text!r}"
        )


def assert_region_markers(output_text: str, expect_secondary_prompt: bool) -> None:
    prompt_start = "\x1b]133;A\x1b\\"
    secondary_start = "\x1b]133;A;k=s\x1b\\"
    input_start = "\x1b]133;B\x1b\\"
    output_start = "\x1b]133;C\x1b\\"
    output_end = "\x1b]133;D;7\x1b\\"

    command_start_index = output_text.find(output_start)
    command_end_index = output_text.find(output_end, command_start_index + 1)
    output_text_index = output_text.find("region-output", command_start_index + 1)
    input_start_index = output_text.rfind(input_start, 0, command_start_index)
    prompt_start_index = output_text.rfind(prompt_start, 0, input_start_index)

    if min(
        prompt_start_index,
        input_start_index,
        command_start_index,
        output_text_index,
        command_end_index,
    ) < 0:
        raise AssertionError(f"missing OSC 133 region marker: output={output_text!r}")
    if not (
        prompt_start_index
        < input_start_index
        < command_start_index
        < output_text_index
        < command_end_index
    ):
        raise AssertionError(f"OSC 133 markers are out of order: output={output_text!r}")

    secondary_index = output_text.find(secondary_start)
    if expect_secondary_prompt and secondary_index < 0:
        raise AssertionError(f"missing OSC 133 secondary-prompt marker: output={output_text!r}")
    if not expect_secondary_prompt and secondary_index >= 0:
        raise AssertionError(f"unexpected OSC 133 secondary-prompt marker: output={output_text!r}")


def assert_text_is_prompt_marked(output_text: str, text: str) -> None:
    prompt_start = "\x1b]133;A\x1b\\"
    input_start = "\x1b]133;B\x1b\\"
    text_index = output_text.rfind(text)
    if text_index < 0:
        raise AssertionError(f"missing prompt text {text!r}: output={output_text!r}")

    prompt_start_index = output_text.rfind(prompt_start, 0, text_index)
    input_start_index = output_text.find(input_start, text_index + len(text))
    if prompt_start_index < 0 or input_start_index < 0:
        raise AssertionError(
            f"prompt text {text!r} is missing an OSC 133 A/B boundary: "
            f"output={output_text!r}"
        )
    if output_text.find(input_start, prompt_start_index, text_index) >= 0:
        raise AssertionError(
            f"prompt text {text!r} appears after OSC 133 input start: "
            f"output={output_text!r}"
        )


def assert_last_prompt_suffix(
    scenario: str, output_text: str, expected_suffix: str
) -> None:
    pre_result = output_text.split("[IC_RESULT_BEGIN]", 1)[0]
    normalized = normalize_terminal_output(pre_result).rstrip("\n")
    prompt = "pty> "
    prompt_index = normalized.rfind(prompt)
    if prompt_index < 0:
        raise AssertionError(
            f"{scenario} missing final prompt in output: normalized_output={normalized!r}"
        )
    actual_suffix = normalized[prompt_index + len(prompt) :]
    if actual_suffix != expected_suffix:
        raise AssertionError(
            f"{scenario} expected final prompt suffix {expected_suffix!r}, got "
            f"{actual_suffix!r}, normalized_output={normalized!r}"
        )


def parse_readline_status_payload(payload: str) -> tuple[str, str, bool, bool]:
    parts = payload.split("|")
    if len(parts) != 4:
        raise AssertionError(f"invalid status payload format: {payload!r}")

    disposition, line, tty_part, lost_part = parts
    if not tty_part.startswith("tty="):
        raise AssertionError(f"invalid tty marker in payload: {payload!r}")
    if not lost_part.startswith("lost="):
        raise AssertionError(f"invalid lost marker in payload: {payload!r}")

    tty_active = tty_part.split("=", 1)[1] == "1"
    tty_lost = lost_part.split("=", 1)[1] == "1"
    return disposition, line, tty_active, tty_lost


def run_case(
    binary: str,
    scenario: str,
    key_bytes: bytes,
    timeout_s: float = 5.0,
    capture_output: bool = False,
    initial_rows: int | None = None,
    initial_cols: int | None = None,
) -> str | tuple[str, str]:
    global PTY_CASE_COUNT
    PTY_CASE_COUNT += 1

    pid, fd = pty.fork()
    if pid == 0:
        # CTest supplies the locally built driver, and scenarios are fixed below.
        # execv receives an argument vector directly; no shell parses these values.
        if initial_rows is not None or initial_cols is not None:
            set_pty_window_size(0, initial_rows or 24, initial_cols or 80)
        if scenario.startswith("line_wrap_marker_"):
            # Isocline treats the C locale as UTF-8 on every supported platform.
            os.environ["LC_ALL"] = "C"
        os.execv(binary, [binary, scenario])  # nosemgrep

    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    output = bytearray()
    deadline = time.monotonic() + timeout_s
    sent = False
    sent_bytes = 0
    cursor_report_sent = False

    try:
        while time.monotonic() < deadline:
            read_pending_output(fd, output)

            if (
                scenario == "prompt_guard_region_marking_external_visible"
                and not cursor_report_sent
                and b"\x1b[6n" in output
            ):
                os.write(fd, b"\x1b[1;22R")
                cursor_report_sent = True

            capture_ready = (
                scenario.startswith("typeahead_capture_")
                and TYPEAHEAD_CAPTURE_READY_MARKER in output
            )
            if not sent and (b"pty> " in output or capture_ready):
                # Nonblocking PTYs can accept only part of a large paste.
                # Keep pumping output and retry the remaining bytes next time.
                if sent_bytes < len(key_bytes):
                    try:
                        sent_bytes += os.write(fd, key_bytes[sent_bytes:])
                    except BlockingIOError:
                        pass
                sent = sent_bytes == len(key_bytes)

            waited_pid, status = os.waitpid(pid, os.WNOHANG)
            if waited_pid == pid:
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                    text = output.decode("utf-8", errors="replace")
                    raise AssertionError(
                        f"case {scenario} failed: exit={status}, output={text!r}"
                    )
                drain_remaining_output(fd, output)
                text = output.decode("utf-8", errors="replace")
                match = RESULT_RE.search(text)
                if match is None:
                    raise AssertionError(
                        f"case {scenario} missing result marker: {text!r}"
                    )
                result = match.group(1).replace("\r", "")
                if capture_output:
                    return result, text
                return result

            time.sleep(0.01)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass

    text = output.decode("utf-8", errors="replace")
    raise AssertionError(f"case {scenario} timed out, output={text!r}")


def run_case_timed(
    binary: str,
    scenario: str,
    chunks: list[bytes],
    timeout_s: float = 8.0,
    initial_delay_s: float = 0.08,
    step_delay_s: float = 0.25,
    poll_interval_s: float = 0.01,
    wait_for_reprompt: bool = False,
) -> str:
    global PTY_CASE_COUNT
    PTY_CASE_COUNT += 1

    pid, fd = pty.fork()
    if pid == 0:
        # CTest supplies the locally built driver, and scenarios are fixed below.
        # execv receives an argument vector directly; no shell parses these values.
        os.execv(binary, [binary, scenario])  # nosemgrep

    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    output = bytearray()
    deadline = time.monotonic() + timeout_s
    next_send_at = time.monotonic() + initial_delay_s
    send_index = 0
    chunk_offset = 0
    prompt_seen = False
    last_output_at = time.monotonic()
    reprompt_output_start: int | None = None
    reprompt_idle_s = max(poll_interval_s * 3, 0.05)

    try:
        while time.monotonic() < deadline:
            now = time.monotonic()

            if read_pending_output(fd, output):
                last_output_at = now
            if not prompt_seen and b"pty> " in output:
                prompt_seen = True

            ready_to_send = prompt_seen and send_index < len(chunks) and now >= next_send_at
            if ready_to_send and reprompt_output_start is not None:
                new_output = output[reprompt_output_start:]
                marker_idx = new_output.find(READLINE_STEP_MARKER)
                prompt_after_marker = (
                    marker_idx >= 0
                    and new_output.find(b"pty> ", marker_idx + len(READLINE_STEP_MARKER)) >= 0
                )
                ready_to_send = prompt_after_marker and (now - last_output_at) >= reprompt_idle_s
            if ready_to_send:
                chunk_to_send = chunks[send_index]
                try:
                    chunk_offset += os.write(fd, chunk_to_send[chunk_offset:])
                except BlockingIOError:
                    pass
                if chunk_offset == len(chunk_to_send):
                    chunk_offset = 0
                    send_index += 1
                    if (
                        wait_for_reprompt
                        and send_index < len(chunks)
                        and chunk_to_send.endswith((b"\r", b"\n"))
                    ):
                        # History triplet cases emit a marker after each hidden readline.
                        # Wait for the marker and the following fresh prompt.
                        reprompt_output_start = len(output)
                    else:
                        reprompt_output_start = None
                    next_send_at = now + step_delay_s

            waited_pid, status = os.waitpid(pid, os.WNOHANG)
            if waited_pid == pid:
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                    text = output.decode("utf-8", errors="replace")
                    raise AssertionError(
                        f"case {scenario} failed: exit={status}, output={text!r}"
                    )
                drain_remaining_output(fd, output, poll_interval_s=poll_interval_s)
                text = output.decode("utf-8", errors="replace")
                match = RESULT_RE.search(text)
                if match is None:
                    raise AssertionError(
                        f"case {scenario} missing result marker: {text!r}"
                    )
                return match.group(1).replace("\r", "")

            time.sleep(poll_interval_s)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass

    text = output.decode("utf-8", errors="replace")
    raise AssertionError(f"case {scenario} timed out, output={text!r}")


def set_pty_window_size(fd: int, rows: int, cols: int) -> None:
    winsize = struct.pack("HHHH", rows, cols, 0, 0)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)


def run_resize_case(
    binary: str,
    scenario: str,
    actions: list[tuple[str, object]],
    initial_rows: int = 24,
    initial_cols: int = 40,
    timeout_s: float = 8.0,
    poll_interval_s: float = 0.01,
    return_after_actions: bool = False,
    respond_to_cursor_queries: bool = False,
) -> str:
    global PTY_CASE_COUNT
    PTY_CASE_COUNT += 1

    pid, fd = pty.fork()
    if pid == 0:
        # Set the size before the driver can cache it or render its first prompt.
        # Setting it in the parent races with startup on slower CI runners.
        set_pty_window_size(0, initial_rows, initial_cols)
        # CTest supplies the locally built driver, and scenarios are fixed below.
        # execv receives an argument vector directly; no shell parses these values.
        os.execv(binary, [binary, scenario])  # nosemgrep

    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    output = bytearray()
    deadline = time.monotonic() + timeout_s
    prompt_seen = False
    action_index = 0
    current_rows = initial_rows
    current_cols = initial_cols
    cursor_reports_sent = 0
    last_output_at = time.monotonic()
    action_output_start = 0

    def normalized_output(start: int = 0) -> str:
        text = output[start:].decode("utf-8", errors="replace")
        return normalize_terminal_output(text)

    try:
        while time.monotonic() < deadline:
            if read_pending_output(fd, output):
                last_output_at = time.monotonic()
                if not prompt_seen and b"pty> " in output:
                    prompt_seen = True
                if respond_to_cursor_queries:
                    query_count = output.count(b"\x1b[6n")
                    if query_count > cursor_reports_sent:
                        _, (row, col) = terminal_state(
                            output.decode("utf-8", errors="replace"), current_rows, current_cols
                        )
                        os.write(fd, f"\x1b[{row + 1};{col + 1}R".encode("ascii"))
                        cursor_reports_sent = query_count

            while prompt_seen and action_index < len(actions):
                action, value = actions[action_index]
                if action == "wait":
                    # A previous render cannot acknowledge the latest input. Raw
                    # bytes also let mouse tests wait for terminal mode changes.
                    recent = (output[action_output_start:] if isinstance(value, bytes)
                              else normalized_output(action_output_start))
                    if value not in recent:
                        break
                elif action == "idle":
                    if time.monotonic() - last_output_at < float(value):
                        break
                elif action == "wait_until":
                    if not value(output.decode("utf-8", errors="replace")):
                        break
                elif action == "send":
                    keys = (
                        value(output.decode("utf-8", errors="replace"))
                        if callable(value)
                        else value
                    )
                    action_output_start = len(output)
                    os.write(fd, keys)
                    last_output_at = time.monotonic()
                elif action == "check":
                    value(output.decode("utf-8", errors="replace"))
                elif action == "resize":
                    action_output_start = len(output)
                    if isinstance(value, tuple):
                        next_rows, next_cols = value
                    else:
                        next_rows, next_cols = current_rows, value
                    set_pty_window_size(fd, next_rows, next_cols)
                    try:
                        os.kill(pid, signal.SIGWINCH)
                    except OSError:
                        pass
                    current_rows = next_rows
                    current_cols = next_cols
                    last_output_at = time.monotonic()
                else:
                    raise AssertionError(
                        f"case {scenario} has unknown resize action {action!r}"
                    )
                action_index += 1

            if return_after_actions and prompt_seen and action_index == len(actions):
                return output.decode("utf-8", errors="replace")

            waited_pid, status = os.waitpid(pid, os.WNOHANG)
            if waited_pid == pid:
                drain_remaining_output(fd, output, poll_interval_s=poll_interval_s)
                text = output.decode("utf-8", errors="replace")
                normalized = normalize_terminal_output(text)
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                    raise AssertionError(
                        f"case {scenario} failed: exit={status}, output={text!r}"
                    )
                if action_index != len(actions):
                    action, value = actions[action_index]
                    raise AssertionError(
                        f"case {scenario} exited before completing {action} {value!r}, "
                        f"normalized_output={normalized!r}"
                    )
                match = RESULT_RE.search(text)
                if match is None:
                    raise AssertionError(
                        f"case {scenario} missing result marker: {text!r}"
                    )
                return match.group(1).replace("\r", "")

            # Drain redraws as soon as they arrive so the PTY output buffer does
            # not throttle long menu navigation sequences on macOS.
            select.select([fd], [], [], poll_interval_s)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass

    normalized = normalized_output()
    if not prompt_seen:
        missing = "initial prompt"
    elif action_index < len(actions):
        action, value = actions[action_index]
        if action == "wait":
            missing = f"fragment {value!r}"
        elif action == "idle":
            missing = f"idle {value!r}"
        elif action == "wait_until":
            missing = "expected terminal state"
        elif action == "send":
            missing = f"send {value!r}"
        else:
            missing = f"resize {value!r}"
    else:
        missing = "case completion"
    raise AssertionError(
        f"case {scenario} timed out while waiting for {missing}, "
        f"normalized_output={normalized!r}"
    )


def observe_resize_case(
    binary: str,
    scenario: str,
    actions: list[tuple[str, object]],
    initial_rows: int = 24,
    initial_cols: int = 40,
    timeout_s: float = 8.0,
    poll_interval_s: float = 0.01,
) -> str:
    return run_resize_case(
        binary,
        scenario,
        actions,
        initial_rows=initial_rows,
        initial_cols=initial_cols,
        timeout_s=timeout_s,
        poll_interval_s=poll_interval_s,
        return_after_actions=True,
    )


def assert_completion_preview_fits(
    output: str, rows: int, cols: int, prefix_rows: int = 0
) -> None:
    normalized = normalize_terminal_output(output)
    prompt_index = normalized.rfind("pty> ")
    if prompt_index < 0:
        raise AssertionError(f"completion preview lost its prompt: {normalized!r}")
    render = normalized[prompt_index:].rstrip()
    input_text, separator, menu = render.partition("\nCompletions")
    if not separator or not input_text.startswith("pty> m02 first line"):
        raise AssertionError(f"completion preview should retain its beginning: {render!r}")
    if not input_text.endswith("...") or "preview line 20" in input_text:
        raise AssertionError(f"tall completion preview should end with an ellipsis: {render!r}")
    if "Completions" in menu or not any(
        line.startswith(("→ m02", "> m02")) for line in menu.splitlines()
    ):
        raise AssertionError(f"tall completion should remain selected in one menu: {render!r}")
    if "esc:cancel)" not in re.sub(r"[←↵]\n", "", menu):
        raise AssertionError(f"completion preview hid the menu footer: {render!r}")
    # These fixtures use single-column characters, including the selection/wrap arrows.
    lines = render.splitlines()
    if len(lines) + prefix_rows > rows or any(len(line) > cols for line in lines):
        raise AssertionError(f"completion preview exceeds the {rows}x{cols} terminal: {render!r}")


def assert_menu_viewports(binary: str) -> None:
    menus = {
        "completion": (b"entry\t", "Completions", 8),
        "history": (b"\x12entry", "120 matches found", 9),
        "palette": (ALT_P + b"zzviewport", "Actions found - case", 9),
        "custom": (F3, "Items - case", 9),
    }

    def check(kind, keys, first, count, selected, suffix="", rows=80, reopen=None,
              resize_to=None):
        opening, marker, _ = menus[kind]
        scenario = "menu_viewport_" + kind + suffix
        actions = [("send", opening + keys), ("wait", marker), ("idle", 0.1)]
        if resize_to is not None:
            actions += [("resize", (resize_to, 160)), ("send", FOCUS_IN), ("idle", 0.2)]
        if reopen is not None:
            # Escape is decoded after a timeout. Wait for dismissal before
            # reopening so slow runners cannot combine it with the next key.
            actions += [("send", b"\x1b"), ("wait", "pty> "), ("idle", 0.1)]
            if kind == "completion":
                # Clear the fixture completer's inserted common prefix before reopening.
                actions += [("send", b"\x1b"), ("wait", "pty> "), ("idle", 0.1)]
            actions += [("send", reopen), ("wait", marker), ("idle", 0.1)]

        def viewport_ready(output):
            render = normalize_terminal_output(output).rsplit(marker, 1)[-1]
            entries = [int(value) for value in re.findall(r"^[ →>]+entry(\d{3})", render, re.M)]
            selection = re.search(r"^[→>] entry(\d{3})", render, re.M)
            return (
                entries == list(range(first, first + count))
                and selection is not None
                and int(selection.group(1)) == selected
            )

        # A quiet interval can occur halfway through a redraw when ASan or
        # other test workers delay the driver. Observe the expected viewport
        # before allowing the footer/cursor output to settle.
        actions += [("wait_until", viewport_ready), ("idle", 0.1)]
        output = observe_resize_case(
            binary,
            scenario,
            actions,
            initial_rows=rows,
            initial_cols=160,
        )
        render = normalize_terminal_output(output).rsplit(marker, 1)[-1]
        entries = [int(value) for value in re.findall(r"^[ →>]+entry(\d{3})", render, re.M)]
        selection = re.search(r"^[→>] entry(\d{3})", render, re.M)
        if entries != list(range(first, first + count)) or (
            selection is None or int(selection.group(1)) != selected
        ):
            raise AssertionError(
                f"{scenario} expected entries {first}-{first + count - 1}, selected {selected}: "
                f"{render!r}"
            )
        if kind == "completion":
            above, below = first, 120 - first - count
            if above and below:
                hint = f"  ({above} above, {below} below)"
            elif above or below:
                hint = f"  ({above or below} more {'above' if above else 'below'})"
            else:
                hint = ""
            if hint and hint + "\n(↑↓/tab/wheel:move" not in render:
                raise AssertionError(f"completion scroll hint should precede the footer: {render!r}")
            if not hint and ("more above" in render or "more below" in render):
                raise AssertionError(f"fitting all completions should hide the scroll hint: {render!r}")
        return render

    for kind, (_, _, short_count) in menus.items():
        default_rows = 15
        check(kind, b"", 0, default_rows, 0, suffix="_default")
        check(kind, b"\n", 0, 68 + short_count, 0, suffix="_default")
        check(kind, b"\n\n", 0, default_rows, 0, suffix="_default")
        check(kind, b"", 0, 50, 0)
        check(kind, b"", 0, 8, 0, suffix="_limit")
        # Ctrl+J bypasses only this invocation's cap, then restores it without moving selection.
        check(kind, b"\n", 0, 68 + short_count, 0, suffix="_limit")
        check(kind, b"\n\n", 0, 8, 0, suffix="_limit")
        check(kind, b"\n", 0, short_count, 0, suffix="_limit", resize_to=12)
        check(kind, b"\n", 0, 68 + short_count, 0, suffix="_limit", rows=12, resize_to=80)
        check(kind, DOWN * 4 + b"\n\n", 0, 8, 4, suffix="_limit")
        # Expanding with fewer results than terminal rows must not create blank items.
        check(kind, b"\n", 0, 120, 0, suffix="_limit", rows=160)
        # Closing and reopening resets the temporary height even after maximizing.
        reopen = {"completion": b"entry\t", "history": b"\x12entry",
                  "palette": ALT_P + b"zzviewport", "custom": F3}[kind]
        check(kind, b"\n", 0, 8, 0, suffix="_limit", reopen=reopen)
        check(kind, b"", 0, 75, 0, suffix="_large", rows=100)
        check(kind, DOWN * 4, 4, 1, 4, suffix="_single")
        check(kind, DOWN * 47, 51 - short_count, short_count, 47, rows=12)

        # Scrolling starts at the three-row margin and stays fixed until the opposite margin.
        check(kind, DOWN * 46, 0, 50, 46)
        check(kind, DOWN * 47, 1, 50, 47)
        check(kind, DOWN * 47 + UP * 43, 1, 50, 4)
        check(kind, DOWN * 47 + UP * 44, 0, 50, 3)
        check(kind, DOWN * 119, 70, 50, 119)

        # Paging must retain the requested page after applying the scroll margin.
        page_down = b"\x1b[1;2B"
        check(kind, page_down, 50, 50, 53)
        check(kind, page_down * 2, 70, 50, 73)
        check(kind, page_down + SHIFT_UP, 0, 50, 3)

    check("completion", PAGEDOWN + PAGEUP, 0, 50, 3)
    check("custom", DOWN * 49, 0, 50, 49, suffix="_no_margin")
    check("custom", DOWN * 50, 1, 50, 50, suffix="_no_margin")
    preview = check("custom", b"", 0, 48, 0, suffix="_preview")
    if "preview second line" not in preview or "preview third line" not in preview:
        raise AssertionError(f"menu content limit should include the expanded preview: {preview!r}")


def assert_menu_dismissal(binary: str) -> None:
    menus = {
        "completion": (
            b"choice\t",
            [
                ("enter", DOWN + b"\r"),
                ("right", DOWN + RIGHT),
                ("end", DOWN + END),
                ("number", b"2"),
            ],
        ),
        "history": (b"\x12choice", [("enter", DOWN + b"\r"), ("tab", DOWN + b"\t")]),
        "palette": (
            ALT_P + b"zzdismiss",
            [("enter", DOWN + b"\r"), ("tab", DOWN + b"\t")],
        ),
        "custom": (F3, [("enter", DOWN + b"\r"), ("tab", DOWN + b"\t")]),
    }
    for suffix, rows, cols in [
        ("", 24, 160),
        ("_multiline_prompt", 24, 160),
        ("", 8, 80),
    ]:

        def assert_closed(output: str, expected: str) -> None:
            screen = terminal_screen(output, rows, cols)
            prompts = [
                line
                for line in screen
                if line.startswith("pty> ") and "[IC_MENU_ACTION_BEGIN]" not in line
            ]
            if (
                len(prompts) != 1
                or prompts[0].replace("MENU-BASE-RIGHT", "").rstrip()
                != f"pty> {expected}"
            ):
                raise AssertionError(
                    f"menu should restore input {expected!r}: screen={screen!r}"
                )
            leftovers = [
                line
                for line in screen
                if line
                and line not in prompts
                and line not in ("MENU-BASE-TOP", "MENU-BASE-MIDDLE")
                and "[IC_MENU_ACTION_BEGIN]" not in line
            ]
            if leftovers:
                raise AssertionError(
                    f"menu rows remain after selection: screen={screen!r}"
                )
            if suffix and not all(
                label in screen for label in ("MENU-BASE-TOP", "MENU-BASE-MIDDLE")
            ):
                raise AssertionError(
                    f"menu should restore the multiline prompt: screen={screen!r}"
                )

        click_position = (1, 1)

        def click_second(output: str) -> bytes:
            nonlocal click_position
            screen = terminal_screen(output, rows, cols)
            for row, line in enumerate(screen, 1):
                if "choicetwo" in line:
                    click_position = (line.index("choicetwo") + 2, row)
                    return mouse_left_press(*click_position)
            raise AssertionError(f"second menu item is not visible: screen={screen!r}")

        for kind, (opening, keyboard_accepts) in menus.items():
            scenario = f"menu_dismiss_{kind}{suffix}"
            for method, keys in keyboard_accepts + [("mouse", click_second)]:
                label = f"{scenario}/{method}/{rows}x{cols}"

                def check_selection(output: str) -> None:
                    if kind in ("palette", "custom"):
                        before_action, marker, _ = output.partition(
                            "[IC_MENU_ACTION_BEGIN]"
                        )
                        if not marker:
                            raise AssertionError(
                                f"{label}: selected action did not run"
                            )
                        assert_closed(before_action, "keep")
                    assert_closed(output, "choicetwo")

                if kind == "history" and method == "enter":
                    result, output = run_case(
                        binary,
                        scenario,
                        opening + keys,
                        capture_output=True,
                        initial_rows=rows,
                        initial_cols=cols,
                    )
                    before_submit, marker, _ = output.partition("[IC_MENU_SUBMIT]")
                    if result != "choicetwo" or not marker or "choiceone" not in output:
                        raise AssertionError(
                            f"{label}: history selection did not submit: {output!r}"
                        )
                    assert_closed(before_submit, "choicetwo")
                    continue

                acceptance = [("send", keys)]
                if method == "mouse":
                    # Let the editor query its screen position between press and release.
                    acceptance += [
                        ("idle", 0.05),
                        ("send", lambda output: mouse_left_release(*click_position)),
                    ]
                result = run_resize_case(
                    binary,
                    scenario,
                    [
                        ("send", opening),
                        ("wait", "choicetwo"),
                        ("idle", 0.05),
                        *acceptance,
                        ("wait", "pty> choicetwo"),
                        ("idle", 0.05),
                        ("check", check_selection),
                        ("send", b"!\r"),
                    ],
                    initial_rows=rows,
                    initial_cols=cols,
                    respond_to_cursor_queries=True,
                )
                if result != "choicetwo!":
                    raise AssertionError(
                        f"{label}: editing after selection returned {result!r}"
                    )

        # Built-in palette actions must also leave the restored input editable.
        for key in (b"\r", b"\t"):
            result = run_resize_case(
                binary,
                f"menu_dismiss_palette{suffix}",
                [
                    ("send", ALT_P + b"cursor left"),
                    ("wait", "Actions found - case"),
                    ("idle", 0.05),
                    ("send", key),
                    ("idle", 0.05),
                    ("check", lambda output: assert_closed(output, "keep")),
                    ("send", b"!\r"),
                ],
                initial_rows=rows,
                initial_cols=cols,
            )
            if result != "kee!p":
                raise AssertionError(
                    f"built-in palette action lost the cursor position: {result!r}"
                )


def assert_line_wrap_marker(binary: str) -> None:
    default_marker = "↵" if IS_DARWIN else "←"
    for mode, marker, width in (
        ("default", default_marker, 1), ("empty", "", 0), ("restored", default_marker, 1),
        ("ascii", "!", 1), ("unicode", "↪", 1), ("wide", "界", 2), ("bracket", "[", 1),
    ):
        scenario = f"line_wrap_marker_{mode}"
        # Cross soft-wrap boundaries before editing, then insert a real newline.
        actual, output = run_case(
            binary, scenario, LEFT * 24 + b"X" + CTRL_END + b"\nnext\r",
            capture_output=True, initial_cols=20,
        )
        expected = "abcdefghijklXmnopqrstuvwxyz0123456789\nnext"
        if actual != expected:
            raise AssertionError(f"{scenario}: expected {expected!r}, got {actual!r}")
        if (default_marker in output) != (mode in ("default", "restored")):
            raise AssertionError(f"{scenario}: unexpected wrap marker visibility: {output!r}")
        first_row = "pty> " + "abcdefghijklmno"[:15 - width] + marker + "\n"
        if first_row not in normalize_terminal_output(output):
            raise AssertionError(f"{scenario}: incorrect wrap boundary: {output!r}")

    def expect_screen(lines: list[str], cursor: tuple[int, int]):
        def check(output: str) -> None:
            screen, position = terminal_state(output, 8, 20)
            expected = lines + [""] * (8 - len(lines))
            if screen != expected or position != cursor:
                raise AssertionError(
                    f"full-width editing: expected {(expected, cursor)!r}, "
                    f"got {(screen, position)!r}; output={output!r}"
                )
        return check

    marker = "↵" if IS_DARWIN else "←"
    result = run_resize_case(
        binary, "line_wrap_marker_restored_boundary",
        [
            ("send", b"abcdefghijklmn"),
            ("idle", 0.1),
            ("check", expect_screen(["pty> abcdefghijklmn"], (0, 19))),
            ("send", b"o"),
            ("idle", 0.1),
            ("check", expect_screen([f"pty> abcdefghijklmn{marker}", "   > o"], (1, 6))),
            ("send", LEFT),
            ("idle", 0.1),
            ("check", expect_screen([f"pty> abcdefghijklmn{marker}", "   > o"], (1, 5))),
            ("send", b"\x7f"),
            ("idle", 0.1),
            ("check", expect_screen(["pty> abcdefghijklmo"], (0, 18))),
            ("send", b"n" + RIGHT + b"\np"),
            ("idle", 0.1),
            ("check", expect_screen(
                [f"pty> abcdefghijklmn{marker}", "   > o", "   > p"], (2, 6)
            )),
            ("resize", 24),
            ("idle", 0.1),
            ("resize", 20),
            ("idle", 0.1),
            ("check", expect_screen(
                [f"pty> abcdefghijklmn{marker}", "   > o", "   > p"], (2, 6)
            )),
            ("send", b"\r"),
        ],
        initial_rows=8, initial_cols=20,
    )
    if result != "abcdefghijklmno\np":
        raise AssertionError(f"single-column marker changed input: {result!r}")

    result = run_resize_case(
        binary, "line_wrap_marker_empty_boundary",
        [
            ("send", b"abcdefghijklmn"),
            ("idle", 0.1),
            ("check", expect_screen(["pty> abcdefghijklmn"], (0, 19))),
            ("send", b"o"),
            ("idle", 0.1),
            ("check", expect_screen(["pty> abcdefghijklmno", "   >"], (1, 5))),
            ("send", b"p"),
            ("idle", 0.1),
            ("check", expect_screen(["pty> abcdefghijklmno", "   > p"], (1, 6))),
            ("send", b"\x7f\x7f"),
            ("idle", 0.1),
            ("check", expect_screen(["pty> abcdefghijklmn"], (0, 19))),
            ("send", b"o\nnext"),
            ("idle", 0.1),
            ("check", expect_screen(["pty> abcdefghijklmno", "   > next"], (1, 9))),
            ("send", b"\r"),
        ],
        initial_rows=8, initial_cols=20,
    )
    if result != "abcdefghijklmno\nnext":
        raise AssertionError(f"full-width editing changed input: {result!r}")

    # Reflow full-width rows, then use vertical movement to edit the last column.
    result = run_resize_case(
        binary, "line_wrap_marker_empty",
        [
            ("idle", 0.1),
            ("resize", 24),
            ("idle", 0.1),
            ("resize", 20),
            ("idle", 0.1),
            ("check", expect_screen(
                ["pty> abcdefghijklmno", "   > pqrstuvwxyz0123", "   > 456789"], (2, 11)
            )),
            ("send", CTRL_HOME + RIGHT * 14 + DOWN + b"Y\r"),
        ],
        initial_rows=8, initial_cols=20,
    )
    expected = "abcdefghijklmnopqrstuvwxyz012Y3456789"
    if result != expected:
        raise AssertionError(f"full-width cursor movement expected {expected!r}, got {result!r}")

    def check_wide_marker(output: str) -> None:
        # The screen helper models single-column cells; expand the known two-column marker.
        expect_screen(
            ["pty> abcdefghijklm##", "   > nopqrstuvwxyz##", "   > 0123456789"], (2, 15)
        )(output.replace("界", "##"))

    result = run_resize_case(
        binary, "line_wrap_marker_wide",
        [
            ("idle", 0.1),
            ("resize", 24),
            ("idle", 0.1),
            ("resize", 20),
            ("idle", 0.1),
            ("check", check_wide_marker),
            ("send", CTRL_HOME + RIGHT * 12 + DOWN + b"Y\r"),
        ],
        initial_rows=8, initial_cols=20,
    )
    expected = "abcdefghijklmnopqrstuvwxyYz0123456789"
    if result != expected:
        raise AssertionError(f"wide-marker cursor movement expected {expected!r}, got {result!r}")


def assert_multiline_history_navigation(binary: str) -> None:
    older = "old first\n\nold middle\nold last"
    newer = "new first\nnew middle\nnew last"
    draft = b"draft\x0asaved"
    cases = [
        ("recall_at_buffer_end", UP, newer),
        ("previous_skips_multiline_rows", UP * 2, older),
        ("next_skips_multiline_rows", UP * 2 + DOWN, newer),
        ("next_restores_empty_input", UP * 2 + DOWN * 2, ""),
        ("oldest_entry_boundary", UP * 3, older),
        ("newest_entry_boundary", DOWN, ""),
        ("typed_multiline_end_starts_history", draft + UP, newer),
        ("restore_multiline_draft_at_end", draft + UP * 2 + DOWN * 2, "draft\nsaved"),
        ("prefix_recall_at_buffer_end", b"old" + UP, older),
        ("ctrl_p_recall_at_buffer_end", b"\x10", newer),
        ("ctrl_n_recall_at_buffer_end", b"\x10\x10\x0e", newer),
    ]
    for label, position in [
        ("start", PAGEUP),
        ("middle", PAGEUP + DOWN + RIGHT),
        ("last_line", HOME),
        ("end", b""),
    ]:
        cases.append((f"shift_up_from_{label}", UP + position + SHIFT_UP, older))
        cases.append((f"shift_down_from_{label}", UP * 2 + position + SHIFT_DOWN, newer))
    cases.append(("shift_up_from_typed_middle", draft + PAGEUP + DOWN + SHIFT_UP, newer))
    cases.append(
        (
            "shift_down_restores_draft",
            draft + SHIFT_UP + PAGEUP + SHIFT_DOWN,
            "draft\nsaved",
        )
    )

    # A narrow terminal also exercises history entries wrapped across visual rows.
    for columns in (80, 12):
        for label, keys, expected in cases:
            result = run_case(
                binary,
                "history_navigation_multiline",
                keys + b"X\r",
                initial_rows=24,
                initial_cols=columns,
            )
            if result != expected + "X":
                raise AssertionError(
                    f"{label} ({columns} columns) expected {expected + 'X'!r}, got {result!r}"
                )


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <isocline_pty_driver>", file=sys.stderr)
        return 2

    binary = os.path.abspath(sys.argv[1])
    if not os.path.exists(binary):
        print(f"driver binary not found: {binary}", file=sys.stderr)
        return 2

    assert_line_wrap_marker(binary)
    assert_menu_viewports(binary)
    assert_menu_dismissal(binary)

    for scenario, keys, expected in [
        ("notification_edit", LEFT + b"\x1b[17~X\r", "aXb"),
        ("notification_edit", b"c\x1b[17~\x1f\r", "ab"),
        ("notification_edit", b"c\x1f\x1b[17~\x19\r", "abc"),
        # Encode Escape explicitly so the following Enter is not decoded as Alt+Enter/Ctrl+J.
        ("notification_completion", b"\t\x1b[27u\r", "plan"),
        ("notification_submit", b"\r", "ab"),
        (
            "notification_multiline",
            LEFT + b"\x1b[17~X\r",
            "first\nsecond\nthird\nfourth\nfifth\nsixth\nseventXh",
        ),
    ]:
        actual, output = run_case(binary, scenario, keys, capture_output=True)
        if actual != expected:
            raise AssertionError(f"{scenario}: expected {expected!r}, got {actual!r}")
        first = output.find("NOTICE-ONE [b]")
        second = output.find("NOTICE-TWO", first)
        restored = output.find("NOTICE-TOP", second)
        if min(first, second, restored) < 0 or output.count("NOTICE-ONE [b]") != 1:
            raise AssertionError(f"{scenario}: missing/duplicate notification or prompt: {output!r}")
        # The old prompt prefix, input and hint rows must all be erased before
        # any notification text reaches the terminal.
        prior_prompt = output.rfind("pty", 0, first)
        if output[prior_prompt:first].count("\x1b[K") < 3:
            raise AssertionError(f"{scenario}: editor rows were not cleared: {output!r}")
        if "NOTICE-RIGHT" in output[:first] and "NOTICE-RIGHT" not in output[restored:]:
            raise AssertionError(f"{scenario}: right prompt was not restored: {output!r}")
        if "\x1b[?2004l" in output[:second]:
            raise AssertionError(f"{scenario}: notification released terminal modes: {output!r}")

    insert = run_case(binary, "insert_backspace", b"ab\x7fcd\r")
    if insert != "acd":
        raise AssertionError(f"insert_backspace expected 'acd', got {insert!r}")

    moved = run_case(binary, "cursor_move_insert", b"\x02Z\r")
    if moved != "aZb":
        raise AssertionError(f"cursor_move_insert expected 'aZb', got {moved!r}")

    home_insert = run_case(binary, "home_insert", b"\x01a\r")
    if home_insert != "abc":
        raise AssertionError(f"home_insert expected 'abc', got {home_insert!r}")

    end_insert = run_case(binary, "end_insert", b"\x05c\r")
    if end_insert != "abc":
        raise AssertionError(f"end_insert expected 'abc', got {end_insert!r}")

    alias_navigation_cases = [
        ("left_arrow_insert", "insert_backspace", b"ab" + LEFT + b"Z\r", "aZb"),
        (
            "right_arrow_insert",
            "insert_backspace",
            b"ab" + LEFT + RIGHT + b"Z\r",
            "abZ",
        ),
        ("home_key_insert", "insert_backspace", b"bc" + HOME + b"a\r", "abc"),
        (
            "end_key_insert",
            "insert_backspace",
            b"ab" + HOME + END + b"c\r",
            "abc",
        ),
        ("ctrl_h_backspace", "insert_backspace", b"ab\x08cd\r", "acd"),
        (
            "ctrl_f_cursor_right_midline",
            "insert_backspace",
            b"ab\x01\x06X\r",
            "aXb",
        ),
        (
            "delete_key_midline",
            "insert_backspace",
            b"abc" + LEFT + b"\x1b[3~\r",
            "ab",
        ),
        ("ctrl_h_at_start", "backspace_at_start_noop", b"\x01\x08\r", "ab"),
        ("delete_key_at_end", "delete_at_end_noop", b"\x05\x1b[3~\r", "ab"),
    ]
    for label, scenario, key_bytes, expected in alias_navigation_cases:
        assert_case(binary, label, scenario, key_bytes, expected)

    backspace_start = run_case(binary, "backspace_at_start_noop", b"\x01\x7f\r")
    if backspace_start != "ab":
        raise AssertionError(
            f"backspace_at_start_noop expected 'ab', got {backspace_start!r}"
        )

    delete_end = run_case(binary, "delete_at_end_noop", b"\x05\x04\r")
    if delete_end != "ab":
        raise AssertionError(f"delete_at_end_noop expected 'ab', got {delete_end!r}")

    kill_end_noop = run_case(binary, "kill_to_end_at_end_noop", b"\x05\x0b\r")
    if kill_end_noop != "ab":
        raise AssertionError(
            f"kill_to_end_at_end_noop expected 'ab', got {kill_end_noop!r}"
        )

    kill_start_noop = run_case(binary, "kill_to_start_at_start_noop", b"\x01\x15\r")
    if kill_start_noop != "ab":
        raise AssertionError(
            f"kill_to_start_at_start_noop expected 'ab', got {kill_start_noop!r}"
        )

    left_boundary = run_case(binary, "left_boundary_insert", b"\x01\x02X\r")
    if left_boundary != "Xab":
        raise AssertionError(
            f"left_boundary_insert expected 'Xab', got {left_boundary!r}"
        )

    right_boundary = run_case(binary, "right_boundary_insert", b"\x05\x06X\r")
    if right_boundary != "abX":
        raise AssertionError(
            f"right_boundary_insert expected 'abX', got {right_boundary!r}"
        )

    append_initial = run_case(binary, "append_to_initial_input", b"c\r")
    if append_initial != "abc":
        raise AssertionError(
            f"append_to_initial_input expected 'abc', got {append_initial!r}"
        )

    redraw_keeps_buffer = run_case(binary, "ctrl_l_redraw_keeps_buffer", b"\x0c\r")
    if redraw_keeps_buffer != "ab":
        raise AssertionError(
            f"ctrl_l_redraw_keeps_buffer expected 'ab', got {redraw_keeps_buffer!r}"
        )

    roundtrip_nav = run_case(binary, "ctrl_a_ctrl_e_append", b"\x01\x05c\r")
    if roundtrip_nav != "abc":
        raise AssertionError(
            f"ctrl_a_ctrl_e_append expected 'abc', got {roundtrip_nav!r}"
        )

    delete_end_noop_2 = run_case(binary, "ctrl_d_at_end_noop", b"\x05\x04\r")
    if delete_end_noop_2 != "ab":
        raise AssertionError(
            f"ctrl_d_at_end_noop expected 'ab', got {delete_end_noop_2!r}"
        )

    kill_to_end = run_case(binary, "ctrl_k_delete_to_end", b"\x02\x02\x0b\r")
    if kill_to_end != "abcd":
        raise AssertionError(
            f"ctrl_k_delete_to_end expected 'abcd', got {kill_to_end!r}"
        )

    kill_to_start = run_case(binary, "ctrl_u_delete_to_start", b"\x02\x02\x15\r")
    if kill_to_start != "ef":
        raise AssertionError(
            f"ctrl_u_delete_to_start expected 'ef', got {kill_to_start!r}"
        )

    kill_then_type = run_case(binary, "ctrl_k_then_type", b"\x02\x02\x0bXY\r")
    if kill_then_type != "abcdXY":
        raise AssertionError(
            f"ctrl_k_then_type expected 'abcdXY', got {kill_then_type!r}"
        )

    kill_start_then_type = run_case(binary, "ctrl_u_then_type", b"\x02\x02\x15XY\r")
    if kill_start_then_type != "XYef":
        raise AssertionError(
            f"ctrl_u_then_type expected 'XYef', got {kill_start_then_type!r}"
        )

    delete_word = run_case(binary, "ctrl_w_delete_word", b"alpha beta\x17\r")
    if delete_word != "alpha ":
        raise AssertionError(
            f"ctrl_w_delete_word expected 'alpha ', got {delete_word!r}"
        )

    delete_single_word = run_case(binary, "ctrl_w_single_word", b"\x17\r")
    if delete_single_word != "":
        raise AssertionError(
            f"ctrl_w_single_word expected empty string, got {delete_single_word!r}"
        )

    delete_mid = run_case(binary, "ctrl_d_delete_mid", b"\x02\x04\r")
    if delete_mid != "ab":
        raise AssertionError(f"ctrl_d_delete_mid expected 'ab', got {delete_mid!r}")

    delete_mid_2 = run_case(binary, "delete_mid_twice", b"\x02\x02\x04\x04\r")
    if delete_mid_2 != "ab":
        raise AssertionError(f"delete_mid_twice expected 'ab', got {delete_mid_2!r}")

    backspace_twice = run_case(binary, "backspace_twice_typed", b"abcd\x7f\x7f\r")
    if backspace_twice != "ab":
        raise AssertionError(
            f"backspace_twice_typed expected 'ab', got {backspace_twice!r}"
        )

    ctrl_w_then_type = run_case(binary, "ctrl_w_then_type", b"alpha beta\x17gamma\r")
    if ctrl_w_then_type != "alpha gamma":
        raise AssertionError(
            f"ctrl_w_then_type expected 'alpha gamma', got {ctrl_w_then_type!r}"
        )

    word_and_edit_alias_cases = [
        (
            "alt_backspace_delete_word_start",
            "insert_backspace",
            b"alpha beta\x1b\x7f\r",
            "alpha",
        ),
        (
            "alt_delete_delete_word_start",
            "insert_backspace",
            b"alpha beta" + ALT_DELETE + b"\r",
            "alpha",
        ),
        (
            "alt_d_delete_word_end",
            "insert_backspace",
            b"alpha beta" + LEFT * 4 + b"\x1bd\r",
            "alpha ",
        ),
        (
            "alt_b_prev_word",
            "insert_backspace",
            b"alpha beta\x1bbX\r",
            "alphaX beta",
        ),
        (
            "alt_f_next_word",
            "insert_backspace",
            b"alpha beta\x01\x1bfX\r",
            "alpha Xbeta",
        ),
        (
            "platform_word_prev_alias",
            "insert_backspace",
            b"alpha beta" + WORD_PREV + b"X\r",
            "alphaX beta",
        ),
        (
            "platform_word_next_alias",
            "insert_backspace",
            b"alpha beta\x01" + WORD_NEXT + b"X\r",
            "alpha Xbeta",
        ),
        ("match_brace_alt_m", "insert_backspace", b"(ab)\x1bmX\r", "(Xab)"),
        ("transpose_ctrl_t", "insert_backspace", b"ab" + LEFT + b"\x14\r", "ba"),
    ]
    for label, scenario, key_bytes, expected in word_and_edit_alias_cases:
        assert_case(binary, label, scenario, key_bytes, expected)

    undo_single = run_case(binary, "undo_single_change", b"c\x1a\r")
    if undo_single != "ab":
        raise AssertionError(f"undo_single_change expected 'ab', got {undo_single!r}")

    undo_alias = run_case(binary, "undo_single_change", b"c\x1f\r")
    if undo_alias != "ab":
        raise AssertionError(f"undo_single_change ctrl+_ expected 'ab', got {undo_alias!r}")

    undo_redo = run_case(binary, "undo_redo_roundtrip", b"c\x1a\x19\r")
    if undo_redo != "abc":
        raise AssertionError(f"undo_redo_roundtrip expected 'abc', got {undo_redo!r}")

    undo_kill = run_case(binary, "undo_after_kill_to_end", b"\x02\x02\x0b\x1a\r")
    if undo_kill != "abcdef":
        raise AssertionError(
            f"undo_after_kill_to_end expected 'abcdef', got {undo_kill!r}"
        )

    redo_cleared = run_case(binary, "redo_cleared_by_new_edit", b"c\x1aX\x19\r")
    if redo_cleared != "abX":
        raise AssertionError(
            f"redo_cleared_by_new_edit expected 'abX', got {redo_cleared!r}"
        )

    multiline_ctrl_j = run_case(binary, "multiline_ctrl_j_insert_newline", b"a\x0ab\r")
    if multiline_ctrl_j != "a\nb":
        raise AssertionError(
            f"multiline_ctrl_j_insert_newline expected 'a\\nb', got {multiline_ctrl_j!r}"
        )

    # Typeahead has a byte-level contract: CR is Return (submit), while LF is
    # Ctrl+J (insert a multiline line feed). Most cases need no live keystroke;
    # this proves a queued Return completes readline on its own. The one live
    # Return case proves queued Ctrl+J did not accidentally submit first.
    long_typeahead = (
        "0123456789abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-"
        "typeahead-beyond-the-legacy-pushback-limit"
    )
    typeahead_cases = [
        ("typeahead_capture_empty", b"", "terminal-active"),
        ("typeahead_capture_paste_wakeup", b"[201~", "paste-ended"),
        ("typeahead_capture_return", b"captured return\r", "captured return"),
        (
            "typeahead_capture_ctrl_j_then_return",
            b"first\nsecond\r",
            "first\nsecond",
        ),
        ("typeahead_return_submit", b"", "queued command"),
        ("typeahead_ctrl_j_then_live_return", b"\r", "first\nsecond"),
        ("typeahead_ctrl_j_then_queued_return", b"", "first\nsecond"),
        ("typeahead_leading_ctrl_j", b"", "\nbody"),
        ("typeahead_repeated_ctrl_j", b"", "a\n\nb"),
        ("typeahead_empty_return", b"", ""),
        ("typeahead_chunked_return", b"", "chunked command"),
        ("typeahead_utf8_return", b"", "café €"),
        ("typeahead_edited_return", b"", "abd"),
        ("typeahead_long_return", b"", long_typeahead),
        ("typeahead_two_returns", b"", "first|second"),
        ("typeahead_return_then_ctrl_j", b"", "first|second\nthird"),
        ("typeahead_crlf_distinct", b"", "first|\nsecond"),
        (
            "typeahead_interrupt_then_return",
            b"\x03queued command\r",
            "<CTRL+C>|queued command",
        ),
        ("vim_typeahead_return_submit", b"", "queued command"),
        ("vim_typeahead_ctrl_j_then_queued_return", b"", "first\nsecond"),
    ]
    for scenario, live_keys, expected in typeahead_cases:
        assert_case(binary, scenario, scenario, live_keys, expected)

    region_result, region_output = run_case(
        binary, "region_marking", b"ok\r", capture_output=True
    )
    if region_result != "ok":
        raise AssertionError(f"region_marking expected 'ok', got {region_result!r}")
    assert_region_markers(region_output, expect_secondary_prompt=False)

    multiline_region_result, multiline_region_output = run_case(
        binary,
        "region_marking_multiline",
        b"a\x0ab\r",
        capture_output=True,
    )
    if multiline_region_result != "a\nb":
        raise AssertionError(
            "region_marking_multiline expected 'a\\nb', "
            f"got {multiline_region_result!r}"
        )
    assert_region_markers(multiline_region_output, expect_secondary_prompt=True)

    transient_result, transient_output = run_case(
        binary,
        "region_marking_transient_prompt_components",
        b"ok\r",
        capture_output=True,
    )
    if transient_result != "ok":
        raise AssertionError(
            "region_marking_transient_prompt_components expected 'ok', "
            f"got {transient_result!r}"
        )
    assert_region_markers(transient_output, expect_secondary_prompt=False)
    assert_text_is_prompt_marked(transient_output, "final-prefix")
    assert_text_is_prompt_marked(transient_output, "final-right")

    multiline_final_result, multiline_final_output = run_case(
        binary,
        "transient_prompt_multiline_clear",
        b"ok\r",
        capture_output=True,
    )
    if multiline_final_result != "ok":
        raise AssertionError(
            "transient_prompt_multiline_clear expected 'ok', "
            f"got {multiline_final_result!r}"
        )
    assert_final_prompt_replaces_multiline_prompt(multiline_final_output)

    multiline_backslash = run_case(
        binary, "multiline_backslash_continuation", b"echo \\\rhi\r"
    )
    if multiline_backslash != "echo \nhi":
        raise AssertionError(
            f"multiline_backslash_continuation expected 'echo \\nhi', got {multiline_backslash!r}"
        )

    multiline_backslash_retained = run_case(
        binary, "multiline_backslash_continuation_retained", b"echo \\\rhi\r"
    )
    if multiline_backslash_retained != "echo \\\nhi":
        raise AssertionError(
            "multiline_backslash_continuation_retained expected the continuation "
            f"character to remain, got {multiline_backslash_retained!r}"
        )

    multiline_backslash_with_following_content = run_case(
        binary,
        "multiline_backslash_submit_with_following_content",
        LEFT + UP + END + b"\r",
    )
    if multiline_backslash_with_following_content != "echo \\\nhi":
        raise AssertionError(
            "Enter on a continued line with following content should submit the complete "
            "buffer, got "
            f"{multiline_backslash_with_following_content!r}"
        )

    multiline_auto_indent = run_case(
        binary, "multiline_auto_continuation_indent", b"if true; then\recho ok\r"
    )
    if multiline_auto_indent != "if true; then\n  echo ok":
        raise AssertionError(
            "Automatic continuation should indent a shell block body, got "
            f"{multiline_auto_indent!r}"
        )

    multiline_auto_indent_disabled = run_case(
        binary, "multiline_auto_continuation_indent_disabled", b"if true; then\recho ok\r"
    )
    if multiline_auto_indent_disabled != "if true; then\necho ok":
        raise AssertionError(
            "Disabling multiline indentation should leave an automatic continuation unindented, got "
            f"{multiline_auto_indent_disabled!r}"
        )

    multiline_initial = run_case(binary, "multiline_initial_ctrl_j", b"\x0acd\r")
    if multiline_initial != "ab\ncd":
        raise AssertionError(
            f"multiline_initial_ctrl_j expected 'ab\\ncd', got {multiline_initial!r}"
        )

    multiline_ctrl_a_stays_on_line = run_case(
        binary, "multiline_ctrl_a_stays_on_line", b"\x01\x01X\r"
    )
    if multiline_ctrl_a_stays_on_line != "ab\ncd\nXef":
        raise AssertionError(
            "multiline_ctrl_a_stays_on_line expected 'ab\\ncd\\nXef', got "
            f"{multiline_ctrl_a_stays_on_line!r}"
        )

    multiline_ctrl_e_stays_on_line = run_case(
        binary,
        "multiline_ctrl_e_stays_on_line",
        b"\x01\x02\x01\x02\x05\x05\x05X\r",
    )
    if multiline_ctrl_e_stays_on_line != "abX\ncd\nef":
        raise AssertionError(
            "multiline_ctrl_e_stays_on_line expected 'abX\\ncd\\nef', got "
            f"{multiline_ctrl_e_stays_on_line!r}"
        )

    viewport_expected = (
        "viewport-line-01\nviewport-line-02\nviewport-line-03\n"
        "viewport-line-04\nviewport-line-05"
    )
    viewport_result, viewport_output = run_case(
        binary,
        "multiline_max_lines_viewport",
        b"\x0c\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if viewport_result != viewport_expected:
        raise AssertionError(
            "multiline_max_lines_viewport should preserve the complete buffer, got "
            f"{viewport_result!r}"
        )
    viewport_render = normalize_terminal_output(
        viewport_output.split("[IC_RESULT_BEGIN]", 1)[0]
    )
    if "viewport-line-01" in viewport_render or "viewport-line-02" in viewport_render:
        raise AssertionError(
            "multiline viewport rendered rows above its three-line window: "
            f"output={viewport_render!r}"
        )
    for expected_line in ("viewport-line-03", "viewport-line-04", "viewport-line-05"):
        if expected_line not in viewport_render:
            raise AssertionError(
                f"multiline viewport omitted visible row {expected_line!r}: "
                f"output={viewport_render!r}"
            )
    for expected_number in ("3| ", "4| ", "5| "):
        if expected_number not in viewport_render:
            raise AssertionError(
                "multiline viewport lost logical line-number state while skipping hidden rows: "
                f"missing={expected_number!r}, output={viewport_render!r}"
            )
    if "\x1b[4A" in viewport_output:
        raise AssertionError(
            "viewport clear/redraw used the five-row logical cursor position instead of the "
            f"three-row screen position: output={viewport_output!r}"
        )

    terminal_cap_expected = (
        "terminal-line-01\nterminal-line-02\nterminal-line-03\n"
        "terminal-line-04\nterminal-line-05"
    )
    terminal_cap_result, terminal_cap_output = run_case(
        binary,
        "multiline_terminal_row_cap",
        b"\r",
        capture_output=True,
        initial_rows=4,
        initial_cols=80,
    )
    if terminal_cap_result != terminal_cap_expected:
        raise AssertionError(
            "terminal row cap should preserve the complete multiline buffer, got "
            f"{terminal_cap_result!r}"
        )
    terminal_cap_render = normalize_terminal_output(
        terminal_cap_output.split("[IC_RESULT_BEGIN]", 1)[0]
    )
    for hidden_line in ("terminal-line-01", "terminal-line-02", "terminal-line-03"):
        if hidden_line in terminal_cap_render:
            raise AssertionError(
                "multiline viewport exceeded the terminal row count after accounting for "
                f"prompt-prefix rows: output={terminal_cap_render!r}"
            )
    for visible_line in ("terminal-line-04", "terminal-line-05"):
        if visible_line not in terminal_cap_render:
            raise AssertionError(
                f"terminal-sized multiline viewport omitted {visible_line!r}: "
                f"output={terminal_cap_render!r}"
            )

    history_search_result, history_search_output = run_case(
        binary,
        "history_search_long_multiline_viewport",
        LEFT + UP + UP + b"\x12\x03\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if history_search_result != viewport_expected.replace("viewport", "history"):
        raise AssertionError(
            "history search should preserve the complete multiline buffer, got "
            f"{history_search_result!r}"
        )
    history_search_render = normalize_terminal_output(
        history_search_output.split("[IC_RESULT_BEGIN]", 1)[0]
    )
    if "history search: > history-line-03\nNo matches" not in history_search_render:
        raise AssertionError(
            "history search should use only the logical line containing the cursor: "
            f"output={history_search_render!r}"
        )

    reset_result, reset_output = run_case(
        binary,
        "multiline_max_lines_prompt_reset",
        b"\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if reset_result != viewport_expected:
        raise AssertionError(
            "multiline_max_lines_prompt_reset should preserve the complete buffer, got "
            f"{reset_result!r}"
        )
    final_prompt_index = reset_output.find("VIEWPORT-FINAL-TOP")
    if final_prompt_index < 0:
        raise AssertionError(
            f"multiline viewport prompt reset omitted the final prompt: output={reset_output!r}"
        )
    reset_clear_index = reset_output.rfind("\x1b[2A", 0, final_prompt_index)
    if reset_clear_index < 0:
        raise AssertionError(
            "multiline viewport prompt reset did not move up by its two visible cursor rows: "
            f"output={reset_output!r}"
        )
    reset_clear_output = reset_output[reset_clear_index:final_prompt_index]
    if reset_clear_output.count("\x1b[K") < 3 or "\x1b[4A" in reset_clear_output:
        raise AssertionError(
            "multiline viewport prompt reset did not clear exactly the rendered viewport: "
            f"clear_output={reset_clear_output!r}"
        )

    menu_expected = (
        "hidden-menu-line-01\nhidden-menu-line-02\nvisible-menu-line-03\n"
        "visible-menu-line-04\ns01"
    )
    menu_result, menu_output = run_case(
        binary,
        "completion_many_menu_long_multiline",
        b"\t\r\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if menu_result != menu_expected:
        raise AssertionError(
            "completion menu over a multiline viewport should preserve the complete buffer, got "
            f"{menu_result!r}"
        )
    menu_render = normalize_terminal_output(menu_output.split("[IC_RESULT_BEGIN]", 1)[0])
    if "s01" not in menu_render:
        raise AssertionError(
            f"completion menu was not rendered below the multiline viewport: {menu_render!r}"
        )
    if "hidden-menu-line-01" in menu_render or "hidden-menu-line-02" in menu_render:
        raise AssertionError(
            "completion menu temporarily exposed input rows outside the configured viewport: "
            f"output={menu_render!r}"
        )

    multiline_navigation_cases = [
        (
            "pageup_input_start",
            b"ab\x0acd\x0aef" + PAGEUP + b"X\r",
            "Xab\ncd\nef",
        ),
        (
            "ctrl_home_input_start",
            b"ab\x0acd\x0aef" + CTRL_HOME + b"X\r",
            "Xab\ncd\nef",
        ),
        (
            "shift_home_input_start",
            b"ab\x0acd\x0aef" + SHIFT_HOME + b"X\r",
            "Xab\ncd\nef",
        ),
        (
            "alt_lt_input_start",
            b"ab\x0acd\x0aef" + ALT_LT + b"X\r",
            "Xab\ncd\nef",
        ),
        (
            "pagedown_input_end",
            b"ab\x0acd\x0aef" + PAGEUP + PAGEDOWN + b"X\r",
            "ab\ncd\nefX",
        ),
        (
            "ctrl_end_input_end",
            b"ab\x0acd\x0aef" + PAGEUP + CTRL_END + b"X\r",
            "ab\ncd\nefX",
        ),
        (
            "shift_end_input_end",
            b"ab\x0acd\x0aef" + PAGEUP + SHIFT_END + b"X\r",
            "ab\ncd\nefX",
        ),
        (
            "alt_gt_input_end",
            b"ab\x0acd\x0aef" + PAGEUP + ALT_GT + b"X\r",
            "ab\ncd\nefX",
        ),
    ]
    for label, key_bytes, expected in multiline_navigation_cases:
        assert_case(
            binary,
            label,
            "multiline_ctrl_j_insert_newline",
            key_bytes,
            expected,
        )

    multiline_row_navigation_cases = [
        ("up_row_navigation", b"ab\x0acd\x0aef" + LEFT + UP + b"X\r", "ab\ncXd\nef"),
        (
            "up_at_end_without_history",
            b"ab\x0acd\x0aef" + UP + b"X\r",
            "ab\ncd\nefX",
        ),
        (
            "shift_up_without_history_keeps_cursor",
            b"ab\x0acd\x0aef" + LEFT + SHIFT_UP + b"X\r",
            "ab\ncd\neXf",
        ),
        (
            "down_row_navigation",
            b"ab\x0acd\x0aef" + PAGEUP + DOWN + b"X\r",
            "ab\nXcd\nef",
        ),
    ]
    for label, key_bytes, expected in multiline_row_navigation_cases:
        assert_case(
            binary,
            label,
            "multiline_ctrl_j_insert_newline",
            key_bytes,
            expected,
        )

    if IS_DARWIN:
        shift_tab_newline = run_case(
            binary, "multiline_ctrl_j_insert_newline", b"a" + SHIFT_TAB + b"b\r"
        )
        if shift_tab_newline != "a\nb":
            raise AssertionError(
                f"multiline shift+tab expected 'a\\nb', got {shift_tab_newline!r}"
            )
    else:
        ctrl_enter_newline = run_case(
            binary, "multiline_ctrl_j_insert_newline", b"a" + CTRL_ENTER + b"b\r"
        )
        if ctrl_enter_newline != "a\nb":
            raise AssertionError(
                f"multiline ctrl+enter expected 'a\\nb', got {ctrl_enter_newline!r}"
            )

    ctrl_c = run_case(binary, "ctrl_c", b"\x03")
    if ctrl_c != "<CTRL+C>":
        raise AssertionError(f"ctrl_c expected '<CTRL+C>', got {ctrl_c!r}")

    ctrl_d = run_case(binary, "ctrl_d_empty", b"\x04")
    if ctrl_d != "<CTRL+D>":
        raise AssertionError(f"ctrl_d_empty expected '<CTRL+D>', got {ctrl_d!r}")

    status_submit = run_case(binary, "status_text", b"ok\r")
    disposition, line, tty_active, tty_lost = parse_readline_status_payload(status_submit)
    if disposition != "submit" or line != "ok":
        raise AssertionError(
            "status_text expected submit with line 'ok', "
            f"got disposition={disposition!r}, line={line!r}"
        )
    if not tty_active or tty_lost:
        raise AssertionError(
            f"status_text expected tty_active=1/lost=0, got tty_active={tty_active}, "
            f"tty_lost={tty_lost}"
        )

    status_interrupt = run_case(binary, "status_ctrl_c", b"\x03")
    disposition, line, tty_active, tty_lost = parse_readline_status_payload(status_interrupt)
    if disposition != "interrupt" or line != "<CTRL+C>":
        raise AssertionError(
            "status_ctrl_c expected interrupt with <CTRL+C>, "
            f"got disposition={disposition!r}, line={line!r}"
        )
    if not tty_active or tty_lost:
        raise AssertionError(
            f"status_ctrl_c expected tty_active=1/lost=0, got tty_active={tty_active}, "
            f"tty_lost={tty_lost}"
        )

    status_eof = run_case(binary, "status_ctrl_d", b"\x04")
    disposition, line, tty_active, tty_lost = parse_readline_status_payload(status_eof)
    if disposition != "eof" or line != "<CTRL+D>":
        raise AssertionError(
            "status_ctrl_d expected eof with <CTRL+D>, "
            f"got disposition={disposition!r}, line={line!r}"
        )
    if not tty_active or tty_lost:
        raise AssertionError(
            f"status_ctrl_d expected tty_active=1/lost=0, got tty_active={tty_active}, "
            f"tty_lost={tty_lost}"
        )

    status_stop = run_case(binary, "status_stop_event", b"")
    disposition, line, tty_active, tty_lost = parse_readline_status_payload(status_stop)
    if disposition != "stop" or line != "<CTRL+C>":
        raise AssertionError(
            "status_stop_event expected stop with compatibility token <CTRL+C>, "
            f"got disposition={disposition!r}, line={line!r}"
        )
    if not tty_active or tty_lost:
        raise AssertionError(
            "status_stop_event expected tty_active=1/lost=0 for synthetic stop event, "
            f"got tty_active={tty_active}, tty_lost={tty_lost}"
        )

    status_idle = run_case(binary, "status_idle", b"")
    if status_idle != "idle|abc\n|cursor=1|tty=1|lost=0":
        raise AssertionError(
            "status_idle should preserve the buffer and cursor while returning an idle "
            f"disposition, got {status_idle!r}"
        )

    idle_menu_cases = [
        (
            "status_idle_completion_menu",
            b"\t",
            "Completions",
            "idle|s|cursor=1|tty=1|lost=0",
        ),
        (
            "status_idle_history_menu",
            b"\x12",
            "history search:",
            "idle|keep|cursor=4|tty=1|lost=0",
        ),
        (
            "status_idle_command_palette",
            ALT_P,
            "command palette:",
            "idle|keep|cursor=4|tty=1|lost=0",
        ),
        (
            "status_idle_custom_menu",
            F3,
            "custom actions:",
            "idle|keep|cursor=4|tty=1|lost=0",
        ),
    ]
    for scenario, menu_key, menu_marker, expected in idle_menu_cases:
        actual, output = run_case(binary, scenario, menu_key, capture_output=True)
        if actual != expected:
            raise AssertionError(
                f"{scenario} should close its open menu and return idle, got {actual!r}"
            )
        if menu_marker not in normalize_terminal_output(output):
            raise AssertionError(
                f"{scenario} did not render the expected menu before going idle: "
                f"output={output!r}"
            )

    ctrl_o_submit = run_case(binary, "insert_backspace", b"abc\x0f")
    if ctrl_o_submit != "abc":
        raise AssertionError(f"ctrl_o_submit expected 'abc', got {ctrl_o_submit!r}")

    ctrl_g_cancel = run_case(binary, "insert_backspace", b"abc\x07")
    if ctrl_g_cancel != "":
        raise AssertionError(f"ctrl_g_cancel expected empty string, got {ctrl_g_cancel!r}")

    assert_timed_case(
        binary,
        "esc_clear_then_enter",
        "insert_backspace",
        [b"abc", b"\x1b", b"\r"],
        "",
        initial_delay_s=0.12,
        step_delay_s=0.5,
    )
    assert_timed_case(
        binary,
        "esc_empty_continues",
        "insert_backspace",
        [b"\x1b", b"ok\r"],
        "ok",
        initial_delay_s=0.12,
        step_delay_s=0.5,
    )

    assert_multiline_history_navigation(binary)

    timed_history_cases = [
        ("history_prev_ctrl_p", "history_prev", [b"one\r", b"two\r", b"\x10\r"], "two"),
        ("history_prev_up", "history_prev", [b"one\r", b"two\r", UP + b"\r"], "two"),
        (
            "history_prev_shift_up",
            "history_prev",
            [b"one\r", b"two\r", SHIFT_UP + b"\r"],
            "two",
        ),
        (
            "history_prev_prev_ctrl_p",
            "history_prev_prev",
            [b"one\r", b"two\r", b"\x10\x10\r"],
            "one",
        ),
        (
            "history_next_ctrl_n",
            "history_next_empty",
            [b"one\r", b"two\r", b"\x10\x0e\r"],
            "",
        ),
        (
            "history_next_down",
            "history_next_empty",
            [b"one\r", b"two\r", b"\x10" + DOWN + b"\r"],
            "",
        ),
        (
            "history_prev_prefix_filter",
            "history_prev_edit",
            [b"alpha\r", b"alpine\r", b"al\x10\r"],
            "alpine",
        ),
    ]
    for label, scenario, chunks, expected in timed_history_cases:
        assert_timed_case(
            binary,
            label,
            scenario,
            chunks,
            expected,
            initial_delay_s=0.12,
            step_delay_s=0.5,
            wait_for_reprompt=True,
        )

    hist_latest = run_case(binary, "history_probe_latest", b"")
    if hist_latest != "echo two":
        raise AssertionError(
            f"history_probe_latest expected 'echo two', got {hist_latest!r}"
        )

    hist_prev = run_case(binary, "history_probe_previous", b"")
    if hist_prev != "echo one":
        raise AssertionError(
            f"history_probe_previous expected 'echo one', got {hist_prev!r}"
        )

    hist_removed = run_case(binary, "history_probe_remove_last", b"")
    if hist_removed != "echo one":
        raise AssertionError(
            f"history_probe_remove_last expected 'echo one', got {hist_removed!r}"
        )

    hist_count = run_case(binary, "history_probe_count", b"")
    if hist_count != "2":
        raise AssertionError(f"history_probe_count expected '2', got {hist_count!r}")

    yank_meta_dot = run_case(binary, "yank_last_arg_meta_dot", b"\x1b.\r")
    if yank_meta_dot != 'mv "dest dir"':
        raise AssertionError(
            f"yank_last_arg_meta_dot expected 'mv \"dest dir\"', got {yank_meta_dot!r}"
        )

    yank_meta_underscore = run_case(binary, "yank_last_arg_meta_underscore", b"\x1b_\r")
    if yank_meta_underscore != 'mv "dest dir"':
        raise AssertionError(
            "yank_last_arg_meta_underscore expected 'mv \"dest dir\"', got "
            f"{yank_meta_underscore!r}"
        )

    yank_repeat = run_case(binary, "yank_last_arg_repeat", b"\x1b.\x1b.\r")
    if yank_repeat != "open beta.txt":
        raise AssertionError(
            f"yank_last_arg_repeat expected 'open beta.txt', got {yank_repeat!r}"
        )

    yank_repeat_underscore = run_case(binary, "yank_last_arg_repeat", b"\x1b_\x1b_\r")
    if yank_repeat_underscore != "open beta.txt":
        raise AssertionError(
            "yank_last_arg_repeat alt+_ expected 'open beta.txt', got "
            f"{yank_repeat_underscore!r}"
        )

    yank_repeat_mixed = run_case(binary, "yank_last_arg_repeat", b"\x1b.\x1b_\r")
    if yank_repeat_mixed != "open beta.txt":
        raise AssertionError(
            "yank_last_arg_repeat mixed meta-. / meta-_ expected 'open beta.txt', got "
            f"{yank_repeat_mixed!r}"
        )

    yank_repeat_mixed_reverse = run_case(
        binary, "yank_last_arg_repeat", b"\x1b_\x1b.\r"
    )
    if yank_repeat_mixed_reverse != "open beta.txt":
        raise AssertionError(
            "yank_last_arg_repeat mixed meta-_ / meta-. expected 'open beta.txt', got "
            f"{yank_repeat_mixed_reverse!r}"
        )

    mouse_wheel_down = b"\x1b[<65;1;1M"
    mouse_wheel_down_shift = b"\x1b[<69;1;1M"
    mouse_release = b"\x1b[<3;1;1m"
    mouse_click_custom_second = mouse_left_click(6, 4)
    mouse_click_completion_second = mouse_left_click(6, 4)
    mouse_click_inline_hint = mouse_left_click(10, 1)

    assert_smart_mouse_selection_suspends(
        binary, "smart_mouse_prompt_selection", 1, 1, "abc"
    )
    assert_smart_mouse_selection_suspends(
        binary, "smart_mouse_status_selection", 1, 2, "x"
    )
    assert_smart_mouse_drag_cases(binary)

    smart_input_result, smart_input_output = run_case(
        binary,
        "smart_mouse_input_click",
        mouse_left_click(6, 1) + b"\r",
        capture_output=True,
    )
    if smart_input_result != "abc":
        raise AssertionError(
            f"smart input click expected 'abc', got {smart_input_result!r}"
        )
    mouse_enable = "\x1b[?1000h\x1b[?1006h"
    if smart_input_output.count(mouse_enable) != 1:
        raise AssertionError(
            "clicking editable input should keep smart mouse capture enabled: "
            f"output={smart_input_output!r}"
        )

    mouse_status_result, mouse_status_output = run_case(
        binary, "insert_backspace", F2 + b"x\x7f\r", capture_output=True
    )
    if mouse_status_result != "":
        raise AssertionError(
            f"mouse status toggle case expected empty result, got {mouse_status_result!r}"
        )
    normalized_mouse_status_output = normalize_terminal_output(mouse_status_output)
    if "Mouse clicking is enabled" not in normalized_mouse_status_output:
        raise AssertionError(
            "status line should show mouse indicator after toggle, got "
            f"normalized_output={normalized_mouse_status_output!r}"
        )
    if "complete:" not in normalized_mouse_status_output:
        raise AssertionError(
            "mouse indicator should not replace default status hints, got "
            f"normalized_output={normalized_mouse_status_output!r}"
        )
    mouse_top_index = normalized_mouse_status_output.rfind("Mouse clicking is enabled")
    hint_index = normalized_mouse_status_output.rfind("complete:")
    if hint_index >= 0 and mouse_top_index > hint_index:
        raise AssertionError(
            "mouse indicator should render above default status hints, got "
            f"normalized_output={normalized_mouse_status_output!r}"
        )

    mouse_nonempty_result, mouse_nonempty_output = run_case(
        binary, "mouse_status_nonempty_buffer", b"\r", capture_output=True
    )
    if mouse_nonempty_result != "x":
        raise AssertionError(
            "mouse non-empty buffer case expected 'x', got "
            f"{mouse_nonempty_result!r}"
        )
    normalized_mouse_nonempty_output = normalize_terminal_output(mouse_nonempty_output)
    if "Mouse clicking is enabled" not in normalized_mouse_nonempty_output:
        raise AssertionError(
            "non-empty mouse status case should show mouse indicator, got "
            f"normalized_output={normalized_mouse_nonempty_output!r}"
        )
    if "complete:" in normalized_mouse_nonempty_output:
        raise AssertionError(
            "default status hints should stay hidden when input buffer has content, got "
            f"normalized_output={normalized_mouse_nonempty_output!r}"
        )

    mouse_default_click = run_case(
        binary,
        "completion_many_menu_mouse_default_on",
        b"s\t" + mouse_click_completion_second + b"\r",
        initial_rows=24,
        initial_cols=80,
    )
    if mouse_default_click != "s02":
        raise AssertionError(
            "completion_many_menu with default mouse enabled expected 's02', got "
            f"{mouse_default_click!r}"
        )

    completion_focus_result, completion_focus_output = run_case(
        binary,
        "completion_many_menu_mouse_default_on",
        b"s\t"
        + mouse_left_press(1, 1)
        + FOCUS_IN
        + mouse_click_completion_second
        + b"\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if completion_focus_result != "s02":
        raise AssertionError(
            "completion menu should restore clicking after focus returns, got "
            f"{completion_focus_result!r}"
        )
    assert_menu_mouse_suspends_until_focus(
        completion_focus_output, "completion_menu_outside_click"
    )

    mouse_enable_sequence = "\x1b[?1000h\x1b[?1006h"
    mouse_disable_sequence = "\x1b[?1000l\x1b[?1006l"

    menu_off_result, menu_off_output = run_case(
        binary,
        "completion_many_menu_off",
        b"s\t" + mouse_click_completion_second + b"\r",
        capture_output=True,
    )
    if menu_off_result != "s02":
        raise AssertionError(
            "menu-only off mode should allow completion clicks immediately, got "
            f"{menu_off_result!r}"
        )
    menu_enable_index = menu_off_output.find(mouse_enable_sequence)
    menu_disable_index = menu_off_output.find(
        mouse_disable_sequence, menu_enable_index + len(mouse_enable_sequence)
    )
    if min(menu_enable_index, menu_disable_index) < 0 or menu_disable_index < menu_enable_index:
        raise AssertionError(
            "completion menu should acquire and release terminal mouse tracking: "
            f"output={menu_off_output!r}"
        )

    all_off_result, all_off_output = run_case(
        binary,
        "completion_many_menu_all_off",
        b"s\t" + mouse_click_completion_second + b"\r\r",
        capture_output=True,
    )
    if all_off_result != "s01":
        raise AssertionError(
            "all-off mode should ignore completion-menu mouse clicks, got "
            f"{all_off_result!r}"
        )
    if mouse_enable_sequence in all_off_output:
        raise AssertionError(
            "all-off mode must not acquire terminal mouse tracking: "
            f"output={all_off_output!r}"
        )

    mouse_hidden_result, mouse_hidden_output = run_case(
        binary,
        "insert_backspace_mouse_default_on_hidden_status",
        b"x\x7f\r",
        capture_output=True,
    )
    if mouse_hidden_result != "":
        raise AssertionError(
            "mouse hidden-status case expected empty result, got "
            f"{mouse_hidden_result!r}"
        )
    normalized_mouse_hidden_output = normalize_terminal_output(mouse_hidden_output)
    if "Mouse clicking is enabled" in normalized_mouse_hidden_output:
        raise AssertionError(
            "hidden mouse status toggle should suppress indicator text, got "
            f"normalized_output={normalized_mouse_hidden_output!r}"
        )
    if "complete:" not in normalized_mouse_hidden_output:
        raise AssertionError(
            "hidden mouse status toggle should keep default status hints visible, got "
            f"normalized_output={normalized_mouse_hidden_output!r}"
        )

    hist_scroll, hist_scroll_output = run_case(
        binary,
        "history_search_scroll",
        b"\x12" + mouse_wheel_down + b"\r",
        capture_output=True,
    )
    if hist_scroll != "history alpha":
        raise AssertionError(
            f"history_search_scroll expected 'history alpha', got {hist_scroll!r}"
        )
    normalized_hist_scroll_output = normalize_terminal_output(hist_scroll_output)
    if "Mouse clicking is enabled" not in normalized_hist_scroll_output:
        raise AssertionError(
            "history search menu should show mouse indicator when click support is active, got "
            f"normalized_output={normalized_hist_scroll_output!r}"
        )

    hist_footer, hist_footer_output = run_case(
        binary,
        "history_search_footer",
        b"\x12\x03\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if hist_footer != "history":
        raise AssertionError(
            f"history_search_footer expected 'history', got {hist_footer!r}"
        )
    normalized_hist_footer_output = re.sub(
        r"[←↵]\n", "", normalize_terminal_output(hist_footer_output)
    )
    if (
        "alt+s:sort" not in normalized_hist_footer_output
        or "alt+d:directory" not in normalized_hist_footer_output
        or "alt+n:nested" not in normalized_hist_footer_output
        or "alt+p:parents" not in normalized_hist_footer_output
        or "esc:cancel)" not in normalized_hist_footer_output
    ):
        raise AssertionError(
            "history search should keep its footer inside an 80-column viewport, got "
            f"normalized_output={normalized_hist_footer_output!r}"
        )

    hist_scroll_toggle = run_case(
        binary, "history_search_scroll", F2 + b"\x12" + mouse_wheel_down + b"\r"
    )
    if hist_scroll_toggle != "history beta":
        raise AssertionError(
            "history_search_scroll with mouse toggle expected 'history beta', got "
            f"{hist_scroll_toggle!r}"
        )

    hist_menu_off = run_case(
        binary, "history_search_menu_off", b"\x12" + mouse_wheel_down + b"\r"
    )
    if hist_menu_off != "history alpha":
        raise AssertionError(
            "menu-only off mode should enable history-menu wheel input, got "
            f"{hist_menu_off!r}"
        )

    hist_all_off = run_case(
        binary, "history_search_all_off", b"\x12" + mouse_wheel_down + b"\r"
    )
    if hist_all_off != "history beta":
        raise AssertionError(
            "all-off mode should ignore history-menu wheel input, got "
            f"{hist_all_off!r}"
        )

    # The scope indicators wrap the header at 80 columns, but not at 120.
    # Pin the viewport so click coordinates do not depend on the parent terminal.
    for columns, second_row in ((80, 5), (120, 4)):
        hist_click = run_case(
            binary,
            "history_search_scroll",
            b"\x12" + mouse_left_click(6, second_row) + b"!\r",
            initial_rows=24,
            initial_cols=columns,
        )
        if hist_click != "history alpha!":
            raise AssertionError(
                f"history_search_click at {columns} columns expected 'history alpha!', "
                f"got {hist_click!r}"
            )

    hist_search_ctrl_s = run_case(binary, "history_search_scroll", b"\x13\r")
    if hist_search_ctrl_s != "history beta":
        raise AssertionError(
            f"history_search_scroll ctrl+s expected 'history beta', got {hist_search_ctrl_s!r}"
        )

    typed_history_result, typed_history_output = run_case(
        binary,
        "history_search_typed_buffer",
        b"history\x12\t\r",
        capture_output=True,
    )
    if typed_history_result != "history":
        raise AssertionError(
            "history search should restore the typed buffer on cancel, got "
            f"{typed_history_result!r}"
        )
    normalized_typed_history_output = normalize_terminal_output(typed_history_output)
    if "3 matches found" not in normalized_typed_history_output:
        raise AssertionError(
            "history search should retain a real entry equal to the live input without adding "
            "another match, got "
            f"normalized_output={normalized_typed_history_output!r}"
        )
    if (
        "→ \n" not in normalized_typed_history_output
        or "→ history" in normalized_typed_history_output
    ):
        raise AssertionError(
            "history search should begin on a blank selection instead of repeating the input, got "
            f"normalized_output={normalized_typed_history_output!r}"
        )

    typed_history_down = run_case(
        binary, "history_search_typed_buffer", b"history\x12" + DOWN + b"\r"
    )
    if typed_history_down != "history":
        raise AssertionError(
            "history search Down should move from the blank selection to the newest match, got "
            f"{typed_history_down!r}"
        )

    hist_sort_alt_s = run_case(
        binary,
        "history_search_sort_alt_s",
        b"\x12" + ALT_S + b"\r",
    )
    if hist_sort_alt_s != "apple":
        raise AssertionError(
            f"history_search_sort_alt_s expected 'apple', got {hist_sort_alt_s!r}"
        )

    hist_timestamp_only, hist_timestamp_only_output = run_case(
        binary,
        "history_search_timestamp_without_exit_code",
        b"\x12\r",
        capture_output=True,
    )
    if hist_timestamp_only != "timestamp-only entry":
        raise AssertionError(
            "history_search_timestamp_without_exit_code expected 'timestamp-only entry', got "
            f"{hist_timestamp_only!r}"
        )
    normalized_timestamp_only_output = normalize_terminal_output(hist_timestamp_only_output)
    if not all(
        text in normalized_timestamp_only_output
        for text in ("timestamp-only entry", "[Time: timestamp-only-value]")
    ):
        raise AssertionError(
            "selected history entries should show a labeled timestamp when exit-code metadata "
            "is absent, got "
            f"normalized_output={normalized_timestamp_only_output!r}"
        )

    hist_expanded_metadata, hist_expanded_metadata_output = run_case(
        binary,
        "history_search_expanded_metadata",
        b"\x12\r",
        capture_output=True,
        initial_cols=80,
    )
    if hist_expanded_metadata != "metadata-rich entry":
        raise AssertionError(
            "history_search_expanded_metadata expected 'metadata-rich entry', got "
            f"{hist_expanded_metadata!r}"
        )
    normalized_expanded_metadata_output = normalize_terminal_output(
        hist_expanded_metadata_output
    )
    metadata_preview_start = normalized_expanded_metadata_output.find(
        "metadata-rich entry ["
    )
    metadata_preview_end = normalized_expanded_metadata_output.find(
        "]", metadata_preview_start
    )
    normalized_metadata_preview = ""
    if metadata_preview_start >= 0 and metadata_preview_end >= 0:
        normalized_metadata_preview = normalized_expanded_metadata_output[
            metadata_preview_start : metadata_preview_end + 1
        ]
        # The renderer writes a platform-specific wrap marker in UTF-8 locales,
        # followed by a screen-row boundary. Non-UTF-8 terminals omit the marker.
        normalized_metadata_preview = re.sub(r"[←↵]?\n", "", normalized_metadata_preview)
    expected_metadata_rows = (
        "Time: timestamp-label-value",
        "Frequency: 7",
        "Exit code: 2",
        "Working Directory: /tmp/metadata-example",
        "Note: first note line",
        "second note line",
        "Long Detail: a deliberately long metadata value",
        "tail-visible",
    )
    if not all(
        row in normalized_metadata_preview for row in expected_metadata_rows
    ):
        raise AssertionError(
            "selected history entries should expand every metadata field with labels, got "
            f"normalized_output={normalized_expanded_metadata_output!r}"
        )

    hist_sort_metadata = run_case(
        binary,
        "history_search_sort_default_metadata",
        b"\x12rank\r",
    )
    if hist_sort_metadata != "rank one":
        raise AssertionError(
            "history_search_sort_default_metadata expected 'rank one', got "
            f"{hist_sort_metadata!r}"
        )

    hist_sort_cycle_metadata, hist_sort_cycle_metadata_output = run_case(
        binary,
        "history_search_sort_cycle_metadata_tag",
        b"\x12" + ALT_S + ALT_S + ALT_S + b"\r",
        capture_output=True,
        initial_cols=80,
    )
    if hist_sort_cycle_metadata != "rank one":
        raise AssertionError(
            "history_search_sort_cycle_metadata_tag expected 'rank one', got "
            f"{hist_sort_cycle_metadata!r}"
        )
    # The scope settings can wrap the sort label onto another screen row.
    normalized_sort_cycle_metadata_output = re.sub(
        r"[←↵]?\n", "", normalize_terminal_output(hist_sort_cycle_metadata_output)
    )
    if "sort rank asc" not in normalized_sort_cycle_metadata_output:
        raise AssertionError(
            "history metadata cycle should report rank sort, got "
            f"normalized_output={normalized_sort_cycle_metadata_output!r}"
        )
    if not all(
        text in normalized_sort_cycle_metadata_output
        for text in ("sort rank asc", "rank one [Rank: 1]")
    ):
        raise AssertionError(
            "history metadata cycle should show the selected rank with its label, got "
            f"normalized_output={normalized_sort_cycle_metadata_output!r}"
        )

    hist_sort_cycle_nonpersistent = run_case_timed(
        binary,
        "history_search_sort_cycle_nonpersistent",
        [b"\x12a" + ALT_S + b"\r", b"\x12a\r"],
        wait_for_reprompt=True,
    )
    if hist_sort_cycle_nonpersistent != "carrot|apple":
        raise AssertionError(
            "history_search_sort_cycle_nonpersistent expected 'carrot|apple', got "
            f"{hist_sort_cycle_nonpersistent!r}"
        )

    hist_multiline_preview, hist_multiline_output = run_case(
        binary,
        "history_search_multiline",
        b"\x12\r",
        capture_output=True,
    )
    if hist_multiline_preview != "mlhist first line\nmlhist second line":
        raise AssertionError(
            "history_search_multiline expected full multiline command, got "
            f"{hist_multiline_preview!r}"
        )
    normalized_hist_multiline_output = normalize_terminal_output(hist_multiline_output)
    if "mlhist second line" not in normalized_hist_multiline_output:
        raise AssertionError(
            "history search menu should show full selected multiline command text, got "
            f"normalized_output={normalized_hist_multiline_output!r}"
        )
    if "mlhist first line..." in normalized_hist_multiline_output:
        raise AssertionError(
            "history search menu should render selected multiline command inline instead of "
            "truncating it, got "
            f"normalized_output={normalized_hist_multiline_output!r}"
        )

    tall_history_expected = "\n".join(
        f"tallhist line {line:02d}" for line in range(1, 13)
    )
    tall_history_result, tall_history_output = run_case(
        binary,
        "history_search_tall_multiline",
        b"\x12\r",
        capture_output=True,
        initial_rows=8,
        initial_cols=80,
    )
    if tall_history_result != tall_history_expected:
        raise AssertionError(
            "truncated history preview should still execute the complete command, got "
            f"{tall_history_result!r}"
        )
    normalized_tall_history_output = re.sub(
        r"[←↵]\n", "",
        normalize_terminal_output(tall_history_output.split("[IC_RESULT_BEGIN]", 1)[0]),
    )
    tall_history_footer_end = normalized_tall_history_output.find("esc:cancel)")
    if tall_history_footer_end < 0:
        raise AssertionError(
            "history search should preserve its footer below a capped multiline preview, got "
            f"output={normalized_tall_history_output!r}"
        )
    tall_history_menu_render = normalized_tall_history_output[:tall_history_footer_end]
    if "tallhist line 03" in tall_history_menu_render:
        raise AssertionError(
            "history search attempted to render a multiline preview taller than its terminal "
            f"row budget: output={tall_history_menu_render!r}"
        )
    if "tallhist line 02..." not in tall_history_menu_render:
        raise AssertionError(
            "history search should mark a terminal-capped multiline preview as truncated, got "
            f"output={tall_history_menu_render!r}"
        )

    hist_multiline_edit = run_case(
        binary,
        "history_search_multiline",
        b"\x12\t!\r",
    )
    if hist_multiline_edit != "mlhist first line\nmlhist second line!":
        raise AssertionError(
            "editing a multiline history-search result should start at the end of the buffer, "
            f"got {hist_multiline_edit!r}"
        )

    history_multiline_prompt_output = run_resize_case(
        binary,
        "history_search_multiline_prompt",
        [("send", b"\x12"), ("wait", "history search:"), ("idle", 0.05)],
        return_after_actions=True,
    )
    assert_menu_replaces_multiline_prompt(
        history_multiline_prompt_output, "history search:"
    )
    history_multiline_prompt_result = run_case_timed(
        binary,
        "history_search_multiline_prompt",
        [b"\x12", b"\x1b", b"\r"],
        step_delay_s=0.5,
    )
    if history_multiline_prompt_result != "history":
        raise AssertionError(
            "history search should restore a multi-line primary prompt and its input on cancel, "
            f"got {history_multiline_prompt_result!r}"
        )

    palette_multiline_prompt_output = run_resize_case(
        binary,
        "command_palette_multiline_prompt",
        [("send", ALT_P), ("wait", "command palette:"), ("idle", 0.05)],
        return_after_actions=True,
    )
    assert_menu_replaces_multiline_prompt(
        palette_multiline_prompt_output, "command palette:"
    )
    palette_multiline_prompt_result = run_case_timed(
        binary,
        "command_palette_multiline_prompt",
        [ALT_P, b"\x1b", b"\r"],
        step_delay_s=0.5,
    )
    if palette_multiline_prompt_result != "keep":
        raise AssertionError(
            "command palette should restore a multi-line primary prompt and its input on cancel, "
            f"got {palette_multiline_prompt_result!r}"
        )

    custom_menu_result, custom_menu_output = run_case(
        binary, "custom_menu_runoff", F3 + DOWN + b"\r\r", capture_output=True
    )
    if custom_menu_result != "restart":
        raise AssertionError(
            "runoff custom menu should return the selected original item index, "
            f"got {custom_menu_result!r}"
        )
    normalized_custom_menu_output = normalize_terminal_output(custom_menu_output)
    if "custom actions:" not in normalized_custom_menu_output or not all(
        label in normalized_custom_menu_output
        for label in ("Show status", "Restart service", "Open logs")
    ):
        raise AssertionError(
            "runoff custom menu should render the application-provided prompt and items, "
            f"got {normalized_custom_menu_output!r}"
        )
    if "CUSTOM-MENU-EXPANDED-DESCRIPTION" not in normalized_custom_menu_output:
        raise AssertionError(
            "the selected custom-menu item should expand its full description, "
            f"got {normalized_custom_menu_output!r}"
        )

    custom_menu_filtered = run_case(
        binary, "custom_menu_runoff", F3 + b"daemon\r\r"
    )
    if custom_menu_filtered != "restart":
        raise AssertionError(
            "custom menu should filter on hidden keywords and preserve source indices, "
            f"got {custom_menu_filtered!r}"
        )

    custom_menu_focus_result, custom_menu_focus_output = run_case(
        binary,
        "custom_menu_mouse_focus",
        F3
        + mouse_left_press(1, 1)
        + FOCUS_IN
        + mouse_click_custom_second
        + b"\r",
        capture_output=True,
    )
    if custom_menu_focus_result != "restart":
        raise AssertionError(
            "custom menu should restore clicking after focus returns, got "
            f"{custom_menu_focus_result!r}"
        )
    assert_menu_mouse_suspends_until_focus(
        custom_menu_focus_output, "custom_menu_outside_click"
    )

    custom_menu_cancelled = run_case_timed(
        binary, "custom_menu_runoff", [F3, b"\x1b", b"\r"], step_delay_s=0.5
    )
    if custom_menu_cancelled != "keep":
        raise AssertionError(
            "cancelling a runoff custom menu should restore the original readline buffer, "
            f"got {custom_menu_cancelled!r}"
        )

    assert_completion_auto_menu_cases(binary)

    comp_single = run_case(binary, "completion_single_tab", b"hel\t\r")
    if comp_single != "hello":
        raise AssertionError(
            f"completion_single_tab expected 'hello', got {comp_single!r}"
        )

    comp_single_hint_click = run_case(
        binary,
        "hint_clears_on_empty_line",
        F2 + b"hel" + mouse_click_inline_hint + b"\r",
    )
    if comp_single_hint_click != "hello":
        raise AssertionError(
            "mouse click on inline hint expected 'hello', got "
            f"{comp_single_hint_click!r}"
        )

    comp_single_type = run_case(binary, "completion_single_then_type", b"hel\t!\r")
    if comp_single_type != "hello!":
        raise AssertionError(
            f"completion_single_then_type expected 'hello!', got {comp_single_type!r}"
        )

    comp_midline = run_case(binary, "completion_midline_single", b"\t\r")
    if comp_midline != "say hello":
        raise AssertionError(
            f"completion_midline_single expected 'say hello', got {comp_midline!r}"
        )

    completion_alias_cases = [
        ("completion_ctrl_f_at_eol", b"hel\x06\r", "hello"),
        ("completion_right_arrow_at_eol", b"hel" + RIGHT + b"\r", "hello"),
        ("completion_word_next_at_eol", b"hel" + WORD_NEXT + b"\r", "hello"),
        ("completion_alt_f_at_eol", b"hel\x1bf\r", "hello"),
    ]
    for label, key_bytes, expected in completion_alias_cases:
        assert_case(binary, label, "completion_single_tab", key_bytes, expected)

    assert_timed_case(
        binary,
        "completion_alt_question",
        "completion_single_tab",
        [b"hel", b"\x1b?", b"\r"],
        "hello",
        initial_delay_s=0.12,
        step_delay_s=0.2,
    )

    empty_hint_result, empty_hint_output = run_case(
        binary, "hint_clears_on_empty_line", b" \x7f\r", capture_output=True
    )
    if empty_hint_result != "":
        raise AssertionError(
            "hint_clears_on_empty_line expected empty string, got "
            f"{empty_hint_result!r}"
        )
    assert_last_prompt_suffix("hint_clears_on_empty_line", empty_hint_output, "")

    comp_nomatch = run_case(binary, "completion_no_match", b"xyz\t\r")
    if comp_nomatch != "xyz":
        raise AssertionError(
            f"completion_no_match expected 'xyz', got {comp_nomatch!r}"
        )

    spell_submit_disabled = run_case(binary, "enter_spell_single_disabled", b"hlelo\r")
    if spell_submit_disabled != "hlelo":
        raise AssertionError(
            "enter_spell_single_disabled expected 'hlelo', got "
            f"{spell_submit_disabled!r}"
        )

    spell_submit_enabled = run_case(binary, "enter_spell_single_enabled", b"hlelo\r")
    if spell_submit_enabled != "hello":
        raise AssertionError(
            "enter_spell_single_enabled expected 'hello', got "
            f"{spell_submit_enabled!r}"
        )

    spell_status_result, spell_status_output = run_case(
        binary, "spell_status_delayed", b"hlelo\r", capture_output=True
    )
    if spell_status_result != "hlelo":
        raise AssertionError(
            f"spell_status_delayed expected 'hlelo', got {spell_status_result!r}"
        )
    normalized_spell_status_output = normalize_terminal_output(spell_status_output)
    if "spell: hlelo -> hello" not in normalized_spell_status_output:
        raise AssertionError(
            "delayed hints should immediately render a current-token spell status, got "
            f"normalized_output={normalized_spell_status_output!r}"
        )

    cross_token_result, cross_token_output = run_case(
        binary, "spell_status_cross_token", b"hlelo add\r", capture_output=True
    )
    if cross_token_result != "hlelo add":
        raise AssertionError(
            f"spell_status_cross_token expected 'hlelo add', got {cross_token_result!r}"
        )
    normalized_cross_token_output = normalize_terminal_output(cross_token_output)
    if "spell:" in normalized_cross_token_output:
        raise AssertionError(
            "spell status should reject a correction whose apply range crosses tokens, got "
            f"normalized_output={normalized_cross_token_output!r}"
        )

    cross_token_tab = run_case(
        binary, "spell_status_cross_token", b"hlelo add\t\r"
    )
    if cross_token_tab != "hlelo add":
        raise AssertionError(
            "Tab should reject a spell correction whose apply range crosses tokens, got "
            f"{cross_token_tab!r}"
        )

    spell_mixed_tab = run_case(binary, "completion_spell_mixed_tab", b"hlelo\t\r")
    if spell_mixed_tab != "hello":
        raise AssertionError(
            "completion_spell_mixed_tab should accept the advertised spell correction first, "
            f"got {spell_mixed_tab!r}"
        )

    comp_common = run_case(binary, "completion_dual_common_prefix", b"pla\t\r\r")
    if comp_common != "planet":
        raise AssertionError(
            f"completion_dual_common_prefix expected 'planet', got {comp_common!r}"
        )

    comp_footer, comp_footer_output = run_case(
        binary,
        "completion_dual_footer",
        b"pla\t\r\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if comp_footer != "planet":
        raise AssertionError(
            f"completion_dual_footer expected 'planet', got {comp_footer!r}"
        )
    normalized_comp_footer_output = re.sub(
        r"[←↵]\n", "", normalize_terminal_output(comp_footer_output)
    )
    if "pgup/pgdn:page ctrl+j:resize esc:cancel" not in normalized_comp_footer_output:
        raise AssertionError(
            "completion menus should show a footer even when every candidate fits, got "
            f"normalized_output={normalized_comp_footer_output!r}"
        )

    for rows in (8, 24):
        assert_resize_case(
            binary, "completion_footer_spacing", "completion_many_menu_off",
            [("send", b"s\t"), ("wait", "enter/right:accept"), ("idle", 0.1),
             ("check", lambda output, rows=rows: assert_completion_footer_spacing(
                 output, rows, 100, "enter/right:accept")),
             ("send", b"\r\r")],
            "s01", initial_rows=rows, initial_cols=100,
        )

    comp_preview_first = run_case(binary, "completion_many_menu_preview", b"s\t\r\r")
    if comp_preview_first != "s01":
        raise AssertionError(
            "completion menu should initially select its first candidate when preview is enabled, "
            f"got {comp_preview_first!r}"
        )

    comp_scroll, comp_scroll_output = run_case(
        binary,
        "completion_many_menu_off",
        b"s\t" + mouse_wheel_down + b"\r\r",
        capture_output=True,
        initial_rows=24,
        initial_cols=80,
    )
    if comp_scroll != "s02":
        raise AssertionError(f"completion menu should scroll immediately, got {comp_scroll!r}")
    normalized_comp_scroll_output = normalize_terminal_output(comp_scroll_output)
    for text in ("Completions", "s11", "s12", "Mouse clicking is enabled"):
        if text not in normalized_comp_scroll_output:
            raise AssertionError(
                f"completion menu should show the full list immediately; missing {text!r}: "
                f"{normalized_comp_scroll_output!r}"
            )
    if (
        "ctrl+j:collapse" in normalized_comp_scroll_output
        or ":expand" in normalized_comp_scroll_output
    ):
        raise AssertionError("completion menu must not advertise expansion/collapse controls")

    for scenario, keys, expected in (
        ("completion_many_menu_off", b"s\t" + mouse_click_completion_second + b"\r", "s02"),
        ("completion_many_menu", F2 + b"s\t" + mouse_click_completion_second + b"\r", "s02"),
        ("completion_many_menu", b"s\t" + F2 + b"\r\r", "s01"),
        ("completion_many_menu", b"s\t" + F2 + mouse_click_completion_second + b"\r", "s02"),
        (
            "completion_many_menu_custom_mouse_toggle",
            b"s\t" + F3 + mouse_click_completion_second + b"\r",
            "s02",
        ),
        (
            "completion_many_menu_mouse_default_on",
            b"s\t" + mouse_wheel_down + mouse_release + b"\r\r",
            "s02",
        ),
        (
            "completion_many_menu_mouse_default_on",
            b"s\t" + mouse_wheel_down_shift + b"\r\r",
            "s02",
        ),
        # Arrow and Tab navigation can reach beyond the old ten-item limit immediately.
        ("completion_many_menu", b"s\t" + DOWN * 10 + b"\r\r", "s11"),
        ("completion_many_menu", b"s\t" + b"\t" * 11 + b"\r\r", "s12"),
        ("completion_many_menu", b"s\t" + UP + b"\r\r", "s12"),
    ):
        result = run_case(binary, scenario, keys, initial_rows=24, initial_cols=80)
        if result != expected:
            raise AssertionError(f"{scenario}: keys={keys!r}, expected {expected!r}, got {result!r}")

    comp_multiline_preview, comp_multiline_preview_output = run_case(
        binary,
        "completion_many_menu_multiline",
        b"m\t" + DOWN + b"\r\r",
        capture_output=True,
    )
    if comp_multiline_preview != "m02":
        raise AssertionError(
            "completion_many_menu_multiline expected 'm02', got "
            f"{comp_multiline_preview!r}"
        )
    normalized_comp_multiline_preview_output = normalize_terminal_output(
        comp_multiline_preview_output
    )
    if (
        "→ m02 first line..." not in normalized_comp_multiline_preview_output
        and "> m02 first line..." not in normalized_comp_multiline_preview_output
    ):
        raise AssertionError(
            "expanded completion menu should keep the selected multiline candidate collapsed, got "
            f"normalized_output={normalized_comp_multiline_preview_output!r}"
        )
    if "m02 second line" in normalized_comp_multiline_preview_output:
        raise AssertionError(
            "expanded completion menu should not expand selected multiline candidate text, got "
            f"normalized_output={normalized_comp_multiline_preview_output!r}"
        )
    if "m05 (abbr first line...)" not in normalized_comp_multiline_preview_output:
        raise AssertionError(
            "expanded completion menu should collapse an unselected multiline source, got "
            f"normalized_output={normalized_comp_multiline_preview_output!r}"
        )
    if "abbr second line" in normalized_comp_multiline_preview_output:
        raise AssertionError(
            "expanded completion menu should keep unselected multiline sources to one row, got "
            f"normalized_output={normalized_comp_multiline_preview_output!r}"
        )

    comp_multiline_replacement, comp_multiline_replacement_output = run_case(
        binary,
        "completion_many_menu_multiline_replacement",
        b"m\t" + DOWN + b"\r\r",
        capture_output=True,
        initial_rows=8,
        initial_cols=80,
    )
    expected_multiline_replacement = "m02 first line\nm02 second line"
    if comp_multiline_replacement != expected_multiline_replacement:
        raise AssertionError(
            "completion_many_menu_multiline_replacement expected "
            f"{expected_multiline_replacement!r}, got {comp_multiline_replacement!r}"
        )
    normalized_comp_multiline_replacement_output = normalize_terminal_output(
        comp_multiline_replacement_output
    )
    # Reserve the scroll hint and both wrapped footer rows below the preview.
    if "(10 more below)" not in normalized_comp_multiline_replacement_output:
        raise AssertionError(
            "completion menu should reserve its footer below the multiline preview, got "
            f"normalized_output={normalized_comp_multiline_replacement_output!r}"
        )
    if "esc:cancel)" not in normalized_comp_multiline_replacement_output:
        raise AssertionError(
            "expanded completion menu should keep its footer inside a short viewport, got "
            f"normalized_output={normalized_comp_multiline_replacement_output!r}"
        )
    preview_menu_prefix = "pty> m02 first line\n   > m02 second line\nCompletions"
    preview_menu = normalized_comp_multiline_replacement_output.rsplit(preview_menu_prefix, 1)[-1]
    entries = re.findall(r"^[ →>]+m\d{2}\b", preview_menu.split("esc:cancel)", 1)[0], re.M)
    if len(entries) != 2:
        raise AssertionError(
            "expanded completion menu rendered too many rows for the multiline preview buffer, got "
            f"normalized_output={normalized_comp_multiline_replacement_output!r}"
        )

    tall_scenario = "completion_many_menu_tall_replacement"
    for scenario, rows, prefix_rows in (
        (tall_scenario, 8, 0),
        (tall_scenario, 24, 0),
        ("completion_many_menu_tall_flattened", 24, 0),
        ("completion_many_menu_tall_wrapped_input", 8, 0),
        ("completion_many_menu_tall_marker_off", 8, 0),
        ("completion_many_menu_tall_prompt_prefix", 8, 2),
    ):
        output = observe_resize_case(
            binary,
            scenario,
            [
                ("send", b"m\t" + DOWN),
                ("idle", 0.1),
            ],
            initial_rows=rows,
            initial_cols=80,
        )
        assert_completion_preview_fits(output, rows, 80, prefix_rows)

    for cols in (80, 40):
        output = observe_resize_case(
            binary,
            tall_scenario,
            [
                ("send", b"m\t" + DOWN),
                ("wait", "preview line 15..."),
                ("resize", (8, cols)),
                # Wake the PTY read on platforms that restart it after SIGWINCH.
                ("send", FOCUS_IN),
                ("wait", "preview line 03..." if cols == 80 else "preview line 02..."),
                ("idle", 0.1),
            ],
            initial_rows=24,
            initial_cols=80,
        )
        assert_completion_preview_fits(output, 8, cols)

    expected_tall_replacement = "m02 first line\n" + "\n".join(
        f"preview line {line:02d}" for line in range(2, 21)
    )
    for keys, expected in (
        (b"m\t" + DOWN + b"\r\r", expected_tall_replacement),
        (b"m\t" + DOWN + DOWN + b"\r\r", "m03"),
        # Resizing must preserve the full replacement even when the preview is shortened.
        (b"m\t" + DOWN + b"\n\r\r", expected_tall_replacement),
        (b"m\t" + DOWN + b"\n\n\r\r", expected_tall_replacement),
    ):
        result = run_case(binary, tall_scenario, keys, initial_rows=8, initial_cols=80)
        if result != expected:
            raise AssertionError(
                f"shortened completion preview changed acceptance/navigation: "
                f"keys={keys!r}, expected={expected!r}, got={result!r}"
            )

    cancelled_preview = run_resize_case(
        binary,
        tall_scenario,
        [
            ("send", b"m\t" + DOWN),
            ("wait", "preview line 03..."),
            ("send", b"\x1b"),
            ("idle", 0.5),
            ("send", b"\r"),
        ],
        initial_rows=8,
        initial_cols=80,
    )
    if cancelled_preview != "m":
        raise AssertionError(
            f"cancelling a shortened preview changed the input: {cancelled_preview!r}"
        )

    help_result, help_output = run_case(
        binary, "insert_backspace", F2 + b"ab" + F1 + b"c\r", capture_output=True
    )
    if help_result != "abc":
        raise AssertionError(f"show_help expected 'abc', got {help_result!r}")
    normalized_help_output = normalize_terminal_output(help_output)
    if "Navigation:" not in normalized_help_output or "Editing:" not in normalized_help_output:
        raise AssertionError(
            "show_help expected rendered help headings, got "
            f"normalized_output={normalized_help_output!r}"
        )
    if (
        "toggle mouse reporting for this prompt (Mouse clicking is enabled; press "
        not in normalized_help_output
        or " to disable)" not in normalized_help_output
    ):
        raise AssertionError(
            "show_help should mark mouse toggle binding as enabled after F2, got "
            f"normalized_output={normalized_help_output!r}"
        )

    vim_cases = [
        ("vim_alt_h_left", "vim_insert_backspace", b"ab\x1bhX\r", "aXb"),
        ("vim_alt_l_midline", "vim_insert_backspace", b"ab\x01\x1blX\r", "aXb"),
        ("vim_alt_l_complete", "vim_completion_single_tab", b"hel\x1bl\r", "hello"),
        (
            "vim_alt_w_word_next",
            "vim_insert_backspace",
            b"alpha beta\x01\x1bwX\r",
            "alpha Xbeta",
        ),
        ("vim_alt_w_complete", "vim_completion_single_tab", b"hel\x1bw\r", "hello"),
    ]
    for label, scenario, key_bytes, expected in vim_cases:
        assert_case(binary, label, scenario, key_bytes, expected)

    vim_timed_cases = [
        ("vim_alt_k_history_prev", "vim_history_prev", [b"one\r", b"two\r", b"\x1bk\r"], "two"),
        (
            "vim_alt_j_history_next",
            "vim_history_next_empty",
            [b"one\r", b"two\r", b"\x1bk\x1bj\r"],
            "",
        ),
    ]
    for label, scenario, chunks, expected in vim_timed_cases:
        assert_timed_case(
            binary,
            label,
            scenario,
            chunks,
            expected,
            initial_delay_s=0.12,
            step_delay_s=0.5,
            wait_for_reprompt=True,
        )

    vim_multiline_cases = [
        # Alt+K/Alt+J share Up/Down behavior: history at the buffer end,
        # visual row movement while editing within the buffer.
        (
            "vim_alt_k_row_up",
            "vim_multiline_ctrl_j_insert_newline",
            b"ab\x0acd\x0aef" + LEFT + b"\x1bkX\r",
            "ab\ncXd\nef",
        ),
        (
            "vim_alt_j_row_down",
            "vim_multiline_ctrl_j_insert_newline",
            b"ab\x0acd\x0aef" + PAGEUP + b"\x1bjX\r",
            "ab\nXcd\nef",
        ),
        (
            "vim_alt_k_at_end_without_history",
            "vim_multiline_ctrl_j_insert_newline",
            b"ab\x0acd\x0aef\x1bkX\r",
            "ab\ncd\nefX",
        ),
        (
            "vim_alt_k_skips_multiline_history_rows",
            "vim_history_navigation_multiline",
            b"\x1bk\x1bkX\r",
            "old first\n\nold middle\nold lastX",
        ),
        (
            "vim_alt_j_skips_multiline_history_rows",
            "vim_history_navigation_multiline",
            b"\x1bk\x1bk\x1bjX\r",
            "new first\nnew middle\nnew lastX",
        ),
        (
            "vim_alt_j_restores_multiline_draft_at_end",
            "vim_history_navigation_multiline",
            b"draft\x0asaved\x1bk\x1bk\x1bj\x1bjX\r",
            "draft\nsavedX",
        ),
    ]
    for label, scenario, key_bytes, expected in vim_multiline_cases:
        assert_case(binary, label, scenario, key_bytes, expected)

    bp_start = b"\x1b[200~"
    bp_end = b"\x1b[201~"

    paste_result, paste_output = run_case(
        binary,
        "paste_status_callback",
        bp_start + b"pasted-status" + bp_end + b"\r",
        capture_output=True,
    )
    if paste_result != "batched-status" or "PASTE-STATUS-READY" not in paste_output:
        raise AssertionError(
            "bracketed paste must update status only after the complete paste: "
            f"result={paste_result!r}, output={paste_output!r}"
        )

    assert_case(
        binary,
        "bracketed_paste_plain",
        "insert_backspace",
        bp_start + b"hello world" + bp_end + b"\r",
        "hello world",
    )
    assert_case(
        binary,
        "bracketed_paste_empty",
        "insert_backspace",
        bp_start + bp_end + b"\r",
        "",
    )
    assert_case(
        binary,
        "bracketed_paste_wrapped",
        "insert_backspace",
        b"pre-" + bp_start + b"MID" + bp_end + b"-post\r",
        "pre-MID-post",
    )
    assert_case(
        binary,
        "bracketed_paste_initial_append",
        "append_to_initial_input",
        bp_start + b"cd" + bp_end + b"\r",
        "abcd",
    )
    assert_case(
        binary,
        "bracketed_paste_midline",
        "cursor_move_insert",
        b"\x02" + bp_start + b"Z" + bp_end + b"\r",
        "aZb",
    )
    assert_case(
        binary,
        "bracketed_paste_two_blocks",
        "insert_backspace",
        bp_start + b"alpha" + bp_end + bp_start + b"beta" + bp_end + b"\r",
        "alphabeta",
    )
    assert_case(
        binary,
        "bracketed_paste_end_without_start",
        "insert_backspace",
        bp_end + b"tail\r",
        "tail",
    )
    assert_case(
        binary,
        "bracketed_paste_singleline_cr",
        "insert_backspace",
        bp_start + b"one\rtwo" + bp_end + b"\r",
        "onetwo",
    )
    assert_case(
        binary,
        "bracketed_paste_multiline_cr",
        "multiline_ctrl_j_insert_newline",
        bp_start + b"one\rtwo" + bp_end + b"\r",
        "one\ntwo",
    )
    assert_case(
        binary,
        "bracketed_paste_multiline_multi_cr",
        "multiline_ctrl_j_insert_newline",
        bp_start + b"a\rb\rc" + bp_end + b"\r",
        "a\nb\nc",
    )

    assert_timed_case(
        binary,
        "bracketed_paste_chunked",
        "insert_backspace",
        [bp_start, b"chunked", bp_end, b"\r"],
        "chunked",
    )
    assert_timed_case(
        binary,
        "bracketed_paste_multiline_chunked",
        "multiline_ctrl_j_insert_newline",
        [bp_start, b"row1\r", b"row2", bp_end, b"\r"],
        "row1\nrow2",
    )
    assert_timed_case(
        binary,
        "bracketed_paste_chunked_two_blocks",
        "insert_backspace",
        [bp_start, b"alpha", bp_end, bp_start, b"beta", bp_end, b"\r"],
        "alphabeta",
    )

    assert_case(
        binary,
        "bracketed_paste_repeated_start_single_end",
        "insert_backspace",
        bp_start + bp_start + b"nested" + bp_end + b"\r",
        "nested",
    )
    assert_case(
        binary,
        "bracketed_paste_repeated_start_double_end",
        "insert_backspace",
        bp_start + bp_start + b"nested" + bp_end + bp_end + b"\r",
        "nested",
    )
    assert_case(
        binary,
        "bracketed_paste_end_then_start",
        "insert_backspace",
        bp_end + bp_start + b"abc" + bp_end + b"\r",
        "abc",
    )
    assert_case(
        binary,
        "bracketed_paste_double_end_no_start",
        "insert_backspace",
        bp_end + bp_end + b"tail\r",
        "tail",
    )
    assert_case(
        binary,
        "bracketed_paste_empty_then_payload",
        "insert_backspace",
        bp_start + bp_end + bp_start + b"payload" + bp_end + b"\r",
        "payload",
    )
    assert_case(
        binary,
        "bracketed_paste_malformed_start_final",
        "insert_backspace",
        b"\x1b[200Xabc\r",
        "abc",
    )
    assert_case(
        binary,
        "bracketed_paste_malformed_end_final",
        "insert_backspace",
        b"\x1b[201Xtail\r",
        "tail",
    )
    assert_case(
        binary,
        "bracketed_paste_unknown_vt_code_outside_paste",
        "insert_backspace",
        b"\x1b[202~z\r",
        "z",
    )

    assert_timed_case(
        binary,
        "bracketed_paste_split_start_marker",
        "insert_backspace",
        [b"\x1b[200", b"~split", bp_end, b"\r"],
        "split",
        step_delay_s=0.003,
        poll_interval_s=0.001,
    )
    assert_timed_case(
        binary,
        "bracketed_paste_split_end_marker",
        "insert_backspace",
        [bp_start, b"splitend", b"\x1b[201", b"~", b"\r"],
        "splitend",
        step_delay_s=0.003,
        poll_interval_s=0.001,
    )
    assert_timed_case(
        binary,
        "bracketed_paste_split_both_markers",
        "insert_backspace",
        [b"\x1b[200", b"~ab", b"\x1b[201", b"~", b"\r"],
        "ab",
        step_delay_s=0.003,
        poll_interval_s=0.001,
    )

    # TODO: Re-enable PTY resize/reflow coverage once terminal resize
    # propagation is stable across local and CI environments.
    # reflow_single_line = "pty> abcdefghij"
    # reflow_cursor_boundary = "pty> abc\n   > "
    # shell_prompt_boundary = (
    #     "pty> CJsShell git:(master) x abc\n                           > "
    # )
    # reflow_wrapped = "pty> ab↵\n   > cd↵\n   > ef↵\n   > gh↵\n   > ij"
    # reflow_wrapped_tail = "↵\n   > ij"
    #
    # assert_resize_case(
    #     binary,
    #     "typed_wrap_cursor_boundary",
    #     "insert_backspace",
    #     [
    #         ("resize", 8),
    #         ("send", b"abc"),
    #         ("wait", reflow_cursor_boundary),
    #         ("send", b"\r"),
    #     ],
    #     "abc",
    # )
    # assert_resize_case(
    #     binary,
    #     "shell_prompt_wrap_boundary",
    #     "shell_prompt_wrap_boundary",
    #     [
    #         ("resize", 32),
    #         ("send", b"abc"),
    #         ("wait", shell_prompt_boundary),
    #         ("send", b"\r"),
    #     ],
    #     "abc",
    # )
    #
    # These regression checks verify resize-driven reflow, including idle redraws
    # while the editor is blocked waiting for input.
    # assert_resize_case(
    #     binary,
    #     "resize_reflow_while_waiting_for_input",
    #     "resize_reflow_initial_input",
    #     [
    #         ("wait", reflow_single_line),
    #         ("idle", 0.05),
    #         ("resize", 8),
    #         ("wait", reflow_wrapped),
    #         ("send", b"\r"),
    #     ],
    #     "abcdefghij",
    #     poll_interval_s=0.001,
    # )
    #
    # resize_observation = observe_resize_case(
    #     binary,
    #     "resize_reflow_initial_input",
    #     [
    #         ("wait", reflow_single_line),
    #         ("resize", 8),
    #         ("wait", reflow_wrapped),
    #     ],
    #     poll_interval_s=0.001,
    # )
    # if re.search(r"\x1b\[[1-9][0-9]*A", resize_observation):
    #     raise AssertionError(
    #         "resize_reflow_initial_input unexpectedly moved the cursor above the "
    #         f"existing prompt origin: output={resize_observation!r}"
    #     )
    #
    # height_resize_observation = observe_resize_case(
    #     binary,
    #     "resize_reflow_initial_input",
    #     [
    #         ("wait", reflow_wrapped),
    #         ("resize", (3, 8)),
    #         ("idle", 0.05),
    #     ],
    #     initial_rows=6,
    #     initial_cols=8,
    #     poll_interval_s=0.001,
    # )
    # height_resize_ups = re.findall(r"\x1b\[(\d+)A", height_resize_observation)
    # if not height_resize_ups or int(height_resize_ups[-1]) != 4:
    #     raise AssertionError(
    #         "height-only resize should walk back using the previous visible cursor row "
    #         f"before redrawing, output={height_resize_observation!r}"
    #     )
    #
    # assert_resize_case(
    #     binary,
    #     "resize_reflow_initial_input",
    #     "resize_reflow_initial_input",
    #     [
    #         ("wait", reflow_single_line),
    #         ("resize", 8),
    #         ("wait", reflow_wrapped),
    #         ("send", b"\r"),
    #     ],
    #     "abcdefghij",
    # )
    # assert_resize_case(
    #     binary,
    #     "resize_reflow_typed_input_expand",
    #     "resize_reflow_typed_input",
    #     [
    #         ("resize", 8),
    #         ("send", b"abcdefghij"),
    #         ("wait", reflow_wrapped_tail),
    #         ("resize", 40),
    #         ("wait", reflow_single_line),
    #         ("send", b"\r"),
    #     ],
    #     "abcdefghij",
    # )

    prompt_guard_expectations = [
        ("prompt_guard_visible_text", True),
        ("prompt_guard_tab_only", True),
        ("prompt_guard_escape_only", False),
        ("prompt_guard_osc_only", False),
        ("prompt_guard_newline_reset", False),
        ("prompt_guard_escape_then_visible", True),
        ("prompt_guard_spaces_only", True),
        ("prompt_guard_controls_only", False),
        ("prompt_guard_carriage_return_only", False),
        ("prompt_guard_visible_then_carriage_return", False),
        ("prompt_guard_visible_then_carriage_return_clear", False),
        ("prompt_guard_forced_visible_line_start", False),
        ("prompt_guard_visible_then_newline", False),
        ("prompt_guard_newline_then_visible", True),
        ("prompt_guard_double_newline_reset", False),
        ("prompt_guard_escape_then_space", True),
        ("prompt_guard_escape_then_newline_then_visible", True),
        ("prompt_guard_visible_then_newline_then_escape", False),
        ("prompt_guard_bracketed_toggle_only", False),
        ("prompt_guard_bracketed_toggle_then_tab", True),
        ("prompt_guard_utf8_visible", True),
        ("prompt_guard_osc_then_space", True),
        ("prompt_guard_region_marking_external_visible", True),
    ]
    for scenario, expect_marker in prompt_guard_expectations:
        assert_prompt_guard_case(binary, scenario, expect_marker)

    print(f"All {PTY_CASE_COUNT} PTY isocline integration tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
