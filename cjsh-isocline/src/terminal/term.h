/*
  term.h

  This file is part of isocline

  MIT License

  Copyright (c) 2026 Caden Finley
  Copyright (c) 2021 Daan Leijen
  Largely modified for CJ's Shell

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

#ifndef CJSH_ISOCLINE_SRC_TERMINAL_TERM_H
#define CJSH_ISOCLINE_SRC_TERMINAL_TERM_H
#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif
#ifndef IC_TERM_H
#define IC_TERM_H

#include "attr.h"
#include "common.h"
#include "tty.h"

struct term_s;
typedef struct term_s term_t;

typedef enum buffer_mode_e
#ifdef __cplusplus
    : std::uint8_t
#endif
{
    UNBUFFERED,
    LINEBUFFERED,
    BUFFERED,
} buffer_mode_e;

#ifdef __cplusplus
typedef buffer_mode_e buffer_mode_t;
#else
typedef uint8_t buffer_mode_t;
#endif

// Primitives
ic_private term_t* term_new(alloc_t* mem, tty_t* tty, bool nocolor, bool silent, int fd_out);
ic_private void term_free(term_t* term);

ic_private bool term_is_interactive(const term_t* term);
ic_private void term_start_raw(term_t* term);
ic_private void term_end_raw(term_t* term, bool force);

ic_private bool term_enable_beep(term_t* term, bool enable);
ic_private bool term_enable_color(term_t* term, bool enable);

ic_private void term_flush(term_t* term);
ic_private buffer_mode_t term_set_buffer_mode(term_t* term, buffer_mode_t mode);

ic_private void term_write_n(term_t* term, const char* s, ssize_t n);
ic_private void term_write(term_t* term, const char* s);
ic_private void term_write_untracked(term_t* term, const char* s);
ic_private void term_writeln(term_t* term, const char* s);
ic_private void term_write_char(term_t* term, char c);

ic_private void term_write_repeat(term_t* term, const char* s, ssize_t count);
ic_private void term_beep(term_t* term);

ic_private bool term_update_dim(term_t* term);

ic_private ssize_t term_get_width(term_t* term);
ic_private ssize_t term_get_height(term_t* term);
ic_private int term_get_color_bits(term_t* term);
ic_private bool term_query_cursor_pos(term_t* term, ssize_t* row, ssize_t* col);
ic_private bool term_is_cursor_at_line_start(term_t* term);
ic_private bool term_line_has_visible_content(term_t* term);
ic_private void term_set_track_output(term_t* term, bool enable);
ic_private void term_reset_line_state(term_t* term);
ic_private void term_mark_line_visible(term_t* term, bool visible);

// Helpers
ic_private void term_writef(term_t* term, const char* fmt, ...);
ic_private void term_vwritef(term_t* term, const char* fmt, va_list args);

ic_private void term_right(term_t* term, ssize_t n);
ic_private void term_up(term_t* term, ssize_t n);
ic_private void term_down(term_t* term, ssize_t n);
ic_private void term_start_of_line(term_t* term);
ic_private void term_clear_line(term_t* term);
ic_private void term_clear_to_end_of_line(term_t* term);
ic_private void term_clear_lines_to_end(term_t* term);

ic_private void term_attr_reset(term_t* term);
ic_private void term_underline(term_t* term, bool on);
ic_private void term_reverse(term_t* term, bool on);
ic_private void term_bold(term_t* term, bool on);
ic_private void term_italic(term_t* term, bool on);

ic_private void term_color(term_t* term, ic_color_t color);
ic_private void term_bgcolor(term_t* term, ic_color_t color);
ic_private void term_underline_color(term_t* term, ic_color_t color);

// Formatted output

ic_private attr_t term_get_attr(const term_t* term);
ic_private void term_set_attr(term_t* term, attr_t attr);
ic_private void term_write_formatted(term_t* term, const char* s, const attr_t* attrs);
ic_private void term_write_formatted_n(term_t* term, const char* s, const attr_t* attrs, ssize_t n);

ic_private ic_color_t color_from_ansi256(ssize_t i);

#endif  // IC_TERM_H

#endif  // CJSH_ISOCLINE_SRC_TERMINAL_TERM_H
