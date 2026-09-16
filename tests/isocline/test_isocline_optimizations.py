#!/usr/bin/env python3

# test_isocline_optimizations.py
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

import sys

import test_isocline_pty as pty_tests
from test_isocline_pty import DOWN, F3, LEFT, UP, assert_case, assert_timed_case


def main(binary: str) -> None:
    compressed_undo_cases = [
        (
            "undo_utf8_middle_edit",
            "λ🌍abcdef".encode() + LEFT * 3 + b"X\x1a\x19\r",
            "λ🌍abcXdef",
        ),
        (
            "undo_delete_and_branch",
            b"abcdef" + LEFT * 3 + b"\x1b[3~\x1a\x19\x1aX\x19\r",
            "abcXdef",
        ),
        (
            "undo_whole_line_delete",
            b"abcdefgh\x15\x1a\x19\x1aq\r",
            "abcdefghq",
        ),
        (
            "undo_large_paste_keeps_character_steps",
            b"\x1b[200~" + b"x" * 2048 + b"\x1b[201~\x1a\x1a\x19\r",
            "x" * 2047,
        ),
        (
            "undo_unicode_paste_roundtrip",
            b"\x1b[200~" + ("λ🐈" * 128).encode() + b"\x1b[201~\x1a\x19\r",
            "λ🐈" * 128,
        ),
    ]
    for label, key_bytes, expected in compressed_undo_cases:
        print(f"Checking {label}", flush=True)
        assert_case(binary, label, "insert_backspace", key_bytes, expected)

    assert_case(
        binary,
        "history_cache_observes_external_writer",
        "history_cache_external_update",
        UP + F3 + DOWN + UP + b"\r",
        "external update",
    )

    timed_history_cases = [
        (
            "history_cache_repeated_navigation",
            "history_prev",
            [b"one\r", b"two\r", b"draft" + (b"\x10\x0e" * 12) + b"\r"],
            "draft",
        ),
        (
            "history_cache_updates_pending_draft",
            "history_prev",
            [b"one\r", b"two\r", b"draft\x10\x0eX\x10\x0e\r"],
            "draftX",
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
    print(f"All {pty_tests.PTY_CASE_COUNT} PTY optimization tests passed")


if __name__ == "__main__":
    main(sys.argv[1])
