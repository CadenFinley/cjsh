/*
  stringbuf.c

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

#include "isocline.h"
#include "unicode.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "common.h"
#include "stringbuf.h"

//-------------------------------------------------------------
// In place growable utf-8 strings
//-------------------------------------------------------------

struct stringbuf_s {
    char* buf;
    ssize_t buflen;
    ssize_t count;
    alloc_t* mem;
};

//-------------------------------------------------------------
// String column width
//-------------------------------------------------------------

// column width of a utf8 single character sequence using internal Unicode data.
static ssize_t utf8_char_width(const char* s, ssize_t n) {
    if (n <= 0) {
        return 0;
    }

    unicode_codepoint_t codepoint = 0;
    ssize_t bytes_read = 0;
    bool ok = unicode_decode_utf8((const uint8_t*)s, n, &codepoint, &bytes_read);

    if (!ok || bytes_read <= 0) {
        // Invalid UTF-8, treat as single-width printable character
        return 1;
    }

    int width = unicode_codepoint_width(codepoint);
    if (width <= 0) {
        return 0;
    }

    return (ssize_t)width;
}

// The column width of a codepoint (0, 1, or 2)
static ssize_t char_column_width(const char* s, ssize_t n) {
    if (s == NULL || n <= 0) {
        return 0;
    } else if ((uint8_t)(*s) < ' ') {
        return 0;  // also for CSI escape sequences
    } else if ((uint8_t)(*s) < 0x7F) {
        return 1;  // printable ASCII needs neither UTF-8 decoding nor Unicode tables
    } else {
        ssize_t w = utf8_char_width(s, n);
#ifdef _WIN32
        return (w <= 0 ? 1 : w);  // windows console seems to use at least one column
#else
        return w;
#endif
    }
}

ic_private ssize_t str_column_width_n(const char* s, ssize_t len) {
    if (s == NULL || len <= 0) {
        return 0;
    }
    ssize_t pos = 0;
    ssize_t cwidth = 0;
    ssize_t cw;
    ssize_t ofs;
    while (s[pos] != 0 && (ofs = str_next_ofs(s, len, pos, &cw)) > 0) {
        cwidth += cw;
        pos += ofs;
    }
    return cwidth;
}

ic_private ssize_t str_column_width(const char* s) {
    return str_column_width_n(s, ic_strlen(s));
}

ic_private ssize_t str_skip_until_fit(const char* s, ssize_t max_width) {
    if (s == NULL) {
        return 0;
    }
    ssize_t cwidth = str_column_width(s);
    ssize_t len = ic_strlen(s);
    ssize_t pos = 0;
    ssize_t next;
    ssize_t cw;
    while (cwidth > max_width && (next = str_next_ofs(s, len, pos, &cw)) > 0) {
        cwidth -= cw;
        pos += next;
    }
    return pos;
}

ic_private ssize_t str_take_while_fit(const char* s, ssize_t max_width) {
    if (s == NULL) {
        return 0;
    }
    const ssize_t len = ic_strlen(s);
    ssize_t pos = 0;
    ssize_t next;
    ssize_t cw;
    ssize_t cwidth = 0;
    while ((next = str_next_ofs(s, len, pos, &cw)) > 0) {
        if (cwidth + cw > max_width) {
            break;
        }
        cwidth += cw;
        pos += next;
    }
    return pos;
}

//-------------------------------------------------------------
// String navigation
//-------------------------------------------------------------

// get offset of the previous codepoint. does not skip back over CSI sequences.
ic_private ssize_t str_prev_ofs(const char* s, ssize_t pos, ssize_t* width) {
    ssize_t ofs = 0;
    if (s != NULL && pos > 0) {
        ofs = 1;
        while (pos > ofs) {
            uint8_t u = (uint8_t)s[pos - ofs];
            if (u < 0x80 || u > 0xBF) {
                break;  // continue while follower
            }
            ofs++;
        }
    }
    if (width != NULL) {
        *width = char_column_width(s + (pos - ofs), ofs);
    }
    return ofs;
}

// skip an escape sequence
// <https://www.xfree86.org/current/ctlseqs.html>
ic_private bool skip_esc(const char* s, ssize_t len, ssize_t* esclen) {
    if (s == NULL || len <= 1 || s[0] != '\x1B') {
        return false;
    }
    if (esclen != NULL) {
        *esclen = 0;
    }
    if (strchr("[PX^_]", s[1]) != NULL) {
        // CSI (ESC [), DCS (ESC P), SOS (ESC X), PM (ESC ^), APC (ESC _), and
        // OSC (ESC ]): terminated with a special sequence
        bool finalCSI = (s[1] == '[');  // CSI terminates with 0x40-0x7F;
                                        // otherwise ST (bell or ESC \)
        ssize_t n = 2;
        while (len > n) {
            char c = s[n++];
            if ((finalCSI && (uint8_t)c >= 0x40 &&
                 (uint8_t)c <= 0x7F) ||        // terminating byte: @A–Z[\]^_`a–z{|}~
                (!finalCSI && c == '\x07') ||  // bell
                (c == '\x02'))                 // STX terminates as well
            {
                if (esclen != NULL) {
                    *esclen = n;
                }
                return true;
            } else if (!finalCSI && c == '\x1B' && len > n && s[n] == '\\') {  // ST (ESC \)
                n++;
                if (esclen != NULL) {
                    *esclen = n;
                }
                return true;
            }
        }
    }
    if (strchr(" #%()*+", s[1]) != NULL) {
        // assume escape sequence of length 3 (like ESC % G)
        if (esclen != NULL) {
            *esclen = (len >= 3 ? 3 : 2);
        }
        return true;
    } else {
        // assume single character escape code (like ESC 7)
        if (esclen != NULL) {
            *esclen = 2;
        }
        return true;
    }
    return false;
}

// Offset to the next codepoint, treats CSI escape sequences as a single code
// point.
ic_private ssize_t str_next_ofs(const char* s, ssize_t len, ssize_t pos, ssize_t* cwidth) {
    ssize_t ofs = 0;
    if (s != NULL && len > pos) {
        if (skip_esc(s + pos, len - pos, &ofs)) {
            // skip escape sequence
        } else {
            ofs = 1;
            // utf8 extended character?
            while (len > pos + ofs) {
                uint8_t u = (uint8_t)s[pos + ofs];
                if (u < 0x80 || u > 0xBF) {
                    break;  // break if not a follower
                }
                ofs++;
            }
        }
    }
    if (cwidth != NULL) {
        *cwidth = char_column_width(s + pos, ofs);
    }
    return ofs;
}

static ssize_t str_limit_to_length(const char* s, ssize_t n) {
    ssize_t i;
    for (i = 0; i < n && s[i] != 0; i++) { /* nothing */
    }
    return i;
}

//-------------------------------------------------------------
// String searching prev/next word, line, ws_word
//-------------------------------------------------------------

static ssize_t str_find_backward(const char* s, ssize_t len, ssize_t pos,
                                 ic_is_char_class_fun_t* match, bool skip_immediate_matches) {
    if (pos > len) {
        pos = len;
    }
    if (pos < 0) {
        pos = 0;
    }
    ssize_t i = pos;
    // skip matching first (say, whitespace in case of the previous
    // start-of-word)
    if (skip_immediate_matches) {
        do {
            ssize_t prev = str_prev_ofs(s, i, NULL);
            if (prev <= 0) {
                break;
            }
            assert(i - prev >= 0);
            if (!match(s + i - prev, (long)prev)) {
                break;
            }
            i -= prev;
        } while (i > 0);
    }
    // find match
    do {
        ssize_t prev = str_prev_ofs(s, i, NULL);
        if (prev <= 0) {
            break;
        }
        assert(i - prev >= 0);
        if (match(s + i - prev, (long)prev)) {
            return i;  // found;
        }
        i -= prev;
    } while (i > 0);
    return -1;  // not found
}

static ssize_t str_find_forward(const char* s, ssize_t len, ssize_t pos,
                                ic_is_char_class_fun_t* match, bool skip_immediate_matches) {
    if (s == NULL || len < 0) {
        return -1;
    }
    if (pos > len) {
        pos = len;
    }
    if (pos < 0) {
        pos = 0;
    }
    ssize_t i = pos;
    ssize_t next;
    // skip matching first (say, whitespace in case of the next end-of-word)
    if (skip_immediate_matches) {
        do {
            next = str_next_ofs(s, len, i, NULL);
            if (next <= 0) {
                break;
            }
            assert(i + next <= len);
            if (!match(s + i, (long)next)) {
                break;
            }
            i += next;
        } while (i < len);
    }
    // and then look
    do {
        next = str_next_ofs(s, len, i, NULL);
        if (next <= 0) {
            break;
        }
        assert(i + next <= len);
        if (match(s + i, (long)next)) {
            return i;  // found
        }
        i += next;
    } while (i < len);
    return -1;
}

static bool char_is_linefeed(const char* s, long n) {
    return (n == 1 && (*s == '\n' || *s == 0));
}

static ssize_t str_find_line_start(const char* s, ssize_t len, ssize_t pos) {
    ssize_t start =
        str_find_backward(s, len, pos, &char_is_linefeed, false /* don't skip immediate matches */);
    return (start < 0 ? 0 : start);
}

static ssize_t str_find_line_end(const char* s, ssize_t len, ssize_t pos) {
    ssize_t end = str_find_forward(s, len, pos, &char_is_linefeed, false);
    return (end < 0 ? len : end);
}

static ssize_t str_find_word_start(const char* s, ssize_t len, ssize_t pos) {
    ssize_t start =
        str_find_backward(s, len, pos, &ic_char_is_idletter, true /* skip immediate matches */);
    return (start < 0 ? 0 : start);
}

static ssize_t str_find_word_end(const char* s, ssize_t len, ssize_t pos) {
    ssize_t end =
        str_find_forward(s, len, pos, &ic_char_is_idletter, true /* skip immediate matches */);
    return (end < 0 ? len : end);
}

static ssize_t str_find_ws_word_start(const char* s, ssize_t len, ssize_t pos) {
    ssize_t start =
        str_find_backward(s, len, pos, &ic_char_is_white, true /* skip immediate matches */);
    return (start < 0 ? 0 : start);
}

static ssize_t str_find_ws_word_end(const char* s, ssize_t len, ssize_t pos) {
    ssize_t end =
        str_find_forward(s, len, pos, &ic_char_is_white, true /* skip immediate matches */);
    return (end < 0 ? len : end);
}

//-------------------------------------------------------------
// String row/column iteration
//-------------------------------------------------------------

// invoke a function for each terminal row; returns total row count.
static ssize_t str_for_each_row(const char* s, ssize_t len, ssize_t termw, ssize_t promptw,
                                ssize_t cpromptw, ssize_t wrap_marker_width, row_fun_t* fun,
                                const void* arg, void* res) {
    if (s == NULL) {
        s = "";
        len = 0;
    }
    ssize_t i;
    ssize_t rcount = 0;
    ssize_t rcol = 0;
    ssize_t rstart = 0;
    ssize_t startw = promptw;
    for (i = 0; i < len;) {
        ssize_t w;
        ssize_t next = str_next_ofs(s, len, i, &w);
        if (next <= 0) {
            debug_msg("str: foreach row: next<=0: len %zd, i %zd, w %zd, buf %s\n", len, i, w, s);
            assert(false);
            break;
        }
        startw = (rcount == 0 ? promptw : cpromptw);
        // Reserve the marker's display width. Without a marker, text can fill the final
        // column and its following cursor belongs on the next row.
        ssize_t termcol = rcol + w + startw + wrap_marker_width;
        if (termw > 0 && i != 0 && termcol > termw && (wrap_marker_width || w > 0)) {
            // wrap
            if (fun != NULL) {
                if (fun(s, rcount, rstart, i - rstart, startw, true, arg, res)) {
                    return rcount;
                }
            }
            rcount++;
            rstart = i;
            rcol = 0;
            startw = cpromptw;
        }
        if (s[i] == '\n') {
            // newline
            if (fun != NULL) {
                if (fun(s, rcount, rstart, i - rstart, startw, false, arg, res)) {
                    return rcount;
                }
            }
            rcount++;
            rstart = i + 1;
            rcol = 0;
            startw = cpromptw;
        }
        assert(s[i] != 0);
        i += next;
        rcol += w;
    }
    if (!wrap_marker_width && termw > 0 && rcol > 0 && startw + rcol >= termw) {
        // Materialize the cursor row after text that exactly fills the terminal.
        // An explicit newline has already emitted its row above.
        if (fun != NULL && fun(s, rcount, rstart, i - rstart, startw, true, arg, res)) {
            return rcount;
        }
        rcount++;
        rstart = i;
        startw = cpromptw;
    }
    if (fun != NULL) {
        if (fun(s, rcount, rstart, i - rstart, startw, false, arg, res)) {
            return rcount;
        }
    }
    return rcount + 1;
}

//-------------------------------------------------------------
// String: get row/column position
//-------------------------------------------------------------

static bool str_get_current_pos_iter(const char* s, ssize_t row, ssize_t row_start, ssize_t row_len,
                                     ssize_t startw, bool is_wrap, const void* arg, void* res) {
    ic_unused(is_wrap);
    ic_unused(startw);
    rowcol_t* rc = (rowcol_t*)res;
    ssize_t pos = *((ssize_t*)arg);

    if (pos >= row_start && pos <= (row_start + row_len)) {
        // found the cursor row
        rc->row_start = row_start;
        rc->row_len = row_len;
        rc->row = row;
        rc->col = str_column_width_n(s + row_start, pos - row_start);
        rc->first_on_row = (pos == row_start);
        if (is_wrap) {
            // if wrapped, we check if the next character is at row_len
            ssize_t next = str_next_ofs(s, row_start + row_len, pos, NULL);
            rc->last_on_row = (pos + next >= row_start + row_len);
        } else {
            // normal last position is right after the last character
            rc->last_on_row = (pos >= row_start + row_len);
        }
        // debug_msg("edit; pos iter: pos: %zd (%c), row_start: %zd, rowlen:
        // %zd\n", pos, s[pos], row_start, row_len);
    }
    return false;  // always continue to count all rows
}

static ssize_t str_get_rc_at_pos(const char* s, ssize_t len, ssize_t termw, ssize_t promptw,
                                 ssize_t cpromptw, ssize_t wrap_marker_width, ssize_t pos,
                                 rowcol_t* rc) {
    memset(rc, 0, sizeof(*rc));
    ssize_t rows = str_for_each_row(s, len, termw, promptw, cpromptw, wrap_marker_width,
                                    &str_get_current_pos_iter, &pos, rc);
    // debug_msg("edit: current pos: (%d, %d) %s %s\n", rc->row, rc->col,
    // rc->first_on_row ? "first" : "", rc->last_on_row ? "last" : "");
    return rows;
}

//-------------------------------------------------------------
// String: get row/column position for a resized terminal
// with potentially "hard-wrapped" rows
//-------------------------------------------------------------
typedef struct wrapped_arg_s {
    ssize_t pos;
    ssize_t newtermw;
    ssize_t wrap_marker_width;
} wrapped_arg_t;

typedef struct wrowcol_s {
    rowcol_t rc;
    ssize_t hrows;  // count of hard-wrapped extra rows
} wrowcol_t;

static bool str_get_current_wrapped_pos_iter(const char* s, ssize_t row, ssize_t row_start,
                                             ssize_t row_len, ssize_t startw, bool is_wrap,
                                             const void* arg, void* res) {
    ic_unused(is_wrap);
    wrowcol_t* wrc = (wrowcol_t*)res;
    const wrapped_arg_t* warg = (const wrapped_arg_t*)arg;

    // iterate through the row and record the postion and hard-wraps
    ssize_t hwidth = startw;
    ssize_t i = 0;
    while (i <= row_len) {  // include rowlen as the cursor position can be just
                            // after the last character
        // get next position and column width
        ssize_t cw;
        ssize_t next;
        bool is_cursor = (warg->pos == row_start + i);
        if (i < row_len) {
            next = str_next_ofs(s + row_start, row_len, i, &cw);
        } else {
            // Account for the marker width or the one-column end-of-input cursor.
            // The newline after a wrapped row does not consume another column.
            cw = (is_wrap ? warg->wrap_marker_width : (is_cursor ? 1 : 0));
            next = 1;
        }

        if (next > 0) {
            if (hwidth + cw > warg->newtermw) {
                // hardwrap
                hwidth = 0;
                wrc->hrows++;
                debug_msg("str: found hardwrap: row: %zd, hrows: %zd\n", row, wrc->hrows);
            }
        } else {
            next++;  // ensure we terminate (as we go up to rowlen)
        }

        // did we find our position?
        if (is_cursor) {
            debug_msg("str: found position: row: %zd, hrows: %zd\n", row, wrc->hrows);
            wrc->rc.row_start = row_start;
            wrc->rc.row_len = row_len;
            wrc->rc.row = wrc->hrows + row;
            wrc->rc.col = hwidth;
            wrc->rc.first_on_row = (i == 0);
            wrc->rc.last_on_row = (i + next >= row_len - (is_wrap ? 1 : 0));
        }

        // advance
        hwidth += cw;
        i += next;
    }
    return false;  // always continue to count all rows
}

static ssize_t str_get_wrapped_rc_at_pos(const char* s, ssize_t len, ssize_t termw,
                                         ssize_t newtermw, ssize_t promptw, ssize_t cpromptw,
                                         ssize_t wrap_marker_width, ssize_t pos, rowcol_t* rc) {
    wrapped_arg_t warg;
    warg.pos = pos;
    warg.newtermw = newtermw;
    warg.wrap_marker_width = wrap_marker_width;
    wrowcol_t wrc;
    memset(&wrc, 0, sizeof(wrc));
    ssize_t rows = str_for_each_row(s, len, termw, promptw, cpromptw, wrap_marker_width,
                                    &str_get_current_wrapped_pos_iter, &warg, &wrc);
    debug_msg("edit: wrapped pos: (%zd,%zd) rows %zd %s %s, hrows: %zd\n", wrc.rc.row, wrc.rc.col,
              rows, wrc.rc.first_on_row ? "first" : "", wrc.rc.last_on_row ? "last" : "",
              wrc.hrows);
    *rc = wrc.rc;
    return (rows + wrc.hrows);
}

//-------------------------------------------------------------
// Set position
//-------------------------------------------------------------

static bool str_set_pos_iter(const char* s, ssize_t row, ssize_t row_start, ssize_t row_len,
                             ssize_t startw, bool is_wrap, const void* arg, void* res) {
    ic_unused(arg);
    ic_unused(is_wrap);
    ic_unused(startw);
    rowcol_t* rc = (rowcol_t*)arg;
    if (rc->row != row) {
        return false;  // keep searching
    }
    // we found our row
    ssize_t col = 0;
    ssize_t i = row_start;
    ssize_t end = row_start + row_len;
    while (col < rc->col && i < end) {
        ssize_t cw;
        ssize_t next = str_next_ofs(s, row_start + row_len, i, &cw);
        if (next <= 0) {
            break;
        }
        i += next;
        col += cw;
    }
    *((ssize_t*)res) = i;
    return true;  // stop iteration
}

static ssize_t str_get_pos_at_rc(const char* s, ssize_t len, ssize_t termw, ssize_t promptw,
                                 ssize_t cpromptw, ssize_t wrap_marker_width, ssize_t row,
                                 ssize_t col /* without prompt */) {
    rowcol_t rc;
    memset(&rc, 0, ssizeof(rc));
    rc.row = row;
    rc.col = col;
    ssize_t pos = -1;
    (void)str_for_each_row(s, len, termw, promptw, cpromptw, wrap_marker_width, &str_set_pos_iter,
                           &rc, &pos);
    return pos;
}

//-------------------------------------------------------------
// String buffer
//-------------------------------------------------------------
static bool sbuf_ensure_extra(stringbuf_t* s, ssize_t extra) {
    const ssize_t maxlen = (ssize_t)(SIZE_MAX / 2) - 1;  // reserve the terminating zero
    if (extra < 0 || s->count > maxlen - extra) {
        return false;
    }
    const ssize_t needed = s->count + extra;
    if (s->buflen >= needed) {
        return true;
    }
    // Geometric growth also applies to large pastes and rendered menus.
    ssize_t newlen = (s->buflen <= 0 ? 256 : s->buflen);
    if (newlen < needed) {
        const ssize_t growth = newlen / 2;
        newlen = (newlen > maxlen - growth ? maxlen : newlen + growth);
        if (newlen < needed) {
            newlen = needed;
        }
    }
    if (s->buflen > 0) {
        debug_msg("stringbuf: reallocate: old %zd, new %zd\n", s->buflen, newlen);
    }
    char* newbuf = mem_realloc_tp(s->mem, char, s->buf,
                                  newlen + 1);  // one more for terminating zero
    if (newbuf == NULL) {
        return false;
    }
    s->buf = newbuf;
    s->buflen = newlen;
    s->buf[s->count] = s->buf[s->buflen] = 0;
    assert(s->buflen >= s->count + extra);
    return true;
}

static void sbuf_init(stringbuf_t* sbuf, alloc_t* mem) {
    sbuf->mem = mem;
    sbuf->buf = NULL;
    sbuf->buflen = 0;
    sbuf->count = 0;
}

static void sbuf_done(stringbuf_t* sbuf) {
    mem_free(sbuf->mem, sbuf->buf);
    sbuf->buf = NULL;
    sbuf->buflen = 0;
    sbuf->count = 0;
}

ic_private void sbuf_free(stringbuf_t* sbuf) {
    if (sbuf == NULL) {
        return;
    }
    sbuf_done(sbuf);
    mem_free(sbuf->mem, sbuf);
}

ic_private stringbuf_t* sbuf_new(alloc_t* mem) {
    stringbuf_t* sbuf = mem_zalloc_tp(mem, stringbuf_t);
    if (sbuf == NULL) {
        return NULL;
    }
    sbuf_init(sbuf, mem);
    return sbuf;
}

// free the sbuf and return the current string buffer as the result
ic_private char* sbuf_free_dup(stringbuf_t* sbuf) {
    if (sbuf == NULL) {
        return NULL;
    }
    char* s = NULL;
    if (sbuf->buf != NULL) {
        s = mem_realloc_tp(sbuf->mem, char, sbuf->buf, sbuf_len(sbuf) + 1);
        if (s == NULL) {
            s = sbuf->buf;
        }
        sbuf->buf = 0;
        sbuf->buflen = 0;
        sbuf->count = 0;
    }
    sbuf_free(sbuf);
    return s;
}

ic_private const char* sbuf_string_at(stringbuf_t* sbuf, ssize_t pos) {
    if (pos < 0 || sbuf->count < pos) {
        return NULL;
    }
    if (sbuf->buf == NULL) {
        return "";
    }
    assert(sbuf->buf[sbuf->count] == 0);
    return sbuf->buf + pos;
}

ic_private const char* sbuf_string(stringbuf_t* sbuf) {
    return sbuf_string_at(sbuf, 0);
}

ic_private char sbuf_char_at(stringbuf_t* sbuf, ssize_t pos) {
    if (sbuf->buf == NULL || pos < 0 || sbuf->count < pos) {
        return 0;
    }
    return sbuf->buf[pos];
}

ic_private char* sbuf_strdup_at(stringbuf_t* sbuf, ssize_t pos) {
    return mem_strdup(sbuf->mem, sbuf_string_at(sbuf, pos));
}

ic_private char* sbuf_strdup(stringbuf_t* sbuf) {
    return mem_strdup(sbuf->mem, sbuf_string(sbuf));
}

ic_private ssize_t sbuf_len(const stringbuf_t* s) {
    if (s == NULL) {
        return 0;
    }
    return s->count;
}

ic_private ssize_t sbuf_append_vprintf(stringbuf_t* sb, const char* fmt, va_list args) {
    const ssize_t min_needed = ic_strlen(fmt);
    if (!sbuf_ensure_extra(sb, min_needed + 16)) {
        return sb->count;
    }
    ssize_t avail = sb->buflen - sb->count;
    va_list args0;
    va_copy(args0, args);
    ssize_t needed = vsnprintf(sb->buf + sb->count, to_size_t(avail), fmt, args0);
    va_end(args0);
    if (needed > avail) {
        sb->buf[sb->count] = 0;
        if (!sbuf_ensure_extra(sb, needed)) {
            return sb->count;
        }
        avail = sb->buflen - sb->count;
        needed = vsnprintf(sb->buf + sb->count, to_size_t(avail), fmt, args);
    }
    assert(needed <= avail);
    sb->count += (needed > avail ? avail : (needed >= 0 ? needed : 0));
    assert(sb->count <= sb->buflen);
    sb->buf[sb->count] = 0;
    return sb->count;
}

ic_private ssize_t sbuf_appendf(stringbuf_t* sb, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ssize_t res = sbuf_append_vprintf(sb, fmt, args);
    va_end(args);
    return res;
}

ic_private ssize_t sbuf_insert_at_n(stringbuf_t* sbuf, const char* s, ssize_t n, ssize_t pos) {
    if (pos < 0 || pos > sbuf->count || s == NULL) {
        return pos;
    }
    n = str_limit_to_length(s, n);
    if (n <= 0 || !sbuf_ensure_extra(sbuf, n)) {
        return pos;
    }
    ic_memmove(sbuf->buf + pos + n, sbuf->buf + pos, sbuf->count - pos);
    ic_memcpy(sbuf->buf + pos, s, n);
    sbuf->count += n;
    sbuf->buf[sbuf->count] = 0;
    return (pos + n);
}

ic_private stringbuf_t* sbuf_split_at(stringbuf_t* sb, ssize_t pos) {
    if (pos < 0) {
        return NULL;
    }
    stringbuf_t* res = sbuf_new(sb->mem);
    if (res == NULL) {
        return NULL;
    }
    if (pos < sb->count) {
        (void)sbuf_append_n(res, sb->buf + pos, sb->count - pos);
        sb->count = pos;
    }
    return res;
}

ic_private ssize_t sbuf_insert_at(stringbuf_t* sbuf, const char* s, ssize_t pos) {
    return sbuf_insert_at_n(sbuf, s, ic_strlen(s), pos);
}

ic_private ssize_t sbuf_insert_char_at(stringbuf_t* sbuf, char c, ssize_t pos) {
    char s[2];
    s[0] = c;
    s[1] = 0;
    return sbuf_insert_at_n(sbuf, s, 1, pos);
}

ic_private ssize_t sbuf_insert_unicode_at(stringbuf_t* sbuf, unicode_t u, ssize_t pos) {
    uint8_t s[5];
    unicode_to_qutf8(u, s);
    return sbuf_insert_at(sbuf, (const char*)s, pos);
}

ic_private void sbuf_delete_at(stringbuf_t* sbuf, ssize_t pos, ssize_t count) {
    if (pos < 0 || pos >= sbuf->count) {
        return;
    }
    if (pos + count > sbuf->count) {
        count = sbuf->count - pos;
    }
    ic_memmove(sbuf->buf + pos, sbuf->buf + pos + count, sbuf->count - pos - count);
    sbuf->count -= count;
    sbuf->buf[sbuf->count] = 0;
}

ic_private void sbuf_delete_from_to(stringbuf_t* sbuf, ssize_t pos, ssize_t end) {
    if (end <= pos) {
        return;
    }
    sbuf_delete_at(sbuf, pos, end - pos);
}

ic_private void sbuf_delete_from(stringbuf_t* sbuf, ssize_t pos) {
    sbuf_delete_at(sbuf, pos, sbuf_len(sbuf) - pos);
}

ic_private void sbuf_clear(stringbuf_t* sbuf) {
    sbuf_delete_at(sbuf, 0, sbuf_len(sbuf));
}

ic_private ssize_t sbuf_append_n(stringbuf_t* sbuf, const char* s, ssize_t n) {
    return sbuf_insert_at_n(sbuf, s, n, sbuf_len(sbuf));
}

ic_private ssize_t sbuf_append(stringbuf_t* sbuf, const char* s) {
    return sbuf_insert_at(sbuf, s, sbuf_len(sbuf));
}

ic_private ssize_t sbuf_append_char(stringbuf_t* sbuf, char c) {
    char buf[2];
    buf[0] = c;
    buf[1] = 0;
    return sbuf_append(sbuf, buf);
}

ic_private void sbuf_replace(stringbuf_t* sbuf, const char* s) {
    sbuf_clear(sbuf);
    (void)sbuf_append(sbuf, s);
}

ic_private ssize_t sbuf_next_ofs(stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth) {
    return str_next_ofs(sbuf->buf, sbuf->count, pos, cwidth);
}

ic_private ssize_t sbuf_prev_ofs(stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth) {
    return str_prev_ofs(sbuf->buf, pos, cwidth);
}

ic_private ssize_t sbuf_next(stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth) {
    ssize_t ofs = sbuf_next_ofs(sbuf, pos, cwidth);
    if (ofs <= 0) {
        return -1;
    }
    assert(pos + ofs <= sbuf->count);
    return pos + ofs;
}

ic_private ssize_t sbuf_prev(stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth) {
    ssize_t ofs = sbuf_prev_ofs(sbuf, pos, cwidth);
    if (ofs <= 0) {
        return -1;
    }
    assert(pos - ofs >= 0);
    return pos - ofs;
}

ic_private ssize_t sbuf_delete_char_before(stringbuf_t* sbuf, ssize_t pos) {
    ssize_t n = sbuf_prev_ofs(sbuf, pos, NULL);
    if (n <= 0) {
        return 0;
    }
    assert(pos - n >= 0);
    sbuf_delete_at(sbuf, pos - n, n);
    return pos - n;
}

ic_private void sbuf_delete_char_at(stringbuf_t* sbuf, ssize_t pos) {
    ssize_t n = sbuf_next_ofs(sbuf, pos, NULL);
    if (n <= 0) {
        return;
    }
    assert(pos + n <= sbuf->count);
    sbuf_delete_at(sbuf, pos, n);
}

ic_private ssize_t sbuf_swap_char(stringbuf_t* sbuf, ssize_t pos) {
    ssize_t next = sbuf_next_ofs(sbuf, pos, NULL);
    if (next <= 0) {
        return 0;
    }
    ssize_t prev = sbuf_prev_ofs(sbuf, pos, NULL);
    if (prev <= 0) {
        return 0;
    }
    char buf[64];
    if (prev >= 63) {
        return 0;
    }
    ic_memcpy(buf, sbuf->buf + pos - prev, prev);
    ic_memmove(sbuf->buf + pos - prev, sbuf->buf + pos, next);
    ic_memmove(sbuf->buf + pos - prev + next, buf, prev);
    return pos - prev;
}

ic_private ssize_t sbuf_find_line_start(stringbuf_t* sbuf, ssize_t pos) {
    return str_find_line_start(sbuf->buf, sbuf->count, pos);
}

ic_private ssize_t sbuf_find_line_end(stringbuf_t* sbuf, ssize_t pos) {
    return str_find_line_end(sbuf->buf, sbuf->count, pos);
}

ic_private ssize_t sbuf_find_word_start(stringbuf_t* sbuf, ssize_t pos) {
    return str_find_word_start(sbuf->buf, sbuf->count, pos);
}

ic_private ssize_t sbuf_find_word_end(stringbuf_t* sbuf, ssize_t pos) {
    return str_find_word_end(sbuf->buf, sbuf->count, pos);
}

ic_private ssize_t sbuf_find_ws_word_start(stringbuf_t* sbuf, ssize_t pos) {
    return str_find_ws_word_start(sbuf->buf, sbuf->count, pos);
}

ic_private ssize_t sbuf_find_ws_word_end(stringbuf_t* sbuf, ssize_t pos) {
    return str_find_ws_word_end(sbuf->buf, sbuf->count, pos);
}

// find row/col position
ic_private ssize_t sbuf_get_pos_at_rc(stringbuf_t* sbuf, ssize_t termw, ssize_t promptw,
                                      ssize_t cpromptw, ssize_t wrap_marker_width, ssize_t row,
                                      ssize_t col) {
    return str_get_pos_at_rc(sbuf->buf, sbuf->count, termw, promptw, cpromptw, wrap_marker_width,
                             row, col);
}

// get row/col for a given position
ic_private ssize_t sbuf_get_rc_at_pos(stringbuf_t* sbuf, ssize_t termw, ssize_t promptw,
                                      ssize_t cpromptw, ssize_t wrap_marker_width, ssize_t pos,
                                      rowcol_t* rc) {
    return str_get_rc_at_pos(sbuf->buf, sbuf->count, termw, promptw, cpromptw, wrap_marker_width,
                             pos, rc);
}

ic_private ssize_t sbuf_get_wrapped_rc_at_pos(stringbuf_t* sbuf, ssize_t termw, ssize_t newtermw,
                                              ssize_t promptw, ssize_t cpromptw,
                                              ssize_t wrap_marker_width, ssize_t pos,
                                              rowcol_t* rc) {
    return str_get_wrapped_rc_at_pos(sbuf->buf, sbuf->count, termw, newtermw, promptw, cpromptw,
                                     wrap_marker_width, pos, rc);
}

ic_private ssize_t sbuf_for_each_row(stringbuf_t* sbuf, ssize_t termw, ssize_t promptw,
                                     ssize_t cpromptw, ssize_t wrap_marker_width, row_fun_t* fun,
                                     void* arg, void* res) {
    if (sbuf == NULL) {
        return 0;
    }
    return str_for_each_row(sbuf->buf, sbuf->count, termw, promptw, cpromptw, wrap_marker_width,
                            fun, arg, res);
}

// Duplicate and decode from utf-8 (for non-utf8 terminals)
ic_private char* sbuf_strdup_from_utf8(stringbuf_t* sbuf) {
    ssize_t len = sbuf_len(sbuf);
    if (sbuf == NULL) {
        return NULL;
    }
    char* s = mem_zalloc_tp_n(sbuf->mem, char, len + 1);
    if (s == NULL) {
        return NULL;
    }
    ssize_t dest = 0;
    for (ssize_t i = 0; i < len;) {
        ssize_t ofs = sbuf_next_ofs(sbuf, i, NULL);
        if (ofs <= 0) {
            // invalid input
            break;
        } else if (ofs == 1) {
            // regular character
            s[dest++] = sbuf->buf[i];
        } else if (sbuf->buf[i] == '\x1B') {
            // skip escape sequences
        } else {
            // decode unicode
            ssize_t nread;
            unicode_t uchr = unicode_from_qutf8((const uint8_t*)(sbuf->buf + i), ofs, &nread);
            uint8_t c;
            if (unicode_is_raw(uchr, &c)) {
                // raw byte, output as is (this will take care of locale
                // specific input)
                s[dest++] = (char)c;
            } else if (uchr <= 0x7F) {
                // allow ascii
                s[dest++] = (char)uchr;
            } else {
                // Skip unsupported unicode characters to keep output best-effort.
                // todo: convert according to locale?
            }
        }
        i += ofs;
    }
    assert(dest <= len);
    s[dest] = 0;
    return s;
}

//-------------------------------------------------------------
// String helpers
//-------------------------------------------------------------

ic_public long ic_prev_char(const char* s, long pos) {
    ssize_t len = ic_strlen(s);
    if (pos < 0 || pos > len) {
        return -1;
    }
    ssize_t ofs = str_prev_ofs(s, pos, NULL);
    if (ofs <= 0) {
        return -1;
    }
    return (pos - ofs);
}

ic_public long ic_next_char(const char* s, long pos) {
    ssize_t len = ic_strlen(s);
    if (pos < 0 || pos > len) {
        return -1;
    }
    ssize_t ofs = str_next_ofs(s, len, pos, NULL);
    if (ofs <= 0) {
        return -1;
    }
    return (pos + ofs);
}

// Convenience: character class for whitespace `[ \t\r\n]`.
ic_public bool ic_char_is_white(const char* s, long len) {
    if (s == NULL || len != 1) {
        return false;
    }
    const char c = *s;
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

// Convenience: character class for non-whitespace `[^ \t\r\n]`.
ic_public bool ic_char_is_nonwhite(const char* s, long len) {
    return !ic_char_is_white(s, len);
}

static bool parse_ssize_internal(const char* s, ssize_t* out, char** parsed_end) {
    if (s == NULL || out == NULL) {
        return false;
    }
    errno = 0;
    char* local_end = NULL;
    long long value = strtoll(s, &local_end, 10);
    if (errno != 0 || local_end == s) {
        return false;
    }
#if defined(SSIZE_MAX) && defined(SSIZE_MIN)
    if (value < (long long)SSIZE_MIN || value > (long long)SSIZE_MAX) {
        return false;
    }
#endif
    *out = (ssize_t)value;
    if (parsed_end != NULL) {
        *parsed_end = local_end;
    }
    return true;
}

static bool parse_ssize_strict(const char* s, ssize_t* out) {
    char* endptr = NULL;
    if (!parse_ssize_internal(s, out, &endptr)) {
        return false;
    }
    return (endptr != NULL) ? (*endptr == '\0') : false;
}

// parse a decimal (leave pi unchanged on error)
ic_private bool ic_atoz(const char* s, ssize_t* pi) {
    return parse_ssize_strict(s, pi);
}

// parse two decimals separated by a semicolon
ic_private bool ic_atoz2(const char* s, ssize_t* pi, ssize_t* pj) {
    if (s == NULL || pi == NULL || pj == NULL) {
        return false;
    }
    char* endptr = NULL;
    if (!parse_ssize_internal(s, pi, &endptr) || endptr == NULL) {
        return false;
    }
    if (*endptr != ';') {
        return false;
    }
    return parse_ssize_strict(endptr + 1, pj);
}

// parse unsigned 32-bit (leave pu unchanged on error)
ic_private bool ic_atou32(const char* s, uint32_t* pu) {
    if (s == NULL || pu == NULL) {
        return false;
    }
    errno = 0;
    char* endptr = NULL;
    unsigned long value = strtoul(s, &endptr, 10);
    if (errno != 0 || endptr == s || *endptr != '\0' || value > UINT32_MAX) {
        return false;
    }
    *pu = (uint32_t)value;
    return true;
}

// Convenience: character class for separators `[ \t\r\n,.;:/\\\(\)\{\}\[\]]`.
ic_public bool ic_char_is_separator(const char* s, long len) {
    if (s == NULL || len != 1) {
        return false;
    }
    const char c = *s;
    return (strchr(" \t\r\n,.;:/\\(){}[]", c) != NULL);
}

// Convenience: character class for non-separators.
ic_public bool ic_char_is_nonseparator(const char* s, long len) {
    return !ic_char_is_separator(s, len);
}

// Convenience: character class for digits (`[0-9]`).
ic_public bool ic_char_is_digit(const char* s, long len) {
    if (s == NULL || len != 1) {
        return false;
    }
    const char c = *s;
    return (c >= '0' && c <= '9');
}

// Convenience: character class for hexadecimal digits (`[A-Fa-f0-9]`).
ic_public bool ic_char_is_hexdigit(const char* s, long len) {
    if (s == NULL || len != 1) {
        return false;
    }
    const char c = *s;
    return ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
}

// Convenience: character class for letters (`[A-Za-z]` and any unicode > 0x80).
ic_public bool ic_char_is_letter(const char* s, long len) {
    if (s == NULL || len <= 0) {
        return false;
    }
    const char c = *s;
    return ((uint8_t)c >= 0x80 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

// Convenience: character class for identifier letters (`[A-Za-z0-9_-]` and any
// unicode > 0x80).
ic_public bool ic_char_is_idletter(const char* s, long len) {
    if (s == NULL || len <= 0) {
        return false;
    }
    const char c = *s;
    return ((uint8_t)c >= 0x80 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || (c == '_') || (c == '-'));
}

// Convenience: character class for filename letters (`[^ \t\r\n`@$><=;|&{(]`).
ic_public bool ic_char_is_filename_letter(const char* s, long len) {
    if (s == NULL || len <= 0) {
        return false;
    }
    const char c = *s;
    return ((uint8_t)c >= 0x80 || (strchr(" \t\r\n`@$><=;|&{}()[]", c) == NULL));
}

// Convenience: If this is a token start, returns the length (or <= 0 if not
// found).
ic_public long ic_is_token(const char* s, long pos, ic_is_char_class_fun_t* is_token_char) {
    if (s == NULL || pos < 0 || is_token_char == NULL) {
        return -1;
    }
    ssize_t len = ic_strlen(s);
    if (pos >= len) {
        return -1;
    }
    if (pos > 0 && is_token_char(s + pos - 1, 1)) {
        return -1;  // token start?
    }
    ssize_t i = pos;
    while (i < len) {
        ssize_t next = str_next_ofs(s, len, i, NULL);
        if (next <= 0) {
            return -1;
        }
        if (!is_token_char(s + i, (long)next)) {
            break;
        }
        i += next;
    }
    return (i - pos);
}

static int ic_strncmp(const char* s1, const char* s2, ssize_t n) {
    return strncmp(s1, s2, to_size_t(n));
}

ic_private ssize_t ic_count_end_overlap(const char* s, const char* postfix) {
    if (s == NULL || postfix == NULL) {
        return 0;
    }
    const ssize_t slen = ic_strlen(s);
    for (ssize_t count = ic_strlen(postfix); count > 0; count--) {
        if (count <= slen && ic_strncmp(&s[slen - count], postfix, count) == 0) {
            return count;
        }
    }
    return 0;
}

// Convenience: Does this match the specified token?
// Ensures not to match prefixes or suffixes, and returns the length of the
// match (in bytes). E.g.
// `ic_match_token("function",0,&ic_char_is_letter,"fun")` returns 0.
ic_public long ic_match_token(const char* s, long pos, ic_is_char_class_fun_t* is_token_char,
                              const char* token) {
    long n = ic_is_token(s, pos, is_token_char);
    if (n > 0 && token != NULL && n == ic_strlen(token) && ic_strncmp(s + pos, token, n) == 0) {
        return n;
    } else {
        return 0;
    }
}

// Convenience: Do any of the specified tokens match?
// Ensures not to match prefixes or suffixes, and returns the length of the
// match (in bytes). Ensures not to match prefixes or suffixes. E.g.
// `ic_match_any_token("function",0,&ic_char_is_letter,{"fun","func",NULL})`
// returns 0.
ic_public long ic_match_any_token(const char* s, long pos, ic_is_char_class_fun_t* is_token_char,
                                  const char** tokens) {
    long n = ic_is_token(s, pos, is_token_char);
    if (n <= 0 || tokens == NULL) {
        return 0;
    }
    for (const char** token = tokens; *token != NULL; token++) {
        if (n == ic_strlen(*token) && ic_strncmp(s + pos, *token, n) == 0) {
            return n;
        }
    }
    return 0;
}
