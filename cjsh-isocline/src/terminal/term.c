/*
  term.c

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

#include "term.h"
#include <sys/types.h>

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attr.h"
#include "common.h"
#include "stringbuf.h"
#include "tty.h"

#if defined(_WIN32)
#include <windows.h>
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#else
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/kd.h>
#endif
#endif

#define IC_CSI "\x1B["

// color support; colors are auto mapped smaller palettes if needed. (see
// `term_color.c`)
typedef enum palette_e {
    MONOCHROME,  // no color
    ANSI8,       // only basic 8 ANSI color     (ESC[<idx>m, idx: 30-37, +10 for
                 // background)
    ANSI16,      // basic + bright ANSI colors  (ESC[<idx>m, idx: 30-37, 90-97, +10
                 // for background)
    ANSI256,     // ANSI 256 color palette      (ESC[38;5;<idx>m, idx: 0-15
                 // standard color, 16-231 6x6x6 rbg colors, 232-255 gray shades)
    ANSIRGB      // direct rgb colors supported (ESC[38;2;<r>;<g>;<b>m)
} palette_t;

// The terminal screen
struct term_s {
    int fd_out;                 // output handle
    ssize_t width;              // screen column width
    ssize_t height;             // screen row height
    ssize_t raw_enabled;        // is raw mode active? counted by start/end pairs
    bool nocolor;               // show colors?
    bool silent;                // enable beep?
    bool is_utf8;               // utf-8 output? determined by the tty
    attr_t attr;                // current text attributes
    palette_t palette;          // color support
    buffer_mode_t bufmode;      // buffer mode
    stringbuf_t* buf;           // buffer for buffered output
    bool track_output;          // track visible output for prompt newline detection
    bool line_state_tracked;    // have we tracked output since last reset?
    bool line_has_visible;      // visible content since last newline
    bool cursor_state_tracked;  // have we tracked cursor-at-line-start state?
    bool cursor_at_line_start;  // tracked cursor position relative to current line start
    tty_t* tty;                 // used on posix to get the cursor position
    alloc_t* mem;               // allocator
#ifdef _WIN32
    HANDLE hcon;             // output console handler
    WORD hcon_default_attr;  // default text attributes
    WORD hcon_orig_attr;     // original text attributes
    DWORD hcon_orig_mode;    // original console mode
    DWORD hcon_mode;         // used console mode
    UINT hcon_orig_cp;       // original console code-page (locale)
    COORD hcon_save_cursor;  // saved cursor position (for escape sequence
                             // emulation)
#endif
};

static bool term_write_direct(term_t* term, const char* s, ssize_t n);
static void term_append_buf(term_t* term, const char* s, ssize_t n);
static bool term_get_cursor_pos(term_t* term, ssize_t* row, ssize_t* col);

//-------------------------------------------------------------
// Colors
//-------------------------------------------------------------

#include "term_color.c"

//-------------------------------------------------------------
// Helpers
//-------------------------------------------------------------

ic_private void term_right(term_t* term, ssize_t n) {
    if (n <= 0) {
        return;
    }
    term_writef(term, IC_CSI "%zdC", n);
}

ic_private void term_up(term_t* term, ssize_t n) {
    if (n <= 0) {
        return;
    }
    term_writef(term, IC_CSI "%zdA", n);
}

ic_private void term_down(term_t* term, ssize_t n) {
    if (n <= 0) {
        return;
    }
    term_writef(term, IC_CSI "%zdB", n);
}

ic_private void term_clear_line(term_t* term) {
    term_write(term, "\r" IC_CSI "K");
}

ic_private void term_clear_to_end_of_line(term_t* term) {
    term_write(term, IC_CSI "K");
}

ic_private void term_start_of_line(term_t* term) {
    term_write(term, "\r");
}

ic_private ssize_t term_get_width(term_t* term) {
    return term->width;
}

ic_private ssize_t term_get_height(term_t* term) {
    return term->height;
}

ic_private bool term_is_cursor_at_line_start(term_t* term) {
    if (term == NULL) {
        return true;
    }
    if (term->cursor_state_tracked) {
        return term->cursor_at_line_start;
    }
    if (!term_is_interactive(term)) {
        return true;
    }
    if (tty_input_pending(term->tty)) {
        return true;
    }
    ssize_t row = 0;
    ssize_t col = 0;
    if (!term_get_cursor_pos(term, &row, &col)) {
        return false;
    }
    return (col <= 1);
}

ic_private bool term_line_has_visible_content(term_t* term) {
    if (term == NULL) {
        return false;
    }
    if (term->line_state_tracked) {
        return term->line_has_visible;
    }
    if (!term_is_interactive(term)) {
        return false;
    }
    if (tty_input_pending(term->tty)) {
        return false;
    }
    ssize_t row = 0;
    ssize_t col = 0;
    if (!term_get_cursor_pos(term, &row, &col)) {
        return false;
    }
    return (col > 1);
}

ic_private bool term_query_cursor_pos(term_t* term, ssize_t* row, ssize_t* col) {
    if (term == NULL || row == NULL || col == NULL) {
        return false;
    }
    if (!term_is_interactive(term) || term->tty == NULL) {
        return false;
    }
    if (tty_input_pending(term->tty)) {
        return false;
    }
    return term_get_cursor_pos(term, row, col);
}

ic_private void term_set_track_output(term_t* term, bool enable) {
    if (term == NULL) {
        return;
    }
    term->track_output = enable;
}

ic_private void term_reset_line_state(term_t* term) {
    if (term == NULL) {
        return;
    }
    term->line_state_tracked = false;
    term->line_has_visible = false;
    term->cursor_state_tracked = false;
    term->cursor_at_line_start = true;
}

ic_private void term_mark_line_visible(term_t* term, bool visible) {
    if (term == NULL) {
        return;
    }
    term->line_state_tracked = true;
    term->line_has_visible = visible;
    term->cursor_state_tracked = true;
    term->cursor_at_line_start = !visible;
}

ic_private void term_attr_reset(term_t* term) {
    term_write(term, IC_CSI "m");
}

ic_private void term_underline(term_t* term, bool on) {
    term_write(term, on ? IC_CSI "4m" : IC_CSI "24m");
}

ic_private void term_reverse(term_t* term, bool on) {
    term_write(term, on ? IC_CSI "7m" : IC_CSI "27m");
}

ic_private void term_bold(term_t* term, bool on) {
    term_write(term, on ? IC_CSI "1m" : IC_CSI "22m");
}

ic_private void term_italic(term_t* term, bool on) {
    term_write(term, on ? IC_CSI "3m" : IC_CSI "23m");
}

ic_private void term_writeln(term_t* term, const char* s) {
    term_write(term, s);
    term_write(term, "\n");
}

ic_private void term_write_char(term_t* term, char c) {
    char buf[2];
    buf[0] = c;
    buf[1] = 0;
    term_write_n(term, buf, 1);
}

ic_private attr_t term_get_attr(const term_t* term) {
    return term->attr;
}

ic_private void term_set_attr(term_t* term, attr_t attr) {
    if (term->nocolor) {
        return;
    }
    if (attr.x.color != term->attr.x.color && attr.x.color != IC_COLOR_NONE) {
        term_color(term, attr.x.color);
        term->attr.x.color = attr.x.color;
    }
    if (attr.x.bgcolor != term->attr.x.bgcolor && attr.x.bgcolor != IC_COLOR_NONE) {
        term_bgcolor(term, attr.x.bgcolor);
        term->attr.x.bgcolor = attr.x.bgcolor;
    }
    if (attr.x.bold != term->attr.x.bold && attr.x.bold != IC_NONE) {
        term_bold(term, attr.x.bold == IC_ON);
        term->attr.x.bold = attr.x.bold;
    }
    if (attr.x.underline != term->attr.x.underline && attr.x.underline != IC_NONE) {
        term_underline(term, attr.x.underline == IC_ON);
        term->attr.x.underline = attr.x.underline;
    }
    if (attr.x.underline_color != term->attr.x.underline_color &&
        attr.x.underline_color != IC_COLOR_NONE) {
        term_underline_color(term, attr.x.underline_color);
        term->attr.x.underline_color = attr.x.underline_color;
    }
    if (attr.x.reverse != term->attr.x.reverse && attr.x.reverse != IC_NONE) {
        term_reverse(term, attr.x.reverse == IC_ON);
        term->attr.x.reverse = attr.x.reverse;
    }
    if (attr.x.italic != term->attr.x.italic && attr.x.italic != IC_NONE) {
        term_italic(term, attr.x.italic == IC_ON);
        term->attr.x.italic = attr.x.italic;
    }
    assert(attr.x.color == term->attr.x.color || attr.x.color == IC_COLOR_NONE);
    assert(attr.x.bgcolor == term->attr.x.bgcolor || attr.x.bgcolor == IC_COLOR_NONE);
    assert(attr.x.bold == term->attr.x.bold || attr.x.bold == IC_NONE);
    assert(attr.x.reverse == term->attr.x.reverse || attr.x.reverse == IC_NONE);
    assert(attr.x.underline == term->attr.x.underline || attr.x.underline == IC_NONE);
    assert(attr.x.italic == term->attr.x.italic || attr.x.italic == IC_NONE);
    assert(attr.x.underline_color == term->attr.x.underline_color ||
           attr.x.underline_color == IC_COLOR_NONE);
}

ic_private void term_clear_lines_to_end(term_t* term) {
    term_write(term, "\r" IC_CSI "J");
}

/*
ic_private void term_show_cursor(term_t* term, bool on) {
  term_write(term, on ? IC_CSI "?25h" : IC_CSI "?25l");
}
*/

//-------------------------------------------------------------
// Formatted output
//-------------------------------------------------------------

ic_private void term_writef(term_t* term, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    term_vwritef(term, fmt, ap);
    va_end(ap);
}

ic_private void term_vwritef(term_t* term, const char* fmt, va_list args) {
    (void)sbuf_append_vprintf(term->buf, fmt, args);
}

ic_private void term_write_formatted(term_t* term, const char* s, const attr_t* attrs) {
    term_write_formatted_n(term, s, attrs, ic_strlen(s));
}

ic_private void term_write_formatted_n(term_t* term, const char* s, const attr_t* attrs,
                                       ssize_t len) {
    if (attrs == NULL) {
        // write directly
        term_write(term, s);
    } else {
        // ensure raw mode from now on
        if (term->raw_enabled <= 0) {
            term_start_raw(term);
        }
        // and output with text attributes
        const attr_t default_attr = term_get_attr(term);
        attr_t attr = attr_none();
        ssize_t i = 0;
        ssize_t n = 0;
        while (i + n < len && s[i + n] != 0) {
            if (!attr_is_eq(attr, attrs[i + n])) {
                if (n > 0) {
                    term_write_n(term, s + i, n);
                    i += n;
                    n = 0;
                }
                attr = attrs[i];
                term_set_attr(term, attr_update_with(default_attr, attr));
            }
            n++;
        }
        if (n > 0) {
            term_write_n(term, s + i, n);
        }
#ifndef NDEBUG
        const ssize_t consumed = i + n;
        assert(s[consumed] != 0 || consumed == len);
#endif
        term_set_attr(term, default_attr);
    }
}

//-------------------------------------------------------------
// Write to the terminal
// The buffered functions are used to reduce cursor flicker
// during refresh
//-------------------------------------------------------------

ic_private void term_beep(term_t* term) {
    if (term->silent) {
        return;
    }
    if (fputc('\a', stderr) == EOF) {
        debug_msg("term: failed to emit bell\n");
        clearerr(stderr);
        return;
    }
    if (fflush(stderr) == EOF) {
        debug_msg("term: failed to flush bell\n");
    }
}

ic_private void term_write_repeat(term_t* term, const char* s, ssize_t count) {
    for (; count > 0; count--) {
        term_write(term, s);
    }
}

ic_private void term_write(term_t* term, const char* s) {
    if (s == NULL || s[0] == 0) {
        return;
    }
    ssize_t n = ic_strlen(s);
    term_write_n(term, s, n);
}

// Write terminal metadata without making it authoritative for prompt newline detection.
// Semantic escape sequences do not describe output written directly by an embedding shell or
// child process, so treating them as tracked output can hide real partial-line content.
ic_private void term_write_untracked(term_t* term, const char* s) {
    if (term == NULL) {
        return;
    }
    const bool track_output = term->track_output;
    term->track_output = false;
    term_write(term, s);
    term->track_output = track_output;
}

// Primitive terminal write; all writes go through here
ic_private void term_write_n(term_t* term, const char* s, ssize_t n) {
    if (s == NULL || n <= 0) {
        return;
    }
    // write to buffer to reduce flicker and to process escape sequences (this
    // may flush too)
    term_append_buf(term, s, n);
}

//-------------------------------------------------------------
// Buffering
//-------------------------------------------------------------

ic_private void term_flush(term_t* term) {
    if (sbuf_len(term->buf) > 0) {
        // term_show_cursor(term,false);
        (void)term_write_direct(term, sbuf_string(term->buf), sbuf_len(term->buf));
        // term_show_cursor(term,true);
        sbuf_clear(term->buf);
    }
}

ic_private buffer_mode_t term_set_buffer_mode(term_t* term, buffer_mode_t mode) {
    buffer_mode_t oldmode = term->bufmode;
    if (oldmode != mode) {
        if (mode == UNBUFFERED) {
            term_flush(term);
        }
        term->bufmode = mode;
    }
    return oldmode;
}

static void term_check_flush(term_t* term, bool contains_nl) {
    if (term->bufmode == UNBUFFERED || sbuf_len(term->buf) > 4000 ||
        (term->bufmode == LINEBUFFERED && contains_nl)) {
        term_flush(term);
    }
}

//-------------------------------------------------------------
// Init
//-------------------------------------------------------------

static void term_init_raw(term_t* term);

ic_private term_t* term_new(alloc_t* mem, tty_t* tty, bool nocolor, bool silent, int fd_out) {
    term_t* term = mem_zalloc_tp(mem, term_t);
    if (term == NULL) {
        return NULL;
    }

    term->fd_out = (fd_out < 0 ? STDOUT_FILENO : fd_out);
    if (fd_out < 0 && tty != NULL && isatty(term->fd_out) == 0 && isatty(STDERR_FILENO) != 0) {
        term->fd_out = STDERR_FILENO;
    }
    term->nocolor = nocolor || (isatty(term->fd_out) == 0);
    term->silent = silent;
    term->mem = mem;
    term->tty = tty;  // can be NULL
    term->width = 80;
    term->height = 25;
    term->is_utf8 = tty_is_utf8(tty);
    term->palette = ANSI16;  // almost universally supported
    term->buf = sbuf_new(mem);
    term->bufmode = LINEBUFFERED;
    term->attr = attr_default();
    term->track_output = true;
    term->line_state_tracked = false;
    term->line_has_visible = false;
    term->cursor_state_tracked = false;
    term->cursor_at_line_start = true;

    // respect NO_COLOR
    if (getenv("NO_COLOR") != NULL) {
        term->nocolor = true;
    }
    if (!term->nocolor) {
        // detect color palette
        // COLORTERM takes precedence
        const char* colorterm = getenv("COLORTERM");
        const char* eterm = getenv("TERM");
        if (ic_contains(colorterm, "24bit") || ic_contains(colorterm, "truecolor") ||
            ic_contains(colorterm, "direct")) {
            term->palette = ANSIRGB;
        } else if (ic_contains(colorterm, "8bit") || ic_contains(colorterm, "256color")) {
            term->palette = ANSI256;
        } else if (ic_contains(colorterm, "4bit") || ic_contains(colorterm, "16color")) {
            term->palette = ANSI16;
        } else if (ic_contains(colorterm, "3bit") || ic_contains(colorterm, "8color")) {
            term->palette = ANSI8;
        } else if (ic_contains(colorterm, "1bit") || ic_contains(colorterm, "nocolor") ||
                   ic_contains(colorterm, "monochrome")) {
            term->palette = MONOCHROME;
        }
        // otherwise check for some specific terminals
        else if (getenv("WT_SESSION") != NULL) {
            term->palette = ANSIRGB;
        }  // Windows terminal
        else if (getenv("ITERM_SESSION_ID") != NULL) {
            term->palette = ANSIRGB;
        }  // iTerm2 terminal
        else if (getenv("VSCODE_PID") != NULL) {
            term->palette = ANSIRGB;
        }  // vscode terminal
        else {
            // and otherwise fall back to checking TERM
            if (ic_contains(eterm, "truecolor") || ic_contains(eterm, "direct") ||
                ic_contains(colorterm, "24bit")) {
                term->palette = ANSIRGB;
            } else if (ic_contains(eterm, "alacritty") || ic_contains(eterm, "kitty")) {
                term->palette = ANSIRGB;
            } else if (ic_contains(eterm, "256color") || ic_contains(eterm, "gnome")) {
                term->palette = ANSI256;
            } else if (ic_contains(eterm, "16color")) {
                term->palette = ANSI16;
            } else if (ic_contains(eterm, "8color")) {
                term->palette = ANSI8;
            } else if (ic_contains(eterm, "monochrome") || ic_contains(eterm, "nocolor") ||
                       ic_contains(eterm, "dumb")) {
                term->palette = MONOCHROME;
            }
        }
        debug_msg("term: color-bits: %d (COLORTERM=%s, TERM=%s)\n", term_get_color_bits(term),
                  colorterm, eterm);
    }

    // read COLUMS/LINES from the environment for a better initial guess.
    const char* env_columns = getenv("COLUMNS");
    if (env_columns != NULL) {
        (void)ic_atoz(env_columns, &term->width);
    }
    const char* env_lines = getenv("LINES");
    if (env_lines != NULL) {
        (void)ic_atoz(env_lines, &term->height);
    }

    // initialize raw terminal output and terminal dimensions
    term_init_raw(term);
    (void)term_update_dim(term);
    term_attr_reset(term);  // ensure we are at default settings

    return term;
}

ic_private bool term_is_interactive(const term_t* term) {
    if (term == NULL || isatty(term->fd_out) == 0) {
        return false;
    }
    // check dimensions (0 is used for debuggers)
    // if (term->width <= 0) return false;

    // check editing support
    const char* eterm = getenv("TERM");
    debug_msg("term: TERM=%s\n", eterm);
    if (eterm != NULL && (strstr("dumb|DUMB|cons25|CONS25|emacs|EMACS", eterm) != NULL)) {
        return false;
    }

    return true;
}

ic_private bool term_enable_beep(term_t* term, bool enable) {
    bool prev = term->silent;
    term->silent = !enable;
    return prev;
}

ic_private bool term_enable_color(term_t* term, bool enable) {
    bool prev = !term->nocolor;
    term->nocolor = !enable;
    return prev;
}

ic_private void term_free(term_t* term) {
    if (term == NULL) {
        return;
    }
    alloc_t* mem = term->mem;
    term_flush(term);
    term_end_raw(term, true);
    sbuf_free(term->buf);
    term->buf = NULL;
    mem_free(mem, term);
}

//-------------------------------------------------------------
// For best portability and applications inserting CSI SGR (ESC[ .. m)
// codes themselves in strings, we interpret these at the
// lowest level so we can have a `term_get_attr` function which
// is needed for bracketed styles etc.
//-------------------------------------------------------------

static void term_append_esc(term_t* term, const char* const s, ssize_t len) {
    if (s[1] == '[' && s[len - 1] == 'm') {
        // it is a CSI SGR sequence: ESC[ ... m
        if (term->nocolor) {
            return;  // ignore escape sequences if nocolor is set
        }
        term->attr = attr_update_with(term->attr, attr_from_esc_sgr(s, len));
    }
    // and write out the escape sequence as-is
    (void)sbuf_append_n(term->buf, s, len);
}

static void term_append_buf(term_t* term, const char* s, ssize_t len) {
    ssize_t pos = 0;
    bool newline = false;
    const bool track_output = (term != NULL && term->track_output);
    if (track_output && len > 0) {
        term->line_state_tracked = true;
        term->cursor_state_tracked = true;
    }
    while (pos < len) {
        // handle ascii sequences in bulk
        ssize_t ascii = 0;
        ssize_t next;
        while ((next = str_next_ofs(s, len, pos + ascii, NULL)) > 0 &&
               (uint8_t)s[pos + ascii] > '\x1B' && (uint8_t)s[pos + ascii] <= 0x7F) {
            ascii += next;
        }
        if (ascii > 0) {
            (void)sbuf_append_n(term->buf, s + pos, ascii);
            if (track_output) {
                term->line_has_visible = true;
                term->cursor_at_line_start = false;
            }
            pos += ascii;
        }
        if (next <= 0) {
            break;
        }

        const uint8_t c = (uint8_t)s[pos];
        // handle utf-8 sequences: append raw bytes for correct display
        if (c >= 0x80) {
            (void)sbuf_append_n(term->buf, s + pos, next);
            if (track_output) {
                term->line_has_visible = true;
                term->cursor_at_line_start = false;
            }
            pos += next;
            continue;
        }

        // handle escape sequence (note: str_next_ofs considers whole CSI escape
        // sequences at a time)
        else if (next > 1 && c == '\x1B') {
            term_append_esc(term, s + pos, next);
        } else if (c < ' ' && c != 0 && (c < '\x07' || c > '\x0D')) {
            // ignore control characters except \a, \b, \t, \n, \r, and
            // form-feed and vertical tab.
        } else {
            if (c == '\n') {
                newline = true;
                if (track_output) {
                    term->line_has_visible = false;
                    term->cursor_at_line_start = true;
                }
            } else if (c == '\r') {
                if (track_output) {
                    term->cursor_at_line_start = true;
                }
            } else if ((c == '\t') && track_output) {
                term->line_has_visible = true;
                term->cursor_at_line_start = false;
            }

            (void)sbuf_append_n(term->buf, s + pos, next);
        }
        pos += next;
    }
    // possibly flush
    term_check_flush(term, newline);
}

//-------------------------------------------------------------
// Platform dependent: Write directly to the terminal
//-------------------------------------------------------------

#if !defined(_WIN32)

// write to the console without further processing
static bool term_write_direct(term_t* term, const char* s, ssize_t n) {
    ssize_t count = 0;
    while (count < n) {
        ssize_t nwritten = write(term->fd_out, s + count, to_size_t(n - count));
        if (nwritten > 0) {
            count += nwritten;
        } else if (errno != EINTR && errno != EAGAIN) {
            debug_msg("term: write failed: length %i, errno %i: \"%s\"\n", n, errno, s);
            return false;
        }
    }
    return true;
}

#else

//----------------------------------------------------------------------------------
// On windows we use the new virtual terminal processing if it is available
// (Windows Terminal) but fall back to  ansi escape emulation on older systems
// but also for example the PS terminal
//
// note: we use row/col as 1-based ANSI escape while windows X/Y coords are
// 0-based.
//-----------------------------------------------------------------------------------

#if !defined(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING (0)
#endif
#if !defined(ENABLE_LVB_GRID_WORLDWIDE)
#define ENABLE_LVB_GRID_WORLDWIDE (0)
#endif

// direct write to the console without further processing
static bool term_write_console(term_t* term, const char* s, ssize_t n) {
    DWORD written;
    // WriteConsoleA(term->hcon, s, (DWORD)(to_size_t(n)), &written, NULL);
    WriteFile(term->hcon, s, (DWORD)(to_size_t(n)), &written,
              NULL);  // so it can be redirected
    return (written == (DWORD)(to_size_t(n)));
}

static bool term_get_cursor_pos(term_t* term, ssize_t* row, ssize_t* col) {
    *row = 0;
    *col = 0;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(term->hcon, &info)) {
        return false;
    }
    *row = (ssize_t)info.dwCursorPosition.Y + 1;
    *col = (ssize_t)info.dwCursorPosition.X + 1;
    return true;
}

static void term_move_cursor_to(term_t* term, ssize_t row, ssize_t col) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(term->hcon, &info)) {
        return;
    }
    if (col > info.dwSize.X) {
        col = info.dwSize.X;
    }
    if (row > info.dwSize.Y) {
        row = info.dwSize.Y;
    }
    if (col <= 0) {
        col = 1;
    }
    if (row <= 0) {
        row = 1;
    }
    COORD coord;
    coord.X = (SHORT)col - 1;
    coord.Y = (SHORT)row - 1;
    SetConsoleCursorPosition(term->hcon, coord);
}

static void term_cursor_save(term_t* term) {
    memset(&term->hcon_save_cursor, 0, sizeof(term->hcon_save_cursor));
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(term->hcon, &info)) {
        return;
    }
    term->hcon_save_cursor = info.dwCursorPosition;
}

static void term_cursor_restore(term_t* term) {
    if (term->hcon_save_cursor.X == 0) {
        return;
    }
    SetConsoleCursorPosition(term->hcon, term->hcon_save_cursor);
}

static void term_move_cursor(term_t* term, ssize_t drow, ssize_t dcol, ssize_t n) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(term->hcon, &info)) {
        return;
    }
    COORD cur = info.dwCursorPosition;
    ssize_t col = (ssize_t)cur.X + 1 + n * dcol;
    ssize_t row = (ssize_t)cur.Y + 1 + n * drow;
    term_move_cursor_to(term, row, col);
}

static void term_cursor_visible(term_t* term, bool visible) {
    CONSOLE_CURSOR_INFO info;
    if (!GetConsoleCursorInfo(term->hcon, &info)) {
        return;
    }
    info.bVisible = visible;
    SetConsoleCursorInfo(term->hcon, &info);
}

static void term_erase_line(term_t* term, ssize_t mode) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(term->hcon, &info)) {
        return;
    }
    DWORD written;
    COORD start;
    ssize_t length;
    if (mode == 2) {
        // entire line
        start.X = 0;
        start.Y = info.dwCursorPosition.Y;
        length = (ssize_t)info.srWindow.Right + 1;
    } else if (mode == 1) {
        // to start of line
        start.X = 0;
        start.Y = info.dwCursorPosition.Y;
        length = info.dwCursorPosition.X;
    } else {
        // to end of line
        length = (ssize_t)info.srWindow.Right - info.dwCursorPosition.X + 1;
        start = info.dwCursorPosition;
    }
    FillConsoleOutputAttribute(term->hcon, term->hcon_default_attr, (DWORD)length, start, &written);
    FillConsoleOutputCharacterA(term->hcon, ' ', (DWORD)length, start, &written);
}

static void term_clear_screen(term_t* term, ssize_t mode) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(term->hcon, &info)) {
        return;
    }
    COORD start;
    start.X = 0;
    start.Y = 0;
    ssize_t length;
    ssize_t width = (ssize_t)info.dwSize.X;
    if (mode == 2) {
        // entire screen
        length = width * info.dwSize.Y;
    } else if (mode == 1) {
        // to cursor
        length = (width * ((ssize_t)info.dwCursorPosition.Y - 1)) + info.dwCursorPosition.X;
    } else {
        // from cursor
        start = info.dwCursorPosition;
        length = (width * ((ssize_t)info.dwSize.Y - info.dwCursorPosition.Y)) +
                 (width - info.dwCursorPosition.X + 1);
    }
    DWORD written;
    FillConsoleOutputAttribute(term->hcon, term->hcon_default_attr, (DWORD)length, start, &written);
    FillConsoleOutputCharacterA(term->hcon, ' ', (DWORD)length, start, &written);
}

static WORD attr_color[8] = {
    0,                                                    // black
    FOREGROUND_RED,                                       // maroon
    FOREGROUND_GREEN,                                     // green
    FOREGROUND_RED | FOREGROUND_GREEN,                    // orange
    FOREGROUND_BLUE,                                      // navy
    FOREGROUND_RED | FOREGROUND_BLUE,                     // purple
    FOREGROUND_GREEN | FOREGROUND_BLUE,                   // teal
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,  // light gray
};

static void term_set_win_attr(term_t* term, attr_t ta) {
    WORD def_attr = term->hcon_default_attr;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(term->hcon, &info)) {
        return;
    }
    WORD cur_attr = info.wAttributes;
    WORD attr = cur_attr;
    if (ta.x.color != IC_COLOR_NONE) {
        if (ta.x.color >= IC_ANSI_BLACK && ta.x.color <= IC_ANSI_SILVER) {
            attr = (attr & 0xFFF0) | attr_color[ta.x.color - IC_ANSI_BLACK];
        } else if (ta.x.color >= IC_ANSI_GRAY && ta.x.color <= IC_ANSI_WHITE) {
            attr = (attr & 0xFFF0) | attr_color[ta.x.color - IC_ANSI_GRAY] | FOREGROUND_INTENSITY;
        } else if (ta.x.color == IC_ANSI_DEFAULT) {
            attr = (attr & 0xFFF0) | (def_attr & 0x000F);
        }
    }
    if (ta.x.bgcolor != IC_COLOR_NONE) {
        if (ta.x.bgcolor >= IC_ANSI_BLACK && ta.x.bgcolor <= IC_ANSI_SILVER) {
            attr = (attr & 0xFF0F) | (WORD)(attr_color[ta.x.bgcolor - IC_ANSI_BLACK] << 4);
        } else if (ta.x.bgcolor >= IC_ANSI_GRAY && ta.x.bgcolor <= IC_ANSI_WHITE) {
            attr = (attr & 0xFF0F) | (WORD)(attr_color[ta.x.bgcolor - IC_ANSI_GRAY] << 4) |
                   BACKGROUND_INTENSITY;
        } else if (ta.x.bgcolor == IC_ANSI_DEFAULT) {
            attr = (attr & 0xFF0F) | (def_attr & 0x00F0);
        }
    }
    if (ta.x.underline != IC_NONE) {
        attr =
            (attr & ~COMMON_LVB_UNDERSCORE) | (ta.x.underline == IC_ON ? COMMON_LVB_UNDERSCORE : 0);
    }
    if (ta.x.reverse != IC_NONE) {
        attr = (attr & ~COMMON_LVB_REVERSE_VIDEO) |
               (ta.x.reverse == IC_ON ? COMMON_LVB_REVERSE_VIDEO : 0);
    }
    if (attr != cur_attr) {
        SetConsoleTextAttribute(term->hcon, attr);
    }
}

static ssize_t esc_param(const char* s, ssize_t def) {
    if (*s == '?') {
        s++;
    }
    ssize_t n = def;
    ic_atoz(s, &n);
    return n;
}

static void esc_param2(const char* s, ssize_t* p1, ssize_t* p2, ssize_t def) {
    if (*s == '?') {
        s++;
    }
    *p1 = def;
    *p2 = def;
    ic_atoz2(s, p1, p2);
}

// Emulate escape sequences on older windows.
static void term_write_esc(term_t* term, const char* s, ssize_t len) {
    ssize_t row;
    ssize_t col;

    if (s[1] == '[') {
        switch (s[len - 1]) {
            case 'A':
                term_move_cursor(term, -1, 0, esc_param(s + 2, 1));
                break;
            case 'B':
                term_move_cursor(term, 1, 0, esc_param(s + 2, 1));
                break;
            case 'C':
                term_move_cursor(term, 0, 1, esc_param(s + 2, 1));
                break;
            case 'D':
                term_move_cursor(term, 0, -1, esc_param(s + 2, 1));
                break;
            case 'H':
                esc_param2(s + 2, &row, &col, 1);
                term_move_cursor_to(term, row, col);
                break;
            case 'K':
                term_erase_line(term, esc_param(s + 2, 0));
                break;
            case 'm':
                term_set_win_attr(term, attr_from_esc_sgr(s, len));
                break;

            // support some less standard escape codes (currently not used by
            // isocline)
            case 'E':  // line down
                term_get_cursor_pos(term, &row, &col);
                row += esc_param(s + 2, 1);
                term_move_cursor_to(term, row, 1);
                break;
            case 'F':  // line up
                term_get_cursor_pos(term, &row, &col);
                row -= esc_param(s + 2, 1);
                term_move_cursor_to(term, row, 1);
                break;
            case 'G':  // absolute column
                term_get_cursor_pos(term, &row, &col);
                col = esc_param(s + 2, 1);
                term_move_cursor_to(term, row, col);
                break;
            case 'J':
                term_clear_screen(term, esc_param(s + 2, 0));
                break;
            case 'h':
                if (strncmp(s + 2, "?25h", 4) == 0) {
                    term_cursor_visible(term, true);
                }
                break;
            case 'l':
                if (strncmp(s + 2, "?25l", 4) == 0) {
                    term_cursor_visible(term, false);
                }
                break;
            case 's':
                term_cursor_save(term);
                break;
            case 'u':
                term_cursor_restore(term);
                break;
                // otherwise ignore
        }
    } else if (s[1] == '7') {
        term_cursor_save(term);
    } else if (s[1] == '8') {
        term_cursor_restore(term);
    } else {
        // Ignore unsupported escape sequences.
    }
}

static bool term_write_direct(term_t* term, const char* s, ssize_t len) {
    term_cursor_visible(term, false);  // reduce flicker
    ssize_t pos = 0;
    if ((term->hcon_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        // use the builtin virtual terminal processing. (enables truecolor for
        // example)
        term_write_console(term, s, len);
        pos = len;
    } else {
        // emulate escape sequences
        while (pos < len) {
            // handle non-control in bulk (including utf-8 sequences)
            // (We don't need to handle utf-8 separately as we set the codepage
            // to always be in utf-8 mode)
            ssize_t nonctrl = 0;
            ssize_t next;
            while ((next = str_next_ofs(s, len, pos + nonctrl, NULL)) > 0 &&
                   (uint8_t)s[pos + nonctrl] >= ' ' && (uint8_t)s[pos + nonctrl] <= 0x7F) {
                nonctrl += next;
            }
            if (nonctrl > 0) {
                term_write_console(term, s + pos, nonctrl);
                pos += nonctrl;
            }
            if (next <= 0) {
                break;
            }

            if ((uint8_t)s[pos] >= 0x80) {
                // utf8 is already processed
                term_write_console(term, s + pos, next);
            } else if (next > 1 && s[pos] == '\x1B') {
                // handle control (note: str_next_ofs considers whole CSI escape
                // sequences at a time)
                term_write_esc(term, s + pos, next);
            } else if (next == 1 &&
                       (s[pos] == '\r' || s[pos] == '\n' || s[pos] == '\t' || s[pos] == '\b')) {
                term_write_console(term, s + pos, next);
            } else {
                // Ignore unsupported control bytes.
            }
            pos += next;
        }
    }
    term_cursor_visible(term, true);
    assert(pos == len);
    return (pos == len);
}
#endif

//-------------------------------------------------------------
// Update terminal dimensions
//-------------------------------------------------------------

#if !defined(_WIN32)

// send escape query that may return a response on the tty
static bool term_esc_query_raw(term_t* term, const char* query, char* buf, ssize_t buflen,
                               tty_response_fun_t* matches, void* arg, long initial_timeout_ms) {
    if (!term_is_interactive(term) || term->tty == NULL || tty_input_pending(term->tty) ||
        buf == NULL || buflen <= 0 || query[0] == 0) {
        return false;
    }
    bool osc = (query[1] == ']');
    if (!term_write_direct(term, query, ic_strlen(query))) {
        return false;
    }
    debug_msg("term: read tty query response to: ESC %s\n", query + 1);
    return tty_read_esc_response_with_timeout(term->tty, query[1], osc, buf, buflen, matches, arg,
                                              initial_timeout_ms);
}

static bool term_esc_query(term_t* term, const char* query, char* buf, ssize_t buflen,
                           tty_response_fun_t* matches, void* arg) {
    if (!term_is_interactive(term) || term->tty == NULL) {
        return false;
    }
    const bool was_raw = tty_is_raw_enabled(term->tty);
    if (!was_raw && !tty_start_raw(term->tty)) {
        return false;
    }
    bool ok = term_esc_query_raw(term, query, buf, buflen, matches, arg, -1);
    if (!was_raw) {
        tty_end_raw(term->tty);
    }
    return ok;
}

// get the cursor position via an ESC[6n
static bool cursor_response_matches(const char* response, void* arg) {
    ic_unused(arg);
    const size_t rowlen = strspn(response, "0123456789");
    if (rowlen == 0 || response[rowlen] != ';') {
        return false;
    }
    const char* column = response + rowlen + 1;
    const size_t collen = strspn(column, "0123456789");
    if (collen == 0 || column[collen] != 'R' || column[collen + 1] != 0) {
        return false;
    }
    // Validate bounds before committing the input bytes to this reply.
    // Use the same signed-size limit as to_ssize_t; SSIZE_MAX is not ISO C11.
    const ssize_t max_value = (ssize_t)(SIZE_MAX / 2);
    for (const char* p = response; *p != 'R';) {
        ssize_t value = 0;
        while (*p >= '0' && *p <= '9') {
            const int digit = *p++ - '0';
            if (value > (max_value - digit) / 10) {
                return false;
            }
            value = 10 * value + digit;
        }
        if (value == 0) {
            return false;
        }
        if (*p == ';') {
            p++;
        }
    }
    return true;
}

static bool term_get_cursor_pos(term_t* term, ssize_t* row, ssize_t* col) {
    // send escape query
    char buf[128];
    if (!term_esc_query(term, "\x1B[6n", buf, 128, cursor_response_matches, NULL)) {
        return false;
    }
    char* end = strchr(buf, 'R');
    if (end != NULL) {
        *end = '\0';
    }
    if (!ic_atoz2(buf, row, col)) {
        return false;
    }
    return true;
}

static void term_set_cursor_pos(term_t* term, ssize_t row, ssize_t col) {
    term_writef(term, IC_CSI "%zd;%zdH", row, col);
}

ic_private bool term_update_dim(term_t* term) {
    if (!term_is_interactive(term)) {
        return false;
    }
    ssize_t cols = 0;
    ssize_t rows = 0;
    struct winsize ws;
    if (ioctl(term->fd_out, TIOCGWINSZ, &ws) >= 0) {
        // ioctl succeeded
        cols = ws.ws_col;  // debuggers return 0 for the column
        rows = ws.ws_row;
    } else {
        // determine width by querying the cursor position
        debug_msg("term: ioctl term-size failed: %d,%d\n", ws.ws_row, ws.ws_col);
        ssize_t col0 = 0;
        ssize_t row0 = 0;
        if (term_get_cursor_pos(term, &row0, &col0)) {
            term_set_cursor_pos(term, 999, 999);
            ssize_t col1 = 0;
            ssize_t row1 = 0;
            if (term_get_cursor_pos(term, &row1, &col1)) {
                cols = col1;
                rows = row1;
            }
            term_set_cursor_pos(term, row0, col0);
        } else {
            // Cannot query position; leave cols/rows at 0.
        }
    }

    // update width and return whether it changed.
    bool changed = (term->width != cols || term->height != rows);
    debug_msg("terminal dim: %zd,%zd: %s\n", rows, cols, changed ? "changed" : "unchanged");
    if (cols > 0) {
        term->width = cols;
        term->height = rows;
    }
    return changed;
}

#else

ic_private bool term_update_dim(term_t* term) {
    if (term->hcon == 0) {
        term->hcon = GetConsoleWindow();
    }
    ssize_t rows = 0;
    ssize_t cols = 0;
    CONSOLE_SCREEN_BUFFER_INFO sbinfo;
    if (GetConsoleScreenBufferInfo(term->hcon, &sbinfo)) {
        cols = (ssize_t)sbinfo.srWindow.Right - (ssize_t)sbinfo.srWindow.Left + 1;
        rows = (ssize_t)sbinfo.srWindow.Bottom - (ssize_t)sbinfo.srWindow.Top + 1;
    }
    bool changed = (term->width != cols || term->height != rows);
    term->width = cols;
    term->height = rows;
    debug_msg("term: update dim: %zd, %zd\n", term->height, term->width);
    return changed;
}

#endif

//-------------------------------------------------------------
// Enable/disable terminal raw mode
//-------------------------------------------------------------

#if !defined(_WIN32)

// On non-windows, the terminal is set in raw mode by the tty.

ic_private void term_start_raw(term_t* term) {
    term->raw_enabled++;
}

ic_private void term_end_raw(term_t* term, bool force) {
    if (term->raw_enabled <= 0) {
        return;
    }
    if (!force) {
        term->raw_enabled--;
    } else {
        term->raw_enabled = 0;
    }
}

static bool color_response_matches(const char* buf, void* arg) {
    const char* prefix = (const char*)arg;
    const size_t prefix_len = strlen(prefix);
    if (strncmp(buf, prefix, prefix_len) != 0) {
        return false;
    }
    const char* rgb = buf + prefix_len;
    for (int i = 0; i < 3; ++i) {
        const size_t digits = strspn(rgb, "0123456789abcdefABCDEF");
        if (digits == 0 || digits > 4) {
            return false;
        }
        rgb += digits;
        if ((i < 2) && (*rgb++ != '/')) {
            return false;
        }
    }
    return *rgb == 0;
}

static bool term_esc_query_color_raw(term_t* term, ssize_t color_idx, uint32_t* color) {
    char query[64];
    char prefix[64];
    char buf[128];
    (void)snprintf(query, sizeof(query), "\x1B]4;%zd;?\x1B\\", color_idx);
    (void)snprintf(prefix, sizeof(prefix), "4;%zd;rgb:", color_idx);
    // Palette discovery is optional. An unsupported OSC 4 query should not
    // delay the first prompt by the much longer escape-key ambiguity timeout.
    if (!term_esc_query_raw(term, query, buf, sizeof(buf), color_response_matches, prefix, 40)) {
        return false;
    }
    const char* rgb = buf + strlen(prefix);

    unsigned long components[3] = {0};
    const char* cursor = rgb;
    for (int idx = 0; idx < 3; idx++) {
        errno = 0;
        char* endptr = NULL;
        unsigned long value = strtoul(cursor, &endptr, 16);
        if (cursor == endptr || errno == ERANGE || value > 0xFFFFFFUL) {
            return false;
        }
        components[idx] = value;
        if (idx < 2) {
            if (*endptr != '/') {
                return false;
            }
            cursor = endptr + 1;
        } else {
            cursor = endptr;
        }
    }

    unsigned int r = (unsigned int)components[0];
    unsigned int g = (unsigned int)components[1];
    unsigned int b = (unsigned int)components[2];
    if (r > 0xFF || g > 0xFF || b > 0xFF) {  // 48-bit rgb, hexadecimal round to 24-bit
        r = (r + 0x7F) / 0x100;              // note: can "overflow", e.g. 0xFFFF -> 0x100.
        g = (g + 0x7F) / 0x100;
        b = (b + 0x7F) / 0x100;
    }
    *color = (ic_cap8(r) << 16) | (ic_cap8(g) << 8) | ic_cap8(b);
    debug_msg("color query: %02x,%02x,%02x: %06x\n", r, g, b, *color);
    return true;
}

// update ansi 16 color palette for better color approximation
static void term_update_ansi16(term_t* term) {
    if (!term_is_interactive(term) || term->tty == NULL) {
        return;
    }
    debug_msg("update ansi colors\n");
#if defined(GIO_CMAP)
    // try ioctl first (on Linux)
    uint8_t cmap[48];
    memset(cmap, 0, 48);
    if (ioctl(term->fd_out, GIO_CMAP, &cmap) >= 0) {
        // success
        for (ssize_t i = 0; i < 48; i += 3) {
            uint32_t color =
                ((uint32_t)(cmap[i]) << 16) | ((uint32_t)(cmap[i + 1]) << 8) | cmap[i + 2];
            debug_msg("term (ioctl) ansi color %d: 0x%06x\n", i, color);
            ansi256[i] = color;
        }
        return;
    } else {
        debug_msg("ioctl GIO_CMAP failed: entry 1: 0x%02x%02x%02x\n", cmap[3], cmap[4], cmap[5]);
    }
#endif
// this seems to be unreliable on some systems (Ubuntu+Gnome terminal) so only
// enable when known ok.
#if __APPLE__
    // otherwise use OSC 4 escape sequence query
    if (tty_start_raw(term->tty)) {
        for (ssize_t i = 0; i < 16; i++) {
            uint32_t color;
            if (!term_esc_query_color_raw(term, i, &color)) {
                break;
            }
            debug_msg("term ansi color %d: 0x%06x\n", i, color);
            ansi256[i] = color;
        }
        tty_end_raw(term->tty);
    }
#endif
}

static void term_init_raw(term_t* term) {
    if (term->palette < ANSIRGB) {
        term_update_ansi16(term);
    }
}

#else

ic_private void term_start_raw(term_t* term) {
    if (term->raw_enabled++ > 0) {
        return;
    }
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(term->hcon, &info)) {
        term->hcon_orig_attr = info.wAttributes;
    }
    term->hcon_orig_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    if (term->hcon_mode == 0) {
        // first time initialization
        DWORD mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
                     ENABLE_LVB_GRID_WORLDWIDE;  // for \r \n and \b
        // use escape sequence handling if available and the terminal supports
        // it (so we can use rgb colors in Windows terminal) Unfortunately, in
        // plain powershell, we can successfully enable terminal processing but
        // it still fails to render correctly; so we require the palette be
        // large enough (like in Windows Terminal)
        if (term->palette >= ANSI256 &&
            SetConsoleMode(term->hcon, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            term->hcon_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            debug_msg("term: console mode: virtual terminal processing enabled\n");
        }
        // no virtual terminal processing, emulate instead
        else if (SetConsoleMode(term->hcon, mode)) {
            term->hcon_mode = mode;
            term->palette = ANSI16;
        }
        GetConsoleMode(term->hcon, &mode);
        debug_msg("term: console mode: orig: 0x%x, new: 0x%x, current 0x%x\n", term->hcon_orig_mode,
                  term->hcon_mode, mode);
    } else {
        SetConsoleMode(term->hcon, term->hcon_mode);
    }
}

ic_private void term_end_raw(term_t* term, bool force) {
    if (term->raw_enabled <= 0) {
        return;
    }
    if (!force && term->raw_enabled > 1) {
        term->raw_enabled--;
    } else {
        term->raw_enabled = 0;
        SetConsoleMode(term->hcon, term->hcon_orig_mode);
        SetConsoleOutputCP(term->hcon_orig_cp);
        SetConsoleTextAttribute(term->hcon, term->hcon_orig_attr);
    }
}

static void term_init_raw(term_t* term) {
    term->hcon = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(term->hcon, &term->hcon_orig_mode);
    CONSOLE_SCREEN_BUFFER_INFOEX info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (GetConsoleScreenBufferInfoEx(term->hcon, &info)) {
        // store default attributes
        term->hcon_default_attr = info.wAttributes;
        // update our color table with the actual colors used.
        for (unsigned i = 0; i < 16; i++) {
            COLORREF cr = info.ColorTable[i];
            uint32_t color = (ic_cap8(GetRValue(cr)) << 16) | (ic_cap8(GetGValue(cr)) << 8) |
                             ic_cap8(GetBValue(cr));  // COLORREF = BGR
            // index is also in reverse in the bits 0 and 2
            unsigned j = (i & 0x08) | ((i & 0x04) >> 2) | (i & 0x02) | (i & 0x01) << 2;
            debug_msg("term: ansi color %d is 0x%06x\n", j, color);
            ansi256[j] = color;
        }
    } else {
        DWORD err = GetLastError();
        debug_msg("term: cannot get console screen buffer: %d %x", err, err);
    }
    term_start_raw(term);  // initialize the hcon_mode
    term_end_raw(term, false);
}

#endif
