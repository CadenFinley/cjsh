/*
  tty.h

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

#ifndef IC_TTY_H
#define IC_TTY_H

#include <stddef.h>
#include <stdint.h>
#include "common.h"
#include "keycodes.h"

//-------------------------------------------------------------
// TTY/Keyboard input
//-------------------------------------------------------------

// Key code
typedef ic_keycode_t code_t;

typedef enum tty_mouse_action_e {
    TTY_MOUSE_ACTION_NONE = 0,
    TTY_MOUSE_ACTION_LEFT_PRESS,
    TTY_MOUSE_ACTION_LEFT_RELEASE,
    TTY_MOUSE_ACTION_LEFT_DRAG,
    TTY_MOUSE_ACTION_WHEEL_UP,
    TTY_MOUSE_ACTION_WHEEL_DOWN,
    TTY_MOUSE_ACTION_OTHER,
} tty_mouse_action_t;

typedef struct tty_mouse_event_s {
    tty_mouse_action_t action;
    ssize_t column;  // 1-based terminal column
    ssize_t row;     // 1-based terminal row
    code_t modifiers;
} tty_mouse_event_t;

// TTY interface
struct tty_s;
typedef struct tty_s tty_t;
struct stringbuf_s;
typedef struct stringbuf_s stringbuf_t;

ic_private tty_t* tty_new(alloc_t* mem, int fd_in);
ic_private void tty_free(tty_t* tty);

ic_private bool tty_is_utf8(const tty_t* tty);
ic_private bool tty_is_raw_enabled(const tty_t* tty);
ic_private bool tty_input_pending(const tty_t* tty);
ic_private bool tty_lost_terminal(const tty_t* tty);
ic_private bool tty_start_raw(tty_t* tty);
ic_private void tty_end_raw(tty_t* tty);
ic_private code_t tty_read(tty_t* tty);
ic_private bool tty_read_timeout(tty_t* tty, long timeout_ms, code_t* c);
ic_private bool tty_capture_pending_raw(tty_t* tty, stringbuf_t* out);
// Preserve the distinction between Return and Ctrl+J while readline is idle.
ic_private void tty_enable_typeahead_capture_mode(tty_t* tty, bool enable);
ic_private void tty_adopt_external_modes(tty_t* tty);
// Queue raw typeahead bytes independently from the small parser pushback buffer.
// The queue survives a readline boundary so bytes following Return are available
// to the next readline cycle.
ic_private bool tty_replay_typeahead(tty_t* tty, const uint8_t* data, size_t length);
ic_private void tty_clear_typeahead_replay(tty_t* tty);
ic_private ssize_t tty_typeahead_replay_count(const tty_t* tty);

ic_private void tty_code_pushback(tty_t* tty, code_t c);
ic_private bool code_is_ascii_char(code_t c, char* chr);
ic_private bool code_is_unicode(code_t c, unicode_t* uchr);
ic_private bool code_is_virt_key(code_t c);

ic_private bool tty_term_resize_event(tty_t* tty);  // did the terminal resize?
ic_private bool tty_async_stop(const tty_t* tty);   // unblock the read asynchronously
ic_private void tty_set_esc_delay(tty_t* tty, long initial_delay_ms, long followup_delay_ms);
ic_private void tty_set_last_mouse_event(tty_t* tty, const tty_mouse_event_t* event);
ic_private void tty_clear_last_mouse_event(tty_t* tty);
ic_private bool tty_get_last_mouse_event(const tty_t* tty, tty_mouse_event_t* event);

// shared between tty.c and tty_esc.c: low level character push
ic_private void tty_cpush_char(tty_t* tty, uint8_t c);
ic_private bool tty_cpop(tty_t* tty, uint8_t* c);
ic_private bool tty_readc_noblock(tty_t* tty, uint8_t* c, long timeout_ms);
ic_private code_t tty_read_esc(tty_t* tty, long esc_initial_timeout,
                               long esc_timeout);  // in tty_esc.c

// used by term.c to read back ANSI escape responses
typedef bool(tty_response_fun_t)(const char* response, void* arg);
ic_private bool tty_read_esc_response_with_timeout(tty_t* tty, char esc_start, bool final_st,
                                                   char* buf, ssize_t buflen,
                                                   tty_response_fun_t* matches, void* arg,
                                                   long initial_timeout_ms);

//-------------------------------------------------------------
// Key codes: a code_t is 32 bits.
// we use the bottom 24 (nah, 21) bits for unicode (up to x0010FFFF)
// The codes after x01000000 are for virtual keys
// and events use  x02000000.
// The top 4 bits are used for modifiers.
//-------------------------------------------------------------

static inline code_t key_char(char c) {
    // careful about signed character conversion (negative char ~> 0x80 - 0xFF)
    return ((uint8_t)c);
}

static inline code_t key_unicode(unicode_t u) {
    return u;
}

#define KEY_MOD_SHIFT IC_KEY_MOD_SHIFT
#define KEY_MOD_ALT IC_KEY_MOD_ALT
#define KEY_MOD_CTRL IC_KEY_MOD_CTRL

#define KEY_NO_MODS IC_KEY_NO_MODS
#define KEY_MODS IC_KEY_MODS

#define WITH_SHIFT IC_KEY_WITH_SHIFT
#define WITH_ALT IC_KEY_WITH_ALT
#define WITH_CTRL IC_KEY_WITH_CTRL

#define KEY_NONE IC_KEY_NONE
#define KEY_CTRL_A IC_KEY_CTRL_A
#define KEY_CTRL_B IC_KEY_CTRL_B
#define KEY_CTRL_C IC_KEY_CTRL_C
#define KEY_CTRL_D IC_KEY_CTRL_D
#define KEY_CTRL_E IC_KEY_CTRL_E
#define KEY_CTRL_F IC_KEY_CTRL_F
#define KEY_BELL IC_KEY_BELL
#define KEY_BACKSP IC_KEY_BACKSP
#define KEY_TAB IC_KEY_TAB
#define KEY_LINEFEED IC_KEY_LINEFEED
#define KEY_CTRL_K IC_KEY_CTRL_K
#define KEY_CTRL_L IC_KEY_CTRL_L
#define KEY_ENTER IC_KEY_ENTER
#define KEY_CTRL_N IC_KEY_CTRL_N
#define KEY_CTRL_O IC_KEY_CTRL_O
#define KEY_CTRL_P IC_KEY_CTRL_P
#define KEY_CTRL_Q IC_KEY_CTRL_Q
#define KEY_CTRL_R IC_KEY_CTRL_R
#define KEY_CTRL_S IC_KEY_CTRL_S
#define KEY_CTRL_T IC_KEY_CTRL_T
#define KEY_CTRL_U IC_KEY_CTRL_U
#define KEY_CTRL_V IC_KEY_CTRL_V
#define KEY_CTRL_W IC_KEY_CTRL_W
#define KEY_CTRL_X IC_KEY_CTRL_X
#define KEY_CTRL_Y IC_KEY_CTRL_Y
#define KEY_CTRL_Z IC_KEY_CTRL_Z
#define KEY_ESC IC_KEY_ESC
#define KEY_SPACE IC_KEY_SPACE
#define KEY_RUBOUT IC_KEY_RUBOUT
#define KEY_UNICODE_MAX IC_KEY_UNICODE_MAX

#define KEY_VIRT IC_KEY_VIRT
#define KEY_UP IC_KEY_UP
#define KEY_DOWN IC_KEY_DOWN
#define KEY_LEFT IC_KEY_LEFT
#define KEY_RIGHT IC_KEY_RIGHT
#define KEY_HOME IC_KEY_HOME
#define KEY_END IC_KEY_END
#define KEY_DEL IC_KEY_DEL
#define KEY_PAGEUP IC_KEY_PAGEUP
#define KEY_PAGEDOWN IC_KEY_PAGEDOWN
#define KEY_INS IC_KEY_INS

#define KEY_F1 IC_KEY_F1
#define KEY_F2 IC_KEY_F2
#define KEY_F3 IC_KEY_F3
#define KEY_F4 IC_KEY_F4
#define KEY_F5 IC_KEY_F5
#define KEY_F6 IC_KEY_F6
#define KEY_F7 IC_KEY_F7
#define KEY_F8 IC_KEY_F8
#define KEY_F9 IC_KEY_F9
#define KEY_F10 IC_KEY_F10
#define KEY_F11 IC_KEY_F11
#define KEY_F12 IC_KEY_F12
#define KEY_F IC_KEY_F

#define KEY_EVENT_BASE IC_KEY_EVENT_BASE
#define KEY_EVENT_RESIZE IC_KEY_EVENT_RESIZE
#define KEY_EVENT_AUTOTAB IC_KEY_EVENT_AUTOTAB
#define KEY_EVENT_STOP IC_KEY_EVENT_STOP
#define KEY_EVENT_INTERRUPT IC_KEY_EVENT_INTERRUPT
#define KEY_EVENT_PROMPT_REFRESH IC_KEY_EVENT_PROMPT_REFRESH
#define KEY_EVENT_READLINE IC_KEY_EVENT_READLINE
#define KEY_EVENT_MOUSE_WHEEL_UP IC_KEY_EVENT_MOUSE_WHEEL_UP
#define KEY_EVENT_MOUSE_WHEEL_DOWN IC_KEY_EVENT_MOUSE_WHEEL_DOWN
#define KEY_EVENT_MOUSE_OTHER IC_KEY_EVENT_MOUSE_OTHER
#define KEY_EVENT_FOCUS_IN IC_KEY_EVENT_FOCUS_IN
#define KEY_EVENT_FOCUS_OUT IC_KEY_EVENT_FOCUS_OUT

#define KEY_CTRL_UP IC_KEY_CTRL_UP
#define KEY_CTRL_DOWN IC_KEY_CTRL_DOWN
#define KEY_CTRL_LEFT IC_KEY_CTRL_LEFT
#define KEY_CTRL_RIGHT IC_KEY_CTRL_RIGHT
#define KEY_CTRL_HOME IC_KEY_CTRL_HOME
#define KEY_CTRL_END IC_KEY_CTRL_END
#define KEY_CTRL_DEL IC_KEY_CTRL_DEL
#define KEY_CTRL_PAGEUP IC_KEY_CTRL_PAGEUP
#define KEY_CTRL_PAGEDOWN IC_KEY_CTRL_PAGEDOWN
#define KEY_CTRL_INS IC_KEY_CTRL_INS
#define KEY_CTRL_SPACE IC_KEY_CTRL_SPACE

#define KEY_SHIFT_TAB IC_KEY_SHIFT_TAB

#endif  // IC_TTY_H
