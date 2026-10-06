/*
  editline_viewport.h

  This file is part of isocline

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
*/

#ifndef IC_EDITLINE_VIEWPORT_H
#define IC_EDITLINE_VIEWPORT_H

#include <stddef.h>
#include <sys/types.h>

#if defined(_MSC_VER)
#include "common.h"  // Provides ssize_t on MSVC.
#endif

typedef struct editline_viewport_s {
    ssize_t input_first_row;
    ssize_t input_row_count;
    ssize_t extra_row_count;
} editline_viewport_t;

static inline editline_viewport_t editline_viewport_for(ssize_t input_rows, ssize_t extra_rows,
                                                        ssize_t cursor_row, ssize_t available_rows,
                                                        size_t max_input_rows,
                                                        size_t bottom_line_count,
                                                        ssize_t previous_input_first_row) {
    if (input_rows < 1) {
        input_rows = 1;
    }
    if (extra_rows < 0) {
        extra_rows = 0;
    }
    if (cursor_row < 0) {
        cursor_row = 0;
    } else if (cursor_row >= input_rows) {
        cursor_row = input_rows - 1;
    }
    if (available_rows < 1) {
        available_rows = 1;
    }

    ssize_t input_row_count = input_rows;
    const ssize_t input_limit = (max_input_rows < 1 ? 1 : (ssize_t)max_input_rows);
    if (input_row_count > input_limit) {
        input_row_count = input_limit;
    }
    if (input_row_count > available_rows) {
        input_row_count = available_rows;
    }

    ssize_t extra_row_count = extra_rows;
    if (input_row_count + extra_row_count > available_rows) {
        // Extra rows are already windowed by the menu builders. Preserve as many of them as
        // possible while always leaving one editable input row visible.
        const ssize_t max_extra_rows = available_rows - 1;
        if (extra_row_count > max_extra_rows) {
            extra_row_count = max_extra_rows;
        }
        input_row_count = available_rows - extra_row_count;
    }

    const ssize_t max_margin = (input_row_count - 1) / 2;
    const ssize_t margin =
        (bottom_line_count >= (size_t)max_margin ? max_margin : (ssize_t)bottom_line_count);
    ssize_t input_first_row = previous_input_first_row;
    if (input_first_row < 0) {
        input_first_row = 0;
    }
    const ssize_t max_first_row = input_rows - input_row_count;
    if (input_first_row > max_first_row) {
        input_first_row = max_first_row;
    }

    // Preserve the previous viewport while the cursor remains inside the configured margins.
    // At the content boundaries, require only the real rows that actually exist.
    const ssize_t rows_after_cursor = input_rows - cursor_row - 1;
    const ssize_t bottom_rows = (rows_after_cursor < margin ? rows_after_cursor : margin);
    const ssize_t min_first_row = cursor_row + bottom_rows - input_row_count + 1;
    if (input_first_row < min_first_row) {
        input_first_row = min_first_row;
    }

    const ssize_t rows_before_cursor = cursor_row;
    const ssize_t top_rows = (rows_before_cursor < margin ? rows_before_cursor : margin);
    const ssize_t max_cursor_first_row = cursor_row - top_rows;
    if (input_first_row > max_cursor_first_row) {
        input_first_row = max_cursor_first_row;
    }

    editline_viewport_t viewport = {
        .input_first_row = input_first_row,
        .input_row_count = input_row_count,
        .extra_row_count = extra_row_count,
    };
    return viewport;
}

#endif  // IC_EDITLINE_VIEWPORT_H
