/*
  tty.c

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

// Match the aggregated build's signal API visibility under strict C11. A POSIX-only
// feature level would hide SIGWINCH on macOS and disable these handlers there.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <assert.h>
#include <stdint.h>
#include <sys/types.h>
#include "common.h"
#include "keycodes.h"

#include "tty.h"

#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define isatty(fd) _isatty(fd)
#define read(fd, s, n) _read(fd, s, n)
#define STDIN_FILENO 0
#if (_WIN32_WINNT < 0x0600)
WINBASEAPI ULONGLONG WINAPI GetTickCount64(VOID);
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>  // IWYU pragma: keep
#include <sys/time.h>    // IWYU pragma: keep
#include <termios.h>
#include <unistd.h>
#endif

#include "stringbuf.h"

#define TTY_PUSH_MAX (32)
#define TTY_CPUSH_MAX (256)

struct tty_s {
    int fd_in;
    bool raw_enabled;
    bool is_utf8;
    bool has_term_resize_event;
    bool term_resize_event;
#if !defined(_WIN32)
    volatile sig_atomic_t readline_event;
    volatile sig_atomic_t readline_wakeup_enabled;
#endif
    bool lost_terminal;
    alloc_t* mem;
    code_t pushbuf[TTY_PUSH_MAX];
    ssize_t push_count;
    uint8_t cpushbuf[TTY_CPUSH_MAX];
    ssize_t cpush_count;
    stringbuf_t* typeahead_replay;
    ssize_t typeahead_replay_pos;
    bool typeahead_capture_mode;
    bool typeahead_crlf_swapped;
    long esc_initial_timeout;

    long esc_timeout;
#if defined(_WIN32)
    HANDLE hcon;
    DWORD hcon_orig_mode;
#else
    struct termios orig_ios;
    struct termios raw_ios;
    struct termios typeahead_ios;
    int wake_pipe[2];
#endif
    bool wake_pipe_initialized;
    bool paste_mode;
    tty_mouse_event_t last_mouse_event;
};

#if !defined(_WIN32)
static bool tty_setup_wakeup_channel(tty_t* tty) {
    tty->wake_pipe[0] = -1;
    tty->wake_pipe[1] = -1;
    if (pipe(tty->wake_pipe) != 0) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (fcntl(tty->wake_pipe[i], F_SETFD, FD_CLOEXEC) == -1) {
            continue;
        }
        int flags = fcntl(tty->wake_pipe[i], F_GETFL, 0);
        if (flags != -1) {
            (void)fcntl(tty->wake_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    tty->wake_pipe_initialized = true;
    return true;
}

static void tty_close_wakeup_channel(tty_t* tty) {
    if (!tty->wake_pipe_initialized) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (tty->wake_pipe[i] >= 0) {
            (void)close(tty->wake_pipe[i]);
            tty->wake_pipe[i] = -1;
        }
    }
    tty->wake_pipe_initialized = false;
}

static void tty_drain_wakeup_pipe(tty_t* tty) {
    if (!tty->wake_pipe_initialized) {
        return;
    }
    uint8_t buffer[64];
    while (read(tty->wake_pipe[0], buffer, sizeof(buffer)) > 0) {
        // keep draining
    }
}

static void tty_wakeup(tty_t* tty) {
    if (tty == NULL || !tty->wake_pipe_initialized) {
        return;
    }
    uint8_t byte = 1;
    (void)write(tty->wake_pipe[1], &byte, sizeof(byte));
}
#else
static bool tty_setup_wakeup_channel(tty_t* tty) {
    ic_unused(tty);
    if (tty != NULL) {
        tty->wake_pipe_initialized = false;
    }
    return true;
}

static void tty_close_wakeup_channel(tty_t* tty) {
    if (tty != NULL) {
        tty->wake_pipe_initialized = false;
    }
}

static void tty_drain_wakeup_pipe(tty_t* tty) {
    ic_unused(tty);
}

static void tty_wakeup(tty_t* tty) {
    ic_unused(tty);
}
#endif

//-------------------------------------------------------------
// Key code helpers
//-------------------------------------------------------------

ic_private bool code_is_ascii_char(code_t c, char* chr) {
    if (c >= ' ' && c <= 0x7F) {
        if (chr != NULL) {
            *chr = (char)c;
        }
        return true;
    } else {
        if (chr != NULL) {
            *chr = 0;
        }
        return false;
    }
}

ic_private bool code_is_unicode(code_t c, unicode_t* uchr) {
    if (c <= KEY_UNICODE_MAX) {
        if (uchr != NULL) {
            *uchr = c;
        }
        return true;
    } else {
        if (uchr != NULL) {
            *uchr = 0;
        }
        return false;
    }
}

ic_private bool code_is_virt_key(code_t c) {
    return (KEY_NO_MODS(c) <= 0x20 || KEY_NO_MODS(c) >= KEY_VIRT);
}

static code_t modify_code(code_t code, bool in_paste_mode);

ic_private void tty_set_last_mouse_event(tty_t* tty, const tty_mouse_event_t* event) {
    if (tty == NULL) {
        return;
    }

    if (event == NULL) {
        memset(&tty->last_mouse_event, 0, sizeof(tty->last_mouse_event));
        tty->last_mouse_event.action = TTY_MOUSE_ACTION_NONE;
        return;
    }

    tty->last_mouse_event = *event;
}

ic_private void tty_clear_last_mouse_event(tty_t* tty) {
    tty_set_last_mouse_event(tty, NULL);
}

ic_private bool tty_get_last_mouse_event(const tty_t* tty, tty_mouse_event_t* event) {
    if (tty == NULL || event == NULL) {
        return false;
    }

    *event = tty->last_mouse_event;
    return (tty->last_mouse_event.action != TTY_MOUSE_ACTION_NONE);
}

static code_t tty_read_utf8(tty_t* tty, uint8_t c0) {
    uint8_t buf[5];
    memset(buf, 0, 5);

    buf[0] = c0;
    ssize_t count = 1;
    if ((c0 > 0x7F) && tty_readc_noblock(tty, buf + count, tty->esc_timeout)) {
        count++;
        if ((c0 > 0xDF) && tty_readc_noblock(tty, buf + count, tty->esc_timeout)) {
            count++;
            if ((c0 > 0xEF) && tty_readc_noblock(tty, buf + count, tty->esc_timeout)) {
                count++;
            }
        }
    }

    buf[count] = 0;
    debug_msg("tty: read utf8: count: %zd: %02x,%02x,%02x,%02x", count, buf[0], buf[1], buf[2],
              buf[3]);

    ssize_t read = 0;
    code_t code = key_unicode(unicode_from_qutf8(buf, count, &read));

    while (count > read) {
        count--;
        if (count >= 0 && count <= 4) {
            tty_cpush_char(tty, buf[count]);
        }
    }
    return code;
}

static bool tty_code_pop(tty_t* tty, code_t* code);

ic_private bool tty_read_timeout(tty_t* tty, long timeout_ms, code_t* code) {
    while (true) {
        tty_clear_last_mouse_event(tty);

#if !defined(_WIN32)
        if (!tty->paste_mode && tty->readline_event != 0) {
            tty->readline_event = 0;
            tty_drain_wakeup_pipe(tty);
            *code = KEY_EVENT_READLINE;
            return true;
        }
#endif

        if (tty_code_pop(tty, code)) {
            return true;
        }

        uint8_t c;

        // A wakeup may interrupt the wait for a key, but never the remaining
        // bytes of an escape/UTF-8 sequence or a terminal query response.
#if !defined(_WIN32)
        tty->readline_wakeup_enabled = 1;
        if (!tty->paste_mode && tty->readline_event != 0) {
            tty->readline_wakeup_enabled = 0;
            continue;
        }
#endif
        const bool have_char = tty_readc_noblock(tty, &c, timeout_ms);
#if !defined(_WIN32)
        tty->readline_wakeup_enabled = 0;
        if (tty->readline_event != 0) {
            tty_drain_wakeup_pipe(tty);
        }
#endif
        if (!have_char) {
#if !defined(_WIN32)
            if (!tty->paste_mode && tty->readline_event != 0) {
                continue;
            }
#endif
            if (tty->term_resize_event) {
                *code = KEY_EVENT_RESIZE;
                tty->term_resize_event = false;
                return true;
            }
            if (timeout_ms < 0) {
                // tty_readc_noblock can return false on wake-up notifications used
                // by tty_code_pushback while still keeping the terminal active.
                // Keep waiting unless the terminal was actually lost.
                if (tty->push_count > 0 || !tty->lost_terminal) {
                    continue;
                }

                /* If a blocking read (timeout_ms < 0) returned no data (likely EOF),
                   surface an explicit stop event so the caller can break out instead
                   of spinning on KEY_NONE. For non-blocking reads, keep returning
                   false as before. */
                *code = KEY_EVENT_STOP;
                return true;
            }
            return false;
        }

        if (c == KEY_ESC) {
            *code = tty_read_esc(tty, tty->esc_initial_timeout, tty->esc_timeout);
            // A discarded terminal response is not a NUL key. In particular,
            // late palette replies must not become Ctrl+Space completions.
            if (*code == KEY_NONE) {
                continue;
            }
        } else if (c <= 0x7F) {
            *code = key_unicode(c);
        } else if (tty->is_utf8) {
            *code = tty_read_utf8(tty, c);
        } else {
            *code = key_unicode(unicode_from_raw(c));
        }

        *code = modify_code(*code, tty->paste_mode);

        if (*code == IC_KEY_PASTE_START) {
            tty->paste_mode = true;
            debug_msg("tty: entering paste mode\n");
        } else if (*code == IC_KEY_PASTE_END) {
            tty->paste_mode = false;
            debug_msg("tty: exiting paste mode\n");
        }

        return true;
    }
}

static code_t modify_code(code_t code, bool in_paste_mode) {
    code_t key = KEY_NO_MODS(code);
    code_t mods = KEY_MODS(code);
    debug_msg("tty: readc %s%s%s 0x%03x ('%c')\n", mods & KEY_MOD_SHIFT ? "shift+" : "",
              mods & KEY_MOD_CTRL ? "ctrl+" : "", mods & KEY_MOD_ALT ? "alt+" : "", key,
              (key >= ' ' && key <= '~' ? key : ' '));

    if (key == KEY_NONE && mods == 0 && !in_paste_mode) {
        code = WITH_CTRL(KEY_SPACE);
        key = KEY_SPACE;
        mods = KEY_MOD_CTRL;
    }

    if (key == KEY_RUBOUT) {
        code = KEY_BACKSP | mods;
    }

    else if (key == key_char('\x1F') && (mods & KEY_MOD_ALT) == 0) {
        key = '_';
        code = WITH_CTRL(key_char('_'));
    }

    else if (in_paste_mode && key == KEY_ENTER) {
        code = KEY_LINEFEED;
    }

    else if (key == KEY_ENTER &&
             (mods == KEY_MOD_SHIFT || mods == KEY_MOD_ALT || mods == KEY_MOD_CTRL)) {
        code = KEY_LINEFEED;
    }

    else if ((mods & KEY_MOD_CTRL) != 0 &&
             ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))) {
        code = (key & 0x1F);
    }

    else if (code == WITH_CTRL(KEY_TAB)) {
        code = KEY_SHIFT_TAB;
    }

    else if (code == WITH_ALT(KEY_DOWN) || code == WITH_ALT('>') || code == WITH_CTRL(KEY_END)) {
        code = KEY_PAGEDOWN;
    } else if (code == WITH_ALT(KEY_UP) || code == WITH_ALT('<') || code == WITH_CTRL(KEY_HOME)) {
        code = KEY_PAGEUP;
    }

    if (key < ' ' && (mods & KEY_MOD_CTRL) != 0) {
        code &= ~KEY_MOD_CTRL;
    }

    return code;
}

ic_private code_t tty_read(tty_t* tty) {
    code_t code;
    if (!tty_read_timeout(tty, -1, &code)) {
        return KEY_NONE;
    }
    return code;
}

//-------------------------------------------------------------
// Read back an ANSI query response
//-------------------------------------------------------------

ic_private bool tty_read_esc_response(tty_t* tty, char esc_start, bool final_st, char* buf,
                                      ssize_t buflen, tty_response_fun_t* matches, void* arg) {
    return tty_read_esc_response_with_timeout(tty, esc_start, final_st, buf, buflen, matches, arg,
                                              -1);
}

ic_private bool tty_read_esc_response_with_timeout(tty_t* tty, char esc_start, bool final_st,
                                                   char* buf, ssize_t buflen,
                                                   tty_response_fun_t* matches, void* arg,
                                                   long initial_timeout_ms) {
    if (tty == NULL || buf == NULL || buflen <= 1 || buflen > TTY_CPUSH_MAX - 4) {
        return false;
    }
    buf[0] = 0;
    uint8_t consumed[TTY_CPUSH_MAX];
    ssize_t count = 0;
    ssize_t len = 0;
    uint8_t c = 0;
    const long wait_ms = initial_timeout_ms < 0 ? 2 * tty->esc_initial_timeout : initial_timeout_ms;
    if (!tty_readc_noblock(tty, &c, wait_ms)) {
        return false;
    }
    consumed[count++] = c;
    if (c != '\x1B') {
        goto rejected;
    }
    if (!tty_readc_noblock(tty, &c, tty->esc_timeout)) {
        goto rejected;
    }
    consumed[count++] = c;
    if (c != esc_start) {
        goto rejected;
    }
    while (len < buflen - 1) {
        if (!tty_readc_noblock(tty, &c, tty->esc_timeout)) {
            goto rejected;
        }
        consumed[count++] = c;
        if (final_st) {
            if (c == '\x07') {
                goto complete;
            } else if (c == '\x1B') {
                if (!tty_readc_noblock(tty, &c, tty->esc_timeout)) {
                    goto rejected;
                }
                consumed[count++] = c;
                if (c == '\\') {
                    goto complete;
                }
                goto rejected;
            }
            if (c < 0x20 || c == 0x7F) {
                goto rejected;
            }
        } else {
            if (c >= 0x40 && c <= 0x7E) {
                buf[len++] = (char)c;
                goto complete;
            }
            if (c < 0x20 || c > 0x3F) {
                goto rejected;
            }
        }
        buf[len++] = (char)c;
    }
    goto rejected;

complete:
    buf[len] = 0;
    if (matches != NULL && matches(buf, arg)) {
        return true;
    }

rejected:
    // These bytes may be typing, paste, or a reply to a different query. Put
    // them back ahead of unread input, including NUL and partial escapes.
    assert(tty->cpush_count + count <= TTY_CPUSH_MAX);
    while (count > 0) {
        tty->cpushbuf[tty->cpush_count++] = consumed[--count];
    }
    buf[0] = 0;
    return false;
}

//-------------------------------------------------------------
// High level code pushback
//-------------------------------------------------------------

static bool tty_code_pop(tty_t* tty, code_t* code) {
    if (tty->push_count <= 0) {
        return false;
    }
    tty->push_count--;
    *code = tty->pushbuf[tty->push_count];
    return true;
}

ic_private void tty_code_pushback(tty_t* tty, code_t c) {
    if (tty->push_count >= TTY_PUSH_MAX) {
        return;
    }
    tty->pushbuf[tty->push_count] = c;
    tty->push_count++;
    tty_wakeup(tty);
}

//-------------------------------------------------------------
// low-level character pushback (for escape sequences and windows)
//-------------------------------------------------------------

ic_private bool tty_cpop(tty_t* tty, uint8_t* c) {
    if (tty->cpush_count <= 0) {
        return false;
    } else {
        tty->cpush_count--;
        *c = tty->cpushbuf[tty->cpush_count];
        return true;
    }
}

static bool tty_typeahead_replay_pop(tty_t* tty, uint8_t* c) {
    if (tty == NULL || c == NULL || tty->typeahead_replay == NULL) {
        return false;
    }

    const ssize_t len = sbuf_len(tty->typeahead_replay);
    if (tty->typeahead_replay_pos < 0 || tty->typeahead_replay_pos >= len) {
        sbuf_clear(tty->typeahead_replay);
        tty->typeahead_replay_pos = 0;
        return false;
    }

    *c = (uint8_t)sbuf_char_at(tty->typeahead_replay, tty->typeahead_replay_pos);
    tty->typeahead_replay_pos++;
    if (tty->typeahead_replay_pos >= len) {
        sbuf_clear(tty->typeahead_replay);
        tty->typeahead_replay_pos = 0;
    }
    return true;
}

ic_private bool tty_replay_typeahead(tty_t* tty, const uint8_t* data, size_t length) {
    if (tty == NULL || tty->typeahead_replay == NULL || (data == NULL && length > 0)) {
        return false;
    }
    if (length == 0) {
        return true;
    }

    if (tty->typeahead_replay_pos > 0) {
        sbuf_delete_from_to(tty->typeahead_replay, 0, tty->typeahead_replay_pos);
        tty->typeahead_replay_pos = 0;
    }
    const ssize_t old_len = sbuf_len(tty->typeahead_replay);
    const ssize_t new_len =
        sbuf_append_n(tty->typeahead_replay, (const char*)data, (ssize_t)length);
    return (new_len == old_len + (ssize_t)length);
}

ic_private void tty_clear_typeahead_replay(tty_t* tty) {
    if (tty == NULL || tty->typeahead_replay == NULL) {
        return;
    }
    sbuf_clear(tty->typeahead_replay);
    tty->typeahead_replay_pos = 0;
}

ic_private ssize_t tty_typeahead_replay_count(const tty_t* tty) {
    if (tty == NULL || tty->typeahead_replay == NULL) {
        return 0;
    }
    const ssize_t remaining = sbuf_len(tty->typeahead_replay) - tty->typeahead_replay_pos;
    return (remaining > 0 ? remaining : 0);
}

static void tty_cpush(tty_t* tty, const char* s) {
    ssize_t len = ic_strlen(s);
    if (tty->cpush_count + len > TTY_CPUSH_MAX) {
        debug_msg("tty: cpush buffer full! (pushing %s)\n", s);
        return;
    }
    for (ssize_t i = 0; i < len; i++) {
        tty->cpushbuf[tty->cpush_count + i] = (uint8_t)s[len - i - 1];
    }
    tty->cpush_count += len;
}

#if defined(_WIN32)
static void tty_cpushf(tty_t* tty, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[TTY_PUSH_MAX + 1];
    vsnprintf(buf, TTY_PUSH_MAX, fmt, args);
    buf[TTY_PUSH_MAX] = 0;
    tty_cpush(tty, buf);
    va_end(args);
}
#endif

ic_private void tty_cpush_char(tty_t* tty, uint8_t c) {
    uint8_t buf[2];
    buf[0] = c;
    buf[1] = 0;
    tty_cpush(tty, (const char*)buf);
}

//-------------------------------------------------------------
// Push escape codes (used on Windows to insert keys)
//-------------------------------------------------------------

#if defined(_WIN32)
static unsigned csi_mods(code_t mods) {
    unsigned m = 1;
    if (mods & KEY_MOD_SHIFT) {
        m += 1;
    }
    if (mods & KEY_MOD_ALT) {
        m += 2;
    }
    if (mods & KEY_MOD_CTRL) {
        m += 4;
    }
    return m;
}

static void tty_cpush_csi_vt(tty_t* tty, code_t mods, uint32_t vtcode) {
    tty_cpushf(tty, "\x1B[%u;%u~", vtcode, csi_mods(mods));
}

static void tty_cpush_csi_xterm(tty_t* tty, code_t mods, char xcode) {
    tty_cpushf(tty, "\x1B[1;%u%c", csi_mods(mods), xcode);
}

static void tty_cpush_csi_unicode(tty_t* tty, code_t mods, uint32_t unicode) {
    if ((unicode < 0x80 && mods == 0) ||
        (mods == KEY_MOD_CTRL && unicode < ' ' && unicode != KEY_TAB && unicode != KEY_ENTER &&
         unicode != KEY_LINEFEED && unicode != KEY_BACKSP) ||
        (mods == KEY_MOD_SHIFT && unicode >= ' ' && unicode <= KEY_RUBOUT)) {
        tty_cpush_char(tty, (uint8_t)unicode);
    } else {
        tty_cpushf(tty, "\x1B[%u;%uu", unicode, csi_mods(mods));
    }
}
#endif

//-------------------------------------------------------------
// Init
//-------------------------------------------------------------

static bool tty_init_raw(tty_t* tty);
static void tty_done_raw(tty_t* tty);

static bool tty_init_utf8(tty_t* tty) {
#ifdef _WIN32
    tty->is_utf8 = true;
#else
    const char* loc = setlocale(LC_ALL, "");
    tty->is_utf8 =
        (ic_icontains(loc, "UTF-8") || ic_icontains(loc, "utf8") || ic_stricmp(loc, "C") == 0);
    debug_msg("tty: utf8: %s (loc=%s)\n", tty->is_utf8 ? "true" : "false", loc);
#endif
    return true;
}

ic_private tty_t* tty_new(alloc_t* mem, int fd_in) {
    tty_t* tty = mem_zalloc_tp(mem, tty_t);
    tty->mem = mem;
    tty->fd_in = (fd_in < 0 ? STDIN_FILENO : fd_in);
    tty->lost_terminal = false;
#if defined(__APPLE__)
    tty->esc_initial_timeout = 200;
#else
    tty->esc_initial_timeout = 100;
#endif
    tty->esc_timeout = 10;
    tty->paste_mode = false;
    tty->typeahead_replay = sbuf_new(mem);
    if (tty->typeahead_replay == NULL) {
        mem_free(mem, tty);
        return NULL;
    }
    tty->typeahead_replay_pos = 0;
    tty->wake_pipe_initialized = false;
    if (!tty_setup_wakeup_channel(tty)) {
        debug_msg("tty: failed to create wakeup pipe\n");
    }
    if (!(isatty(tty->fd_in) && tty_init_raw(tty) && tty_init_utf8(tty))) {
        tty_free(tty);
        return NULL;
    }
    return tty;
}

ic_private void tty_free(tty_t* tty) {
    if (tty == NULL) {
        return;
    }
    tty_end_raw(tty);
    tty_done_raw(tty);
    tty_close_wakeup_channel(tty);
    sbuf_free(tty->typeahead_replay);
    mem_free(tty->mem, tty);
}

ic_private bool tty_is_utf8(const tty_t* tty) {
    if (tty == NULL) {
        return true;
    }
    return tty->is_utf8;
}

ic_private bool tty_is_raw_enabled(const tty_t* tty) {
    if (tty == NULL) {
        return false;
    }
    return tty->raw_enabled;
}

ic_private bool tty_input_pending(const tty_t* tty) {
    if (tty == NULL) {
        return false;
    }
    if (tty->push_count > 0 || tty->cpush_count > 0 || tty_typeahead_replay_count(tty) > 0) {
        return true;
    }
#if defined(FIONREAD)
    int navail = 0;
    if (ioctl(tty->fd_in, FIONREAD, &navail) == 0) {
        return (navail > 0);
    }
#endif
    return false;
}

ic_private bool tty_capture_pending_raw(tty_t* tty, stringbuf_t* out) {
    if (out == NULL) {
        return false;
    }
    sbuf_clear(out);

    if (tty == NULL) {
        return false;
    }

#if defined(_WIN32)
    return false;
#else
    if (isatty(tty->fd_in) == 0) {
        return false;
    }

    int fd_flags = fcntl(tty->fd_in, F_GETFL, 0);
    if (fd_flags == -1) {
        return false;
    }

    bool restore_flags = false;
    if ((fd_flags & O_NONBLOCK) == 0) {
        if (fcntl(tty->fd_in, F_SETFL, fd_flags | O_NONBLOCK) != 0) {
            return false;
        }
        restore_flags = true;
    }

    bool decode_swapped_crlf = tty->typeahead_crlf_swapped;
    bool decode_cooked_return = false;
    struct termios original_termios;
    memset(&original_termios, 0, sizeof(original_termios));
    bool restore_termios = false;
    if (tcgetattr(tty->fd_in, &original_termios) == 0) {
        // A foreground program can leave raw input active without updating our
        // cached capture state. Its queued Return bytes are already CR; swapping
        // them would replay Ctrl+J instead of submitting the user's command.
        if ((original_termios.c_iflag & (ICRNL | INLCR)) != (ICRNL | INLCR)) {
            decode_swapped_crlf = false;
        }
        decode_cooked_return = !decode_swapped_crlf && (original_termios.c_iflag & ICRNL) != 0;
        struct termios raw_termios = original_termios;
        if (!decode_swapped_crlf) {
            raw_termios.c_iflag &= (tcflag_t)(~(ICRNL | INLCR));
        }
        raw_termios.c_lflag &= (tcflag_t)(~(ICANON | ECHO));
        raw_termios.c_cc[VMIN] = 0;
        raw_termios.c_cc[VTIME] = 0;
        if (tcsetattr(tty->fd_in, TCSANOW, &raw_termios) == 0) {
            restore_termios = true;
        }
    }

    char buffer[256];
    for (;;) {
        ssize_t bytes_read = read(tty->fd_in, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            if (decode_swapped_crlf || decode_cooked_return) {
                for (ssize_t i = 0; i < bytes_read; ++i) {
                    if (buffer[i] == '\n') {
                        buffer[i] = '\r';
                    } else if (decode_swapped_crlf && buffer[i] == '\r') {
                        buffer[i] = '\n';
                    }
                }
            }
            (void)sbuf_append_n(out, buffer, bytes_read);
            if (bytes_read < (ssize_t)sizeof(buffer)) {
                break;
            }
            continue;
        }

        if (bytes_read == 0) {
            // VMIN=0/VTIME=0 permits an empty read from a live terminal.
            // Only the normal blocking reader can interpret zero as EOF.
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        break;
    }

    if (restore_termios) {
        (void)tcsetattr(tty->fd_in, TCSANOW, &original_termios);
    }
    if (restore_flags) {
        (void)fcntl(tty->fd_in, F_SETFL, fd_flags);
    }

    if (sbuf_len(out) > 0) {
        tty->lost_terminal = false;
        return true;
    }
    return false;
#endif
}

ic_private bool tty_lost_terminal(const tty_t* tty) {
    if (tty == NULL) {
        return false;
    }
    return tty->lost_terminal;
}

ic_private bool tty_term_resize_event(tty_t* tty) {
    if (tty == NULL) {
        return true;
    }
    if (tty->has_term_resize_event) {
        if (!tty->term_resize_event) {
            return false;
        }
        tty->term_resize_event = false;
    }
    return true;
}

ic_private void tty_set_esc_delay(tty_t* tty, long initial_delay_ms, long followup_delay_ms) {
    tty->esc_initial_timeout =
        (initial_delay_ms < 0 ? 0 : (initial_delay_ms > 1000 ? 1000 : initial_delay_ms));
    tty->esc_timeout =
        (followup_delay_ms < 0 ? 0 : (followup_delay_ms > 1000 ? 1000 : followup_delay_ms));
}

//-------------------------------------------------------------
// Unix
//-------------------------------------------------------------
#if !defined(_WIN32)

static bool tty_read_from_fd(tty_t* tty, uint8_t* c) {
    for (;;) {
        *c = 0;
        ssize_t nread = read(tty->fd_in, (char*)c, 1);
        if (nread == 1) {
            tty->lost_terminal = false;
            return true;
        }
        if (nread == 0) {
            tty->lost_terminal = true;
            return false;
        }
        if (nread < 0) {
            int err = errno;
            if (err == EINTR) {
                continue;
            }
#if defined(EAGAIN) && defined(EWOULDBLOCK)
            if (err == EAGAIN || err == EWOULDBLOCK) {
                continue;
            }
#endif
            if (err == EIO && isatty(tty->fd_in)) {
                pid_t pgid = getpgrp();
                if (tcsetpgrp(tty->fd_in, pgid) == 0) {
                    tty->lost_terminal = false;
                    continue;
                }

                tty->lost_terminal = true;
            }
            return false;
        }
    }
}

typedef enum {
    TTY_EVENT_ERROR = -1,
    TTY_EVENT_INPUT = 1,
    TTY_EVENT_WAKE = 2,
} tty_event_t;

static tty_event_t tty_wait_for_tty_event(tty_t* tty) {
    while (true) {
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(tty->fd_in, &readset);
        int maxfd = tty->fd_in;
        // A queued wakeup must not split escape sequences or terminal replies.
        // Their timed reads also reach this blocking path when input is ready.
        const bool have_wakeup = tty->wake_pipe_initialized && tty->readline_wakeup_enabled != 0;
        if (have_wakeup) {
            FD_SET(tty->wake_pipe[0], &readset);
            if (tty->wake_pipe[0] > maxfd) {
                maxfd = tty->wake_pipe[0];
            }
        }

        int ready = select(maxfd + 1, &readset, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return TTY_EVENT_ERROR;
        }

        if (have_wakeup && FD_ISSET(tty->wake_pipe[0], &readset)) {
            tty_drain_wakeup_pipe(tty);
            return TTY_EVENT_WAKE;
        }

        if (FD_ISSET(tty->fd_in, &readset)) {
            return TTY_EVENT_INPUT;
        }
    }
}

static bool tty_readc_blocking(tty_t* tty, uint8_t* c) {
    if (tty_cpop(tty, c)) {
        return true;
    }
    if (tty_typeahead_replay_pop(tty, c)) {
        return true;
    }

    while (true) {
        tty_event_t event = tty_wait_for_tty_event(tty);
        if (event == TTY_EVENT_ERROR) {
            tty->lost_terminal = true;
            return false;
        }
        if (event == TTY_EVENT_WAKE) {
            return false;
        }

        if (event == TTY_EVENT_INPUT) {
            return tty_read_from_fd(tty, c);
        }
    }
}

ic_private bool tty_readc_noblock(tty_t* tty, uint8_t* c, long timeout_ms) {
    if (tty_cpop(tty, c)) {
        return true;
    }
    if (tty_typeahead_replay_pop(tty, c)) {
        return true;
    }

    if (timeout_ms < 0) {
        return tty_readc_blocking(tty, c);
    }

#if defined(FIONREAD)
    {
        int navail = 0;
        if (ioctl(0, FIONREAD, &navail) == 0) {
            if (navail >= 1) {
                return tty_readc_blocking(tty, c);
            } else if (timeout_ms == 0) {
                return false;
            }
        }
    }
#endif

#if defined(FD_SET)

    fd_set readset;
    struct timeval time;
    FD_ZERO(&readset);
    FD_SET(tty->fd_in, &readset);
    int maxfd = tty->fd_in;
    const bool allow_wakeup = (tty->wake_pipe_initialized && tty->readline_wakeup_enabled != 0);
    if (allow_wakeup) {
        FD_SET(tty->wake_pipe[0], &readset);
        if (tty->wake_pipe[0] > maxfd) {
            maxfd = tty->wake_pipe[0];
        }
    }
    time.tv_sec = (timeout_ms > 0 ? timeout_ms / 1000 : 0);
    time.tv_usec = (timeout_ms > 0 ? 1000 * (timeout_ms % 1000) : 0);
    if (select(maxfd + 1, &readset, NULL, NULL, &time) > 0) {
        if (allow_wakeup && FD_ISSET(tty->wake_pipe[0], &readset)) {
            tty_drain_wakeup_pipe(tty);
            return false;
        }
        return tty_readc_blocking(tty, c);
    }
#else

    do {

#if defined(FIONREAD)
        int navail = 0;
        if (ioctl(0, FIONREAD, &navail) == 0 && navail >= 1) {
            return tty_readc_blocking(tty, c);
        }
#elif defined(O_NONBLOCK)

        int fstatus = fcntl(tty->fd_in, F_GETFL, 0);
        if (fstatus != -1) {
            if (fcntl(tty->fd_in, F_SETFL, (fstatus | O_NONBLOCK)) != -1) {
                char buf[2] = {0, 0};
                ssize_t nread = read(tty->fd_in, buf, 1);
                fcntl(tty->fd_in, F_SETFL, fstatus);
                if (nread >= 1) {
                    *c = (uint8_t)buf[0];
                    return true;
                }
            }
        }
#else
#error "define an nonblocking read for this platform"
#endif

        if (timeout_ms > 0) {
            usleep(50 * 1000L);
            timeout_ms -= 100;
            if (timeout_ms < 0) {
                timeout_ms = 0;
            }
        }
    } while (timeout_ms > 0);
#endif
    return false;
}

#if defined(TIOCSTI)
ic_private bool tty_async_stop(const tty_t* tty) {
    char c = KEY_CTRL_C;
    return (ioctl(tty->fd_in, TIOCSTI, &c) >= 0);
}
#else
ic_private bool tty_async_stop(const tty_t* tty) {
    return false;
}
#endif

#if defined(SIGWINCH) && defined(SA_RESTART)

static tty_t* sig_tty;

typedef struct signal_handler_s {
    union {
        int _avoid_warning;
        struct sigaction previous;
    } action;
    int signum;
    bool installed;
} signal_handler_t;

static signal_handler_t sighandlers[] = {
    {.signum = SIGWINCH}, {.signum = SIGTERM}, {.signum = SIGINT},  {.signum = SIGQUIT},
    {.signum = SIGHUP},   {.signum = SIGSEGV}, {.signum = SIGTRAP}, {.signum = SIGBUS},
    {.signum = SIGTSTP},  {.signum = SIGTTIN}, {.signum = SIGTTOU}, {.signum = 0}};

static void sig_handler(int signum, siginfo_t* siginfo, void* uap);

static bool signal_install_wrapper(signal_handler_t* sh) {
    struct sigaction handler = sh->action.previous;
    handler.sa_sigaction = sig_handler;
    handler.sa_flags = SA_SIGINFO | (handler.sa_flags & SA_RESTART);
    sh->installed = (sigaction(sh->signum, &handler, NULL) == 0);
    return sh->installed;
}

static void sig_handler(int signum, siginfo_t* siginfo, void* uap) {
    ic_unused(siginfo);
    ic_unused(uap);
    const int saved_errno = errno;
    tty_t* tty = sig_tty;
    const bool resume_raw = (tty != NULL && tty->raw_enabled && signum != SIGWINCH);
    if (signum == SIGWINCH) {
        if (tty != NULL) {
            tty->term_resize_event = true;
        }
    } else if (resume_raw) {
        (void)tcsetattr(tty->fd_in, TCSANOW, &tty->orig_ios);
        tty->raw_enabled = false;
    }

    signal_handler_t* sh = sighandlers;
    while (sh->signum != 0 && sh->signum != signum) {
        sh++;
    }
    // Let the kernel apply the saved disposition and mask. This also honors
    // SA_RESETHAND on platforms that report it through sigaction. A default
    // termination never returns; a caught signal or a stop followed by SIGCONT can.
    // The zero entry terminates the table and must never be treated as a signal.
    if (signum > 0 && sh->signum == signum && sigaction(signum, &sh->action.previous, NULL) == 0) {
        sigset_t blocked, unblocked;
        (void)sigprocmask(SIG_SETMASK, NULL, &blocked);
        unblocked = blocked;
        sigdelset(&unblocked, signum);
        (void)sigprocmask(SIG_SETMASK, &unblocked, NULL);
        (void)raise(signum);
        (void)sigprocmask(SIG_SETMASK, &blocked, NULL);
        if (sig_tty == tty && sigaction(signum, NULL, &sh->action.previous) == 0) {
            if (sh->action.previous.sa_handler == SIG_IGN) {
                sh->installed = false;
            } else {
                (void)signal_install_wrapper(sh);
            }
        }
    }

    if (resume_raw && sig_tty == tty) {
        (void)tty_start_raw(tty);
    }
    errno = saved_errno;
}

static void signals_install(tty_t* tty) {
    sig_tty = tty;

    for (signal_handler_t* sh = sighandlers; sh->signum != 0; sh++) {
        sh->installed = false;
        if (sigaction(sh->signum, NULL, &sh->action.previous) == 0) {
            if (sh->action.previous.sa_handler != SIG_IGN) {
                if (signal_install_wrapper(sh) && sh->signum == SIGWINCH) {
                    sig_tty->has_term_resize_event = true;
                };
            }
        }
    }
}

static void signals_restore(void) {
    for (signal_handler_t* sh = sighandlers; sh->signum != 0; sh++) {
        struct sigaction current;
        if (sh->installed && sigaction(sh->signum, NULL, &current) == 0 &&
            (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == sig_handler) {
            (void)sigaction(sh->signum, &sh->action.previous, NULL);
        }
        sh->installed = false;
    }
    sig_tty = NULL;
}

#else
static void signals_install(tty_t* tty) {
    ic_unused(tty);
}
static void signals_restore(void) {
}

#endif

ic_public void ic_notify_resize(void) {
#if !defined(_WIN32) && defined(SIGWINCH) && defined(SA_RESTART)
    if (sig_tty != NULL) {
        sig_tty->term_resize_event = true;
        tty_wakeup(sig_tty);
    }
#endif
}

ic_public void ic_notify_readline(void) {
#if !defined(_WIN32) && defined(SIGWINCH) && defined(SA_RESTART)
    const int saved_errno = errno;
    if (sig_tty != NULL) {
        sig_tty->readline_event = 1;
        if (sig_tty->readline_wakeup_enabled != 0) {
            tty_wakeup(sig_tty);
        }
    }
    errno = saved_errno;
#endif
}

ic_private bool tty_start_raw(tty_t* tty) {
    if (tty == NULL) {
        return false;
    }
    if (tty->raw_enabled) {
        return true;
    }
    if (tcsetattr(tty->fd_in, TCSANOW, &tty->raw_ios) < 0) {
        return false;
    }
    tty->raw_enabled = true;
    tty->typeahead_crlf_swapped = false;
    return true;
}

ic_private void tty_end_raw(tty_t* tty) {
    if (tty == NULL) {
        return;
    }
    if (!tty->raw_enabled) {
        return;
    }
    // Preserve bytes that arrived under raw-mode CR/LF semantics before
    // switching to the swapped capture termios. Otherwise a raw Return still
    // waiting in the kernel queue is later decoded as if it had been swapped
    // by typeahead_ios, turning it into Ctrl+J. This is especially visible
    // when input immediately follows Ctrl+C at a readline boundary.
    if (tty->typeahead_capture_mode) {
        stringbuf_t* pending = sbuf_new(tty->mem);
        if (pending != NULL) {
            if (tty_capture_pending_raw(tty, pending)) {
                const char* bytes = sbuf_string(pending);
                const ssize_t length = sbuf_len(pending);
                (void)tty_replay_typeahead(tty, (const uint8_t*)bytes, (size_t)length);
            }
            sbuf_free(pending);
        }
    }

    const struct termios* restore_ios =
        (tty->typeahead_capture_mode ? &tty->typeahead_ios : &tty->orig_ios);
    if (tcsetattr(tty->fd_in, TCSANOW, restore_ios) < 0) {
        return;
    }
    tty->raw_enabled = false;
    tty->typeahead_crlf_swapped =
        (tty->typeahead_capture_mode && (tty->orig_ios.c_iflag & ICRNL) != 0 &&
         (tty->orig_ios.c_iflag & IGNCR) == 0);
}

ic_private void tty_enable_typeahead_capture_mode(tty_t* tty, bool enable) {
    if (tty == NULL) {
        return;
    }
    tty->typeahead_capture_mode = enable;
    if (tty->raw_enabled) {
        tty->typeahead_crlf_swapped = false;
        return;
    }

    const struct termios* target = (enable ? &tty->typeahead_ios : &tty->orig_ios);
    if (tcsetattr(tty->fd_in, TCSANOW, target) < 0) {
        tty->typeahead_crlf_swapped = false;
        return;
    }
    tty->typeahead_crlf_swapped =
        (enable && (tty->orig_ios.c_iflag & ICRNL) != 0 && (tty->orig_ios.c_iflag & IGNCR) == 0);
}

static void tty_derive_capture_modes(tty_t* tty) {
    tty->typeahead_ios = tty->orig_ios;

    // With both mappings enabled, the line discipline maps terminal Return
    // (CR) to LF and Ctrl+J (LF) to CR. Capture swaps them back before replay.
    // This mode belongs only to shell capture, never to foreground commands.
    if ((tty->orig_ios.c_iflag & ICRNL) != 0 && (tty->orig_ios.c_iflag & IGNCR) == 0) {
        tty->typeahead_ios.c_iflag |= INLCR;
    }
}

static void tty_derive_modes(tty_t* tty) {
    tty_derive_capture_modes(tty);
    tty->raw_ios = tty->orig_ios;

    tty->raw_ios.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INLCR | INPCK | ISTRIP | IXON);

    tty->raw_ios.c_cflag |= CS8;

    tty->raw_ios.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);

    tty->raw_ios.c_cc[VTIME] = 0;
    tty->raw_ios.c_cc[VMIN] = 1;
}

ic_private void tty_adopt_external_modes(tty_t* tty) {
    // Only adopt settings while commands own the modes. Editor/capture modes
    // are implementation details and must never become the external baseline.
    if (tty == NULL || tty->raw_enabled || tty->typeahead_capture_mode) {
        return;
    }
    struct termios modes;
    if (tcgetattr(tty->fd_in, &modes) != 0) {
        return;
    }
    // Canonical external settings are intentional (including -isig, -icrnl,
    // -opost, and control characters). A program leaving noncanonical modes
    // behind is treated as an abandoned raw session: restore the last baseline.
    if ((modes.c_lflag & ICANON) == 0) {
        modes = tty->orig_ios;
    }
    // Echo is always restored for external reads and prompt hooks. The editor
    // independently owns its input mappings, output processing and key bindings.
    modes.c_lflag |= ECHO;
#ifdef FLUSHO
    modes.c_lflag &= ~(tcflag_t)FLUSHO;
#endif
    tty->orig_ios = modes;
    // Keep the editor's own modes stable. Changes such as erase/intr/tostop
    // belong to the external baseline; the editor has its own key bindings.
    tty_derive_capture_modes(tty);
}

static bool tty_init_raw(tty_t* tty) {
    if (tcgetattr(tty->fd_in, &tty->orig_ios) == -1) {
        return false;
    }
    tty_derive_modes(tty);

    signals_install(tty);

    return true;
}

static void tty_done_raw(tty_t* tty) {
    ic_unused(tty);
    signals_restore();
}

#else

//-------------------------------------------------------------
// Windows
// For best portability we push CSI escape sequences directly
// to the character stream (instead of returning key codes).
//-------------------------------------------------------------

ic_private void tty_adopt_external_modes(tty_t* tty) {
    ic_unused(tty);
}

static void tty_waitc_console(tty_t* tty, long timeout_ms);

ic_private bool tty_readc_noblock(tty_t* tty, uint8_t* c, long timeout_ms) {
    if (tty_cpop(tty, c)) {
        return true;
    }
    if (tty_typeahead_replay_pop(tty, c)) {
        return true;
    }

    tty_waitc_console(tty, timeout_ms);
    return tty_cpop(tty, c);
}

static void tty_waitc_console(tty_t* tty, long timeout_ms) {
    INPUT_RECORD inp;
    DWORD count;
    uint32_t surrogate_hi = 0;
    while (true) {
        if (timeout_ms >= 0) {
            if (!GetNumberOfConsoleInputEvents(tty->hcon, &count)) {
                return;
            }
            if (count == 0) {
                if (timeout_ms == 0) {
                    return;
                } else {
                    ULONGLONG start_ms = GetTickCount64();
                    DWORD res = WaitForSingleObject(tty->hcon, (DWORD)timeout_ms);
                    switch (res) {
                        case WAIT_OBJECT_0: {
                            ULONGLONG waited_ms = (GetTickCount64() - start_ms);
                            timeout_ms -= (long)waited_ms;
                            if (timeout_ms < 0) {
                                timeout_ms = 0;
                            }
                            break;
                        }
                        case WAIT_TIMEOUT:
                        case WAIT_ABANDONED:
                        case WAIT_FAILED:
                        default:
                            return;
                    }
                }
            }
        }

        if (!ReadConsoleInputW(tty->hcon, &inp, 1, &count)) {
            return;
        }
        if (count != 1) {
            return;
        }

        if (inp.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            tty->term_resize_event = true;
            continue;
        }

        if (inp.EventType != KEY_EVENT) {
            continue;
        }

        DWORD modstate = inp.Event.KeyEvent.dwControlKeyState;

        if (!inp.Event.KeyEvent.bKeyDown && inp.Event.KeyEvent.wVirtualKeyCode == VK_SHIFT) {
            modstate &= (DWORD)~SHIFT_PRESSED;
        }

        DWORD altgr = LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED;
        if ((modstate & altgr) == altgr) {
            modstate &= ~altgr;
        }

        code_t mods = 0;
        if ((modstate & (RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED)) != 0) {
            mods |= KEY_MOD_CTRL;
        }
        if ((modstate & (RIGHT_ALT_PRESSED | LEFT_ALT_PRESSED)) != 0) {
            mods |= KEY_MOD_ALT;
        }
        if ((modstate & SHIFT_PRESSED) != 0) {
            mods |= KEY_MOD_SHIFT;
        }

        uint32_t chr = (uint32_t)inp.Event.KeyEvent.uChar.UnicodeChar;
        WORD virt = inp.Event.KeyEvent.wVirtualKeyCode;
        debug_msg("tty: console %s: %s%s%s virt 0x%04x, chr 0x%04x ('%c')\n",
                  inp.Event.KeyEvent.bKeyDown ? "down" : "up", mods & KEY_MOD_CTRL ? "ctrl-" : "",
                  mods & KEY_MOD_ALT ? "alt-" : "", mods & KEY_MOD_SHIFT ? "shift-" : "", virt, chr,
                  chr);

        if (!inp.Event.KeyEvent.bKeyDown && virt != VK_MENU) {
            continue;
        }

        if (chr == 0) {
            switch (virt) {
                case VK_UP:
                    tty_cpush_csi_xterm(tty, mods, 'A');
                    return;
                case VK_DOWN:
                    tty_cpush_csi_xterm(tty, mods, 'B');
                    return;
                case VK_RIGHT:
                    tty_cpush_csi_xterm(tty, mods, 'C');
                    return;
                case VK_LEFT:
                    tty_cpush_csi_xterm(tty, mods, 'D');
                    return;
                case VK_END:
                    tty_cpush_csi_xterm(tty, mods, 'F');
                    return;
                case VK_HOME:
                    tty_cpush_csi_xterm(tty, mods, 'H');
                    return;
                case VK_DELETE:
                    tty_cpush_csi_vt(tty, mods, 3);
                    return;
                case VK_PRIOR:
                    tty_cpush_csi_vt(tty, mods, 5);
                    return;
                case VK_NEXT:
                    tty_cpush_csi_vt(tty, mods, 6);
                    return;
                case VK_TAB:
                    tty_cpush_csi_unicode(tty, mods, 9);
                    return;
                case VK_RETURN:
                    tty_cpush_csi_unicode(tty, mods, 13);
                    return;
                default: {
                    uint32_t vtcode = 0;
                    if (virt >= VK_F1 && virt <= VK_F5) {
                        vtcode = 10 + (virt - VK_F1);
                    } else if (virt >= VK_F6 && virt <= VK_F10) {
                        vtcode = 17 + (virt - VK_F6);
                    } else if (virt >= VK_F11 && virt <= VK_F12) {
                        vtcode = 13 + (virt - VK_F11);
                    }
                    if (vtcode > 0) {
                        tty_cpush_csi_vt(tty, mods, vtcode);
                        return;
                    }
                }
            }

        }

        else if (chr >= 0xD800 && chr <= 0xDBFF) {
            surrogate_hi = (chr - 0xD800);
        }

        else if (chr >= 0xDC00 && chr <= 0xDFFF) {
            chr = ((surrogate_hi << 10) + (chr - 0xDC00) + 0x10000);
            tty_cpush_csi_unicode(tty, mods, chr);
            surrogate_hi = 0;
            return;
        }

        else {
            tty_cpush_csi_unicode(tty, mods, chr);
            return;
        }
    }
}

ic_private bool tty_async_stop(const tty_t* tty) {
    INPUT_RECORD events[2];
    memset(events, 0, 2 * sizeof(INPUT_RECORD));
    events[0].EventType = KEY_EVENT;
    events[0].Event.KeyEvent.bKeyDown = TRUE;
    events[0].Event.KeyEvent.uChar.AsciiChar = KEY_CTRL_C;
    events[1] = events[0];
    events[1].Event.KeyEvent.bKeyDown = FALSE;
    DWORD nwritten = 0;
    WriteConsoleInput(tty->hcon, events, 2, &nwritten);
    return (nwritten == 2);
}

ic_private bool tty_start_raw(tty_t* tty) {
    if (tty->raw_enabled) {
        return true;
    }
    GetConsoleMode(tty->hcon, &tty->hcon_orig_mode);
    DWORD mode = ENABLE_QUICK_EDIT_MODE | ENABLE_WINDOW_INPUT

        ;
    SetConsoleMode(tty->hcon, mode);
    tty->raw_enabled = true;
    return true;
}

ic_private void tty_enable_typeahead_capture_mode(tty_t* tty, bool enable) {
    if (tty == NULL) {
        return;
    }
    tty->typeahead_capture_mode = enable;
    tty->typeahead_crlf_swapped = false;
}

ic_private void tty_end_raw(tty_t* tty) {
    if (!tty->raw_enabled) {
        return;
    }
    SetConsoleMode(tty->hcon, tty->hcon_orig_mode);
    tty->raw_enabled = false;
}

static bool tty_init_raw(tty_t* tty) {
    tty->hcon = GetStdHandle(STD_INPUT_HANDLE);
    tty->has_term_resize_event = true;
    return true;
}

static void tty_done_raw(tty_t* tty) {
    ic_unused(tty);
}

#endif
